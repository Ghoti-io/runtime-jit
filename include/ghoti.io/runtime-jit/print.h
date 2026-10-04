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
 * @file print.h
 * @stability free
 *
 * The IR printer: deterministic text, and the only way tests assert the shape
 * of a function.
 */

#ifndef GHOTI_IO_GRJIT_PRINT_H
#define GHOTI_IO_GRJIT_PRINT_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Writes a function as text into the caller's buffer.
 *
 * As `snprintf` does: at most `size` bytes are written, terminator included,
 * and `*out_length` receives the length the whole text needs, so a caller
 * can size the buffer and call again. The same function always prints the
 * same text.
 *
 * @param function The function.
 * @param buffer The buffer; may be NULL when `size` is 0.
 * @param size Its size in bytes.
 * @param out_length Receives the full length, excluding the terminator;
 *   written on success.
 * @return ::GRJIT_OK or ::GRJIT_ERR_INVALID for a NULL function or output.
 */
GRJIT_API GRJIT_Result grjit_function_print(const GRJIT_Function * function,
    char * buffer, size_t size, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_PRINT_H */
