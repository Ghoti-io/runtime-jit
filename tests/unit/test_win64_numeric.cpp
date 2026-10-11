/**
 * @file
 *
 * The Win64 half of spec-runtime-float's story 5 that is specific to its ABI: what the Windows x86-64 flavour of
 * the x86-64 backend encodes for the numeric operations, on every host (emission runs everywhere, so a Linux
 * laptop checks the Win64 bytes it emits), and, under wine, the Microsoft x64 helper ABI against a probe in MinGW
 * assembly, a float result with junk above bit 31, and sentinels in the callee-saved registers.
 *
 * Win64's callee-saved registers are `rbx`, `rbp`, `rdi`, `rsi`, `r12`-`r15` and `xmm6`-`xmm15`; the flavour
 * encodes none of them (`rbp` is the frame register, saved and restored by the prologue and the epilogue).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../float_fixture.h"
#include "../int_fixture.h"

#include "../../src/code/code_internal.h"
#include "../../src/x86_64/asm_internal.h"

#include <string>

namespace {

constexpr uint32_t kRequestOffset = 0x40;

constexpr uint32_t kCalleeSavedGpr = (1u << GRJIT_RBX) | (1u << GRJIT_RSI) | (1u << GRJIT_RDI) |
    (1u << GRJIT_R12) | (1u << GRJIT_R13) | (1u << GRJIT_R14) | (1u << GRJIT_R15);
constexpr uint32_t kScratchGpr = (1u << GRJIT_RAX) | (1u << GRJIT_RCX) | (1u << GRJIT_RDX) |
    (1u << GRJIT_R8) | (1u << GRJIT_R9) | (1u << GRJIT_R10) | (1u << GRJIT_R11);

struct Emitted {
  GRJIT_Emitted e{};
  GRJIT_Result result;
  Emitted(const GRJIT_Function * f, GRJIT_Arch arch) {
    result = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, kRequestOffset, &e);
  }
  ~Emitted() { grjit_emitted_free(&e); }
  Emitted(const Emitted &) = delete;
  Emitted & operator=(const Emitted &) = delete;
};

} // namespace

TEST(Win64Numeric, NoCalleeSavedGeneralRegisterIsEncodedForAnyIntegerInstructionAtEitherWidth) {
  uint32_t seen = 0;
  size_t checked = 0;
  for (const char * ty : {"i32", "i64"}) {
    for (const std::string & instr : intfx::all_int_ops(ty)) {
      intfx::WasmOp op = intfx::build_wasm_op(instr);
      Fn f(op.function);
      Emitted w(f, GRJIT_ARCH_X86_64_WIN64);
      ASSERT_EQ(w.result, GRJIT_OK) << instr;
      EXPECT_EQ(w.e.regs_used & kCalleeSavedGpr, 0u) << instr << ": rbx, rsi, rdi or r12-r15 was encoded";
      EXPECT_EQ(w.e.regs_used & ~(kScratchGpr | (1u << GRJIT_RSP) | (1u << GRJIT_RBP)), 0u) << instr;
      seen |= w.e.regs_used;
      checked++;
    }
  }
  EXPECT_GT(checked, 60u);
  EXPECT_NE(seen & (1u << GRJIT_RAX), 0u);
  EXPECT_NE(seen & (1u << GRJIT_RCX), 0u);
  EXPECT_NE(seen & (1u << GRJIT_RDX), 0u) << "the divisions and the bit counts use rdx";
}

GRJIT_TEST_MAIN()
