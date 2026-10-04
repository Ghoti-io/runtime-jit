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
 * The AArch64 assembler (AD-9: new code, not seeded from any other
 * assembler). Encodings are the Arm Architecture Reference Manual's, checked
 * in the tests against the bytes `aarch64-linux-gnu-as` produces for the same
 * instruction.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "asm_internal.h"

#include <string.h>

void grjit_a64_init(
    GRJIT_A64Asm * a, const GRJIT_Allocator * allocator, size_t limit) {
  memset(a, 0, sizeof *a);
  a->allocator = allocator;
  a->limit = limit;
  a->status = GRJIT_A64_OK;
}

void grjit_a64_free(GRJIT_A64Asm * a) {
  const GRJIT_Allocator * al = a->allocator;
  if (al == NULL) {
    return;
  }
  al->free_fn(al->ctx, a->buffer);
  al->free_fn(al->ctx, a->label_at);
  al->free_fn(al->ctx, a->fixups);
  memset(a, 0, sizeof *a);
}

GRJIT_A64Status grjit_a64_status(const GRJIT_A64Asm * a) {
  return a->status;
}

const uint8_t * grjit_a64_bytes(const GRJIT_A64Asm * a) {
  return a->buffer;
}

size_t grjit_a64_size(const GRJIT_A64Asm * a) {
  return a->length;
}

void grjit_a64_set_long_branches(GRJIT_A64Asm * a, bool enabled) {
  a->long_branches = enabled;
}

static void fail(GRJIT_A64Asm * a, GRJIT_A64Status status) {
  if (a->status == GRJIT_A64_OK) {
    a->status = status;
  }
}

static bool reserve(GRJIT_A64Asm * a, size_t n) {
  if (a->status != GRJIT_A64_OK) {
    return false;
  }
  if (n > a->limit || a->length > a->limit - n) {
    fail(a, GRJIT_A64_LIMIT);
    return false;
  }
  if (a->length + n <= a->capacity) {
    return true;
  }
  size_t cap = a->capacity == 0 ? 256 : a->capacity;
  while (cap < a->length + n) {
    cap *= 2;
  }
  uint8_t * grown = a->allocator->realloc_fn(a->allocator->ctx, a->buffer, cap);
  if (grown == NULL) {
    fail(a, GRJIT_A64_OOM);
    return false;
  }
  a->buffer = grown;
  a->capacity = cap;
  return true;
}

void grjit_a64_word(GRJIT_A64Asm * a, uint32_t word) {
  if (reserve(a, 4)) {
    for (int i = 0; i < 4; i++) {
      a->buffer[a->length++] = (uint8_t)(word >> (8 * i));
    }
  }
}

/* ---- Labels and branches -------------------------------------------------- */

GRJIT_Label grjit_a64_label(GRJIT_A64Asm * a) {
  if (a->status != GRJIT_A64_OK) {
    return SIZE_MAX;
  }
  if (a->label_count == a->label_capacity) {
    size_t cap = a->label_capacity == 0 ? 32 : a->label_capacity * 2;
    size_t * grown =
        a->allocator->realloc_fn(a->allocator->ctx, a->label_at, cap * sizeof *grown);
    if (grown == NULL) {
      fail(a, GRJIT_A64_OOM);
      return SIZE_MAX;
    }
    a->label_at = grown;
    a->label_capacity = cap;
  }
  a->label_at[a->label_count] = SIZE_MAX;
  return a->label_count++;
}

void grjit_a64_bind(GRJIT_A64Asm * a, GRJIT_Label label) {
  if (a->status != GRJIT_A64_OK) {
    return;
  }
  if (label >= a->label_count) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  a->label_at[label] = a->length;
}

size_t grjit_a64_label_offset(const GRJIT_A64Asm * a, GRJIT_Label label) {
  return label < a->label_count ? a->label_at[label] : SIZE_MAX;
}

static void add_fixup(GRJIT_A64Asm * a, GRJIT_Label label, GRJIT_A64FixupKind kind) {
  if (a->status != GRJIT_A64_OK) {
    return;
  }
  if (a->fixup_count == a->fixup_capacity) {
    size_t cap = a->fixup_capacity == 0 ? 16 : a->fixup_capacity * 2;
    GRJIT_A64Fixup * grown =
        a->allocator->realloc_fn(a->allocator->ctx, a->fixups, cap * sizeof *grown);
    if (grown == NULL) {
      fail(a, GRJIT_A64_OOM);
      return;
    }
    a->fixups = grown;
    a->fixup_capacity = cap;
  }
  a->fixups[a->fixup_count].at = a->length;
  a->fixups[a->fixup_count].label = label;
  a->fixups[a->fixup_count].kind = kind;
  a->fixup_count++;
}

#define B_RANGE (INT64_C(1) << 25)   /* words, signed 26 bits */
#define COND_RANGE (INT64_C(1) << 18) /* words, signed 19 bits */

/* `b label`: always the one instruction. */
void grjit_a64_b(GRJIT_A64Asm * a, GRJIT_Label label) {
  if (a->status != GRJIT_A64_OK) {
    return;
  }
  if (label >= a->label_count) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  size_t target = a->label_at[label];
  if (target != SIZE_MAX) {
    int64_t words = ((int64_t)target - (int64_t)a->length) / 4;
    if (words < -B_RANGE || words >= B_RANGE) {
      fail(a, GRJIT_A64_LIMIT);
      return;
    }
    grjit_a64_word(a, 0x14000000u | ((uint32_t)words & 0x03FFFFFFu));
  } else {
    add_fixup(a, label, GRJIT_A64_FIXUP_B);
    grjit_a64_word(a, 0x14000000u);
  }
}

/* A conditional branch whose encoding is `base | (imm19 << 5)`, with
 * `inverse` the encoding of the opposite test (same operands, imm19 = 2: over
 * the `b` that follows it). */
static void cond_branch(
    GRJIT_A64Asm * a, uint32_t base, uint32_t inverse, GRJIT_Label label) {
  if (a->status != GRJIT_A64_OK) {
    return;
  }
  if (label >= a->label_count) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  size_t target = a->label_at[label];
  if (target != SIZE_MAX) {
    int64_t words = ((int64_t)target - (int64_t)a->length) / 4;
    if (words >= -COND_RANGE && words < COND_RANGE) {
      grjit_a64_word(a, base | (((uint32_t)words & 0x7FFFFu) << 5));
      return;
    }
  } else if (!a->long_branches) {
    add_fixup(a, label, GRJIT_A64_FIXUP_COND);
    grjit_a64_word(a, base);
    return;
  }
  /* The long form: the opposite test skips the `b`, which reaches anything the
   * cap allows. */
  grjit_a64_word(a, inverse | (2u << 5));
  grjit_a64_b(a, label);
}

void grjit_a64_bcond(GRJIT_A64Asm * a, GRJIT_A64Cond cond, GRJIT_Label label) {
  cond_branch(a, 0x54000000u | (uint32_t)cond,
      0x54000000u | ((uint32_t)cond ^ 1u), label);
}

void grjit_a64_cbz(GRJIT_A64Asm * a, GRJIT_A64Reg rt, GRJIT_Label label) {
  cond_branch(a, 0xB4000000u | (uint32_t)rt, 0xB5000000u | (uint32_t)rt, label);
}

void grjit_a64_cbnz(GRJIT_A64Asm * a, GRJIT_A64Reg rt, GRJIT_Label label) {
  cond_branch(a, 0xB5000000u | (uint32_t)rt, 0xB4000000u | (uint32_t)rt, label);
}

void grjit_a64_finish(GRJIT_A64Asm * a) {
  if (a->status != GRJIT_A64_OK) {
    return;
  }
  for (size_t i = 0; i < a->fixup_count; i++) {
    const GRJIT_A64Fixup * f = &a->fixups[i];
    size_t target = a->label_at[f->label];
    if (target == SIZE_MAX) {
      fail(a, GRJIT_A64_BAD);
      return;
    }
    int64_t words = ((int64_t)target - (int64_t)f->at) / 4;
    uint32_t insn = 0;
    for (int k = 0; k < 4; k++) {
      insn |= (uint32_t)a->buffer[f->at + (size_t)k] << (8 * k);
    }
    if (f->kind == GRJIT_A64_FIXUP_B) {
      if (words < -B_RANGE || words >= B_RANGE) {
        fail(a, GRJIT_A64_LIMIT);
        return;
      }
      insn |= (uint32_t)words & 0x03FFFFFFu;
    } else {
      if (words < -COND_RANGE || words >= COND_RANGE) {
        fail(a, GRJIT_A64_FAR);
        return;
      }
      insn |= ((uint32_t)words & 0x7FFFFu) << 5;
    }
    for (int k = 0; k < 4; k++) {
      a->buffer[f->at + (size_t)k] = (uint8_t)(insn >> (8 * k));
    }
  }
  a->fixup_count = 0;
}

/* ---- Moves and immediates --------------------------------------------------- */

void grjit_a64_movz(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint16_t imm, unsigned hw) {
  grjit_a64_word(a, 0xD2800000u | (hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}

void grjit_a64_movn(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint16_t imm, unsigned hw) {
  grjit_a64_word(a, 0x92800000u | (hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}

void grjit_a64_movk(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint16_t imm, unsigned hw) {
  grjit_a64_word(a, 0xF2800000u | (hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}

/* Counts the 16-bit pieces that are not all zeros and not all ones. */
static void piece_counts(uint64_t imm, unsigned * nonzero, unsigned * not_ones) {
  *nonzero = 0;
  *not_ones = 0;
  for (unsigned i = 0; i < 4; i++) {
    uint16_t piece = (uint16_t)(imm >> (16 * i));
    *nonzero += piece != 0;
    *not_ones += piece != 0xFFFF;
  }
}

unsigned grjit_a64_mov_ri_count(uint64_t imm) {
  unsigned nonzero, not_ones;
  piece_counts(imm, &nonzero, &not_ones);
  unsigned n = not_ones < nonzero ? not_ones : nonzero;
  return n == 0 ? 1 : n;
}

void grjit_a64_mov_ri(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint64_t imm) {
  unsigned nonzero, not_ones;
  piece_counts(imm, &nonzero, &not_ones);
  if (not_ones < nonzero) {
    /* Mostly ones: start from `movn`, which sets every other piece to ones. */
    bool first = true;
    for (unsigned i = 0; i < 4; i++) {
      uint16_t piece = (uint16_t)(imm >> (16 * i));
      if (piece == 0xFFFF) {
        continue;
      }
      if (first) {
        grjit_a64_movn(a, rd, (uint16_t)~piece, i);
        first = false;
      } else {
        grjit_a64_movk(a, rd, piece, i);
      }
    }
    if (first) {
      grjit_a64_movn(a, rd, 0, 0); /* all ones */
    }
    return;
  }
  bool first = true;
  for (unsigned i = 0; i < 4; i++) {
    uint16_t piece = (uint16_t)(imm >> (16 * i));
    if (piece == 0) {
      continue;
    }
    if (first) {
      grjit_a64_movz(a, rd, piece, i);
      first = false;
    } else {
      grjit_a64_movk(a, rd, piece, i);
    }
  }
  if (first) {
    grjit_a64_movz(a, rd, 0, 0); /* zero */
  }
}

void grjit_a64_mov_rr(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm) {
  grjit_a64_word(a, 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd);
}

void grjit_a64_mov32_rr(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm) {
  grjit_a64_word(a, 0x2A0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd);
}

void grjit_a64_mov_sp(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn) {
  grjit_a64_add_imm(a, rd, rn, 0, false);
}

void grjit_a64_add_imm(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn,
    uint32_t imm12, bool shift12) {
  if (imm12 > 0xFFFu) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  grjit_a64_word(a, 0x91000000u | (shift12 ? 1u << 22 : 0u) | (imm12 << 10) |
      ((uint32_t)rn << 5) | (uint32_t)rd);
}

void grjit_a64_sub_imm(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn,
    uint32_t imm12, bool shift12) {
  if (imm12 > 0xFFFu) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  grjit_a64_word(a, 0xD1000000u | (shift12 ? 1u << 22 : 0u) | (imm12 << 10) |
      ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* An `sp` adjustment is the high part (a 12-bit immediate shifted by 12) and
 * the low part, each a multiple of 16 when `bytes` is, so `sp` stays aligned
 * between the two instructions. */
void grjit_a64_sub_sp(GRJIT_A64Asm * a, uint32_t bytes) {
  if (bytes >= (UINT32_C(1) << 24)) {
    fail(a, GRJIT_A64_LIMIT);
    return;
  }
  if (bytes >> 12) {
    grjit_a64_sub_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, bytes >> 12, true);
  }
  if (bytes & 0xFFFu) {
    grjit_a64_sub_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, bytes & 0xFFFu, false);
  }
}

void grjit_a64_add_sp(GRJIT_A64Asm * a, uint32_t bytes) {
  if (bytes >= (UINT32_C(1) << 24)) {
    fail(a, GRJIT_A64_LIMIT);
    return;
  }
  if (bytes >> 12) {
    grjit_a64_add_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, bytes >> 12, true);
  }
  if (bytes & 0xFFFu) {
    grjit_a64_add_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, bytes & 0xFFFu, false);
  }
}

/* ---- Arithmetic --------------------------------------------------------------- */

void grjit_a64_alu(GRJIT_A64Asm * a, GRJIT_A64Alu op, GRJIT_A64Reg rd,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm) {
  uint32_t base;
  switch (op) {
    case GRJIT_A64_ADD: base = 0x8B000000u; break;
    case GRJIT_A64_SUB: base = 0xCB000000u; break;
    case GRJIT_A64_AND: base = 0x8A000000u; break;
    case GRJIT_A64_ORR: base = 0xAA000000u; break;
    case GRJIT_A64_EOR: base = 0xCA000000u; break;
    case GRJIT_A64_MUL: base = 0x9B007C00u; break; /* madd xd, xn, xm, xzr */
    case GRJIT_A64_LSLV: base = 0x9AC02000u; break;
    case GRJIT_A64_LSRV: base = 0x9AC02400u; break;
    case GRJIT_A64_ASRV: base = 0x9AC02800u; break;
    default:
      fail(a, GRJIT_A64_BAD);
      return;
  }
  grjit_a64_word(a, base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

void grjit_a64_neg(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm) {
  grjit_a64_alu(a, GRJIT_A64_SUB, rd, GRJIT_A64_XZR, rm);
}

void grjit_a64_mvn(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm) {
  /* orn xd, xzr, xm */
  grjit_a64_word(a, 0xAA2003E0u | ((uint32_t)rm << 16) | (uint32_t)rd);
}

void grjit_a64_cmp(GRJIT_A64Asm * a, GRJIT_A64Reg rn, GRJIT_A64Reg rm) {
  /* subs xzr, xn, xm */
  grjit_a64_word(a, 0xEB00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5));
}

void grjit_a64_cset(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Cond cond) {
  /* csinc wd, wzr, wzr, !cond */
  grjit_a64_word(a, 0x1A9F07E0u | (((uint32_t)cond ^ 1u) << 12) | (uint32_t)rd);
}

/* ---- Loads and stores ----------------------------------------------------------- */

/* The unscaled-immediate encoding of each kind; the other forms derive from it. */
static uint32_t load_base(GRJIT_A64Mem kind) {
  switch (kind) {
    case GRJIT_A64_MEM_U8: return 0x38400000u;
    case GRJIT_A64_MEM_U16: return 0x78400000u;
    case GRJIT_A64_MEM_U32: return 0xB8400000u;
    case GRJIT_A64_MEM_X64: return 0xF8400000u;
    case GRJIT_A64_MEM_S8: return 0x38800000u;
    case GRJIT_A64_MEM_S16: return 0x78800000u;
    case GRJIT_A64_MEM_S32: return 0xB8800000u;
  }
  return 0;
}

static uint32_t store_base(GRJIT_A64Mem kind) {
  switch (kind) {
    case GRJIT_A64_MEM_U8: return 0x38000000u;
    case GRJIT_A64_MEM_U16: return 0x78000000u;
    case GRJIT_A64_MEM_U32: return 0xB8000000u;
    case GRJIT_A64_MEM_X64: return 0xF8000000u;
    default: return 0;
  }
}

static unsigned mem_shift(GRJIT_A64Mem kind) {
  switch (kind) {
    case GRJIT_A64_MEM_U8: case GRJIT_A64_MEM_S8: return 0;
    case GRJIT_A64_MEM_U16: case GRJIT_A64_MEM_S16: return 1;
    case GRJIT_A64_MEM_U32: case GRJIT_A64_MEM_S32: return 2;
    case GRJIT_A64_MEM_X64: return 3;
  }
  return 0;
}

static void mem_unscaled(GRJIT_A64Asm * a, uint32_t base, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, int32_t imm9) {
  if (base == 0 || imm9 < -256 || imm9 > 255) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  grjit_a64_word(a, base | (((uint32_t)imm9 & 0x1FFu) << 12) |
      ((uint32_t)rn << 5) | (uint32_t)rt);
}

static void mem_scaled(GRJIT_A64Asm * a, uint32_t base, unsigned shift,
    GRJIT_A64Reg rt, GRJIT_A64Reg rn, uint32_t uimm) {
  if (base == 0 || (uimm & ((1u << shift) - 1u)) != 0 ||
      (uimm >> shift) > 0xFFFu) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  /* The scaled form is the unscaled one with bit 24 set. */
  grjit_a64_word(a, (base & ~0x00200000u) | 0x01000000u |
      ((uimm >> shift) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

static void mem_reg(GRJIT_A64Asm * a, uint32_t base, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm) {
  if (base == 0) {
    fail(a, GRJIT_A64_BAD);
    return;
  }
  /* LDR/STR (register), option UXTX, no shift: bit 21, bits 11:10 = 10. */
  grjit_a64_word(a, base | 0x00200000u | (3u << 13) | 0x800u |
      ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

void grjit_a64_load_unscaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind,
    GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t imm9) {
  mem_unscaled(a, load_base(kind), rt, rn, imm9);
}

void grjit_a64_load_scaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, uint32_t uimm) {
  mem_scaled(a, load_base(kind), mem_shift(kind), rt, rn, uimm);
}

void grjit_a64_load_reg(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm) {
  mem_reg(a, load_base(kind), rt, rn, rm);
}

void grjit_a64_store_unscaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind,
    GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t imm9) {
  mem_unscaled(a, store_base(kind), rt, rn, imm9);
}

void grjit_a64_store_scaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, uint32_t uimm) {
  mem_scaled(a, store_base(kind), mem_shift(kind), rt, rn, uimm);
}

void grjit_a64_store_reg(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm) {
  mem_reg(a, store_base(kind), rt, rn, rm);
}

/* Picks the shortest form that holds `disp`; else puts it in x17. */
static void mem_any(GRJIT_A64Asm * a, uint32_t base, unsigned shift,
    GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t disp) {
  uint32_t mask = (1u << shift) - 1u;
  if (disp >= 0 && ((uint32_t)disp & mask) == 0 && ((uint32_t)disp >> shift) <= 0xFFFu) {
    mem_scaled(a, base, shift, rt, rn, (uint32_t)disp);
  } else if (disp >= -256 && disp <= 255) {
    mem_unscaled(a, base, rt, rn, disp);
  } else {
    grjit_a64_mov_ri(a, GRJIT_A64_X17, (uint64_t)(int64_t)disp);
    mem_reg(a, base, rt, rn, GRJIT_A64_X17);
  }
}

void grjit_a64_load(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, int32_t disp) {
  mem_any(a, load_base(kind), mem_shift(kind), rt, rn, disp);
}

void grjit_a64_store(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, int32_t disp) {
  mem_any(a, store_base(kind), mem_shift(kind), rt, rn, disp);
}

void grjit_a64_push_frame_record(GRJIT_A64Asm * a) {
  grjit_a64_word(a, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
}

void grjit_a64_pop_frame_record(GRJIT_A64Asm * a) {
  grjit_a64_word(a, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
}

void grjit_a64_ret(GRJIT_A64Asm * a) {
  grjit_a64_word(a, 0xD65F03C0u);
}

void grjit_a64_blr(GRJIT_A64Asm * a, GRJIT_A64Reg rn) {
  grjit_a64_word(a, 0xD63F0000u | ((uint32_t)rn << 5));
}

void grjit_a64_brk(GRJIT_A64Asm * a, uint16_t imm) {
  grjit_a64_word(a, 0xD4200000u | ((uint32_t)imm << 5));
}
