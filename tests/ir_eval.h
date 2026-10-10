/**
 * @file
 *
 * A test-only evaluator of the IR (AD-9: the library never interprets it).
 *
 * It exists to be wrong in a different way from the backend: it reads the
 * operations from the function and does in C++ what each is documented to do
 * in `ir.h`, with vregs as 64-bit words, memory as real memory, calls to the
 * same helpers by address, and a guard failure as an exit with the same
 * reconstructed slots. The differential test compares the two.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_IR_EVAL_H
#define GHOTI_IO_GRJIT_TESTS_IR_EVAL_H

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/a/layout.h>

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__FAST_MATH__)
#error "the evaluator and every float test are built without -ffast-math"
#endif

namespace ireval {

/* The exit the evaluator reports for a partial operation reached with operands its guards
 * should have kept out: no compiled run has it. */
constexpr uint32_t kPartialFault = 0xBAD0;

struct Result {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
  GRCORE_PollIdentity guard_identity{0, 0};
  bool timed_out = false;
};

inline uint64_t call_native(uint64_t address, const uint64_t * a, size_t n) {
  auto fn = reinterpret_cast<void *>(address);
  switch (n) {
    case 0: return reinterpret_cast<uint64_t (*)()>(fn)();
    case 1: return reinterpret_cast<uint64_t (*)(uint64_t)>(fn)(a[0]);
    case 2: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(fn)(a[0], a[1]);
    case 3: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2]);
    case 4: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2], a[3]);
    case 5: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2], a[3], a[4]);
    default: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
  }
}

inline uint64_t operand(const std::vector<uint64_t> & regs, const GRJIT_Operand & o) {
  return o.kind == GRJIT_OPERAND_VREG ? regs[o.vreg] : static_cast<uint64_t>(o.imm);
}

/* ---- The integer operations (numeric-operations.md), written to be wrong differently from the
 * backend: no builtins, no instruction a compiler would turn into the backend's own, and the
 * width carried in a plain number. An i32 is the low 32 bits of a word and every result of
 * width 32 is cut to them, so the invariant the backend keeps (zero-extended) is stated here
 * by construction. */

inline uint64_t cut(unsigned width, uint64_t v) { return width == 32 ? (v & 0xFFFFFFFFu) : v; }

inline int64_t sext(unsigned width, uint64_t v) {
  if (width == 32) {
    uint64_t low = v & 0xFFFFFFFFu;
    return static_cast<int64_t>(low >= 0x80000000u ? low | 0xFFFFFFFF00000000u : low);
  }
  return static_cast<int64_t>(v);
}

inline uint64_t count_leading_zeros(unsigned width, uint64_t v) {
  v = cut(width, v);
  uint64_t n = 0;
  for (int bit = static_cast<int>(width) - 1; bit >= 0 && ((v >> bit) & 1) == 0; bit--) {
    n++;
  }
  return n;
}

inline uint64_t count_trailing_zeros(unsigned width, uint64_t v) {
  v = cut(width, v);
  uint64_t n = 0;
  for (unsigned bit = 0; bit < width && ((v >> bit) & 1) == 0; bit++) {
    n++;
  }
  return n;
}

inline uint64_t population(unsigned width, uint64_t v) {
  uint64_t n = 0;
  for (unsigned bit = 0; bit < width; bit++) {
    n += (v >> bit) & 1;
  }
  return n;
}

inline uint64_t rotate_left(unsigned width, uint64_t v, uint64_t count) {
  v = cut(width, v);
  unsigned k = static_cast<unsigned>(count % width);
  if (k == 0) {
    return v;
  }
  return cut(width, (v << k) | (v >> (width - k)));
}

inline uint64_t sign_extend_from(unsigned from, unsigned width, uint64_t v) {
  uint64_t low = from == 64 ? v : (v & ((uint64_t{1} << from) - 1));
  if ((low >> (from - 1)) & 1) {
    low |= from == 64 ? 0 : ~uint64_t{0} << from;
  }
  return cut(width, low);
}

inline bool compare(GRJIT_Cmp c, uint64_t a, uint64_t b) {
  int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
  switch (c) {
    case GRJIT_CMP_EQ: return a == b;
    case GRJIT_CMP_NE: return a != b;
    case GRJIT_CMP_LT: return sa < sb;
    case GRJIT_CMP_LE: return sa <= sb;
    case GRJIT_CMP_GT: return sa > sb;
    case GRJIT_CMP_GE: return sa >= sb;
    case GRJIT_CMP_ULT: return a < b;
    case GRJIT_CMP_ULE: return a <= b;
    case GRJIT_CMP_UGT: return a > b;
    default: return a >= b;
  }
}

/* ---- The float operations (numeric-operations.md; spec-runtime-float, story 3) ---------------------
 *
 * Written out, not borrowed: `fmin`, `fmax`, `rint`, `nearbyint`, `ceil`, `floor`, `trunc` and the
 * float-to-integer and integer-to-float casts differ from WebAssembly in a NaN, in a signed zero,
 * in a tie or in what they do out of range, and are exactly where an emitter and a hand-written
 * evaluator would share a mistake if both leaned on the C library. What is borrowed is IEEE
 * arithmetic itself (+ - * / and sqrt on float and double, which the C++ source states and the
 * tests build with -ffp-contract=off), and the width conversions between float and double, which
 * are the one thing the standard defines exactly.
 *
 * Floats are compared by bits with any two NaNs equal (`same_float`): C++ may swap the operands
 * of a commutative `a + b`, so which payload survives when both are NaN is not the source's. */

inline float f32_of(uint64_t bits) {
  uint32_t b = static_cast<uint32_t>(bits);
  float f;
  std::memcpy(&f, &b, sizeof f);
  return f;
}
inline uint64_t bits_of(float f) {
  uint32_t b;
  std::memcpy(&b, &f, sizeof b);
  return b;
}
inline double f64_of(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof d);
  return d;
}
inline uint64_t bits_of(double d) {
  uint64_t b;
  std::memcpy(&b, &d, sizeof b);
  return b;
}

/* A float of `width` (32 or 64) bits as a double, exactly (every float is a double). */
inline double as_double(unsigned width, uint64_t bits) {
  return width == 32 ? static_cast<double>(f32_of(bits)) : f64_of(bits);
}

inline bool is_nan_bits(unsigned width, uint64_t bits) {
  if (width == 32) {
    return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
  }
  return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (bits & 0x000FFFFFFFFFFFFFull) != 0;
}

/* The result of an operation on a NaN: a NaN, quiet. Which one is not fixed (see above). */
inline uint64_t quiet_nan(unsigned width) { return width == 32 ? 0x7FC00000u : 0x7FF8000000000000ull; }

/* The bit-for-bit comparison of the tests: equal bits, or both NaN. A NaN never equals a number,
 * and +0 and -0 differ. */
inline bool same_float(unsigned width, uint64_t a, uint64_t b) {
  const uint64_t mask = width == 32 ? 0xFFFFFFFFull : ~uint64_t{0};
  a &= mask;
  b &= mask;
  if (is_nan_bits(width, a) || is_nan_bits(width, b)) {
    return is_nan_bits(width, a) && is_nan_bits(width, b);
  }
  return a == b;
}

inline uint64_t sign_bit(unsigned width) { return width == 32 ? 0x80000000ull : 0x8000000000000000ull; }

inline bool negative(unsigned width, uint64_t bits) { return (bits & sign_bit(width)) != 0; }

/* The double `d` (the value of a float of `width` bits, exactly, or an integer that fits) as that
 * float's bits. The cast to float rounds to nearest even, once, from a double that is itself exact. */
inline uint64_t from_double(unsigned width, double d) {
  return width == 32 ? bits_of(static_cast<float>(d)) : bits_of(d);
}

/* An integer-valued result with the sign of `like` when it is a zero. */
inline uint64_t integer_result(unsigned width, double value, uint64_t like) {
  uint64_t bits = from_double(width, value);
  if (value == 0.0) {
    bits = negative(width, like) ? sign_bit(width) : 0;
  }
  return bits;
}

/* trunc, floor, ceil, nearest of a float of `width` bits. The 2^52 (2^23) bound is the magnitude
 * from which every float is an integer; below it, a signed 64-bit cast is exact. */
inline uint64_t round_float(GRJIT_OpKind kind, unsigned width, uint64_t bits) {
  if (is_nan_bits(width, bits)) {
    return quiet_nan(width);
  }
  const double x = as_double(width, bits);
  const double limit = width == 32 ? 8388608.0 : 4503599627370496.0;
  if (x >= limit || x <= -limit || x - x != 0.0 /* an infinity */) {
    return bits;
  }
  double t = static_cast<double>(static_cast<int64_t>(x)); /* toward zero */
  switch (kind) {
    case GRJIT_OP_FTRUNC:
      break;
    case GRJIT_OP_FFLOOR:
      if (t > x) {
        t -= 1.0;
      }
      break;
    case GRJIT_OP_FCEIL:
      if (t < x) {
        t += 1.0;
      }
      break;
    default: { /* nearest, ties to even */
      double f = t;
      if (f > x) {
        f -= 1.0; /* floor */
      }
      const double diff = x - f; /* in [0, 1), exact */
      if (diff < 0.5) {
        t = f;
      } else if (diff > 0.5) {
        t = f + 1.0;
      } else {
        t = (static_cast<int64_t>(f) % 2 == 0) ? f : f + 1.0;
      }
      break;
    }
  }
  return integer_result(width, t, bits);
}

/* min and max as WebAssembly defines them. */
inline uint64_t min_max(bool want_min, unsigned width, uint64_t a, uint64_t b) {
  if (is_nan_bits(width, a) || is_nan_bits(width, b)) {
    return quiet_nan(width);
  }
  const double x = as_double(width, a), y = as_double(width, b);
  if (x == 0.0 && y == 0.0) {
    /* min: -0 if either is -0; max: +0 if either is +0 */
    return want_min ? (negative(width, a) ? a : b) : (negative(width, a) ? b : a);
  }
  return (want_min ? x < y : x > y) ? a : b;
}

/* A correctly rounded (to nearest, ties to even) float of `width` bits from an integer magnitude
 * and a sign, by integer arithmetic alone. */
inline uint64_t float_from_integer(unsigned width, uint64_t magnitude, bool neg) {
  const unsigned mant_bits = width == 32 ? 23 : 52;
  const unsigned bias = width == 32 ? 127 : 1023;
  uint64_t sign = neg ? sign_bit(width) : 0;
  if (magnitude == 0) {
    return 0; /* an integer zero converts to +0 */
  }
  unsigned msb = 63;
  while (((magnitude >> msb) & 1) == 0) {
    msb--;
  }
  uint64_t mant;
  if (msb <= mant_bits) {
    mant = magnitude << (mant_bits - msb);
  } else {
    const unsigned shift = msb - mant_bits;
    mant = magnitude >> shift;
    const uint64_t rem = magnitude & ((uint64_t{1} << shift) - 1);
    const uint64_t half = uint64_t{1} << (shift - 1);
    if (rem > half || (rem == half && (mant & 1))) {
      mant++;
      if (mant == (uint64_t{2} << mant_bits)) { /* carried out of the significand */
        mant >>= 1;
        msb++;
      }
    }
  }
  const uint64_t exponent = msb + bias;
  return sign | (exponent << mant_bits) | (mant & ((uint64_t{1} << mant_bits) - 1));
}

/* The truncation of a float to a destination: false when it does not fit (a NaN included), which
 * is a trap that the guards in front of a partial operation keep out. `bits` is 32 or 64, signed
 * or not. */
inline bool trunc_in_range(unsigned src_width, uint64_t bits, unsigned dst_bits, bool is_signed,
    uint64_t * out) {
  if (is_nan_bits(src_width, bits)) {
    return false;
  }
  const double x = as_double(src_width, bits);
  if (x - x != 0.0) {
    return false;
  }
  double lo, hi; /* the exact bounds, exclusive */
  if (is_signed) {
    lo = dst_bits == 32 ? -2147483649.0 : -9223372036854775809.0;
    hi = dst_bits == 32 ? 2147483648.0 : 9223372036854775808.0;
  } else {
    lo = -1.0;
    hi = dst_bits == 32 ? 4294967296.0 : 18446744073709551616.0;
  }
  /* -2^63 - 1 is not a double: it rounds to -2^63, so the bound is "at least" there. */
  const bool below = dst_bits == 64 && is_signed ? x < lo : x <= lo;
  if (below || x >= hi) {
    return false;
  }
  if (is_signed) {
    *out = static_cast<uint64_t>(static_cast<int64_t>(x));
  } else if (x >= 9223372036854775808.0) {
    *out = static_cast<uint64_t>(static_cast<int64_t>(x - 9223372036854775808.0)) + (uint64_t{1} << 63);
  } else {
    *out = static_cast<uint64_t>(static_cast<int64_t>(x));
  }
  if (dst_bits == 32) {
    *out &= 0xFFFFFFFFu;
  }
  return true;
}

/* The saturating truncation: NaN is 0, out of range is the nearest bound. */
inline uint64_t trunc_sat(unsigned src_width, uint64_t bits, unsigned dst_bits, bool is_signed) {
  if (is_nan_bits(src_width, bits)) {
    return 0;
  }
  const double x = as_double(src_width, bits);
  uint64_t v;
  if (trunc_in_range(src_width, bits, dst_bits, is_signed, &v)) {
    return v;
  }
  const bool up = x > 0.0;
  if (is_signed) {
    if (dst_bits == 32) {
      return up ? 0x7FFFFFFFull : 0x80000000ull;
    }
    return up ? 0x7FFFFFFFFFFFFFFFull : 0x8000000000000000ull;
  }
  if (!up) {
    return 0;
  }
  return dst_bits == 32 ? 0xFFFFFFFFull : ~uint64_t{0};
}

/* One float or conversion operation; false for an operation that is not one. `a` and `b` are the
 * operand words, `dw` the destination's width in bits, `sw` the width of the source register, and
 * `src_int` whether it is an integer. A partial truncation out of range is a program error
 * (`partial` set), reported like a division by zero. */
inline bool float_op(const GRJIT_Op & op, unsigned dw, unsigned sw, uint64_t a, uint64_t b,
    bool dst_is_float, uint64_t * out, bool * partial) {
  const uint64_t sign = sign_bit(dw);
  switch (op.kind) {
    case GRJIT_OP_FADD:
    case GRJIT_OP_FSUB:
    case GRJIT_OP_FMUL:
    case GRJIT_OP_FDIV:
      if (dw == 32) {
        const float x = f32_of(a), y = f32_of(b);
        const float r = op.kind == GRJIT_OP_FADD ? x + y
                        : op.kind == GRJIT_OP_FSUB ? x - y
                        : op.kind == GRJIT_OP_FMUL ? x * y
                                                   : x / y;
        *out = bits_of(r);
      } else {
        const double x = f64_of(a), y = f64_of(b);
        const double r = op.kind == GRJIT_OP_FADD ? x + y
                         : op.kind == GRJIT_OP_FSUB ? x - y
                         : op.kind == GRJIT_OP_FMUL ? x * y
                                                    : x / y;
        *out = bits_of(r);
      }
      return true;
    case GRJIT_OP_FSQRT:
      *out = dw == 32 ? bits_of(static_cast<float>(__builtin_sqrt(static_cast<double>(f32_of(a)))))
                      : bits_of(__builtin_sqrt(f64_of(a)));
      return true;
    case GRJIT_OP_FMIN:
    case GRJIT_OP_FMAX:
      *out = min_max(op.kind == GRJIT_OP_FMIN, dw, a, b);
      return true;
    case GRJIT_OP_FCOPYSIGN:
      *out = (a & ~sign & (dw == 32 ? 0xFFFFFFFFull : ~uint64_t{0})) | (b & sign);
      return true;
    case GRJIT_OP_FNEG:
      *out = (a ^ sign) & (dw == 32 ? 0xFFFFFFFFull : ~uint64_t{0});
      return true;
    case GRJIT_OP_FABS:
      *out = a & ~sign & (dw == 32 ? 0xFFFFFFFFull : ~uint64_t{0});
      return true;
    case GRJIT_OP_FCEIL:
    case GRJIT_OP_FFLOOR:
    case GRJIT_OP_FTRUNC:
    case GRJIT_OP_FNEAREST:
      *out = round_float(op.kind, dw, a);
      return true;
    case GRJIT_OP_CONVERT_S: {
      const int64_t v = sw == 32 ? sext(32, a) : static_cast<int64_t>(a);
      const bool neg = v < 0;
      *out = float_from_integer(dw, neg ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v), neg);
      return true;
    }
    case GRJIT_OP_CONVERT_U:
      *out = float_from_integer(dw, sw == 32 ? (a & 0xFFFFFFFFu) : a, false);
      return true;
    case GRJIT_OP_TRUNC_S:
    case GRJIT_OP_TRUNC_U: {
      uint64_t v = 0;
      if (!trunc_in_range(sw, a, dw, op.kind == GRJIT_OP_TRUNC_S, &v)) {
        *partial = true;
        *out = 0;
      } else {
        *out = v;
      }
      return true;
    }
    case GRJIT_OP_TRUNC_SAT_S:
    case GRJIT_OP_TRUNC_SAT_U:
      *out = trunc_sat(sw, a, dw, op.kind == GRJIT_OP_TRUNC_SAT_S);
      return true;
    case GRJIT_OP_DEMOTE:
      *out = bits_of(static_cast<float>(f64_of(a)));
      return true;
    case GRJIT_OP_PROMOTE:
      *out = bits_of(static_cast<double>(f32_of(a)));
      return true;
    case GRJIT_OP_REINTERPRET:
      *out = dw == 32 ? (a & 0xFFFFFFFFu) : a;
      return true;
    default:
      (void)dst_is_float;
      return false;
  }
}

/* A float comparison: the ordered conditions, false for any NaN except NE. */
inline bool float_compare(GRJIT_Cmp c, unsigned width, uint64_t a, uint64_t b) {
  if (is_nan_bits(width, a) || is_nan_bits(width, b)) {
    return c == GRJIT_CMP_NE;
  }
  const double x = as_double(width, a), y = as_double(width, b);
  switch (c) {
    case GRJIT_CMP_EQ: return x == y;
    case GRJIT_CMP_NE: return x != y;
    case GRJIT_CMP_LT: return x < y;
    case GRJIT_CMP_LE: return x <= y;
    case GRJIT_CMP_GT: return x > y;
    default: return x >= y;
  }
}

inline void deopt_slots(const GRJIT_Function * f, const GRJIT_Op & op,
    const std::vector<uint64_t> & regs, Result * r) {
  const GRJIT_FrameState * s = grjit_function_frame_state(f, op.state);
  size_t n = grjit_function_interp_slot_count(f);
  r->out.assign(n + 1, 0);
  for (size_t i = 0; i < n; i++) {
    const GRJIT_FrameSlot & slot = s->slots[i];
    r->out[i] = slot.kind == GRJIT_FRAME_SLOT_VREG ? regs[slot.vreg]
              : slot.kind == GRJIT_FRAME_SLOT_CONSTANT ? static_cast<uint64_t>(slot.constant)
                                                       : 0;
  }
  r->guard_identity = s->identity;
  r->exit = GRJIT_EXIT_DEOPT;
}

/* Runs `f` on `args`. `ctx` is the context compiled polls load through. */
inline Result eval(const GRJIT_Function * f, void * ctx, GRJIT_EntryHook hook,
    const std::vector<uint64_t> & args, size_t step_limit = 2000000) {
  Result r;
  size_t slots = grjit_function_interp_slot_count(f);
  r.out.assign(slots + 1, 0xDEADBEEFu);
  if (hook != nullptr) {
    uint32_t refused = hook(ctx);
    if (refused != 0) {
      r.out[0] = refused;
      r.exit = GRJIT_EXIT_REFUSED;
      return r;
    }
  }
  std::vector<uint64_t> regs(grjit_function_vreg_count(f), 0);
  std::vector<unsigned> width(regs.size(), 64);
  std::vector<bool> is_float(regs.size(), false);
  for (size_t v = 0; v < regs.size(); v++) {
    const GRJIT_Type t = grjit_function_vreg_type(f, static_cast<GRJIT_VReg>(v));
    width[v] = t == GRJIT_TYPE_I32 || t == GRJIT_TYPE_F32 ? 32 : 64;
    is_float[v] = t == GRJIT_TYPE_F32 || t == GRJIT_TYPE_F64;
  }
  for (size_t i = 0; i < grjit_function_param_count(f); i++) {
    regs[i] = cut(width[i], i < args.size() ? args[i] : 0);
  }
  GRJIT_BlockId block = 0;
  size_t steps = 0;
  for (;;) {
    size_t n = 0;
    const GRJIT_Op * ops = grjit_function_block_ops(f, block, &n);
    GRJIT_BlockId next = block;
    bool jumped = false;
    for (size_t i = 0; i < n && !jumped; i++) {
      const GRJIT_Op & op = ops[i];
      if (++steps > step_limit) {
        r.timed_out = true;
        return r;
      }
      uint64_t a = op.a.kind == GRJIT_OPERAND_NONE ? 0 : operand(regs, op.a);
      uint64_t b = op.b.kind == GRJIT_OPERAND_NONE ? 0 : operand(regs, op.b);
      const unsigned w = op.dst != GRJIT_NO_VREG ? width[op.dst] : 64;
      switch (op.kind) {
        case GRJIT_OP_CONST: regs[op.dst] = cut(w, a); break;
        case GRJIT_OP_MOVE: regs[op.dst] = cut(w, a); break;
        case GRJIT_OP_BITCAST: regs[op.dst] = a; break;
        case GRJIT_OP_ADD: regs[op.dst] = cut(w, a + b); break;
        case GRJIT_OP_SUB: regs[op.dst] = cut(w, a - b); break;
        case GRJIT_OP_MUL: regs[op.dst] = cut(w, a * b); break;
        case GRJIT_OP_AND: regs[op.dst] = cut(w, a & b); break;
        case GRJIT_OP_OR: regs[op.dst] = cut(w, a | b); break;
        case GRJIT_OP_XOR: regs[op.dst] = cut(w, a ^ b); break;
        case GRJIT_OP_SHL: regs[op.dst] = cut(w, cut(w, a) << (b % w)); break;
        case GRJIT_OP_SHR: regs[op.dst] = cut(w, cut(w, a) >> (b % w)); break;
        case GRJIT_OP_SAR:
          regs[op.dst] = cut(w, static_cast<uint64_t>(sext(w, a) >> (b % w)));
          break;
        case GRJIT_OP_NEG: regs[op.dst] = cut(w, 0 - a); break;
        case GRJIT_OP_NOT: regs[op.dst] = cut(w, ~a); break;
        case GRJIT_OP_CLZ: regs[op.dst] = count_leading_zeros(w, a); break;
        case GRJIT_OP_CTZ: regs[op.dst] = count_trailing_zeros(w, a); break;
        case GRJIT_OP_POPCNT: regs[op.dst] = population(w, a); break;
        case GRJIT_OP_ROTL: regs[op.dst] = rotate_left(w, a, b); break;
        case GRJIT_OP_ROTR: regs[op.dst] = rotate_left(w, a, w - (b % w)); break;
        case GRJIT_OP_EXT8_S: regs[op.dst] = sign_extend_from(8, w, a); break;
        case GRJIT_OP_EXT16_S: regs[op.dst] = sign_extend_from(16, w, a); break;
        case GRJIT_OP_EXT32_S: regs[op.dst] = sign_extend_from(32, 64, a); break;
        case GRJIT_OP_WRAP: regs[op.dst] = cut(32, a); break;
        case GRJIT_OP_EXTEND_S: regs[op.dst] = sign_extend_from(32, 64, a); break;
        case GRJIT_OP_EXTEND_U: regs[op.dst] = cut(32, a); break;
        case GRJIT_OP_DIV_S:
        case GRJIT_OP_DIV_U:
        case GRJIT_OP_REM_S:
        case GRJIT_OP_REM_U: {
          const uint64_t ua = cut(w, a), ub = cut(w, b);
          const int64_t sa = sext(w, a), sb = sext(w, b);
          const int64_t min = w == 32 ? INT32_MIN : INT64_MIN;
          if (ub == 0 || (op.kind == GRJIT_OP_DIV_S && sa == min && sb == -1)) {
            /* A partial operation reached with the operands its guards keep out: the
             * program is wrong, not the operation. Reported as an exit no run has. */
            r.exit = kPartialFault;
            return r;
          }
          uint64_t v;
          if (op.kind == GRJIT_OP_DIV_U) {
            v = ua / ub;
          } else if (op.kind == GRJIT_OP_REM_U) {
            v = ua % ub;
          } else if (op.kind == GRJIT_OP_DIV_S) {
            v = static_cast<uint64_t>(sa / sb);
          } else {
            v = sb == -1 ? 0 : static_cast<uint64_t>(sa % sb);
          }
          regs[op.dst] = cut(w, v);
          break;
        }
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
        case GRJIT_OP_REINTERPRET: {
          const unsigned sw = op.a.kind == GRJIT_OPERAND_VREG ? width[op.a.vreg] : w;
          bool partial = false;
          uint64_t v = 0;
          float_op(op, w, sw, a, b, is_float[op.dst], &v, &partial);
          if (partial) {
            r.exit = kPartialFault;
            return r;
          }
          regs[op.dst] = cut(w, v);
          break;
        }
        case GRJIT_OP_CMP: {
          const bool fcmp = (op.a.kind == GRJIT_OPERAND_VREG && is_float[op.a.vreg]) ||
                            (op.b.kind == GRJIT_OPERAND_VREG && is_float[op.b.vreg]);
          if (fcmp) {
            const unsigned cw = op.a.kind == GRJIT_OPERAND_VREG && is_float[op.a.vreg]
                                    ? width[op.a.vreg] : width[op.b.vreg];
            regs[op.dst] = float_compare(op.cmp, cw, cut(cw, a), cut(cw, b)) ? 1 : 0;
            break;
          }
          /* A comparison of 32-bit registers compares their low 32 bits; the signed ones
           * read them as signed. */
          const bool narrow = (op.a.kind == GRJIT_OPERAND_VREG && width[op.a.vreg] == 32) ||
                              (op.b.kind == GRJIT_OPERAND_VREG && width[op.b.vreg] == 32);
          if (narrow) {
            regs[op.dst] = compare(op.cmp, static_cast<uint64_t>(sext(32, a)),
                               static_cast<uint64_t>(sext(32, b)))
                               ? 1 : 0;
            if (op.cmp >= GRJIT_CMP_ULT) {
              regs[op.dst] = compare(op.cmp, cut(32, a), cut(32, b)) ? 1 : 0;
            }
          } else {
            regs[op.dst] = compare(op.cmp, a, b) ? 1 : 0;
          }
          break;
        }
        case GRJIT_OP_LOAD:
        case GRJIT_OP_LOAD_S: {
          uint64_t v = 0;
          std::memcpy(&v, reinterpret_cast<const unsigned char *>(a) + op.disp, op.width / 8);
          if (op.kind == GRJIT_OP_LOAD_S && op.width < 64 && ((v >> (op.width - 1)) & 1)) {
            v |= ~uint64_t{0} << op.width;
          }
          regs[op.dst] = v;
          break;
        }
        case GRJIT_OP_STORE:
          std::memcpy(reinterpret_cast<unsigned char *>(a) + op.disp, &b, op.width / 8);
          break;
        case GRJIT_OP_CALL: {
          uint64_t argv[6] = {0, 0, 0, 0, 0, 0};
          for (size_t k = 0; k < op.arg_count; k++) {
            argv[k] = operand(regs, op.args[k]);
          }
          uint64_t v = call_native(op.address, argv, op.arg_count);
          if (op.dst != GRJIT_NO_VREG) {
            regs[op.dst] = cut(width[op.dst], v);
          }
          break;
        }
        case GRJIT_OP_POLL: {
          uint64_t word;
          std::memcpy(&word,
              reinterpret_cast<const unsigned char *>(ctx) + grcore_jit_layout()->request_word_offset, 8);
          if (word != 0) {
            const GRJIT_FrameState * s = grjit_function_frame_state(f, op.state);
            uint32_t refused = grjit_function_poll_helper(f)(ctx, s->identity.function, s->identity.offset);
            if (refused != 0) {
              r.out.assign(slots + 1, 0xDEADBEEFu);
              r.out[0] = refused;
              r.exit = GRJIT_EXIT_REFUSED;
              return r;
            }
          }
          break;
        }
        case GRJIT_OP_GUARD:
          if (a == 0) {
            deopt_slots(f, op, regs, &r);
            return r;
          }
          break;
        case GRJIT_OP_BR: next = op.target; jumped = true; break;
        case GRJIT_OP_BR_IF: next = a != 0 ? op.target : op.target_else; jumped = true; break;
        case GRJIT_OP_RET:
          if (op.a.kind != GRJIT_OPERAND_NONE) {
            r.out[0] = a;
          }
          r.exit = GRJIT_EXIT_RETURNED;
          return r;
        default:
          return r;
      }
    }
    if (!jumped) {
      return r; // fell off a block: not a verified function
    }
    block = next;
  }
}

} // namespace ireval

#endif
