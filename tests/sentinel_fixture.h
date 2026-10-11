/**
 * @file
 *
 * What the sentinel tests of the arm64 and Win64 numeric files share (spec-runtime-float, story 5): a plain function that
 * uses every family of float operation, stores and loads each width, calls a helper that trashes the caller-saved
 * vector registers with a float live across the call, polls, and guards on a mode; the poll helper and the world that
 * can post the request that takes its slow path; and a run through the sentinel trampoline of calls_asm.h
 * (`fx_call_with_sentinels`), which loads the callee-saved registers with known values, calls the code and reads them
 * back. The including file defines `FX_ASM_SENTINELS` and includes calls_asm.h first.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_SENTINEL_FIXTURE_H
#define GHOTI_IO_GRJIT_TESTS_SENTINEL_FIXTURE_H

#include "test_helpers.h"

#include "float_fixture.h"

#if FX_HAVE_CALLS_ASM && defined(FX_ASM_SENTINELS)

namespace sentfx {

inline uint32_t g_answer = 0;
inline int g_polls = 0;
inline uint32_t sentinel_poll(void *, uint64_t, uint64_t) {
  g_polls++;
  return g_answer;
}

inline const GRCORE_Key kSentinelKey = GRCORE_KEY_INIT("sentinel", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE, nullptr,
    nullptr, nullptr, nullptr, nullptr);

/* (A null `clobber` leaves the call out, which gives the twin the evaluator can run: the evaluator calls a helper with
 * integer registers only, and the helper is the identity on its float, so the twin's answer is the one to expect.)
 *
 * f(a: f64, b: f32, mode: i64, p: ptr) -> i64: every family of float operation, a store and a load of each width,
 * a helper that trashes the vector registers with a float live across it, a poll and a guard on `mode`. The result
 * folds every float (the inputs are finite and no operation makes a NaN, so the bits are fixed).
 *
 * Precondition: a caller never passes `a == 0`. The sequence divides by `a` (`t3 / a`), and a zero would make the
 * quotient a NaN and the folded bits depend on the host's default NaN. */
inline GRJIT_Function * sentinel_function(const void * clobber, size_t fold_limit = SIZE_MAX) {
  using namespace floatfx;
  B b("sentinel", 2);
  b.poll_helper(sentinel_poll);
  GRJIT_VReg a = b.param(GRJIT_TYPE_F64), f32 = b.param(GRJIT_TYPE_F32), mode = b.param(GRJIT_TYPE_I64),
             p = b.param(GRJIT_TYPE_PTR);
  auto F = [&] { return b.reg(GRJIT_TYPE_F64); };
  auto S = [&] { return b.reg(GRJIT_TYPE_F32); };
  GRJIT_VReg t1 = F(), t2 = F(), t3 = F(), t4 = F(), m1 = F(), m2 = F(), cs = F(), ng = F(), ab = F(), sq = F(),
             c1 = F(), c2 = F(), c3 = F(), c4 = F(), j = F(), pro = F(), rf = F(), kept = F(), after = F(), ld = F();
  GRJIT_VReg s1 = S(), s2 = S(), s3 = S(), s4 = S(), n1 = S(), n2 = S(), dem = S(), k = S(), sl = S(), sq32 = S(),
             sc = S(), sf = S(), st = S(), sn = S();
  GRJIT_VReg lt = b.reg(GRJIT_TYPE_I32), ge = b.reg(GRJIT_TYPE_I32), i = b.reg(GRJIT_TYPE_I64),
             u = b.reg(GRJIT_TYPE_I64), ri = b.reg(GRJIT_TYPE_I64), i32 = b.reg(GRJIT_TYPE_I32),
             u32 = b.reg(GRJIT_TYPE_I32), r32 = b.reg(GRJIT_TYPE_I32), tr = b.reg(GRJIT_TYPE_I32),
             l = b.reg(GRJIT_TYPE_I32), h = b.reg(GRJIT_TYPE_I32), ok = b.reg(GRJIT_TYPE_I32),
             ld32 = b.reg(GRJIT_TYPE_F32);
  GRJIT_VReg acc = b.reg(GRJIT_TYPE_I64);
  b.at(b.block());
  auto state = [&] {
    return std::vector<GRJIT_FrameSlot>{grjit_frame_slot_vreg(mode), grjit_frame_slot_vreg(a)};
  };
  // arithmetic, min, max, copysign, neg, abs, sqrt, the four roundings
  b.bin(GRJIT_OP_FADD, t1, V(a), V(a));
  b.bin(GRJIT_OP_FSUB, t2, V(t1), V(a));
  b.bin(GRJIT_OP_FMUL, t3, V(t2), V(a));
  b.bin(GRJIT_OP_FDIV, t4, V(t3), V(a));
  b.bin(GRJIT_OP_FMIN, m1, V(t4), V(a));
  b.bin(GRJIT_OP_FMAX, m2, V(t4), V(a));
  b.bin(GRJIT_OP_FCOPYSIGN, cs, V(m1), V(a));
  b.un(GRJIT_OP_FNEG, ng, V(cs));
  b.un(GRJIT_OP_FABS, ab, V(ng));
  b.un(GRJIT_OP_FSQRT, sq, V(ab));
  b.un(GRJIT_OP_FCEIL, c1, V(sq));
  b.un(GRJIT_OP_FFLOOR, c2, V(sq));
  b.un(GRJIT_OP_FTRUNC, c3, V(sq));
  b.un(GRJIT_OP_FNEAREST, c4, V(sq));
  // the single-precision twins
  b.bin(GRJIT_OP_FADD, s1, V(f32), V(f32));
  b.bin(GRJIT_OP_FSUB, s2, V(s1), V(f32));
  b.bin(GRJIT_OP_FMUL, s3, V(s2), V(f32));
  b.bin(GRJIT_OP_FDIV, s4, V(s3), V(f32));
  b.bin(GRJIT_OP_FMIN, n1, V(s4), V(f32));
  b.bin(GRJIT_OP_FMAX, n2, V(s4), V(f32));
  b.bin(GRJIT_OP_FCOPYSIGN, sc, V(n1), V(f32));
  b.un(GRJIT_OP_FSQRT, sq32, V(n2));
  b.un(GRJIT_OP_FNEAREST, sn, V(sq32));
  b.un(GRJIT_OP_FNEG, sf, V(sc));
  b.un(GRJIT_OP_FABS, st, V(sf));
  b.un(GRJIT_OP_FFLOOR, sl, V(st));
  // comparisons, conversions both ways, demote, promote, reinterpret, the saturating and the guarded truncation
  b.cmp(GRJIT_CMP_LT, lt, V(a), V(t1));
  b.cmp(GRJIT_CMP_GE, ge, V(f32), V(s1));
  b.un(GRJIT_OP_TRUNC_SAT_S, i, V(a));
  b.un(GRJIT_OP_CONVERT_S, j, V(i));
  b.un(GRJIT_OP_TRUNC_SAT_U, u, V(f32));
  b.un(GRJIT_OP_CONVERT_U, k, V(u));
  b.un(GRJIT_OP_DEMOTE, dem, V(a));
  b.un(GRJIT_OP_PROMOTE, pro, V(f32));
  b.un(GRJIT_OP_REINTERPRET, ri, V(a));
  b.un(GRJIT_OP_REINTERPRET, rf, V(ri));
  b.un(GRJIT_OP_TRUNC_SAT_S, i32, V(f32));
  b.un(GRJIT_OP_CONVERT_U, after, V(i32));
  b.un(GRJIT_OP_TRUNC_SAT_U, u32, V(a));
  b.un(GRJIT_OP_REINTERPRET, r32, V(f32));
  b.cmp(GRJIT_CMP_GT, l, V(a), grjit_operand_f64_imm(f64bits(-2147483649.0)));
  b.cmp(GRJIT_CMP_LT, h, V(a), grjit_operand_f64_imm(f64bits(2147483648.0)));
  b.bin(GRJIT_OP_AND, ok, V(l), V(h));
  b.guard(V(ok), {kTrapFunction, kSiteIntegerOverflow}, state());
  b.un(GRJIT_OP_TRUNC_S, tr, V(a));
  // a store and a load of each width
  b.store(p, 0, 64, V(c4));
  b.store(p, 8, 32, V(sn));
  b.load(ld, p, 0, 64);
  b.load(ld32, p, 8, 32);
  // a float live across a helper that trashes every caller-saved vector register
  b.mov(kept, V(t4));
  if (clobber != nullptr) {
    b.call(c1, clobber, {V(c1)});
  }
  // a poll, whose slow path calls the helper, and a guard on the mode
  b.poll({7, 1}, state());
  b.guard(V(mode), {7, 2}, state());
  // fold: every register through its bits
  size_t folded = 0;
  auto fold64 = [&](GRJIT_VReg f) {
    if (folded++ >= fold_limit) return;
    GRJIT_VReg bits = b.reg(GRJIT_TYPE_I64);
    b.un(GRJIT_OP_REINTERPRET, bits, V(f));
    b.bin(GRJIT_OP_ROTL, acc, V(acc), I(5));
    b.bin(GRJIT_OP_XOR, acc, V(acc), V(bits));
  };
  auto fold32 = [&](GRJIT_VReg f) {
    if (folded++ >= fold_limit) return;
    GRJIT_VReg bits = b.reg(GRJIT_TYPE_I32), wide = b.reg(GRJIT_TYPE_I64);
    b.un(GRJIT_OP_REINTERPRET, bits, V(f));
    b.un(GRJIT_OP_EXTEND_U, wide, V(bits));
    b.bin(GRJIT_OP_ROTL, acc, V(acc), I(7));
    b.bin(GRJIT_OP_XOR, acc, V(acc), V(wide));
  };
  auto foldi = [&](GRJIT_VReg v, bool wide32) {
    if (folded++ >= fold_limit) return;
    GRJIT_VReg x = v;
    if (wide32) {
      x = b.reg(GRJIT_TYPE_I64);
      b.un(GRJIT_OP_EXTEND_U, x, V(v));
    }
    b.bin(GRJIT_OP_ROTL, acc, V(acc), I(3));
    b.bin(GRJIT_OP_XOR, acc, V(acc), V(x));
  };
  b.cnst(acc, 0);
  for (GRJIT_VReg f : {t1, t2, t3, t4, m1, m2, cs, ng, ab, sq, c1, c2, c3, c4, j, pro, rf, kept, after, ld}) {
    fold64(f);
  }
  for (GRJIT_VReg f : {s1, s2, s3, s4, n1, n2, dem, k, sl, sq32, sc, sf, st, sn, ld32}) {
    fold32(f);
  }
  foldi(i, false);
  foldi(u, false);
  foldi(ri, false);
  foldi(lt, true);
  foldi(ge, true);
  foldi(i32, true);
  foldi(u32, true);
  foldi(r32, true);
  foldi(tr, true);
  b.ret(V(acc));
  return b.finish();
}

struct SentinelWorld : JitWorld {
  GRCORE_RequestKind kind = 0;
  GRCORE_Port * port = nullptr;
  SentinelWorld() {
    EXPECT_EQ(grcore_context_request_kind(ctx, &kSentinelKey, &kind), GRCORE_OK);
    EXPECT_EQ(grcore_context_port(ctx, &port), GRCORE_OK);
    g_polls = 0;
    g_answer = 0;
  }
  ~SentinelWorld() { grcore_port_release(port); }
  void post() { EXPECT_EQ(grcore_port_post(port, kind), GRCORE_OK); }
};

struct SentinelRun {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
  bool intact = true;
};

/* Runs `code` through the sentinel trampoline and checks every sentinel (x19-x29 and the low halves of d8-d15). */
inline SentinelRun run_with_sentinels(JitWorld & w, const GRJIT_Code * code, std::vector<uint64_t> args, const char * what) {
  SentinelRun r;
  r.out.assign(grjit_code_out_words(code), 0xDEADBEEFu);
  args.resize(std::max<size_t>(args.size(), grjit_code_param_count(code)));
  uint64_t regs[fx::kSentinelCount];
  for (size_t i = 0; i < fx::kSentinelCount; i++) {
    regs[i] = fx::sentinel_value(i);
  }
  r.exit = fx_call_with_sentinels(grjit_code_entry(code), w.ctx, args.data(), r.out.data(), regs);
  for (size_t i = 0; i < fx::kSentinelCount; i++) {
    if (regs[i] != fx::sentinel_value(i)) {
      r.intact = false;
      ADD_FAILURE() << fx::sentinel_name(i) << " changed after " << what << ": " << std::hex << regs[i]
                    << ", it was " << fx::sentinel_value(i);
    }
  }
  return r;
}


} // namespace sentfx

#endif // FX_HAVE_CALLS_ASM && FX_ASM_SENTINELS

#endif // GHOTI_IO_GRJIT_TESTS_SENTINEL_FIXTURE_H
