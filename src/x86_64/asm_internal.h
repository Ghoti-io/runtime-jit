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
 * The x86-64 assembler: just the instructions the baseline backend emits,
 * written for this library (it is not ctang's `binary.h`, AD-9). Never
 * installed.
 *
 * Instructions are appended to a growing buffer. A failure to grow (the
 * allocator or the byte cap) is remembered, not reported per instruction:
 * every emitter becomes a no-op and ::grjit_asm_status says what happened, so
 * emission code is not a ladder of checks.
 *
 * Branches go to *labels*. A label bound before a branch to it is reached by
 * a backward `rel8` when the distance allows and `rel32` otherwise; a
 * forward branch is always `rel32` with a fixup that ::grjit_asm_finish
 * patches when the label is bound.
 */

#ifndef GHOTI_IO_GRJIT_SRC_X86_64_ASM_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_X86_64_ASM_INTERNAL_H

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

/** General-purpose registers, by hardware number. */
typedef enum GRJIT_Reg {
  GRJIT_RAX = 0, GRJIT_RCX, GRJIT_RDX, GRJIT_RBX, GRJIT_RSP, GRJIT_RBP,
  GRJIT_RSI, GRJIT_RDI, GRJIT_R8, GRJIT_R9, GRJIT_R10, GRJIT_R11, GRJIT_R12,
  GRJIT_R13, GRJIT_R14, GRJIT_R15
} GRJIT_Reg;

/** Condition codes, by their encoding in `jcc` and `setcc`. */
typedef enum GRJIT_Cond {
  GRJIT_COND_O = 0, GRJIT_COND_NO, GRJIT_COND_B, GRJIT_COND_AE, GRJIT_COND_E,
  GRJIT_COND_NE, GRJIT_COND_BE, GRJIT_COND_A, GRJIT_COND_S, GRJIT_COND_NS,
  GRJIT_COND_P, GRJIT_COND_NP, GRJIT_COND_L, GRJIT_COND_GE, GRJIT_COND_LE,
  GRJIT_COND_G
} GRJIT_Cond;

/** The two-register ALU instructions, by their `op r/m64, r64` opcode. */
typedef enum GRJIT_AluOp {
  GRJIT_ALU_ADD = 0x01, GRJIT_ALU_OR = 0x09, GRJIT_ALU_AND = 0x21,
  GRJIT_ALU_SUB = 0x29, GRJIT_ALU_XOR = 0x31, GRJIT_ALU_CMP = 0x39
} GRJIT_AluOp;

/** SSE registers, by hardware number. Only `xmm0` to `xmm7` are used by the emitter, so no
 *  encoding here needs a REX bit for one (the emitter's own scratch is `xmm0`-`xmm7`). */
typedef enum GRJIT_Xmm {
  GRJIT_XMM0 = 0, GRJIT_XMM1, GRJIT_XMM2, GRJIT_XMM3, GRJIT_XMM4, GRJIT_XMM5, GRJIT_XMM6,
  GRJIT_XMM7
} GRJIT_Xmm;

/** The scalar arithmetic instructions, by their opcode byte after `0F`. */
typedef enum GRJIT_SseOp {
  GRJIT_SSE_SQRT = 0x51, GRJIT_SSE_ADD = 0x58, GRJIT_SSE_MUL = 0x59, GRJIT_SSE_SUB = 0x5C,
  GRJIT_SSE_MIN = 0x5D, GRJIT_SSE_DIV = 0x5E, GRJIT_SSE_MAX = 0x5F
} GRJIT_SseOp;

/** The bitwise instructions on a whole register (the `ps` forms, which do not care whether
 *  the bits are a float's), by their opcode byte after `0F`. */
typedef enum GRJIT_SseLogic {
  GRJIT_SSE_AND = 0x54, GRJIT_SSE_ANDN = 0x55, GRJIT_SSE_OR = 0x56, GRJIT_SSE_XOR = 0x57
} GRJIT_SseLogic;

/** The predicates of `cmpss`/`cmpsd`, which write all ones or all zeros. */
typedef enum GRJIT_SsePred {
  GRJIT_SSEP_EQ = 0, GRJIT_SSEP_LT = 1, GRJIT_SSEP_LE = 2, GRJIT_SSEP_UNORD = 3
} GRJIT_SsePred;

/** The shifts and rotates, by their ModRM extension in `D1`/`D3`/`C1`. */
typedef enum GRJIT_ShiftOp {
  GRJIT_SHIFT_ROL = 0, GRJIT_SHIFT_ROR = 1, GRJIT_SHIFT_SHL = 4,
  GRJIT_SHIFT_SHR = 5, GRJIT_SHIFT_SAR = 7
} GRJIT_ShiftOp;

/** What went wrong, if anything. */
typedef enum GRJIT_AsmStatus {
  GRJIT_ASM_OK,
  GRJIT_ASM_OOM,   ///< The allocator returned NULL.
  GRJIT_ASM_LIMIT, ///< The code would exceed the byte cap.
  GRJIT_ASM_BAD    ///< A label was never bound, or a branch cannot reach.
} GRJIT_AsmStatus;

typedef struct GRJIT_AsmFixup {
  size_t at;          ///< Offset of the rel32 field.
  GRJIT_Label label;  ///< The label it goes to.
} GRJIT_AsmFixup;

/** The assembler. Zero-initialise by ::grjit_asm_init. */
typedef struct GRJIT_Asm {
  const GRJIT_Allocator * allocator;
  uint8_t * buffer;
  size_t length;
  size_t capacity;
  size_t limit;
  GRJIT_AsmStatus status;
  size_t * label_at;   ///< Offset of each bound label, SIZE_MAX if unbound.
  size_t label_count;
  size_t label_capacity;
  GRJIT_AsmFixup * fixups;
  size_t fixup_count;
  size_t fixup_capacity;
  uint32_t regs_used;  ///< Bit `r` is set once register `r` has been encoded.
  uint32_t xmm_used;   ///< Bit `n` is set once `xmm`n has been encoded by an SSE instruction.
} GRJIT_Asm;

/** Starts an assembler whose output may not exceed `limit` bytes. */
void grjit_asm_init(GRJIT_Asm * a, const GRJIT_Allocator * allocator, size_t limit);
/** Frees everything the assembler holds. */
void grjit_asm_free(GRJIT_Asm * a);
/** The first failure, or ::GRJIT_ASM_OK. */
GRJIT_AsmStatus grjit_asm_status(const GRJIT_Asm * a);
/** Every register an emitted instruction has named, one bit per hardware
 *  number (`rsp` included). It is what the Win64 tests read to show that a
 *  callee-saved register (`rbx`, `rsi`, `rdi`, `r12`-`r15`) is never touched,
 *  without disassembling anything. */
uint32_t grjit_asm_regs_used(const GRJIT_Asm * a);
/** The `xmm` registers encoded so far, bit `n` for `xmm`n. */
uint32_t grjit_asm_xmm_used(const GRJIT_Asm * a);
/** The bytes emitted so far. */
const uint8_t * grjit_asm_bytes(const GRJIT_Asm * a);
/** Their count: also the offset of the next instruction. */
size_t grjit_asm_size(const GRJIT_Asm * a);

/** Makes a label. Returns SIZE_MAX if the table could not grow. */
GRJIT_Label grjit_asm_label(GRJIT_Asm * a);
/** Binds a label to the current offset. */
void grjit_asm_bind(GRJIT_Asm * a, GRJIT_Label label);
/** The offset a label is bound to, or SIZE_MAX. */
size_t grjit_asm_label_offset(const GRJIT_Asm * a, GRJIT_Label label);
/** Patches every forward branch; ::GRJIT_ASM_BAD if a label is unbound. */
void grjit_asm_finish(GRJIT_Asm * a);

/** Appends raw bytes (for padding and the tests). */
void grjit_asm_raw(GRJIT_Asm * a, const void * bytes, size_t count);

void grjit_asm_mov_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src);
/** `mov r32, r32`: copies the low half and clears the upper half. */
void grjit_asm_mov32_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src);
/** The shortest of `mov r32, imm32`, `mov r64, simm32` and `movabs`. */
void grjit_asm_mov_ri(GRJIT_Asm * a, GRJIT_Reg dst, uint64_t imm);
/** Always `movabs r64, imm64` (10 bytes). */
void grjit_asm_mov_ri64(GRJIT_Asm * a, GRJIT_Reg dst, uint64_t imm);
void grjit_asm_load64(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
void grjit_asm_store64(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src);
/** `lea dst, [base + disp]`. */
void grjit_asm_lea(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
/** `test byte [base + disp], al`: a read of that byte that changes only the
 *  flags, for the stack probes of a Win64 frame of a page or more. */
void grjit_asm_probe(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp);
/** Zero-extending loads of 8, 16 and 32 bits into a 64-bit register. */
void grjit_asm_load8u(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
void grjit_asm_load16u(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
void grjit_asm_load32u(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
/** Sign-extending loads of 8, 16 and 32 bits into a 64-bit register. */
void grjit_asm_load8s(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
void grjit_asm_load16s(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
void grjit_asm_load32s(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg base, int32_t disp);
/** Narrow stores of the low 8, 16 and 32 bits of a register. */
void grjit_asm_store8(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src);
void grjit_asm_store16(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src);
void grjit_asm_store32(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Reg src);

void grjit_asm_alu_rr(GRJIT_Asm * a, GRJIT_AluOp op, GRJIT_Reg dst, GRJIT_Reg src);
void grjit_asm_imul_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src);
void grjit_asm_neg(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_not(GRJIT_Asm * a, GRJIT_Reg r);
/** Shifts of a 64-bit register by `cl`. */
void grjit_asm_shl_cl(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_shr_cl(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_sar_cl(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_test_rr(GRJIT_Asm * a, GRJIT_Reg x, GRJIT_Reg y);

/* The forms the integer operations of numeric-operations.md need (spec-runtime-float).
 * The 32-bit forms are real 32-bit instructions: the hardware masks a shift count to
 * five bits, and the upper half of the destination is cleared. */
/** `op r32, r32` (the 32-bit twin of ::grjit_asm_alu_rr). */
void grjit_asm_alu32_rr(GRJIT_Asm * a, GRJIT_AluOp op, GRJIT_Reg dst, GRJIT_Reg src);
/** `imul r32, r32`. */
void grjit_asm_imul32_rr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src);
/** `neg r32` and `not r32`. */
void grjit_asm_neg32(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_not32(GRJIT_Asm * a, GRJIT_Reg r);
/** A shift or rotate of `r` by `cl`, 64 bits when `wide` and 32 otherwise. */
void grjit_asm_shift_cl(GRJIT_Asm * a, GRJIT_ShiftOp op, GRJIT_Reg r, bool wide);
/** The same by an immediate count. */
void grjit_asm_shift_ri(GRJIT_Asm * a, GRJIT_ShiftOp op, GRJIT_Reg r, uint8_t count, bool wide);
/** `movsx`/`movsxd` of the low 8, 16 or 32 bits of `src` into `dst`, 64 bits when `wide`
 *  (32 bits is `movsxd` and only exists wide). */
void grjit_asm_movsx(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, unsigned from_bits, bool wide);
/** `bsr` and `bsf`: the index of the highest and lowest set bit of `src`; ZF is set, and
 *  `dst` unspecified, when `src` is zero. */
void grjit_asm_bsr(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide);
void grjit_asm_bsf(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide);
/** `lzcnt`, `tzcnt` and `popcnt dst, src` (`F3 [REX] 0F BD / BC / B8 /r`), 64 bits when `wide`.
 *  Three functions and no prefix parameter on purpose: without the `F3` the first two are `bsr`
 *  and `bsf`, which a CPU without LZCNT or BMI1 runs these as (no fault, other answers). The
 *  result is the operand width for a zero source (`lzcnt`, `tzcnt`); ZF/CF are not used. Emitted
 *  only for a feature in the effective set (features.h). */
void grjit_asm_lzcnt(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide);
void grjit_asm_tzcnt(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide);
void grjit_asm_popcnt(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src, bool wide);
/** `cmovcc dst, src`. */
void grjit_asm_cmov(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Reg dst, GRJIT_Reg src, bool wide);
/** `cdq` and `cqo`: sign-extend `eax` into `edx`, `rax` into `rdx`. */
void grjit_asm_cdq(GRJIT_Asm * a);
void grjit_asm_cqo(GRJIT_Asm * a);
/** `div r` and `idiv r` (`edx:eax` or `rdx:rax` over `r`): raise #DE in hardware on a zero
 *  divisor, and `idiv` on an overflowing quotient. Only guarded code reaches them. */
void grjit_asm_div(GRJIT_Asm * a, GRJIT_Reg r, bool is_signed, bool wide);
/** `cmp r32, imm8` (sign-extended). */
void grjit_asm_cmp_ri32(GRJIT_Asm * a, GRJIT_Reg r, int8_t imm);
void grjit_asm_setcc(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Reg r);
/** `movzx r32, r8`, which clears the upper half of the 64-bit register. */
void grjit_asm_movzx_r8(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src);

/* The SSE2 forms of the float operations (spec-runtime-float, story 3). SSE2 only: no
 * instruction here is newer than x86-64-v1. `dbl` selects the double (`sd`) form, the single
 * (`ss`) otherwise. A register operand that is an `xmm` is never recorded in
 * ::grjit_asm_regs_used (it shares no numbering with the general registers): it is recorded in
 * ::grjit_asm_xmm_used. */
/** `movss`/`movsd xmm, [base + disp]`: loads 32 or 64 bits and clears the rest of the register. */
void grjit_asm_movs_load(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Reg base, int32_t disp, bool dbl);
/** `movss`/`movsd [base + disp], xmm`: stores 32 or 64 bits. */
void grjit_asm_movs_store(GRJIT_Asm * a, GRJIT_Reg base, int32_t disp, GRJIT_Xmm src, bool dbl);
/** `movd xmm, r32` or, when `wide`, `movq xmm, r64`: clears the rest of the register. */
void grjit_asm_movd_xr(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Reg src, bool wide);
/** `movd r32, xmm` (the upper half of `r` is cleared) or `movq r64, xmm`. */
void grjit_asm_movd_rx(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Xmm src, bool wide);
/** `movaps xmm, xmm`. */
void grjit_asm_movaps(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src);
/** `addss`/`subss`/`mulss`/`divss`/`minss`/`maxss`/`sqrtss` and the `sd` forms: `dst = dst op src`
 *  (`sqrt`: `dst = sqrt(src)`). */
void grjit_asm_sse_rr(GRJIT_Asm * a, GRJIT_SseOp op, GRJIT_Xmm dst, GRJIT_Xmm src, bool dbl);
/** `andps`/`andnps`/`orps`/`xorps`: `dst = dst op src` on the whole register (`andn`: `~dst & src`). */
void grjit_asm_sse_logic(GRJIT_Asm * a, GRJIT_SseLogic op, GRJIT_Xmm dst, GRJIT_Xmm src);
/** `ucomiss`/`ucomisd x, y`: ZF, PF and CF as for an unsigned compare of `x` with `y`, with all
 *  three set when either is a NaN. */
void grjit_asm_ucomis(GRJIT_Asm * a, GRJIT_Xmm x, GRJIT_Xmm y, bool dbl);
/** `cmpss`/`cmpsd dst, src, pred`: all ones in the low 32 or 64 bits of `dst` if `pred` holds
 *  for `dst` and `src`, else all zeros. */
void grjit_asm_cmps(GRJIT_Asm * a, GRJIT_SsePred pred, GRJIT_Xmm dst, GRJIT_Xmm src, bool dbl);
/** `roundss`/`roundsd dst, src, imm` (SSE4.1, `66 0F 3A 0A/0B /r ib`): rounds the low 32 or 64 bits
 *  of `src` as `imm` says (bits 0-1 the mode, bit 2 use MXCSR instead, bit 3 no precision
 *  exception) and leaves the rest of `dst` as it was. Emitted only when SSE4.1 is in the
 *  effective set. */
void grjit_asm_roundss(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src, uint8_t imm);
void grjit_asm_roundsd(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src, uint8_t imm);
/** `cvtsi2ss`/`cvtsi2sd xmm, r32` or, when `wide_src`, `r64` (a signed integer). */
void grjit_asm_cvtsi2s(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Reg src, bool dbl, bool wide_src);
/** `cvttss2si`/`cvttsd2si r, xmm`: truncating, to 32 or, when `wide_dst`, 64 bits. An operand
 *  out of range (or a NaN) gives the "integer indefinite" value, not a fault. */
void grjit_asm_cvtts2si(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Xmm src, bool dbl, bool wide_dst);
/** `cvtss2sd` (`to_double`) or `cvtsd2ss`. */
void grjit_asm_cvts2s(GRJIT_Asm * a, GRJIT_Xmm dst, GRJIT_Xmm src, bool to_double);

void grjit_asm_push(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_pop(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_sub_rsp(GRJIT_Asm * a, uint32_t bytes);
void grjit_asm_add_rsp(GRJIT_Asm * a, uint32_t bytes);
void grjit_asm_call_r(GRJIT_Asm * a, GRJIT_Reg r);
/** `jmp r64`: an indirect jump, for a tail call, which enters the callee with
 *  the stack and frame register already set as a call would leave them. */
void grjit_asm_jmp_r(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_ret(GRJIT_Asm * a);
/** `ret imm16`: returns and pops `bytes` more (the callee pops its own stack
 *  arguments in the internal convention). */
void grjit_asm_ret_imm(GRJIT_Asm * a, uint16_t bytes);
/** `lea dst, [rip + label]`: the address of a label, for a return address the
 *  code stores before it makes the call that returns to it. */
void grjit_asm_lea_rip(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Label label);
/** `cmp r, imm32` (sign-extended to 64 bits). */
void grjit_asm_cmp_ri(GRJIT_Asm * a, GRJIT_Reg r, int32_t imm);
/** `cmp r, [base + disp]`: the flags of `r - [base + disp]`. */
void grjit_asm_cmp_rm(GRJIT_Asm * a, GRJIT_Reg r, GRJIT_Reg base, int32_t disp);
void grjit_asm_leave(GRJIT_Asm * a);
void grjit_asm_ud2(GRJIT_Asm * a);
void grjit_asm_jmp(GRJIT_Asm * a, GRJIT_Label label);
void grjit_asm_jcc(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Label label);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_X86_64_ASM_INTERNAL_H */
