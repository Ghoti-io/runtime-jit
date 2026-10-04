/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-jit.
 *
 * Ghoti.io Runtime-jit is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-jit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Code memory (AD-13): pages from the context's counting page provider, filled
 * while read-write and flipped to read-execute once. The flip is the
 * provider's `protect`, so the Windows page-protection path is runtime-core's
 * `VirtualProtect` branch, and the Windows unwind information (a
 * `RUNTIME_FUNCTION` and an `UNWIND_INFO`) is written into the same mapping
 * before the flip and registered after it.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "code_internal.h"

#include <string.h>

#if defined(_WIN64) && defined(__x86_64__)
#include <windows.h>
#endif

static uint64_t g_icache_syncs; /* __atomic builtins only */

void grjit_icache_sync(void * mapping, size_t size) {
  /* Only the arm64 branch of grjit_memory_create calls this; an x86-64 mapping
   * never does. On an x86-64 host the builtin is a no-op (instruction fetch is
   * coherent with stores), which lets a test make an arm64 mapping there and
   * count that this line was reached: the claim an emulator cannot make. */
  __builtin___clear_cache((char *)mapping, (char *)mapping + size);
  __atomic_add_fetch(&g_icache_syncs, 1, __ATOMIC_RELAXED);
}

uint64_t grjit_icache_sync_count(void) {
  return __atomic_load_n(&g_icache_syncs, __ATOMIC_RELAXED);
}

static void put32(uint8_t * at, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    at[i] = (uint8_t)(v >> (8 * i));
  }
}

size_t grjit_unwind_info_build(const GRJIT_Prologue * p, uint8_t * out) {
  /* UNWIND_INFO: Version 1 and no flags; SizeOfProlog; CountOfCodes;
   * FrameRegister (rbp, 5) with FrameOffset 0. Then the codes, each a byte of
   * offset (the end of the instruction in the prologue) and a byte of
   * operation (low nibble) and information (high nibble). */
  enum { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE = 1, UWOP_ALLOC_SMALL = 2,
         UWOP_SET_FPREG = 3, RBP = 5 };
  if (p->push_end == 0 || p->setfp_end <= p->push_end ||
      p->alloc_end < p->setfp_end || p->alloc_end > 255 ||
      p->alloc_bytes % 8 != 0) {
    return 0;
  }
  uint8_t * codes = out + 4;
  size_t slots = 0;
  uint32_t n = p->alloc_bytes;
  if (n != 0) {
    if (n <= 128) {
      codes[2 * slots] = (uint8_t)p->alloc_end;
      codes[2 * slots + 1] =
          (uint8_t)(UWOP_ALLOC_SMALL | (((n - 8) / 8) << 4));
      slots += 1;
    } else if (n / 8 <= 0xFFFF) {
      codes[2 * slots] = (uint8_t)p->alloc_end;
      codes[2 * slots + 1] = (uint8_t)(UWOP_ALLOC_LARGE | (0 << 4));
      codes[2 * slots + 2] = (uint8_t)((n / 8) & 0xFF);
      codes[2 * slots + 3] = (uint8_t)((n / 8) >> 8);
      slots += 2;
    } else {
      codes[2 * slots] = (uint8_t)p->alloc_end;
      codes[2 * slots + 1] = (uint8_t)(UWOP_ALLOC_LARGE | (1 << 4));
      put32(codes + 2 * slots + 2, n);
      slots += 3;
    }
  }
  codes[2 * slots] = (uint8_t)p->setfp_end;
  codes[2 * slots + 1] = UWOP_SET_FPREG;
  slots += 1;
  codes[2 * slots] = (uint8_t)p->push_end;
  codes[2 * slots + 1] = (uint8_t)(UWOP_PUSH_NONVOL | (RBP << 4));
  slots += 1;
  out[0] = 1;
  out[1] = (uint8_t)p->alloc_end;
  out[2] = (uint8_t)slots;
  out[3] = (uint8_t)(RBP | (0 << 4));
  if (slots % 2 != 0) {
    codes[2 * slots] = 0;
    codes[2 * slots + 1] = 0;
    slots += 1;
  }
  return 4 + 2 * slots;
}

static const GRJIT_UnwindOps * g_unwind_ops; /* __atomic builtins only */

const GRJIT_UnwindOps * grjit_unwind_set_ops(const GRJIT_UnwindOps * ops) {
  return __atomic_exchange_n(&g_unwind_ops, ops, __ATOMIC_ACQ_REL);
}

#if defined(_WIN64) && defined(__x86_64__)
static bool windows_add(void * table, uint32_t count, uintptr_t base) {
  return RtlAddFunctionTable((PRUNTIME_FUNCTION)table, count, (DWORD64)base) != 0;
}

static void windows_remove(void * table) {
  RtlDeleteFunctionTable((PRUNTIME_FUNCTION)table);
}

static const GRJIT_UnwindOps g_windows_ops = {windows_add, windows_remove};
#endif

static const GRJIT_UnwindOps * unwind_ops(void) {
  const GRJIT_UnwindOps * ops = __atomic_load_n(&g_unwind_ops, __ATOMIC_ACQUIRE);
  if (ops != NULL) {
    return ops;
  }
#if defined(_WIN64) && defined(__x86_64__)
  return &g_windows_ops;
#else
  return NULL;
#endif
}

GRJIT_Result grjit_unwind_register(void * base, void * table) {
  const GRJIT_UnwindOps * ops = unwind_ops();
  if (ops == NULL) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  return ops->add(table, 1, (uintptr_t)base) ? GRJIT_OK : GRJIT_ERR_IO;
}

void grjit_unwind_deregister(void * table) {
  const GRJIT_UnwindOps * ops = unwind_ops();
  if (ops != NULL && table != NULL) {
    ops->remove(table);
  }
}

GRJIT_Result grjit_memory_create(const GRCORE_PageProvider * pages,
    GRJIT_Arch arch, const uint8_t * bytes, size_t length,
    const GRJIT_Prologue * prologue, void ** out_mapping,
    size_t * out_mapped_size, void ** out_unwind_table) {
  if (pages == NULL || pages->protect == NULL) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  size_t page = pages->page_size;
  if (page == 0 || length == 0 || length > SIZE_MAX - page) {
    return GRJIT_ERR_INVALID;
  }
  /* Win64: the RUNTIME_FUNCTION and the UNWIND_INFO follow the code in the
   * same pages (RVAs are 32 bits from one base, so an allocation elsewhere
   * might be out of reach), each 4-byte aligned. */
  uint8_t info[GRJIT_UNWIND_INFO_MAX];
  size_t info_size = 0;
  size_t table_at = 0;
  size_t used = length;
  if (arch == GRJIT_ARCH_X86_64_WIN64) {
    if (prologue == NULL || out_unwind_table == NULL) {
      return GRJIT_ERR_INVALID;
    }
    if (length > UINT32_MAX / 2) {
      return GRJIT_ERR_LIMIT; /* the RVAs of the unwind data are 32-bit */
    }
    info_size = grjit_unwind_info_build(prologue, info);
    if (info_size == 0) {
      return GRJIT_ERR_INTERNAL;
    }
    table_at = (length + 3) / 4 * 4;
    used = table_at + GRJIT_RUNTIME_FUNCTION_BYTES + info_size;
  }
  if (used > SIZE_MAX - page) {
    return GRJIT_ERR_INVALID;
  }
  size_t size = (used + page - 1) / page * page;
  void * mapping = pages->map(pages->ctx, size);
  if (mapping == NULL) {
    return GRJIT_ERR_OOM;
  }
  memcpy(mapping, bytes, length);
  /* The tail of the last page would otherwise be zero bytes, which decode as
   * an instruction on x86-64 (and as `udf #0` on arm64, which traps, but
   * only by accident); a breakpoint instruction traps on purpose. */
  if (arch == GRJIT_ARCH_ARM64) {
    static const unsigned char brk0[4] = {0x00, 0x00, 0x20, 0xD4};
    for (size_t at = length; at + 4 <= size; at += 4) {
      memcpy((unsigned char *)mapping + at, brk0, sizeof brk0);
    }
    /* Everything is written, nothing is executable yet. The caches are made
     * coherent here, between the writes and the flip. */
    grjit_icache_sync(mapping, size);
  } else {
    memset((unsigned char *)mapping + length, 0xCC, size - length);
  }
  if (arch == GRJIT_ARCH_X86_64_WIN64) {
    uint8_t * rf = (uint8_t *)mapping + table_at;
    put32(rf, 0);
    put32(rf + 4, (uint32_t)length);
    put32(rf + 8, (uint32_t)(table_at + GRJIT_RUNTIME_FUNCTION_BYTES));
    memcpy(rf + GRJIT_RUNTIME_FUNCTION_BYTES, info, info_size);
  }
  GRCORE_Result r =
      grcore_page_protect(pages, mapping, size, GRCORE_PAGE_READ_EXECUTE);
  if (r != GRCORE_OK) {
    pages->unmap(pages->ctx, mapping, size);
    return r == GRCORE_ERR_IO ? GRJIT_ERR_IO : GRJIT_ERR_INTERNAL;
  }
  if (arch == GRJIT_ARCH_X86_64_WIN64) {
    *out_unwind_table = NULL;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 7
    /* Planted defect 7 (tests only): the table is written and never
     * registered, so a native stack walk finds nothing for the code. */
#else
    GRJIT_Result reg = grjit_unwind_register(mapping, (uint8_t *)mapping + table_at);
    if (reg == GRJIT_OK) {
      *out_unwind_table = (uint8_t *)mapping + table_at;
    } else if (reg != GRJIT_ERR_UNSUPPORTED) {
      pages->unmap(pages->ctx, mapping, size);
      return reg;
    }
#endif
  }
  *out_mapping = mapping;
  *out_mapped_size = size;
  return GRJIT_OK;
}

void grjit_memory_destroy(const GRCORE_PageProvider * pages, void * mapping,
    size_t mapped_size, void * unwind_table) {
  /* Deregistered before the pages go: the operating system must never hold a
   * table that points into unmapped memory. */
  grjit_unwind_deregister(unwind_table);
  if (mapping != NULL) {
    pages->unmap(pages->ctx, mapping, mapped_size);
  }
}
