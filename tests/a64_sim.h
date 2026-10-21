/**
 * @file
 *
 * A functional simulator for the AArch64 subset the arm64 backend emits, so
 * that what it emits can be *executed* on any host and compared with the
 * evaluator, long before (and independently of) a run under qemu-aarch64.
 *
 * It decodes exactly the instructions the assembler produces and stops with an
 * error on any other word, so a stray encoding cannot pass as a no-op. It also
 * checks what the AArch64 procedure call standard asks of compiled code and the
 * backend promises: the callee-saved registers, the frame pointer, the link
 * register and the stack pointer are what they were on entry when it returns,
 * `x18` is never read or written, the stack pointer is 16-byte aligned at every
 * call, and a native helper that returns 32 bits leaves the upper half of x0
 * undefined (the simulator fills it with noise), as a real one may. After a call
 * the caller-saved registers are noise too, so code that kept something in one
 * across a call fails.
 *
 * A call (`blr`) to an address outside the code is a call of a host function
 * with the first six registers as arguments; a test says which addresses return
 * 32 bits, or gives an address a *signature* (Config::sigs): which of its arguments are
 * integers, `float`s and `double`s and what it returns. The integer arguments then take
 * `x0`-`x5` and the float ones `d0`-`d7`, each in order among themselves, as AAPCS64
 * says; the call is made through the host's own C ABI (a SysV x86-64 host or an AAPCS64
 * one passes arguments by that same rule, so the host function sees what a real call
 * would give it), and a Windows host, whose rule differs, does not simulate a signature.
 *
 * **Floating point** (story 5 of spec-runtime-float). The scalar subset the backend emits:
 * `s` and `d` loads and stores, `fmov`, `fadd fsub fmul fdiv fmin fmax fsqrt fabs fneg`,
 * `frintn frintp frintm frintz`, `fcmp`, `fcvt`, `scvtf ucvtf fcvtzs fcvtzu`, and `cnt`/`addv`
 * for the population count. The Arm rules are written out and not left to the host's:
 * a NaN operand is returned quietened (a signalling NaN first, then the first operand's),
 * any other NaN is the positive default NaN, `fmin`/`fmax` order `-0 < +0` and propagate a
 * NaN, the conversions to an integer saturate and give 0 for a NaN, and `fabs`/`fneg` touch
 * the sign bit only. Only the arithmetic itself is the host's (IEEE on a machine whose
 * rounding mode is the default, built with `-ffp-contract=off`). The low 64 bits of
 * `d8`-`d15` are callee-saved and checked at the return; every other vector register is
 * filled with noise after a call. A file of its own tests the simulator
 * (tests/unit/test_arm64_numeric.cpp): it is written by the same author as the emitter and
 * is a dev loop, not the verdict; qemu-aarch64 and real arm64 hardware are.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_A64_SIM_H
#define GHOTI_IO_GRJIT_TESTS_A64_SIM_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace a64sim {

using Native = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// The simulator calls a helper through this one six-argument type whatever the
// helper declares, the way an AArch64 `blr` does (an argument it does not read
// is harmlessly in a register). That is not a call the language defines, and
// clang's -fsanitize=function checks the type of every indirect call, so the
// simulator's run() opts out of that one check; GCC has no such check.
#if defined(__clang__)
#define A64_SIM_NO_FUNCTION_CHECK __attribute__((no_sanitize("function")))
#else
#define A64_SIM_NO_FUNCTION_CHECK
#endif

// The signature of a host function: one character per argument ('i' a 64-bit integer, 'f' a
// float, 'd' a double) and its result ('i' a 64-bit integer, 'w' a 32-bit one whose upper half the
// call leaves as noise, 'f' a float whose bits 32-63 of d0 are noise, 'd' a double, 'v' none).
struct Sig {
  std::string args;
  char result = 'i';
};

// Whether a signature is simulated on this host (the host's C ABI classifies arguments as
// AAPCS64 does).
#if defined(_WIN32)
constexpr bool kSignatures = false;
#else
constexpr bool kSignatures = true;
#endif

struct Config {
  const uint8_t * code = nullptr;
  size_t size = 0;
  uint64_t args[4] = {0, 0, 0, 0};       // x0..x3 on entry
  std::set<uint64_t> returns32;          // host functions that return a uint32_t
  std::map<uint64_t, Sig> sigs;          // host functions with float arguments or results
  uint64_t d_args[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // d0..d7 on entry (bits)
  size_t max_steps = 200000000;
  size_t stack_bytes = 4u << 20;
  size_t call_count = 0;                 // out: native calls made
  size_t steps = 0;                      // out
  size_t stack_used = 0;                 // out: deepest the stack pointer went
};

struct Result {
  bool ok = false;
  std::string error;
  uint64_t x0 = 0;
  uint64_t d0 = 0;       // the bits of d0 at the return
};

inline uint64_t noise(uint64_t seed) {
  uint64_t z = seed * 0x9E3779B97F4A7C15ull + 0x1234567;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  return z ^ (z >> 31);
}

// ---- IEEE 754 binary32 / binary64 as bits, with Arm's NaN rules -------------------------------

struct Fmt {
  bool dbl;
  uint64_t sign, exp, frac, quiet, mask;
  static Fmt of(bool d) {
    return d ? Fmt{true, 1ull << 63, 0x7FF0000000000000ull, 0x000FFFFFFFFFFFFFull, 1ull << 51, ~0ull}
             : Fmt{false, 1ull << 31, 0x7F800000ull, 0x007FFFFFull, 1ull << 22, 0xFFFFFFFFull};
  }
  bool is_nan(uint64_t b) const { return (b & exp) == exp && (b & frac) != 0; }
  bool is_snan(uint64_t b) const { return is_nan(b) && (b & quiet) == 0; }
  uint64_t default_nan() const { return exp | quiet; }
  uint64_t quieten(uint64_t b) const { return b | quiet; }
  uint64_t is_zero(uint64_t b) const { return (b & ~sign & mask) == 0; }
  double to_double(uint64_t b) const {
    if (dbl) {
      double d;
      std::memcpy(&d, &b, 8);
      return d;
    }
    float f;
    uint32_t w = static_cast<uint32_t>(b);
    std::memcpy(&f, &w, 4);
    return f;
  }
};

// ARM ARM FPProcessNaNs: a signalling NaN first (the first operand's before the second's), then a
// quiet one; the result is quietened. Returns false when neither operand is a NaN.
inline bool process_nans(const Fmt & f, uint64_t a, uint64_t b, uint64_t * out) {
  if (f.is_snan(a)) {
    *out = f.quieten(a);
  } else if (f.is_snan(b)) {
    *out = f.quieten(b);
  } else if (f.is_nan(a)) {
    *out = a;
  } else if (f.is_nan(b)) {
    *out = b;
  } else {
    return false;
  }
  return true;
}

enum class Bin { Add, Sub, Mul, Div, Min, Max };

inline uint64_t fp_binary(Bin op, bool dbl, uint64_t a, uint64_t b) {
  const Fmt f = Fmt::of(dbl);
  uint64_t r;
  if (process_nans(f, a, b, &r)) {
    return r;
  }
  if (op == Bin::Min || op == Bin::Max) {
    const double x = f.to_double(a), y = f.to_double(b);
    if (x == y) {
      if (f.is_zero(a)) { // two zeros: -0 < +0
        const bool neg = op == Bin::Min ? ((a | b) & f.sign) != 0 : ((a & b) & f.sign) != 0;
        return neg ? f.sign : 0;
      }
      return a;
    }
    const bool take_a = op == Bin::Min ? x < y : x > y;
    return take_a ? a : b;
  }
  volatile double x = f.to_double(a), y = f.to_double(b);
  volatile float fx = static_cast<float>(x), fy = static_cast<float>(y);
  uint64_t out;
  if (dbl) {
    volatile double z;
    switch (op) {
      case Bin::Add: z = x + y; break;
      case Bin::Sub: z = x - y; break;
      case Bin::Mul: z = x * y; break;
      default: z = x / y; break;
    }
    double zz = z;
    std::memcpy(&out, &zz, 8);
  } else {
    volatile float z;
    switch (op) {
      case Bin::Add: z = fx + fy; break;
      case Bin::Sub: z = fx - fy; break;
      case Bin::Mul: z = fx * fy; break;
      default: z = fx / fy; break;
    }
    float zz = z;
    uint32_t w;
    std::memcpy(&w, &zz, 4);
    out = w;
  }
  return f.is_nan(out) ? f.default_nan() : out;
}

enum class Un { Sqrt, Abs, Neg, RintN, RintP, RintM, RintZ };

inline uint64_t fp_unary(Un op, bool dbl, uint64_t a) {
  const Fmt f = Fmt::of(dbl);
  if (op == Un::Abs) {
    return a & ~f.sign & f.mask;
  }
  if (op == Un::Neg) {
    return (a ^ f.sign) & f.mask;
  }
  if (f.is_nan(a)) {
    return f.quieten(a);
  }
  uint64_t out;
  if (dbl) {
    double x = f.to_double(a), z;
    switch (op) {
      case Un::Sqrt: z = std::sqrt(x); break;
      case Un::RintN: z = std::nearbyint(x); break;
      case Un::RintP: z = std::ceil(x); break;
      case Un::RintM: z = std::floor(x); break;
      default: z = std::trunc(x); break;
    }
    std::memcpy(&out, &z, 8);
  } else {
    float x = static_cast<float>(f.to_double(a)), z;
    switch (op) {
      case Un::Sqrt: z = std::sqrt(x); break;
      case Un::RintN: z = std::nearbyint(x); break;
      case Un::RintP: z = std::ceil(x); break;
      case Un::RintM: z = std::floor(x); break;
      default: z = std::trunc(x); break;
    }
    uint32_t w;
    std::memcpy(&w, &z, 4);
    out = w;
  }
  return f.is_nan(out) ? f.default_nan() : out;
}

// fcvtzs / fcvtzu: toward zero, saturating, a NaN is 0.
inline uint64_t fp_to_int(bool is_signed, bool dbl, bool wide, uint64_t a) {
  const Fmt f = Fmt::of(dbl);
  if (f.is_nan(a)) {
    return 0;
  }
  const double t = std::trunc(f.to_double(a));
  if (is_signed) {
    if (wide) {
      if (t >= 9223372036854775808.0) return static_cast<uint64_t>(INT64_MAX);
      if (t < -9223372036854775808.0) return static_cast<uint64_t>(INT64_MIN);
      return static_cast<uint64_t>(static_cast<int64_t>(t));
    }
    if (t >= 2147483648.0) return static_cast<uint32_t>(INT32_MAX);
    if (t < -2147483648.0) return static_cast<uint32_t>(INT32_MIN);
    return static_cast<uint32_t>(static_cast<int32_t>(t));
  }
  if (t <= 0.0) return 0;
  if (wide) {
    if (t >= 18446744073709551616.0) return UINT64_MAX;
    return static_cast<uint64_t>(t);
  }
  if (t >= 4294967296.0) return UINT32_MAX;
  return static_cast<uint32_t>(t);
}

// scvtf / ucvtf: one rounding, to nearest even.
inline uint64_t int_to_fp(bool is_signed, bool dbl, bool wide, uint64_t a) {
  uint64_t out;
  if (dbl) {
    volatile double z;
    if (is_signed) {
      z = wide ? static_cast<double>(static_cast<int64_t>(a)) : static_cast<double>(static_cast<int32_t>(a));
    } else {
      z = wide ? static_cast<double>(a) : static_cast<double>(static_cast<uint32_t>(a));
    }
    double zz = z;
    std::memcpy(&out, &zz, 8);
  } else {
    volatile float z;
    if (is_signed) {
      z = wide ? static_cast<float>(static_cast<int64_t>(a)) : static_cast<float>(static_cast<int32_t>(a));
    } else {
      z = wide ? static_cast<float>(a) : static_cast<float>(static_cast<uint32_t>(a));
    }
    float zz = z;
    uint32_t w;
    std::memcpy(&w, &zz, 4);
    out = w;
  }
  return out;
}

// fcvt: binary64 to binary32 and back. A NaN keeps its sign and the top of its payload and is
// quietened; anything else is the host's conversion (round to nearest even).
inline uint64_t fp_convert(bool to_double, uint64_t a) {
  if (to_double) {
    const Fmt s = Fmt::of(false), d = Fmt::of(true);
    uint32_t w = static_cast<uint32_t>(a);
    if (s.is_nan(w)) {
      return d.exp | d.quiet | ((w & s.sign) ? d.sign : 0) | (static_cast<uint64_t>(w & s.frac) << 29);
    }
    float x;
    std::memcpy(&x, &w, 4);
    double z = x;
    uint64_t out;
    std::memcpy(&out, &z, 8);
    return out;
  }
  const Fmt s = Fmt::of(false), d = Fmt::of(true);
  if (d.is_nan(a)) {
    return s.exp | s.quiet | ((a & d.sign) ? s.sign : 0) | ((a & d.frac) >> 29);
  }
  volatile double x;
  std::memcpy(const_cast<double *>(&x), &a, 8);
  volatile float z = static_cast<float>(x);
  float zz = z;
  uint32_t w;
  std::memcpy(&w, &zz, 4);
  return w;
}

class Cpu {
 public:
  A64_SIM_NO_FUNCTION_CHECK Result run(Config & cfg) {
    Result res;
    // Filled with a pattern, not zeros: a slot the code reads before it wrote it (a bug) would then show, and a slot
    // written with fewer bits than its word has (an f32 stored as 32) keeps garbage above them, as it does on hardware.
    std::vector<uint8_t> stack(cfg.stack_bytes + 64, 0xA5);
    uintptr_t top = (reinterpret_cast<uintptr_t>(stack.data()) + cfg.stack_bytes) & ~uintptr_t{15};
    uintptr_t floor = reinterpret_cast<uintptr_t>(stack.data());
    for (int i = 0; i < 32; i++) {
      x_[i] = noise(static_cast<uint64_t>(i));
      d_[i] = noise(static_cast<uint64_t>(100 + i));
    }
    for (int i = 0; i < 4; i++) {
      x_[i] = cfg.args[i];
    }
    for (int i = 0; i < 8; i++) {
      d_[i] = cfg.d_args[i];
    }
    const uint64_t sentinel_lr = 0xDEAD0000DEAD0000ull;
    const uint64_t entry_fp = 0xF00D0000F00Dull;
    x_[29] = entry_fp;
    x_[30] = sentinel_lr;
    uint64_t saved[32];
    uint64_t saved_d[32];
    std::memcpy(saved, x_, sizeof saved);
    std::memcpy(saved_d, d_, sizeof saved_d);
    sp_ = top;
    pc_ = 0;
    n_ = z_ = c_ = v_ = false;
    uintptr_t code_base = reinterpret_cast<uintptr_t>(cfg.code);
    uintptr_t lowest_sp = top;
    auto fail = [&](const std::string & what) {
      char at[64];
      std::snprintf(at, sizeof at, " (pc offset %llu)", static_cast<unsigned long long>(pc_));
      res.error = what + at;
      return res;
    };
    for (cfg.steps = 0; cfg.steps < cfg.max_steps; cfg.steps++) {
      if (pc_ == sentinel_lr) {
        break;
      }
      if (pc_ % 4 != 0 || pc_ + 4 > cfg.size) {
        return fail("the program counter left the code");
      }
      uint32_t w;
      std::memcpy(&w, cfg.code + pc_, 4);
      uint64_t next = pc_ + 4;
      if (sp_ < lowest_sp) {
        lowest_sp = sp_;
      }
      if (sp_ < floor + 256) {
        return fail("the stack overflowed");
      }
      // ---- decode ------------------------------------------------------------
      unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
      auto reg = [&](unsigned r) -> uint64_t { return r == 31 ? 0 : x_[r]; };
      auto regsp = [&](unsigned r) -> uint64_t { return r == 31 ? sp_ : x_[r]; };
      auto set = [&](unsigned r, uint64_t v) {
        if (r != 31) {
          x_[r] = v;
        }
      };
      auto setsp = [&](unsigned r, uint64_t v) {
        if (r == 31) {
          sp_ = v;
        } else {
          x_[r] = v;
        }
      };
      auto touches18 = [&](unsigned a, unsigned b, unsigned c) { return a == 18 || b == 18 || c == 18; };
      const bool sf = (w >> 31) != 0;
      const uint64_t wmask = sf ? ~uint64_t{0} : 0xFFFFFFFFull;
      const unsigned width = sf ? 64 : 32;
      // A scalar floating-point operation's format, from the `type` field: single, double, or neither.
      const unsigned ftype = (w >> 22) & 3;
      const bool fp_known = ftype == 0 || ftype == 1;
      const bool fdbl = ftype == 1;
      const uint64_t fmask = fdbl ? ~uint64_t{0} : 0xFFFFFFFFull;
      if ((w & 0xFF800000u) == 0xD2800000u || (w & 0xFF800000u) == 0x92800000u ||
          (w & 0xFF800000u) == 0xF2800000u) {
        unsigned hw = (w >> 21) & 3;
        uint64_t imm = uint64_t{(w >> 5) & 0xFFFFu} << (16 * hw);
        if (rd == 18) {
          return fail("x18 written");
        }
        if ((w & 0xFF800000u) == 0xD2800000u) {
          set(rd, imm);
        } else if ((w & 0xFF800000u) == 0x92800000u) {
          set(rd, ~imm);
        } else {
          set(rd, (reg(rd) & ~(uint64_t{0xFFFF} << (16 * hw))) | imm);
        }
      } else if ((w & 0x7FE0FC00u) == 0x2A000000u || (w & 0x7FE0FC00u) == 0x0A000000u ||
                 (w & 0x7FE0FC00u) == 0x4A000000u || (w & 0x7FE0FC00u) == 0x0B000000u ||
                 (w & 0x7FE0FC00u) == 0x4B000000u || (w & 0x7FE0FC00u) == 0x2A200000u ||
                 (w & 0x7FE0FC00u) == 0x0A200000u || (w & 0x7FE0FC00u) == 0x6B000000u) {
        // orr, and, eor, add, sub, orn, bic, subs; shifted register, no shift, either width
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        const uint64_t a = reg(rn) & wmask, b = reg(rm) & wmask;
        uint64_t r;
        switch (w & 0x7FE0FC00u) {
          case 0x2A000000u: r = a | b; break;
          case 0x0A000000u: r = a & b; break;
          case 0x4A000000u: r = a ^ b; break;
          case 0x0B000000u: r = a + b; break;
          case 0x4B000000u: r = a - b; break;
          case 0x2A200000u: r = a | ~b; break;
          case 0x0A200000u: r = a & ~b; break;
          default: { // subs
            r = (a - b) & wmask;
            n_ = ((r >> (width - 1)) & 1) != 0;
            z_ = r == 0;
            c_ = a >= b;
            v_ = ((((a ^ b) & (a ^ r)) >> (width - 1)) & 1) != 0;
            break;
          }
        }
        set(rd, r & wmask);
      } else if ((w & 0x7FE0FC00u) == 0x1B007C00u) { // mul (madd with xzr)
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, (reg(rn) * reg(rm)) & wmask);
      } else if ((w & 0x7FE08000u) == 0x1B008000u) { // msub rd, rn, rm, ra
        const unsigned ra = (w >> 10) & 31;
        if (touches18(rd, rn, rm) || ra == 18) {
          return fail("x18 touched");
        }
        set(rd, (reg(ra) - reg(rn) * reg(rm)) & wmask);
      } else if ((w & 0x7FE0FC00u) == 0x1AC02000u || (w & 0x7FE0FC00u) == 0x1AC02400u ||
                 (w & 0x7FE0FC00u) == 0x1AC02800u || (w & 0x7FE0FC00u) == 0x1AC02C00u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        const uint64_t a = reg(rn) & wmask;
        const unsigned cnt = static_cast<unsigned>(reg(rm) & (width - 1));
        uint64_t r;
        switch (w & 0x7FE0FC00u) {
          case 0x1AC02000u: r = a << cnt; break;
          case 0x1AC02400u: r = a >> cnt; break;
          case 0x1AC02800u:
            r = sf ? static_cast<uint64_t>(static_cast<int64_t>(a) >> cnt)
                   : static_cast<uint64_t>(static_cast<int32_t>(static_cast<uint32_t>(a)) >> cnt);
            break;
          default: r = cnt == 0 ? a : ((a >> cnt) | (a << (width - cnt))); break;
        }
        set(rd, r & wmask);
      } else if ((w & 0x7FE0FC00u) == 0x1AC00C00u || (w & 0x7FE0FC00u) == 0x1AC00800u) { // sdiv, udiv
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        const uint64_t a = reg(rn) & wmask, b = reg(rm) & wmask;
        uint64_t r;
        if (b == 0) {
          r = 0; // no trap: the quotient of a division by zero is zero
        } else if ((w & 0x400u) == 0) { // udiv
          r = a / b;
        } else if (sf) { // sdiv: MIN / -1 is MIN
          const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
          r = (sa == INT64_MIN && sb == -1) ? static_cast<uint64_t>(INT64_MIN) : static_cast<uint64_t>(sa / sb);
        } else {
          const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
          r = (sa == INT32_MIN && sb == -1) ? static_cast<uint32_t>(INT32_MIN) : static_cast<uint32_t>(sa / sb);
        }
        set(rd, r & wmask);
      } else if ((w & 0x7FFFFC00u) == 0x5AC01000u || (w & 0x7FFFFC00u) == 0x5AC00000u) { // clz, rbit
        if (touches18(rd, rn, 0)) {
          return fail("x18 touched");
        }
        const uint64_t a = reg(rn) & wmask;
        uint64_t r = 0;
        if ((w & 0x1000u) != 0) { // clz
          r = width;
          for (unsigned i = 0; i < width; i++) {
            if ((a >> (width - 1 - i)) & 1) {
              r = i;
              break;
            }
          }
        } else {
          for (unsigned i = 0; i < width; i++) {
            r |= ((a >> i) & 1) << (width - 1 - i);
          }
        }
        set(rd, r);
      } else if ((w & 0x7F800000u) == 0x13000000u && ((w >> 22) & 1) == (sf ? 1u : 0u) &&
                 ((w >> 16) & 63) == 0 && (((w >> 10) & 63) == 7 || ((w >> 10) & 63) == 15 ||
                                           ((w >> 10) & 63) == 31)) { // sbfm #0, #7|15|31: sxtb, sxth, sxtw
        if (touches18(rd, rn, 0)) {
          return fail("x18 touched");
        }
        const unsigned bits = ((w >> 10) & 63) + 1;
        if (bits == 32 && !sf) {
          return fail("an instruction the backend does not emit: sxtw to a w register");
        }
        const uint64_t a = reg(rn) & ((uint64_t{1} << bits) - 1);
        const uint64_t sext = static_cast<uint64_t>(static_cast<int64_t>(a << (64 - bits)) >> (64 - bits));
        set(rd, sext & wmask);
      } else if ((w & 0xFFE00C00u) == 0x1A800400u) { // csinc wd
        unsigned cond = (w >> 12) & 15;
        uint64_t v = cond_holds(cond) ? (reg(rn) & 0xFFFFFFFFu) : ((reg(rm) + 1) & 0xFFFFFFFFu);
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, v);
      } else if ((w & 0xFF800000u) == 0x91000000u || (w & 0xFF800000u) == 0xD1000000u) {
        uint64_t imm = (w >> 10) & 0xFFFu;
        if (w & (1u << 22)) {
          imm <<= 12;
        }
        if (rd == 18 || rn == 18) {
          return fail("x18 touched");
        }
        uint64_t a = regsp(rn);
        setsp(rd, (w & 0x40000000u) ? a - imm : a + imm);
      } else if ((w & 0xFFE0FC00u) == 0x8B206000u || (w & 0xFFE0FC00u) == 0xCB206000u) {
        // add/sub (extended register, uxtx): the form that can name sp
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        uint64_t a = regsp(rn), b = reg(rm);
        setsp(rd, (w & 0x40000000u) ? a - b : a + b);
      } else if ((w & 0xFFC0001Fu) == 0xF100001Fu) { // cmp xn, #imm12
        const uint64_t a = reg(rn), b = (w >> 10) & 0xFFFu, r = a - b;
        n_ = (r >> 63) != 0;
        z_ = r == 0;
        c_ = a >= b;
        v_ = (((a ^ b) & (a ^ r)) >> 63) != 0;
      } else if ((w & 0x3B000000u) == 0x39000000u || (w & 0x3B200C00u) == 0x38000000u ||
                 (w & 0x3B200C00u) == 0x38200800u) {
        unsigned size = (w >> 30) & 3, opc = (w >> 22) & 3;
        const bool vec = (w & 0x04000000u) != 0; // the V bit: a vector/FP register
        uint64_t offset;
        if ((w & 0x3B000000u) == 0x39000000u) {
          offset = uint64_t{(w >> 10) & 0xFFFu} << size;
        } else if ((w & 0x3B200C00u) == 0x38000000u) {
          offset = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>((w >> 12) << 23) >> 23));
        } else {
          if (((w >> 13) & 7) != 3 || ((w >> 12) & 1) != 0) {
            return fail("a register-offset form that is not UXTX with no shift");
          }
          offset = reg(rm);
        }
        if (rn == 18 || (!vec && rd == 18) || (rm == 18 && (w & 0x00200000u))) {
          return fail("x18 touched");
        }
        uint64_t address = regsp(rn) + offset;
        unsigned bytes = 1u << size;
        if (vec) {
          // s and d only: size 2 and 3, and a plain load or store (opc 1 / 0)
          if ((size != 2 && size != 3) || opc > 1) {
            return fail("an instruction the backend does not emit: a vector load or store that is not s or d");
          }
          if (opc == 0) {
            std::memcpy(reinterpret_cast<void *>(address), &d_[rd], bytes);
          } else {
            uint64_t v = 0;
            std::memcpy(&v, reinterpret_cast<const void *>(address), bytes);
            d_[rd] = v;
          }
        } else if (opc == 0) { // store
          uint64_t v = reg(rd);
          std::memcpy(reinterpret_cast<void *>(address), &v, bytes);
        } else {
          uint64_t v = 0;
          std::memcpy(&v, reinterpret_cast<const void *>(address), bytes);
          if (opc == 2) { // sign-extend to 64 bits
            unsigned bits = 8u * bytes;
            v = static_cast<uint64_t>(static_cast<int64_t>(v << (64 - bits)) >> (64 - bits));
          }
          set(rd, v);
        }
      } else if (w == 0xA9BF7BFDu) { // stp x29, x30, [sp, #-16]!
        sp_ -= 16;
        std::memcpy(reinterpret_cast<void *>(sp_), &x_[29], 8);
        std::memcpy(reinterpret_cast<void *>(sp_ + 8), &x_[30], 8);
      } else if (w == 0xA8C17BFDu) { // ldp x29, x30, [sp], #16
        std::memcpy(&x_[29], reinterpret_cast<const void *>(sp_), 8);
        std::memcpy(&x_[30], reinterpret_cast<const void *>(sp_ + 8), 8);
        sp_ += 16;
      } else if (w == 0xD65F03C0u) { // ret
        next = x_[30];
      } else if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) { // blr xn
        uint64_t target = x_[rn];
        if (rn == 18) {
          return fail("x18 touched");
        }
        if (sp_ % 16 != 0) {
          return fail("sp is not 16-byte aligned at a call");
        }
        if (target >= code_base && target < code_base + cfg.size) {
          return fail("a call into the code itself is not simulated");
        }
        cfg.call_count++;
        uint64_t r;
        Sig sig;
        bool typed = false;
        auto found = cfg.sigs.find(target);
        if (found != cfg.sigs.end()) {
          if (!kSignatures) {
            return fail("a helper with a signature is not simulated on this host");
          }
          typed = true;
          sig = found->second;
        }
        uint64_t fp_result = 0;
        if (!typed) {
          r = reinterpret_cast<Native>(target)(x_[0], x_[1], x_[2], x_[3], x_[4], x_[5]);
          if (cfg.returns32.count(target)) {
            r = (noise(cfg.call_count) << 32) | (r & 0xFFFFFFFFu);
          }
        } else {
          uint64_t ia[6] = {0, 0, 0, 0, 0, 0};
          double fa[8] = {0, 0, 0, 0, 0, 0, 0, 0};
          unsigned ni = 0, nf = 0;
          for (char c : sig.args) {
            if (c == 'i') {
              if (ni >= 6) {
                return fail("a helper with more than six integer arguments is not simulated");
              }
              ia[ni] = x_[ni];
              ni++;
            } else {
              if (nf >= 8) {
                return fail("a helper with more than eight float arguments is not simulated");
              }
              std::memcpy(&fa[nf], &d_[nf], 8); // the whole register's bits; the callee reads s or d
              nf++;
            }
          }
          if (sig.result == 'f' || sig.result == 'd') {
            using FpNative = double (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                double, double, double, double, double, double, double, double);
            double dr = reinterpret_cast<FpNative>(target)(ia[0], ia[1], ia[2], ia[3], ia[4], ia[5],
                fa[0], fa[1], fa[2], fa[3], fa[4], fa[5], fa[6], fa[7]);
            std::memcpy(&fp_result, &dr, 8);
            r = x_[0];
          } else {
            using IntNative = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                double, double, double, double, double, double, double, double);
            r = reinterpret_cast<IntNative>(target)(ia[0], ia[1], ia[2], ia[3], ia[4], ia[5],
                fa[0], fa[1], fa[2], fa[3], fa[4], fa[5], fa[6], fa[7]);
            if (sig.result == 'w') {
              r = (noise(cfg.call_count) << 32) | (r & 0xFFFFFFFFu);
            }
          }
        }
        uint64_t ret_to = pc_ + 4;
        for (int i = 1; i <= 17; i++) {
          x_[i] = noise(cfg.call_count * 64 + static_cast<uint64_t>(i));
        }
        // Every caller-saved vector register is noise after a call; d8-d15 are the callee's to keep.
        for (int i = 0; i < 32; i++) {
          if (i < 8 || i >= 16) {
            d_[i] = noise(cfg.call_count * 64 + 32 + static_cast<uint64_t>(i));
          }
        }
        x_[0] = r;
        if (typed && sig.result == 'f') {
          // A float comes back in the low 32 bits of d0; whatever is above them is not the caller's.
          d_[0] = (noise(cfg.call_count + 7777) << 32) | (fp_result & 0xFFFFFFFFu);
        } else if (typed && sig.result == 'd') {
          d_[0] = fp_result;
        }
        x_[30] = ret_to;
        // The call returned: the return address was ours to give.
      } else if ((w & 0xFC000000u) == 0x14000000u) { // b
        next = pc_ + static_cast<int64_t>(static_cast<int32_t>(w << 6) >> 6) * 4;
      } else if ((w & 0xFF000010u) == 0x54000000u) { // b.cond
        int64_t imm = static_cast<int32_t>((w >> 5) << 13) >> 13;
        if (cond_holds(w & 15)) {
          next = pc_ + static_cast<uint64_t>(imm * 4);
        }
      } else if ((w & 0xFF000000u) == 0xB4000000u || (w & 0xFF000000u) == 0xB5000000u) {
        int64_t imm = static_cast<int32_t>((w >> 5) << 13) >> 13;
        bool taken = (w & 0x01000000u) ? reg(rd) != 0 : reg(rd) == 0;
        if (taken) {
          next = pc_ + static_cast<uint64_t>(imm * 4);
        }
      } else if ((w & 0xFFE0001Fu) == 0xD4200000u) {
        return fail("brk: the code ran into a trap");
      } else if ((w & 0xFFFFFC00u) == 0x1E270000u || (w & 0xFFFFFC00u) == 0x9E670000u ||
                 (w & 0xFFFFFC00u) == 0x1E260000u || (w & 0xFFFFFC00u) == 0x9E660000u) {
        // fmov between a general register and s (w) or d (x), either way
        if ((w & 0x00010000u) != 0 ? rn == 18 : rd == 18) {
          return fail("x18 touched");
        }
        if ((w & 0x00010000u) != 0) { // opcode 111: to the vector register
          d_[rd] = sf ? reg(rn) : (reg(rn) & 0xFFFFFFFFull);
        } else {
          set(rd, sf ? d_[rn] : (d_[rn] & 0xFFFFFFFFull));
        }
      } else if ((w & 0xFF3FFC00u) == 0x1E204000u && fp_known) { // fmov vd, vn
        d_[rd] = d_[rn] & fmask;
      } else if ((w & 0xFF20FC00u) == 0x1E200800u || (w & 0xFF20FC00u) == 0x1E201800u ||
                 (w & 0xFF20FC00u) == 0x1E202800u || (w & 0xFF20FC00u) == 0x1E203800u ||
                 (w & 0xFF20FC00u) == 0x1E204800u || (w & 0xFF20FC00u) == 0x1E205800u) {
        if (!fp_known) {
          return fail("an instruction the backend does not emit: a half-precision operation");
        }
        static const Bin ops[6] = {Bin::Mul, Bin::Div, Bin::Add, Bin::Sub, Bin::Max, Bin::Min};
        d_[rd] = fp_binary(ops[(w >> 12) & 7], fdbl, d_[rn] & fmask, d_[rm] & fmask);
      } else if (fp_known && ((w & 0xFF3FFC00u) == 0x1E20C000u || (w & 0xFF3FFC00u) == 0x1E214000u ||
                              (w & 0xFF3FFC00u) == 0x1E21C000u || (w & 0xFF3FFC00u) == 0x1E244000u ||
                              (w & 0xFF3FFC00u) == 0x1E24C000u || (w & 0xFF3FFC00u) == 0x1E254000u ||
                              (w & 0xFF3FFC00u) == 0x1E25C000u)) {
        // fabs, fneg, fsqrt, frintn, frintp, frintm, frintz (not frinta/frintx/frinti)
        Un op;
        switch ((w >> 15) & 0x3F) {
          case 1: op = Un::Abs; break;
          case 2: op = Un::Neg; break;
          case 3: op = Un::Sqrt; break;
          case 8: op = Un::RintN; break;
          case 9: op = Un::RintP; break;
          case 10: op = Un::RintM; break;
          default: op = Un::RintZ; break;
        }
        d_[rd] = fp_unary(op, fdbl, d_[rn] & fmask);
      } else if (w == (0x1E624000u | (rn << 5) | rd)) { // fcvt sd, dn
        d_[rd] = fp_convert(false, d_[rn]);
      } else if (w == (0x1E22C000u | (rn << 5) | rd)) { // fcvt dd, sn
        d_[rd] = fp_convert(true, d_[rn] & 0xFFFFFFFFull);
      } else if ((w & 0xFF20FC1Fu) == 0x1E202000u && fp_known) { // fcmp vn, vm
        const Fmt f = Fmt::of(fdbl);
        const uint64_t a = d_[rn] & fmask, b = d_[rm] & fmask;
        if (f.is_nan(a) || f.is_nan(b)) {
          n_ = false; z_ = false; c_ = true; v_ = true;
        } else {
          const double x = f.to_double(a), y = f.to_double(b);
          if (x == y) { n_ = false; z_ = true; c_ = true; v_ = false; }
          else if (x < y) { n_ = true; z_ = false; c_ = false; v_ = false; }
          else { n_ = false; z_ = false; c_ = true; v_ = false; }
        }
      } else if (fp_known && ((w & 0x7F3FFC00u) == 0x1E220000u || (w & 0x7F3FFC00u) == 0x1E230000u)) {
        // scvtf / ucvtf vd, rn
        if (rn == 18) {
          return fail("x18 touched");
        }
        d_[rd] = int_to_fp((w & 0x00010000u) == 0, fdbl, sf, reg(rn));
      } else if (fp_known && ((w & 0x7F3FFC00u) == 0x1E380000u || (w & 0x7F3FFC00u) == 0x1E390000u)) {
        // fcvtzs / fcvtzu rd, vn
        if (rd == 18) {
          return fail("x18 written");
        }
        set(rd, fp_to_int((w & 0x00010000u) == 0, fdbl, sf, d_[rn] & fmask) & wmask);
      } else if ((w & 0xFFFFFC00u) == 0x0E205800u) { // cnt vd.8b, vn.8b
        uint64_t r = 0;
        for (int i = 0; i < 8; i++) {
          r |= static_cast<uint64_t>(__builtin_popcountll((d_[rn] >> (8 * i)) & 0xFFu)) << (8 * i);
        }
        d_[rd] = r;
      } else if ((w & 0xFFFFFC00u) == 0x0E31B800u) { // addv bd, vn.8b
        uint64_t sum = 0;
        for (int i = 0; i < 8; i++) {
          sum += (d_[rn] >> (8 * i)) & 0xFFu;
        }
        d_[rd] = sum & 0xFFu;
      } else {
        char what[64];
        std::snprintf(what, sizeof what, "an instruction the backend does not emit: 0x%08x", w);
        return fail(what);
      }
      pc_ = next;
    }
    if (pc_ != sentinel_lr) {
      return fail("the step limit was reached");
    }
    cfg.stack_used = top - lowest_sp;
    if (sp_ != top) {
      return fail("sp is not where it was on entry");
    }
    for (int i : {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29}) {
      if (x_[i] != saved[i]) {
        char what[64];
        std::snprintf(what, sizeof what, "callee-saved x%d changed", i);
        return fail(what);
      }
    }
    if (x_[18] != saved[18]) {
      return fail("x18 changed");
    }
    // The low 64 bits of v8-v15 are callee-saved; the upper halves are not, and are not modelled.
    for (int i = 8; i <= 15; i++) {
      if (d_[i] != saved_d[i]) {
        char what[64];
        std::snprintf(what, sizeof what, "callee-saved d%d changed", i);
        return fail(what);
      }
    }
    res.ok = true;
    res.x0 = x_[0];
    res.d0 = d_[0];
    return res;
  }

 private:
  bool cond_holds(unsigned cond) const {
    bool r;
    switch (cond >> 1) {
      case 0: r = z_; break;
      case 1: r = c_; break;
      case 2: r = n_; break;
      case 3: r = v_; break;
      case 4: r = c_ && !z_; break;
      case 5: r = n_ == v_; break;
      case 6: r = n_ == v_ && !z_; break;
      default: r = true; break;
    }
    return (cond & 1) && cond != 15 ? !r : r;
  }

  uint64_t x_[32];
  uint64_t d_[32];
  uint64_t sp_ = 0;
  uint64_t pc_ = 0;
  bool n_ = false, z_ = false, c_ = false, v_ = false;
};

} // namespace a64sim

#endif
