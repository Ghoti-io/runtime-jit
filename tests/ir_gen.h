/**
 * @file
 *
 * The generator of the differential: a seeded random function over every
 * operation, immediates at the edges, shifts by 0, 1, 63 and by registers
 * holding 63 and 64, every comparison, every memory width, calls with 0 to 6
 * arguments, GC-point calls, polls and guards. Shared by the differential,
 * which runs what it builds, and by the code-pinning test, which only emits it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_IR_GEN_H
#define GHOTI_IO_GRJIT_TESTS_IR_GEN_H

#include "test_helpers.h"

#include <random>

namespace irgen {

constexpr size_t kArena = 512;
constexpr size_t kArenaBase = 160; // the PTR parameter points here

inline const int64_t kEdge[] = {0, 1, -1, INT64_MIN, INT64_MAX, 2, -2, 255, -256, 0x7FFFFFFF,
    -0x80000000LL, 0xFFFFFFFFLL, 0x123456789ABCDEF0LL};

struct Gen {
  std::mt19937_64 rng;
  B b;
  GRJIT_VReg p0, p1, p2, p3;
  GRJIT_VReg q, c63, c64;
  GRJIT_VReg d[8];
  GRJIT_VReg r[2];
  GRJIT_VReg t;

  const void * const * helpers;
  GRJIT_PollHelper poll_fn;

  /* `helpers` is seven functions of 0 to 6 arguments; `poll_fn` the poll
   * helper. A test that only emits code passes made-up addresses. */
  Gen(uint64_t seed, const void * const * helper_table, GRJIT_PollHelper poll)
      : rng(seed), b("gen", 3), helpers(helper_table), poll_fn(poll) {}

  uint64_t pick(uint64_t n) { return rng() % n; }
  int64_t edge() {
    return pick(4) == 0 ? static_cast<int64_t>(rng())
                        : kEdge[pick(sizeof kEdge / sizeof *kEdge)];
  }
  GRJIT_VReg dreg() { return d[pick(8)]; }
  GRJIT_VReg rreg() { return r[pick(2)]; }
  GRJIT_Operand val() { return pick(3) == 0 ? I(edge()) : V(dreg()); }
  GRJIT_Operand shift_count() {
    switch (pick(6)) {
      case 0: return I(0);
      case 1: return I(1);
      case 2: return I(63);
      case 3: return V(c63);
      case 4: return V(c64);
      default: return V(dreg());
    }
  }
  std::vector<GRJIT_FrameSlot> state() {
    return {grjit_frame_slot_vreg(pick(4) == 0 ? rreg() : dreg()),
        grjit_frame_slot_constant(edge()), grjit_frame_slot_dead()};
  }

  /* Every random choice is made in a statement of its own, in a fixed order:
   * the order in which a compiler evaluates the arguments of a call is
   * unspecified, and a generator that depends on it builds different
   * functions under different compilers, which a pinned hash of their code
   * cannot tolerate. */
  void one_op() {
    static const GRJIT_OpKind bins[] = {GRJIT_OP_ADD, GRJIT_OP_SUB, GRJIT_OP_MUL,
        GRJIT_OP_AND, GRJIT_OP_OR, GRJIT_OP_XOR, GRJIT_OP_SHL, GRJIT_OP_SHR,
        GRJIT_OP_SAR};
    static const uint32_t widths[] = {8, 16, 32, 64};
    switch (pick(20)) {
      case 0: case 1: case 2: case 3: {
        GRJIT_OpKind k = bins[pick(9)];
        bool shift = k == GRJIT_OP_SHL || k == GRJIT_OP_SHR || k == GRJIT_OP_SAR;
        GRJIT_VReg dst = dreg();
        GRJIT_Operand x = pick(5) == 0 ? I(edge()) : V(dreg());
        GRJIT_Operand y = shift ? shift_count() : val();
        b.bin(k, dst, x, y);
        break;
      }
      case 4: {
        GRJIT_OpKind k = pick(2) ? GRJIT_OP_NEG : GRJIT_OP_NOT;
        GRJIT_VReg dst = dreg();
        GRJIT_VReg src = dreg();
        b.un(k, dst, V(src));
        break;
      }
      case 5: {
        GRJIT_Cmp c = static_cast<GRJIT_Cmp>(pick(GRJIT_CMP_COUNT));
        GRJIT_VReg dst = dreg();
        GRJIT_VReg x = dreg();
        GRJIT_Operand y = val();
        b.cmp(c, dst, V(x), y);
        break;
      }
      case 6: {
        GRJIT_VReg dst = dreg();
        int64_t imm = edge();
        b.cnst(dst, imm);
        break;
      }
      case 7: {
        GRJIT_VReg dst = dreg();
        GRJIT_Operand src = pick(4) == 0 ? I(edge()) : V(dreg());
        b.mov(dst, src);
        break;
      }
      case 8: case 9: {
        GRJIT_VReg dst = dreg();
        int32_t disp = static_cast<int32_t>(pick(260)) - 100;
        uint32_t width = widths[pick(4)];
        bool sign = pick(2) != 0;
        b.load(dst, q, disp, width, sign);
        break;
      }
      case 10: case 11: {
        int32_t disp = static_cast<int32_t>(pick(260)) - 100;
        uint32_t width = widths[pick(4)];
        GRJIT_Operand v = val();
        b.store(q, disp, width, v);
        break;
      }
      case 12:
        if (pick(2)) {
          GRJIT_VReg x = rreg();
          GRJIT_VReg y = rreg();
          b.mov(x, V(y));
        } else {
          GRJIT_VReg x = rreg();
          int64_t imm = (static_cast<int64_t>(pick(1000)) << 4) | 1;
          b.cnst(x, imm);
        }
        break;
      case 13: {
        GRJIT_Cmp c = pick(2) ? GRJIT_CMP_EQ : GRJIT_CMP_NE;
        GRJIT_VReg dst = dreg();
        GRJIT_VReg x = rreg();
        GRJIT_VReg y = rreg();
        b.cmp(c, dst, V(x), V(y));
        break;
      }
      case 14: case 15: {
        size_t n = pick(7);
        std::vector<GRJIT_Operand> args;
        for (size_t i = 0; i < n; i++) {
          GRJIT_Operand a = pick(5) == 0 ? V(rreg()) : val();
          args.push_back(a);
        }
        GRJIT_VReg dst = pick(4) == 0 ? GRJIT_NO_VREG : dreg();
        if (pick(2)) {
          b.call(dst, helpers[n], args);
        } else {
          const GRCORE_CodeSiteKind kinds[] = {GRCORE_SITE_GC_POINT_POLL,
              GRCORE_SITE_GC_POINT_ALLOC_SLOW, GRCORE_SITE_GC_POINT_CALL,
              GRCORE_SITE_GC_POINT_FRAME_PUSH, GRCORE_SITE_GC_POINT_NESTED_ENTRY};
          uint64_t offset = pick(100);
          std::vector<GRJIT_FrameSlot> st = state();
          GRCORE_CodeSiteKind kind = kinds[pick(5)];
          b.call_gc(dst, helpers[n], args, {1, offset}, st, kind);
        }
        break;
      }
      case 18: {
        GRJIT_VReg dst = dreg();
        GRJIT_VReg src = rreg();
        b.bitcast(dst, src);
        break;
      }
      case 19: {
        GRJIT_VReg dst = rreg();
        GRJIT_VReg src = dreg();
        b.bitcast(dst, src);
        break;
      }
      case 16: {
        uint64_t offset = pick(100);
        std::vector<GRJIT_FrameSlot> st = state();
        b.poll({2, offset}, st);
        break;
      }
      default: {
        // A guard that holds about half the time.
        GRJIT_Cmp c = static_cast<GRJIT_Cmp>(pick(GRJIT_CMP_COUNT));
        GRJIT_VReg x = dreg();
        GRJIT_Operand y = val();
        b.cmp(c, t, V(x), y);
        if (pick(3) == 0) {
          uint64_t offset = pick(100);
          std::vector<GRJIT_FrameSlot> st = state();
          b.guard(V(t), {3, offset}, st);
        }
        break;
      }
    }
  }

  void straight(int n) {
    for (int i = 0; i < n; i++) {
      one_op();
    }
  }

  /* A segment of the function, continuing in the current block, which is
   * where control is when this returns. */
  GRJIT_BlockId segment(GRJIT_BlockId cur, int depth) {
    b.at(cur);
    straight(1 + static_cast<int>(pick(8)));
    int kind = depth >= 2 ? 0 : static_cast<int>(pick(4));
    if (kind == 1) { // if / else
      GRJIT_BlockId then_b = b.block(), else_b = b.block(), join = b.block();
      b.at(cur);
      GRJIT_Cmp c = static_cast<GRJIT_Cmp>(pick(GRJIT_CMP_COUNT));
      GRJIT_VReg x = dreg();
      GRJIT_Operand y = val();
      b.cmp(c, t, V(x), y);
      b.br_if(V(t), then_b, else_b);
      GRJIT_BlockId t_end = segment(then_b, depth + 1);
      b.at(t_end);
      b.br(join);
      GRJIT_BlockId e_end = segment(else_b, depth + 1);
      b.at(e_end);
      b.br(join);
      return join;
    }
    if (kind == 2) { // a counted loop of 1..4 iterations
      GRJIT_VReg i = b.reg();
      GRJIT_BlockId head = b.block(), body = b.block(), after = b.block();
      b.at(cur);
      b.cnst(i, 0);
      b.br(head);
      b.at(head);
      int64_t bound = 1 + static_cast<int64_t>(pick(4));
      b.cmp(GRJIT_CMP_LT, t, V(i), I(bound));
      b.br_if(V(t), body, after);
      GRJIT_BlockId body_end = segment(body, depth + 1);
      b.at(body_end);
      b.bin(GRJIT_OP_ADD, i, V(i), I(1));
      b.br(head);
      return after;
    }
    return cur;
  }

  GRJIT_Function * build() {
    p0 = b.param(GRJIT_TYPE_PTR);
    p1 = b.param(GRJIT_TYPE_I64);
    p2 = b.param(GRJIT_TYPE_I64);
    p3 = b.param(GRJIT_TYPE_REF);
    q = b.reg(GRJIT_TYPE_PTR);
    c63 = b.reg();
    c64 = b.reg();
    for (auto & v : d) {
      v = b.reg();
    }
    for (auto & v : r) {
      v = b.reg(GRJIT_TYPE_REF);
    }
    t = b.reg();
    b.poll_helper(poll_fn);
    GRJIT_BlockId entry = b.block();
    b.at(entry);
    b.mov(q, V(p0));
    b.cnst(c63, 63);
    b.cnst(c64, 64);
    b.mov(d[0], V(p1));
    b.mov(d[1], V(p2));
    for (int i = 2; i < 8; i++) {
      b.cnst(d[i], edge());
    }
    b.mov(r[0], V(p3));
    b.cnst(r[1], (static_cast<int64_t>(pick(1000)) << 4) | 1);
    b.cnst(t, 0);
    GRJIT_BlockId end = segment(entry, 0);
    if (pick(2)) {
      end = segment(end, 1);
    }
    b.at(end);
    // The result folds every data register, so a wrong one cannot hide.
    GRJIT_VReg acc = t;
    b.mov(acc, I(0));
    for (auto v : d) {
      b.bin(GRJIT_OP_XOR, acc, V(acc), V(v));
      b.bin(GRJIT_OP_MUL, acc, V(acc), I(0x100000001B3));
    }
    GRJIT_VReg sink = dreg();
    b.cmp(GRJIT_CMP_EQ, sink, V(r[0]), V(r[1]));
    b.ret(V(acc));
    return b.finish();
  }
};

} // namespace irgen

#endif
