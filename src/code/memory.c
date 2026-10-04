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
 * `VirtualProtect` branch, which is written and not yet run there.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "code_internal.h"

#include <string.h>

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

GRJIT_Result grjit_memory_create(const GRCORE_PageProvider * pages,
    GRJIT_Arch arch, const uint8_t * bytes, size_t length, void ** out_mapping,
    size_t * out_mapped_size) {
  if (pages == NULL || pages->protect == NULL) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  size_t page = pages->page_size;
  if (page == 0 || length == 0 || length > SIZE_MAX - page) {
    return GRJIT_ERR_INVALID;
  }
  size_t size = (length + page - 1) / page * page;
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
  GRCORE_Result r =
      grcore_page_protect(pages, mapping, size, GRCORE_PAGE_READ_EXECUTE);
  if (r != GRCORE_OK) {
    pages->unmap(pages->ctx, mapping, size);
    return r == GRCORE_ERR_IO ? GRJIT_ERR_IO : GRJIT_ERR_INTERNAL;
  }
  *out_mapping = mapping;
  *out_mapped_size = size;
  return GRJIT_OK;
}

void grjit_memory_destroy(
    const GRCORE_PageProvider * pages, void * mapping, size_t mapped_size) {
  if (mapping != NULL) {
    pages->unmap(pages->ctx, mapping, mapped_size);
  }
}

GRJIT_Result grjit_unwind_register(void * mapping, size_t size) {
  (void)mapping;
  (void)size;
  /* TODO(windows): RtlAddFunctionTable; see notes/suite/WINDOWS-TODO.md. */
  return GRJIT_ERR_UNSUPPORTED;
}
