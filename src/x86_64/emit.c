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
 * The baseline emitter: one operation at a time, every value in its frame
 * slot, fixed caller-saved scratch registers (rax, rcx, rdx, rsi, rdi, r8,
 * r9). No GC reference is ever left in a callee-saved register, which is
 * AD-17's rule held by construction rather than checked.
 *
 * The register roles are fixed: `rax` and `rcx` carry the operands of an
 * operation, `rdx` the `out` pointer in the exits, and `rdi`, `rsi`, `rdx`,
 * `rcx`, `r8`, `r9` the arguments of a helper call (SysV).
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "emit_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/compiled.h>

#include <stdlib.h>
#include <string.h>

/* The registers of each x86-64 convention (emit_internal.h). The C ABI's carry a
 * helper's arguments: SysV all six of the IR's; Win64 the first four, the fifth
 * and sixth going at [rsp + 32] and [rsp + 40], above the callee's shadow space.
 * The internal convention between compiled functions has the same registers as
 * the C ABI of its target for its arguments, with the context in the caller-saved
 * `r10` on both. It differs where the C ABI is a burden: no shadow space between
 * compiled functions on Win64, the callee pops the stack arguments on both, and
 * only caller-saved registers are used (Win64's `rbx`, `rdi`, `rsi`, `r12`-`r15`
 * and `xmm6`-`xmm15` are callee-saved there, so the SysV tail-call copy through
 * `rdi` and the SysV cause in `rsi` are `rcx` and `rdx` here). */
static const GRJIT_Reg sysv_c_args[6] = {
    GRJIT_RDI, GRJIT_RSI, GRJIT_RDX, GRJIT_RCX, GRJIT_R8, GRJIT_R9};
static const GRJIT_Reg win64_c_args[4] = {GRJIT_RCX, GRJIT_RDX, GRJIT_R8, GRJIT_R9};
static const GRJIT_X86Internal sysv_internal = {
    .args = sysv_c_args,
    .reg_args = GRJIT_SYSV_INTERNAL_REG_ARGS,
    .ctx = GRJIT_R10,
    .scratch = GRJIT_R11,
    .tail_copy = GRJIT_RDI,
    .status = GRJIT_RDX,
    .c_ret2 = GRJIT_RDX,
};
const GRJIT_X86Abi grjit_x86_abi_sysv = {
    .c_args = sysv_c_args,
    .c_reg_args = 6,
    .shadow_bytes = 0,
    .hidden_pair = false,
    .internal = &sysv_internal,
};
static const GRJIT_X86Internal win64_internal = {
    .args = win64_c_args,
    .reg_args = GRJIT_WIN64_INTERNAL_REG_ARGS,
    .ctx = GRJIT_R10,
    .scratch = GRJIT_R11,
    .tail_copy = GRJIT_RCX,
    .status = GRJIT_RDX,
    .c_ret2 = GRJIT_RDX,
};
const GRJIT_X86Abi grjit_x86_abi_win64 = {
    .c_args = win64_c_args,
    .c_reg_args = 4,
    .shadow_bytes = 32,
    .hidden_pair = true,
    .internal = &win64_internal,
};

/* Loads an operand into a register: a register's slot, or an immediate. */
static void load_operand(GRJIT_Emit * e, GRJIT_Reg r, const GRJIT_Operand * o) {
  if (o->kind == GRJIT_OPERAND_VREG) {
    grjit_asm_load64(&e->as, r, GRJIT_RBP, grjit_emit_slot(o->vreg));
  } else {
    grjit_asm_mov_ri(&e->as, r, (uint64_t)o->imm);
  }
}

void grjit_emit_operand_load(GRJIT_Emit * e, GRJIT_Reg r, const GRJIT_Operand * o) {
  load_operand(e, r, o);
}

static void store_result(GRJIT_Emit * e, GRJIT_VReg dst, GRJIT_Reg r) {
  grjit_asm_store64(&e->as, GRJIT_RBP, grjit_emit_slot(dst), r);
}

/* A register kept zero-extended in its word: an i32, and an f32 for the same reason (its bits
 * are 32; a word that arrives from a caller, a callee or memory is cut to them). */
static bool dst_is_i32_reg(const GRJIT_Emit * e, GRJIT_VReg v) {
  return e->c.f->vregs[v].type == GRJIT_TYPE_I32 || e->c.f->vregs[v].type == GRJIT_TYPE_F32;
}

static GRJIT_Cond cmp_cond(GRJIT_Cmp c) {
  switch (c) {
    case GRJIT_CMP_EQ:
      return GRJIT_COND_E;
    case GRJIT_CMP_NE:
      return GRJIT_COND_NE;
    case GRJIT_CMP_LT:
      return GRJIT_COND_L;
    case GRJIT_CMP_LE:
      return GRJIT_COND_LE;
    case GRJIT_CMP_GT:
      return GRJIT_COND_G;
    case GRJIT_CMP_GE:
      return GRJIT_COND_GE;
    case GRJIT_CMP_ULT:
      return GRJIT_COND_B;
    case GRJIT_CMP_ULE:
      return GRJIT_COND_BE;
    case GRJIT_CMP_UGT:
      return GRJIT_COND_A;
    case GRJIT_CMP_UGE:
    case GRJIT_CMP_COUNT:
      break;
  }
  return GRJIT_COND_AE;
}

static void emit_load(GRJIT_Emit * e, const GRJIT_Op * op, bool sign) {
  GRJIT_Asm * a = &e->as;
  if (op->index.kind == GRJIT_OPERAND_VREG) {
    load_operand(e, GRJIT_RAX, &op->index);
    for (unsigned s = 1; s < op->scale; s <<= 1) {
      grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RAX);
    }
    load_operand(e, GRJIT_R11, &op->a);
    grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_R11, GRJIT_RAX);
    grjit_asm_mov_rr(a, GRJIT_RCX, GRJIT_R11);
  } else {
    load_operand(e, GRJIT_RCX, &op->a);
  }
  switch (op->width) {
    case 8:
      (sign ? grjit_asm_load8s : grjit_asm_load8u)(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
    case 16:
      (sign ? grjit_asm_load16s : grjit_asm_load16u)(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
    case 32:
      (sign ? grjit_asm_load32s : grjit_asm_load32u)(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
    default:
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RCX, op->disp);
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

static void emit_store(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  if (op->index.kind == GRJIT_OPERAND_VREG) {
    load_operand(e, GRJIT_RCX, &op->index);
    for (unsigned s = 1; s < op->scale; s <<= 1) {
      grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_RCX, GRJIT_RCX);
    }
    load_operand(e, GRJIT_R11, &op->a);
    grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_R11, GRJIT_RCX);
    grjit_asm_mov_rr(a, GRJIT_RCX, GRJIT_R11);
  } else {
    load_operand(e, GRJIT_RCX, &op->a);
  }
  load_operand(e, GRJIT_RAX, &op->b);
  switch (op->width) {
    case 8:
      grjit_asm_store8(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
    case 16:
      grjit_asm_store16(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
    case 32:
      grjit_asm_store32(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
    default:
      grjit_asm_store64(a, GRJIT_RCX, op->disp, GRJIT_RAX);
      break;
  }
}

static void emit_binary(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  load_operand(e, GRJIT_RAX, &op->a);
  load_operand(e, GRJIT_RCX, &op->b);
  GRJIT_OpKind kind = op->kind;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 1
  /* Planted defect 1 (tests only): a logical and an arithmetic right shift
   * trade places. The differential must catch it. */
  if (kind == GRJIT_OP_SHR) {
    kind = GRJIT_OP_SAR;
  } else if (kind == GRJIT_OP_SAR) {
    kind = GRJIT_OP_SHR;
  }
#endif
  switch (kind) {
    case GRJIT_OP_ADD:
      grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_SUB:
      grjit_asm_alu_rr(a, GRJIT_ALU_SUB, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_MUL:
      grjit_asm_imul_rr(a, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_AND:
      grjit_asm_alu_rr(a, GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_OR:
      grjit_asm_alu_rr(a, GRJIT_ALU_OR, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_XOR:
      grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_SHL:
      grjit_asm_shl_cl(a, GRJIT_RAX);
      break;
    case GRJIT_OP_SHR:
      grjit_asm_shr_cl(a, GRJIT_RAX);
      break;
    default:
      grjit_asm_sar_cl(a, GRJIT_RAX);
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

/* ---- Float arguments of a helper call (SysV; spec-runtime-float, story 3) ---------------------
 *
 * (Win64 numbers its four register slots by position, not by class: see emit_call.)
 * The integer-class arguments take `rdi rsi rdx rcx r8 r9` in order among themselves, the float
 * ones `xmm0` to `xmm7` in order among themselves. An f32 is in the low 32 bits of its register,
 * an f64 in its 64; a typed immediate goes through `rax`, which the call loads over afterwards. */

static bool operand_is_float(const GRJIT_Emit * e, const GRJIT_Operand * o) {
  if (o->kind == GRJIT_OPERAND_F32_IMM || o->kind == GRJIT_OPERAND_F64_IMM) {
    return true;
  }
  if (o->kind != GRJIT_OPERAND_VREG) {
    return false;
  }
  const GRJIT_Type t = e->c.f->vregs[o->vreg].type;
  return grjit_type_is_float(t);
}

static bool operand_is_double(const GRJIT_Emit * e, const GRJIT_Operand * o) {
  return o->kind == GRJIT_OPERAND_F64_IMM ||
         (o->kind == GRJIT_OPERAND_VREG && e->c.f->vregs[o->vreg].type == GRJIT_TYPE_F64);
}

static bool call_has_float(const GRJIT_Emit * e, const GRJIT_Op * op) {
  for (size_t i = 0; i < op->arg_count; i++) {
    if (operand_is_float(e, &op->args[i])) {
      return true;
    }
  }
  if (op->dst != GRJIT_NO_VREG) {
    const GRJIT_Type t = e->c.f->vregs[op->dst].type;
    return grjit_type_is_float(t);
  }
  return false;
}

static void emit_float_arguments(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  unsigned integer = 0;
  unsigned floating = 0;
  for (size_t i = 0; i < op->arg_count; i++) {
    const GRJIT_Operand * o = &op->args[i];
    if (!operand_is_float(e, o)) {
      load_operand(e, C_ARG(e, integer++), o);
      continue;
    }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 49
    /* Planted defect 49 (tests only): the float and integer arguments are numbered jointly, as
     * Win64 numbers them, so the second float of (i64, f64, i64, f32) goes to xmm3. */
    const GRJIT_Xmm x = (GRJIT_Xmm)(i < 8 ? i : 7);
    (void)floating;
#elif defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 48
    /* Planted defect 48 (tests only): the second float argument goes to the wrong register,
     * xmm2 and not xmm1. */
    const GRJIT_Xmm x = (GRJIT_Xmm)(floating == 1 ? 2 : floating);
    floating++;
#else
    const GRJIT_Xmm x = (GRJIT_Xmm)floating++;
#endif
    const bool dbl = operand_is_double(e, o);
    if (o->kind == GRJIT_OPERAND_VREG) {
      grjit_asm_movs_load(a, x, GRJIT_RBP, grjit_emit_slot(o->vreg), dbl);
    } else {
      grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)o->imm);
      grjit_asm_movd_xr(a, x, GRJIT_RAX, dbl);
    }
  }
}

static void emit_call(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  /* In a callable function every call that can reach a GC point records where
   * the walk starts first (AD-28), so a collection under the helper sees this
   * frame and the compiled frames below it. It uses rax and rcx, which the
   * arguments are loaded over afterwards. */
  GRJIT_Label walk_ret = 0;
  bool record = e->c.callable && op->attr == GRJIT_CALL_GC_POINT;
  if (record) {
    walk_ret = grjit_asm_label(a);
    grjit_emit_store_walk_cell(e, walk_ret);
  }
  if (e->win64) {
    /* The stack arguments first, through rax: they are stores to the outgoing
     * area, and the register arguments are loads straight from slots, so no
     * order between them can clobber an operand. Microsoft x64 numbers the four
     * register slots by position: argument i is `rcx rdx r8 r9` for an integer and
     * `xmm0`-`xmm3` for a float, the slot being the same number for both (so
     * (i64, f64, i64, f32) is rcx, xmm1, r8, xmm3). The fifth and sixth arguments are
     * stack words above the 32 bytes of shadow space, a float as its bit word whatever
     * its class (a callee reads an f32 from the low 32 bits). A helper is non-variadic:
     * a variadic callee would want a float repeated in the integer register, which is
     * unsupported here as on SysV. */
    for (size_t i = e->abi->c_reg_args; i < op->arg_count; i++) {
      load_operand(e, GRJIT_RAX, &op->args[i]);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 90
      /* Planted defect 90 (tests only): a stack-passed argument is stored without the shadow space's
       * offset, on top of the callee's shadow words. */
      grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - e->abi->c_reg_args)), GRJIT_RAX);
#else
      grjit_asm_store64(a, GRJIT_RSP,
          (int32_t)(e->abi->shadow_bytes + 8 * (i - e->abi->c_reg_args)), GRJIT_RAX);
#endif
    }
    unsigned integer = 0;
    unsigned floating = 0;
    for (size_t i = 0; i < op->arg_count && i < e->abi->c_reg_args; i++) {
      const GRJIT_Operand * o = &op->args[i];
      if (!operand_is_float(e, o)) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 89
        /* Planted defect 89 (tests only): the arguments are numbered by class, as SysV numbers them
         * (integers among themselves, floats among themselves), not by position. */
        load_operand(e, C_ARG(e, integer++), o);
        continue;
#else
        load_operand(e, C_ARG(e, i), o);
        continue;
#endif
      }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 89
      const GRJIT_Xmm x = (GRJIT_Xmm)floating++;
#else
      const GRJIT_Xmm x = (GRJIT_Xmm)i;
      (void)floating;
#endif
      const bool dbl = operand_is_double(e, o);
      if (o->kind == GRJIT_OPERAND_VREG) {
        grjit_asm_movs_load(a, x, GRJIT_RBP, grjit_emit_slot(o->vreg), dbl);
      } else {
        grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)o->imm);
        grjit_asm_movd_xr(a, x, GRJIT_RAX, dbl);
      }
    }
    (void)integer;
  } else if (call_has_float(e, op)) {
    emit_float_arguments(e, op);
  } else {
    for (size_t i = 0; i < op->arg_count; i++) {
      load_operand(e, C_ARG(e, i), &op->args[i]);
    }
  }
  grjit_asm_mov_ri(a, GRJIT_RAX, op->address);
  grjit_asm_call_r(a, GRJIT_RAX);
  if (record) {
    grjit_asm_bind(a, walk_ret);
  }
  if (op->attr == GRJIT_CALL_GC_POINT) {
    /* The return address is the site. */
    const GRJIT_FrameState * s = &e->c.f->states[op->state];
    grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), op->site_kind,
        s->identity, e->c.live_cursor++, op->state);
  }
  if (op->dst != GRJIT_NO_VREG) {
    const GRJIT_Type result = e->c.f->vregs[op->dst].type;
    if (grjit_type_is_float(result)) {
      /* A float comes back in xmm0: an f32 in its low 32 bits (whatever is above them is not
       * ours), an f64 in its 64. */
      grjit_asm_movd_rx(a, GRJIT_RAX, GRJIT_XMM0, result == GRJIT_TYPE_F64);
    } else if (dst_is_i32_reg(e, op->dst)) {
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX); /* a helper's 32-bit result: its upper half is not ours */
    }
    store_result(e, op->dst, GRJIT_RAX);
  }
}

/* Finds the callee's entry for a call or a tail call and leaves it in `rax`
 * and in the entry-save slot, or goes to the returned exit label: through the
 * slot (one compare; the compile hook for an empty slot; an exit for a refused
 * one) or through the code pointer (the registered, not retired, tagged check).
 * `live` is the first of the operation's liveness sites and `exit_live` the
 * one the exit's stub is made from. */
static GRJIT_Label emit_dispatch(
    GRJIT_Emit * e, const GRJIT_Op * op, size_t live, size_t exit_live) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const bool slot = op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_SLOT;
  const size_t n = op->arg_count;
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(GRJIT_SHAPE_REGS(f, e->c.shape), e->c.shape);
  GRJIT_Label exit = grjit_asm_label(a);
  GRJIT_Pending p;
  memset(&p, 0, sizeof p);
  p.op = op;
  if (slot) {
    GRJIT_Label slow = grjit_asm_label(a);
    GRJIT_Label back = grjit_asm_label(a);
    if (op->address != 0) grjit_asm_mov_ri(a, GRJIT_RAX, op->address);
    else load_operand(e, GRJIT_RAX, &op->a);
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RAX, 0);
    /* One compare tells compiled code (above one) from empty (zero) and from
     * refused (one). */
    grjit_asm_cmp_ri(a, GRJIT_RAX, (int32_t)GRCORE_ENTRY_REFUSED);
    grjit_asm_jcc(a, GRJIT_COND_BE, slow);
    grjit_asm_bind(a, back);
    p.kind = GRJIT_PENDING_CALL_SLOW;
    p.entry = slow;
    p.back = back;
    p.exit = exit;
    p.live_index = live;
    grjit_emit_add_pending(&e->c, &p);
  } else {
    load_operand(e, GRJIT_RAX, &op->a);
    grjit_asm_store64(a, GRJIT_RBP, entry_slot, GRJIT_RAX);
    /* The target must be the internal entry of code registered in this context:
     * an address that is not is never entered. */
    grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, GRJIT_SLOT_CTX);
    grjit_asm_mov_rr(a, C_ARG(e, 1), GRJIT_RAX);
    grjit_asm_mov_ri(a, C_ARG(e, 2), op->callee);
    grjit_asm_mov_ri(a, C_ARG(e, 3), GRJIT_CALL_SIGNATURE(n,
        op->kind == GRJIT_OP_TAIL_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_PTR
            ? op->callee_result_count : op->result_count));
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)grjit_call_target_ok);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_E, exit);
  }
  memset(&p, 0, sizeof p);
  p.kind = GRJIT_PENDING_CALL_EXIT;
  p.entry = exit;
  p.op = op;
  p.live_index = exit_live;
  grjit_emit_add_pending(&e->c, &p);
  if (slot) {
    grjit_asm_store64(a, GRJIT_RBP, entry_slot, GRJIT_RAX);
  }
  return exit;
}

/* The arguments of a call or a tail call, into the arguments area. */
static void emit_stage_arguments(GRJIT_Emit * e, const GRJIT_Op * op) {
  const GRJIT_Function * f = e->c.f;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  for (size_t i = 0; i < op->arg_count; i++) {
    load_operand(e, GRJIT_RAX, &op->args[i]);
    grjit_asm_store64(&e->as, GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i), GRJIT_RAX);
  }
}

/* A call to another compiled function (AD-28), through an entry slot or a code
 * pointer. In order: find the entry (the slot's word, or the checked pointer);
 * copy the arguments into the frame's arguments area, where the push hook reads
 * them and a moving collector updates them; push the callee's guest frame (the
 * frame-push GC point); call through the internal convention; test the status
 * for DEOPTED; store the result; pop. Every way out before the call is an
 * exit through the exit state; every way out after it is DEOPTED. */
static void emit_guest_call(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_FrameState * st = &f->states[op->state];
  const size_t n = op->arg_count;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(regs, e->c.shape);
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += 3; /* the push, the call and the exit */

  GRJIT_Label exit = emit_dispatch(e, op, live, live + 2);
  emit_stage_arguments(e, op);

  /* The push: the callee's guest frame, counted as the interpreter counts it. */
  GRJIT_Label push_ret = grjit_asm_label(a);
  grjit_emit_store_walk_cell(e, push_ret);
  grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, GRJIT_SLOT_CTX);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    grjit_asm_load64(a, C_ARG(e, 1), GRJIT_RBP, GRJIT_SLOT_ENV);
  else
    load_operand(e, C_ARG(e, 1), &op->environment);
  grjit_asm_mov_ri(a, C_ARG(e, 2), op->callee);
  grjit_asm_lea(a, C_ARG(e, 3), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, 0));
  if (e->win64) {
    grjit_asm_mov_ri(a, GRJIT_RAX, n);
    grjit_asm_store64(a, GRJIT_RSP, 32, GRJIT_RAX);
    grjit_asm_mov_ri(a, GRJIT_RAX, op->result_count);
    grjit_asm_store64(a, GRJIT_RSP, 40, GRJIT_RAX);
  } else {
    grjit_asm_mov_ri(a, C_ARG(e, 4), n);
    grjit_asm_mov_ri(a, C_ARG(e, 5), op->result_count);
  }
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.push);
  grjit_asm_call_r(a, GRJIT_RAX);
  grjit_asm_bind(a, push_ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_asm_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, exit);

  /* The call. The stack arguments are pushed just before it and popped by the
   * callee, which keeps the stack 16-aligned (the area is whole 16-byte units). */
  const unsigned ireg = e->abi->internal->reg_args;
  const size_t call_args = n + (op->result_count > 1 ? 1u : 0u);
  const uint32_t area = grjit_stack_arg_bytes(call_args, ireg);
  if (area != 0) {
    grjit_asm_sub_rsp(a, area);
    for (size_t i = ireg; i < n; i++) {
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
      grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - ireg)), GRJIT_RAX);
    }
    if (op->result_count > 1 && n >= ireg) {
      grjit_asm_lea(a, GRJIT_RAX, GRJIT_RBP,
          GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, e->c.shape.args_area));
      grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (n - ireg)), GRJIT_RAX);
    }
  }
  for (size_t i = 0; i < n && i < ireg; i++) {
    grjit_asm_load64(a, IARG(e, i), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
  }
  if (op->result_count > 1 && n < ireg) {
    grjit_asm_lea(a, IARG(e, n), GRJIT_RBP,
        GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, e->c.shape.args_area));
  }
  grjit_asm_load64(a, ICTX(e), GRJIT_RBP, GRJIT_SLOT_CTX);
  /* The internal guest ABI carries a separate callee environment in r11.  The
   * current call target inherits the caller environment; explicit target
   * environments are staged by the call operation. */
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    grjit_asm_load64(a, GRJIT_R11, GRJIT_RBP, GRJIT_SLOT_ENV);
  else
    load_operand(e, GRJIT_R11, &op->environment);
  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, entry_slot);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* The return address is the site: this frame, with the callee running. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_asm_size(a),
      GRCORE_SITE_GC_POINT_CALL, st->identity, live + 1, op->state, op);
  if (op->landing_target != GRJIT_NO_BLOCK) {
    grjit_emit_add_exception_site(&e->c, (uint32_t)grjit_asm_size(a), op->landing_target);
    GRJIT_Label not_exception = grjit_asm_label(a);
    GRJIT_Label propagate_exception = grjit_asm_label(a);
    grjit_asm_cmp_ri(a, ISTATUS(e), (int32_t)GRJIT_STATUS_EXCEPTION);
    grjit_asm_jcc(a, GRJIT_COND_NE, not_exception);
    grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, GRJIT_SLOT_CTX);
    grjit_asm_load64(a, C_ARG(e, 1), GRJIT_RBP, GRJIT_SLOT_ENV);
    grjit_asm_mov_ri(a, C_ARG(e, 2), op->callee);
    grjit_asm_mov_ri(a, C_ARG(e, 3), op->landing_target);
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.catch_exception);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, propagate_exception);
    grjit_asm_mov_ri(a, ISTATUS(e), GRJIT_STATUS_RETURNED);
    grjit_asm_jmp(a, e->blocks[op->landing_target]);
    grjit_asm_bind(a, propagate_exception);
    grjit_asm_mov_ri(a, ISTATUS(e), GRJIT_STATUS_EXCEPTION);
    grjit_asm_jmp(a, e->ret_propagate);
    grjit_asm_bind(a, not_exception);
  }
  grjit_asm_test_rr(a, ISTATUS(e), ISTATUS(e));
  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_propagate);
  if (op->result_count != 0) {
    if (dst_is_i32_reg(e, op->dst)) {
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX); /* the callee returned a word; an i32 is its low half */
    }
    store_result(e, op->results[0], GRJIT_RAX);
    for (size_t i = 1; i < op->result_count; i++) {
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP,
          GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots,
              e->c.shape.args_area + i - 1));
      if (dst_is_i32_reg(e, op->results[i])) grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
      store_result(e, op->results[i], GRJIT_RAX);
    }
  }
  /* The call is complete: pop the callee's guest frame. */
  grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.pop);
  grjit_asm_call_r(a, GRJIT_RAX);
}

/* A tail call (AD-28): the callee replaces this function's frame, native and
 * guest, and returns to this function's caller. In order: find the entry (as a
 * call does); the arguments into the arguments area; the engine's `tail` hook,
 * which replaces the guest frame as one step and is the frame-push GC point;
 * then, with no GC point until the jump, the replacement of the native frame.
 *
 * Let `ra` be the address of this frame's return address (`rbp + 8`), `in_A` the
 * bytes of stack arguments this function takes and `in_T` those the callee
 * takes (whole 16-byte units). `R = ra + 8 + in_A` is where the original caller
 * expects `rsp` after the return, so the callee's `ret in_T` must leave it
 * there: the return address goes to `ra' = R - 8 - in_T`, and the callee's stack
 * arguments to `[ra' + 8, R)`. The callee's prologue then finds exactly what a
 * call leaves. `rsp` is moved last, and nothing below it is read after, so no
 * signal can see a half-moved frame.
 *
 * The arguments area is the staging place: its end is at or below `ra'` (the
 * shape's `pad` makes it so), so no copy overwrites a source and the copies are
 * in any order. Everything that is read from this frame after the first write
 * (the context, the return address, the caller's saved base) is loaded first.
 * The walk is unchanged: after the jump the callee's frame holds this
 * function's saved base and return address, so it is one compiled frame beside
 * one guest frame, as any callee of the original caller is. */
static void emit_tail_call(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_FrameState * st = &f->states[op->state];
  const size_t n = op->arg_count;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(regs, e->c.shape);
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += 2; /* the hook and the exit */

  GRJIT_Label exit = emit_dispatch(e, op, live, live + 1);
  emit_stage_arguments(e, op);

  GRJIT_Label hook_ret = grjit_asm_label(a);
  grjit_emit_store_walk_cell(e, hook_ret);
  grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, GRJIT_SLOT_CTX);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    grjit_asm_load64(a, C_ARG(e, 1), GRJIT_RBP, GRJIT_SLOT_ENV);
  else
    load_operand(e, C_ARG(e, 1), &op->environment);
  grjit_asm_mov_ri(a, C_ARG(e, 2), op->callee);
  grjit_asm_lea(a, C_ARG(e, 3), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, 0));
  if (e->win64) {
    grjit_asm_mov_ri(a, GRJIT_RAX, n);
    grjit_asm_store64(a, GRJIT_RSP, 32, GRJIT_RAX);
    grjit_asm_mov_ri(a, GRJIT_RAX, op->callee_result_count);
    grjit_asm_store64(a, GRJIT_RSP, 40, GRJIT_RAX);
  } else {
    grjit_asm_mov_ri(a, C_ARG(e, 4), n);
    grjit_asm_mov_ri(a, C_ARG(e, 5), op->callee_result_count);
  }
  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.tail);
  grjit_asm_call_r(a, GRJIT_RAX);
  grjit_asm_bind(a, hook_ret);
  /* The hook is a frame-push GC point of this frame, whose guest frame is still
   * this function's: the state is the caller's, and the area is in the map. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_asm_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_jcc(a, GRJIT_COND_NE, exit);

#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 14
  /* Planted defect 14 (tests only): the tail call is made as a call, and its
   * frame kept: the answer is the same and the native stack grows with every tail
   * call, which no result shows and the stack pointer does. */
  {
    const unsigned ireg = e->abi->internal->reg_args;
    const uint32_t area_bytes = grjit_stack_arg_bytes(n + (e->c.shape.has_result_buffer ? 1u : 0u), ireg);
    if (area_bytes != 0) {
      grjit_asm_sub_rsp(a, area_bytes);
      for (size_t i = ireg; i < n; i++) {
        grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
        grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - ireg)), GRJIT_RAX);
      }
      if (e->c.shape.has_result_buffer) {
        grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_ARGS);
        grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (n - ireg)), GRJIT_RAX);
      }
    }
    for (size_t i = 0; i < n && i < ireg; i++) {
      grjit_asm_load64(a, IARG(e, i), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
    }
    if (e->c.shape.has_result_buffer && n < ireg) grjit_asm_load64(a, IARG(e, n), GRJIT_RBP, GRJIT_SLOT_ARGS);
    grjit_asm_load64(a, ICTX(e), GRJIT_RBP, GRJIT_SLOT_CTX);
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, entry_slot);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_emit_callable_epilogue(e);
    return;
  }
#endif
  /* The frame replacement. */
  const unsigned ireg = e->abi->internal->reg_args;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 32
  /* Planted defect 32 (tests only): the stack-argument copy goes through rdi, the SysV
   * register, which is callee-saved on Win64 and never restored. */
  const GRJIT_Reg copy = e->win64 ? GRJIT_RDI : e->abi->internal->tail_copy;
#else
  const GRJIT_Reg copy = e->abi->internal->tail_copy;
#endif
  const GRJIT_Reg scratch = ISCRATCH(e);
  const int64_t in_a = e->c.shape.incoming_bytes;
  const int64_t in_t = grjit_stack_arg_bytes(n + (e->c.shape.has_result_buffer ? 1u : 0u), ireg);
  const int32_t ra_new = (int32_t)(8 + in_a - in_t); /* `ra'`, from `rbp` */
  const bool buffer = e->c.shape.has_result_buffer;
  /* Everything this frame still holds that the copy can overwrite is read first, into a
   * register that nothing below writes before its use: the copy runs over the frame's slots
   * when the callee takes more stack arguments than this function does, and those include the
   * environment's and the result buffer's (the fixed slots) and the vregs the explicit
   * environment may name. The argument registers are free until the arguments are loaded
   * from the area, which is below the copy, so they carry the temporaries; the one that will
   * hold the result buffer's pointer (`IARG(n)`, when it is passed in a register) is not
   * among them. */
  GRJIT_Reg temps[8] = {GRJIT_RAX};
  size_t temp_count = 0;
  for (size_t j = 0; j < ireg; j++) {
    if (IARG(e, j) == copy || (buffer && n < ireg && j == n)) continue;
    temps[temp_count++] = IARG(e, j);
  }
  const GRJIT_Reg env_tmp = temps[0];
  const GRJIT_Reg buf_tmp = n >= ireg ? temps[1] : IARG(e, n);
  const GRJIT_Reg link_tmp = n >= ireg ? copy : temps[1];
  grjit_asm_load64(a, ICTX(e), GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_load64(a, scratch, GRJIT_RBP, 8);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    grjit_asm_load64(a, env_tmp, GRJIT_RBP, GRJIT_SLOT_ENV);
  else
    load_operand(e, env_tmp, &op->environment);
  if (buffer) grjit_asm_load64(a, buf_tmp, GRJIT_RBP, GRJIT_SLOT_ARGS);
  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, entry_slot);
  /* Stack arguments can cover the old saved-rbp word when the replacement
   * callee needs more stack space than this function. Save that link just
   * below the replacement rsp, where the callee's prologue will write it too. */
  grjit_asm_load64(a, link_tmp, GRJIT_RBP, 0);
  grjit_asm_store64(a, GRJIT_RBP, ra_new - 8, link_tmp);
  size_t copy_to = n;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 11
  /* Planted defect 11 (tests only): the last stack argument is not copied, so
   * the callee reads what the destination held. */
  if (copy_to > ireg) {
    copy_to--;
  }
#endif
  for (size_t i = ireg; i < copy_to; i++) {
    grjit_asm_load64(a, copy, GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
    grjit_asm_store64(a, GRJIT_RBP,
        (int32_t)(ra_new + 8 + 8 * (int64_t)(i - ireg)), copy);
  }
  if (buffer && n >= ireg) {
    grjit_asm_store64(a, GRJIT_RBP,
        (int32_t)(ra_new + 8 + 8 * (int64_t)(n - ireg)), buf_tmp);
  }
  grjit_asm_store64(a, GRJIT_RBP, ra_new, scratch);
  grjit_asm_mov_rr(a, GRJIT_R11, env_tmp);
  for (size_t i = 0; i < n && i < ireg; i++) {
    grjit_asm_load64(a, IARG(e, i), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
  }
  grjit_asm_lea(a, GRJIT_RSP, GRJIT_RBP, ra_new);
  grjit_asm_load64(a, GRJIT_RBP, GRJIT_RSP, -8);
  grjit_asm_jmp_r(a, GRJIT_RAX);
}

/* A call to a registered native (AD-28, AD-17; natives.h for the contract). In
 * order, and after it nothing is reordered:
 *
 *  1. the native-stack check: `lea rax, [rsp - (S + stack_bytes)]` against the
 *     context's limit word (zero means none); below it is an exit before the call,
 *     the native is not called and the interpreter makes the call itself. `S` is
 *     the stack-argument area, below;
 *  2. the walk start, first: the frame base and the return address of this call
 *     go in the context's cell before the area is made, before any argument is
 *     moved and before the call, so every GC point the native reaches sees this
 *     frame and the compiled frames below it;
 *  3. the arguments by the C ABI: with `N = 1 + n` words (the context first), the
 *     first six in `rdi, rsi, rdx, rcx, r8, r9`, the rest, `k = N - 6`, at
 *     `[rsp + 8 i]` in an area of `S = round_up_16(8 k)` bytes made by `sub rsp, S`,
 *     so `rsp` is 16-aligned at the call. Every operand is read from a frame slot
 *     or is an immediate, so the order of the loads cannot clobber one;
 *  4. the call, through `rax`; the caller pops (`add rsp, S`);
 *  5. the result is stored to `dst` before the status is looked at, so the state
 *     after the call sees it; with a status, a non-zero `rdx` is the exit through
 *     the state after the call;
 *  6. nothing is assumed preserved: the context, every operand and every live value
 *     is read from the frame again, and every caller-saved register is clobbered.
 *
 * The return address is the call's site: this frame in the call's state, with the
 * stack map of what is live after the call (the result excluded). The arguments
 * are not mapped: a native receives raw words, and what it holds across a GC point
 * it protects itself. */
static void emit_native_call(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_NativeDesc * d = grjit_native_table_get(f->natives, op->native);
  if (d == NULL || d->param_count != op->arg_count) {
    /* The verifier refused this already; a descriptor cannot change under a
     * verified function (the table is append-only), so this is the library's own
     * invariant. */
    e->c.error = GRJIT_ERR_INTERNAL;
    return;
  }
  const bool status = (d->flags & GRJIT_NATIVE_STATUS) != 0;
  const size_t n = op->arg_count;
  const size_t words = n + 1; /* the context first */
  /* A native with a status returns its 16-byte pair through a hidden pointer on Win64
   * (the first C word, so the context is the second), in `rax:rdx` on SysV. */
  const bool hidden = status && e->abi->hidden_pair;
  const unsigned creg = e->abi->c_reg_args;
  const size_t cwords = words + (hidden ? 1u : 0u);
  const size_t stack_words = cwords > creg ? cwords - creg : 0;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 31
  /* Planted defect 31 (tests only): the area has no shadow space, so the stack words are where
   * SysV puts them, at the bottom of the area, and a native that spills its register arguments to
   * their home space writes over the first of them (and reads its own at the wrong place). */
  const uint32_t shadow = 0;
#else
  const uint32_t shadow = e->abi->shadow_bytes;
#endif
  /* The IR argument that is the first C stack word, and where the buffer is (after
   * the stack words, above the callee's shadow space). */
  const size_t first_stack = creg - 1 - (hidden ? 1u : 0u);
  const uint32_t buffer_at = shadow + (uint32_t)(8 * stack_words);
  uint32_t area = (uint32_t)((shadow + stack_words * 8 + (hidden ? 16 : 0) + 15) / 16 * 16);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 17
  /* Planted defect 17 (tests only): the stack-argument area is not rounded to
   * sixteen bytes, so an odd count of stack words leaves the stack misaligned at
   * the call. */
  area = (uint32_t)(stack_words * 8);
#endif
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += status ? 3 : 2; /* the call, the exit before it, the status exit */
  const GRJIT_FrameState * st = &f->states[op->state];

  /* 1. The check. */
  GRJIT_Label exit = grjit_asm_label(a);
  {
    GRJIT_Pending p;
    memset(&p, 0, sizeof p);
    p.kind = GRJIT_PENDING_NATIVE_EXIT;
    p.entry = exit;
    p.op = op;
    p.live_index = live + 1;
    grjit_emit_add_pending(&e->c, &p);
  }
  uint64_t need = (uint64_t)area + d->stack_bytes;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 18
  /* Planted defect 18 (tests only): the check leaves out the stack-argument area. */
  need = d->stack_bytes;
#endif
  grjit_asm_lea(a, GRJIT_RAX, GRJIT_RSP, -(int32_t)need);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_cmp_rm(a, GRJIT_RAX, GRJIT_RCX, (int32_t)e->c.native_limit_offset);
  grjit_asm_jcc(a, GRJIT_COND_B, exit);

  /* 2. The walk start, before anything moves. */
  GRJIT_Label ret = grjit_asm_label(a);
#if defined(GRJIT_TEST_PLANT_BUG) && (GRJIT_TEST_PLANT_BUG == 15 || GRJIT_TEST_PLANT_BUG == 36)
  /* Planted defects 15 and 36 (tests only): the walk start is stored after the
   * call, so a collection the native triggers finds the previous call's. */
#else
  grjit_emit_store_walk_cell(e, ret);
#endif

  /* 3. The arguments. */
  if (area != 0) {
    grjit_asm_sub_rsp(a, area);
  }
  for (size_t i = first_stack; i < n; i++) {
    /* IR argument i is C argument i + 1 (i + 2 behind a hidden pointer). */
    load_operand(e, GRJIT_RAX, &op->args[i]);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 19
    /* Planted defect 19 (tests only): each stack argument is one word too high. */
    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(shadow + 8 * (i - first_stack) + 8), GRJIT_RAX);
#else
    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(shadow + 8 * (i - first_stack)), GRJIT_RAX);
#endif
  }
  const unsigned ctx_arg = hidden ? 1u : 0u;
  if (hidden) {
    grjit_asm_lea(a, C_ARG(e, 0), GRJIT_RSP, (int32_t)buffer_at);
  }
  grjit_asm_load64(a, C_ARG(e, ctx_arg), GRJIT_RBP, GRJIT_SLOT_CTX);
  for (size_t i = 0; i < n && i + 1 + ctx_arg < creg; i++) {
    load_operand(e, C_ARG(e, i + 1 + ctx_arg), &op->args[i]);
  }

  /* 4. The call; the return address is the site. */
  grjit_asm_mov_ri(a, GRJIT_RAX, d->address);
  grjit_asm_call_r(a, GRJIT_RAX);
#if defined(GRJIT_TEST_PLANT_BUG) && (GRJIT_TEST_PLANT_BUG == 15 || GRJIT_TEST_PLANT_BUG == 36)
  /* The late store, through registers that leave the native's rax and rdx alone. */
  grjit_asm_lea_rip(a, GRJIT_R8, ret);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, GRJIT_SLOT_CTX);
  grjit_asm_store64(a, GRJIT_RCX, (int32_t)e->c.walk_cell_offset, GRJIT_RBP);
  grjit_asm_store64(a, GRJIT_RCX, (int32_t)e->c.walk_cell_offset + 8, GRJIT_R8);
#endif
  grjit_asm_bind(a, ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GC_POINT_CALL,
      st->identity, live, op->state, op);
  if (hidden) {
    /* The pair is in the buffer, read before `rsp` moves back: the value, and the 32
     * bits of the status. The half of the status word above it (`reserved`) is
     * padding a native may leave as it likes (natives.h), and is never read. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 30
    /* Planted defect 30 (tests only): the pair is read from `rax:rdx`, where SysV
     * returns it and Win64 does not. */
    grjit_asm_mov_rr(a, GRJIT_RDX, GRJIT_RDX);
#else
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RSP, (int32_t)buffer_at);
    grjit_asm_load32u(a, GRJIT_RDX, GRJIT_RSP, (int32_t)buffer_at + 8);
#endif
  }
  if (area != 0) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 16
    /* Planted defect 16 (tests only): the caller does not pop the stack
     * arguments, so rsp drifts by their area with every call. */
#else
    grjit_asm_add_rsp(a, area);
#endif
  }

  /* 5. The result before the status; a non-zero status leaves. */
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_RAX);
  }
  if (status) {
    GRJIT_Label leave = grjit_asm_label(a);
    GRJIT_Pending p;
    memset(&p, 0, sizeof p);
    p.kind = GRJIT_PENDING_NATIVE_STATUS;
    p.entry = leave;
    p.op = op;
    p.live_index = live + 2;
    grjit_emit_add_pending(&e->c, &p);
    /* The status is the 32 bits of edx: the half above is padding a native may leave as it
     * likes (natives.h), so it is cleared and never read. (Behind a hidden pointer it was
     * loaded as 32 bits already.) */
    const GRJIT_Reg second = e->abi->internal->c_ret2;
    if (!hidden) {
      grjit_asm_mov32_rr(a, second, second);
    }
    grjit_asm_test_rr(a, second, second);
    grjit_asm_jcc(a, GRJIT_COND_NE, leave);
  }
}

/* ---- The integer operations of numeric-operations.md (spec-runtime-float, story 2) ------------
 *
 * The width of an operation is its destination's: an i32 operation is made of real 32-bit
 * instructions (the hardware masks a shift or rotate count to five bits, `div` divides
 * `edx:eax`, and every 32-bit write clears the upper half of its register), so the result
 * is zero-extended in its slot by construction and no mask follows it. Nothing here shares
 * a line with the code above. `clz`, `ctz` and `popcnt` are the x86-64-v1 sequences: the
 * LZCNT/BMI1/POPCNT forms and the feature mask are story 4's, and these stay as their
 * fallback. A division never traps here: the verifier has refused it without its guards,
 * and `rem_s` answers `MIN % -1` without reaching `idiv`, which faults there. */

static bool dst_is_i32(const GRJIT_Emit * e, const GRJIT_Op * op) {
  return op->dst != GRJIT_NO_VREG && e->c.f->vregs[op->dst].type == GRJIT_TYPE_I32;
}

static bool operand_is_i32(const GRJIT_Emit * e, const GRJIT_Operand * o) {
  return o->kind == GRJIT_OPERAND_VREG && e->c.f->vregs[o->vreg].type == GRJIT_TYPE_I32;
}

/* ADD .. SAR on i32 operands. */
static void emit_binary32(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  GRJIT_Operand left = op->a;
  GRJIT_Operand right = op->b;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 42
  /* Planted defect 42 (tests only): the operands of an i32 subtraction are swapped. */
  if (op->kind == GRJIT_OP_SUB) {
    left = op->b;
    right = op->a;
  }
#endif
  load_operand(e, GRJIT_RAX, &left);
  load_operand(e, GRJIT_RCX, &right);
  switch (op->kind) {
    case GRJIT_OP_ADD:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 41
      /* Planted defect 41 (tests only): the addition is a 64-bit one, so a carry out of
       * bit 31 lands in the upper half, which an i32 must keep zero. */
      grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX);
#else
      grjit_asm_alu32_rr(a, GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX);
#endif
      break;
    case GRJIT_OP_SUB:
      grjit_asm_alu32_rr(a, GRJIT_ALU_SUB, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_MUL:
      grjit_asm_imul32_rr(a, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_AND:
      grjit_asm_alu32_rr(a, GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_OR:
      grjit_asm_alu32_rr(a, GRJIT_ALU_OR, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_XOR:
      grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RCX);
      break;
    case GRJIT_OP_SHL:
    case GRJIT_OP_SHR:
    case GRJIT_OP_SAR: {
      const GRJIT_ShiftOp sh = op->kind == GRJIT_OP_SHL   ? GRJIT_SHIFT_SHL
                               : op->kind == GRJIT_OP_SHR ? GRJIT_SHIFT_SHR
                                                          : GRJIT_SHIFT_SAR;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 40
      /* Planted defect 40 (tests only): the count is masked to 64, not to 32: a 64-bit
       * shift, cut to 32 bits afterwards. */
      grjit_asm_shift_cl(a, sh, GRJIT_RAX, true);
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
#else
      grjit_asm_shift_cl(a, sh, GRJIT_RAX, false);
#endif
      break;
    }
    default:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

/* Whether the arm that needs feature `f` is taken. It is taken only when `f` is in the set this
 * emit was given (`e->features`, the effective set of the compile: the host's less the caller's
 * mask), never from the host's own set and never from anything else, and taking it is recorded in
 * `features_used` at this point, so the record is what the bytes need and not what was permitted.
 * `lzcnt` and `tzcnt` do not fault on a CPU without them: they run as `bsr` and `bsf`. */
static bool take_feature(GRJIT_Emit * e, GRJIT_Features f) {
  GRJIT_Features have = e->features;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 62
  /* Planted defect 62 (tests only): the arm is chosen from the host's set, not the effective one,
   * so a mask changes neither the bytes nor (below) what is recorded. */
  have = grjit_features_detected();
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 63
  /* Planted defect 63 (tests only): the feature arm is never taken. */
  have = GRJIT_FEATURES_NONE;
#endif
  if ((have & f) == 0) {
    return false;
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 60
  /* Planted defect 60 (tests only): the recorded set is always empty. */
#else
  e->features_used |= f;
#endif
  return true;
}

/* `popcnt` without the POPCNT instruction: the parallel bit count. */
static void emit_popcnt_v1(GRJIT_Emit * e, bool wide) {
  GRJIT_Asm * a = &e->as;
#define ALU(op, d, s) ((wide) ? grjit_asm_alu_rr(a, op, d, s) : grjit_asm_alu32_rr(a, op, d, s))
#define MOV(d, s) ((wide) ? grjit_asm_mov_rr(a, d, s) : grjit_asm_mov32_rr(a, d, s))
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 66
  /* Planted defect 66 (tests only): a wrong mask constant in the v1 sequence, which a host with
   * POPCNT never runs unless the feature is masked off. */
  const uint64_t m1 = wide ? UINT64_C(0x5555555555555554) : UINT64_C(0x55555554);
#else
  const uint64_t m1 = wide ? UINT64_C(0x5555555555555555) : UINT64_C(0x55555555);
#endif
  const uint64_t m2 = wide ? UINT64_C(0x3333333333333333) : UINT64_C(0x33333333);
  const uint64_t m4 = wide ? UINT64_C(0x0F0F0F0F0F0F0F0F) : UINT64_C(0x0F0F0F0F);
  const uint64_t h01 = wide ? UINT64_C(0x0101010101010101) : UINT64_C(0x01010101);
  /* x -= (x >> 1) & m1 */
  MOV(GRJIT_RCX, GRJIT_RAX);
  grjit_asm_shift_ri(a, GRJIT_SHIFT_SHR, GRJIT_RCX, 1, wide);
  grjit_asm_mov_ri(a, GRJIT_RDX, m1);
  ALU(GRJIT_ALU_AND, GRJIT_RCX, GRJIT_RDX);
  ALU(GRJIT_ALU_SUB, GRJIT_RAX, GRJIT_RCX);
  /* x = (x & m2) + ((x >> 2) & m2) */
  MOV(GRJIT_RCX, GRJIT_RAX);
  grjit_asm_shift_ri(a, GRJIT_SHIFT_SHR, GRJIT_RAX, 2, wide);
  grjit_asm_mov_ri(a, GRJIT_RDX, m2);
  ALU(GRJIT_ALU_AND, GRJIT_RCX, GRJIT_RDX);
  ALU(GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RDX);
  ALU(GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX);
  /* x = (x + (x >> 4)) & m4 */
  MOV(GRJIT_RCX, GRJIT_RAX);
  grjit_asm_shift_ri(a, GRJIT_SHIFT_SHR, GRJIT_RCX, 4, wide);
  ALU(GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX);
  grjit_asm_mov_ri(a, GRJIT_RDX, m4);
  ALU(GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RDX);
  /* x = (x * h01) >> (width - 8) */
  grjit_asm_mov_ri(a, GRJIT_RDX, h01);
  if (wide) {
    grjit_asm_imul_rr(a, GRJIT_RAX, GRJIT_RDX);
  } else {
    grjit_asm_imul32_rr(a, GRJIT_RAX, GRJIT_RDX);
  }
  grjit_asm_shift_ri(a, GRJIT_SHIFT_SHR, GRJIT_RAX, wide ? 56 : 24, wide);
#undef ALU
#undef MOV
}

/* CLZ, CTZ, POPCNT, EXT*_S, WRAP, EXTEND_S, EXTEND_U. */
static void emit_int_unary(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const bool wide = !dst_is_i32(e, op);
  load_operand(e, GRJIT_RAX, &op->a);
  switch (op->kind) {
    case GRJIT_OP_CLZ:
      if (take_feature(e, GRJIT_FEATURE_LZCNT)) {
        /* `lzcnt` gives the width for zero in the instruction itself: no cmov. */
        grjit_asm_lzcnt(a, GRJIT_RAX, GRJIT_RAX, wide);
        break;
      }
      /* width - 1 - bsr(x), which is bsr(x) ^ (width - 1); for zero, bsr leaves ZF set and
       * its destination unspecified, so cmov puts 2 * width - 1 there: (2w - 1) ^ (w - 1) = w. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 67
      /* Planted defect 67 (tests only): the zero case of the v1 `clz` is off by one in the value
       * the cmov supplies, so clz(0) is wrong; only the masked arm on a host with LZCNT runs it. */
      grjit_asm_mov_ri(a, GRJIT_RCX, wide ? 126 : 62);
#else
      grjit_asm_mov_ri(a, GRJIT_RCX, wide ? 127 : 63);
#endif
      grjit_asm_bsr(a, GRJIT_RAX, GRJIT_RAX, wide);
      grjit_asm_cmov(a, GRJIT_COND_E, GRJIT_RAX, GRJIT_RCX, wide);
      grjit_asm_mov_ri(a, GRJIT_RDX, wide ? 63 : 31);
      grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RDX);
      break;
    case GRJIT_OP_CTZ:
      if (take_feature(e, GRJIT_FEATURE_BMI1)) {
        /* `tzcnt` gives the width for zero in the instruction itself: no cmov. */
        grjit_asm_tzcnt(a, GRJIT_RAX, GRJIT_RAX, wide);
        break;
      }
      grjit_asm_mov_ri(a, GRJIT_RCX, wide ? 64 : 32);
      grjit_asm_bsf(a, GRJIT_RAX, GRJIT_RAX, wide);
      grjit_asm_cmov(a, GRJIT_COND_E, GRJIT_RAX, GRJIT_RCX, wide);
      break;
    case GRJIT_OP_POPCNT:
      if (take_feature(e, GRJIT_FEATURE_POPCNT)) {
        grjit_asm_popcnt(a, GRJIT_RAX, GRJIT_RAX, wide);
      } else {
        emit_popcnt_v1(e, wide);
      }
      break;
    case GRJIT_OP_EXT8_S:
      grjit_asm_movsx(a, GRJIT_RAX, GRJIT_RAX, 8, wide);
      break;
    case GRJIT_OP_EXT16_S:
      grjit_asm_movsx(a, GRJIT_RAX, GRJIT_RAX, 16, wide);
      break;
    case GRJIT_OP_EXT32_S:
    case GRJIT_OP_EXTEND_S:
      grjit_asm_movsx(a, GRJIT_RAX, GRJIT_RAX, 32, true);
      break;
    case GRJIT_OP_WRAP:
    case GRJIT_OP_EXTEND_U:
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
      break;
    case GRJIT_OP_NEG:
      grjit_asm_neg32(a, GRJIT_RAX);
      break;
    case GRJIT_OP_NOT:
      grjit_asm_not32(a, GRJIT_RAX);
      break;
    default:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

/* ROTL and ROTR, either width. */
static void emit_rotate(GRJIT_Emit * e, const GRJIT_Op * op) {
  load_operand(e, GRJIT_RAX, &op->a);
  load_operand(e, GRJIT_RCX, &op->b);
  grjit_asm_shift_cl(&e->as, op->kind == GRJIT_OP_ROTL ? GRJIT_SHIFT_ROL : GRJIT_SHIFT_ROR,
      GRJIT_RAX, !dst_is_i32(e, op));
  store_result(e, op->dst, GRJIT_RAX);
}

/* DIV_S, DIV_U, REM_S, REM_U, either width. The divisor is non-zero and, for DIV_S, the
 * quotient does not overflow: the guards are in front of this in the IR. */
static void emit_divide(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const bool wide = !dst_is_i32(e, op);
  const bool is_signed = op->kind == GRJIT_OP_DIV_S || op->kind == GRJIT_OP_REM_S;
  const bool remainder = op->kind == GRJIT_OP_REM_S || op->kind == GRJIT_OP_REM_U;
  load_operand(e, GRJIT_RAX, &op->a);
  load_operand(e, GRJIT_RCX, &op->b);
  GRJIT_Label minus_one = 0;
  GRJIT_Label done = 0;
  if (op->kind == GRJIT_OP_REM_S) {
    /* `MIN % -1` is 0 and `idiv` faults on it: a divisor of -1 skips the division. */
    minus_one = grjit_asm_label(a);
    done = grjit_asm_label(a);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 39
    /* Planted defect 39 (tests only): `idiv` is reached on `MIN % -1`. */
    (void)minus_one;
#else
    if (wide) {
      grjit_asm_cmp_ri(a, GRJIT_RCX, -1);
    } else {
      grjit_asm_cmp_ri32(a, GRJIT_RCX, -1);
    }
    grjit_asm_jcc(a, GRJIT_COND_E, minus_one);
#endif
  }
  if (is_signed) {
    if (wide) {
      grjit_asm_cqo(a);
    } else {
      grjit_asm_cdq(a);
    }
  } else {
    grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RDX, GRJIT_RDX);
  }
  grjit_asm_div(a, GRJIT_RCX, is_signed, wide);
  if (remainder) {
    if (wide) {
      grjit_asm_mov_rr(a, GRJIT_RAX, GRJIT_RDX);
    } else {
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RDX);
    }
  }
  if (op->kind == GRJIT_OP_REM_S) {
    grjit_asm_jmp(a, done);
    grjit_asm_bind(a, minus_one);
    grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_bind(a, done);
  }
  store_result(e, op->dst, GRJIT_RAX);
}

/* A comparison of two i32 operands: a 32-bit `cmp`, then 0 or 1. */
static void emit_cmp32(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  load_operand(e, GRJIT_RAX, &op->a);
  load_operand(e, GRJIT_RCX, &op->b);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 42
  /* Planted defect 42 (tests only): the operands of an i32 ordered comparison are swapped. */
  if (op->cmp != GRJIT_CMP_EQ && op->cmp != GRJIT_CMP_NE) {
    grjit_asm_alu32_rr(a, GRJIT_ALU_CMP, GRJIT_RCX, GRJIT_RAX);
  } else {
    grjit_asm_alu32_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX);
  }
#else
  grjit_asm_alu32_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX);
#endif
  grjit_asm_setcc(a, cmp_cond(op->cmp), GRJIT_RAX);
  grjit_asm_movzx_r8(a, GRJIT_RAX, GRJIT_RAX);
  store_result(e, op->dst, GRJIT_RAX);
}

/* ---- The float operations of numeric-operations.md (spec-runtime-float, story 3) --------------
 *
 * Every sequence is SSE2 (x86-64-v1), runs the same on every host, and keeps the emitter's shape: a
 * value is a bit word in its frame slot, `xmm0` to `xmm7` are scratch, and a result goes back to its
 * slot as a full word. An f32 result is taken out of its register with `movd eax, xmm`, which
 * clears the upper half of the word, so an f32 is zero-extended in its slot by construction (the
 * upper bits of an xmm register after a scalar single operation are not always zero: `cvtsd2ss`
 * leaves bits 32 to 63 of its destination alone). Story 4 adds the SSE4.1 `roundss`/`roundsd` arm of
 * the four rounding operations; the sequences here stay as its fallback, so they are separate
 * functions.
 *
 * Nothing here calls libm and nothing reads or writes MXCSR: the default rounding (to nearest
 * even), exceptions masked, no flush to zero. Nothing traps: a conversion that would is guarded in
 * the IR, and `cvtt*` returns the "integer indefinite" value in hardware, never a fault. */

static bool reg_is_f64(const GRJIT_Emit * e, GRJIT_VReg v) {
  return e->c.f->vregs[v].type == GRJIT_TYPE_F64;
}

/* A float operand into an xmm register: the slot (a 32- or 64-bit load that clears the rest), or
 * an immediate's bits through `rax`. */
static void load_float(GRJIT_Emit * e, GRJIT_Xmm x, const GRJIT_Operand * o, bool dbl) {
  GRJIT_Asm * a = &e->as;
  if (o->kind == GRJIT_OPERAND_VREG) {
    grjit_asm_movs_load(a, x, GRJIT_RBP, grjit_emit_slot(o->vreg), dbl);
  } else {
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)o->imm);
    grjit_asm_movd_xr(a, x, GRJIT_RAX, dbl);
  }
}

/* A float result into its slot as a whole word. */
static void store_float(GRJIT_Emit * e, GRJIT_VReg dst, GRJIT_Xmm x, bool dbl) {
  GRJIT_Asm * a = &e->as;
  if (dbl) {
    grjit_asm_movs_store(a, GRJIT_RBP, grjit_emit_slot(dst), x, true);
  } else {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 55
    /* Planted defect 55 (tests only): an f32 is taken out of its register as 64 bits, so
     * whatever the operation left above bit 31 (`cvtsd2ss` leaves half of the double there)
     * lands in the slot and the f32 is not zero-extended. */
    grjit_asm_movd_rx(a, GRJIT_RAX, x, true);
#else
    grjit_asm_movd_rx(a, GRJIT_RAX, x, false);
#endif
    store_result(e, dst, GRJIT_RAX);
  }
}

/* The bit patterns of the constants the sequences need, for the width they work in. */
static uint64_t float_bits(double v, bool dbl) {
  if (dbl) {
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
  }
  float f = (float)v;
  uint32_t b;
  memcpy(&b, &f, sizeof b);
  return b;
}

static uint64_t sign_mask(bool dbl) { return dbl ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000); }
static uint64_t abs_mask(bool dbl) { return dbl ? UINT64_C(0x7FFFFFFFFFFFFFFF) : UINT64_C(0x7FFFFFFF); }

/* FADD FSUB FMUL FDIV. IEEE-754 operations on SSE2 are the hardware's own, operands in order. */
static void emit_float_arith(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  GRJIT_Asm * a = &e->as;
  GRJIT_Operand left = op->a;
  GRJIT_Operand right = op->b;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 45
  /* Planted defect 45 (tests only): the operands of a float subtraction are swapped. */
  if (op->kind == GRJIT_OP_FSUB) {
    left = op->b;
    right = op->a;
  }
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 46
  /* Planted defect 46 (tests only): the operands of a float division are swapped. */
  if (op->kind == GRJIT_OP_FDIV) {
    left = op->b;
    right = op->a;
  }
#endif
  load_float(e, GRJIT_XMM0, &left, dbl);
  load_float(e, GRJIT_XMM1, &right, dbl);
  const GRJIT_SseOp which = op->kind == GRJIT_OP_FADD   ? GRJIT_SSE_ADD
                            : op->kind == GRJIT_OP_FSUB ? GRJIT_SSE_SUB
                            : op->kind == GRJIT_OP_FMUL ? GRJIT_SSE_MUL
                                                        : GRJIT_SSE_DIV;
  grjit_asm_sse_rr(a, which, GRJIT_XMM0, GRJIT_XMM1, dbl);
  store_float(e, op->dst, GRJIT_XMM0, dbl);
}

static void emit_float_sqrt(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  load_float(e, GRJIT_XMM1, &op->a, dbl);
  grjit_asm_sse_rr(&e->as, GRJIT_SSE_SQRT, GRJIT_XMM0, GRJIT_XMM1, dbl);
  store_float(e, op->dst, GRJIT_XMM0, dbl);
}

/* FMIN and FMAX. `minsd`/`maxsd` return their second operand for a NaN and for equal operands,
 * which is wrong twice for WebAssembly: a NaN must give a NaN, and `min(-0, +0)` is `-0` while
 * `max(-0, +0)` is `+0`. So: an unordered compare is the NaN case (an add of the two returns a
 * quiet NaN, the first operand's if it is one), equal operands combine their bits (OR for min, AND
 * for max, which on two zeros is the sign rule and on any other equal pair changes nothing), and
 * everything else is the instruction. */
static void emit_float_minmax(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  GRJIT_Asm * a = &e->as;
  const bool is_min = op->kind == GRJIT_OP_FMIN;
  load_float(e, GRJIT_XMM0, &op->a, dbl);
  load_float(e, GRJIT_XMM1, &op->b, dbl);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 50
  /* Planted defect 50 (tests only): min and max are the bare instruction, which returns the
   * second operand for a NaN on either side and for min(-0, +0) and max(-0, +0). */
  grjit_asm_sse_rr(a, is_min ? GRJIT_SSE_MIN : GRJIT_SSE_MAX, GRJIT_XMM0, GRJIT_XMM1, dbl);
  store_float(e, op->dst, GRJIT_XMM0, dbl);
  return;
#endif
  GRJIT_Label nan = grjit_asm_label(a);
  GRJIT_Label equal = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  grjit_asm_ucomis(a, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_jcc(a, GRJIT_COND_P, nan);
  grjit_asm_jcc(a, GRJIT_COND_E, equal);
  grjit_asm_sse_rr(a, is_min ? GRJIT_SSE_MIN : GRJIT_SSE_MAX, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, equal);
  grjit_asm_sse_logic(a, is_min ? GRJIT_SSE_OR : GRJIT_SSE_AND, GRJIT_XMM0, GRJIT_XMM1);
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, nan);
  grjit_asm_sse_rr(a, GRJIT_SSE_ADD, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_bind(a, done);
  store_float(e, op->dst, GRJIT_XMM0, dbl);
}

/* FNEG, FABS and FCOPYSIGN act on the sign bit only, in the integer registers: a signalling NaN
 * keeps its payload and its quiet bit, and neg(+0) is -0, neither of which arithmetic gives. */
static void emit_float_sign(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  GRJIT_Asm * a = &e->as;
#define ALU(o, d, s) (dbl ? grjit_asm_alu_rr(a, o, d, s) : grjit_asm_alu32_rr(a, o, d, s))
  if (op->kind == GRJIT_OP_FNEG || op->kind == GRJIT_OP_FABS) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 53
    /* Planted defect 53 (tests only): negation and absolute value go through arithmetic: x * -1
     * and (x with the sign cleared) + 0, which quiet a signalling NaN. */
    if (op->kind == GRJIT_OP_FNEG) {
      load_float(e, GRJIT_XMM0, &op->a, dbl);
      grjit_asm_mov_ri(a, GRJIT_RAX, float_bits(-1.0, dbl));
      grjit_asm_movd_xr(a, GRJIT_XMM1, GRJIT_RAX, dbl);
      grjit_asm_sse_rr(a, GRJIT_SSE_MUL, GRJIT_XMM0, GRJIT_XMM1, dbl);
    } else {
      load_operand(e, GRJIT_RAX, &op->a);
      grjit_asm_mov_ri(a, GRJIT_RCX, abs_mask(dbl));
      ALU(GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX);
      grjit_asm_movd_xr(a, GRJIT_XMM0, GRJIT_RAX, dbl);
      grjit_asm_sse_logic(a, GRJIT_SSE_XOR, GRJIT_XMM1, GRJIT_XMM1);
      grjit_asm_sse_rr(a, GRJIT_SSE_ADD, GRJIT_XMM0, GRJIT_XMM1, dbl);
    }
    store_float(e, op->dst, GRJIT_XMM0, dbl);
    return;
#endif
    load_operand(e, GRJIT_RAX, &op->a);
    if (op->kind == GRJIT_OP_FNEG) {
      grjit_asm_mov_ri(a, GRJIT_RCX, sign_mask(dbl));
      ALU(GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RCX);
    } else {
      grjit_asm_mov_ri(a, GRJIT_RCX, abs_mask(dbl));
      ALU(GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX);
    }
  } else {
    load_operand(e, GRJIT_RAX, &op->a);
    load_operand(e, GRJIT_RCX, &op->b);
    grjit_asm_mov_ri(a, GRJIT_RDX, abs_mask(dbl));
    ALU(GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RDX);
    grjit_asm_mov_ri(a, GRJIT_RDX, sign_mask(dbl));
    ALU(GRJIT_ALU_AND, GRJIT_RCX, GRJIT_RDX);
    ALU(GRJIT_ALU_OR, GRJIT_RAX, GRJIT_RCX);
  }
#undef ALU
  store_result(e, op->dst, GRJIT_RAX);
}

/* FCEIL FFLOOR FTRUNC FNEAREST on SSE2 alone, no `roundss`, no libm.
 *
 * For |x| below 2^52 (2^23 for f32) `(|x| + 2^52) - 2^52` is the nearest integer to |x|, ties to
 * even, because the sum has no bits below the unit place and the default mode rounds it that way.
 * That is `nearest` after the sign of x is put back with a bit operation (which keeps the sign of a
 * zero result: nearest(-0.3) is -0). The others adjust it by one: `trunc` subtracts 1 when the
 * nearest of |x| is above |x|; `ceil` adds 1 to the signed nearest when it is below x; `floor`
 * subtracts 1 when it is above x. The adjustment is a compare mask ANDed with 1.0, no branch.
 * A value with |x| >= 2^52 is an integer already, and an infinity is unchanged: the result is x.
 * A NaN (the compare is unordered) gets the quiet bit by an add, which keeps its sign and payload
 * otherwise. */
static void emit_float_round(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  GRJIT_Asm * a = &e->as;
  /* MASK and ONE are written only after SIGN is built (ABS is dead from there: it is read to make MAG and SIGN) and
   * after T is built (MAGIC is dead from there), so under Win64, where `xmm6`-`xmm15` are callee-saved and are never
   * encoded, they take those two dead registers; the instructions are the same and the registers are the only
   * difference. SysV keeps `xmm6` and `xmm7`, and its bytes do not change. */
  const GRJIT_Xmm X = GRJIT_XMM0, ABS = GRJIT_XMM2, MAGIC = GRJIT_XMM3, SIGN = GRJIT_XMM4,
                  T = GRJIT_XMM5, MAG = GRJIT_XMM1;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 88
  /* Planted defect 88 (tests only): the Win64 flavour keeps the SysV registers, naming the callee-saved
   * `xmm6` and `xmm7`. */
  const GRJIT_Xmm MASK = GRJIT_XMM6, ONE = GRJIT_XMM7;
#else
  const GRJIT_Xmm MASK = e->win64 ? ABS : GRJIT_XMM6, ONE = e->win64 ? MAGIC : GRJIT_XMM7;
#endif
  GRJIT_Label nan = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  load_float(e, X, &op->a, dbl);
  grjit_asm_mov_ri(a, GRJIT_RAX, abs_mask(dbl));
  grjit_asm_movd_xr(a, ABS, GRJIT_RAX, dbl);
  grjit_asm_movaps(a, MAG, X);
  grjit_asm_sse_logic(a, GRJIT_SSE_AND, MAG, ABS);                    /* |x| */
  grjit_asm_mov_ri(a, GRJIT_RAX, float_bits(dbl ? 4503599627370496.0 : 8388608.0, dbl));
  grjit_asm_movd_xr(a, MAGIC, GRJIT_RAX, dbl);                         /* 2^52 or 2^23 */
  grjit_asm_ucomis(a, MAGIC, MAG, dbl);
  grjit_asm_jcc(a, GRJIT_COND_P, nan);
  grjit_asm_jcc(a, GRJIT_COND_BE, done);                               /* 2^52 <= |x|: x itself */
  grjit_asm_movaps(a, T, MAG);
  grjit_asm_sse_rr(a, GRJIT_SSE_ADD, T, MAGIC, dbl);
  grjit_asm_sse_rr(a, GRJIT_SSE_SUB, T, MAGIC, dbl);                   /* nearest(|x|) */
  grjit_asm_movaps(a, SIGN, ABS);
  grjit_asm_sse_logic(a, GRJIT_SSE_ANDN, SIGN, X);                     /* the sign bit of x */
  grjit_asm_mov_ri(a, GRJIT_RAX, float_bits(1.0, dbl));
  grjit_asm_movd_xr(a, ONE, GRJIT_RAX, dbl);
  switch (op->kind) {
    case GRJIT_OP_FNEAREST:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 52
      /* Planted defect 52 (tests only): halves round away from zero. The ties-to-even
       * result is bumped by one when |x| is exactly half a unit above it, which turns 2.5
       * into 3 (and leaves 3.5, which rounds up to even anyway, alone). */
      grjit_asm_movaps(a, MASK, MAG);
      grjit_asm_sse_rr(a, GRJIT_SSE_SUB, MASK, T, dbl);
      grjit_asm_mov_ri(a, GRJIT_RAX, float_bits(0.5, dbl));
      grjit_asm_movd_xr(a, e->win64 ? X : ABS, GRJIT_RAX, dbl); /* (X is dead here and is the result after) */
      grjit_asm_cmps(a, GRJIT_SSEP_EQ, MASK, e->win64 ? X : ABS, dbl);
      grjit_asm_sse_logic(a, GRJIT_SSE_AND, MASK, ONE);
      grjit_asm_sse_rr(a, GRJIT_SSE_ADD, T, MASK, dbl);
#endif
      grjit_asm_sse_logic(a, GRJIT_SSE_OR, T, SIGN);
      break;
    case GRJIT_OP_FTRUNC:
      grjit_asm_movaps(a, MASK, MAG);
      grjit_asm_cmps(a, GRJIT_SSEP_LT, MASK, T, dbl);                  /* |x| < nearest(|x|) */
      grjit_asm_sse_logic(a, GRJIT_SSE_AND, MASK, ONE);
      grjit_asm_sse_rr(a, GRJIT_SSE_SUB, T, MASK, dbl);
      grjit_asm_sse_logic(a, GRJIT_SSE_OR, T, SIGN);
      break;
    case GRJIT_OP_FCEIL:
      grjit_asm_sse_logic(a, GRJIT_SSE_OR, T, SIGN);
      grjit_asm_movaps(a, MASK, T);
      grjit_asm_cmps(a, GRJIT_SSEP_LT, MASK, X, dbl);                  /* t < x */
      /* t + 1 as t - (-1): a masked-out term is +0, and -0 - (+0) is -0 where -0 + (+0) is +0. */
      grjit_asm_mov_ri(a, GRJIT_RAX, float_bits(-1.0, dbl));
      grjit_asm_movd_xr(a, ONE, GRJIT_RAX, dbl);
      grjit_asm_sse_logic(a, GRJIT_SSE_AND, MASK, ONE);
      grjit_asm_sse_rr(a, GRJIT_SSE_SUB, T, MASK, dbl);
      /* ceil(-0.999) is -0, and (-1) + 1 is +0: a result of x's sign bit is right for every
       * ceil (a negative x ceils to a value at or below -0). */
      grjit_asm_sse_logic(a, GRJIT_SSE_OR, T, SIGN);
      break;
    default: /* FFLOOR */
      grjit_asm_sse_logic(a, GRJIT_SSE_OR, T, SIGN);
      grjit_asm_movaps(a, MASK, X);
      grjit_asm_cmps(a, GRJIT_SSEP_LT, MASK, T, dbl);                  /* x < t */
      grjit_asm_sse_logic(a, GRJIT_SSE_AND, MASK, ONE);
      grjit_asm_sse_rr(a, GRJIT_SSE_SUB, T, MASK, dbl);
      break;
  }
  grjit_asm_movaps(a, X, T);
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, nan);
  grjit_asm_sse_rr(a, GRJIT_SSE_ADD, X, X, dbl);                       /* quiet, sign and payload kept */
  grjit_asm_bind(a, done);
  store_float(e, op->dst, X, dbl);
}

/* FCEIL FFLOOR FTRUNC FNEAREST with SSE4.1: one `roundss`/`roundsd` whose immediate is the
 * mode (bits 0-1: 0 nearest-even, 1 down, 2 up, 3 toward zero) with bit 3 set so that no precision
 * exception is raised: 0x0A ceil, 0x09 floor, 0x0B trunc, 0x08 nearest. The instruction is
 * IEEE `roundToIntegral`, which is what the v1 sequence above builds from SSE2: the sign of a
 * zero result kept, ties to even for `nearest`, an infinity and a value at or above 2^52 (2^23)
 * unchanged, and a NaN quiet with its sign and payload kept. The F32 result is stored by the
 * existing `movd`, which writes a zero-extended word. */
static void emit_float_round_sse41(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  GRJIT_Asm * a = &e->as;
  uint8_t imm = 0x08;
  switch (op->kind) {
    case GRJIT_OP_FCEIL:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 65
      imm = 0x09; /* Planted defect 65 (tests only): floor and ceil swapped. */
#else
      imm = 0x0A;
#endif
      break;
    case GRJIT_OP_FFLOOR:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 65
      imm = 0x0A;
#else
      imm = 0x09;
#endif
      break;
    case GRJIT_OP_FTRUNC:
      imm = 0x0B;
      break;
    default: /* FNEAREST */
      imm = 0x08;
      break;
  }
  load_float(e, GRJIT_XMM0, &op->a, dbl);
  if (dbl) {
    grjit_asm_roundsd(a, GRJIT_XMM0, GRJIT_XMM0, imm);
  } else {
    grjit_asm_roundss(a, GRJIT_XMM0, GRJIT_XMM0, imm);
  }
  store_float(e, op->dst, GRJIT_XMM0, dbl);
}

/* An x86 `setcc` pair joins the flags of `ucomis*`, which sets ZF, PF and CF together for an
 * unordered pair: EQ needs ZF and no PF, NE needs not-ZF or PF, and LT/LE are GT/GE with the
 * operands swapped, because the unordered case then fails the `above` test (CF is set) by
 * itself. */
static void emit_float_cmp(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  GRJIT_VReg witness = op->a.kind == GRJIT_OPERAND_VREG &&
                               grjit_type_is_float(e->c.f->vregs[op->a.vreg].type)
                           ? op->a.vreg
                           : op->b.vreg;
  const bool dbl = reg_is_f64(e, witness);
  load_float(e, GRJIT_XMM0, &op->a, dbl);
  load_float(e, GRJIT_XMM1, &op->b, dbl);
  bool swap = op->cmp == GRJIT_CMP_LT || op->cmp == GRJIT_CMP_LE;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 47
  /* Planted defect 47 (tests only): the operands of an ordered float compare are not swapped for
   * LT and LE and are for GT and GE. */
  swap = op->cmp == GRJIT_CMP_GT || op->cmp == GRJIT_CMP_GE;
#endif
  grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RCX, GRJIT_RCX);
  if (swap) {
    grjit_asm_ucomis(a, GRJIT_XMM1, GRJIT_XMM0, dbl);
  } else {
    grjit_asm_ucomis(a, GRJIT_XMM0, GRJIT_XMM1, dbl);
  }
  switch (op->cmp) {
    case GRJIT_CMP_EQ:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 43
      /* Planted defect 43 (tests only): no test of PF, so an unordered pair compares equal. */
      grjit_asm_setcc(a, GRJIT_COND_E, GRJIT_RAX);
#else
      grjit_asm_setcc(a, GRJIT_COND_E, GRJIT_RAX);
      grjit_asm_setcc(a, GRJIT_COND_NP, GRJIT_RCX);
      grjit_asm_alu32_rr(a, GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX);
#endif
      break;
    case GRJIT_CMP_NE:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 43
      grjit_asm_setcc(a, GRJIT_COND_NE, GRJIT_RAX);
#else
      grjit_asm_setcc(a, GRJIT_COND_NE, GRJIT_RAX);
      grjit_asm_setcc(a, GRJIT_COND_P, GRJIT_RCX);
      grjit_asm_alu32_rr(a, GRJIT_ALU_OR, GRJIT_RAX, GRJIT_RCX);
#endif
      break;
    case GRJIT_CMP_GT:
    case GRJIT_CMP_LT:
      grjit_asm_setcc(a, GRJIT_COND_A, GRJIT_RAX);
      break;
    default: /* GE, LE */
      grjit_asm_setcc(a, GRJIT_COND_AE, GRJIT_RAX);
      break;
  }
  store_result(e, op->dst, GRJIT_RAX);
}

/* CONVERT_S and CONVERT_U: integer to float. From 32 bits the value is widened (sign- or
 * zero-extended) and converted as the 64-bit integer it now is, which is exact in the integer and
 * one rounding in the float. From 64 bits unsigned there is no SSE2 form: a value below 2^63 is a
 * signed one; above it, the halve-with-sticky-bit sequence: shift right one and OR the bit shifted
 * out back in (so the halved value rounds the way the whole would, round-to-odd), convert, double.
 * Converting the halved value without the sticky bit rounds twice (a tie that was not one). */
static void emit_float_convert(GRJIT_Emit * e, const GRJIT_Op * op, bool dbl) {
  GRJIT_Asm * a = &e->as;
  const bool wide_src = e->c.f->vregs[op->a.vreg].type == GRJIT_TYPE_I64;
  const bool is_signed = op->kind == GRJIT_OP_CONVERT_S;
  load_operand(e, GRJIT_RAX, &op->a);
  grjit_asm_sse_logic(a, GRJIT_SSE_XOR, GRJIT_XMM0, GRJIT_XMM0);
  if (!wide_src) {
    if (is_signed) {
      grjit_asm_movsx(a, GRJIT_RAX, GRJIT_RAX, 32, true);
    }
    grjit_asm_cvtsi2s(a, GRJIT_XMM0, GRJIT_RAX, dbl, true);
  } else if (is_signed) {
    grjit_asm_cvtsi2s(a, GRJIT_XMM0, GRJIT_RAX, dbl, true);
  } else {
    GRJIT_Label big = grjit_asm_label(a);
    GRJIT_Label done = grjit_asm_label(a);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_S, big);
    grjit_asm_cvtsi2s(a, GRJIT_XMM0, GRJIT_RAX, dbl, true);
    grjit_asm_jmp(a, done);
    grjit_asm_bind(a, big);
    grjit_asm_mov_rr(a, GRJIT_RCX, GRJIT_RAX);
    grjit_asm_shift_ri(a, GRJIT_SHIFT_SHR, GRJIT_RCX, 1, true);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 51
    /* Planted defect 51 (tests only): the bit shifted out is dropped, so the halved value
     * rounds twice. */
#else
    grjit_asm_mov_ri(a, GRJIT_RDX, 1);
    grjit_asm_alu_rr(a, GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RDX);
    grjit_asm_alu_rr(a, GRJIT_ALU_OR, GRJIT_RCX, GRJIT_RAX);
#endif
    grjit_asm_cvtsi2s(a, GRJIT_XMM0, GRJIT_RCX, dbl, true);
    grjit_asm_sse_rr(a, GRJIT_SSE_ADD, GRJIT_XMM0, GRJIT_XMM0, dbl);
    grjit_asm_bind(a, done);
  }
  store_float(e, op->dst, GRJIT_XMM0, dbl);
}

/* TRUNC_S and TRUNC_U: float to integer, partial. The IR has put its guards in front, so the
 * operand is not a NaN and its truncation fits the destination, and `cvtt*2si` to 64 bits is exact.
 * An unsigned 64-bit result at or above 2^63 does not fit a signed conversion: subtract 2^63,
 * convert, set the top bit. */
static void load_float_const(GRJIT_Emit * e, GRJIT_Xmm x, double v, bool dbl) {
  grjit_asm_mov_ri(&e->as, GRJIT_RAX, float_bits(v, dbl));
  grjit_asm_movd_xr(&e->as, x, GRJIT_RAX, dbl);
}

/* The conversion of x (in xmm0) to a 64-bit integer, unsigned semantic for a value that may be at
 * or above 2^63, into rax. Clobbers xmm1, rcx. */
static void emit_cvt_u64(GRJIT_Emit * e, bool dbl) {
  GRJIT_Asm * a = &e->as;
  GRJIT_Label big = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  load_float_const(e, GRJIT_XMM1, 9223372036854775808.0, dbl);
  grjit_asm_ucomis(a, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_jcc(a, GRJIT_COND_AE, big);
  grjit_asm_cvtts2si(a, GRJIT_RAX, GRJIT_XMM0, dbl, true);
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, big);
  grjit_asm_sse_rr(a, GRJIT_SSE_SUB, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_cvtts2si(a, GRJIT_RAX, GRJIT_XMM0, dbl, true);
  grjit_asm_mov_ri(a, GRJIT_RCX, UINT64_C(0x8000000000000000));
  grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RCX);
  grjit_asm_bind(a, done);
}

static void emit_float_trunc(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const bool dbl = reg_is_f64(e, op->a.vreg);
  const bool to64 = e->c.f->vregs[op->dst].type == GRJIT_TYPE_I64;
  load_float(e, GRJIT_XMM0, &op->a, dbl);
  if (to64 && op->kind == GRJIT_OP_TRUNC_U) {
    emit_cvt_u64(e, dbl);
  } else {
    /* In range, so the 64-bit conversion is exact; an i32 result is its low half (for a
     * signed one, in range, the two's-complement low half is the value). */
    grjit_asm_cvtts2si(a, GRJIT_RAX, GRJIT_XMM0, dbl, true);
    if (!to64) {
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    }
  }
  store_result(e, op->dst, GRJIT_RAX);
}

/* TRUNC_SAT_S and TRUNC_SAT_U: total. NaN is 0; a value at or beyond a bound is that bound's
 * integer; the rest truncates. `cvtt*2si` returns the "integer indefinite" value (the minimum) for
 * what it cannot represent, which is an answer for nothing here, so every case it would get wrong
 * is decided before it runs. */
static void emit_float_trunc_sat(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const bool dbl = reg_is_f64(e, op->a.vreg);
  const bool to64 = e->c.f->vregs[op->dst].type == GRJIT_TYPE_I64;
  const bool sgn = op->kind == GRJIT_OP_TRUNC_SAT_S;
  GRJIT_Label nan = grjit_asm_label(a);
  GRJIT_Label low = grjit_asm_label(a);
  GRJIT_Label high = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  load_float(e, GRJIT_XMM0, &op->a, dbl);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 54
  /* Planted defect 54 (tests only): a NaN is not given 0: the unordered compares below fall to
   * the low bound. */
  (void)nan;
#else
  grjit_asm_ucomis(a, GRJIT_XMM0, GRJIT_XMM0, dbl);
  grjit_asm_jcc(a, GRJIT_COND_P, nan);
#endif
  if (sgn) {
    load_float_const(e, GRJIT_XMM1, to64 ? -9223372036854775808.0 : -2147483648.0, dbl);
  } else {
    grjit_asm_sse_logic(a, GRJIT_SSE_XOR, GRJIT_XMM1, GRJIT_XMM1); /* the lower bound is 0 */
  }
  grjit_asm_ucomis(a, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_jcc(a, GRJIT_COND_BE, low);
  load_float_const(e, GRJIT_XMM1,
      sgn ? (to64 ? 9223372036854775808.0 : 2147483648.0)
          : (to64 ? 18446744073709551616.0 : 4294967296.0),
      dbl);
  grjit_asm_ucomis(a, GRJIT_XMM0, GRJIT_XMM1, dbl);
  grjit_asm_jcc(a, GRJIT_COND_AE, high);
  if (to64 && !sgn) {
    emit_cvt_u64(e, dbl);
  } else {
    grjit_asm_cvtts2si(a, GRJIT_RAX, GRJIT_XMM0, dbl, true);
    if (!to64) {
      grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    }
  }
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, low); /* signed: the minimum; unsigned: 0 */
  grjit_asm_mov_ri(a, GRJIT_RAX, !sgn ? 0 : to64 ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000));
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, high);
  grjit_asm_mov_ri(a, GRJIT_RAX,
      sgn ? (to64 ? UINT64_C(0x7FFFFFFFFFFFFFFF) : UINT64_C(0x7FFFFFFF))
          : (to64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF)));
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, nan);
  grjit_asm_alu32_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RAX);
  grjit_asm_bind(a, done);
  store_result(e, op->dst, GRJIT_RAX);
}

static void emit_float_op(GRJIT_Emit * e, const GRJIT_Op * op) {
  GRJIT_Asm * a = &e->as;
  const bool dbl = op->dst != GRJIT_NO_VREG && reg_is_f64(e, op->dst);
  switch (op->kind) {
    case GRJIT_OP_FADD:
    case GRJIT_OP_FSUB:
    case GRJIT_OP_FMUL:
    case GRJIT_OP_FDIV:
      emit_float_arith(e, op, dbl);
      break;
    case GRJIT_OP_FMIN:
    case GRJIT_OP_FMAX:
      emit_float_minmax(e, op, dbl);
      break;
    case GRJIT_OP_FCOPYSIGN:
    case GRJIT_OP_FNEG:
    case GRJIT_OP_FABS:
      emit_float_sign(e, op, dbl);
      break;
    case GRJIT_OP_FSQRT:
      emit_float_sqrt(e, op, dbl);
      break;
    case GRJIT_OP_FCEIL:
    case GRJIT_OP_FFLOOR:
    case GRJIT_OP_FTRUNC:
    case GRJIT_OP_FNEAREST:
      if (take_feature(e, GRJIT_FEATURE_SSE4_1)) {
        emit_float_round_sse41(e, op, dbl);
      } else {
        emit_float_round(e, op, dbl);
      }
      break;
    case GRJIT_OP_CONVERT_S:
    case GRJIT_OP_CONVERT_U:
      emit_float_convert(e, op, dbl);
      break;
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U:
      emit_float_trunc(e, op);
      break;
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U:
      emit_float_trunc_sat(e, op);
      break;
    case GRJIT_OP_DEMOTE:
      load_float(e, GRJIT_XMM0, &op->a, true);
      grjit_asm_cvts2s(a, GRJIT_XMM0, GRJIT_XMM0, false);
      store_float(e, op->dst, GRJIT_XMM0, false);
      break;
    case GRJIT_OP_PROMOTE:
      load_float(e, GRJIT_XMM0, &op->a, false);
      grjit_asm_cvts2s(a, GRJIT_XMM0, GRJIT_XMM0, true);
      store_float(e, op->dst, GRJIT_XMM0, true);
      break;
    case GRJIT_OP_REINTERPRET:
      /* The bits are the bits: a slot to a slot. Both an i32 and an f32 are zero-extended, so the
       * word needs no cut, but it is cut anyway where the destination is 32 bits wide. */
      load_operand(e, GRJIT_RAX, &op->a);
      if (e->c.f->vregs[op->dst].type == GRJIT_TYPE_I32 ||
          e->c.f->vregs[op->dst].type == GRJIT_TYPE_F32) {
        grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
      }
      store_result(e, op->dst, GRJIT_RAX);
      break;
    default:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
}

static void emit_op(GRJIT_Emit * e, const GRJIT_Op * op, size_t block) {
  GRJIT_Asm * a = &e->as;
  switch (op->kind) {
    case GRJIT_OP_INSTANCE:
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_ENV);
      store_result(e, op->dst, GRJIT_RAX);
      break;
    case GRJIT_OP_CONST:
    case GRJIT_OP_MOVE:
    case GRJIT_OP_BITCAST:
      load_operand(e, GRJIT_RAX, &op->a);
      if (dst_is_i32_reg(e, op->dst)) {
        grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX); /* an i32 or f32 immediate: its low 32 bits */
      }
      store_result(e, op->dst, GRJIT_RAX);
      break;
    case GRJIT_OP_ADD:
    case GRJIT_OP_SUB:
    case GRJIT_OP_MUL:
    case GRJIT_OP_AND:
    case GRJIT_OP_OR:
    case GRJIT_OP_XOR:
    case GRJIT_OP_SHL:
    case GRJIT_OP_SHR:
    case GRJIT_OP_SAR:
      if (dst_is_i32(e, op)) {
        emit_binary32(e, op);
      } else {
        emit_binary(e, op);
      }
      break;
    case GRJIT_OP_NEG:
    case GRJIT_OP_NOT:
      if (dst_is_i32(e, op)) {
        emit_int_unary(e, op);
        break;
      }
      load_operand(e, GRJIT_RAX, &op->a);
      if (op->kind == GRJIT_OP_NEG) {
        grjit_asm_neg(a, GRJIT_RAX);
      } else {
        grjit_asm_not(a, GRJIT_RAX);
      }
      store_result(e, op->dst, GRJIT_RAX);
      break;
    case GRJIT_OP_CMP:
      if (operand_is_float(e, &op->a) || operand_is_float(e, &op->b)) {
        emit_float_cmp(e, op);
        break;
      }
      if (operand_is_i32(e, &op->a) || operand_is_i32(e, &op->b)) {
        emit_cmp32(e, op);
        break;
      }
      load_operand(e, GRJIT_RAX, &op->a);
      load_operand(e, GRJIT_RCX, &op->b);
      grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX);
      grjit_asm_setcc(a, cmp_cond(op->cmp), GRJIT_RAX);
      grjit_asm_movzx_r8(a, GRJIT_RAX, GRJIT_RAX);
      store_result(e, op->dst, GRJIT_RAX);
      break;
    case GRJIT_OP_LOAD:
      emit_load(e, op, false);
      break;
    case GRJIT_OP_LOAD_S:
      emit_load(e, op, true);
      break;
    case GRJIT_OP_STORE:
      emit_store(e, op);
      break;
    case GRJIT_OP_CALL:
      emit_call(e, op);
      break;
    case GRJIT_OP_POLL: {
      /* mov rax,[ctx]; mov rax,[rax+request_word]; test; jnz slow */
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_POLL;
      p.entry = grjit_asm_label(a);
      p.back = grjit_asm_label(a);
      p.op = op;
      p.live_index = e->c.live_cursor++;
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_CTX);
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RAX, (int32_t)e->c.request_offset);
      grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
      grjit_asm_jcc(a, GRJIT_COND_NE, p.entry);
      grjit_asm_bind(a, p.back);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_GUARD: {
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_GUARD;
      p.entry = grjit_asm_label(a);
      p.back = 0;
      p.op = op;
      p.live_index = e->c.live_cursor++;
      load_operand(e, GRJIT_RAX, &op->a);
      grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
      grjit_asm_jcc(a, GRJIT_COND_E, p.entry);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_BR:
      if (op->target != block + 1) {
        grjit_asm_jmp(a, e->blocks[op->target]);
      }
      break;
    case GRJIT_OP_BR_IF:
      load_operand(e, GRJIT_RAX, &op->a);
      grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
      if (op->target_else == block + 1) {
        grjit_asm_jcc(a, GRJIT_COND_NE, e->blocks[op->target]);
      } else if (op->target == block + 1) {
        grjit_asm_jcc(a, GRJIT_COND_E, e->blocks[op->target_else]);
      } else {
        grjit_asm_jcc(a, GRJIT_COND_NE, e->blocks[op->target]);
        grjit_asm_jmp(a, e->blocks[op->target_else]);
      }
      break;
    case GRJIT_OP_BR_TABLE:
      load_operand(e, GRJIT_RAX, &op->a);
      grjit_asm_mov_ri(a, GRJIT_RCX, op->target_count);
      grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX);
      grjit_asm_jcc(a, GRJIT_COND_AE, e->blocks[op->default_target]);
      for (size_t i = 0; i < op->target_count; i++) {
        grjit_asm_mov_ri(a, GRJIT_RCX, i);
        grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX);
        grjit_asm_jcc(a, GRJIT_COND_E, e->blocks[op->targets[i]]);
      }
      grjit_asm_jmp(a, e->blocks[op->default_target]);
      break;
    case GRJIT_OP_TRAP:
      grjit_asm_mov_ri(a, GRJIT_RAX, op->trap_kind);
      if (e->c.callable) {
        grjit_asm_mov_ri(a, ISTATUS(e), GRJIT_STATUS_TRAPPED);
        grjit_emit_callable_epilogue(e);
      } else {
        grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
        grjit_asm_store64(a, GRJIT_RDX, 0, GRJIT_RAX);
        grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_TRAP);
        grjit_emit_epilogue(e);
      }
      break;
    case GRJIT_OP_RETHROW:
      grjit_asm_mov_ri(a, GRJIT_RAX, 0);
      grjit_asm_mov_ri(a, ISTATUS(e), GRJIT_STATUS_EXCEPTION);
      grjit_emit_callable_epilogue(e);
      break;
    case GRJIT_OP_RET:
      if (e->c.callable) {
        /* The internal convention: the result in rax (zero for none), the
         * status in rdx, and the callee pops its own stack arguments. */
        if (op->result_count > 1) {
          grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_ARGS);
          for (size_t i = 1; i < op->result_count; i++) {
            load_operand(e, GRJIT_RAX, &op->returns[i]);
            grjit_asm_store64(a, GRJIT_RDX, (int32_t)(8 * (i - 1)), GRJIT_RAX);
          }
        }
        if (op->result_count != 0) load_operand(e, GRJIT_RAX, &op->returns[0]);
        else grjit_asm_mov_ri(a, GRJIT_RAX, 0);
        grjit_asm_mov_ri(a, ISTATUS(e), GRJIT_STATUS_RETURNED);
        grjit_emit_callable_epilogue(e);
        break;
      }
      if (op->result_count != 0) {
        if (op->result_count == 1) {
          load_operand(e, GRJIT_RAX, &op->returns[0]);
          grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
          grjit_asm_store64(a, GRJIT_RDX, 0, GRJIT_RAX);
        } else {
          grjit_asm_load64(a, GRJIT_RDX, GRJIT_RBP, GRJIT_SLOT_OUT);
          for (size_t i = 0; i < op->result_count; i++) {
            load_operand(e, GRJIT_RAX, &op->returns[i]);
            grjit_asm_store64(a, GRJIT_RDX, (int32_t)(8 * i), GRJIT_RAX);
          }
        }
      }
      grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_RETURNED);
      grjit_emit_epilogue(e);
      break;
    case GRJIT_OP_CALL_SLOT:
    case GRJIT_OP_CALL_PTR:
      emit_guest_call(e, op);
      break;
    case GRJIT_OP_TAIL_CALL_SLOT:
    case GRJIT_OP_TAIL_CALL_PTR:
      emit_tail_call(e, op);
      break;
    case GRJIT_OP_CALL_NATIVE:
      emit_native_call(e, op);
      break;
    case GRJIT_OP_CLZ:
    case GRJIT_OP_CTZ:
    case GRJIT_OP_POPCNT:
    case GRJIT_OP_EXT8_S:
    case GRJIT_OP_EXT16_S:
    case GRJIT_OP_EXT32_S:
    case GRJIT_OP_WRAP:
    case GRJIT_OP_EXTEND_S:
    case GRJIT_OP_EXTEND_U:
    case GRJIT_OP_ROTL:
    case GRJIT_OP_ROTR:
    case GRJIT_OP_DIV_S:
    case GRJIT_OP_DIV_U:
    case GRJIT_OP_REM_S:
    case GRJIT_OP_REM_U:
      if (op->kind == GRJIT_OP_ROTL || op->kind == GRJIT_OP_ROTR) {
        emit_rotate(e, op);
      } else if (op->kind >= GRJIT_OP_DIV_S && op->kind <= GRJIT_OP_REM_U) {
        emit_divide(e, op);
      } else {
        emit_int_unary(e, op);
      }
      break;
    case GRJIT_OP_FADD:
    case GRJIT_OP_FSUB:
    case GRJIT_OP_FMUL:
    case GRJIT_OP_FDIV:
    case GRJIT_OP_FMIN:
    case GRJIT_OP_FMAX:
    case GRJIT_OP_FCOPYSIGN:
    case GRJIT_OP_FNEG:
    case GRJIT_OP_FABS:
    case GRJIT_OP_FSQRT:
    case GRJIT_OP_FCEIL:
    case GRJIT_OP_FFLOOR:
    case GRJIT_OP_FTRUNC:
    case GRJIT_OP_FNEAREST:
    case GRJIT_OP_CONVERT_S:
    case GRJIT_OP_CONVERT_U:
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U:
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U:
    case GRJIT_OP_DEMOTE:
    case GRJIT_OP_PROMOTE:
    case GRJIT_OP_REINTERPRET:
      emit_float_op(e, op);
      break;
    case GRJIT_OP_COUNT:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
}

void grjit_emit_epilogue(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  if (e->win64) {
    /* The forms a Win64 unwinder recognises when it stops inside an epilogue:
     * `lea rsp, [fp + 0]` for a frame with a frame register, then the pops,
     * then `ret`. `leave` does the same work and is not one of them. */
    grjit_asm_lea(a, GRJIT_RSP, GRJIT_RBP, 0);
    grjit_asm_pop(a, GRJIT_RBP);
    grjit_asm_ret(a);
  } else {
    grjit_asm_leave(a);
    grjit_asm_ret(a);
  }
}

/* Win64: a frame of a page or more is probed a page at a time, downwards from
 * the return address, before rsp moves, so that the guard page is touched in
 * order and the stack grows to cover the frame (a single access further down
 * than the guard page is a stack overflow). The last probe is exactly at the
 * new rsp. It uses two volatile registers, `low` and `cursor`: r10 and r11 in a
 * plain function, and rax and r11 in a callable one, where r10 is the context
 * and the four argument registers are live. */
static void emit_stack_probe(GRJIT_Emit * e, uint32_t alloc, GRJIT_Reg low, GRJIT_Reg cursor) {
  GRJIT_Asm * a = &e->as;
  GRJIT_Label loop = grjit_asm_label(a);
  GRJIT_Label tail = grjit_asm_label(a);
  grjit_asm_lea(a, low, GRJIT_RSP, -(int32_t)alloc);
  grjit_asm_lea(a, cursor, GRJIT_RSP, 8);
  grjit_asm_bind(a, loop);
  grjit_asm_lea(a, cursor, cursor, -(int32_t)GRJIT_WIN64_PAGE);
  grjit_asm_alu_rr(a, GRJIT_ALU_CMP, cursor, low);
  grjit_asm_jcc(a, GRJIT_COND_BE, tail);
  grjit_asm_probe(a, cursor, 0);
  grjit_asm_jmp(a, loop);
  grjit_asm_bind(a, tail);
  grjit_asm_probe(a, low, 0);
}

/* The stubs the blocks queued. A stub may add sites but never another stub. */
static void emit_pending(GRJIT_Emit * e) {
  for (size_t i = 0; i < e->c.pending_count && e->c.error == GRJIT_OK; i++) {
    const GRJIT_Pending * p = &e->c.pending[i];
    switch (p->kind) {
      case GRJIT_PENDING_POLL:
        grjit_emit_poll_stub(e, p);
        break;
      case GRJIT_PENDING_GUARD:
        grjit_emit_guard_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_SLOW:
        grjit_emit_call_slow_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_EXIT:
        grjit_emit_call_exit_stub(e, p);
        break;
      case GRJIT_PENDING_NATIVE_EXIT:
        grjit_emit_native_exit_stub(e, p);
        break;
      case GRJIT_PENDING_NATIVE_STATUS:
        grjit_emit_native_status_stub(e, p);
        break;
    }
  }
}

/* The entry adapter of a callable function (AD-28): the C ABI of
 * ::GRJIT_EntryFn on one side and the internal convention on the other. It
 * saves the caller's `rbp`, keeps `out` and the context in its own frame,
 * runs the entry hook, loads the arguments from the `args` array into the
 * registers and the stack area, sets `rbp` to the chain-end marker (so the
 * first compiled frame's saved caller base is the marker, which ends a walk;
 * a/compiled.h), calls the internal entry, and turns its status into an exit.
 * It uses no callee-saved register but `rbp`, which it restores. It clears the
 * walk-start cell on the way out, so a walk after the run reads no dead frames. */
static void emit_adapter_win64(GRJIT_Emit * e);

static void emit_adapter(GRJIT_Emit * e) {
  if (e->win64) {
    emit_adapter_win64(e);
    return;
  }
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const size_t params = f->param_count;
  const uint32_t in_bytes = e->c.shape.incoming_bytes;
  const unsigned ireg = e->abi->internal->reg_args;
  const GRJIT_Reg scratch = ISCRATCH(e);
  const GRJIT_Reg status = ISTATUS(e);
  GRJIT_Label deopted = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  grjit_asm_push(a, GRJIT_RBP);
  grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP);
  grjit_asm_sub_rsp(a, 48);
  grjit_asm_store64(a, GRJIT_RSP, 32, C_ARG(e, 3)); /* out */
  grjit_asm_store64(a, GRJIT_RSP, 8, C_ARG(e, 0));  /* context */
  grjit_asm_store64(a, GRJIT_RSP, 16, C_ARG(e, 1)); /* environment */
  grjit_asm_store64(a, GRJIT_RSP, 24, C_ARG(e, 2)); /* args */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 9
  /* Planted defect 9 (tests only): a callee-saved register is used and never
   * restored. The caller's own value must survive the call. */
  grjit_asm_mov_rr(a, GRJIT_RBX, GRJIT_RDX);
  grjit_asm_mov_rr(a, GRJIT_R12, GRJIT_RSI);
#endif
  if (e->c.hook != NULL) {
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.hook);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);
  }
  grjit_asm_load64(a, scratch, GRJIT_RSP, 24);
  if (in_bytes != 0) {
    grjit_asm_sub_rsp(a, in_bytes);
    for (size_t i = ireg; i < params; i++) {
      grjit_asm_load64(a, GRJIT_RAX, scratch, (int32_t)(8 * i));
      grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - ireg)), GRJIT_RAX);
    }
  }
  for (size_t i = 0; i < params && i < ireg; i++) {
    grjit_asm_load64(a, IARG(e, i), scratch, (int32_t)(8 * i));
  }
  if (e->c.shape.has_result_buffer) {
    /* `out` is in the adapter's frame, which is `in_bytes` above the stack just made. */
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RSP, (int32_t)(in_bytes + 32));
    grjit_asm_lea(a, GRJIT_RAX, GRJIT_RAX, 8);
    if (params < ireg) grjit_asm_mov_rr(a, IARG(e, params), GRJIT_RAX);
    else grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (params - ireg)), GRJIT_RAX);
  }
  grjit_asm_load64(a, ICTX(e), GRJIT_RSP, (int32_t)(in_bytes + 8));
  grjit_asm_load64(a, GRJIT_R11, GRJIT_RSP, (int32_t)(in_bytes + 16));
  grjit_asm_mov_ri64(a, GRJIT_RBP, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_asm_lea_rip(a, GRJIT_RAX, e->internal);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* rsp is where it was after the frame was made: the callee popped the stack
   * arguments. */
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, 32);
  grjit_asm_load64(a, GRJIT_R8, GRJIT_RSP, 8);
  grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_R9, GRJIT_R9);
  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset, GRJIT_R9);
  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset + 8, GRJIT_R9);
  grjit_asm_test_rr(a, status, status);
  grjit_asm_jcc(a, GRJIT_COND_NE, deopted);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_RETURNED);
  grjit_asm_jmp(a, done);
  /* DEOPTED: every compiled frame was rebuilt into its guest frame before any
   * of them returned; out[0] is the cause. */
  grjit_asm_bind(a, deopted);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_DEOPT);
  GRJIT_Label not_trap = grjit_asm_label(a);
  grjit_asm_cmp_ri(a, status, (int32_t)GRJIT_STATUS_TRAPPED);
  grjit_asm_jcc(a, GRJIT_COND_NE, not_trap);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_TRAP);
  grjit_asm_bind(a, not_trap);
  /* FAILED: the engine's hook refused the rebuild; out[0] is its answer. */
  GRJIT_Label not_failed = grjit_asm_label(a);
  grjit_asm_cmp_ri(a, status, (int32_t)GRJIT_STATUS_FAILED);
  grjit_asm_jcc(a, GRJIT_COND_NE, not_failed);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REBUILD_FAILED);
  grjit_asm_bind(a, not_failed);
  GRJIT_Label not_exception = grjit_asm_label(a);
  grjit_asm_cmp_ri(a, status, (int32_t)GRJIT_STATUS_EXCEPTION);
  grjit_asm_jcc(a, GRJIT_COND_NE, not_exception);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_EXCEPTION);
  grjit_asm_bind(a, not_exception);
  grjit_asm_bind(a, done);
  grjit_asm_add_rsp(a, 48);
  grjit_asm_pop(a, GRJIT_RBP);
  grjit_asm_ret(a);
  /* The entry hook refused (eax is its answer). */
  grjit_asm_bind(a, e->refuse);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, 32);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REFUSED);
  grjit_asm_add_rsp(a, 48);
  grjit_asm_pop(a, GRJIT_RBP);
  grjit_asm_ret(a);
}

/* The Win64 entry adapter. The same job as the SysV one, with a **static frame**:
 * `push rbp; sub rsp, N'` and everything addressed from `rsp`, no frame register.
 * It sets `rbp` to GRCORE_COMPILED_CHAIN_END before it calls the internal entry,
 * which a frame-register unwinder would take for a real base, so its unwind
 * information (memory.c) has none, and the unwinder finds its caller from the
 * `rsp` it is given, which is the bottom of this frame: the callee's frame says
 * `rsp = rbp + 16` for it, and the stack arguments, which the call pushes the
 * return address below, are exactly at the bottom.
 *
 *   rsp + 0         the C hook's 32 bytes of shadow space and, later, the stack
 *                   arguments of the internal call (`W = max(32, in_A)` bytes, so
 *                   one allocation is right for both)
 *   rsp + W + 8..24 `args`, the context, `out`
 *
 * The callee's `ret in_A` returns `rsp` above the arguments, so the slots are read
 * from `rsp + (offset - in_A)` afterwards and `N' - in_A` is freed. N' is a multiple
 * of 16 and the entry's `rsp` is 8 mod 16, so after `push rbp` and the allocation
 * the stack is 16-aligned at every call. */
static void emit_adapter_win64(GRJIT_Emit * e) {
  GRJIT_Asm * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const size_t params = f->param_count;
  const uint32_t in_bytes = e->c.shape.incoming_bytes;
  const unsigned ireg = e->abi->internal->reg_args;
  const GRJIT_Reg scratch = ISCRATCH(e);
  const GRJIT_Reg status = ISTATUS(e);
  const uint32_t w = in_bytes > 32 ? in_bytes : 32;
  const uint32_t frame = w + 48;
  const int32_t s_ctx = (int32_t)w + 8, s_env = (int32_t)w + 16;
  const int32_t s_args = (int32_t)w + 24, s_out = (int32_t)w + 32;
  GRJIT_Label deopted = grjit_asm_label(a);
  GRJIT_Label done = grjit_asm_label(a);
  grjit_asm_push(a, GRJIT_RBP);
  e->prologue.adapter_push_end = (uint32_t)grjit_asm_size(a);
  grjit_asm_sub_rsp(a, frame);
  e->prologue.adapter_alloc_end = (uint32_t)grjit_asm_size(a);
  e->prologue.adapter_alloc_bytes = frame;
  grjit_asm_store64(a, GRJIT_RSP, s_out, C_ARG(e, 3));
  grjit_asm_store64(a, GRJIT_RSP, s_ctx, C_ARG(e, 0));
  grjit_asm_store64(a, GRJIT_RSP, s_env, C_ARG(e, 1));
  grjit_asm_store64(a, GRJIT_RSP, s_args, C_ARG(e, 2));
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 9
  /* Planted defect 9 (tests only): callee-saved registers of Win64 are used and never
   * restored. The caller's own values must survive the call. */
  grjit_asm_mov_rr(a, GRJIT_RBX, GRJIT_RDX);
  grjit_asm_mov_rr(a, GRJIT_RSI, GRJIT_RCX);
#endif
  if (e->c.hook != NULL) {
    /* The context is still in the first C argument register. */
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.hook);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);
  }
  grjit_asm_load64(a, scratch, GRJIT_RSP, s_args);
  for (size_t i = ireg; i < params; i++) {
    grjit_asm_load64(a, GRJIT_RAX, scratch, (int32_t)(8 * i));
    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - ireg)), GRJIT_RAX);
  }
  for (size_t i = 0; i < params && i < ireg; i++) {
    grjit_asm_load64(a, IARG(e, i), scratch, (int32_t)(8 * i));
  }
  if (e->c.shape.has_result_buffer) {
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RSP, s_out);
    grjit_asm_lea(a, GRJIT_RAX, GRJIT_RAX, 8);
    if (params < ireg) grjit_asm_mov_rr(a, IARG(e, params), GRJIT_RAX);
    else grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (params - ireg)), GRJIT_RAX);
  }
  grjit_asm_load64(a, ICTX(e), GRJIT_RSP, s_ctx);
  grjit_asm_load64(a, GRJIT_R11, GRJIT_RSP, s_env);
  grjit_asm_mov_ri64(a, GRJIT_RBP, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_asm_lea_rip(a, GRJIT_RAX, e->internal);
  grjit_asm_call_r(a, GRJIT_RAX);
  /* The callee popped the stack arguments: rsp is `in_A` above the bottom. */
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, s_out - (int32_t)in_bytes);
  grjit_asm_load64(a, GRJIT_R8, GRJIT_RSP, s_ctx - (int32_t)in_bytes);
  grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_R9, GRJIT_R9);
  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset, GRJIT_R9);
  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset + 8, GRJIT_R9);
  grjit_asm_test_rr(a, status, status);
  grjit_asm_jcc(a, GRJIT_COND_NE, deopted);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_RETURNED);
  grjit_asm_jmp(a, done);
  grjit_asm_bind(a, deopted);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_DEOPT);
  GRJIT_Label not_trap = grjit_asm_label(a);
  grjit_asm_cmp_ri(a, status, (int32_t)GRJIT_STATUS_TRAPPED);
  grjit_asm_jcc(a, GRJIT_COND_NE, not_trap);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_TRAP);
  grjit_asm_bind(a, not_trap);
  GRJIT_Label not_failed = grjit_asm_label(a);
  grjit_asm_cmp_ri(a, status, (int32_t)GRJIT_STATUS_FAILED);
  grjit_asm_jcc(a, GRJIT_COND_NE, not_failed);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REBUILD_FAILED);
  grjit_asm_bind(a, not_failed);
  GRJIT_Label not_exception = grjit_asm_label(a);
  grjit_asm_cmp_ri(a, status, (int32_t)GRJIT_STATUS_EXCEPTION);
  grjit_asm_jcc(a, GRJIT_COND_NE, not_exception);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_EXCEPTION);
  grjit_asm_bind(a, not_exception);
  grjit_asm_bind(a, done);
  grjit_asm_add_rsp(a, frame - in_bytes);
  grjit_asm_pop(a, GRJIT_RBP);
  grjit_asm_ret(a);
  /* The entry hook refused (eax is its answer); nothing is pushed yet. */
  grjit_asm_bind(a, e->refuse);
  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, s_out);
  grjit_asm_store64(a, GRJIT_RCX, 0, GRJIT_RAX);
  grjit_asm_mov_ri(a, GRJIT_RAX, GRJIT_EXIT_REFUSED);
  grjit_asm_add_rsp(a, frame);
  grjit_asm_pop(a, GRJIT_RBP);
  grjit_asm_ret(a);
  e->prologue.adapter_end = (uint32_t)grjit_asm_size(a);
}

/* A callable function: the adapter, the tag, the internal entry with the
 * stack-limit check, the blocks and the stubs. */
static GRJIT_Result emit_callable(const GRJIT_Function * f, GRJIT_Emit * e,
    uint32_t frame_bytes) {
  GRJIT_Asm * a = &e->as;
  e->internal = grjit_asm_label(a);
  e->overflow = grjit_asm_label(a);
  e->ret_deopted = grjit_asm_label(a);
  e->ret_failed = grjit_asm_label(a);
  e->ret_propagate = grjit_asm_label(a);
  emit_adapter(e);
  /* The internal entry on a 16-byte boundary with the tag just before it: the
   * magic and parameter count, then the function's token. */
  static const uint8_t trap = 0xCC;
  /* An assembler that has failed (its buffer could not grow) appends nothing, so
   * its size stops changing: the loop ends there, and the status is returned. */
  while (a->status == GRJIT_ASM_OK && (grjit_asm_size(a) + GRJIT_ENTRY_TAG_BYTES) % 16 != 0) {
    grjit_asm_raw(a, &trap, 1);
  }
  const uint64_t tag[2] = {GRJIT_ENTRY_TAG_WORD(f->param_count,
      grjit_function_result_count(f)), f->token};
  grjit_asm_raw(a, tag, sizeof tag);
  grjit_asm_bind(a, e->internal);
  e->internal_offset = (uint32_t)grjit_asm_size(a);
  e->prologue.body_begin = e->internal_offset;

  uint32_t alloc = frame_bytes;
  if (e->win64) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 37
    /* Planted defect 37 (tests only): the callable frame has no outgoing area, so a
     * hook's shadow space lands on the frame's own slots. */
#else
    alloc += GRJIT_WIN64_OUTGOING;
#endif
    if (alloc > (uint32_t)INT32_MAX - GRJIT_WIN64_PAGE) {
      return GRJIT_ERR_LIMIT;
    }
  }
  grjit_asm_push(a, GRJIT_RBP);
  e->prologue.push_end = (uint32_t)grjit_asm_size(a);
  grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP);
  e->prologue.setfp_end = (uint32_t)grjit_asm_size(a);
  /* The native-stack check, in bytes (AD-28): the lowest address this frame
   * will use, against the context's limit word (r10 holds the context). Below
   * it, the chain deopts at the call site and the interpreter continues. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 10
  /* Planted defect 10 (tests only): the frame is not counted, so a function
   * whose frame crosses the limit is let in. */
  grjit_asm_mov_rr(a, GRJIT_RAX, GRJIT_RSP);
#else
  grjit_asm_lea(a, GRJIT_RAX, GRJIT_RSP, -(int32_t)alloc);
#endif
  grjit_asm_cmp_rm(a, GRJIT_RAX, ICTX(e), (int32_t)e->c.native_limit_offset);
  grjit_asm_jcc(a, GRJIT_COND_B, e->overflow);
  if (e->win64 && (uint64_t)alloc + 8 > GRJIT_WIN64_PAGE) {
    /* A frame of a page or more is probed a page at a time, as a plain function's
     * is; r10 is the context and the argument registers are live. */
    emit_stack_probe(e, alloc, GRJIT_RAX, GRJIT_R11);
  }
  grjit_asm_sub_rsp(a, alloc);
  e->prologue.alloc_end = (uint32_t)grjit_asm_size(a);
  e->prologue.alloc_bytes = alloc;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 9
  grjit_asm_mov_rr(a, GRJIT_R12, GRJIT_R10);
#endif
  grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_CTX, ICTX(e));
  grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ENV, GRJIT_R11);
  const unsigned ireg = e->abi->internal->reg_args;
  if (f->param_count < ireg) {
    if (e->c.shape.has_result_buffer) grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ARGS, IARG(e, f->param_count));
  } else {
    grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP,
        (int32_t)(16 + 8 * (f->param_count - ireg)));
    if (e->c.shape.has_result_buffer) grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ARGS, GRJIT_RAX);
  }
  for (size_t i = 0; i < f->param_count; i++) {
    if (i < ireg) {
      if (dst_is_i32_reg(e, (GRJIT_VReg)i)) {
        grjit_asm_mov32_rr(a, IARG(e, i), IARG(e, i)); /* an i32 argument is its low 32 bits */
      }
      grjit_asm_store64(a, GRJIT_RBP, grjit_emit_slot((GRJIT_VReg)i), IARG(e, i));
    } else {
      grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, (int32_t)(16 + 8 * (i - ireg)));
      if (dst_is_i32_reg(e, (GRJIT_VReg)i)) {
        grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
      }
      grjit_asm_store64(a, GRJIT_RBP, grjit_emit_slot((GRJIT_VReg)i), GRJIT_RAX);
    }
  }
  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_asm_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  emit_pending(e);
  grjit_emit_ret_deopted(e);
  grjit_emit_ret_status(e);
  grjit_emit_overflow_stub(e);
  return e->c.error;
}

GRJIT_Result grjit_emit_function(const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    bool win64, GRJIT_Features features, const GRJIT_LiveSites * live, GRJIT_Emit * e) {
  memset(e, 0, sizeof *e);
  e->win64 = win64;
  /* The CPU-feature arms are plain x86 instructions with no ABI dependence, so the Win64 flavour takes the
   * same arms as SysV under the same effective set, records the set it used and honours the caller's mask. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 91
  /* Planted defect 91 (tests only): the Win64 flavour is given no feature again, as it was before it emitted its
   * feature arms, so a host with the features never takes one there and records none. */
  e->features = win64 ? GRJIT_FEATURES_NONE : (features & GRJIT_FEATURES_ALL);
#else
  e->features = features & GRJIT_FEATURES_ALL;
#endif
  e->abi = win64 ? &grjit_x86_abi_win64 : &grjit_x86_abi_sysv;
  grjit_emit_common_init(&e->c, f,
      e->abi->internal->reg_args,
      allocator, hook, request_offset, frame_bytes, live);
  GRJIT_Asm * a = &e->as;
  grjit_asm_init(a, allocator, max_code_bytes);
  e->blocks = allocator->malloc_fn(allocator->ctx, f->block_count * sizeof *e->blocks);
  if (e->blocks == NULL) {
    return GRJIT_ERR_OOM;
  }
  for (size_t b = 0; b < f->block_count; b++) {
    e->blocks[b] = grjit_asm_label(a);
  }
  e->refuse = grjit_asm_label(a);

  if (f->callable) {
    GRJIT_Result cr = emit_callable(f, e, frame_bytes);
    if (cr != GRJIT_OK) {
      return cr;
    }
    goto finish;
  }

  /* Prologue: frame, the three fixed slots, the entry hook, the parameters. */
  uint32_t alloc = frame_bytes;
  if (win64) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 6
    /* Planted defect 6 (tests only): no outgoing area, so a callee's shadow
     * space and the stack arguments land on the frame's own slots. */
#else
    alloc += GRJIT_WIN64_OUTGOING;
#endif
  }
  if (win64 && alloc > (uint32_t)INT32_MAX - GRJIT_WIN64_PAGE) {
    /* The allocation and the probe displacements are signed 32-bit fields of
     * the encodings; a cap raised past 2 GiB is a limit, not a wrapped value. */
    return GRJIT_ERR_LIMIT;
  }
  grjit_asm_push(a, GRJIT_RBP);
  e->prologue.push_end = (uint32_t)grjit_asm_size(a);
  grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP);
  e->prologue.setfp_end = (uint32_t)grjit_asm_size(a);
  if (win64 && (uint64_t)alloc + 8 > GRJIT_WIN64_PAGE) {
    emit_stack_probe(e, alloc, GRJIT_R10, GRJIT_R11);
  }
  grjit_asm_sub_rsp(a, alloc);
  e->prologue.alloc_end = (uint32_t)grjit_asm_size(a);
  e->prologue.alloc_bytes = alloc;
  if (win64) {
    /* Microsoft x64: context, environment, args, out. */
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_CTX, GRJIT_RCX);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ENV, GRJIT_RDX);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ARGS, GRJIT_R8);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_R9);
  } else {
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_CTX, GRJIT_RDI);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ENV, GRJIT_RSI);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_ARGS, GRJIT_RDX);
    grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_RCX);
  }
  if (hook != NULL) {
    grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)hook);
    grjit_asm_call_r(a, GRJIT_RAX);
    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);
    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);
  }
  if (f->param_count != 0) {
    /* The pointer to the arguments is held in a scratch register: rsi on SysV,
     * where it is caller-saved, and r10 on Win64, where rsi is callee-saved. */
    GRJIT_Reg args_reg = win64 ? GRJIT_R10 : GRJIT_RSI;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 5
    /* Planted defect 5 (tests only): the SysV register, callee-saved on Win64,
     * is used and never restored. */
    args_reg = GRJIT_RSI;
#endif
    grjit_asm_load64(a, args_reg, GRJIT_RBP, GRJIT_SLOT_ARGS);
    for (size_t i = 0; i < f->param_count; i++) {
      grjit_asm_load64(a, GRJIT_RAX, args_reg, (int32_t)(8 * i));
      if (dst_is_i32_reg(e, (GRJIT_VReg)i)) {
        grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX); /* an i32 argument is its low 32 bits */
      }
      grjit_asm_store64(
          a, GRJIT_RBP, grjit_emit_slot((GRJIT_VReg)i), GRJIT_RAX);
    }
  }

  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_asm_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  /* A pending stub may add sites but never another pending stub. */
  emit_pending(e);
  grjit_emit_refuse_stub(e);
finish:
  grjit_asm_finish(a);
  if (e->c.error != GRJIT_OK) {
    return e->c.error;
  }
  switch (grjit_asm_status(a)) {
    case GRJIT_ASM_OK:
      break;
    case GRJIT_ASM_OOM:
      return GRJIT_ERR_OOM;
    case GRJIT_ASM_LIMIT:
      return GRJIT_ERR_LIMIT;
    default:
      return GRJIT_ERR_INTERNAL;
  }
  grjit_emit_sort_sites(&e->c);
  for (size_t i = 0; i < e->c.exception_site_count; i++)
    e->c.exception_sites[i].landing_offset = (uint32_t)grjit_asm_label_offset(a,
        e->blocks[e->c.exception_sites[i].landing_block]);
  return GRJIT_OK;
}

void grjit_emit_free(GRJIT_Emit * e) {
  if (e->c.allocator == NULL) {
    return;
  }
  e->c.allocator->free_fn(e->c.allocator->ctx, e->blocks);
  grjit_asm_free(&e->as);
  grjit_emit_common_free(&e->c);
  memset(e, 0, sizeof *e);
}
