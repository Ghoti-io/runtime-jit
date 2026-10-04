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
 * @file code_internal.h
 *
 * The compiled-code handle and the code memory behind it. Never installed.
 */

#ifndef GHOTI_IO_GRJIT_SRC_CODE_CODE_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_CODE_CODE_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/code.h>

#include <ghoti.io/runtime-core/b/page.h>

#include "../x86_64/emit_internal.h"

struct GRJIT_Code {
  const GRJIT_Allocator * allocator;
  const GRCORE_PageProvider * pages;
  void * mapping;
  size_t mapped_size;
  size_t code_bytes;
  GRJIT_EntryFn entry;
  GRJIT_MetaStorage meta;
  size_t out_words;
  size_t param_count;
  uint32_t request_offset;
};

/**
 * Maps pages from `pages`, copies `length` bytes into them while they are
 * read-write, and makes them read-execute with the provider's `protect`.
 * Never both writable and executable (AD-13).
 *
 * @return ::GRJIT_OK; ::GRJIT_ERR_UNSUPPORTED if the provider has no
 *   `protect` (nothing is mapped); ::GRJIT_ERR_OOM if it cannot map;
 *   ::GRJIT_ERR_IO if `protect` fails (the mapping is released first).
 */
GRJIT_Result grjit_memory_create(const GRCORE_PageProvider * pages,
    const uint8_t * bytes, size_t length, void ** out_mapping,
    size_t * out_mapped_size);

/** Releases a mapping made by ::grjit_memory_create. */
void grjit_memory_destroy(
    const GRCORE_PageProvider * pages, void * mapping, size_t mapped_size);

/**
 * Registers the code's unwind information with the operating system.
 *
 * TODO(windows): Windows needs RtlAddFunctionTable for a native stack walk
 * through compiled frames; it is not written. On Linux the frame-pointer
 * chain is enough for this library's consumers. Always
 * ::GRJIT_ERR_UNSUPPORTED today.
 */
GRJIT_Result grjit_unwind_register(void * mapping, size_t size);

#endif /* GHOTI_IO_GRJIT_SRC_CODE_CODE_INTERNAL_H */
