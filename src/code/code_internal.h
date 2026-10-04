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

/** The instruction sets (and calling conventions) the library can emit. Every
 *  one can be emitted on any host; only the one the library was built for can
 *  be run. `GRJIT_ARCH_X86_64_WIN64` is the x86-64 backend with the Microsoft
 *  calling convention and Windows unwind information: a flavour of the same
 *  emitter, not a second one. */
typedef enum GRJIT_Arch {
  GRJIT_ARCH_X86_64,
  GRJIT_ARCH_ARM64,
  GRJIT_ARCH_X86_64_WIN64
} GRJIT_Arch;

/** The architecture of the build, for a target that has a backend. */
GRJIT_INTERNAL_API GRJIT_Arch grjit_native_arch(void);

/** What an emit produces: the machine code, and the metadata for it. */
typedef struct GRJIT_Emitted {
  const GRJIT_Allocator * allocator;
  uint8_t * bytes;
  size_t size;
  GRJIT_MetaStorage meta;
  GRJIT_Prologue prologue; ///< x86-64 only: where the prologue's instructions end.
  uint32_t regs_used;      ///< x86-64 only: bit r set if register r was encoded.
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
  void * unwind_table; ///< The registered RUNTIME_FUNCTION (Win64), or NULL.
};

/**
 * Maps pages from `pages`, copies `length` bytes into them while they are
 * read-write, fills the rest of the last page with a trapping instruction of
 * `arch`, makes the instruction cache coherent (arm64) and makes them
 * read-execute with the provider's `protect`. Never both writable and
 * executable (AD-13).
 *
 * For ::GRJIT_ARCH_X86_64_WIN64 the unwind information is written in the same
 * mapping, 4-byte aligned after the code (a `RUNTIME_FUNCTION` for
 * `[0, length)`, then the `UNWIND_INFO` it names, RVAs from the start of the
 * mapping), before the flip, and registered after it with
 * ::grjit_unwind_register; `*out_unwind_table` receives the registered table,
 * or NULL if this target registers nothing.
 *
 * @param prologue Required for Win64, ignored otherwise.
 * @param out_unwind_table Required for Win64, ignored otherwise.
 * @return ::GRJIT_OK; ::GRJIT_ERR_UNSUPPORTED if the provider has no
 *   `protect` (nothing is mapped); ::GRJIT_ERR_OOM if it cannot map;
 *   ::GRJIT_ERR_IO if `protect` or the registration fails (the mapping is
 *   released first, and nothing stays registered).
 */
GRJIT_INTERNAL_API GRJIT_Result grjit_memory_create(
    const GRCORE_PageProvider * pages, GRJIT_Arch arch, const uint8_t * bytes,
    size_t length, const GRJIT_Prologue * prologue, void ** out_mapping,
    size_t * out_mapped_size, void ** out_unwind_table);

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

/** Releases a mapping made by ::grjit_memory_create, first removing its
 *  unwind registration (`unwind_table`, NULL if none): the operating system
 *  must not be left holding a table that points into unmapped pages. */
GRJIT_INTERNAL_API void grjit_memory_destroy(const GRCORE_PageProvider * pages,
    void * mapping, size_t mapped_size, void * unwind_table);

/** The largest `UNWIND_INFO` ::grjit_unwind_info_build writes: the four-byte
 *  header, five code slots (a 32-bit allocation, `SET_FPREG`, `PUSH_NONVOL`)
 *  and one slot of padding to a whole number of DWORDs. */
#define GRJIT_UNWIND_INFO_MAX 16
/** The size of a `RUNTIME_FUNCTION`: three DWORDs, begin, end and the unwind
 *  information, all RVAs. */
#define GRJIT_RUNTIME_FUNCTION_BYTES 12

/**
 * Builds the Windows x64 `UNWIND_INFO` (version 1, no flags, no handler) of a
 * frame `push rbp; mov rbp, rsp; sub rsp, N`: frame register `rbp` at offset
 * 0, and the unwind codes, latest first, `ALLOC_SMALL` (8 to 128 bytes),
 * `ALLOC_LARGE` with a 16-bit count of 8-byte units (up to 512 KiB minus 8) or
 * with a 32-bit size, `SET_FPREG`, `PUSH_NONVOL rbp`, each at the offset of
 * the end of its instruction. `alloc_bytes` of 0 has no allocation code. The
 * array is padded to an even number of slots.
 *
 * Portable and pure, so its bytes are tested on every host.
 *
 * @param out At least ::GRJIT_UNWIND_INFO_MAX bytes.
 * @return The size in bytes (a multiple of 4), or 0 if the prologue cannot be
 *   described: an `alloc_bytes` that is not a multiple of 8, offsets that are
 *   out of order, or a prologue longer than the 255 bytes `SizeOfProlog` holds.
 */
GRJIT_INTERNAL_API size_t grjit_unwind_info_build(
    const GRJIT_Prologue * prologue, uint8_t * out);

/** What registers and removes a table with the operating system. */
typedef struct GRJIT_UnwindOps {
  /** Registers `count` entries at `table`, RVAs from `base`. True if done. */
  bool (*add)(void * table, uint32_t count, uintptr_t base);
  /** Removes what `add` registered at `table`. */
  void (*remove)(void * table);
} GRJIT_UnwindOps;

/**
 * Registers the one `RUNTIME_FUNCTION` at `table` (the begin address is an RVA
 * from `base`) so that a native stack walk can pass through the frame. On
 * Windows x86-64 it is `RtlAddFunctionTable`; on every other target there is
 * nothing to register with and the answer is ::GRJIT_ERR_UNSUPPORTED, which
 * ::grjit_memory_create reads as "nothing to do".
 *
 * @return ::GRJIT_OK; ::GRJIT_ERR_UNSUPPORTED on a target with no registration;
 *   ::GRJIT_ERR_IO if the operating system refused.
 */
GRJIT_INTERNAL_API GRJIT_Result grjit_unwind_register(void * base, void * table);

/** Removes a registration made by ::grjit_unwind_register. */
GRJIT_INTERNAL_API void grjit_unwind_deregister(void * table);

/**
 * Replaces what registers and removes tables, for the tests: a registration
 * that is seen to be made, in order, and to fail, on a host that has no
 * unwinder of its own. NULL restores the target's own (Windows x86-64's
 * `Rtl` functions; nothing elsewhere). Returns what was in force, NULL for the
 * target's own. Process-wide, like ::grjit_icache_sync_count, and for tests
 * only: nothing in the library sets it.
 */
GRJIT_INTERNAL_API const GRJIT_UnwindOps * grjit_unwind_set_ops(
    const GRJIT_UnwindOps * ops);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_CODE_CODE_INTERNAL_H */
