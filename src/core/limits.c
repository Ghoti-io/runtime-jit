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
 * The limits: their defaults, and the resolution of a caller's partly-filled
 * struct (a zero field is that field's default).
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "../ir/ir_internal.h"

#define GRJIT_DEFAULT_BLOCKS 4096u
#define GRJIT_DEFAULT_VREGS 65536u
#define GRJIT_DEFAULT_OPERATIONS 262144u
#define GRJIT_DEFAULT_CALL_ARGUMENTS 6u
#define GRJIT_DEFAULT_GUEST_CALL_ARGUMENTS 16u
#define GRJIT_DEFAULT_FRAME_STATE_SLOTS 4096u
#define GRJIT_DEFAULT_FRAME_BYTES (1u << 20)
#define GRJIT_DEFAULT_CODE_BYTES (16u << 20)
#define GRJIT_DEFAULT_SITE_ENTRIES (4u << 20)
#define GRJIT_DEFAULT_NATIVE_ARGUMENTS 16u
#define GRJIT_DEFAULT_NATIVE_STACK_BYTES (64u << 10)

/* Compiled code addresses its frame with a 32-bit displacement and its
 * branches with a 32-bit offset, so a cap past these is no cap. */
#define GRJIT_FRAME_CEILING ((size_t)1 << 30)
#define GRJIT_CODE_CEILING ((size_t)1 << 30)
/* A native's stack check is `lea rax, [rsp - (S + bytes)]`, a 32-bit displacement. */
#define GRJIT_NATIVE_STACK_CEILING ((size_t)1 << 30)

void grjit_limits_default(GRJIT_Limits * limits) {
  if (limits == NULL) {
    return;
  }
  limits->max_blocks = GRJIT_DEFAULT_BLOCKS;
  limits->max_vregs = GRJIT_DEFAULT_VREGS;
  limits->max_operations = GRJIT_DEFAULT_OPERATIONS;
  limits->max_call_arguments = GRJIT_DEFAULT_CALL_ARGUMENTS;
  limits->max_frame_state_slots = GRJIT_DEFAULT_FRAME_STATE_SLOTS;
  limits->max_frame_bytes = GRJIT_DEFAULT_FRAME_BYTES;
  limits->max_code_bytes = GRJIT_DEFAULT_CODE_BYTES;
  limits->max_site_entries = GRJIT_DEFAULT_SITE_ENTRIES;
  limits->max_guest_call_arguments = GRJIT_DEFAULT_GUEST_CALL_ARGUMENTS;
  limits->max_native_arguments = GRJIT_DEFAULT_NATIVE_ARGUMENTS;
  limits->max_native_stack_bytes = GRJIT_DEFAULT_NATIVE_STACK_BYTES;
}

static size_t clamp_max(size_t value, size_t ceiling) {
  return value > ceiling ? ceiling : value;
}

void grjit_limits_resolve(const GRJIT_Limits * in, GRJIT_Limits * out) {
  grjit_limits_default(out);
  if (in == NULL) {
    return;
  }
  if (in->max_blocks != 0) {
    out->max_blocks = clamp_max(in->max_blocks, UINT32_MAX - 1);
  }
  if (in->max_vregs != 0) {
    out->max_vregs = clamp_max(in->max_vregs, UINT32_MAX - 1);
  }
  if (in->max_operations != 0) {
    out->max_operations = in->max_operations;
  }
  if (in->max_call_arguments != 0) {
    out->max_call_arguments =
        clamp_max(in->max_call_arguments, GRJIT_BACKEND_MAX_ARGS);
  }
  if (in->max_frame_state_slots != 0) {
    /* A guard exit addresses `out[i]` with a 32-bit displacement of 8 * i. */
    out->max_frame_state_slots =
        clamp_max(in->max_frame_state_slots, GRJIT_FRAME_CEILING / 8);
  }
  if (in->max_frame_bytes != 0) {
    out->max_frame_bytes = clamp_max(in->max_frame_bytes, GRJIT_FRAME_CEILING);
  }
  if (in->max_code_bytes != 0) {
    out->max_code_bytes = clamp_max(in->max_code_bytes, GRJIT_CODE_CEILING);
  }
  if (in->max_site_entries != 0) {
    out->max_site_entries = in->max_site_entries;
  }
  if (in->max_guest_call_arguments != 0) {
    out->max_guest_call_arguments =
        clamp_max(in->max_guest_call_arguments, GRJIT_BUILDER_MAX_ARGS);
  }
  if (in->max_native_arguments != 0) {
    out->max_native_arguments =
        clamp_max(in->max_native_arguments, GRJIT_BUILDER_MAX_ARGS);
  }
  if (in->max_native_stack_bytes != 0) {
    out->max_native_stack_bytes =
        clamp_max(in->max_native_stack_bytes, GRJIT_NATIVE_STACK_CEILING);
  }
}
