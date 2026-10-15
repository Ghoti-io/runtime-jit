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
 * The arm64 baseline emitter: one operation at a time, every value in its
 * frame slot, fixed caller-saved scratch registers (see emit_internal.h). The
 * semantics are the IR's, not the instruction set's: shifts take the count
 * modulo 64 (`lslv`, `lsrv` and `asrv` already do), `MUL` wraps, a comparison
 * yields 0 or 1, loads of 8, 16 and 32 bits zero- or sign-extend as `LOAD` and
 * `LOAD_S` say, narrow stores store the low bits, an immediate is any 64-bit
 * value and a displacement is any `int32`.
 *
 * The numeric operations of numeric-operations.md (spec-runtime-float, story 5) are emitted in the
 * same shape: a value is a 64-bit word in its slot, `x0`/`x1`/`x2` (and `d0`/`d1`) are scratch, and
 * an `I32` or `F32` is kept zero-extended in its word. The 32-bit integer forms (`w` registers)
 * clear the upper half of their destination by the architecture, so a result needs no mask; what
 * arrives from outside (a parameter, a helper's or a callee's result, an immediate) is cut with
 * `mov w, w` at the site where it enters.
 *
 * A *callable* function (AD-28) has the adapter, the tag and the internal entry first (see
 * emit_internal.h for the convention) and the same operations after them; the calls, tail calls
 * and native calls are `emit_guest_call`, `emit_tail_call` and `emit_native_call`, each
 * documented above its sequence.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "emit_internal.h"

#include "../ir/ir_internal.h"

#include <ghoti.io/runtime-core/a/compiled.h>

#include <string.h>

#define A GRJIT_A64Asm

/* The registers that carry arguments: a helper's (the IR's six), the internal convention's eight
 * and a native's seven after the context, which are all AAPCS64's x0-x7. */
static const GRJIT_A64Reg arg_regs_internal[GRJIT_ARM64_INTERNAL_REG_ARGS] = {
    GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X2, GRJIT_A64_X3, GRJIT_A64_X4,
    GRJIT_A64_X5, GRJIT_A64_X6, GRJIT_A64_X7};

/* A value from the frame: the frame base plus a (negative) slot offset, which
 * is one `ldur` for the first 32 slots and a materialised offset beyond. */
static void load_slot(GRJIT_A64Emit * e, GRJIT_A64Reg r, int32_t offset) {
  grjit_a64_load(&e->as, GRJIT_A64_MEM_X64, r, GRJIT_A64_FP, offset);
}

static void store_slot(GRJIT_A64Emit * e, GRJIT_A64Reg r, int32_t offset) {
  grjit_a64_store(&e->as, GRJIT_A64_MEM_X64, r, GRJIT_A64_FP, offset);
}

/* Loads an operand into a register: a register's slot, or an immediate. */
static void load_operand(GRJIT_A64Emit * e, GRJIT_A64Reg r, const GRJIT_Operand * o) {
  if (o->kind == GRJIT_OPERAND_VREG) {
    load_slot(e, r, grjit_emit_slot(o->vreg));
  } else {
    grjit_a64_mov_ri(&e->as, r, (uint64_t)o->imm);
  }
}

static void store_result(GRJIT_A64Emit * e, GRJIT_VReg dst, GRJIT_A64Reg r) {
  store_slot(e, r, grjit_emit_slot(dst));
}

static GRJIT_A64Cond cmp_cond(GRJIT_Cmp c) {
  switch (c) {
    case GRJIT_CMP_EQ: return GRJIT_A64_EQ;
    case GRJIT_CMP_NE: return GRJIT_A64_NE;
    case GRJIT_CMP_LT: return GRJIT_A64_LT;
    case GRJIT_CMP_LE: return GRJIT_A64_LE;
    case GRJIT_CMP_GT: return GRJIT_A64_GT;
    case GRJIT_CMP_GE: return GRJIT_A64_GE;
    case GRJIT_CMP_ULT: return GRJIT_A64_LO;
    case GRJIT_CMP_ULE: return GRJIT_A64_LS;
    case GRJIT_CMP_UGT: return GRJIT_A64_HI;
    case GRJIT_CMP_UGE:
    case GRJIT_CMP_COUNT:
      break;
  }
  return GRJIT_A64_HS;
}

/* A register kept zero-extended in its word: an i32, and an f32 for the same reason (its bits are 32;
 * a word that arrives from a caller, a callee or memory is cut to them). */
static bool vreg_is_narrow(const GRJIT_A64Emit * e, GRJIT_VReg v) {
  return grjit_type_is_narrow(e->c.f->vregs[v].type);
}

static bool dst_is_i32(const GRJIT_A64Emit * e, const GRJIT_Op * op) {
  return op->dst != GRJIT_NO_VREG && e->c.f->vregs[op->dst].type == GRJIT_TYPE_I32;
}

static bool operand_is_i32(const GRJIT_A64Emit * e, const GRJIT_Operand * o) {
  return o->kind == GRJIT_OPERAND_VREG && e->c.f->vregs[o->vreg].type == GRJIT_TYPE_I32;
}

static bool operand_is_float(const GRJIT_A64Emit * e, const GRJIT_Operand * o) {
  if (o->kind == GRJIT_OPERAND_F32_IMM || o->kind == GRJIT_OPERAND_F64_IMM) {
    return true;
  }
  return o->kind == GRJIT_OPERAND_VREG && grjit_type_is_float(e->c.f->vregs[o->vreg].type);
}

static bool operand_is_double(const GRJIT_A64Emit * e, const GRJIT_Operand * o) {
  return o->kind == GRJIT_OPERAND_F64_IMM ||
         (o->kind == GRJIT_OPERAND_VREG && e->c.f->vregs[o->vreg].type == GRJIT_TYPE_F64);
}

static bool vreg_is_f64(const GRJIT_A64Emit * e, GRJIT_VReg v) {
  return e->c.f->vregs[v].type == GRJIT_TYPE_F64;
}

static bool call_has_float(const GRJIT_A64Emit * e, const GRJIT_Op * op) {
  for (size_t i = 0; i < op->arg_count; i++) {
    if (operand_is_float(e, &op->args[i])) {
      return true;
    }
  }
  return op->dst != GRJIT_NO_VREG && grjit_type_is_float(e->c.f->vregs[op->dst].type);
}

/* A float operand into a vector register: the slot (an `s` or `d` load, which clears the rest of the
 * register), or an immediate's bits through the general register `tmp` (`fmov s, w` takes the low 32
 * bits of an f32 immediate). Only `v0` to `v7` are ever named. */
static void load_float(GRJIT_A64Emit * e, GRJIT_A64VReg v, const GRJIT_Operand * o, bool dbl, GRJIT_A64Reg tmp) {
  A * a = &e->as;
  if (o->kind == GRJIT_OPERAND_VREG) {
    grjit_a64_load(a, dbl ? GRJIT_A64_MEM_F64 : GRJIT_A64_MEM_F32, GRJIT_A64_VREG_AS_REG(v), GRJIT_A64_FP,
        grjit_emit_slot(o->vreg));
  } else {
    grjit_a64_mov_ri(a, tmp, (uint64_t)o->imm);
    grjit_a64_fmov_from_gpr(a, dbl ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, v, tmp);
  }
}

/* A float result into its slot as a whole word. An f64 is stored as it is; an f32 is taken out of its
 * register with `fmov w, s`, which clears the upper half of the general register, and stored as a word,
 * so an f32 is zero-extended in its slot by construction (a store of `s` alone would leave the upper half
 * of the slot as it was). */
static void store_float(GRJIT_A64Emit * e, GRJIT_VReg dst, GRJIT_A64VReg v, bool dbl) {
  A * a = &e->as;
  if (dbl) {
    grjit_a64_store(a, GRJIT_A64_MEM_F64, GRJIT_A64_VREG_AS_REG(v), GRJIT_A64_FP, grjit_emit_slot(dst));
  } else {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 81
    /* Planted defect 81 (tests only): an f32 result is stored with `str s`, so the upper half of its slot
     * keeps what the slot held before. */
    grjit_a64_store(a, GRJIT_A64_MEM_F32, GRJIT_A64_VREG_AS_REG(v), GRJIT_A64_FP, grjit_emit_slot(dst));
#else
    grjit_a64_fmov_to_gpr(a, GRJIT_A64_SINGLE, GRJIT_A64_X0, v);
    store_result(e, dst, GRJIT_A64_X0);
#endif
  }
}

static void emit_load(GRJIT_A64Emit * e, const GRJIT_Op * op, bool sign) {
  if (op->index.kind == GRJIT_OPERAND_VREG) {
    load_operand(e, GRJIT_A64_X0, &op->index);
    for (unsigned s = 1; s < op->scale; s <<= 1) {
      grjit_a64_add_uxtx(&e->as, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X0);
    }
    load_operand(e, GRJIT_A64_X1, &op->a);
    grjit_a64_add_uxtx(&e->as, GRJIT_A64_X1, GRJIT_A64_X1, GRJIT_A64_X0);
  } else {
    load_operand(e, GRJIT_A64_X1, &op->a);
  }
  GRJIT_A64Mem kind;
  switch (op->width) {
    case 8: kind = sign ? GRJIT_A64_MEM_S8 : GRJIT_A64_MEM_U8; break;
    case 16: kind = sign ? GRJIT_A64_MEM_S16 : GRJIT_A64_MEM_U16; break;
    case 32: kind = sign ? GRJIT_A64_MEM_S32 : GRJIT_A64_MEM_U32; break;
    default: kind = GRJIT_A64_MEM_X64; break;
  }
  grjit_a64_load(&e->as, kind, GRJIT_A64_X0, GRJIT_A64_X1, op->disp);
  store_result(e, op->dst, GRJIT_A64_X0);
}

static void emit_store(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  if (op->index.kind == GRJIT_OPERAND_VREG) {
    load_operand(e, GRJIT_A64_X16, &op->index);
    for (unsigned s = 1; s < op->scale; s <<= 1) {
      grjit_a64_add_uxtx(&e->as, GRJIT_A64_X16, GRJIT_A64_X16, GRJIT_A64_X16);
    }
    load_operand(e, GRJIT_A64_X1, &op->a);
    grjit_a64_add_uxtx(&e->as, GRJIT_A64_X1, GRJIT_A64_X1, GRJIT_A64_X16);
  } else {
    load_operand(e, GRJIT_A64_X1, &op->a);
  }
  load_operand(e, GRJIT_A64_X0, &op->b);
  GRJIT_A64Mem kind;
  switch (op->width) {
    case 8: kind = GRJIT_A64_MEM_U8; break;
    case 16: kind = GRJIT_A64_MEM_U16; break;
    case 32: kind = GRJIT_A64_MEM_U32; break;
    default: kind = GRJIT_A64_MEM_X64; break;
  }
  grjit_a64_store(&e->as, kind, GRJIT_A64_X0, GRJIT_A64_X1, op->disp);
}

static void emit_binary(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  load_operand(e, GRJIT_A64_X0, &op->a);
  load_operand(e, GRJIT_A64_X1, &op->b);
  GRJIT_OpKind kind = op->kind;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 4
  /* Planted defect 4 (tests only): a logical and an arithmetic right shift
   * trade places in the arm64 emitter. The differential must catch it under
   * qemu-aarch64. */
  if (kind == GRJIT_OP_SHR) {
    kind = GRJIT_OP_SAR;
  } else if (kind == GRJIT_OP_SAR) {
    kind = GRJIT_OP_SHR;
  }
#endif
  GRJIT_A64Alu alu;
  switch (kind) {
    case GRJIT_OP_ADD: alu = GRJIT_A64_ADD; break;
    case GRJIT_OP_SUB: alu = GRJIT_A64_SUB; break;
    case GRJIT_OP_MUL: alu = GRJIT_A64_MUL; break;
    case GRJIT_OP_AND: alu = GRJIT_A64_AND; break;
    case GRJIT_OP_OR: alu = GRJIT_A64_ORR; break;
    case GRJIT_OP_XOR: alu = GRJIT_A64_EOR; break;
    case GRJIT_OP_SHL: alu = GRJIT_A64_LSLV; break;
    case GRJIT_OP_SHR: alu = GRJIT_A64_LSRV; break;
    default: alu = GRJIT_A64_ASRV; break;
  }
  grjit_a64_alu(a, alu, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  store_result(e, op->dst, GRJIT_A64_X0);
}

static void emit_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  /* In a callable function every call that can reach a GC point records where the walk starts
   * first (AD-28), so a collection under the helper sees this frame and the compiled frames
   * below it. It uses x15 and x16, which the arguments and the call are loaded over after. */
  GRJIT_Label walk_ret = 0;
  const bool record = e->c.callable && op->attr == GRJIT_CALL_GC_POINT;
  if (record) {
    walk_ret = grjit_a64_label(a);
    grjit_a64_emit_store_walk_cell(e, walk_ret);
  }
  if (call_has_float(e, op)) {
    /* AAPCS64 numbers the integer-class arguments x0-x5 among themselves and the float ones d0-d7 among
     * themselves (an f32 in the low 32 bits, `s`n): (i64, f64, i64, f32) is x0, d0, x1, s1. The builder's
     * limit of six arguments means none is passed on the stack. A float immediate goes through x16, which
     * the call address is loaded into after. */
    unsigned integer = 0;
    unsigned floating = 0;
    for (size_t i = 0; i < op->arg_count; i++) {
      const GRJIT_Operand * o = &op->args[i];
      if (!operand_is_float(e, o)) {
        load_operand(e, arg_regs_internal[integer++], o);
        continue;
      }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 84
      /* Planted defect 84 (tests only): the float and integer arguments are numbered jointly, as Win64
       * numbers them, so the second float of (i64, f64, i64, f32) goes to s3. */
      const GRJIT_A64VReg v = (GRJIT_A64VReg)(i < 8 ? i : 7);
      (void)floating;
#else
      const GRJIT_A64VReg v = (GRJIT_A64VReg)floating++;
#endif
      load_float(e, v, o, operand_is_double(e, o), GRJIT_A64_X16);
    }
  } else {
    for (size_t i = 0; i < op->arg_count; i++) {
      load_operand(e, arg_regs_internal[i], &op->args[i]);
    }
  }
  grjit_a64_mov_ri(a, GRJIT_A64_X16, op->address);
  grjit_a64_blr(a, GRJIT_A64_X16);
  if (record) {
    grjit_a64_bind(a, walk_ret);
  }
  if (op->attr == GRJIT_CALL_GC_POINT) {
    /* The return address is the site. */
    const GRJIT_FrameState * s = &e->c.f->states[op->state];
    grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), op->site_kind,
        s->identity, e->c.live_cursor++, op->state);
  }
  if (op->dst != GRJIT_NO_VREG) {
    const GRJIT_Type result = e->c.f->vregs[op->dst].type;
    if (grjit_type_is_float(result)) {
      /* A float comes back in d0: an f32 in its low 32 bits (whatever is above them is not ours: `fmov w`
       * takes 32 and clears the rest), an f64 in its 64. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 85
      /* Planted defect 85 (tests only): an f32 result is taken as `fmov x0, d0`, so whatever the callee
       * left in bits 32-63 of d0 lands in the slot. */
      grjit_a64_fmov_to_gpr(a, GRJIT_A64_DOUBLE, GRJIT_A64_X0, GRJIT_A64_V0);
#else
      grjit_a64_fmov_to_gpr(a, result == GRJIT_TYPE_F64 ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_X0,
          GRJIT_A64_V0);
#endif
    } else if (vreg_is_narrow(e, op->dst)) {
      /* A helper's 32-bit result: the upper half of x0 is not ours. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 93
      /* Planted defect 93 (tests only): a helper's 32-bit result is not cut, so what the callee left above bit
       * 31 of x0 lands in the slot. */
#else
      grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
#endif
    }
    store_result(e, op->dst, GRJIT_A64_X0);
  }
}


/* ---- Calls between compiled functions, tail calls and natives (AD-28) -------------------- */

/* `rd = base + offset` for any offset a frame can have: one instruction when it fits an
 * immediate, two when it needs the shifted high part, and from 16 MiB up `x17` holds the
 * magnitude and one extended-register instruction applies it (the native-stack check of a
 * native that declares up to the 1 GiB the limits allow, and a frame up to the same ceiling,
 * are as legal here as on x86-64). `base` is `sp` or `x29`; `rd` may be `sp`. */
static void lea(GRJIT_A64Emit * e, GRJIT_A64Reg rd, GRJIT_A64Reg base, int64_t offset) {
  A * a = &e->as;
  const bool neg = offset < 0;
  const uint64_t mag = neg ? (uint64_t)(-offset) : (uint64_t)offset;
  if (mag >= (UINT64_C(1) << 24)) {
    grjit_a64_mov_ri(a, GRJIT_A64_X17, mag);
    (neg ? grjit_a64_sub_uxtx : grjit_a64_add_uxtx)(a, rd, base, GRJIT_A64_X17);
    return;
  }
  GRJIT_A64Reg from = base;
  if (mag >> 12) {
    (neg ? grjit_a64_sub_imm : grjit_a64_add_imm)(a, rd, from, (uint32_t)(mag >> 12), true);
    from = rd;
  }
  if ((mag & 0xFFFu) != 0 || from == base) {
    (neg ? grjit_a64_sub_imm : grjit_a64_add_imm)(a, rd, from, (uint32_t)(mag & 0xFFFu), false);
  }
}

/* The stack-argument bytes the internal convention puts for `n` arguments. */
static uint32_t stack_bytes(size_t n) {
  return grjit_stack_arg_bytes(n, GRJIT_ARM64_INTERNAL_REG_ARGS);
}

/* Finds the callee's entry for a call or a tail call and leaves it in `x16` and in the
 * entry-save slot, or goes to the returned exit label: through the slot (one compare; the
 * compile hook for an empty slot; an exit for a refused one) or through the code pointer
 * (the registered, not retired, tagged, aligned check). `live` is the first of the
 * operation's liveness sites and `exit_live` the one the exit's stub is made from. */
static GRJIT_Label emit_dispatch(
    GRJIT_A64Emit * e, const GRJIT_Op * op, size_t live, size_t exit_live) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const bool slot = op->kind == GRJIT_OP_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_SLOT;
  const size_t n = op->arg_count;
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(GRJIT_SHAPE_REGS(f, e->c.shape), e->c.shape);
  GRJIT_Label exit = grjit_a64_label(a);
  GRJIT_Pending p;
  memset(&p, 0, sizeof p);
  p.op = op;
  if (slot) {
    GRJIT_Label slow = grjit_a64_label(a);
    GRJIT_Label back = grjit_a64_label(a);
    if (op->address != 0) grjit_a64_mov_ri(a, GRJIT_A64_X16, op->address);
    else load_operand(e, GRJIT_A64_X16, &op->a);
    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X16, GRJIT_A64_X16, 0);
    /* One compare tells compiled code (above one) from empty (zero) and from refused (one). */
    grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);
    grjit_a64_bcond(a, GRJIT_A64_LS, slow);
    grjit_a64_bind(a, back);
    p.kind = GRJIT_PENDING_CALL_SLOW;
    p.entry = slow;
    p.back = back;
    p.exit = exit;
    p.live_index = live;
    grjit_emit_add_pending(&e->c, &p);
  } else {
    load_operand(e, GRJIT_A64_X16, &op->a);
    store_slot(e, GRJIT_A64_X16, entry_slot);
    /* The target must be the internal entry of code registered in this context: an address
     * that is not is never entered. */
    load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
    grjit_a64_mov_rr(a, GRJIT_A64_X1, GRJIT_A64_X16);
    grjit_a64_mov_ri(a, GRJIT_A64_X2, op->callee);
    grjit_a64_mov_ri(a, GRJIT_A64_X3, GRJIT_CALL_SIGNATURE(n,
        op->kind == GRJIT_OP_TAIL_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_PTR
            ? op->callee_result_count : op->result_count));
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)grjit_call_target_ok);
    grjit_a64_blr(a, GRJIT_A64_X16);
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    grjit_a64_cbz(a, GRJIT_A64_X0, exit);
  }
  memset(&p, 0, sizeof p);
  p.kind = GRJIT_PENDING_CALL_EXIT;
  p.entry = exit;
  p.op = op;
  p.live_index = exit_live;
  grjit_emit_add_pending(&e->c, &p);
  if (slot) {
    store_slot(e, GRJIT_A64_X16, entry_slot);
  }
  return exit;
}

/* The arguments of a call or a tail call, into the arguments area. */
static void emit_stage_arguments(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  const GRJIT_Function * f = e->c.f;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  for (size_t i = 0; i < op->arg_count; i++) {
    load_operand(e, GRJIT_A64_X0, &op->args[i]);
    store_slot(e, GRJIT_A64_X0, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
  }
}

/* The hook call of a push or a tail call, after the arguments are staged: stores the walk
 * start for the site that ends at `ret`, then calls `hook(ctx, callee, area, n)`. */
static void emit_frame_hook(GRJIT_A64Emit * e, const GRJIT_Op * op, GRJIT_Label ret, uint64_t hook) {
  A * a = &e->as;
  const size_t regs = GRJIT_SHAPE_REGS(e->c.f, e->c.shape);
  grjit_a64_emit_store_walk_cell(e, ret);
  load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    load_slot(e, GRJIT_A64_X1, GRJIT_SLOT_ENV);
  else
    load_operand(e, GRJIT_A64_X1, &op->environment);
  grjit_a64_mov_ri(a, GRJIT_A64_X2, op->callee);
  lea(e, GRJIT_A64_X3, GRJIT_A64_FP, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, 0));
  grjit_a64_mov_ri(a, GRJIT_A64_X4, op->arg_count);
  grjit_a64_mov_ri(a, GRJIT_A64_X5,
      op->kind == GRJIT_OP_TAIL_CALL_SLOT || op->kind == GRJIT_OP_TAIL_CALL_PTR
          ? op->callee_result_count : op->result_count);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, hook);
  grjit_a64_blr(a, GRJIT_A64_X16);
}

/* A call to another compiled function (AD-28), through an entry slot or a code pointer. In
 * order: find the entry; copy the arguments into the frame's arguments area, where the push
 * hook reads them and a moving collector updates them; push the callee's guest frame (the
 * frame-push GC point); call through the internal convention; test the status for DEOPTED;
 * store the result; pop. Every way out before the call is an exit through the exit state;
 * every way out after it is DEOPTED. */
static void emit_guest_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
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
  GRJIT_Label push_ret = grjit_a64_label(a);
  emit_frame_hook(e, op, push_ret, (uint64_t)(uintptr_t)f->hooks.push);
  grjit_a64_bind(a, push_ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);

  /* The call. The stack arguments are made just before it, at `[sp]`, and popped by the
   * callee, which keeps `sp` 16-aligned (the area is whole 16-byte units). */
  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
  const size_t call_args = n + (op->result_count > 1 ? 1u : 0u);
  const uint32_t area = stack_bytes(call_args);
  if (area != 0) {
    grjit_a64_sub_sp(a, area);
    for (size_t i = ireg; i < n; i++) {
      load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));
    }
    if (op->result_count > 1 && n >= ireg) {
      lea(e, GRJIT_A64_X10, GRJIT_A64_FP,
          GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, e->c.shape.args_area));
      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,
          (int32_t)(8 * (n - ireg)));
    }
  }
  for (size_t i = 0; i < n && i < ireg; i++) {
    load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
  }
  if (n < ireg) {
    if (op->result_count > 1) lea(e, arg_regs_internal[n], GRJIT_A64_FP,
        GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, e->c.shape.args_area));
  }
  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    load_slot(e, GRJIT_A64_X8, GRJIT_SLOT_ENV);
  else
    load_operand(e, GRJIT_A64_X8, &op->environment);
  load_slot(e, GRJIT_A64_X16, entry_slot);
  grjit_a64_blr(a, GRJIT_A64_X16);
  /* The return address is the site: this frame, with the callee running. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_CALL, st->identity, live + 1, op->state, op);
  if (op->landing_target != GRJIT_NO_BLOCK) {
    grjit_emit_add_exception_site(&e->c, (uint32_t)grjit_a64_size(a), op->landing_target);
    GRJIT_Label not_exception = grjit_a64_label(a);
    GRJIT_Label propagate_exception = grjit_a64_label(a);
    grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_EXCEPTION);
    grjit_a64_bcond(a, GRJIT_A64_NE, not_exception);
    load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
    load_slot(e, GRJIT_A64_X1, GRJIT_SLOT_ENV);
    grjit_a64_mov_ri(a, GRJIT_A64_X2, op->callee);
    grjit_a64_mov_ri(a, GRJIT_A64_X3, op->landing_target);
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)f->hooks.catch_exception);
    grjit_a64_blr(a, GRJIT_A64_X16);
    grjit_a64_cbnz(a, GRJIT_A64_X0, propagate_exception);
    grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_RETURNED);
    grjit_a64_b(a, e->blocks[op->landing_target]);
    grjit_a64_bind(a, propagate_exception);
    grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_EXCEPTION);
    grjit_a64_b(a, e->ret_propagate);
    grjit_a64_bind(a, not_exception);
  }
  grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);
  if (op->result_count != 0) {
    if (vreg_is_narrow(e, op->results[0])) {
      grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0); /* the callee returned a word; an i32 or f32 is its low half */
    }
    store_result(e, op->results[0], GRJIT_A64_X0);
    for (size_t i = 1; i < op->result_count; i++) {
      load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots,
          e->c.shape.args_area + i - 1));
      if (vreg_is_narrow(e, op->results[i])) grjit_a64_mov32_rr(a, GRJIT_A64_X10, GRJIT_A64_X10);
      store_result(e, op->results[i], GRJIT_A64_X10);
    }
  }
  /* The call is complete: pop the callee's guest frame. */
  load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)f->hooks.pop);
  grjit_a64_blr(a, GRJIT_A64_X16);
}

/* A tail call (AD-28): the callee replaces this function's frame, native and guest, and
 * returns to this function's caller. In order: find the entry (as a call does); the arguments
 * into the arguments area; the engine's `tail` hook, which replaces the guest frame as one
 * step and is the frame-push GC point; then, with no GC point until the jump, the replacement
 * of the native frame.
 *
 * Let `in_A` be the bytes of stack arguments this function takes and `in_T` those the callee
 * takes (whole 16-byte units). `R = x29 + 16 + in_A` is where the original caller expects `sp`
 * after the return, so the callee's `add sp, sp, #in_T` must leave it there: the callee is
 * entered with `sp = R - in_T`, its stack arguments at `[sp, R)`, and with `x29` and `x30` as
 * they were for this function (its caller's frame record and the return address, which the
 * callee's prologue pushes again). `sp` moves last and nothing below it is read after, so no
 * signal can see a half-moved frame.
 *
 * The arguments area is the staging place: its end is at or below the lowest address the copy
 * writes (the shape's `pad` makes it so; the shared shape counts a return-address word arm64
 * does not have, so it is conservative by up to one slot), so no copy overwrites a source and
 * the copies are in any order. Everything that is read from this frame after the first write
 * (the context, the return address, the caller's saved base) is loaded first. The walk is
 * unchanged: after the jump the callee's frame record holds this function's saved base and
 * return address, so it is one compiled frame beside one guest frame, as any callee of the
 * original caller is. */
static void emit_tail_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_FrameState * st = &f->states[op->state];
  const size_t n = op->arg_count;
  const size_t regs = GRJIT_SHAPE_REGS(f, e->c.shape);
  const int32_t entry_slot = GRJIT_ENTRY_SAVE_SLOT(regs, e->c.shape);
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += 2; /* the hook and the exit */

  GRJIT_Label exit = emit_dispatch(e, op, live, live + 1);
  emit_stage_arguments(e, op);

  GRJIT_Label hook_ret = grjit_a64_label(a);
  emit_frame_hook(e, op, hook_ret, (uint64_t)(uintptr_t)f->hooks.tail);
  grjit_a64_bind(a, hook_ret);
  /* The hook is a frame-push GC point of this frame, whose guest frame is still this
   * function's: the state is the caller's, and the area is in the map. */
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),
      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);
  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);

  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 24
  /* Planted defect 24 (tests only): the tail call is made as a call, and its frame kept: the
   * answer is the same and the native stack grows with every tail call, which no result shows
   * and the stack pointer does. */
  {
    const uint32_t area_bytes = stack_bytes(n + (e->c.shape.has_result_buffer ? 1u : 0u));
    if (area_bytes != 0) {
      grjit_a64_sub_sp(a, area_bytes);
      for (size_t i = ireg; i < n; i++) {
        load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
        grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));
      }
      if (e->c.shape.has_result_buffer) {
        load_slot(e, GRJIT_A64_X10, GRJIT_SLOT_ARGS);
        grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,
            (int32_t)(8 * (n - ireg)));
      }
    }
    for (size_t i = 0; i < n && i < ireg; i++) {
      load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
    }
    if (e->c.shape.has_result_buffer && n < ireg) load_slot(e, arg_regs_internal[n], GRJIT_SLOT_ARGS);
    load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    load_slot(e, GRJIT_A64_X8, GRJIT_SLOT_ENV);
  else
    load_operand(e, GRJIT_A64_X8, &op->environment);
    load_slot(e, GRJIT_A64_X16, entry_slot);
    grjit_a64_blr(a, GRJIT_A64_X16);
    grjit_a64_emit_callable_epilogue(e);
    return;
  }
#endif
  /* The frame replacement. */
  const int64_t in_a = e->c.shape.incoming_bytes;
  const int64_t in_t = stack_bytes(n + (e->c.shape.has_result_buffer ? 1u : 0u));
  const int64_t sp_new = 16 + in_a - in_t; /* from x29 */
  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  if (op->environment.kind == GRJIT_OPERAND_NONE)
    load_slot(e, GRJIT_A64_X8, GRJIT_SLOT_ENV);
  else
    load_operand(e, GRJIT_A64_X8, &op->environment);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X30, GRJIT_A64_FP, 8);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 0);
  /* The result buffer's pointer and the entry are read before the copy, which can run over
   * the frame's fixed slots and vreg slots when the callee takes more stack arguments than
   * this function does; the context and the environment above already were. */
  if (e->c.shape.has_result_buffer) load_slot(e, GRJIT_A64_X11, GRJIT_SLOT_ARGS);
  load_slot(e, GRJIT_A64_X16, entry_slot);
  size_t copy_to = n;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 25
  /* Planted defect 25 (tests only): the last stack argument is not copied, so the callee
   * reads what the destination held. */
  if (copy_to > ireg) {
    copy_to--;
  }
#endif
  for (size_t i = ireg; i < copy_to; i++) {
    load_slot(e, GRJIT_A64_X10, GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
    store_slot(e, GRJIT_A64_X10, (int32_t)(sp_new + 8 * (int64_t)(i - ireg)));
  }
  if (e->c.shape.has_result_buffer && n >= ireg) {
    store_slot(e, GRJIT_A64_X11,
        (int32_t)(sp_new + 8 * (int64_t)(n - ireg)));
  }
  for (size_t i = 0; i < n && i < ireg; i++) {
    load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.area_slots, i));
  }
  if (e->c.shape.has_result_buffer && n < ireg) grjit_a64_mov_rr(a, arg_regs_internal[n], GRJIT_A64_X11);
  lea(e, GRJIT_A64_SP, GRJIT_A64_FP, sp_new);
  grjit_a64_mov_rr(a, GRJIT_A64_FP, GRJIT_A64_X15);
  grjit_a64_br(a, GRJIT_A64_X16);
}

/* A call to a registered native (AD-28, AD-17; natives.h for the contract). In order, and
 * after it nothing is reordered:
 *
 *  1. the native-stack check: `sub x16, sp, #(S + stack_bytes)` against the context's limit
 *     word (zero means none); below it is an exit before the call, the native is not called
 *     and the interpreter makes the call itself. `S` is the stack-argument area, below. The
 *     one formula is the same on every target; on arm64 the return address is not on the
 *     stack, so a native has eight bytes more than it declared (natives.h);
 *  2. the walk start, first: the frame base and the address after the call go in the
 *     context's cell before the area is made, before any argument is moved and before the
 *     call, so every GC point the native reaches sees this frame and the compiled frames
 *     below it;
 *  3. the arguments by AAPCS64: with `N = 1 + n` words (the context first), the first eight
 *     in `x0`-`x7`, the rest, `k = N - 8`, at `[sp + 8 i]` in an area of `S = round_up_16(8
 *     k)` bytes made by `sub sp, sp, #S`, so `sp` is 16-aligned at the call. Every operand
 *     is read from a frame slot or is an immediate, so the order of the loads cannot clobber
 *     one;
 *  4. the call, through `x16`; the caller pops (`add sp, sp, #S`);
 *  5. the result is stored to `dst` before the status is looked at, so the state after the
 *     call sees it; with a status, a non-zero `w1` (the upper half of `x1` is the pair's
 *     `reserved` word, which is not looked at) is the exit through the state after the call;
 *  6. nothing is assumed preserved: the context, every operand and every live value is read
 *     from the frame again, and every caller-saved register is clobbered.
 *
 * The address after the call is the call's site: this frame in the call's state, with the
 * stack map of what is live after the call (the result excluded). The arguments are not
 * mapped: a native receives raw words, and what it holds across a GC point it protects
 * itself. */
static void emit_native_call(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const GRJIT_NativeDesc * d = grjit_native_table_get(f->natives, op->native);
  if (d == NULL || d->param_count != op->arg_count) {
    /* The verifier refused this already; a descriptor cannot change under a verified
     * function (the table is append-only), so this is the library's own invariant. */
    e->c.error = GRJIT_ERR_INTERNAL;
    return;
  }
  const bool status = (d->flags & GRJIT_NATIVE_STATUS) != 0;
  const size_t n = op->arg_count;
  const size_t words = n + 1; /* the context first */
  const unsigned creg = GRJIT_ARM64_INTERNAL_REG_ARGS; /* AAPCS64 passes eight words in registers too */
  const size_t stack_words = words > creg ? words - creg : 0;
  uint32_t area = (uint32_t)((stack_words * 8 + 15) / 16 * 16);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 21
  /* Planted defect 21 (tests only): the stack-argument area is not rounded to sixteen bytes, so
   * an odd count of stack words leaves `sp` misaligned at the call. */
  area = (uint32_t)(stack_words * 8);
#endif
  const size_t live = e->c.live_cursor;
  e->c.live_cursor += status ? 3 : 2; /* the call, the exit before it, the status exit */
  const GRJIT_FrameState * st = &f->states[op->state];

  /* 1. The check. */
  GRJIT_Label exit = grjit_a64_label(a);
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
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 28
  /* Planted defect 28 (tests only): the check leaves out the stack-argument area. */
  need = d->stack_bytes;
#endif
  lea(e, GRJIT_A64_X16, GRJIT_A64_SP, -(int64_t)need);
  load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_CTX);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_X15, (int32_t)e->c.native_limit_offset);
  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);
  grjit_a64_bcond(a, GRJIT_A64_LO, exit);

  /* 2. The walk start, before anything moves. */
  GRJIT_Label ret = grjit_a64_label(a);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 20
  /* Planted defect 20 (tests only): the walk start is stored after the call, so a collection
   * the native triggers finds the previous call's. */
#else
  grjit_a64_emit_store_walk_cell(e, ret);
#endif

  /* 3. The arguments. */
  if (area != 0) {
    grjit_a64_sub_sp(a, area);
  }
  for (size_t i = creg - 1; i < n; i++) {
    /* IR argument i is C argument i + 1; the last register word is the one IR argument before. */
    load_operand(e, GRJIT_A64_X10, &op->args[i]);
    grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,
        (int32_t)(8 * (i - (creg - 1))));
  }
  load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
  for (size_t i = 0; i < n && i + 1 < creg; i++) {
    load_operand(e, arg_regs_internal[i + 1], &op->args[i]);
  }

  /* 4. The call; the address after it is the site. */
  grjit_a64_mov_ri(a, GRJIT_A64_X16, d->address);
  grjit_a64_blr(a, GRJIT_A64_X16);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 20
  /* The late store, through registers that leave the native's x0 and x1 alone. */
  grjit_a64_adr(a, GRJIT_A64_X16, ret);
  load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_CTX);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_FP, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X16, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset + 8);
#endif
  grjit_a64_bind(a, ret);
  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GC_POINT_CALL,
      st->identity, live, op->state, op);
  if (area != 0) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 22
    /* Planted defect 22 (tests only): the caller does not pop the stack arguments, so `sp`
     * drifts by their area with every call. */
#else
    grjit_a64_add_sp(a, area);
#endif
  }

  /* 5. The result before the status; a non-zero status leaves. */
  if (op->dst != GRJIT_NO_VREG) {
    store_result(e, op->dst, GRJIT_A64_X0);
  }
  if (status) {
    GRJIT_Label leave = grjit_a64_label(a);
    GRJIT_Pending p;
    memset(&p, 0, sizeof p);
    p.kind = GRJIT_PENDING_NATIVE_STATUS;
    p.entry = leave;
    p.op = op;
    p.live_index = live + 2;
    grjit_emit_add_pending(&e->c, &p);
    /* The status is the 32 bits of w1: the half above is the pair's `reserved` word, which a
     * native may leave as it likes (natives.h), so it is cleared and never read. */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 26
    /* Planted defect 26 (tests only): the status is tested on all of x1, so garbage in the
     * upper half exits a call whose status is zero. */
#else
    grjit_a64_mov32_rr(a, GRJIT_A64_X1, GRJIT_A64_X1);
#endif
    grjit_a64_cbnz(a, GRJIT_A64_X1, leave);
  }
}

/* ---- The integer operations of numeric-operations.md (spec-runtime-float, story 5) ---------------
 *
 * The width of an operation is its destination's: an i32 operation is made of real `w` instructions,
 * which take their shift and rotate counts modulo 32 and clear the upper half of the destination, so
 * the result is zero-extended in its slot by construction and no mask follows it. Nothing here
 * faults: `sdiv` and `udiv` answer a zero divisor with 0 and `MIN / -1` with `MIN`, so a division
 * whose guard a frontend forgot is a wrong answer, not a signal (the verifier refuses it, and the
 * emitter does not add or move a guard). `rem_s` is `sdiv` then `msub`, which gives `MIN % -1 = 0`
 * with no special case. */

/* ADD .. SAR on i32 operands. */
static void emit_binary32(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  GRJIT_Operand left = op->a;
  GRJIT_Operand right = op->b;
  load_operand(e, GRJIT_A64_X0, &left);
  load_operand(e, GRJIT_A64_X1, &right);
  GRJIT_A64Alu alu;
  switch (op->kind) {
    case GRJIT_OP_ADD: alu = GRJIT_A64_ADD; break;
    case GRJIT_OP_SUB: alu = GRJIT_A64_SUB; break;
    case GRJIT_OP_MUL: alu = GRJIT_A64_MUL; break;
    case GRJIT_OP_AND: alu = GRJIT_A64_AND; break;
    case GRJIT_OP_OR: alu = GRJIT_A64_ORR; break;
    case GRJIT_OP_XOR: alu = GRJIT_A64_EOR; break;
    case GRJIT_OP_SHL: alu = GRJIT_A64_LSLV; break;
    case GRJIT_OP_SHR: alu = GRJIT_A64_LSRV; break;
    default: alu = GRJIT_A64_ASRV; break;
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 69
  /* Planted defect 69 (tests only): an i32 addition, subtraction and multiplication are emitted with
   * 64-bit registers, so a carry out of bit 31 lands in the upper half, which an i32 must keep zero. */
  if (alu == GRJIT_A64_ADD || alu == GRJIT_A64_SUB || alu == GRJIT_A64_MUL) {
    grjit_a64_alu(a, alu, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
    store_result(e, op->dst, GRJIT_A64_X0);
    return;
  }
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 70
  /* Planted defect 70 (tests only): an i32 shift is the 64-bit instruction (the count is taken
   * modulo 64, not 32), cut to 32 bits afterwards. */
  if (alu == GRJIT_A64_LSLV || alu == GRJIT_A64_LSRV || alu == GRJIT_A64_ASRV) {
    grjit_a64_alu(a, alu, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    store_result(e, op->dst, GRJIT_A64_X0);
    return;
  }
#endif
  grjit_a64_alu32(a, alu, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* A comparison of two operands, one at least an i32: a 32-bit `cmp`, then 0 or 1. */
static void emit_cmp32(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  load_operand(e, GRJIT_A64_X0, &op->a);
  load_operand(e, GRJIT_A64_X1, &op->b);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 71
  /* Planted defect 71 (tests only): an ordered i32 comparison compares the whole register, so
   * 0xFFFFFFFF (-1) is not below 1. */
  if (op->cmp != GRJIT_CMP_EQ && op->cmp != GRJIT_CMP_NE) {
    grjit_a64_cmp(a, GRJIT_A64_X0, GRJIT_A64_X1);
  } else {
    grjit_a64_cmp32(a, GRJIT_A64_X0, GRJIT_A64_X1);
  }
#else
  grjit_a64_cmp32(a, GRJIT_A64_X0, GRJIT_A64_X1);
#endif
  grjit_a64_cset(a, GRJIT_A64_X0, cmp_cond(op->cmp));
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* NEG and NOT on an i32 are the `w` forms. */
static void emit_neg_not32(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  load_operand(e, GRJIT_A64_X0, &op->a);
  if (op->kind == GRJIT_OP_NEG) {
    grjit_a64_neg32(a, GRJIT_A64_X0, GRJIT_A64_X0);
  } else {
    grjit_a64_mvn32(a, GRJIT_A64_X0, GRJIT_A64_X0);
  }
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* CLZ, CTZ, POPCNT, EXT*_S, WRAP, EXTEND_S, EXTEND_U. `clz` of zero is the width. `ctz` is `rbit`
 * then `clz`, which is the width for zero too. `popcnt` goes through the vector unit (AArch64 has no
 * scalar form before FEAT_CSSC): the value into `v0`, `cnt` the bits of each byte, `addv` the bytes,
 * the sum back as a 32-bit word. A 32-bit value is moved with `fmov s, w`, which clears the rest of
 * the register, so the bytes above it count nothing. */
static void emit_int_unary(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const bool wide = !dst_is_i32(e, op);
  load_operand(e, GRJIT_A64_X0, &op->a);
  switch (op->kind) {
    case GRJIT_OP_CLZ:
      grjit_a64_bits(a, GRJIT_A64_CLZ, GRJIT_A64_X0, GRJIT_A64_X0, wide);
      break;
    case GRJIT_OP_CTZ:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 72
      /* Planted defect 72 (tests only): no bit reversal, so the trailing zeros are the leading ones. */
#else
      grjit_a64_bits(a, GRJIT_A64_RBIT, GRJIT_A64_X0, GRJIT_A64_X0, wide);
#endif
      grjit_a64_bits(a, GRJIT_A64_CLZ, GRJIT_A64_X0, GRJIT_A64_X0, wide);
      break;
    case GRJIT_OP_POPCNT:
      grjit_a64_fmov_from_gpr(a, wide ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE, GRJIT_A64_V0, GRJIT_A64_X0);
      grjit_a64_cnt8b(a, GRJIT_A64_V0, GRJIT_A64_V0);
      grjit_a64_addv8b(a, GRJIT_A64_V0, GRJIT_A64_V0);
      grjit_a64_fmov_to_gpr(a, GRJIT_A64_SINGLE, GRJIT_A64_X0, GRJIT_A64_V0);
      break;
    case GRJIT_OP_EXT8_S:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 96
      /* Planted defect 96 (tests only): an i32 sign extension of 8 or 16 bits is the 64-bit form, so a negative
       * value has ones above bit 31. */
      grjit_a64_sext(a, GRJIT_A64_SXTB, GRJIT_A64_X0, GRJIT_A64_X0, true);
#else
      grjit_a64_sext(a, GRJIT_A64_SXTB, GRJIT_A64_X0, GRJIT_A64_X0, wide);
#endif
      break;
    case GRJIT_OP_EXT16_S:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 96
      grjit_a64_sext(a, GRJIT_A64_SXTH, GRJIT_A64_X0, GRJIT_A64_X0, true);
#else
      grjit_a64_sext(a, GRJIT_A64_SXTH, GRJIT_A64_X0, GRJIT_A64_X0, wide);
#endif
      break;
    case GRJIT_OP_EXT32_S:
    case GRJIT_OP_EXTEND_S:
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 97
      /* Planted defect 97 (tests only): the 32-bit sign extensions (i64.extend32_s, i64.extend_i32_s) stop at
       * 16 bits. */
      grjit_a64_sext(a, GRJIT_A64_SXTH, GRJIT_A64_X0, GRJIT_A64_X0, true);
#else
      grjit_a64_sext(a, GRJIT_A64_SXTW, GRJIT_A64_X0, GRJIT_A64_X0, true);
#endif
      break;
    case GRJIT_OP_WRAP:
    case GRJIT_OP_EXTEND_U:
      /* The source is zero-extended already (an i32), or the low half is wanted (wrap): the cut is
       * emitted either way, so a dirty input cannot pass through. */
      grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
      break;
    default:
      e->c.error = GRJIT_ERR_INTERNAL;
      return;
  }
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* ROTL and ROTR, either width. `rorv` takes the count modulo the width of the form; a left rotation by
 * `c` is a right rotation by `-c`, and the negated count is taken modulo the width the same way. */
static void emit_rotate(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const bool wide = !dst_is_i32(e, op);
  load_operand(e, GRJIT_A64_X0, &op->a);
  load_operand(e, GRJIT_A64_X1, &op->b);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 95
  /* Planted defect 95 (tests only): a left rotation is a right rotation by the same count. */
  if (false) {
#else
  if (op->kind == GRJIT_OP_ROTL) {
#endif
    if (wide) {
      grjit_a64_neg(a, GRJIT_A64_X1, GRJIT_A64_X1);
    } else {
      grjit_a64_neg32(a, GRJIT_A64_X1, GRJIT_A64_X1);
    }
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 70
  /* Planted defect 70, rotate half (tests only): an i32 rotation is the 64-bit instruction, cut to
   * 32 bits afterwards. */
  if (!wide) {
    grjit_a64_alu(a, GRJIT_A64_RORV, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    store_result(e, op->dst, GRJIT_A64_X0);
    return;
  }
#endif
  if (wide) {
    grjit_a64_alu(a, GRJIT_A64_RORV, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  } else {
    grjit_a64_alu32(a, GRJIT_A64_RORV, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  }
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* DIV_S, DIV_U, REM_S, REM_U, either width. The divisor is non-zero and, for DIV_S, the quotient does
 * not overflow: the guards are in front of this in the IR. */
static void emit_divide(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const bool wide = !dst_is_i32(e, op);
  const bool is_signed = op->kind == GRJIT_OP_DIV_S || op->kind == GRJIT_OP_REM_S;
  const bool remainder = op->kind == GRJIT_OP_REM_S || op->kind == GRJIT_OP_REM_U;
  load_operand(e, GRJIT_A64_X0, &op->a);
  load_operand(e, GRJIT_A64_X1, &op->b);
  GRJIT_A64Alu divop = is_signed ? GRJIT_A64_SDIV : GRJIT_A64_UDIV;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 73
  /* Planted defect 73 (tests only): an unsigned division and remainder are emitted signed. */
  divop = GRJIT_A64_SDIV;
#endif
  if (!remainder) {
    (wide ? grjit_a64_alu : grjit_a64_alu32)(a, divop, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  } else {
    (wide ? grjit_a64_alu : grjit_a64_alu32)(a, divop, GRJIT_A64_X2, GRJIT_A64_X0, GRJIT_A64_X1);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 75
    /* Planted defect 75 (tests only): the remainder's `msub` has the divisor and the dividend
     * swapped, so it is divisor - quotient * divisor. */
    grjit_a64_msub(a, GRJIT_A64_X0, GRJIT_A64_X2, GRJIT_A64_X1, GRJIT_A64_X1, wide);
#else
    grjit_a64_msub(a, GRJIT_A64_X0, GRJIT_A64_X2, GRJIT_A64_X1, GRJIT_A64_X0, wide);
#endif
  }
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* ---- The float operations of numeric-operations.md (spec-runtime-float, story 5) ----------------
 *
 * The emitter's shape holds: a value is a bit word in its frame slot, `d0` and `d1` (and `x0`-`x2` for the
 * integer side) are scratch, nothing is kept in a vector register between two IR operations, and a result
 * goes back to its slot as a whole word. `v8` to `v15` are never named: only their low 64 bits are
 * callee-saved under AAPCS64 and this code does not use them at all. The hardware instructions have
 * exactly WebAssembly's semantics where the table below says so, which is why none of these is a
 * sequence: `fmin`/`fmax` (a NaN operand gives a NaN, -0 < +0; `fminnm`/`fmaxnm` ignore a quiet NaN and
 * are the wrong instructions), `frintn/p/m/z` (ties to even, up, down, toward zero, the sign of a zero
 * kept, a NaN quiet; `frinta` is ties away and `frinti`/`frintx` follow FPCR), `scvtf`/`ucvtf` (one
 * rounding), `fcvtzs`/`fcvtzu` (toward zero, saturating, a NaN is 0), `fcvt` (a signalling NaN quiet) and
 * `fneg`/`fabs` (the sign bit only, so a signalling NaN keeps its payload). `fcmp` leaves NZCV = 0011 for
 * an unordered pair, so the integer `lt` and `le` are true for a NaN and the emitter uses `mi` and `ls`.
 * FPCR is assumed to be the default (round to nearest, no flush to zero, default NaN off, no trap enabled)
 * and is never written. */

static GRJIT_A64FSize fsize(bool dbl) {
  return dbl ? GRJIT_A64_DOUBLE : GRJIT_A64_SINGLE;
}

/* FADD FSUB FMUL FDIV FMIN FMAX: `d0 = d0 op d1`, operands in order. */
static void emit_float_binary(GRJIT_A64Emit * e, const GRJIT_Op * op, bool dbl) {
  A * a = &e->as;
  GRJIT_Operand left = op->a;
  GRJIT_Operand right = op->b;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 76
  /* Planted defect 76 (tests only): the operands of a float subtraction are swapped. */
  if (op->kind == GRJIT_OP_FSUB) {
    left = op->b;
    right = op->a;
  }
#endif
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 77
  /* Planted defect 77 (tests only): the operands of a float division are swapped. */
  if (op->kind == GRJIT_OP_FDIV) {
    left = op->b;
    right = op->a;
  }
#endif
  load_float(e, GRJIT_A64_V0, &left, dbl, GRJIT_A64_X2);
  load_float(e, GRJIT_A64_V1, &right, dbl, GRJIT_A64_X2);
  GRJIT_A64FpBinary which;
  switch (op->kind) {
    case GRJIT_OP_FADD: which = GRJIT_A64_FADD; break;
    case GRJIT_OP_FSUB: which = GRJIT_A64_FSUB; break;
    case GRJIT_OP_FMUL: which = GRJIT_A64_FMUL; break;
    case GRJIT_OP_FDIV: which = GRJIT_A64_FDIV; break;
    case GRJIT_OP_FMIN: which = GRJIT_A64_FMIN; break;
    default: which = GRJIT_A64_FMAX; break;
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 79
  /* Planted defect 79 (tests only): min and max are `fminnm` and `fmaxnm`, which return the number when
   * one operand is a quiet NaN, where WebAssembly wants a NaN. (Written as the raw word: the assembler has
   * no such form on purpose.) */
  if (which == GRJIT_A64_FMIN || which == GRJIT_A64_FMAX) {
    grjit_a64_word(a, (which == GRJIT_A64_FMIN ? 0x1E207800u : 0x1E206800u) | (dbl ? 0x00400000u : 0u) |
        (1u << 16) | 0u);
    store_float(e, op->dst, GRJIT_A64_V0, dbl);
    return;
  }
#endif
  grjit_a64_fp_binary(a, which, fsize(dbl), GRJIT_A64_V0, GRJIT_A64_V0, GRJIT_A64_V1);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 86
  /* Planted defect 86 (tests only): a float sequence keeps its result in d8 as well, which it never saves
   * and never restores: a callee-saved register changed by compiled code. */
  if (op->kind == GRJIT_OP_FADD) {
    grjit_a64_fmov_vv(a, GRJIT_A64_DOUBLE, GRJIT_A64_V8, GRJIT_A64_V0);
  }
#endif
  store_float(e, op->dst, GRJIT_A64_V0, dbl);
}

/* FSQRT, FABS, FNEG and the four roundings: `d0 = op d0`. */
static void emit_float_unary(GRJIT_A64Emit * e, const GRJIT_Op * op, bool dbl) {
  A * a = &e->as;
  load_float(e, GRJIT_A64_V0, &op->a, dbl, GRJIT_A64_X2);
  GRJIT_A64FpUnary which;
  switch (op->kind) {
    case GRJIT_OP_FSQRT: which = GRJIT_A64_FSQRT; break;
    case GRJIT_OP_FABS: which = GRJIT_A64_FABS; break;
    case GRJIT_OP_FNEG: which = GRJIT_A64_FNEG; break;
    case GRJIT_OP_FCEIL: which = GRJIT_A64_FRINTP; break;
    case GRJIT_OP_FFLOOR: which = GRJIT_A64_FRINTM; break;
    case GRJIT_OP_FTRUNC: which = GRJIT_A64_FRINTZ; break;
    default: which = GRJIT_A64_FRINTN; break;
  }
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 80
  /* Planted defect 80 (tests only): nearest is `frinta`, which rounds halves away from zero. (The raw
   * word: the assembler has no such form on purpose.) */
  if (which == GRJIT_A64_FRINTN) {
    grjit_a64_word(a, 0x1E264000u | (dbl ? 0x00400000u : 0u));
    store_float(e, op->dst, GRJIT_A64_V0, dbl);
    return;
  }
#endif
  grjit_a64_fp_unary(a, which, fsize(dbl), GRJIT_A64_V0, GRJIT_A64_V0);
  store_float(e, op->dst, GRJIT_A64_V0, dbl);
}

/* FCOPYSIGN: the magnitude bits of `a` with the sign bit of `b`, in the integer registers (there is no
 * scalar form): `(a & ~sign) | (b & sign)`. The mask is one `movz`. An f32 word is cut to 32 bits after,
 * so an immediate with bits above 31 does not pass through. */
static void emit_float_copysign(GRJIT_A64Emit * e, const GRJIT_Op * op, bool dbl) {
  A * a = &e->as;
  load_operand(e, GRJIT_A64_X0, &op->a);
  load_operand(e, GRJIT_A64_X1, &op->b);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 98
  /* Planted defect 98 (tests only): the sign mask of an f32 is the f64 one, so the sign bit of bit 31 is
   * taken from nowhere. */
  grjit_a64_mov_ri(a, GRJIT_A64_X2, UINT64_C(0x8000000000000000));
#else
  grjit_a64_mov_ri(a, GRJIT_A64_X2, dbl ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000));
#endif
  grjit_a64_alu(a, GRJIT_A64_BIC, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X2);
  grjit_a64_alu(a, GRJIT_A64_AND, GRJIT_A64_X1, GRJIT_A64_X1, GRJIT_A64_X2);
  grjit_a64_alu(a, GRJIT_A64_ORR, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1);
  if (!dbl) {
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
  }
  store_result(e, op->dst, GRJIT_A64_X0);
}

/* A comparison of two floats: `fcmp`, then 0 or 1. EQ NE GT GE are eq ne gt ge; LT and LE are `mi` and
 * `ls`, which an unordered pair does not satisfy (the integer lt and le would). */
static void emit_float_cmp(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const bool dbl = operand_is_float(e, &op->a) ? operand_is_double(e, &op->a) : operand_is_double(e, &op->b);
  load_float(e, GRJIT_A64_V0, &op->a, dbl, GRJIT_A64_X2);
  load_float(e, GRJIT_A64_V1, &op->b, dbl, GRJIT_A64_X2);
  grjit_a64_fcmp(a, fsize(dbl), GRJIT_A64_V0, GRJIT_A64_V1);
  GRJIT_A64Cond cond;
  switch (op->cmp) {
    case GRJIT_CMP_EQ: cond = GRJIT_A64_EQ; break;
    case GRJIT_CMP_NE: cond = GRJIT_A64_NE; break;
    case GRJIT_CMP_GT: cond = GRJIT_A64_GT; break;
    case GRJIT_CMP_GE: cond = GRJIT_A64_GE; break;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 78
    /* Planted defect 78 (tests only): LT and LE are the integer conditions, which are true for a NaN. */
    case GRJIT_CMP_LT: cond = GRJIT_A64_LT; break;
    case GRJIT_CMP_LE: cond = GRJIT_A64_LE; break;
#else
    case GRJIT_CMP_LT: cond = GRJIT_A64_MI; break;
    case GRJIT_CMP_LE: cond = GRJIT_A64_LS; break;
#endif
    default: /* the unsigned conditions: the verifier refuses them for floats */
      e->c.error = GRJIT_ERR_INTERNAL;
      return;
  }
  grjit_a64_cset(a, GRJIT_A64_X0, cond);
  store_result(e, op->dst, GRJIT_A64_X0);
}

static void emit_float_op(GRJIT_A64Emit * e, const GRJIT_Op * op) {
  A * a = &e->as;
  const bool dbl = op->dst != GRJIT_NO_VREG && vreg_is_f64(e, op->dst);
  switch (op->kind) {
    case GRJIT_OP_FADD:
    case GRJIT_OP_FSUB:
    case GRJIT_OP_FMUL:
    case GRJIT_OP_FDIV:
    case GRJIT_OP_FMIN:
    case GRJIT_OP_FMAX:
      emit_float_binary(e, op, dbl);
      break;
    case GRJIT_OP_FCOPYSIGN:
      emit_float_copysign(e, op, dbl);
      break;
    case GRJIT_OP_FNEG:
    case GRJIT_OP_FABS:
    case GRJIT_OP_FSQRT:
    case GRJIT_OP_FCEIL:
    case GRJIT_OP_FFLOOR:
    case GRJIT_OP_FTRUNC:
    case GRJIT_OP_FNEAREST:
      emit_float_unary(e, op, dbl);
      break;
    case GRJIT_OP_CONVERT_S:
    case GRJIT_OP_CONVERT_U: {
      /* Integer to float, one rounding, from the source's width (an i32 is a `w` register: `scvtf d, w`
       * reads it as signed, `ucvtf d, w` as unsigned). */
      const bool wide_src = e->c.f->vregs[op->a.vreg].type == GRJIT_TYPE_I64;
      bool is_signed = op->kind == GRJIT_OP_CONVERT_S;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 83
      /* Planted defect 83 (tests only): an unsigned 64-bit source is converted as a signed one. */
      if (wide_src) {
        is_signed = true;
      }
#endif
      load_operand(e, GRJIT_A64_X0, &op->a);
      grjit_a64_cvt_to_fp(a, is_signed, fsize(dbl), GRJIT_A64_V0, GRJIT_A64_X0, wide_src);
      store_float(e, op->dst, GRJIT_A64_V0, dbl);
      break;
    }
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U:
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U: {
      /* Float to integer: `fcvtzs`/`fcvtzu` are total (toward zero, saturating, a NaN is 0), which is
       * exactly the saturating operations; the partial TRUNC_* are the same instruction behind the
       * guards a frontend puts in front, so a dropped guard is a wrong answer and not a signal. An i32
       * destination is a `w` register, which clears the upper half. */
      const bool src_double = e->c.f->vregs[op->a.vreg].type == GRJIT_TYPE_F64;
      const bool wide = e->c.f->vregs[op->dst].type == GRJIT_TYPE_I64;
      bool is_signed = op->kind == GRJIT_OP_TRUNC_S || op->kind == GRJIT_OP_TRUNC_SAT_S;
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 82
      /* Planted defect 82 (tests only): the saturating unsigned truncation is the signed instruction. */
      if (op->kind == GRJIT_OP_TRUNC_SAT_U) {
        is_signed = true;
      }
#endif
      load_float(e, GRJIT_A64_V0, &op->a, src_double, GRJIT_A64_X2);
      grjit_a64_cvt_from_fp(a, is_signed, fsize(src_double), GRJIT_A64_X0, GRJIT_A64_V0, wide);
      store_result(e, op->dst, GRJIT_A64_X0);
      break;
    }
    case GRJIT_OP_DEMOTE:
      load_float(e, GRJIT_A64_V0, &op->a, true, GRJIT_A64_X2);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 99
      /* Planted defect 99 (tests only): a demotion that does not convert, the double's low 32 bits stored. */
#else
      grjit_a64_fcvt(a, GRJIT_A64_SINGLE, GRJIT_A64_V0, GRJIT_A64_V0);
#endif
      store_float(e, op->dst, GRJIT_A64_V0, false);
      break;
    case GRJIT_OP_PROMOTE:
      load_float(e, GRJIT_A64_V0, &op->a, false, GRJIT_A64_X2);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 100
      /* Planted defect 100 (tests only): a promotion that does not convert, the single's bits stored as the
       * double's low word. */
#else
      grjit_a64_fcvt(a, GRJIT_A64_DOUBLE, GRJIT_A64_V0, GRJIT_A64_V0);
#endif
      store_float(e, op->dst, GRJIT_A64_V0, true);
      break;
    case GRJIT_OP_REINTERPRET:
      /* The bits are the bits: a slot to a slot, cut to 32 bits where the destination is 32 wide. */
      load_operand(e, GRJIT_A64_X0, &op->a);
      if (vreg_is_narrow(e, op->dst)) {
        grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
      }
      store_result(e, op->dst, GRJIT_A64_X0);
      break;
    default:
      e->c.error = GRJIT_ERR_INTERNAL;
      break;
  }
}

static void emit_op(GRJIT_A64Emit * e, const GRJIT_Op * op, size_t block) {
  A * a = &e->as;
  switch (op->kind) {
    case GRJIT_OP_INSTANCE:
      load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_ENV);
      store_result(e, op->dst, GRJIT_A64_X0);
      break;
    case GRJIT_OP_CONST:
    case GRJIT_OP_MOVE:
    case GRJIT_OP_BITCAST:
      load_operand(e, GRJIT_A64_X0, &op->a);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 94
      /* Planted defect 94 (tests only): a move of a 32-bit value is not cut, so an immediate or a register with
       * bits above 31 passes through whole. */
      if (false) {
#else
      if (vreg_is_narrow(e, op->dst)) {
#endif
        grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0); /* an i32 or f32 immediate: its low 32 bits */
      }
      store_result(e, op->dst, GRJIT_A64_X0);
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
        emit_neg_not32(e, op);
        break;
      }
      load_operand(e, GRJIT_A64_X0, &op->a);
      if (op->kind == GRJIT_OP_NEG) {
        grjit_a64_neg(a, GRJIT_A64_X0, GRJIT_A64_X0);
      } else {
        grjit_a64_mvn(a, GRJIT_A64_X0, GRJIT_A64_X0);
      }
      store_result(e, op->dst, GRJIT_A64_X0);
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
      load_operand(e, GRJIT_A64_X0, &op->a);
      load_operand(e, GRJIT_A64_X1, &op->b);
      grjit_a64_cmp(a, GRJIT_A64_X0, GRJIT_A64_X1);
      grjit_a64_cset(a, GRJIT_A64_X0, cmp_cond(op->cmp));
      store_result(e, op->dst, GRJIT_A64_X0);
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
      /* ldr x0,[ctx]; ldr x0,[x0+request_word]; cbnz x0, slow */
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_POLL;
      p.entry = grjit_a64_label(a);
      p.back = grjit_a64_label(a);
      p.op = op;
      p.live_index = e->c.live_cursor++;
      load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X0,
          (int32_t)e->c.request_offset);
      grjit_a64_cbnz(a, GRJIT_A64_X0, p.entry);
      grjit_a64_bind(a, p.back);
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_GUARD: {
      GRJIT_Pending p;
      p.kind = GRJIT_PENDING_GUARD;
      p.entry = grjit_a64_label(a);
      p.back = 0;
      p.op = op;
      p.live_index = e->c.live_cursor++;
      load_operand(e, GRJIT_A64_X0, &op->a);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 74
      /* Planted defect 74 (tests only): the guard's test is not emitted. arm64 never faults in
       * hardware, so a zero divisor then gives 0 and no trap: only an assertion that the trap fired
       * can see it. The stub is queued and bound, and nothing branches to it. */
#else
      grjit_a64_cbz(a, GRJIT_A64_X0, p.entry);
#endif
      grjit_emit_add_pending(&e->c, &p);
      break;
    }
    case GRJIT_OP_BR:
      if (op->target != block + 1) {
        grjit_a64_b(a, e->blocks[op->target]);
      }
      break;
    case GRJIT_OP_BR_IF:
      load_operand(e, GRJIT_A64_X0, &op->a);
      if (op->target_else == block + 1) {
        grjit_a64_cbnz(a, GRJIT_A64_X0, e->blocks[op->target]);
      } else if (op->target == block + 1) {
        grjit_a64_cbz(a, GRJIT_A64_X0, e->blocks[op->target_else]);
      } else {
        grjit_a64_cbnz(a, GRJIT_A64_X0, e->blocks[op->target]);
        grjit_a64_b(a, e->blocks[op->target_else]);
      }
      break;
    case GRJIT_OP_BR_TABLE:
      load_operand(e, GRJIT_A64_X0, &op->a);
      grjit_a64_mov_ri(a, GRJIT_A64_X1, op->target_count);
      grjit_a64_cmp(a, GRJIT_A64_X0, GRJIT_A64_X1);
      grjit_a64_bcond(a, GRJIT_A64_HS, e->blocks[op->default_target]);
      for (size_t i = 0; i < op->target_count; i++) {
        grjit_a64_mov_ri(a, GRJIT_A64_X1, i);
        grjit_a64_cmp(a, GRJIT_A64_X0, GRJIT_A64_X1);
        grjit_a64_bcond(a, GRJIT_A64_EQ, e->blocks[op->targets[i]]);
      }
      grjit_a64_b(a, e->blocks[op->default_target]);
      break;
    case GRJIT_OP_TRAP:
      grjit_a64_mov_ri(a, GRJIT_A64_X0, op->trap_kind);
      if (e->c.callable) {
        grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_TRAPPED);
        grjit_a64_emit_callable_epilogue(e);
      } else {
        load_slot(e, GRJIT_A64_X2, GRJIT_SLOT_OUT);
        grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
        grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_TRAP);
        grjit_a64_emit_epilogue(e);
      }
      break;
    case GRJIT_OP_RETHROW:
      grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
      grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_EXCEPTION);
      grjit_a64_emit_callable_epilogue(e);
      break;
    case GRJIT_OP_RET:
      if (e->c.callable) {
        /* The internal convention: the result in x0 (zero for none), the status in x1, and
         * the callee pops its own stack arguments. */
        if (op->result_count > 1) {
          load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_ARGS);
          for (size_t i = 1; i < op->result_count; i++) {
            load_operand(e, GRJIT_A64_X10, &op->returns[i]);
            grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_X15,
                (int32_t)(8 * (i - 1)));
          }
        }
        if (op->result_count != 0) load_operand(e, GRJIT_A64_X0, &op->returns[0]);
        else grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);
        grjit_a64_mov_ri(a, GRJIT_A64_X1, GRJIT_STATUS_RETURNED);
        grjit_a64_emit_callable_epilogue(e);
        break;
      }
      if (op->result_count != 0) {
        if (op->result_count == 1) {
          load_operand(e, GRJIT_A64_X0, &op->returns[0]);
          load_slot(e, GRJIT_A64_X2, GRJIT_SLOT_OUT);
          grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
        } else {
          load_slot(e, GRJIT_A64_X2, GRJIT_SLOT_OUT);
          for (size_t i = 0; i < op->result_count; i++) {
            load_operand(e, GRJIT_A64_X0, &op->returns[i]);
            grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2,
                (int32_t)(8 * i));
          }
        }
      }
      grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_RETURNED);
      grjit_a64_emit_epilogue(e);
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
      emit_int_unary(e, op);
      break;
    case GRJIT_OP_ROTL:
    case GRJIT_OP_ROTR:
      emit_rotate(e, op);
      break;
    case GRJIT_OP_DIV_S:
    case GRJIT_OP_DIV_U:
    case GRJIT_OP_REM_S:
    case GRJIT_OP_REM_U:
      emit_divide(e, op);
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

void grjit_a64_emit_epilogue(GRJIT_A64Emit * e) {
  A * a = &e->as;
  grjit_a64_mov_sp(a, GRJIT_A64_SP, GRJIT_A64_FP);
  grjit_a64_pop_frame_record(a);
  grjit_a64_ret(a);
}

/* The entry adapter of a callable function (AD-28): the C ABI of ::GRJIT_EntryFn on one side
 * and the internal convention on the other. It makes a frame record and keeps `out`, the
 * context and `args` in its own frame, runs the entry hook, loads the arguments from the
 * `args` array into the registers and the stack area, sets `x29` to the chain-end marker (so
 * the first compiled frame's saved caller base is the marker, which ends a walk;
 * a/compiled.h), calls the internal entry, and turns its status into an exit. It uses no
 * callee-saved register but `x29`, which it restores from its own frame record (never from
 * `x29`, which is the marker by then). It clears the walk-start cell on the way out, so a
 * walk after the run reads no dead frames. Its frame is `sp` + 0 .. 32: `out` at 0, the
 * context at 8, `args` at 16. */
static void emit_adapter(GRJIT_A64Emit * e) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const size_t params = f->param_count;
  const uint32_t in_bytes = e->c.shape.incoming_bytes;
  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
  GRJIT_Label deopted = grjit_a64_label(a);
  GRJIT_Label not_failed = grjit_a64_label(a);
  GRJIT_Label done = grjit_a64_label(a);
  grjit_a64_push_frame_record(a);
  grjit_a64_sub_sp(a, 48);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X3, GRJIT_A64_SP, 0);  /* out */
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_SP, 8);  /* context */
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X1, GRJIT_A64_SP, 16); /* environment */
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_SP, 24); /* args */
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 27 && defined(GRJIT_TEST_PLANT_FORM) && GRJIT_TEST_PLANT_FORM == 2
  /* Planted defect 27, second form (tests only): a callee-saved register is used and never
   * restored. The caller's own value must survive the call. */
  grjit_a64_mov_rr(a, GRJIT_A64_X19, GRJIT_A64_X1);
#endif
  if (e->c.hook != NULL) {
    grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.hook);
    grjit_a64_blr(a, GRJIT_A64_X16);
    /* The hook returns a uint32_t; the upper half of x0 is not defined. */
    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
    grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);
  }
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_SP, 24);
  if (in_bytes != 0) {
    grjit_a64_sub_sp(a, in_bytes);
    for (size_t i = ireg; i < params; i++) {
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_X15, (int32_t)(8 * i));
      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));
    }
  }
  for (size_t i = 0; i < params && i < ireg; i++) {
    grjit_a64_load(a, GRJIT_A64_MEM_X64, arg_regs_internal[i], GRJIT_A64_X15, (int32_t)(8 * i));
  }
  if (e->c.shape.has_result_buffer) {
    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, 0);
    grjit_a64_add_imm(a, GRJIT_A64_X10, GRJIT_A64_X10, 8, false);
    if (params < ireg) grjit_a64_mov_rr(a, arg_regs_internal[params], GRJIT_A64_X10);
    else grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,
        (int32_t)(8 * (params - ireg)));
  }
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X9, GRJIT_A64_SP, (int32_t)(in_bytes + 8));
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X8, GRJIT_A64_SP, (int32_t)(in_bytes + 16));
  grjit_a64_mov_ri(a, GRJIT_A64_X29, (uint64_t)GRCORE_COMPILED_CHAIN_END);
  grjit_a64_adr(a, GRJIT_A64_X16, e->internal);
  grjit_a64_blr(a, GRJIT_A64_X16);
  /* sp is where it was after the frame was made: the callee popped the stack arguments. */
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_SP, 0);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_SP, 8);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset + 8);
  grjit_a64_cbnz(a, GRJIT_A64_X1, deopted);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_RETURNED);
  grjit_a64_b(a, done);
  /* DEOPTED: every compiled frame was rebuilt into its guest frame before any of them
   * returned; out[0] is the cause. */
  grjit_a64_bind(a, deopted);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_DEOPT);
  GRJIT_Label not_trapped = grjit_a64_label(a);
  grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_TRAPPED);
  grjit_a64_bcond(a, GRJIT_A64_NE, not_trapped);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_TRAP);
  grjit_a64_bind(a, not_trapped);
  /* FAILED: the engine's hook refused the rebuild; out[0] is its answer. */
  grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_FAILED);
  grjit_a64_bcond(a, GRJIT_A64_NE, not_failed);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_REBUILD_FAILED);
  grjit_a64_bind(a, not_failed);
  GRJIT_Label not_exception = grjit_a64_label(a);
  grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_EXCEPTION);
  grjit_a64_bcond(a, GRJIT_A64_NE, not_exception);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_EXCEPTION);
  grjit_a64_bind(a, not_exception);
  grjit_a64_bind(a, done);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 27 && !(defined(GRJIT_TEST_PLANT_FORM) && GRJIT_TEST_PLANT_FORM == 2)
  /* Planted defect 27 (tests only): the adapter gives the frame record back but leaves `x29`
   * as the chain-end marker the caller of the entry then sees. */
  grjit_a64_add_sp(a, 48);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X30, GRJIT_A64_SP, 8);
  grjit_a64_add_sp(a, 16);
  grjit_a64_ret(a);
#else
  grjit_a64_add_sp(a, 48);
  grjit_a64_pop_frame_record(a);
  grjit_a64_ret(a);
#endif
  /* The entry hook refused (w0 is its answer). */
  grjit_a64_bind(a, e->refuse);
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X2, GRJIT_A64_SP, 0);
  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 0);
  grjit_a64_mov_ri(a, GRJIT_A64_X0, GRJIT_EXIT_REFUSED);
  grjit_a64_add_sp(a, 48);
  grjit_a64_pop_frame_record(a);
  grjit_a64_ret(a);
}

/* The stubs the blocks queued. A stub may add sites but never another stub. */
static void emit_pending(GRJIT_A64Emit * e) {
  for (size_t i = 0; i < e->c.pending_count && e->c.error == GRJIT_OK; i++) {
    const GRJIT_Pending * p = &e->c.pending[i];
    switch (p->kind) {
      case GRJIT_PENDING_POLL:
        grjit_a64_emit_poll_stub(e, p);
        break;
      case GRJIT_PENDING_GUARD:
        grjit_a64_emit_guard_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_SLOW:
        grjit_a64_emit_call_slow_stub(e, p);
        break;
      case GRJIT_PENDING_CALL_EXIT:
        grjit_a64_emit_call_exit_stub(e, p);
        break;
      case GRJIT_PENDING_NATIVE_EXIT:
        grjit_a64_emit_native_exit_stub(e, p);
        break;
      case GRJIT_PENDING_NATIVE_STATUS:
        grjit_a64_emit_native_status_stub(e, p);
        break;
    }
  }
}

/* The internal entry of a callable function: a frame record, the native-stack check (in bytes,
 * AD-28: the lowest address this frame will use against the context's limit word, `x9` holding
 * the context; below it, the chain deopts at the call site and the interpreter continues), the
 * frame, the context and the parameters from `x0`-`x7` and the caller's stack arguments. */
static void emit_internal_prologue(GRJIT_A64Emit * e) {
  A * a = &e->as;
  const GRJIT_Function * f = e->c.f;
  const uint32_t alloc = e->c.frame_bytes;
  const unsigned ireg = GRJIT_ARM64_INTERNAL_REG_ARGS;
  grjit_a64_push_frame_record(a);
  grjit_a64_mov_sp(a, GRJIT_A64_FP, GRJIT_A64_SP);
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 29
  /* Planted defect 29 (tests only): the frame is not counted, so a function whose frame
   * crosses the limit is let in. */
  grjit_a64_mov_sp(a, GRJIT_A64_X16, GRJIT_A64_SP);
#else
  lea(e, GRJIT_A64_X16, GRJIT_A64_SP, -(int64_t)alloc);
#endif
  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_X9, (int32_t)e->c.native_limit_offset);
  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);
  grjit_a64_bcond(a, GRJIT_A64_LO, e->overflow);
  grjit_a64_sub_sp(a, alloc);
  store_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);
  store_slot(e, GRJIT_A64_X8, GRJIT_SLOT_ENV);
  if (f->param_count < ireg) {
    if (e->c.shape.has_result_buffer) store_slot(e, arg_regs_internal[f->param_count], GRJIT_SLOT_ARGS);
  } else {
    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_FP,
        (int32_t)(16 + 8 * (f->param_count - ireg)));
    if (e->c.shape.has_result_buffer) store_slot(e, GRJIT_A64_X10, GRJIT_SLOT_ARGS);
  }
  for (size_t i = 0; i < f->param_count; i++) {
    if (i < ireg) {
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 87
      /* Planted defect 87 (tests only): an i32 or f32 parameter is not cut to 32 bits on entry. */
#else
      if (vreg_is_narrow(e, (GRJIT_VReg)i)) {
        grjit_a64_mov32_rr(a, arg_regs_internal[i], arg_regs_internal[i]); /* an i32 argument is its low 32 bits */
      }
#endif
      store_slot(e, arg_regs_internal[i], grjit_emit_slot((GRJIT_VReg)i));
    } else {
      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_FP, (int32_t)(16 + 8 * (i - ireg)));
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 87
#else
      if (vreg_is_narrow(e, (GRJIT_VReg)i)) {
        grjit_a64_mov32_rr(a, GRJIT_A64_X10, GRJIT_A64_X10);
      }
#endif
      store_slot(e, GRJIT_A64_X10, grjit_emit_slot((GRJIT_VReg)i));
    }
  }
}

/* One assembly of the function. */
static GRJIT_Result emit_pass(const GRJIT_Function * f, GRJIT_A64Emit * e,
    size_t max_code_bytes, bool long_branches) {
  const GRJIT_Allocator * allocator = e->c.allocator;
  GRJIT_A64Asm * a = &e->as;
  grjit_a64_init(a, allocator, max_code_bytes);
  grjit_a64_set_long_branches(a, long_branches);
  e->blocks = allocator->malloc_fn(allocator->ctx, f->block_count * sizeof *e->blocks);
  if (e->blocks == NULL) {
    return GRJIT_ERR_OOM;
  }
  for (size_t b = 0; b < f->block_count; b++) {
    e->blocks[b] = grjit_a64_label(a);
  }
  e->refuse = grjit_a64_label(a);

  if (f->callable) {
    e->internal = grjit_a64_label(a);
    e->overflow = grjit_a64_label(a);
    e->ret_deopted = grjit_a64_label(a);
    e->ret_failed = grjit_a64_label(a);
    e->ret_propagate = grjit_a64_label(a);
    emit_adapter(e);
    /* The internal entry on a 16-byte boundary with the tag just before it: the magic and
     * parameter count, then the function's token. The padding is trap words. */
    /* A failed assembler emits nothing, so its size stops changing: the loop ends there. */
    while (grjit_a64_status(a) == GRJIT_A64_OK && (grjit_a64_size(a) + GRJIT_ENTRY_TAG_BYTES) % 16 != 0) {
      grjit_a64_brk(a, 0);
    }
    grjit_a64_data64(a, GRJIT_ENTRY_TAG_WORD(f->param_count,
        grjit_function_result_count(f)));
    grjit_a64_data64(a, f->token);
    grjit_a64_bind(a, e->internal);
    e->internal_offset = (uint32_t)grjit_a64_size(a);
    emit_internal_prologue(e);
  } else {
    /* Prologue: frame record, frame, the three fixed slots, the entry hook, the
     * parameters. */
    grjit_a64_push_frame_record(a);
    grjit_a64_mov_sp(a, GRJIT_A64_FP, GRJIT_A64_SP);
    grjit_a64_sub_sp(a, e->c.frame_bytes);
    store_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);
    store_slot(e, GRJIT_A64_X1, GRJIT_SLOT_ENV);
    store_slot(e, GRJIT_A64_X2, GRJIT_SLOT_ARGS);
    store_slot(e, GRJIT_A64_X3, GRJIT_SLOT_OUT);
    if (e->c.hook != NULL) {
      grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)e->c.hook);
      grjit_a64_blr(a, GRJIT_A64_X16);
      /* The hook returns a uint32_t; the upper half of x0 is not defined. */
      grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);
      grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);
    }
    if (f->param_count != 0) {
      load_slot(e, GRJIT_A64_X1, GRJIT_SLOT_ARGS);
      for (size_t i = 0; i < f->param_count; i++) {
        grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1,
            (int32_t)(8 * i));
#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 92
        /* Planted defect 92 (tests only): an i32 or f32 argument word of the plain entry is not cut to 32 bits. */
        if (false) {
#else
        if (vreg_is_narrow(e, (GRJIT_VReg)i)) {
#endif
          grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0); /* an i32 argument is its low 32 bits */
        }
        store_slot(e, GRJIT_A64_X0, grjit_emit_slot((GRJIT_VReg)i));
      }
    }
  }

  for (size_t b = 0; b < f->block_count && e->c.error == GRJIT_OK; b++) {
    grjit_a64_bind(a, e->blocks[b]);
    const GRJIT_BlockInfo * blk = &f->blocks[b];
    for (size_t i = 0; i < blk->count; i++) {
      emit_op(e, &blk->ops[i], b);
    }
  }
  emit_pending(e);
  if (f->callable) {
    grjit_a64_emit_ret_deopted(e);
    grjit_a64_emit_ret_status(e);
    grjit_a64_emit_overflow_stub(e);
  } else {
    grjit_a64_emit_refuse_stub(e);
  }
  grjit_a64_finish(a);
  if (e->c.error != GRJIT_OK) {
    return e->c.error;
  }
  switch (grjit_a64_status(a)) {
    case GRJIT_A64_OK:
      return GRJIT_OK;
    case GRJIT_A64_OOM:
      return GRJIT_ERR_OOM;
    case GRJIT_A64_LIMIT:
      return GRJIT_ERR_LIMIT;
    default:
      return GRJIT_ERR_INTERNAL;
  }
}

/* Drops what one pass made so another can start (the common state is
 * rebuilt). */
static void reset_pass(GRJIT_A64Emit * e, const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, GRJIT_EntryHook hook,
    uint32_t request_offset, uint32_t frame_bytes, const GRJIT_LiveSites * live) {
  allocator->free_fn(allocator->ctx, e->blocks);
  e->blocks = NULL;
  grjit_a64_free(&e->as);
  grjit_emit_common_free(&e->c);
  grjit_emit_common_init(&e->c, f, GRJIT_ARM64_INTERNAL_REG_ARGS, allocator, hook,
      request_offset, frame_bytes, live);
}

GRJIT_Result grjit_a64_emit_function(const GRJIT_Function * f,
    const GRJIT_Allocator * allocator, size_t max_code_bytes,
    GRJIT_EntryHook hook, uint32_t request_offset, uint32_t frame_bytes,
    const GRJIT_LiveSites * live, GRJIT_A64Emit * e) {
  memset(e, 0, sizeof *e);
  grjit_emit_common_init(&e->c, f, GRJIT_ARM64_INTERNAL_REG_ARGS, allocator, hook,
      request_offset, frame_bytes, live);
  GRJIT_Result r = emit_pass(f, e, max_code_bytes, false);
  if (r == GRJIT_ERR_INTERNAL && grjit_a64_status(&e->as) == GRJIT_A64_FAR) {
    /* Some forward conditional branch is more than 1 MiB from its label. */
    reset_pass(e, f, allocator, hook, request_offset, frame_bytes, live);
    r = emit_pass(f, e, max_code_bytes, true);
  }
  if (r != GRJIT_OK) {
    return r;
  }
  grjit_emit_sort_sites(&e->c);
  for (size_t i = 0; i < e->c.exception_site_count; i++)
    e->c.exception_sites[i].landing_offset = (uint32_t)grjit_a64_label_offset(&e->as,
        e->blocks[e->c.exception_sites[i].landing_block]);
  return GRJIT_OK;
}

void grjit_a64_emit_free(GRJIT_A64Emit * e) {
  if (e->c.allocator == NULL) {
    return;
  }
  e->c.allocator->free_fn(e->c.allocator->ctx, e->blocks);
  grjit_a64_free(&e->as);
  grjit_emit_common_free(&e->c);
  memset(e, 0, sizeof *e);
}
