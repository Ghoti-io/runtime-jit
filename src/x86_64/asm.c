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
 * The x86-64 assembler (AD-9: new code, not ctang's `binary.h`). Encodings
 * are the Intel SDM's, checked in the tests against byte sequences written
 * out there and, where `objdump` exists, against its disassembly.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "asm_internal.h"

#include <string.h>

void grjit_asm_init(
    GRJIT_Asm * a, const GRJIT_Allocator * allocator, size_t limit) {
  memset(a, 0, sizeof *a);
  a->allocator = allocator;
  a->limit = limit;
  a->status = GRJIT_ASM_OK;
}

void grjit_asm_free(GRJIT_Asm * a) {
  const GRJIT_Allocator * al = a->allocator;
  if (al == NULL) {
    return;
  }
  al->free_fn(al->ctx, a->buffer);
  al->free_fn(al->ctx, a->label_at);
  al->free_fn(al->ctx, a->fixups);
  memset(a, 0, sizeof *a);
}

GRJIT_AsmStatus grjit_asm_status(const GRJIT_Asm * a) {
  return a->status;
}

const uint8_t * grjit_asm_bytes(const GRJIT_Asm * a) {
  return a->buffer;
}

size_t grjit_asm_size(const GRJIT_Asm * a) {
  return a->length;
}

uint32_t grjit_asm_regs_used(const GRJIT_Asm * a) {
  return a->regs_used;
}

uint32_t grjit_asm_xmm_used(const GRJIT_Asm * a) {
  return a->xmm_used;
}

/* Records that an SSE instruction names `xmm`x, in a mask of its own: the callee-saved `xmm6` to `xmm15` of
 * Win64 are what a structural test reads it for. */
static void note_xmm(GRJIT_Asm * a, GRJIT_Xmm x) {
  a->xmm_used |= (uint32_t)1 << ((unsigned)x & 15u);
}

/* Records that an instruction names register `r`. Called for the operands that
 * are registers only, never for the opcode extension a ModRM reg field can
 * carry. */
static void note(GRJIT_Asm * a, int r) {
  a->regs_used |= (uint32_t)1 << (r & 15);
}

static void fail(GRJIT_Asm * a, GRJIT_AsmStatus status) {
  if (a->status == GRJIT_ASM_OK) {
    a->status = status;
  }
}

static bool reserve(GRJIT_Asm * a, size_t n) {
  if (a->status != GRJIT_ASM_OK) {
    return false;
  }
  if (n > a->limit || a->length > a->limit - n) {
    fail(a, GRJIT_ASM_LIMIT);
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
    fail(a, GRJIT_ASM_OOM);
    return false;
  }
  a->buffer = grown;
  a->capacity = cap;
  return true;
}

static void byte(GRJIT_Asm * a, uint8_t b) {
  if (reserve(a, 1)) {
    a->buffer[a->length++] = b;
  }
}

static void imm32(GRJIT_Asm * a, uint32_t v) {
  if (reserve(a, 4)) {
    for (int i = 0; i < 4; i++) {
      a->buffer[a->length++] = (uint8_t)(v >> (8 * i));
    }
  }
}

static void imm64(GRJIT_Asm * a, uint64_t v) {
  if (reserve(a, 8)) {
    for (int i = 0; i < 8; i++) {
      a->buffer[a->length++] = (uint8_t)(v >> (8 * i));
    }
  }
}

void grjit_asm_raw(GRJIT_Asm * a, const void * bytes, size_t count) {
  if (reserve(a, count)) {
    memcpy(a->buffer + a->length, bytes, count);
    a->length += count;
  }
}

/* A REX prefix: W, then the extensions of the ModRM reg and rm fields. It is
 * emitted when any bit is set, or when `force` says an 8-bit register that
 * would otherwise name ah..bh must name spl..dil. */
static void rex(GRJIT_Asm * a, bool w, int reg, int rm, bool force) {
  uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | (reg >= 8 ? 4 : 0) | (rm >= 8 ? 1 : 0));
  if (r != 0x40 || force) {
    byte(a, r);
  }
}

static void modrm_rr(GRJIT_Asm * a, int reg, int rm) {
  byte(a, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* ModRM (and SIB, and displacement) for `[base + disp]`. rsp and r12 as a
 * base need a SIB byte; rbp and r13 cannot use mod 00. */
static void modrm_mem(GRJIT_Asm * a, int reg, GRJIT_Reg base, int32_t disp) {
  int rm = base & 7;
  int mod;
  if (disp == 0 && rm != 5) {
    mod = 0;
  } else if (disp >= -128 && disp <= 127) {
    mod = 1;
  } else {
    mod = 2;
  }
  byte(a, (uint8_t)((mod << 6) | ((reg & 7) << 3) | rm));
  if (rm == 4) {
    byte(a, 0x24);
  }
  if (mod == 1) {
    byte(a, (uint8_t)(int8_t)disp);
  } else if (mod == 2) {
    imm32(a, (uint32_t)disp);
  }
}

/* One instruction with a memory operand: optional 0x66 prefix, REX, one or
 * two opcode bytes, ModRM. */
static void inst_mem(GRJIT_Asm * a, bool p66, bool w, uint8_t op1, int op2,
    int reg, GRJIT_Reg base, int32_t disp, bool force_rex) {
  note(a, reg);
  note(a, base);
  if (p66) {
    byte(a, 0x66);
  }
  rex(a, w, reg, base, force_rex);
  byte(a, op1);
  if (op2 >= 0) {
    byte(a, (uint8_t)op2);
  }
  modrm_mem(a, reg, base, disp);
}

void grjit_asm_mov_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, true, src, dst, false);
  byte(a, 0x89);
  modrm_rr(a, src, dst);
}

void grjit_asm_mov32_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, false, src, dst, false);
  byte(a, 0x89);
  modrm_rr(a, src, dst);
}

void grjit_asm_mov_ri(GRJIT_Asm * a, GRJIT_Reg dst, uint64_t imm) {
  note(a, dst);
  if (imm <= UINT32_MAX) {
    rex(a, false, 0, dst, false);
    byte(a, (uint8_t)(0xB8 + (dst & 7)));
    imm32(a, (uint32_t)imm);
  } else if ((int64_t)imm >= INT32_MIN && (int64_t)imm <= INT32_MAX) {
    rex(a, true, 0, dst, false);
    byte(a, 0xC7);
    modrm_rr(a, 0, dst);
    imm32(a, (uint32_t)imm);
  } else {
    grjit_asm_mov_ri64(a, dst, imm);
  }
}

void grjit_asm_mov_ri64(GRJIT_Asm * a, GRJIT_Reg dst, uint64_t imm) {
  note(a, dst);
  rex(a, true, 0, dst, false);
  byte(a, (uint8_t)(0xB8 + (dst & 7)));
  imm64(a, imm);
}

void grjit_asm_load64(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, true, 0x8B, -1, dst, base, disp, false);
}

void grjit_asm_store64(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src) {
  inst_mem(a, false, true, 0x89, -1, src, base, disp, false);
}

void grjit_asm_lea(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, true, 0x8D, -1, dst, base, disp, false);
}

void grjit_asm_probe(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, false, 0x84, -1, GRJIT_RAX, base, disp, false);
}

void grjit_asm_load8u(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, false, 0x0F, 0xB6, dst, base, disp, false);
}

void grjit_asm_load16u(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, false, 0x0F, 0xB7, dst, base, disp, false);
}

void grjit_asm_load32u(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, false, 0x8B, -1, dst, base, disp, false);
}

void grjit_asm_load8s(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, true, 0x0F, 0xBE, dst, base, disp, false);
}

void grjit_asm_load16s(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, true, 0x0F, 0xBF, dst, base, disp, false);
}

void grjit_asm_load32s(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, true, 0x63, -1, dst, base, disp, false);
}

void grjit_asm_store8(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src) {
  inst_mem(a, false, false, 0x88, -1, src, base, disp, src >= 4 && src < 8);
}

void grjit_asm_store16(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src) {
  inst_mem(a, true, false, 0x89, -1, src, base, disp, false);
}

void grjit_asm_store32(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src) {
  inst_mem(a, false, false, 0x89, -1, src, base, disp, false);
}

void grjit_asm_alu_rr(GRJIT_Asm * a, GRJIT_AluOp op, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, true, src, dst, false);
  byte(a, (uint8_t)op);
  modrm_rr(a, src, dst);
}

void grjit_asm_imul_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, true, dst, src, false);
  byte(a, 0x0F);
  byte(a, 0xAF);
  modrm_rr(a, dst, src);
}

static void group3(GRJIT_Asm * a, uint8_t opcode, int ext, GRJIT_Reg r) {
  note(a, r);
  rex(a, true, 0, r, false);
  byte(a, opcode);
  modrm_rr(a, ext, r);
}

void grjit_asm_neg(GRJIT_Asm * a, GRJIT_Reg r) {
  group3(a, 0xF7, 3, r);
}

void grjit_asm_not(GRJIT_Asm * a, GRJIT_Reg r) {
  group3(a, 0xF7, 2, r);
}

void grjit_asm_shl_cl(GRJIT_Asm * a, GRJIT_Reg r) {
  group3(a, 0xD3, 4, r);
}

void grjit_asm_shr_cl(GRJIT_Asm * a, GRJIT_Reg r) {
  group3(a, 0xD3, 5, r);
}

void grjit_asm_sar_cl(GRJIT_Asm * a, GRJIT_Reg r) {
  group3(a, 0xD3, 7, r);
}

static void group3w(GRJIT_Asm * a, bool w, uint8_t opcode, int ext, GRJIT_Reg r) {
  note(a, r);
  rex(a, w, 0, r, false);
  byte(a, opcode);
  modrm_rr(a, ext, r);
}

void grjit_asm_alu32_rr(GRJIT_Asm * a, GRJIT_AluOp op, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, false, src, dst, false);
  byte(a, (uint8_t)op);
  modrm_rr(a, src, dst);
}

void grjit_asm_imul32_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, false, dst, src, false);
  byte(a, 0x0F);
  byte(a, 0xAF);
  modrm_rr(a, dst, src);
}

void grjit_asm_neg32(GRJIT_Asm * a, GRJIT_Reg r) {
  group3w(a, false, 0xF7, 3, r);
}

void grjit_asm_not32(GRJIT_Asm * a, GRJIT_Reg r) {
  group3w(a, false, 0xF7, 2, r);
}

void grjit_asm_shift_cl(GRJIT_Asm * a, GRJIT_ShiftOp op, GRJIT_Reg r, bool wide) {
  group3w(a, wide, 0xD3, (int)op, r);
}

void grjit_asm_shift_ri(GRJIT_Asm * a, GRJIT_ShiftOp op, GRJIT_Reg r, uint8_t count, bool wide) {
  group3w(a, wide, 0xC1, (int)op, r);
  byte(a, count);
}

void grjit_asm_movsx(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, unsigned from_bits, bool wide) {
  note(a, dst);
  note(a, src);
  if (from_bits == 32) {
    /* movsxd r64, r/m32; there is no 32-to-32 form. */
    rex(a, true, dst, src, false);
    byte(a, 0x63);
  } else {
    rex(a, wide, dst, src, from_bits == 8 && src >= 4 && src < 8);
    byte(a, 0x0F);
    byte(a, from_bits == 8 ? 0xBE : 0xBF);
  }
  modrm_rr(a, dst, src);
}

void grjit_asm_bsr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
  note(a, dst);
  note(a, src);
  rex(a, wide, dst, src, false);
  byte(a, 0x0F);
  byte(a, 0xBD);
  modrm_rr(a, dst, src);
}

void grjit_asm_bsf(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
  note(a, dst);
  note(a, src);
  rex(a, wide, dst, src, false);
  byte(a, 0x0F);
  byte(a, 0xBC);
  modrm_rr(a, dst, src);
}

/* `lzcnt`, `tzcnt` and `popcnt` are `F3 [REX] 0F BD / BC / B8 /r`. Without the F3 the first two are
 * `bsr` and `bsf` and the third is a different instruction, and a CPU that lacks LZCNT or BMI1 runs
 * the F3 forms as the bit scans instead of faulting: so each is a function of its own, with its own
 * opcode and its mandatory prefix inside it, and no caller supplies a prefix. */
static void f3_count(GRJIT_Asm * a, uint8_t opcode, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
  note(a, dst);
  note(a, src);
  byte(a, 0xF3);
  rex(a, wide, dst, src, false);
  byte(a, 0x0F);
  byte(a, opcode);
  modrm_rr(a, dst, src);
}

void grjit_asm_lzcnt(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 64
  /* Planted defect 64 (tests only): `lzcnt` emitted without its F3 prefix, which is `bsr`. */
  grjit_asm_bsr(a, dst, src, wide);
#else
  f3_count(a, 0xBD, dst, src, wide);
#endif
}

void grjit_asm_tzcnt(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
  f3_count(a, 0xBC, dst, src, wide);
}

void grjit_asm_popcnt(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
  f3_count(a, 0xB8, dst, src, wide);
}

void grjit_asm_cmov(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Reg dst, GRJIT_Reg src, bool wide) {
  note(a, dst);
  note(a, src);
  rex(a, wide, dst, src, false);
  byte(a, 0x0F);
  byte(a, (uint8_t)(0x40 + cond));
  modrm_rr(a, dst, src);
}

void grjit_asm_cdq(GRJIT_Asm * a) {
  note(a, GRJIT_RAX);
  note(a, GRJIT_RDX);
  byte(a, 0x99);
}

void grjit_asm_cqo(GRJIT_Asm * a) {
  note(a, GRJIT_RAX);
  note(a, GRJIT_RDX);
  byte(a, 0x48);
  byte(a, 0x99);
}

void grjit_asm_div(GRJIT_Asm * a, GRJIT_Reg r, bool is_signed, bool wide) {
  note(a, GRJIT_RAX);
  note(a, GRJIT_RDX);
  group3w(a, wide, 0xF7, is_signed ? 7 : 6, r);
}

void grjit_asm_cmp_ri32(GRJIT_Asm * a, GRJIT_Reg r, int8_t imm) {
  note(a, r);
  rex(a, false, 0, r, false);
  byte(a, 0x83);
  modrm_rr(a, 7, r);
  byte(a, (uint8_t)imm);
}

void grjit_asm_test_rr(GRJIT_Asm * a, GRJIT_Reg x, GRJIT_Reg y) {
  note(a, x);
  note(a, y);
  rex(a, true, y, x, false);
  byte(a, 0x85);
  modrm_rr(a, y, x);
}

void grjit_asm_setcc(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Reg r) {
  note(a, r);
  rex(a, false, 0, r, r >= 4 && r < 8);
  byte(a, 0x0F);
  byte(a, (uint8_t)(0x90 + cond));
  modrm_rr(a, 0, r);
}

void grjit_asm_movzx_r8(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src) {
  note(a, dst);
  note(a, src);
  rex(a, false, dst, src, src >= 4 && src < 8);
  byte(a, 0x0F);
  byte(a, 0xB6);
  modrm_rr(a, dst, src);
}

/* ---- SSE2 (spec-runtime-float, story 3) ------------------------------------------------------
 *
 * Legacy-encoded scalar SSE: an optional mandatory prefix (F3 for single, F2 for double, 66 for the
 * integer moves and `ucomisd`), then REX if a general register above r7 or REX.W is needed, then 0F
 * and the opcode. The prefix comes before REX. Only the general registers are recorded in
 * regs_used; the `xmm` ones are recorded in xmm_used. */

static void sse_rr(GRJIT_Asm * a, int prefix, bool w, uint8_t op, int reg, int rm) {
  if (prefix != 0) {
    byte(a, (uint8_t)prefix);
  }
  rex(a, w, reg, rm, false);
  byte(a, 0x0F);
  byte(a, op);
  modrm_rr(a, reg, rm);
}

static int sse_prefix(bool dbl) {
  return dbl ? 0xF2 : 0xF3;
}

void grjit_asm_movs_load(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Reg base, int32_t disp, bool dbl) {
  note_xmm(a, dst);
  note(a, base);
  byte(a, (uint8_t)sse_prefix(dbl));
  rex(a, false, dst, base, false);
  byte(a, 0x0F);
  byte(a, 0x10);
  modrm_mem(a, dst, base, disp);
}

void grjit_asm_movs_store(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Xmm src, bool dbl) {
  note_xmm(a, src);
  note(a, base);
  byte(a, (uint8_t)sse_prefix(dbl));
  rex(a, false, src, base, false);
  byte(a, 0x0F);
  byte(a, 0x11);
  modrm_mem(a, src, base, disp);
}

void grjit_asm_movd_xr(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Reg src, bool wide) {
  note_xmm(a, dst);
  note(a, src);
  sse_rr(a, 0x66, wide, 0x6E, dst, src);
}

void grjit_asm_movd_rx(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Xmm src, bool wide) {
  note_xmm(a, src);
  note(a, dst);
  sse_rr(a, 0x66, wide, 0x7E, src, dst);
}

void grjit_asm_movaps(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src) {
  note_xmm(a, dst);
  note_xmm(a, src);
  sse_rr(a, 0, false, 0x28, dst, src);
}

void grjit_asm_sse_rr(GRJIT_Asm * a, GRJIT_SseOp op, GRJIT_Xmm dst, GRJIT_Xmm src, bool dbl) {
  note_xmm(a, dst);
  note_xmm(a, src);
  sse_rr(a, sse_prefix(dbl), false, (uint8_t)op, dst, src);
}

void grjit_asm_sse_logic(GRJIT_Asm * a, GRJIT_SseLogic op, GRJIT_Xmm dst, GRJIT_Xmm src) {
  note_xmm(a, dst);
  note_xmm(a, src);
  sse_rr(a, 0, false, (uint8_t)op, dst, src);
}

void grjit_asm_ucomis(GRJIT_Asm * a, GRJIT_Xmm x, GRJIT_Xmm y, bool dbl) {
  note_xmm(a, x);
  note_xmm(a, y);
  sse_rr(a, dbl ? 0x66 : 0, false, 0x2E, x, y);
}

/* `roundss`/`roundsd xmm, xmm, imm8` (SSE4.1): `66 [REX] 0F 3A 0A/0B /r ib`. */
static void sse_round(GRJIT_Asm * a, uint8_t opcode, GRJIT_Xmm dst, GRJIT_Xmm src, uint8_t imm) {
  note_xmm(a, dst);
  note_xmm(a, src);
  byte(a, 0x66);
  rex(a, false, dst, src, false);
  byte(a, 0x0F);
  byte(a, 0x3A);
  byte(a, opcode);
  modrm_rr(a, dst, src);
  byte(a, imm);
}

void grjit_asm_roundss(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src, uint8_t imm) {
  sse_round(a, 0x0A, dst, src, imm);
}

void grjit_asm_roundsd(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src, uint8_t imm) {
  sse_round(a, 0x0B, dst, src, imm);
}

void grjit_asm_cmps(GRJIT_Asm * a, GRJIT_SsePred pred, GRJIT_Xmm dst, GRJIT_Xmm src, bool dbl) {
  note_xmm(a, dst);
  note_xmm(a, src);
  sse_rr(a, sse_prefix(dbl), false, 0xC2, dst, src);
  byte(a, (uint8_t)pred);
}

void grjit_asm_cvtsi2s(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Reg src, bool dbl, bool wide_src) {
  note_xmm(a, dst);
  note(a, src);
  sse_rr(a, sse_prefix(dbl), wide_src, 0x2A, dst, src);
}

void grjit_asm_cvtts2si(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Xmm src, bool dbl, bool wide_dst) {
  note_xmm(a, src);
  note(a, dst);
  sse_rr(a, sse_prefix(dbl), wide_dst, 0x2C, dst, src);
}

void grjit_asm_cvts2s(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src, bool to_double) {
  note_xmm(a, dst);
  note_xmm(a, src);
  sse_rr(a, to_double ? 0xF3 : 0xF2, false, 0x5A, dst, src);
}

void grjit_asm_push(GRJIT_Asm * a, GRJIT_Reg r) {
  note(a, r);
  rex(a, false, 0, r, false);
  byte(a, (uint8_t)(0x50 + (r & 7)));
}

void grjit_asm_pop(GRJIT_Asm * a, GRJIT_Reg r) {
  note(a, r);
  rex(a, false, 0, r, false);
  byte(a, (uint8_t)(0x58 + (r & 7)));
}

static void rsp_adjust(GRJIT_Asm * a, int ext, uint32_t bytes) {
  note(a, GRJIT_RSP);
  rex(a, true, 0, GRJIT_RSP, false);
  if (bytes <= 127) {
    byte(a, 0x83);
    modrm_rr(a, ext, GRJIT_RSP);
    byte(a, (uint8_t)bytes);
  } else {
    byte(a, 0x81);
    modrm_rr(a, ext, GRJIT_RSP);
    imm32(a, bytes);
  }
}

void grjit_asm_sub_rsp(GRJIT_Asm * a, uint32_t bytes) {
  rsp_adjust(a, 5, bytes);
}

void grjit_asm_add_rsp(GRJIT_Asm * a, uint32_t bytes) {
  rsp_adjust(a, 0, bytes);
}

void grjit_asm_call_r(GRJIT_Asm * a, GRJIT_Reg r) {
  note(a, r);
  rex(a, false, 0, r, false);
  byte(a, 0xFF);
  modrm_rr(a, 2, r);
}

void grjit_asm_jmp_r(GRJIT_Asm * a, GRJIT_Reg r) {
  note(a, r);
  rex(a, false, 0, r, false);
  byte(a, 0xFF);
  modrm_rr(a, 4, r);
}

void grjit_asm_ret(GRJIT_Asm * a) {
  byte(a, 0xC3);
}

void grjit_asm_ret_imm(GRJIT_Asm * a, uint16_t bytes) {
  byte(a, 0xC2);
  byte(a, (uint8_t)(bytes & 0xFF));
  byte(a, (uint8_t)(bytes >> 8));
}

void grjit_asm_cmp_ri(GRJIT_Asm * a, GRJIT_Reg r, int32_t imm) {
  note(a, r);
  rex(a, true, 0, r, false);
  if (imm >= -128 && imm <= 127) {
    byte(a, 0x83);
    modrm_rr(a, 7, r);
    byte(a, (uint8_t)(int8_t)imm);
  } else {
    byte(a, 0x81);
    modrm_rr(a, 7, r);
    imm32(a, (uint32_t)imm);
  }
}

void grjit_asm_cmp_rm(GRJIT_Asm * a, GRJIT_Reg r, GRJIT_Reg base, int32_t disp) {
  inst_mem(a, false, true, 0x3B, -1, r, base, disp, false);
}

void grjit_asm_leave(GRJIT_Asm * a) {
  byte(a, 0xC9);
}

void grjit_asm_ud2(GRJIT_Asm * a) {
  byte(a, 0x0F);
  byte(a, 0x0B);
}

GRJIT_Label grjit_asm_label(GRJIT_Asm * a) {
  if (a->status != GRJIT_ASM_OK) {
    return SIZE_MAX;
  }
  if (a->label_count == a->label_capacity) {
    size_t cap = a->label_capacity == 0 ? 16 : a->label_capacity * 2;
    size_t * grown =
        a->allocator->realloc_fn(a->allocator->ctx, a->label_at, cap * sizeof *grown);
    if (grown == NULL) {
      fail(a, GRJIT_ASM_OOM);
      return SIZE_MAX;
    }
    a->label_at = grown;
    a->label_capacity = cap;
  }
  a->label_at[a->label_count] = SIZE_MAX;
  return a->label_count++;
}

void grjit_asm_bind(GRJIT_Asm * a, GRJIT_Label label) {
  if (a->status == GRJIT_ASM_OK && label < a->label_count) {
    a->label_at[label] = a->length;
  }
}

size_t grjit_asm_label_offset(const GRJIT_Asm * a, GRJIT_Label label) {
  return label < a->label_count ? a->label_at[label] : SIZE_MAX;
}

static void add_fixup(GRJIT_Asm * a, GRJIT_Label label) {
  if (a->status != GRJIT_ASM_OK) {
    return;
  }
  if (a->fixup_count == a->fixup_capacity) {
    size_t cap = a->fixup_capacity == 0 ? 16 : a->fixup_capacity * 2;
    GRJIT_AsmFixup * grown =
        a->allocator->realloc_fn(a->allocator->ctx, a->fixups, cap * sizeof *grown);
    if (grown == NULL) {
      fail(a, GRJIT_ASM_OOM);
      return;
    }
    a->fixups = grown;
    a->fixup_capacity = cap;
  }
  a->fixups[a->fixup_count].at = a->length;
  a->fixups[a->fixup_count].label = label;
  a->fixup_count++;
}

/* A branch with opcode `short_op` (rel8) or `long_op` (rel32, with a 0x0F
 * escape when `escape` is set). */
static void branch(GRJIT_Asm * a, uint8_t short_op, bool escape, uint8_t long_op,
    GRJIT_Label label) {
  if (a->status != GRJIT_ASM_OK) {
    return;
  }
  if (label >= a->label_count) {
    fail(a, GRJIT_ASM_BAD);
    return;
  }
  size_t target = a->label_at[label];
  if (target != SIZE_MAX) {
    int64_t rel = (int64_t)target - (int64_t)(a->length + 2);
    if (rel >= -128) {
      byte(a, short_op);
      byte(a, (uint8_t)(int8_t)rel);
      return;
    }
  }
  if (escape) {
    byte(a, 0x0F);
  }
  byte(a, long_op);
  if (target != SIZE_MAX) {
    int64_t rel = (int64_t)target - (int64_t)(a->length + 4);
    imm32(a, (uint32_t)(int32_t)rel);
  } else {
    add_fixup(a, label);
    imm32(a, 0);
  }
}

void grjit_asm_jmp(GRJIT_Asm * a, GRJIT_Label label) {
  branch(a, 0xEB, false, 0xE9, label);
}

void grjit_asm_jcc(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Label label) {
  branch(a, (uint8_t)(0x70 + cond), true, (uint8_t)(0x80 + cond), label);
}

void grjit_asm_lea_rip(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Label label) {
  if (a->status != GRJIT_ASM_OK) {
    return;
  }
  if (label >= a->label_count) {
    fail(a, GRJIT_ASM_BAD);
    return;
  }
  note(a, dst);
  rex(a, true, dst, 0, false);
  byte(a, 0x8D);
  byte(a, (uint8_t)(((dst & 7) << 3) | 5)); /* mod 00, rm 101: [rip + disp32] */
  size_t target = a->label_at[label];
  if (target != SIZE_MAX) {
    imm32(a, (uint32_t)(int32_t)((int64_t)target - (int64_t)(a->length + 4)));
  } else {
    add_fixup(a, label);
    imm32(a, 0);
  }
}

void grjit_asm_finish(GRJIT_Asm * a) {
  if (a->status != GRJIT_ASM_OK) {
    return;
  }
  for (size_t i = 0; i < a->fixup_count; i++) {
    size_t target = a->label_at[a->fixups[i].label];
    if (target == SIZE_MAX) {
      fail(a, GRJIT_ASM_BAD);
      return;
    }
    int64_t rel = (int64_t)target - (int64_t)(a->fixups[i].at + 4);
    uint32_t v = (uint32_t)(int32_t)rel;
    for (int k = 0; k < 4; k++) {
      a->buffer[a->fixups[i].at + (size_t)k] = (uint8_t)(v >> (8 * k));
    }
  }
  a->fixup_count = 0;
}
