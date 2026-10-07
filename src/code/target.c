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
 * The check a call through a code pointer makes at run time (AD-28): the target
 * must be the internal entry of compiled code registered in the context, so an
 * address that is not compiled code is never entered.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "code_internal.h"

#include <ghoti.io/runtime-core/a/registry.h>

#include <string.h>

uint32_t grjit_call_target_ok(
    void * context, uint64_t target, uint64_t callee, uint64_t arg_count) {
  GRCORE_CodeRange range;
  /* Every internal entry is on a sixteen-byte boundary with its tag before it (both emitters
   * pad to it), so an address that is not is no entry, whatever bytes lie before it: a branch
   * to a misaligned address faults on arm64, and on every target it is the middle of an
   * instruction or of the code's own data. */
  if ((target & (GRJIT_ENTRY_TAG_BYTES - 1u)) != 0) {
    return 0;
  }
  if (!grcore_code_lookup((const GRCORE_Context *)context, (uintptr_t)target, &range) ||
      range.retired || target < range.start + GRJIT_ENTRY_TAG_BYTES) {
    return 0;
  }
  /* The tag is in the sixteen bytes before the entry, which are inside the
   * range because the entry is at least that far in. */
  uint64_t tag[2];
  memcpy(tag, (const void *)(uintptr_t)(target - sizeof tag), sizeof tag);
  return tag[0] == GRJIT_ENTRY_TAG_WORD(arg_count) && tag[1] == callee ? 1u : 0u;
}
