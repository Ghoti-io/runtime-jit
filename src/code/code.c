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
 * Compiling a function, and the compiled-code handle. Everything that
 * produces or runs machine code is for Linux x86-64 only; elsewhere
 * ::grjit_backend_available is false and ::grjit_compile says so.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/verify.h>

#include "code_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/layout.h>

#include <assert.h>
#include <string.h>

#if defined(__x86_64__) && defined(__linux__)
#define GRJIT_HAVE_BACKEND 1
#else
#define GRJIT_HAVE_BACKEND 0
#endif

bool grjit_backend_available(void) {
  return GRJIT_HAVE_BACKEND != 0;
}

void grjit_code_destroy(GRJIT_Code * code) {
  if (code == NULL) {
    return;
  }
  const GRJIT_Allocator * a = code->allocator;
  grjit_memory_destroy(code->pages, code->mapping, code->mapped_size);
  grjit_metadata_free(&code->meta);
  a->free_fn(a->ctx, code);
}

const GRCORE_CodeMeta * grjit_code_meta(const GRJIT_Code * code) {
  return code == NULL ? NULL : &code->meta.meta;
}

GRJIT_EntryFn grjit_code_entry(const GRJIT_Code * code) {
  return code == NULL ? NULL : code->entry;
}

const void * grjit_code_address(const GRJIT_Code * code) {
  return code == NULL ? NULL : code->mapping;
}

size_t grjit_code_size(const GRJIT_Code * code) {
  return code == NULL ? 0 : code->code_bytes;
}

size_t grjit_code_mapped_size(const GRJIT_Code * code) {
  return code == NULL ? 0 : code->mapped_size;
}

size_t grjit_code_out_words(const GRJIT_Code * code) {
  return code == NULL ? 0 : code->out_words;
}

size_t grjit_code_param_count(const GRJIT_Code * code) {
  return code == NULL ? 0 : code->param_count;
}

uint32_t grjit_code_call(const GRJIT_Code * code, void * context,
    const uint64_t * args, uint64_t * out) {
  /* Code is compiled against one build's layout of the context; a core that
   * moved the request word makes every poll in it read the wrong word. */
  assert(code->request_offset == grcore_jit_layout()->request_word_offset);
  return code->entry(context, args, out);
}

static GRJIT_Result verify_for_compile(
    const GRJIT_Function * f, const GRJIT_Limits * limits) {
  GRJIT_Result r = grjit_function_verify(f, limits, NULL, 0);
  if (r == GRJIT_ERR_LIMIT || r == GRJIT_ERR_OOM) {
    return r;
  }
  return r == GRJIT_OK ? GRJIT_OK : GRJIT_ERR_INVALID;
}

GRJIT_Result grjit_compile(const GRJIT_CompileOptions * options,
    const GRJIT_Function * function, GRJIT_Code ** out_code) {
  if (options == NULL || function == NULL || out_code == NULL ||
      options->pages == NULL) {
    return GRJIT_ERR_INVALID;
  }
#if !GRJIT_HAVE_BACKEND
  return GRJIT_ERR_UNSUPPORTED;
#else
  const GRJIT_Allocator * a = grjit_allocator_or_default(options->allocator);
  GRJIT_Limits limits;
  grjit_limits_resolve(options->limits, &limits);
  GRJIT_Result r = verify_for_compile(function, options->limits);
  if (r != GRJIT_OK) {
    return r;
  }
  if (options->pages->protect == NULL) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  /* Three fixed slots and one per register, rounded to keep rsp 16-aligned. */
  size_t slots = function->vreg_count + GRJIT_FIXED_SLOTS;
  size_t frame = (slots * 8 + 15) / 16 * 16;
  if (frame > limits.max_frame_bytes) {
    return GRJIT_ERR_LIMIT;
  }

  GRJIT_LiveSites live;
  r = grjit_liveness_compute(function, a, &live);
  if (r != GRJIT_OK) {
    return r;
  }
  GRJIT_Emit emit;
  GRJIT_MetaStorage meta;
  memset(&meta, 0, sizeof meta);
  GRJIT_Code * code = NULL;
  r = grjit_emit_function(function, a, limits.max_code_bytes, options->entry_hook,
      grcore_jit_layout()->request_word_offset, (uint32_t)frame, &live, &emit);
  if (r != GRJIT_OK) {
    goto done;
  }
  size_t code_bytes = grjit_asm_size(&emit.as);
  r = grjit_metadata_build(function, &live, emit.sites, emit.site_count,
      (uint32_t)frame, (uint32_t)code_bytes, a, &meta);
  if (r != GRJIT_OK) {
    goto done;
  }
  if (grcore_codemeta_validate(&meta.meta, code_bytes, NULL) != GRCORE_OK) {
    r = GRJIT_ERR_INTERNAL;
    goto done;
  }
  code = a->calloc_fn(a->ctx, 1, sizeof *code);
  if (code == NULL) {
    r = GRJIT_ERR_OOM;
    goto done;
  }
  r = grjit_memory_create(options->pages, grjit_asm_bytes(&emit.as), code_bytes,
      &code->mapping, &code->mapped_size);
  if (r != GRJIT_OK) {
    a->free_fn(a->ctx, code);
    code = NULL;
    goto done;
  }
  code->allocator = a;
  code->pages = options->pages;
  code->code_bytes = code_bytes;
  code->meta = meta;
  code->meta.meta.sites = code->meta.sites;
  memset(&meta, 0, sizeof meta);
  code->out_words = function->interp_slots + 1;
  code->param_count = function->param_count;
  code->request_offset = grcore_jit_layout()->request_word_offset;
  memcpy(&code->entry, &code->mapping, sizeof code->entry);
  *out_code = code;
  r = GRJIT_OK;
done:
  grjit_metadata_free(&meta);
  grjit_emit_free(&emit);
  grjit_liveness_free(&live);
  return r;
#endif
}
