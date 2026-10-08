/**
 * @file
 *
 * The Win64 convention for calls between compiled functions, tail calls and natives (AD-28), and the
 * unwind information that goes with it (story 7b of the calls spec).
 *
 * Two kinds of test. The first reads the code the emitter produces for Win64 and needs no Windows: it runs
 * on every host, the way `testArm64_calls` reads arm64 code, and holds the numbers of the convention
 * independently of the library's own constants (four register arguments, no shadow space between compiled
 * functions, 32 bytes of it for a native, the pair of a native with a status behind a hidden pointer, the
 * callee popping its stack arguments), the registers the code names, and the shape of the unwind table (two
 * entries, the adapter's with no frame register). The second runs the code and the Windows unwinder on a
 * Windows target (under wine here, which is wine's `ntdll` and not Windows'): a walk from inside a native
 * up through a chain and its adapter, the unwinder started at every instruction of both prologues and
 * epilogues, a frame of three pages, and the table gone after the code is destroyed.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define FX_ASM_SENTINELS
#include "../calls_asm.h"
#include "../callable_gen.h"
#include "../calls_fixture.h"

#include "../../src/backend/backend_internal.h"
#include "../../src/code/code_internal.h"
#include "../../src/ir/ir_internal.h"
#include "../../src/x86_64/asm_internal.h"

#include <cstring>
#include <set>
#include <string>

#if defined(_WIN64) && defined(__x86_64__)
#define GRJIT_TEST_WIN64 1
#endif

using namespace fx;

namespace {

constexpr uint32_t kRequestOffset = 0x40;
using Bytes = std::vector<uint8_t>;

/* The registers a Win64 callee must preserve, which no code of this backend may name beside `rbp`. */
constexpr uint32_t kCalleeSaved = (1u << GRJIT_RBX) | (1u << GRJIT_RSI) | (1u << GRJIT_RDI) |
    (1u << GRJIT_R12) | (1u << GRJIT_R13) | (1u << GRJIT_R14) | (1u << GRJIT_R15);
constexpr uint32_t kScratch = (1u << GRJIT_RAX) | (1u << GRJIT_RCX) | (1u << GRJIT_RDX) |
    (1u << GRJIT_R8) | (1u << GRJIT_R9) | (1u << GRJIT_R10) | (1u << GRJIT_R11);

struct Emit {
  GRJIT_Emitted e{};
  GRJIT_Result result = GRJIT_OK;
  explicit Emit(const GRJIT_Function * f, GRJIT_Arch arch = GRJIT_ARCH_X86_64_WIN64,
      GRJIT_EntryHook hook = nullptr, uint32_t request = kRequestOffset) {
    result = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, hook, request, &e);
  }
  Emit(const Emit &) = delete;
  Emit & operator=(const Emit &) = delete;
  ~Emit() { grjit_emitted_free(&e); }
  bool ok() const { return result == GRJIT_OK; }
  /* The bytes of the body: from the internal entry on. */
  const uint8_t * body() const { return e.bytes + e.internal_offset; }
  size_t body_size() const { return e.size - e.internal_offset; }
  size_t count(const Bytes & want, size_t from, size_t to) const {
    size_t n = 0;
    for (size_t i = from; i + want.size() <= to; i++) {
      if (std::memcmp(e.bytes + i, want.data(), want.size()) == 0) {
        n++;
      }
    }
    return n;
  }
};

uint32_t round16(size_t n) { return static_cast<uint32_t>((n + 15) / 16 * 16); }

/* The stack-argument bytes of the internal convention for `n` arguments, from the Win64 convention
 * as the story states it and not from the library's constants: four in registers. */
uint32_t stack_args(size_t n) { return n > 4 ? round16(8 * (n - 4)) : 0; }

GRJIT_CallHooks hooks() {
  GRJIT_CallHooks h{};
  h.push = reinterpret_cast<decltype(h.push)>(0x30000);
  h.pop = reinterpret_cast<decltype(h.pop)>(0x30100);
  h.compile = reinterpret_cast<decltype(h.compile)>(0x30200);
  h.deopt = reinterpret_cast<decltype(h.deopt)>(0x30300);
  h.tail = reinterpret_cast<decltype(h.tail)>(0x30400);
  return h;
}

/* A function of `params` I64 parameters, with enough registers that its frame needs a 32-bit
 * displacement and allocation (so `sub rsp, imm8` in its body is an argument area and nothing else),
 * that returns its first parameter (or zero). */
GRJIT_Function * plain_callable(unsigned params) {
  B b("w", 4);
  b.callable(hooks());
  EXPECT_EQ(grjit_builder_set_token(b.b, 4242), GRJIT_OK);
  std::vector<GRJIT_VReg> p;
  for (unsigned i = 0; i < params; i++) {
    p.push_back(b.param(GRJIT_TYPE_I64));
  }
  for (int i = 0; i < 40; i++) {
    (void)b.reg();
  }
  b.at(b.block());
  b.ret(params == 0 ? I(0) : V(p[0]));
  return b.finish();
}

/* A function of `params` parameters that calls a slot with `args` arguments (the first parameter or
 * an immediate). */
GRJIT_Function * calls_with(unsigned params, unsigned args, bool tail) {
  B b("w", 4);
  b.callable(hooks());
  EXPECT_EQ(grjit_builder_set_token(b.b, 4243), GRJIT_OK);
  std::vector<GRJIT_VReg> p;
  for (unsigned i = 0; i < params; i++) {
    p.push_back(b.param(GRJIT_TYPE_I64));
  }
  GRJIT_VReg r = b.reg();
  for (int i = 0; i < 40; i++) {
    (void)b.reg();
  }
  b.poll_helper(cg::kPollHelper);
  b.at(b.block());
  static uint64_t word = 0;
  std::vector<GRJIT_Operand> a;
  for (unsigned i = 0; i < args; i++) {
    a.push_back(i < params ? V(p[i]) : I(static_cast<int64_t>(i)));
  }
  std::vector<GRJIT_FrameSlot> slots;
  for (unsigned i = 0; i < params; i++) {
    slots.push_back(grjit_frame_slot_vreg(p[i]));
  }
  if (tail) {
    b.tail_call_slot(&word, 1, a, GRCORE_PollIdentity{1, 2}, slots);
  } else {
    std::vector<GRJIT_FrameSlot> after = slots;
    b.call_slot(r, &word, 1, a, GRCORE_PollIdentity{1, 2}, slots, GRCORE_PollIdentity{1, 3}, after);
    b.ret(V(r));
  }
  return b.finish();
}

} // namespace

/* ---- The convention, read from the code ----------------------------------------------------------- */

TEST(Win64Calls, NoCalleeSavedRegisterIsEncodedInAnyGeneratedCallableFunction) {
  uint32_t seen = 0;
  unsigned functions = 0;
  auto visit = [&](const GRJIT_Function * f, unsigned id) {
    Emit em(f);
    EXPECT_TRUE(em.ok()) << id;
    EXPECT_EQ(em.e.regs_used & kCalleeSaved, 0u)
        << "function " << id << ": a callee-saved register (rbx, rsi, rdi, r12-r15) was encoded";
    EXPECT_EQ(em.e.regs_used & ~(kScratch | (1u << GRJIT_RSP) | (1u << GRJIT_RBP)), 0u) << id;
    seen |= em.e.regs_used;
    functions++;
    return true;
  };
  cg::each_call_function(visit);
  cg::each_tail_function(visit);
  cg::each_native_function(visit);
  EXPECT_EQ(functions, 336u + 168u + 512u) << "every function of the three families";
  EXPECT_EQ(seen & kScratch, kScratch) << "the scan is not vacuous: every scratch register is used by some";
}

TEST(Win64Calls, TheSysVCodeOfTheSameFunctionsDoesNameCalleeSavedRegistersSoTheScanCanFail) {
  uint32_t seen = 0;
  cg::each_call_function([&](const GRJIT_Function * f, unsigned) {
    Emit em(f, GRJIT_ARCH_X86_64);
    seen |= em.e.regs_used;
    return true;
  });
  EXPECT_NE(seen & (1u << GRJIT_RSI), 0u);
  EXPECT_NE(seen & (1u << GRJIT_RDI), 0u);
}

TEST(Win64Calls, ACalleeWithMoreThanFourParametersPopsTheRestWithRetAndAnImmediateOfWholeSixteens) {
  /* `lea rsp,[rbp]; pop rbp; ret imm16` (or plain `ret` for none) is how every compiled function ends; with the
   * four register arguments of the Win64 internal convention a function of n parameters pops
   * round16(8 (n - 4)) bytes, and none for four or fewer. */
  for (unsigned n = 0; n <= 16; n++) {
    SCOPED_TRACE(n);
    Fn f(plain_callable(n));
    Emit em(f);
    ASSERT_TRUE(em.ok());
    const uint32_t in_a = stack_args(n);
    Bytes tail = {0x48, 0x8D, 0x65, 0x00, 0x5D};
    Bytes want = tail;
    if (in_a == 0) {
      want.push_back(0xC3);
    } else {
      want.push_back(0xC2);
      want.push_back(static_cast<uint8_t>(in_a));
      want.push_back(static_cast<uint8_t>(in_a >> 8));
    }
    const size_t from = em.e.internal_offset, to = em.e.size;
    const size_t returns = em.count(want, from, to);
    EXPECT_EQ(returns, em.count(tail, from, to)) << "every epilogue of the body pops exactly " << in_a;
    EXPECT_GE(returns, 3u) << "the return, the deopt and the failure";
    /* The parameters: the first four are stored from rcx, rdx, r8, r9 and the rest loaded from
     * [rbp + 16 + 8 i]. */
    /* The first four parameters are stored from rcx, rdx, r8 and r9, each to its own slot. */
    const uint8_t stores[4][3] = {{0x48, 0x89, 0x4D}, {0x48, 0x89, 0x55}, {0x4C, 0x89, 0x45}, {0x4C, 0x89, 0x4D}};
    for (unsigned i = 0; i < 4; i++) {
      const Bytes store = {stores[i][0], stores[i][1], stores[i][2], static_cast<uint8_t>(-8 * (static_cast<int>(i) + 4))};
      EXPECT_EQ(em.count(store, from, from + 96), i < n ? 1u : 0u) << "register argument " << i;
    }
    size_t loads = 0;
    for (unsigned k = 0; k + 4 < n; k++) {
      const Bytes load = {0x48, 0x8B, 0x45, static_cast<uint8_t>(16 + 8 * k)}; // mov rax, [rbp + 16 + 8 k]
      loads += em.count(load, from, from + 256);
    }
    EXPECT_EQ(loads, n > 4 ? n - 4 : 0u) << "a stack parameter is read from above the return address, with no shadow space";
  }
}

TEST(Win64Calls, TheAdaptersFrameHoldsTheStackArgumentsAndTheShadowSpaceInOneAllocation) {
  for (unsigned n : {0u, 1u, 4u, 5u, 6u, 9u, 16u}) {
    SCOPED_TRACE(n);
    Fn f(plain_callable(n));
    Emit em(f);
    ASSERT_TRUE(em.ok());
    const uint32_t in_a = stack_args(n);
    const uint32_t w = in_a > 32 ? in_a : 32;
    EXPECT_EQ(em.e.prologue.adapter_alloc_bytes, w + 32) << "N' = max(32, in_A) rounded, plus the three saved words and one spare";
    EXPECT_EQ(em.e.prologue.adapter_push_end, 1u) << "push rbp";
    EXPECT_GT(em.e.prologue.adapter_alloc_end, em.e.prologue.adapter_push_end);
    EXPECT_LE(em.e.prologue.adapter_end, em.e.internal_offset - 16u) << "the tag and its padding are in neither function";
    EXPECT_EQ(em.e.prologue.body_begin, em.e.internal_offset);
    EXPECT_EQ(em.e.internal_offset % 16, 0u);
    /* The body's own frame is the base frame and the 48-byte outgoing area. */
    EXPECT_EQ(em.e.prologue.alloc_bytes % 16, 0u);
    EXPECT_EQ(em.e.prologue.push_end, em.e.internal_offset + 1);
    EXPECT_EQ(em.e.prologue.setfp_end, em.e.internal_offset + 4);
  }
}

TEST(Win64Calls, ACallPushesRoundedStackArgumentsJustBeforeTheCallAndTheCalleePopsThem) {
  for (unsigned args = 0; args <= 16; args++) {
    SCOPED_TRACE(args);
    Fn f(calls_with(2, args, false));
    Emit em(f);
    ASSERT_TRUE(em.ok());
    const uint32_t area = stack_args(args);
    /* The body's allocation is a 32-bit one (a frame of forty-odd registers); a `sub rsp, imm8`
     * in the body is an argument area, and there is no `add rsp` that pops it (the callee does). */
    const size_t from = em.e.internal_offset, to = em.e.size;
    std::vector<size_t> subs;
    for (size_t i = from; i + 4 <= to; i++) {
      if (em.e.bytes[i] == 0x48 && em.e.bytes[i + 1] == 0x83 && em.e.bytes[i + 2] == 0xEC) {
        subs.push_back(i);
      }
      if (em.e.bytes[i] == 0x48 && em.e.bytes[i + 1] == 0x83 && em.e.bytes[i + 2] == 0xC4) {
        ADD_FAILURE() << "the caller pops nothing: the callee's `ret imm16` does";
      }
    }
    /* The last `sub rsp, 32` is the overflow stub's room for the hook's shadow space, emitted after
     * everything else; the others are argument areas. */
    ASSERT_FALSE(subs.empty());
    ASSERT_EQ(em.e.bytes[subs.back() + 3], 32u);
    subs.pop_back();
    EXPECT_EQ(subs.size(), area != 0 ? 1u : 0u);
    for (size_t at : subs) {
      EXPECT_EQ(em.e.bytes[at + 3], area) << "the area is the rounded stack arguments";
    }
    /* The area is whole sixteens: rsp is aligned at the call. */
    EXPECT_EQ(area % 16, 0u);
  }
}

TEST(Win64Calls, ATailCallMovesTheReturnAddressByTheDifferenceOfTheTwoStackAreas) {
  /* `lea rsp, [rbp + ra']; mov rbp, rax; jmp r11` ends the replacement, with `ra' = 8 + in_A - in_T`.
   * Read for every pair of caller parameters and callee arguments from 0 to 16: both sides of the
   * four-register boundary, the callee wider, narrower and equal. */
  for (unsigned params = 0; params <= 16; params++) {
    for (unsigned args = 0; args <= 16; args++) {
      SCOPED_TRACE(testing::Message() << params << " parameters, " << args << " arguments");
      Fn f(calls_with(params, args, true));
      Emit em(f);
      ASSERT_TRUE(em.ok());
      const int64_t want = 8 + int64_t{stack_args(params)} - int64_t{stack_args(args)};
      const Bytes end = {0x48, 0x89, 0xC5, 0x41, 0xFF, 0xE3}; // mov rbp, rax ; jmp r11
      size_t found = 0;
      for (size_t i = em.e.internal_offset; i + end.size() <= em.e.size; i++) {
        if (std::memcmp(em.e.bytes + i, end.data(), end.size()) != 0) {
          continue;
        }
        found++;
        int64_t disp;
        if (i >= 4 && em.e.bytes[i - 4] == 0x48 && em.e.bytes[i - 3] == 0x8D && em.e.bytes[i - 2] == 0x65) {
          disp = static_cast<int8_t>(em.e.bytes[i - 1]);
        } else {
          ASSERT_GE(i, 7u);
          ASSERT_EQ(em.e.bytes[i - 7], 0x48);
          ASSERT_EQ(em.e.bytes[i - 6], 0x8D);
          ASSERT_EQ(em.e.bytes[i - 5], 0xA5);
          int32_t d32;
          std::memcpy(&d32, em.e.bytes + i - 4, 4);
          disp = d32;
        }
        EXPECT_EQ(disp, want) << "the callee's return address, and so its stack arguments' end, are where its ret imm16 needs them";
      }
      EXPECT_EQ(found, 1u) << "one frame replacement";
      /* The copy goes through rcx and never rdi: a tail call to a callee with stack arguments
       * loads each into rcx and stores it into the old frame. */
      if (args > 4) {
        EXPECT_EQ(em.e.regs_used & (1u << GRJIT_RDI), 0u);
      }
    }
  }
}

TEST(Win64Calls, ANativeCallMakesTheShadowSpaceTheStackWordsAndTheBufferInOneRoundedArea) {
  /* The area is `round16(32 + 8 k + 16 for a status)` with `k` the C words past the fourth
   * (the context first, and for a status the hidden pointer before it). Read from the `sub rsp` in front of
   * the call for every arity from 0 to 16, with and without a status. */
  unsigned seen = 0;
  cg::each_native_function([&](const GRJIT_Function * f, unsigned id) {
    Emit em(f);
    EXPECT_TRUE(em.ok());
    /* The natives of the family: each function calls one with `arity` parameters. Find the
     * area from the sequence: sub rsp (imm8 or imm32) and read it back against the descriptor. */
    size_t arity = 0;
    bool status = false;
    for (size_t b = 0; b < f->block_count && arity == 0 && !status; b++) {
      for (size_t i = 0; i < f->blocks[b].count; i++) {
        const GRJIT_Op & op = f->blocks[b].ops[i];
        if (op.kind == GRJIT_OP_CALL_NATIVE) {
          const GRJIT_NativeDesc * d = grjit_native_table_get(f->natives, op.native);
          arity = op.arg_count;
          status = (d->flags & GRJIT_NATIVE_STATUS) != 0;
          break;
        }
      }
    }
    const size_t words = arity + 1 + (status ? 1 : 0);
    const size_t stack_words = words > 4 ? words - 4 : 0;
    const uint32_t area = round16(32 + 8 * stack_words + (status ? 16 : 0));
    size_t matches = 0;
    for (size_t i = em.e.internal_offset; i + 7 <= em.e.size; i++) {
      if (em.e.bytes[i] == 0x48 && em.e.bytes[i + 1] == 0x83 && em.e.bytes[i + 2] == 0xEC &&
          em.e.bytes[i + 3] == area && area <= 127) {
        matches++;
      }
      if (em.e.bytes[i] == 0x48 && em.e.bytes[i + 1] == 0x81 && em.e.bytes[i + 2] == 0xEC) {
        uint32_t v;
        std::memcpy(&v, em.e.bytes + i + 3, 4);
        if (v == area) {
          matches++;
        }
      }
    }
    EXPECT_GE(matches, 1u) << "function " << id << ": " << arity << " arguments"
                           << (status ? ", with a status" : "") << ": the area is " << area;
    seen++;
    return true;
  });
  EXPECT_EQ(seen, 512u);
}

/* ---- The unwind table, on any host ---------------------------------------------------------------- */

namespace {

struct FakeTable {
  std::vector<std::pair<const void *, uint32_t>> adds;
  std::vector<const void *> removes;
};
FakeTable * g_fake = nullptr;
bool fake_add(void * table, uint32_t count, uintptr_t base) {
  (void)base;
  g_fake->adds.emplace_back(table, count);
  return true;
}
void fake_remove(void * table) { g_fake->removes.push_back(table); }

struct OpsScope {
  const GRJIT_UnwindOps * previous;
  OpsScope(FakeTable * t) {
    g_fake = t;
    static const GRJIT_UnwindOps ops = {fake_add, fake_remove};
    previous = grjit_unwind_set_ops(&ops);
  }
  ~OpsScope() {
    grjit_unwind_set_ops(previous);
    g_fake = nullptr;
  }
};

uint32_t rd32(const uint8_t * p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

} // namespace

TEST(Win64CallsUnwind, ACallableFunctionHasTwoSortedEntriesRegisteredTogetherAndRemovedTogether) {
  for (unsigned n : {0u, 5u, 9u}) {
    SCOPED_TRACE(n);
    Fn f(plain_callable(n));
    Emit em(f);
    ASSERT_TRUE(em.ok());
    JitWorld w;
    FakeTable t;
    OpsScope scope(&t);
    void * mapping = nullptr;
    size_t mapped = 0;
    void * table = nullptr;
    ASSERT_EQ(grjit_memory_create(w.pages(), GRJIT_ARCH_X86_64_WIN64, em.e.bytes, em.e.size, &em.e.prologue,
                  &mapping, &mapped, &table),
        GRJIT_OK);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(t.adds.size(), 1u) << "one registration";
    EXPECT_EQ(t.adds[0].first, table);
    EXPECT_EQ(t.adds[0].second, 2u) << "of two entries";
    const uint8_t * rf = static_cast<const uint8_t *>(table);
    /* Sorted by begin address: the adapter's, then the body's. */
    EXPECT_EQ(rd32(rf), 0u);
    EXPECT_EQ(rd32(rf + 4), em.e.prologue.adapter_end);
    EXPECT_EQ(rd32(rf + 12), em.e.internal_offset);
    EXPECT_EQ(rd32(rf + 16), em.e.size);
    EXPECT_LT(rd32(rf), rd32(rf + 12));
    EXPECT_LE(rd32(rf + 4), rd32(rf + 12)) << "the entries do not overlap";
    /* The adapter's information: version 1, a prologue of its two instructions, no frame register. */
    const uint8_t * ai = static_cast<const uint8_t *>(mapping) + rd32(rf + 8);
    EXPECT_EQ(ai[0], 1u);
    EXPECT_EQ(ai[1], em.e.prologue.adapter_alloc_end) << "SizeOfProlog";
    EXPECT_EQ(ai[3], 0u) << "no frame register: the adapter sets rbp to a marker that is no base";
    /* The body's: the frame register is rbp (5) at offset zero. */
    const uint8_t * bi = static_cast<const uint8_t *>(mapping) + rd32(rf + 20);
    EXPECT_EQ(bi[0], 1u);
    EXPECT_EQ(bi[1], em.e.prologue.alloc_end - em.e.internal_offset);
    EXPECT_EQ(bi[3], 5u) << "rbp is the frame register of a body";
    grjit_memory_destroy(w.pages(), mapping, mapped, table);
    ASSERT_EQ(t.removes.size(), 1u);
    EXPECT_EQ(t.removes[0], table) << "the one table, both entries";
  }
}

TEST(Win64CallsUnwind, ThePlainFunctionStillHasOneEntryForAllItsCode) {
  B b("plain", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.ret(V(x));
  Fn f(b.finish());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  JitWorld w;
  FakeTable t;
  OpsScope scope(&t);
  void * mapping = nullptr;
  size_t mapped = 0;
  void * table = nullptr;
  ASSERT_EQ(grjit_memory_create(w.pages(), GRJIT_ARCH_X86_64_WIN64, em.e.bytes, em.e.size, &em.e.prologue,
                &mapping, &mapped, &table),
      GRJIT_OK);
  ASSERT_EQ(t.adds.size(), 1u);
  EXPECT_EQ(t.adds[0].second, 1u);
  const uint8_t * rf = static_cast<const uint8_t *>(table);
  EXPECT_EQ(rd32(rf), 0u);
  EXPECT_EQ(rd32(rf + 4), em.e.size);
  grjit_memory_destroy(w.pages(), mapping, mapped, table);
}

TEST(Win64CallsUnwind, TheAdapterInfoCountsItsAllocationAndItsPushAndNothingElse) {
  GRJIT_Prologue p{};
  p.adapter_push_end = 1;
  p.adapter_alloc_end = 8;
  p.adapter_alloc_bytes = 64;
  uint8_t out[GRJIT_UNWIND_INFO_MAX];
  ASSERT_EQ(grjit_unwind_info_build_adapter(&p, out), 8u);
  EXPECT_EQ(out[0], 1u);
  EXPECT_EQ(out[1], 8u);
  EXPECT_EQ(out[2], 2u) << "an allocation and a push";
  EXPECT_EQ(out[3], 0u) << "and no frame register";
  EXPECT_EQ(out[4], 8u);
  EXPECT_EQ(out[5], (2u | ((64u - 8u) / 8u << 4))) << "UWOP_ALLOC_SMALL of 64";
  EXPECT_EQ(out[6], 1u);
  EXPECT_EQ(out[7], (0u | (5u << 4))) << "UWOP_PUSH_NONVOL rbp";
  p.adapter_alloc_bytes = 4096;
  ASSERT_GT(grjit_unwind_info_build_adapter(&p, out), 0u);
  EXPECT_EQ(out[3], 0u);
  GRJIT_Prologue none{};
  EXPECT_EQ(grjit_unwind_info_build_adapter(&none, out), 0u) << "no push, nothing to describe";
}

#ifdef GRJIT_TEST_WIN64

/* A native that spills its four register arguments to their home space, the 32 bytes the caller of a Win64 function
 * reserves above its return address, and then adds the three words it was given on the stack, which sit above that
 * space. A call that reserved no shadow space puts those words where the spill lands. Written in assembly because
 * C++ cannot say which bytes it spills to. */
extern "C" uint64_t grjit_test_spill_native(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
asm(R"(
  .text
  .globl grjit_test_spill_native
grjit_test_spill_native:
  mov %rcx, 8(%rsp)
  mov %rdx, 16(%rsp)
  mov %r8, 24(%rsp)
  mov %r9, 32(%rsp)
  mov 40(%rsp), %rax
  add 48(%rsp), %rax
  add 56(%rsp), %rax
  ret
)");

/* A poll helper that scribbles over its own shadow space, [rsp + 8, rsp + 40) at its entry, which is the 32 bytes
 * the frame of a caller reserves for it; and returns zero, "carry on". A frame without that area has live slots
 * there. */
extern "C" uint32_t grjit_test_scribbling_poll(void *, uint64_t, uint64_t);
asm(R"(
  .text
  .globl grjit_test_scribbling_poll
grjit_test_scribbling_poll:
  movabs $0x5AD05AD05AD05AD0, %r10
  mov %r10, 8(%rsp)
  mov %r10, 16(%rsp)
  mov %r10, 24(%rsp)
  mov %r10, 32(%rsp)
  xor %eax, %eax
  ret
)");

TEST(Win64CallsRun, ANativeThatSpillsItsRegisterArgumentsToItsHomeSpaceLeavesItsStackWordsAndTheCallersSlotsAlone) {
  Engine e;
  const int spill = e.add_native(reinterpret_cast<const void *>(grjit_test_spill_native),
      std::vector<GRJIT_Type>(6, GRJIT_TYPE_I64), GRJIT_TYPE_I64, 0, 64, "spill");
  const int f = e.reserve();
  {
    P p("f", {GRJIT_TYPE_I64});
    int a = p.local(), b = p.local(), c = p.local(), d = p.local(), g = p.local(), h = p.local(), r = p.local();
    p.cnst(a, 1);
    p.cnst(b, 2);
    p.cnst(c, 3);
    p.cnst(d, 4000);
    p.cnst(g, 500);
    p.cnst(h, 60);
    p.native(r, spill, {a, b, c, d, g, h});
    // A live value across the call, in a slot a spill into the frame would reach.
    int s = p.local();
    p.bin(K::ADD, s, r, 0);
    p.ret(s);
    e.set(f, p.done());
  }
  Outcome o = e.run_compiled(f, {7});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(o.value, 4000u + 500u + 60u + 7u) << "the three stack words, summed above the shadow space, and the live parameter";
}

TEST(Win64CallsRun, AHookThatScribblesOnItsShadowSpaceFindsTheFramesOutgoingAreaAndNotItsSlots) {
  JitWorld w;
  B b("scribble", 8);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.poll_helper(reinterpret_cast<GRJIT_PollHelper>(grjit_test_scribbling_poll));
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg v[7];
  for (auto & r : v) {
    r = b.reg();
  }
  GRJIT_VReg sum = b.reg();
  b.at(b.block());
  for (int i = 0; i < 7; i++) {
    b.bin(GRJIT_OP_ADD, v[i], V(x), I(i + 1));
  }
  std::vector<GRJIT_FrameSlot> slots = {grjit_frame_slot_vreg(x)};
  for (auto & r : v) {
    slots.push_back(grjit_frame_slot_vreg(r));
  }
  b.poll(GRCORE_PollIdentity{1, 1}, slots);
  b.bin(GRJIT_OP_ADD, sum, V(v[0]), V(v[1]));
  for (int i = 2; i < 7; i++) {
    b.bin(GRJIT_OP_ADD, sum, V(sum), V(v[i]));
  }
  b.ret(V(sum));
  Fn f(b.finish());
  char why[256] = {0};
  ASSERT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << why;
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  uint64_t * request = reinterpret_cast<uint64_t *>(
      reinterpret_cast<unsigned char *>(w.ctx) + grcore_jit_layout()->request_word_offset);
  *request = 1; // the poll takes its slow path, and the helper runs
  auto r = c.run(w.ctx, {100});
  EXPECT_EQ(r.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(r.out[0], 7u * 100u + 28u) << "the seven slots live across the helper, none of them scribbled on";
  *request = 0;
}

#endif

/* ---- Running it: the Windows unwinder through a chain ---------------------------------------------- */

TEST(Win64Calls, ACallableFunctionWhoseFrameIsThreePagesRunsTwentyDeepIntoStackNoCallHasTouched) {
  /* On Win64 the prologue probes such a frame a page at a time, downwards, so the guard page is hit
   * in order and the stack grows to cover it; one access further down than the guard page is a stack
   * overflow. Twenty frames of three pages each go 240 KiB below the stack this test started on.
   * Elsewhere the kernel grows the stack and the result is the same. */
  Engine e;
  const int big = e.reserve();
  {
    P p("big", {GRJIT_TYPE_I64});
    int last = 0;
    for (int i = 0; i < 1450; i++) {
      last = p.local();
    }
    int zero = p.local(), t = p.local(), one = p.local(), n1 = p.local(), r = p.local(), forty = p.local();
    p.cnst(last, 7);
    p.cnst(zero, 0);
    p.bin(K::EQ, t, 0, zero);
    int br = p.brz(t);
    p.cnst(forty, 40);
    p.ret(forty);
    p.patch(br, p.here());
    p.cnst(one, 1);
    p.bin(K::SUB, n1, 0, one);
    p.call(r, big, {n1});
    p.bin(K::ADD, forty, r, last);
    p.ret(forty);
    e.set(big, p.done());
  }
  ASSERT_TRUE(e.compile_fn(big));
  const GRCORE_CodeMeta * meta = grjit_code_meta(e.code_of(big).code);
  EXPECT_GT(meta->frame_bytes + kOutgoingBytes, 2u * 4096u);
  EXPECT_LT(meta->frame_bytes + kOutgoingBytes, 4u * 4096u) << "three pages: more than two and less than four";
  Outcome out = e.run_compiled(big, {20});
  ASSERT_TRUE(out.finished);
  EXPECT_EQ(out.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_EQ(out.value, 40u + 20u * 7u);
}

#ifdef GRJIT_TEST_WIN64

namespace {

struct Step {
  uint64_t rip = 0, rsp = 0, rbp = 0;
  uint64_t begin = 0, end = 0;
  bool found = false;
};
std::vector<Step> g_steps;
std::vector<uintptr_t> g_bases;       // the compiled frames' bases, innermost first, by the walk
std::vector<uintptr_t> g_returns;     // and their return addresses
std::vector<uintptr_t> g_saved;       // and the caller's base each holds (the stack is gone after the run)
uintptr_t g_trampoline = 0;


/* The native the chain ends in: it records the unwinder's steps from its own frame up through every
 * compiled frame and the adapter to the first function the unwind information does not know (the
 * test's trampoline, which is assembly). */
__attribute__((noinline)) uint64_t nat_unwind_probe(void * ctx, uint64_t v) {
  g_steps.clear();
  g_bases.clear();
  g_returns.clear();
  g_saved.clear();
  GRCORE_CompiledWalk w;
  GRCORE_CompiledFrame f;
  if (grcore_compiled_walk_begin(static_cast<GRCORE_Context *>(ctx), &w) == GRCORE_OK) {
    while (grcore_compiled_walk_next(&w, &f) == GRCORE_CWALK_FRAME) {
      g_bases.push_back(f.frame_base);
      g_returns.push_back(*reinterpret_cast<const uintptr_t *>(f.frame_base + 8));
      g_saved.push_back(*reinterpret_cast<const uintptr_t *>(f.frame_base));
    }
  }
  CONTEXT c;
  RtlCaptureContext(&c);
  for (int i = 0; i < 64; i++) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    Step s;
    s.rip = c.Rip;
    s.rsp = c.Rsp;
    s.rbp = c.Rbp;
    s.found = rf != nullptr;
    if (rf != nullptr) {
      s.begin = image + rf->BeginAddress;
      s.end = image + rf->EndAddress;
    }
    g_steps.push_back(s);
    if (rf == nullptr) {
      break;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
  }
  return v;
}

/* f(n, 1, 2, 3, 4, 5): six parameters, so two stack arguments in every callee's frame: the chain
 * of n + 1 frames ends in the probe. */
int add_unwind_chain(Engine & e, int probe) {
  int f = e.reserve();
  P p("f", {GRJIT_TYPE_I64, GRJIT_TYPE_I64, GRJIT_TYPE_I64, GRJIT_TYPE_I64, GRJIT_TYPE_I64, GRJIT_TYPE_I64});
  int zero = p.local(), t = p.local(), one = p.local(), n1 = p.local(), r = p.local();
  p.cnst(zero, 0);
  p.bin(K::EQ, t, 0, zero);
  int br = p.brz(t);
  p.native(r, probe, {0});
  p.ret(r);
  p.patch(br, p.here());
  p.cnst(one, 1);
  p.bin(K::SUB, n1, 0, one);
  p.call(r, f, {n1, 1, 2, 3, 4, 5});
  p.ret(r);
  e.set(f, p.done());
  return f;
}

} // namespace

TEST(Win64CallsUnwind, RtlVirtualUnwindFromInsideANativeGoesUpThroughThreeFramesAndTheAdapterToTheCaller) {
  Engine e;
  const int probe = e.add_native(reinterpret_cast<const void *>(nat_unwind_probe), {GRJIT_TYPE_I64},
      GRJIT_TYPE_I64, 0, 4096, "probe");
  const int f = add_unwind_chain(e, probe);
  ASSERT_TRUE(e.compile_fn(f));
  const GRJIT_Code * code = e.code_of(f).code;
  const uintptr_t image = reinterpret_cast<uintptr_t>(grjit_code_address(code));
  const uintptr_t internal = grjit_code_internal_entry(code);
  const size_t size = grjit_code_size(code);
  g_trampoline = reinterpret_cast<uintptr_t>(&fx_call_with_sentinels);
  const uint32_t exit = fx::call_with_sentinels_and_check(e, f, {2, 1, 2, 3, 4, 5}, "the unwinding chain");
  ASSERT_EQ(exit, uint32_t{GRJIT_EXIT_RETURNED});
  ASSERT_EQ(g_bases.size(), 3u) << "three compiled frames below the native";
  ASSERT_GE(g_steps.size(), 6u);
  /* Step 0 is the probe itself. */
  EXPECT_TRUE(g_steps[0].found);
  /* Steps 1 to 3 are the compiled bodies, innermost first. */
  for (size_t k = 0; k < 3; k++) {
    SCOPED_TRACE(k);
    const Step & s = g_steps[1 + k];
    ASSERT_TRUE(s.found) << "a body is found by its return address";
    EXPECT_EQ(s.begin, internal) << "the entry is the body's: it begins at the internal entry";
    EXPECT_EQ(s.end, image + size);
    EXPECT_EQ(s.rbp, g_bases[k]) << "the unwound frame register is this frame's own base";
    EXPECT_EQ(s.rip, k == 0 ? s.rip : g_returns[k - 1]) << "the return address of the frame below";
  }
  /* After the outermost body: the adapter, with the marker in rbp. */
  const Step & adapter = g_steps[4];
  ASSERT_TRUE(adapter.found);
  EXPECT_EQ(adapter.begin, image) << "the adapter's entry begins at the first byte";
  EXPECT_LT(adapter.end, internal);
  EXPECT_EQ(adapter.rip, g_returns[2]) << "the outermost frame's return address";
  EXPECT_EQ(adapter.rbp, GRCORE_COMPILED_CHAIN_END) << "its saved base is the marker";
  EXPECT_EQ(adapter.rsp, g_bases[2] + 16) << "its stack pointer is the bottom of its frame, where the stack arguments are";
  /* And past it the test's trampoline, with the registers the adapter saved. */
  const Step & after = g_steps[5];
  EXPECT_EQ(after.rbp, sentinel_value(1)) << "the adapter's pop gives rbp back: the caller's, not the marker";
  EXPECT_GE(after.rip, g_trampoline);
  EXPECT_LT(after.rip, g_trampoline + 1024);
  EXPECT_EQ(after.rsp, adapter.rsp + 64 + 16) << "N' = 64 for two stack arguments: the allocation, the saved rbp and the return address";
  EXPECT_FALSE(after.found) << "the trampoline is assembly with no unwind information, where the walk stops";
  /* Each body frame's link and return address, as the unwinder recovered them. */
  for (size_t k = 0; k < 3; k++) {
    const uintptr_t saved = g_saved[k];
    EXPECT_EQ(g_steps[2 + k].rbp, saved) << "the caller's base, or for the last the marker";
    EXPECT_EQ(g_steps[2 + k].rsp, g_bases[k] + 16);
  }
}

TEST(Win64CallsUnwind, AfterDestroyTheLookupOfEitherEntryFindsNothing) {
  JitWorld w;
  Fn f(plain_callable(6));
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  GRJIT_Code * code = c.code;
  const uintptr_t image = reinterpret_cast<uintptr_t>(grjit_code_address(code));
  const uintptr_t internal = grjit_code_internal_entry(code);
  DWORD64 base = 0;
  PRUNTIME_FUNCTION adapter = RtlLookupFunctionEntry(image + 2, &base, nullptr);
  PRUNTIME_FUNCTION body = RtlLookupFunctionEntry(internal + 8, &base, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(body, nullptr);
  EXPECT_NE(adapter, body);
  EXPECT_EQ(base + adapter->BeginAddress, image);
  EXPECT_EQ(base + body->BeginAddress, internal);
  grjit_code_destroy(code);
  c.code = nullptr; // destroyed here, to look at what is left
  EXPECT_EQ(RtlLookupFunctionEntry(image + 2, &base, nullptr), nullptr) << "the adapter's entry is gone";
  EXPECT_EQ(RtlLookupFunctionEntry(internal + 8, &base, nullptr), nullptr) << "and the body's";
}


namespace {

/* The unwinder asked, as an exception or a stack walk would ask it, from a stack and registers made up
 * for one instruction. `rsp` and `rbp` are what the registers hold when that instruction is about to
 * run; what comes back is what the unwinder says the caller's are. */
struct Unwound {
  bool found = false;
  uint64_t rip = 0, rsp = 0, rbp = 0;
};
Unwound unwind_at(uintptr_t pc, uint64_t rsp, uint64_t rbp) {
  CONTEXT c;
  std::memset(&c, 0, sizeof c);
  c.Rip = pc;
  c.Rsp = rsp;
  c.Rbp = rbp;
  DWORD64 image = 0;
  PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &image, nullptr);
  Unwound u;
  if (rf == nullptr) {
    return u;
  }
  PVOID handler_data = nullptr;
  DWORD64 establisher = 0;
  RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, rf, &c, &handler_data, &establisher, nullptr);
  u.found = true;
  u.rip = c.Rip;
  u.rsp = c.Rsp;
  u.rbp = c.Rbp;
  return u;
}

constexpr uint64_t kCallerRbp = 0x1234567800000000ull;
constexpr uint64_t kReturnTo = 0x00007ff700001110ull;

} // namespace

TEST(Win64CallsUnwind, TheUnwinderRecoversTheCallerFromEveryInstructionOfBothProloguesAndEpilogues) {
  /* Wine's unwinder reads `ret imm16` as the end of an epilogue and applies the immediate, which
   * the unwind codes do not (they say the caller's `rsp` is the address of the stack arguments).
   * The two agree when there are none, so the body's three epilogue instructions are checked for a
   * function with no stack arguments against the codes' answer, and for one with them against
   * wine's, said so in the failure. */
  for (unsigned n : {0u, 6u}) {
    SCOPED_TRACE(n);
    JitWorld w;
    Fn f(plain_callable(n));
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    Emit em(f, GRJIT_ARCH_X86_64_WIN64, nullptr, grcore_jit_layout()->request_word_offset);
    ASSERT_TRUE(em.ok());
    const GRJIT_Prologue & pro = em.e.prologue;
    ASSERT_EQ(grjit_code_size(c.code), em.e.size);
    const uintptr_t image = reinterpret_cast<uintptr_t>(grjit_code_address(c.code));
    const uint8_t * code = static_cast<const uint8_t *>(grjit_code_address(c.code));
    ASSERT_EQ(std::memcmp(code, em.e.bytes, em.e.size), 0);
    const uint32_t in_a = stack_args(n);
    std::vector<uint64_t> stack(512, 0);
    const uint64_t b = reinterpret_cast<uintptr_t>(&stack[300]); // a frame base
    auto put = [&](uint64_t at, uint64_t v) { *reinterpret_cast<uint64_t *>(at) = v; };
    put(b, kCallerRbp);
    put(b + 8, kReturnTo);

    /* ---- The body's prologue: its first instruction, after the push, after the move, just before the allocation
     * and after it. */
    const uint32_t entry = em.e.internal_offset;
    const uint32_t sub_len = pro.alloc_bytes > 127 ? 7u : 4u;
    struct Pc {
      const char * what;
      uint32_t off;
      uint64_t rsp, rbp;
    };
    const Pc prologue[] = {
        {"the first instruction", entry, b + 8, kCallerRbp},
        {"after push rbp", pro.push_end, b, kCallerRbp},
        {"after mov rbp, rsp", pro.setfp_end, b, b},
        {"before sub rsp", pro.alloc_end - sub_len, b, b},
        {"after sub rsp", pro.alloc_end, b - pro.alloc_bytes, b},
        {"in the middle of the body", pro.alloc_end + 20, b - 100, b},
    };
    for (const Pc & p : prologue) {
      Unwound u = unwind_at(image + p.off, p.rsp, p.rbp);
      ASSERT_TRUE(u.found) << p.what;
      EXPECT_EQ(u.rip, kReturnTo) << p.what;
      EXPECT_EQ(u.rsp, b + 16) << p.what << ": the caller's stack pointer is the address of the stack arguments";
      EXPECT_EQ(u.rbp, kCallerRbp) << p.what;
    }

    /* ---- The body's epilogue: lea rsp,[rbp]; pop rbp; ret / ret imm16. */
    const Bytes lea_pop = {0x48, 0x8D, 0x65, 0x00, 0x5D};
    size_t at = 0;
    for (size_t i = entry; i + 5 <= em.e.size; i++) {
      if (std::memcmp(code + i, lea_pop.data(), 5) == 0) {
        at = i;
        break;
      }
    }
    ASSERT_NE(at, 0u);
    const uint64_t expect_rsp = b + 16 + in_a; // what wine's emulation of `ret imm16` gives; `ret` has in_a == 0
    const Pc epilogue[] = {
        {"lea rsp, [rbp]", static_cast<uint32_t>(at), b - pro.alloc_bytes, b},
        {"pop rbp", static_cast<uint32_t>(at + 4), b, b},
        {"ret", static_cast<uint32_t>(at + 5), b + 8, kCallerRbp},
    };
    for (const Pc & p : epilogue) {
      Unwound u = unwind_at(image + p.off, p.rsp, p.rbp);
      ASSERT_TRUE(u.found) << p.what;
      EXPECT_EQ(u.rip, kReturnTo) << p.what;
      EXPECT_EQ(u.rsp, expect_rsp)
          << p.what << (in_a == 0 ? "" : ": wine applies the immediate of ret imm16, which the unwind codes do not");
      EXPECT_EQ(u.rbp, kCallerRbp) << p.what;
    }

    /* ---- The adapter: a static frame, `push rbp; sub rsp, N'`, whose stack is recovered from rsp alone. */
    const uint32_t nprime = pro.adapter_alloc_bytes;
    const uint64_t r = b + 8; // the address of the adapter's return address
    const uint64_t bottom = r - 8 - nprime;
    put(r - 8, kCallerRbp);
    put(r, kReturnTo);
    const Pc adapter_pro[] = {
        {"the adapter's first instruction", 0, r, kCallerRbp},
        {"after the adapter's push rbp", pro.adapter_push_end, r - 8, kCallerRbp},
        {"after the adapter's sub rsp", pro.adapter_alloc_end, bottom, kCallerRbp},
        // Past the allocation rbp holds the chain-end marker, which no frame register may read.
        {"inside the adapter, rbp the marker", pro.adapter_alloc_end + 4, bottom, GRCORE_COMPILED_CHAIN_END},
    };
    for (const Pc & p : adapter_pro) {
      Unwound u = unwind_at(image + p.off, p.rsp, p.rbp);
      ASSERT_TRUE(u.found) << p.what;
      EXPECT_EQ(u.rip, kReturnTo) << p.what;
      EXPECT_EQ(u.rsp, r + 8) << p.what;
      EXPECT_EQ(u.rbp, kCallerRbp) << p.what;
    }
    /* After its internal call returns, with rsp at the bottom of the frame (as an unwind through the callee
     * puts it): the instruction after `call rax`. */
    {
      size_t call_at = 0;
      for (size_t i = 0; i + 2 <= pro.adapter_end; i++) {
        if (code[i] == 0xFF && code[i + 1] == 0xD0) {
          call_at = i + 2;
        }
      }
      ASSERT_NE(call_at, 0u);
      Unwound u = unwind_at(image + call_at, bottom, GRCORE_COMPILED_CHAIN_END);
      ASSERT_TRUE(u.found);
      EXPECT_EQ(u.rip, kReturnTo);
      EXPECT_EQ(u.rsp, r + 8);
      EXPECT_EQ(u.rbp, kCallerRbp) << "recovered from the adapter's own frame, not from the marker";
    }
    /* The adapter's last `add rsp; pop rbp; ret`: the refusal path, freeing the whole frame. */
    {
      size_t pop_at = 0;
      for (size_t i = 0; i + 2 <= pro.adapter_end; i++) {
        if (code[i] == 0x5D && code[i + 1] == 0xC3) {
          pop_at = i;
        }
      }
      ASSERT_NE(pop_at, 0u);
      const size_t add_at = pop_at - (nprime > 127 ? 7 : 4);
      const Pc epi[] = {
          {"the adapter's add rsp", static_cast<uint32_t>(add_at), bottom, kCallerRbp},
          {"the adapter's pop rbp", static_cast<uint32_t>(pop_at), r - 8, kCallerRbp},
          {"the adapter's ret", static_cast<uint32_t>(pop_at + 1), r, kCallerRbp},
      };
      for (const Pc & p : epi) {
        Unwound u = unwind_at(image + p.off, p.rsp, p.rbp);
        ASSERT_TRUE(u.found) << p.what;
        EXPECT_EQ(u.rip, kReturnTo) << p.what;
        EXPECT_EQ(u.rsp, r + 8) << p.what;
        EXPECT_EQ(u.rbp, kCallerRbp) << p.what;
      }
    }
  }
}

#endif // GRJIT_TEST_WIN64

GRJIT_TEST_MAIN()
