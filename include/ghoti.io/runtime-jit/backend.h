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
 * @file backend.h
 * @stability free
 *
 * The baseline backends: x86-64 (SysV on Linux, Microsoft x64 on Windows) and
 * arm64.
 *
 * The baseline is deliberately dull: every register and parameter lives in a
 * frame slot at a negative offset from the frame pointer (`rbp` on x86-64,
 * `x29` on arm64, where `[x29]` and `[x29 + 8]` hold the caller's frame
 * pointer and the return address exactly as `[rbp]` and `[rbp + 8]` do), each
 * operation is emitted by itself through fixed caller-saved scratch registers,
 * and there is no register allocator and no pass (AD-26). So no GC reference
 * is ever held in a callee-saved register across a call (AD-17 holds by
 * construction), the stack map of a site is the set of live REF slots, and the
 * stack-map and deopt format, and every consumer's frame walk, are the same on
 * both.
 *
 * The backend is chosen when the library is built, by the compiler's target,
 * never at run time. It exists on Linux x86-64, Linux arm64 and Windows x86-64
 * and is compiled out elsewhere: ::grjit_backend_available is then false and
 * ::grjit_compile returns ::GRJIT_ERR_UNSUPPORTED. Windows arm64 and macOS are
 * not here. On Windows x86-64 the code is in the Microsoft x64 calling
 * convention and carries unwind information registered with the system; on
 * arm64 the instruction cache is made coherent with the code after it is
 * written and before it is made executable.
 */

#ifndef GHOTI_IO_GRJIT_BACKEND_H
#define GHOTI_IO_GRJIT_BACKEND_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/limits.h>

#include <ghoti.io/runtime-core/b/page.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The entry check: called before anything else in the function with
 *   the context.
 *
 * A non-zero return makes the function return ::GRJIT_EXIT_REFUSED with that
 * value in `out[0]`. It is the stand-in for AD-21's "checked at every JIT
 * prologue" until the engine's trampoline owns native-depth accounting.
 *
 * The hook runs before the parameters are in their frame slots and no site is
 * recorded for it, so it **must not reach a GC point**: it may not allocate,
 * poll, or call anything that can, or the caller's reference arguments would be
 * unmapped while a collection ran.
 */
typedef uint32_t (*GRJIT_EntryHook)(void * context);

/** @brief What a compile needs. A struct the caller fills: compiling is a
 *  call, not a long-lived object. */
typedef struct GRJIT_CompileOptions {
  const GRCORE_PageProvider * pages; ///< Required: where the code's pages come
                                     ///< from, with a `protect`. A provider
                                     ///< states its own size (runtime-core's
                                     ///< `b/page.h`), and one that is not
                                     ///< valid is ::GRJIT_ERR_INVALID.
  const GRJIT_Allocator * allocator; ///< NULL for the default.
  const GRJIT_Limits * limits;       ///< NULL for the defaults.
  GRJIT_EntryHook entry_hook;        ///< NULL for none.
} GRJIT_CompileOptions;

/** @brief Whether this build can compile and run code: true on Linux x86-64,
 *  Linux arm64 and Windows x86-64, false elsewhere. */
GRJIT_API bool grjit_backend_available(void);

/**
 * @brief Whether this build can compile calls between compiled functions, tail
 *   calls and calls to natives (AD-28): true on Linux x86-64 SysV, false on every
 *   other target.
 *
 * Elsewhere ::grjit_compile refuses a callable function, and a function with
 * ::GRJIT_OP_CALL_SLOT, ::GRJIT_OP_CALL_PTR, ::GRJIT_OP_TAIL_CALL_SLOT,
 * ::GRJIT_OP_TAIL_CALL_PTR or ::GRJIT_OP_CALL_NATIVE, with
 * ::GRJIT_ERR_UNSUPPORTED before emitting a byte (arm64 and Windows x86-64 have
 * them in a later story), so an engine asks this once and builds callable
 * functions only where it is true, and otherwise leaves a call, a tail call and
 * a native call as an exit.
 */
GRJIT_API bool grjit_backend_calls_available(void);

/**
 * @brief Verifies a function and compiles it.
 *
 * An unverified function is refused with ::GRJIT_ERR_INVALID (a function over
 * a cap, ::GRJIT_ERR_LIMIT). A frame over `max_frame_bytes` or code over
 * `max_code_bytes` is ::GRJIT_ERR_LIMIT. A page provider that is not valid
 * (`grcore_page_provider_valid`) is ::GRJIT_ERR_INVALID; one with no
 * `protect`, or whose size ends before it, is ::GRJIT_ERR_UNSUPPORTED; and one
 * whose `protect` fails, or (Windows x86-64) a
 * system that refuses the unwind registration, is ::GRJIT_ERR_IO. The
 * emitted metadata is validated before this returns; a failure is
 * ::GRJIT_ERR_INTERNAL. On any failure everything made is freed and the page
 * provider's accounting is where it started.
 *
 * @param options The options; `pages` is required.
 * @param function The function.
 * @param out_code Receives the code, only on success.
 * @return ::GRJIT_OK or the error above (also ::GRJIT_ERR_OOM and
 *   ::GRJIT_ERR_UNSUPPORTED on a target with no backend).
 */
GRJIT_API GRJIT_Result grjit_compile(const GRJIT_CompileOptions * options,
    const GRJIT_Function * function, GRJIT_Code ** out_code);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_BACKEND_H */
