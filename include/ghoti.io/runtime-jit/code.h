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
 * @file code.h
 * @stability free
 *
 * Compiled code: what ::grjit_compile returns, how it is called, and the
 * metadata it carries.
 *
 * One ::GRJIT_Code owns one page mapping, taken from the page provider the
 * caller named (a context's, so the pages are counted, AD-13). The mapping is
 * written while it is read-write and flipped to read-execute once; it is
 * never both. x86-64 needs no instruction-cache maintenance after the flip.
 *
 * A ::GRJIT_Code is immutable once compiled and may be called from any thread
 * the engine allows (AD-22). It is single-owner: reference counting of
 * compiled code by the core (AD-13) and a code cache arrive with tier-up, not
 * here. Destroying a code object unmaps its pages, so destroying one that a
 * thread is still running is the caller's error.
 *
 * **The calling convention** is the target's own for
 * `uint32_t (void * context, const uint64_t * args, uint64_t * out)`: SysV on
 * Linux x86-64 (`rdi`, `rsi`, `rdx`), Microsoft x64 on Windows x86-64 (`rcx`,
 * `rdx`, `r8`) and AAPCS64 on Linux arm64. The function loads its parameters
 * from `args`, and returns one of the exits below. `out` must hold
 * ::grjit_code_out_words words. On Windows x86-64 the code's pages also hold its
 * unwind information, registered with the operating system for as long as the
 * code lives, so that a native stack walk (a debugger, an exception dispatch,
 * `RtlVirtualUnwind`) passes through the compiled frame; ::grjit_code_destroy
 * removes the registration before it unmaps the pages.
 */

#ifndef GHOTI_IO_GRJIT_CODE_H
#define GHOTI_IO_GRJIT_CODE_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>

#include <ghoti.io/runtime-core/a/codemeta.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How compiled code ended. */
typedef enum GRJIT_Exit {
  /** `RET` ran. The returned value, if any, is in `out[0]`. */
  GRJIT_EXIT_RETURNED = 0,
  /** A guard failed. `out[0..interp_slot_count)` hold the guard's frame
   *  state (a register's value, a constant, zero for a dead slot) and
   *  `out[interp_slot_count]` the guard site's code offset, which
   *  ::grcore_codemeta_find turns into the site's record. */
  GRJIT_EXIT_DEOPT = 1,
  /** The entry hook or the poll helper returned non-zero; the value is in
   *  `out[0]` and nothing after it ran. */
  GRJIT_EXIT_REFUSED = 2
} GRJIT_Exit;

/** @brief The signature of compiled code. */
typedef uint32_t (*GRJIT_EntryFn)(
    void * context, const uint64_t * args, uint64_t * out);

/** @brief Compiled code. Opaque. */
typedef struct GRJIT_Code GRJIT_Code;

/**
 * @brief Unmaps the code's pages and frees it.
 *
 * Calling this while any thread is inside the code is the caller's error.
 *
 * @param code The code; NULL is ignored.
 */
GRJIT_API void grjit_code_destroy(GRJIT_Code * code);

/**
 * @brief The stack maps and deopt records of every site, owned by the code.
 *
 * Validated by ::grcore_codemeta_validate before ::grjit_compile returned.
 * Offsets are from ::grjit_code_address. Never NULL for a live code.
 */
GRJIT_API const GRCORE_CodeMeta * grjit_code_meta(const GRJIT_Code * code);

/** @brief The entry point, with the calling convention above. */
GRJIT_API GRJIT_EntryFn grjit_code_entry(const GRJIT_Code * code);

/** @brief The address of the first byte of code. */
GRJIT_API const void * grjit_code_address(const GRJIT_Code * code);

/** @brief The number of bytes of machine code. */
GRJIT_API size_t grjit_code_size(const GRJIT_Code * code);

/** @brief The number of bytes mapped (the code rounded to whole pages). */
GRJIT_API size_t grjit_code_mapped_size(const GRJIT_Code * code);

/** @brief The number of words `out` must hold: at least one, and one more
 *  than the function's interpreter slots. */
GRJIT_API size_t grjit_code_out_words(const GRJIT_Code * code);

/** @brief The number of parameters, which `args` must hold. */
GRJIT_API size_t grjit_code_param_count(const GRJIT_Code * code);

/**
 * @brief Calls the code.
 *
 * Asserts, in a build without `NDEBUG`, that the layout descriptor of
 * runtime-core still names the request-word offset the code was compiled
 * against.
 *
 * @param code The code.
 * @param context The context pointer the code's polls load through.
 * @param args The parameters, `grjit_code_param_count` words.
 * @param out At least ::grjit_code_out_words words.
 * @return A ::GRJIT_Exit.
 */
GRJIT_API uint32_t grjit_code_call(const GRJIT_Code * code, void * context,
    const uint64_t * args, uint64_t * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_CODE_H */
