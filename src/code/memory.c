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

/* One UNWIND_INFO: Version 1 and no flags; SizeOfProlog; CountOfCodes;
 * FrameRegister (rbp, 5) with FrameOffset 0, or none. Then the codes, each a byte
 * of offset (the end of the instruction in the prologue, from the function's
 * first byte) and a byte of operation (low nibble) and information (high nibble),
 * latest first, and a padding slot if their number is odd. `setfp_end` of zero is
 * a frame with no frame register (the entry adapter's: it sets `rbp` to a marker
 * that a frame-register unwinder would take for a base). */
static size_t info_build(uint32_t push_end, uint32_t setfp_end, bool frame_register,
    uint32_t alloc_end, uint32_t alloc_bytes, uint8_t * out) {
  enum { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE = 1, UWOP_ALLOC_SMALL = 2,
         UWOP_SET_FPREG = 3, RBP = 5 };
  if (push_end == 0 || alloc_end > 255 || alloc_bytes % 8 != 0 ||
      (frame_register ? (setfp_end <= push_end || alloc_end < setfp_end)
                      : (setfp_end != 0 || alloc_end < push_end))) {
    return 0;
  }
  uint8_t * codes = out + 4;
  size_t slots = 0;
  uint32_t n = alloc_bytes;
  if (n != 0) {
    if (n <= 128) {
      codes[2 * slots] = (uint8_t)alloc_end;
      codes[2 * slots + 1] =
          (uint8_t)(UWOP_ALLOC_SMALL | (((n - 8) / 8) << 4));
      slots += 1;
    } else if (n / 8 <= 0xFFFF) {
      codes[2 * slots] = (uint8_t)alloc_end;
      codes[2 * slots + 1] = (uint8_t)(UWOP_ALLOC_LARGE | (0 << 4));
      codes[2 * slots + 2] = (uint8_t)((n / 8) & 0xFF);
      codes[2 * slots + 3] = (uint8_t)((n / 8) >> 8);
      slots += 2;
    } else {
      codes[2 * slots] = (uint8_t)alloc_end;
      codes[2 * slots + 1] = (uint8_t)(UWOP_ALLOC_LARGE | (1 << 4));
      put32(codes + 2 * slots + 2, n);
      slots += 3;
    }
  }
  if (frame_register) {
    codes[2 * slots] = (uint8_t)setfp_end;
    codes[2 * slots + 1] = UWOP_SET_FPREG;
    slots += 1;
  }
  codes[2 * slots] = (uint8_t)push_end;
  codes[2 * slots + 1] = (uint8_t)(UWOP_PUSH_NONVOL | (RBP << 4));
  slots += 1;
  out[0] = 1;
  out[1] = (uint8_t)alloc_end;
  out[2] = (uint8_t)slots;
  out[3] = (uint8_t)(frame_register ? RBP : 0);
  if (slots % 2 != 0) {
    codes[2 * slots] = 0;
    codes[2 * slots + 1] = 0;
    slots += 1;
  }
  return 4 + 2 * slots;
}

size_t grjit_unwind_info_build(const GRJIT_Prologue * p, uint8_t * out) {
  /* The body's offsets are from the function's own first byte, which is the
   * internal entry for a callable function and zero for a plain one. */
  const uint32_t b = p->body_begin;
  if (p->push_end < b || p->setfp_end < b || p->alloc_end < b) {
    return 0;
  }
  /* A body, plain or callable, has a frame register: a prologue with no `mov rbp, rsp` is refused, and the
   * form with none is the adapter's builder's alone. */
  return info_build(p->push_end - b, p->setfp_end - b, true, p->alloc_end - b, p->alloc_bytes, out);
}

size_t grjit_unwind_info_build_adapter(const GRJIT_Prologue * p, uint8_t * out) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 33
  /* Planted defect 33 (tests only): the adapter's unwind information names rbp as a frame
   * register, which the adapter has set to the chain-end marker: an unwinder takes the
   * marker for the base of the frame and finds its caller in garbage. */
  return info_build(p->adapter_push_end, p->adapter_alloc_end, true, p->adapter_alloc_end,
      p->adapter_alloc_bytes, out);
#else
  return info_build(p->adapter_push_end, 0, false, p->adapter_alloc_end, p->adapter_alloc_bytes, out);
#endif
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

GRJIT_Result grjit_unwind_register(void * base, void * table, uint32_t count) {
  const GRJIT_UnwindOps * ops = unwind_ops();
  if (ops == NULL) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  return ops->add(table, count, (uintptr_t)base) ? GRJIT_OK : GRJIT_ERR_IO;
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
  if (!grcore_page_provider_valid(pages) ||
      !GRCORE_PAGE_PROVIDER_HAS(pages, protect) || pages->protect == NULL) {
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
  uint8_t adapter_info[GRJIT_UNWIND_INFO_MAX];
  size_t info_size = 0;
  size_t adapter_info_size = 0;
  size_t table_at = 0;
  size_t used = length;
  /* A callable function is two functions to the unwinder (the adapter and the
   * body), registered together, so a walk through the code finds either. */
  uint32_t entries = 1;
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
    if (prologue->adapter_end != 0) {
      adapter_info_size = grjit_unwind_info_build_adapter(prologue, adapter_info);
      if (adapter_info_size == 0 || prologue->adapter_end > prologue->body_begin ||
          prologue->body_begin >= length) {
        return GRJIT_ERR_INTERNAL;
      }
      entries = 2;
    }
    table_at = (length + 3) / 4 * 4;
    used = table_at + entries * GRJIT_RUNTIME_FUNCTION_BYTES + info_size + adapter_info_size;
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
    /* The entries sorted by begin address (the adapter's, at zero, then the
     * body's), then the unwind information they name. */
    uint8_t * rf = (uint8_t *)mapping + table_at;
    const size_t info_at = table_at + entries * GRJIT_RUNTIME_FUNCTION_BYTES;
    const size_t adapter_info_at = info_at + info_size;
    uint8_t * body_rf = rf;
    if (entries == 2) {
      put32(rf, 0);
      put32(rf + 4, prologue->adapter_end);
      put32(rf + 8, (uint32_t)adapter_info_at);
      body_rf = rf + GRJIT_RUNTIME_FUNCTION_BYTES;
      memcpy((uint8_t *)mapping + adapter_info_at, adapter_info, adapter_info_size);
    }
    put32(body_rf, prologue->body_begin);
    put32(body_rf + 4, (uint32_t)length);
    put32(body_rf + 8, (uint32_t)info_at);
    memcpy((uint8_t *)mapping + info_at, info, info_size);
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
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 35
    /* Planted defect 35 (tests only): only the adapter's RUNTIME_FUNCTION is registered, so
     * a walk through a body finds nothing. */
    GRJIT_Result reg = grjit_unwind_register(mapping, (uint8_t *)mapping + table_at, 1);
#else
    GRJIT_Result reg = grjit_unwind_register(mapping, (uint8_t *)mapping + table_at, entries);
#endif
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
