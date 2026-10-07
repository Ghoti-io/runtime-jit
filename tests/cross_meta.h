/**
 * @file
 *
 * The metadata (and the code) of a function as each emitter makes it, on any host. `grjit_emit_for` emits for
 * a target without mapping or running anything, so a test that reads the stack maps of the call, tail-call
 * and native sites can read the arm64 emitter's on an x86-64 host and the x86-64 emitter's on an arm64 one:
 * the sites, their kinds, their identities and their maps are decided by code the two emitters have in
 * common and by code each has of its own, and a defect in either shows in the table of its own.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_CROSS_META_H
#define GHOTI_IO_GRJIT_TESTS_CROSS_META_H

#include "test_helpers.h"

#include "../src/code/code_internal.h"

#include <cstring>
#include <vector>

namespace xm {

/** A function emitted for `arch`. */
struct Emit {
  GRJIT_Emitted e{};
  GRJIT_Result result = GRJIT_OK;
  GRJIT_Arch arch;
  Emit(GRJIT_Arch a, const GRJIT_Function * f, const GRJIT_Limits * limits = nullptr) : arch(a) {
    result = grjit_emit_for(a, f, grjit_allocator_default(), limits, nullptr,
        grcore_jit_layout()->request_word_offset, &e);
  }
  Emit(const Emit &) = delete;
  Emit & operator=(const Emit &) = delete;
  ~Emit() { grjit_emitted_free(&e); }
  bool ok() const { return result == GRJIT_OK; }
  const GRCORE_CodeMeta * meta() const { return &e.meta.meta; }
  uint32_t word(size_t i) const {
    uint32_t w;
    std::memcpy(&w, e.bytes + 4 * i, 4);
    return w;
  }
  size_t words() const { return e.size / 4; }
};

inline const char * arch_name(GRJIT_Arch a) {
  return a == GRJIT_ARCH_ARM64 ? "arm64" : a == GRJIT_ARCH_X86_64 ? "x86-64" : "win64";
}

/** The amounts the arm64 code subtracts from `sp` into `x16`, which is how its native-stack check and its
 *  prologue's check form the lowest address they will use: `sub x16, sp, #imm` (and the second step,
 *  `sub x16, x16, #imm`, when the amount needs the shifted high part), or, from 2^24 bytes, `x17` built by
 *  movz/movk and `sub x16, sp, x17`. Read from the words, so that a lost flag or a dropped part shows as a
 *  different amount. */
inline std::vector<uint64_t> arm64_sub_x16_sp_amounts(const Emit & em) {
  std::vector<uint64_t> out;
  for (size_t i = 0; i < em.words(); i++) {
    const uint32_t w = em.word(i);
    if ((w & 0xFF8003FFu) == 0xD10003F0u) { // sub x16, sp, #imm {, lsl #12}
      const uint64_t imm = (w >> 10) & 0xFFFu;
      uint64_t total = (w & (1u << 22)) ? imm << 12 : imm;
      if (i + 1 < em.words()) {
        const uint32_t n = em.word(i + 1);
        if ((n & 0xFF8003FFu) == 0xD1000210u) { // sub x16, x16, #imm {, lsl #12}
          const uint64_t imm2 = (n >> 10) & 0xFFFu;
          total += (n & (1u << 22)) ? imm2 << 12 : imm2;
        }
      }
      out.push_back(total);
    } else if (w == 0xCB3163F0u) { // sub x16, sp, x17
      uint64_t value = 0;
      size_t j = i;
      std::vector<uint32_t> parts;
      while (j > 0) {
        const uint32_t p = em.word(j - 1);
        if ((p & 0xFF80001Fu) == 0xD2800011u || (p & 0xFF80001Fu) == 0xF2800011u) { // movz/movk x17
          parts.push_back(p);
          j--;
          if ((p & 0xFF80001Fu) == 0xD2800011u) {
            break;
          }
        } else {
          break;
        }
      }
      for (size_t k = parts.size(); k-- > 0;) {
        const uint32_t p = parts[k];
        const unsigned hw = (p >> 21) & 3;
        const uint64_t imm16 = (p >> 5) & 0xFFFF;
        value = ((p & 0xFF800000u) == 0xD2800000u) ? imm16 << (16 * hw)
                                                  : (value & ~(UINT64_C(0xFFFF) << (16 * hw))) | (imm16 << (16 * hw));
      }
      out.push_back(value);
    }
  }
  return out;
}

/** Runs `check(meta, name)` on the metadata of `f` as the x86-64 emitter and the arm64 emitter make it
 *  (whatever this host is), and, when the host runs one of them, on the code it compiles and maps. */
template <class Check>
void for_each_target_meta(const GRJIT_Function * f, const GRCORE_PageProvider * pages, Check check) {
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64}) {
    SCOPED_TRACE(arch_name(arch));
    Emit em(arch, f);
    ASSERT_TRUE(em.ok()) << grjit_result_string(em.result);
    check(em.meta(), arch_name(arch));
  }
  // Where the host's own backend has calls (Win64's has none until its own story): the code it maps.
  if (pages != nullptr && grjit_backend_calls_available()) {
    SCOPED_TRACE("compiled and mapped");
    Compiled c(f, pages);
    ASSERT_TRUE(c) << grjit_result_string(c.result);
    check(grjit_code_meta(c.code), "native");
  }
}

} // namespace xm

#endif // GHOTI_IO_GRJIT_TESTS_CROSS_META_H
