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

#include "../backend/backend_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The instruction sets the library can emit. Both can be emitted on any
 *  host; only the one the library was built for can be run. */
typedef enum GRJIT_Arch {
  GRJIT_ARCH_X86_64,
  GRJIT_ARCH_ARM64
} GRJIT_Arch;

/** The architecture of the build, for a target that has a backend. */
GRJIT_INTERNAL_API GRJIT_Arch grjit_native_arch(void);

/** What an emit produces: the machine code, and the metadata for it. */
typedef struct GRJIT_Emitted {
  const GRJIT_Allocator * allocator;
  uint8_t * bytes;
  size_t size;
  GRJIT_MetaStorage meta;
} GRJIT_Emitted;

/**
 * Emits a verified function for `arch` and builds and validates its metadata,
 * without mapping or running anything, so that the structural tests of the
 * arm64 backend run on an x86-64 host. The frame cap is checked here.
 *
 * @return ::GRJIT_OK; ::GRJIT_ERR_LIMIT for a frame or code over its cap (or a
 *   branch that cannot reach); ::GRJIT_ERR_OOM; ::GRJIT_ERR_INTERNAL for
 *   metadata that fails its validator. Nothing is left allocated on failure.
 */
GRJIT_INTERNAL_API GRJIT_Result grjit_emit_for(GRJIT_Arch arch,
    const GRJIT_Function * function, const GRJIT_Allocator * allocator,
    const GRJIT_Limits * limits, GRJIT_EntryHook hook, uint32_t request_offset,
    GRJIT_Emitted * out);

/** Frees what ::grjit_emit_for made. */
GRJIT_INTERNAL_API void grjit_emitted_free(GRJIT_Emitted * emitted);

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
 * read-write, fills the rest of the last page with a trapping instruction of
 * `arch`, makes the instruction cache coherent (arm64) and makes them
 * read-execute with the provider's `protect`. Never both writable and
 * executable (AD-13).
 *
 * @return ::GRJIT_OK; ::GRJIT_ERR_UNSUPPORTED if the provider has no
 *   `protect` (nothing is mapped); ::GRJIT_ERR_OOM if it cannot map;
 *   ::GRJIT_ERR_IO if `protect` fails (the mapping is released first).
 */
GRJIT_INTERNAL_API GRJIT_Result grjit_memory_create(
    const GRCORE_PageProvider * pages, GRJIT_Arch arch, const uint8_t * bytes,
    size_t length, void ** out_mapping, size_t * out_mapped_size);

/**
 * Makes the instruction cache coherent with what was just written to
 * `[mapping, mapping + size)`. On arm64 the data and instruction caches are
 * separate and a write is not visible to instruction fetch until this runs
 * (`__builtin___clear_cache`); on x86-64 it is a no-op. Counted, so a test can
 * see it was reached. **`qemu-user` translates lazily and does not model this**:
 * no run under it proves the call is right, only that it is made.
 */
GRJIT_INTERNAL_API void grjit_icache_sync(void * mapping, size_t size);

/** How many times ::grjit_icache_sync has been called in this process (only the
 *  arm64 path of ::grjit_memory_create calls it). A diagnostic a test reads to
 *  prove the call is reached; nothing depends on it. */
GRJIT_INTERNAL_API uint64_t grjit_icache_sync_count(void);

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

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_CODE_CODE_INTERNAL_H */
