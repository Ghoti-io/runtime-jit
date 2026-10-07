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
 * @file limits.h
 * @stability free
 *
 * The caps on what the IR, the verifier and the backend will accept.
 *
 * `runtime-jit` has no options object: compiling is a call, not a long-lived
 * object, so its configuration is the struct the caller fills for that call
 * (::GRJIT_CompileOptions) and this one, with a `_default()` as CONVENTIONS.md
 * section 5 gives every library that reads untrusted structure.
 */

#ifndef GHOTI_IO_GRJIT_LIMITS_H
#define GHOTI_IO_GRJIT_LIMITS_H

#include <ghoti.io/runtime-jit/macros.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The caps. A field of zero means that field's default, and a `NULL`
 *   `GRJIT_Limits *` anywhere in this library means all defaults.
 *
 * A cap that is exceeded is reported as ::GRJIT_ERR_LIMIT and changes
 * nothing. The backend passes at most 6 integer arguments to a helper, so a
 * call with more is a limit error from the verifier whatever
 * `max_call_arguments` says; the builder holds up to 16 so that the verifier
 * can be shown the refusal. A call to another compiled function has its own cap,
 * `max_guest_call_arguments`.
 */
typedef struct GRJIT_Limits {
  size_t max_blocks;           ///< Basic blocks in a function. Default 4096.
  size_t max_vregs;            ///< Virtual registers. Default 65536.
  size_t max_operations;       ///< Operations in a function. Default 262144.
  size_t max_call_arguments;   ///< Arguments of a call. Default 6.
  size_t max_frame_state_slots; ///< Slots of one frame state. Default 4096.
  size_t max_frame_bytes;      ///< Native frame of compiled code, bytes.
                               ///< Default 1 MiB.
  size_t max_code_bytes;       ///< Emitted machine code, bytes. Default
                               ///< 16 MiB.
  size_t max_site_entries;     ///< The registers recorded as live, summed over
                               ///< every site of a function (what the stack
                               ///< maps are made of). Default 4 Mi, 16 MiB of
                               ///< working memory. The code and frame caps do
                               ///< not bound it: sites times live registers
                               ///< can be many times either.
  size_t max_guest_call_arguments; ///< Arguments of a call or a tail call to
                               ///< another compiled function (`CALL_SLOT`,
                               ///< `CALL_PTR`, `TAIL_CALL_SLOT`, `TAIL_CALL_PTR`), which
                               ///< the internal convention passes in registers
                               ///< (six on x86-64 SysV, eight on arm64) and then
                               ///< on the stack. Default and ceiling 16.
  size_t max_native_arguments; ///< Arguments of a native (`CALL_NATIVE`), not
                               ///< counting the context, which every native
                               ///< takes first: the C ABI passes the first five
                               ///< in registers on x86-64 SysV (seven on arm64)
                               ///< and the rest on the stack.
                               ///< Default and ceiling 16.
  size_t max_native_stack_bytes; ///< The most `GRJIT_NativeDesc::stack_bytes` a
                               ///< descriptor may declare. Default 64 KiB;
                               ///< ceiling 1 GiB.
} GRJIT_Limits;

/**
 * @brief Fills `limits` with the defaults.
 *
 * @param limits The struct to initialise; NULL is ignored.
 */
GRJIT_API void grjit_limits_default(GRJIT_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_LIMITS_H */
