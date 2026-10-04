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
 * Library-wide entry points: the version this build reports.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/libver.h>

const char * grjit_version_string(void) {
  return GRJIT_VERSION_STRING;
}

unsigned grjit_version_number(void) {
  return GRJIT_VERSION_NUMBER;
}
