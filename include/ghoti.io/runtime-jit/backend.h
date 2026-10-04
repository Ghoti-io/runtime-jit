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
 * The x86-64 baseline backend.
 *
 * The baseline is deliberately dull: every register and parameter lives in a
 * frame slot at a negative offset from `rbp`, each operation is emitted by
 * itself through fixed caller-saved scratch registers, and there is no
 * register allocator and no pass (AD-26). So no GC reference is ever held in
 * a callee-saved register across a call (AD-17 holds by construction), and
 * the stack map of a site is the set of live REF slots.
 *
 * It exists on Linux x86-64 and is compiled out elsewhere:
 * ::grjit_backend_available is then false and ::grjit_compile returns
 * ::GRJIT_ERR_UNSUPPORTED. The Windows unwind registration is a stub.
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
                                     ///< from, with a `protect`.
  const GRJIT_Allocator * allocator; ///< NULL for the default.
  const GRJIT_Limits * limits;       ///< NULL for the defaults.
  GRJIT_EntryHook entry_hook;        ///< NULL for none.
} GRJIT_CompileOptions;

/** @brief Whether this build can compile and run code: true on Linux x86-64. */
GRJIT_API bool grjit_backend_available(void);

/**
 * @brief Verifies a function and compiles it.
 *
 * An unverified function is refused with ::GRJIT_ERR_INVALID (a function over
 * a cap, ::GRJIT_ERR_LIMIT). A frame over `max_frame_bytes` or code over
 * `max_code_bytes` is ::GRJIT_ERR_LIMIT. A page provider with no `protect` is
 * ::GRJIT_ERR_UNSUPPORTED and one whose `protect` fails is ::GRJIT_ERR_IO. The
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
