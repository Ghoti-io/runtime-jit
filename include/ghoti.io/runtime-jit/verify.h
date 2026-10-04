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
 * @file verify.h
 * @stability free
 *
 * The IR verifier. The backend refuses a function that has not passed it.
 */

#ifndef GHOTI_IO_GRJIT_VERIFY_H
#define GHOTI_IO_GRJIT_VERIFY_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/limits.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Checks a function.
 *
 * Refuses, with ::GRJIT_ERR_INVALID unless the offence is a limit: a block
 * that does not end in exactly one terminator or that has one before its
 * end; a branch to a block that does not exist; an operand of the wrong type
 * for its operation; a register used where it may not have been assigned on
 * every path (the reason names the register and the block); a frame state
 * whose length is not the function's `interp_slot_count` or that names a
 * register the function does not have; a `GC_POINT` call or a `POLL` with no
 * frame state, or a `POLL` in a function with no poll helper; a derived
 * pointer that is malformed or live at a site where its base is not. A
 * function over a cap of `limits`, or a call of more than 6 arguments, is
 * ::GRJIT_ERR_LIMIT.
 *
 * Nothing is changed. The caller's `reason` buffer receives the first
 * offence as text, written only when the function is refused.
 *
 * @param function The function.
 * @param limits The caps; NULL for the defaults.
 * @param reason A buffer for the reason; may be NULL.
 * @param reason_size Its size in bytes.
 * @return ::GRJIT_OK, ::GRJIT_ERR_INVALID, ::GRJIT_ERR_LIMIT or
 *   ::GRJIT_ERR_OOM (the dataflow allocates).
 */
GRJIT_API GRJIT_Result grjit_function_verify(const GRJIT_Function * function,
    const GRJIT_Limits * limits, char * reason, size_t reason_size);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_VERIFY_H */
