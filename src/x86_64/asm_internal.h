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
} GRJIT_Asm;

/** Starts an assembler whose output may not exceed `limit` bytes. */
void grjit_asm_init(GRJIT_Asm * a, const GRJIT_Allocator * allocator, size_t limit);
/** Frees everything the assembler holds. */
void grjit_asm_free(GRJIT_Asm * a);
/** The first failure, or ::GRJIT_ASM_OK. */
GRJIT_AsmStatus grjit_asm_status(const GRJIT_Asm * a);
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
void grjit_asm_setcc(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Reg r);
/** `movzx r32, r8`, which clears the upper half of the 64-bit register. */
void grjit_asm_movzx_r8(GRJIT_Asm * a, GRJIT_Reg dst, GRJIT_Reg src);

void grjit_asm_push(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_pop(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_sub_rsp(GRJIT_Asm * a, uint32_t bytes);
void grjit_asm_add_rsp(GRJIT_Asm * a, uint32_t bytes);
void grjit_asm_call_r(GRJIT_Asm * a, GRJIT_Reg r);
void grjit_asm_ret(GRJIT_Asm * a);
void grjit_asm_leave(GRJIT_Asm * a);
void grjit_asm_ud2(GRJIT_Asm * a);
void grjit_asm_jmp(GRJIT_Asm * a, GRJIT_Label label);
void grjit_asm_jcc(GRJIT_Asm * a, GRJIT_Cond cond, GRJIT_Label label);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_SRC_X86_64_ASM_INTERNAL_H */
