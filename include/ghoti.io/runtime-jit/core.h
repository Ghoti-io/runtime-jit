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
 * @file core.h
 * @stability free
 *
 * Result codes and the version this build reports.
 */

#ifndef GHOTI_IO_GRJIT_CORE_H
#define GHOTI_IO_GRJIT_CORE_H

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of an operation.
 *
 * Zero is success.  ::GRJIT_RESULT_COUNT closes the enum so a test can check
 * the string table is complete.
 *
 * This is the suite's fixed vocabulary, numbered as in every other library
 * and as in runtime-core's ::GRCORE_Result up to ::GRJIT_ERR_INTERNAL. It has
 * no ERR_GUEST: a JIT has no guest of its own. Malformed IR is
 * ::GRJIT_ERR_INVALID, a stated cap in ::GRJIT_Limits is ::GRJIT_ERR_LIMIT, a
 * target the backend does not exist for is ::GRJIT_ERR_UNSUPPORTED, a page
 * provider that cannot protect is ::GRJIT_ERR_IO, and metadata that fails its
 * own validator is ::GRJIT_ERR_INTERNAL.
 */
typedef enum {
  GRJIT_OK = 0,          ///< The operation succeeded.
  GRJIT_ERR_IO,          ///< A read, write, or seek failed.
  GRJIT_ERR_FORMAT,      ///< Well-formed bytes, but not a format handled here.
  GRJIT_ERR_UNSUPPORTED, ///< The format is known and the feature is not
                          ///< implemented.
  GRJIT_ERR_LIMIT,       ///< A stated cap was exceeded.
  GRJIT_ERR_CORRUPT,     ///< The bytes are not a valid encoding of this
                          ///< format.
  GRJIT_ERR_OOM,         ///< The allocator returned NULL.
  GRJIT_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GRJIT_ERR_INTERNAL,    ///< The library's own invariant failed.
  GRJIT_RESULT_COUNT
} GRJIT_Result;

/**
 * @brief Static description of a result code.
 *
 * The string is never NULL, never allocated, and never contains caller data.
 *
 * @param result The result code, including values outside the enum.
 * @return A static string.
 */
GRJIT_API const char * grjit_result_string(GRJIT_Result result);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * @return A static string, never NULL.  "0.0.0", "0.0.0-dev" when BRANCH
 *   was overridden, and with "-debug" appended for a BUILD=debug build
 *   ("0.0.0-debug", "0.0.0-dev-debug").
 */
GRJIT_API const char * grjit_version_string(void);

/**
 * @brief This build's version, packed as ::GRJIT_MAKE_VERSION packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GRJIT_API unsigned grjit_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_CORE_H */
