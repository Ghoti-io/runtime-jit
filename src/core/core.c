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
 * @file
 *
 * Result strings.
 *
 * The string table is indexed by the enum. A new result that is not added
 * here falls through to "Unknown error", and the unit test that walks
 * GRJIT_RESULT_COUNT rejects that.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>

const char * grjit_result_string(GRJIT_Result result) {
  switch (result) {
    case GRJIT_OK:
      return "No error";
    case GRJIT_ERR_IO:
      return "Input/output error";
    case GRJIT_ERR_FORMAT:
      return "Format not recognised";
    case GRJIT_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GRJIT_ERR_LIMIT:
      return "Limit exceeded";
    case GRJIT_ERR_CORRUPT:
      return "Corrupt input";
    case GRJIT_ERR_OOM:
      return "Out of memory";
    case GRJIT_ERR_INVALID:
      return "Invalid argument";
    case GRJIT_ERR_INTERNAL:
      return "Internal error";
    case GRJIT_RESULT_COUNT:
      break;
  }
  return "Unknown error";
}
