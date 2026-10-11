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
 * @file asm_internal.h
 *
 * The AArch64 assembler: just the instructions the arm64 baseline emits,
 * written for this library (it is not seeded from any other assembler, AD-9).
 * Never installed. It is plain C that only emits bytes, so it compiles and is
 * tested on every host; only running what it emits needs an AArch64 machine.
 *
 * Every instruction is one little-endian 32-bit word. A failure to grow (the
 * allocator or the byte cap) is remembered, not reported per instruction:
 * every emitter becomes a no-op and ::grjit_a64_status says what happened.
 *
 * **Reach.** `b` reaches +-128 MiB, `b.cond`, `cbz` and `cbnz` only +-1 MiB,
 * and the byte cap defaults to 16 MiB. A conditional branch to a label that is
 * already bound is a short branch when it reaches and the long form when it
 * does not. A forward one is a short branch with a fixup, unless the assembler
 * is in *long* mode (::grjit_a64_set_long_branches), when it is the long form:
 * the inverted condition over the next instruction, then a `b` that reaches
 * everything the cap allows. The emitter assembles in short mode first and, if
 * ::grjit_a64_finish reports ::GRJIT_A64_FAR (a forward short branch cannot
 * reach its label), assembles again in long mode. A `b` that cannot reach (a
 * cap set above 128 MiB) is ::GRJIT_A64_LIMIT.
 *
 * `adr` reaches +-1 MiB and follows the same rule: a short `adr` when the label
 * is bound within reach; a forward one a short `adr` with a fixup, unless the
 * assembler is in long mode; and otherwise the long form, `adrp` and `add`, which
 * reaches +-4 GiB. The long form counts pages from the start of the code, so it
 * is right only for code that starts on a 4 KiB boundary, which a page mapping
 * does. (An `adr` in this library is always a few instructions from its label:
 * the address after the call it precedes. The long form exists so that the rule
 * is the same for every label, not because an emitter needs it.)
 */

#ifndef GHOTI_IO_GRJIT_SRC_ARM64_ASM_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_ARM64_ASM_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/core.h>

#include "../backend/backend_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** General-purpose registers by number. Register 31 is `sp` where an
 *  instruction takes a stack pointer and `xzr` where it takes a zero. */
typedef enum GRJIT_A64Reg {
  GRJIT_A64_X0 = 0, GRJIT_A64_X1, GRJIT_A64_X2, GRJIT_A64_X3, GRJIT_A64_X4,
  GRJIT_A64_X5, GRJIT_A64_X6, GRJIT_A64_X7, GRJIT_A64_X8, GRJIT_A64_X9,
  GRJIT_A64_X10, GRJIT_A64_X11, GRJIT_A64_X12, GRJIT_A64_X13, GRJIT_A64_X14,
  GRJIT_A64_X15, GRJIT_A64_X16, GRJIT_A64_X17, GRJIT_A64_X18, GRJIT_A64_X19,
  GRJIT_A64_X20, GRJIT_A64_X21, GRJIT_A64_X22, GRJIT_A64_X23, GRJIT_A64_X24,
  GRJIT_A64_X25, GRJIT_A64_X26, GRJIT_A64_X27, GRJIT_A64_X28, GRJIT_A64_X29,
  GRJIT_A64_X30, GRJIT_A64_SP, GRJIT_A64_XZR = 31
} GRJIT_A64Reg;

/** The frame pointer and link register, by their usual names. */
#define GRJIT_A64_FP GRJIT_A64_X29
#define GRJIT_A64_LR GRJIT_A64_X30

/** Condition codes, by their encoding. */
typedef enum GRJIT_A64Cond {
  GRJIT_A64_EQ = 0, GRJIT_A64_NE, GRJIT_A64_HS, GRJIT_A64_LO, GRJIT_A64_MI,
  GRJIT_A64_PL, GRJIT_A64_VS, GRJIT_A64_VC, GRJIT_A64_HI, GRJIT_A64_LS,
  GRJIT_A64_GE, GRJIT_A64_LT, GRJIT_A64_GT, GRJIT_A64_LE
} GRJIT_A64Cond;

/** The scalar floating-point / Advanced SIMD registers by number (`v0` to `v31`, used as `s`, `d`
 *  or `b`). `v8` to `v15` are callee-saved in their low 64 bits by the procedure call standard;
 *  the baseline emitter never encodes them, which the assembler records
 *  (::grjit_a64_vregs_used) so a test can say so. */
typedef enum GRJIT_A64VReg {
  GRJIT_A64_V0 = 0, GRJIT_A64_V1, GRJIT_A64_V2, GRJIT_A64_V3, GRJIT_A64_V4, GRJIT_A64_V5,
  GRJIT_A64_V6, GRJIT_A64_V7, GRJIT_A64_V8, GRJIT_A64_V9, GRJIT_A64_V10, GRJIT_A64_V11,
  GRJIT_A64_V12, GRJIT_A64_V13, GRJIT_A64_V14, GRJIT_A64_V15, GRJIT_A64_V16, GRJIT_A64_V17,
  GRJIT_A64_V18, GRJIT_A64_V19, GRJIT_A64_V20, GRJIT_A64_V21, GRJIT_A64_V22, GRJIT_A64_V23,
  GRJIT_A64_V24, GRJIT_A64_V25, GRJIT_A64_V26, GRJIT_A64_V27, GRJIT_A64_V28, GRJIT_A64_V29,
  GRJIT_A64_V30, GRJIT_A64_V31
} GRJIT_A64VReg;

/** A `v` register as the `rt` of a floating-point load or store (::GRJIT_A64_MEM_F32,
 *  ::GRJIT_A64_MEM_F64), whose register argument is typed as a general one. */
static inline GRJIT_A64Reg GRJIT_A64_VREG_AS_REG(GRJIT_A64VReg v) {
  return (GRJIT_A64Reg)v;
}

/** The scalar width of a floating-point instruction: `s` (binary32) or `d` (binary64). */
typedef enum GRJIT_A64FSize { GRJIT_A64_SINGLE, GRJIT_A64_DOUBLE } GRJIT_A64FSize;

/** The three-register data-processing instructions. ::grjit_a64_alu is the 64-bit form,
 *  ::grjit_a64_alu32 the 32-bit one (`w` registers, which clear the upper half of the
 *  destination); `SDIV`, `UDIV` and `RORV` are new with the numeric operations, and a
 *  division by zero gives zero and `MIN / -1` gives `MIN` (neither traps). */
typedef enum GRJIT_A64Alu {
  GRJIT_A64_ADD, GRJIT_A64_SUB, GRJIT_A64_AND, GRJIT_A64_ORR, GRJIT_A64_EOR,
  GRJIT_A64_MUL, GRJIT_A64_LSLV, GRJIT_A64_LSRV, GRJIT_A64_ASRV,
  GRJIT_A64_RORV, GRJIT_A64_SDIV, GRJIT_A64_UDIV, GRJIT_A64_BIC
} GRJIT_A64Alu;

/** The two-register bit instructions: count leading zeros, reverse bits. */
typedef enum GRJIT_A64Bits { GRJIT_A64_CLZ, GRJIT_A64_RBIT } GRJIT_A64Bits;

/** The sign extensions, `sxtb`, `sxth` and `sxtw` (the last 64-bit only). */
typedef enum GRJIT_A64Ext { GRJIT_A64_SXTB, GRJIT_A64_SXTH, GRJIT_A64_SXTW } GRJIT_A64Ext;

/** The binary scalar floating-point operations. `FMIN` and `FMAX` are the instructions with
 *  IEEE 754-2019 `minimum` / `maximum` semantics (a NaN operand gives a NaN, `-0 < +0`), which is
 *  WebAssembly's; `fminnm` / `fmaxnm` (a quiet NaN is ignored) are deliberately not here. */
typedef enum GRJIT_A64FpBinary {
  GRJIT_A64_FADD, GRJIT_A64_FSUB, GRJIT_A64_FMUL, GRJIT_A64_FDIV, GRJIT_A64_FMIN, GRJIT_A64_FMAX
} GRJIT_A64FpBinary;

/** The unary scalar floating-point operations. The rounding ones have the rounding direction in
 *  the instruction (`n` nearest even, `p` up, `m` down, `z` toward zero); the forms that follow
 *  FPCR (`frinti`, `frintx`) and ties-away (`frinta`) are deliberately not here. */
typedef enum GRJIT_A64FpUnary {
  GRJIT_A64_FSQRT, GRJIT_A64_FABS, GRJIT_A64_FNEG, GRJIT_A64_FRINTN, GRJIT_A64_FRINTP,
  GRJIT_A64_FRINTM, GRJIT_A64_FRINTZ
} GRJIT_A64FpUnary;

/** What went wrong, if anything. */
typedef enum GRJIT_A64Status {
  GRJIT_A64_OK,
  GRJIT_A64_OOM,    ///< The allocator returned NULL.
  GRJIT_A64_LIMIT,  ///< The code would exceed the byte cap, or a `b` cannot reach.
  GRJIT_A64_BAD,    ///< A label was never bound, or an operand cannot encode.
  GRJIT_A64_FAR     ///< A short conditional branch cannot reach: assemble again
                    ///< in long mode.
} GRJIT_A64Status;

/** How a fixup is patched. */
typedef enum GRJIT_A64FixupKind {
  GRJIT_A64_FIXUP_B,     ///< `b`: imm26 in bits 0..25.
  GRJIT_A64_FIXUP_COND,  ///< `b.cond`, `cbz`, `cbnz`: imm19 in bits 5..23.
  GRJIT_A64_FIXUP_ADR,   ///< `adr`: a byte offset, immlo in bits 29..30 and immhi in 5..23.
  GRJIT_A64_FIXUP_ADRP,  ///< `adrp`: the same fields, in pages.
  GRJIT_A64_FIXUP_LO12   ///< The `add` after an `adrp`: the target's low 12 bits, in 10..21.
} GRJIT_A64FixupKind;

typedef struct GRJIT_A64Fixup {
  size_t at;                 ///< Offset of the instruction.
  GRJIT_Label label;
  GRJIT_A64FixupKind kind;
} GRJIT_A64Fixup;

/** The assembler. */
typedef struct GRJIT_A64Asm {
  const GRJIT_Allocator * allocator;
  uint8_t * buffer;
  size_t length;
  size_t capacity;
  size_t limit;
  GRJIT_A64Status status;
  bool long_branches;      ///< Forward conditional branches use the long form.
  uint32_t vregs_used;     ///< Bit `n` is set once `v`n has been encoded.
  size_t * label_at;       ///< Offset of each bound label, SIZE_MAX if unbound.
  size_t label_count;
  size_t label_capacity;
  GRJIT_A64Fixup * fixups;
  size_t fixup_count;
  size_t fixup_capacity;
} GRJIT_A64Asm;

/** Starts an assembler whose output may not exceed `limit` bytes. */
void grjit_a64_init(
    GRJIT_A64Asm * a, const GRJIT_Allocator * allocator, size_t limit);
/** Frees everything the assembler holds. */
void grjit_a64_free(GRJIT_A64Asm * a);
GRJIT_A64Status grjit_a64_status(const GRJIT_A64Asm * a);
const uint8_t * grjit_a64_bytes(const GRJIT_A64Asm * a);
size_t grjit_a64_size(const GRJIT_A64Asm * a);
/** Chooses the long form for forward conditional branches (see above). */
void grjit_a64_set_long_branches(GRJIT_A64Asm * a, bool enabled);

GRJIT_Label grjit_a64_label(GRJIT_A64Asm * a);
void grjit_a64_bind(GRJIT_A64Asm * a, GRJIT_Label label);
size_t grjit_a64_label_offset(const GRJIT_A64Asm * a, GRJIT_Label label);
/** Patches every forward branch. ::GRJIT_A64_BAD for an unbound label,
 *  ::GRJIT_A64_FAR for a short conditional branch out of reach,
 *  ::GRJIT_A64_LIMIT for a `b` out of reach. */
void grjit_a64_finish(GRJIT_A64Asm * a);

/** Appends one instruction word. */
void grjit_a64_word(GRJIT_A64Asm * a, uint32_t word);

void grjit_a64_movz(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint16_t imm, unsigned hw);
void grjit_a64_movn(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint16_t imm, unsigned hw);
void grjit_a64_movk(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint16_t imm, unsigned hw);
/** Any 64-bit immediate: the shorter of a `movz` and a `movn` start, then
 *  `movk` for each 16-bit piece that differs. One to four instructions. */
void grjit_a64_mov_ri(GRJIT_A64Asm * a, GRJIT_A64Reg rd, uint64_t imm);
/** How many instructions ::grjit_a64_mov_ri emits for `imm`. */
unsigned grjit_a64_mov_ri_count(uint64_t imm);
void grjit_a64_mov_rr(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm);
/** `mov w, w`: copies the low half and clears the upper half. */
void grjit_a64_mov32_rr(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm);
/** `add rd, rn, #0`, which is `mov` where either is `sp`. */
void grjit_a64_mov_sp(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn);

/** `add`/`sub` with a 12-bit immediate, shifted left by 12 when `shift12`. */
void grjit_a64_add_imm(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn,
    uint32_t imm12, bool shift12);
void grjit_a64_sub_imm(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn,
    uint32_t imm12, bool shift12);
/** `add`/`sub` of an extended register (`uxtx`): the form that can name `sp`. */
void grjit_a64_add_uxtx(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn, GRJIT_A64Reg rm);
void grjit_a64_sub_uxtx(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn, GRJIT_A64Reg rm);
/** `sp -= bytes` / `sp += bytes` for a multiple of 16: at most two instructions below
 *  16 MiB, and from there up `x17` holds the amount (three to six words, one of them the
 *  adjustment), so there is no size it refuses. */
void grjit_a64_sub_sp(GRJIT_A64Asm * a, uint32_t bytes);
void grjit_a64_add_sp(GRJIT_A64Asm * a, uint32_t bytes);

void grjit_a64_alu(GRJIT_A64Asm * a, GRJIT_A64Alu op, GRJIT_A64Reg rd,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm);
/** The 32-bit form of ::grjit_a64_alu: `w` registers, the upper half of the destination cleared. */
void grjit_a64_alu32(GRJIT_A64Asm * a, GRJIT_A64Alu op, GRJIT_A64Reg rd,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm);
void grjit_a64_neg(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm);
void grjit_a64_mvn(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm);
void grjit_a64_cmp(GRJIT_A64Asm * a, GRJIT_A64Reg rn, GRJIT_A64Reg rm);
void grjit_a64_neg32(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm);
void grjit_a64_mvn32(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rm);
void grjit_a64_cmp32(GRJIT_A64Asm * a, GRJIT_A64Reg rn, GRJIT_A64Reg rm);
/** `msub rd, rn, rm, ra`: `rd = ra - rn * rm`, `wide` for the 64-bit form. */
void grjit_a64_msub(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Reg rn, GRJIT_A64Reg rm,
    GRJIT_A64Reg ra, bool wide);
/** `clz` / `rbit`, `wide` for the 64-bit form (a `clz` of zero is the width). */
void grjit_a64_bits(GRJIT_A64Asm * a, GRJIT_A64Bits op, GRJIT_A64Reg rd, GRJIT_A64Reg rn, bool wide);
/** `sxtb` / `sxth` / `sxtw` from the low bits of `rn` to the `w` or `x` form of `rd`; `SXTW` is
 *  64-bit only. */
void grjit_a64_sext(GRJIT_A64Asm * a, GRJIT_A64Ext kind, GRJIT_A64Reg rd, GRJIT_A64Reg rn, bool wide);
/** `cset wd, cond`: 1 if the condition holds, else 0; the upper half is
 *  cleared. */
void grjit_a64_cset(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_A64Cond cond);

/** The widths a load or store takes. `F32` and `F64` are the `s` and `d` registers: `rt` is then
 *  a `v` register number (a ::GRJIT_A64VReg value), and the load clears the rest of the vector
 *  register. */
typedef enum GRJIT_A64Mem {
  GRJIT_A64_MEM_U8, GRJIT_A64_MEM_U16, GRJIT_A64_MEM_U32, GRJIT_A64_MEM_X64,
  GRJIT_A64_MEM_S8, GRJIT_A64_MEM_S16, GRJIT_A64_MEM_S32,
  GRJIT_A64_MEM_F32, GRJIT_A64_MEM_F64
} GRJIT_A64Mem;

/** The three addressing forms, one instruction each. Stores accept only the
 *  unsigned kinds. `imm9` is -256..255; `uimm` is a byte offset that is a
 *  multiple of the access size and below 4096 times it. */
void grjit_a64_load_unscaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind,
    GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t imm9);
void grjit_a64_load_scaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, uint32_t uimm);
void grjit_a64_load_reg(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm);
void grjit_a64_store_unscaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind,
    GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t imm9);
void grjit_a64_store_scaled(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, uint32_t uimm);
void grjit_a64_store_reg(GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt,
    GRJIT_A64Reg rn, GRJIT_A64Reg rm);

/** `rt = [rn + disp]` for any `int32` displacement: one instruction when the
 *  displacement fits an immediate form, otherwise `disp` is materialised in
 *  `x17` first and the register form is used (so `rn` and `rt` must not be
 *  `x17`). */
void grjit_a64_load(
    GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t disp);
/** `[rn + disp] = rt`, likewise. */
void grjit_a64_store(
    GRJIT_A64Asm * a, GRJIT_A64Mem kind, GRJIT_A64Reg rt, GRJIT_A64Reg rn, int32_t disp);

/** `stp x29, x30, [sp, #-16]!` and `ldp x29, x30, [sp], #16`. */
void grjit_a64_push_frame_record(GRJIT_A64Asm * a);
void grjit_a64_pop_frame_record(GRJIT_A64Asm * a);

void grjit_a64_ret(GRJIT_A64Asm * a);
void grjit_a64_blr(GRJIT_A64Asm * a, GRJIT_A64Reg rn);
void grjit_a64_br(GRJIT_A64Asm * a, GRJIT_A64Reg rn);
/** `adr rd, label`: the address of a label, in one instruction when it reaches
 *  (see the reach rule above), else `adrp` and `add`. */
void grjit_a64_adr(GRJIT_A64Asm * a, GRJIT_A64Reg rd, GRJIT_Label label);
/** `cmp rn, #imm12` (`subs xzr, rn, #imm12`), 64-bit. */
void grjit_a64_cmp_imm(GRJIT_A64Asm * a, GRJIT_A64Reg rn, uint32_t imm12);
/** Appends a 64-bit value as data, little-endian: the tag before an internal entry. */
void grjit_a64_data64(GRJIT_A64Asm * a, uint64_t value);
void grjit_a64_brk(GRJIT_A64Asm * a, uint16_t imm);

/* ---- Scalar floating point and the bit count (spec-runtime-float, story 5) -------------------
 *
 * Only `v0` to `v7` and `v16` to `v31` are caller-saved in full; the emitter uses `v0` to `v7`
 * only. Every function below that names a `v` register records it in ::grjit_a64_vregs_used. */

/** The set of `v` registers encoded so far, bit `n` for `v`n. */
uint32_t grjit_a64_vregs_used(const GRJIT_A64Asm * a);
/** `fmov sd, wn` / `fmov dd, xn`: the bits of a general register into a vector register (a
 *  `single` takes the low 32 bits and clears the rest). */
void grjit_a64_fmov_from_gpr(GRJIT_A64Asm * a, GRJIT_A64FSize size, GRJIT_A64VReg vd, GRJIT_A64Reg rn);
/** `fmov wd, sn` / `fmov xd, dn`: the bits of a vector register's `s` or `d` into a general one
 *  (`single` clears the upper half of the general register). */
void grjit_a64_fmov_to_gpr(GRJIT_A64Asm * a, GRJIT_A64FSize size, GRJIT_A64Reg rd, GRJIT_A64VReg vn);
/** `fmov vd, vn`, `s` or `d`. */
void grjit_a64_fmov_vv(GRJIT_A64Asm * a, GRJIT_A64FSize size, GRJIT_A64VReg vd, GRJIT_A64VReg vn);
void grjit_a64_fp_binary(GRJIT_A64Asm * a, GRJIT_A64FpBinary op, GRJIT_A64FSize size,
    GRJIT_A64VReg vd, GRJIT_A64VReg vn, GRJIT_A64VReg vm);
void grjit_a64_fp_unary(GRJIT_A64Asm * a, GRJIT_A64FpUnary op, GRJIT_A64FSize size,
    GRJIT_A64VReg vd, GRJIT_A64VReg vn);
/** `fcmp vn, vm`; an unordered pair sets `NZCV = 0011`. */
void grjit_a64_fcmp(GRJIT_A64Asm * a, GRJIT_A64FSize size, GRJIT_A64VReg vn, GRJIT_A64VReg vm);
/** `fcvt`: to the `to` size from the other one. */
void grjit_a64_fcvt(GRJIT_A64Asm * a, GRJIT_A64FSize to, GRJIT_A64VReg vd, GRJIT_A64VReg vn);
/** `scvtf` / `ucvtf vd, rn` from a `w` (`wide` false) or `x` register to the `to` size. */
void grjit_a64_cvt_to_fp(GRJIT_A64Asm * a, bool is_signed, GRJIT_A64FSize to, GRJIT_A64VReg vd,
    GRJIT_A64Reg rn, bool wide);
/** `fcvtzs` / `fcvtzu rd, vn` from the `from` size to a `w` (`wide` false) or `x` register:
 *  toward zero, saturating, a NaN giving 0. */
void grjit_a64_cvt_from_fp(GRJIT_A64Asm * a, bool is_signed, GRJIT_A64FSize from, GRJIT_A64Reg rd,
    GRJIT_A64VReg vn, bool wide);
/** `cnt vd.8b, vn.8b` and `addv bd, vn.8b`: the population count of a 64-bit value, in two steps. */
void grjit_a64_cnt8b(GRJIT_A64Asm * a, GRJIT_A64VReg vd, GRJIT_A64VReg vn);
void grjit_a64_addv8b(GRJIT_A64Asm * a, GRJIT_A64VReg vd, GRJIT_A64VReg vn);
void grjit_a64_b(GRJIT_A64Asm * a, GRJIT_Label label);
void grjit_a64_bcond(GRJIT_A64Asm * a, GRJIT_A64Cond cond, GRJIT_Label label);
/** `cbz` / `cbnz` on the 64-bit register. */
void grjit_a64_cbz(GRJIT_A64Asm * a, GRJIT_A64Reg rt, GRJIT_Label label);
void grjit_a64_cbnz(GRJIT_A64Asm * a, GRJIT_A64Reg rt, GRJIT_Label label);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_ARM64_ASM_INTERNAL_H */
