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
 * Compiling a function, and the compiled-code handle. Machine code is made
 * for Linux x86-64, Linux arm64 and Windows x86-64, chosen by the compiler's target when the
 * library is built, never at run time; elsewhere ::grjit_backend_available is
 * false and ::grjit_compile says so. Both instruction sets can be *emitted* on
 * any host (::grjit_emit_for), which is how the arm64 backend's encodings,
 * frame, offsets and branch ranges are tested on an x86-64 machine; only the
 * native one can be mapped and run.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/verify.h>

#include "code_internal.h"
#include "../x86_64/emit_internal.h"
#include "../arm64/emit_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/layout.h>

#include <assert.h>
#include <string.h>

#if defined(__x86_64__) && defined(__linux__)
#define GRJIT_HAVE_BACKEND 1
#define GRJIT_NATIVE GRJIT_ARCH_X86_64
#elif defined(_WIN64) && defined(__x86_64__)
#define GRJIT_HAVE_BACKEND 1
#define GRJIT_NATIVE GRJIT_ARCH_X86_64_WIN64
#elif defined(__aarch64__) && defined(__linux__)
#define GRJIT_HAVE_BACKEND 1
#define GRJIT_NATIVE GRJIT_ARCH_ARM64
#else
#define GRJIT_HAVE_BACKEND 0
#define GRJIT_NATIVE GRJIT_ARCH_X86_64
#endif

GRJIT_Arch grjit_native_arch(void) {
  return GRJIT_NATIVE;
}

bool grjit_backend_available(void) {
  return GRJIT_HAVE_BACKEND != 0;
}

void grjit_code_destroy(GRJIT_Code * code) {
  if (code == NULL) {
    return;
  }
  const GRJIT_Allocator * a = code->allocator;
  grjit_memory_destroy(
      code->pages, code->mapping, code->mapped_size, code->unwind_table);
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

/* The entry is generated code, which carries no type signature in front of it.
 * clang's -fsanitize=function reads one (the 8 bytes before the callee) at every
 * indirect call, and the first byte of the code is the first byte of its
 * mapping, so the read faults. The call is checked by construction instead:
 * the entry point is ours and its type is the one declared here. GCC has no
 * such check. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
uint32_t grjit_code_call(const GRJIT_Code * code, void * context,
    const uint64_t * args, uint64_t * out) {
  /* Code is compiled against one build's layout of the context; a core that
   * moved the request word makes every poll in it read the wrong word, so the
   * call is refused before any of it runs. The same refusal answers a NULL
   * code. It is the exit the poll helper uses, with ::GRCORE_ERR_INVALID in
   * out[0] (when there is somewhere to put it), and costs one compare on a
   * call that has just loaded the entry point. */
  if (code == NULL ||
      code->request_offset != grcore_jit_layout()->request_word_offset) {
    if (out != NULL) {
      out[0] = (uint64_t)GRCORE_ERR_INVALID;
    }
    return GRJIT_EXIT_REFUSED;
  }
  return code->entry(context, args, out);
}

#if GRJIT_HAVE_BACKEND
static GRJIT_Result verify_for_compile(
    const GRJIT_Function * f, const GRJIT_Limits * limits) {
  GRJIT_Result r = grjit_function_verify(f, limits, NULL, 0);
  if (r == GRJIT_ERR_LIMIT || r == GRJIT_ERR_OOM) {
    return r;
  }
  return r == GRJIT_OK ? GRJIT_OK : GRJIT_ERR_INVALID;
}
#endif

/* Calls between compiled functions (AD-28) have no emitter yet. A function that
 * has the new operations, or is callable, is refused with
 * GRJIT_ERR_UNSUPPORTED and nothing of it is emitted, so the bytes of every
 * other function are exactly what they were. */
static bool grjit_emit_supports(GRJIT_Arch arch, const GRJIT_Function * f) {
  (void)arch;
  if (f->callable) {
    return false;
  }
  for (size_t b = 0; b < f->block_count; b++) {
    for (size_t i = 0; i < f->blocks[b].count; i++) {
      GRJIT_OpKind k = f->blocks[b].ops[i].kind;
      if (k == GRJIT_OP_CALL_SLOT || k == GRJIT_OP_CALL_PTR) {
        return false;
      }
    }
  }
  return true;
}

GRJIT_Result grjit_emit_for(GRJIT_Arch arch, const GRJIT_Function * function,
    const GRJIT_Allocator * a, const GRJIT_Limits * limits_in,
    GRJIT_EntryHook hook, uint32_t request_offset, GRJIT_Emitted * out) {
  GRJIT_Limits limits;
  grjit_limits_resolve(limits_in, &limits);
  memset(out, 0, sizeof *out);
  if (!grjit_emit_supports(arch, function)) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  /* Three fixed slots and one per register, rounded to keep the stack pointer
   * 16-aligned. */
  size_t slots = function->vreg_count + GRJIT_FIXED_SLOTS;
  size_t frame = (slots * 8 + 15) / 16 * 16;
  if (frame > limits.max_frame_bytes) {
    return GRJIT_ERR_LIMIT;
  }
  GRJIT_LiveSites live;
  GRJIT_Result r = grjit_liveness_compute(function, a, limits.max_site_entries, &live);
  if (r != GRJIT_OK) {
    return r;
  }
  const uint8_t * bytes = NULL;
  size_t code_bytes = 0;
  const GRJIT_SiteRec * recs = NULL;
  size_t rec_count = 0;
  GRJIT_Emit x86;
  GRJIT_A64Emit a64;
  memset(&x86, 0, sizeof x86);
  memset(&a64, 0, sizeof a64);
  if (arch == GRJIT_ARCH_ARM64) {
    r = grjit_a64_emit_function(function, a, limits.max_code_bytes, hook,
        request_offset, (uint32_t)frame, &live, &a64);
    bytes = grjit_a64_bytes(&a64.as);
    code_bytes = grjit_a64_size(&a64.as);
    recs = a64.c.sites;
    rec_count = a64.c.site_count;
  } else {
    r = grjit_emit_function(function, a, limits.max_code_bytes, hook,
        request_offset, (uint32_t)frame, arch == GRJIT_ARCH_X86_64_WIN64,
        &live, &x86);
    out->prologue = x86.prologue;
    out->regs_used = grjit_asm_regs_used(&x86.as);
    bytes = grjit_asm_bytes(&x86.as);
    code_bytes = grjit_asm_size(&x86.as);
    recs = x86.c.sites;
    rec_count = x86.c.site_count;
  }
  if (r != GRJIT_OK) {
    goto done;
  }
  r = grjit_metadata_build(function, &live, recs, rec_count, (uint32_t)frame,
      (uint32_t)code_bytes, a, &out->meta);
  if (r != GRJIT_OK) {
    goto done;
  }
  if (grcore_codemeta_validate(&out->meta.meta, code_bytes, NULL) != GRCORE_OK) {
    r = GRJIT_ERR_INTERNAL;
    goto done;
  }
  out->bytes = a->malloc_fn(a->ctx, code_bytes);
  if (out->bytes == NULL) {
    r = GRJIT_ERR_OOM;
    goto done;
  }
  memcpy(out->bytes, bytes, code_bytes);
  out->size = code_bytes;
  out->allocator = a;
  out->meta.meta.sites = out->meta.sites;
  r = GRJIT_OK;
done:
  if (r != GRJIT_OK) {
    grjit_metadata_free(&out->meta);
    memset(out, 0, sizeof *out);
  }
  grjit_emit_free(&x86);
  grjit_a64_emit_free(&a64);
  grjit_liveness_free(&live);
  return r;
}

void grjit_emitted_free(GRJIT_Emitted * emitted) {
  if (emitted->allocator == NULL) {
    return;
  }
  emitted->allocator->free_fn(emitted->allocator->ctx, emitted->bytes);
  grjit_metadata_free(&emitted->meta);
  memset(emitted, 0, sizeof *emitted);
}

GRJIT_Result grjit_compile(const GRJIT_CompileOptions * options,
    const GRJIT_Function * function, GRJIT_Code ** out_code) {
  if (options == NULL || function == NULL || out_code == NULL ||
      !grcore_page_provider_valid(options->pages)) {
    return GRJIT_ERR_INVALID;
  }
#if !GRJIT_HAVE_BACKEND
  return GRJIT_ERR_UNSUPPORTED;
#else
  const GRJIT_Allocator * a = grjit_allocator_or_default(options->allocator);
  GRJIT_Result r = verify_for_compile(function, options->limits);
  if (r != GRJIT_OK) {
    return r;
  }
  /* A provider whose size ends before `protect` has none. */
  if (!GRCORE_PAGE_PROVIDER_HAS(options->pages, protect) ||
      options->pages->protect == NULL) {
    return GRJIT_ERR_UNSUPPORTED;
  }
  GRJIT_Emitted emitted;
  r = grjit_emit_for(GRJIT_NATIVE, function, a, options->limits,
      options->entry_hook, grcore_jit_layout()->request_word_offset, &emitted);
  if (r != GRJIT_OK) {
    return r;
  }
  GRJIT_Code * code = a->calloc_fn(a->ctx, 1, sizeof *code);
  if (code == NULL) {
    grjit_emitted_free(&emitted);
    return GRJIT_ERR_OOM;
  }
  r = grjit_memory_create(options->pages, GRJIT_NATIVE, emitted.bytes,
      emitted.size, &emitted.prologue, &code->mapping, &code->mapped_size,
      &code->unwind_table);
  if (r != GRJIT_OK) {
    a->free_fn(a->ctx, code);
    grjit_emitted_free(&emitted);
    return r;
  }
  code->allocator = a;
  code->pages = options->pages;
  code->code_bytes = emitted.size;
  code->meta = emitted.meta;
  code->meta.meta.sites = code->meta.sites;
  /* The metadata now belongs to the code; the bytes were copied into the
   * mapping and are ours to free. */
  memset(&emitted.meta, 0, sizeof emitted.meta);
  grjit_emitted_free(&emitted);
  code->out_words = function->interp_slots + 1;
  code->param_count = function->param_count;
  code->request_offset = grcore_jit_layout()->request_word_offset;
  memcpy(&code->entry, &code->mapping, sizeof code->entry);
  *out_code = code;
  return GRJIT_OK;
#endif
}
