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
 * 32 bits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_A64_SIM_H
#define GHOTI_IO_GRJIT_TESTS_A64_SIM_H

#include <cstdint>
#include <cstdio>
#include <cstring>
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

struct Config {
  const uint8_t * code = nullptr;
  size_t size = 0;
  uint64_t args[3] = {0, 0, 0};          // x0, x1, x2 on entry
  std::set<uint64_t> returns32;          // host functions that return a uint32_t
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
};

inline uint64_t noise(uint64_t seed) {
  uint64_t z = seed * 0x9E3779B97F4A7C15ull + 0x1234567;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  return z ^ (z >> 31);
}

class Cpu {
 public:
  A64_SIM_NO_FUNCTION_CHECK Result run(Config & cfg) {
    Result res;
    std::vector<uint8_t> stack(cfg.stack_bytes + 64, 0);
    uintptr_t top = (reinterpret_cast<uintptr_t>(stack.data()) + cfg.stack_bytes) & ~uintptr_t{15};
    uintptr_t floor = reinterpret_cast<uintptr_t>(stack.data());
    for (int i = 0; i < 32; i++) {
      x_[i] = noise(static_cast<uint64_t>(i));
    }
    for (int i = 0; i < 3; i++) {
      x_[i] = cfg.args[i];
    }
    const uint64_t sentinel_lr = 0xDEAD0000DEAD0000ull;
    const uint64_t entry_fp = 0xF00D0000F00Dull;
    x_[29] = entry_fp;
    x_[30] = sentinel_lr;
    uint64_t saved[32];
    std::memcpy(saved, x_, sizeof saved);
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
      } else if ((w & 0xFFE0FC00u) == 0xAA000000u) { // orr xd, xn, xm
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) | reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0x2A000000u) { // orr wd, wn, wm
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, (reg(rn) | reg(rm)) & 0xFFFFFFFFu);
      } else if ((w & 0xFFE0FC00u) == 0xAA200000u) { // orn xd, xn, xm
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) | ~reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0x8A000000u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) & reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0xCA000000u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) ^ reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0x8B000000u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) + reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0xCB000000u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) - reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0xEB000000u) { // subs
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        uint64_t a = reg(rn), b = reg(rm), r = a - b;
        n_ = (r >> 63) != 0;
        z_ = r == 0;
        c_ = a >= b;
        v_ = (((a ^ b) & (a ^ r)) >> 63) != 0;
        set(rd, r);
      } else if ((w & 0xFFE0FC00u) == 0x9B007C00u) { // mul
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) * reg(rm));
      } else if ((w & 0xFFE0FC00u) == 0x9AC02000u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) << (reg(rm) & 63));
      } else if ((w & 0xFFE0FC00u) == 0x9AC02400u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, reg(rn) >> (reg(rm) & 63));
      } else if ((w & 0xFFE0FC00u) == 0x9AC02800u) {
        if (touches18(rd, rn, rm)) {
          return fail("x18 touched");
        }
        set(rd, static_cast<uint64_t>(static_cast<int64_t>(reg(rn)) >> (reg(rm) & 63)));
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
      } else if ((w & 0x3B000000u) == 0x39000000u || (w & 0x3B200C00u) == 0x38000000u ||
                 (w & 0x3B200C00u) == 0x38200800u) {
        unsigned size = (w >> 30) & 3, opc = (w >> 22) & 3;
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
        if (rn == 18 || rd == 18 || (rm == 18 && (w & 0x00200000u))) {
          return fail("x18 touched");
        }
        uint64_t address = regsp(rn) + offset;
        unsigned bytes = 1u << size;
        if (opc == 0) { // store
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
        uint64_t r = reinterpret_cast<Native>(target)(x_[0], x_[1], x_[2], x_[3], x_[4], x_[5]);
        if (cfg.returns32.count(target)) {
          r = (noise(cfg.call_count) << 32) | (r & 0xFFFFFFFFu);
        }
        uint64_t ret_to = pc_ + 4;
        for (int i = 1; i <= 17; i++) {
          x_[i] = noise(cfg.call_count * 64 + static_cast<uint64_t>(i));
        }
        x_[0] = r;
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
    res.ok = true;
    res.x0 = x_[0];
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
  uint64_t sp_ = 0;
  uint64_t pc_ = 0;
  bool n_ = false, z_ = false, c_ = false, v_ = false;
};

} // namespace a64sim

#endif
