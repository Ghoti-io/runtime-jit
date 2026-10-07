/**
 * @file
 *
 * The machine code of the generated functions, pinned, for both backends.
 *
 * The differential proves that compiled code means what the IR means; it does
 * not notice a refactor that changes which bytes mean it. This test emits the
 * differential's 2000 generated functions (never running them, with made-up
 * helper addresses and a made-up request-word offset, so the bytes depend on
 * nothing about the process or about the core's struct layout) for each
 * instruction set, on any host, and folds every byte of every function into
 * one 64-bit FNV-1a value, which must equal the one recorded here.
 *
 * A change to a backend that is meant to change its code changes the pin in the
 * same commit and says why. A change that is not must leave it alone: moving
 * the liveness pass and the metadata builder into a directory both backends
 * share is what recorded the x86-64 pin (it is the hash of the tree before that
 * move, and the same after it). The arm64 pin is the arm64 backend's first.
 *
 * A fourth pin covers what the generated functions cannot: callable functions
 * with calls to other compiled functions and no tail call (AD-28). It was
 * measured at the commit before tail calls (story 5 of the calls spec) and
 * holds the claim that adding them left every byte of such a function as it
 * was. It is emitted for x86-64 SysV only, the one target that has calls.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../ir_gen.h"

#include "../../src/code/code_internal.h"

namespace {

constexpr int kFunctions = 2000;
constexpr uint32_t kRequestOffset = 0x40;

/* Nothing here is ever run, so the helpers can be made-up addresses. */
const void * const kFakeHelpers[7] = {
    reinterpret_cast<const void *>(0x10000), reinterpret_cast<const void *>(0x10100),
    reinterpret_cast<const void *>(0x10200), reinterpret_cast<const void *>(0x10300),
    reinterpret_cast<const void *>(0x10400), reinterpret_cast<const void *>(0x10500),
    reinterpret_cast<const void *>(0x10600)};
const GRJIT_PollHelper kFakePoll = reinterpret_cast<GRJIT_PollHelper>(0x20000);

uint64_t fold(uint64_t h, uint8_t byte) { return (h ^ byte) * 0x100000001B3ull; }

struct Pin {
  uint64_t hash = 0xCBF29CE484222325ull;
  uint64_t bytes = 0;
};

Pin pin_of(GRJIT_Arch arch) {
  Pin pin;
  for (uint64_t seed = 1; seed <= kFunctions; seed++) {
    irgen::Gen gen(seed, kFakeHelpers, kFakePoll);
    Fn f(gen.build());
    GRJIT_Emitted e;
    GRJIT_Result r = grjit_emit_for(
        arch, f, grjit_allocator_default(), nullptr, nullptr, kRequestOffset, &e);
    EXPECT_EQ(r, GRJIT_OK) << "seed " << seed << ": " << grjit_result_string(r);
    if (r != GRJIT_OK) {
      return pin;
    }
    for (size_t i = 0; i < e.size; i++) {
      pin.hash = fold(pin.hash, e.bytes[i]);
    }
    pin.hash = fold(pin.hash, 0xFF); // a function boundary
    pin.bytes += e.size;
    grjit_emitted_free(&e);
  }
  return pin;
}

/* ---- Callable functions with calls and no tail call -------------------------
 *
 * One function for every combination of a parameter count, an argument count
 * and a form of call (slot, code pointer in a register, code pointer as an
 * immediate), each in two shapes: a call and a return; and a guard, a poll, a
 * derived pointer as an argument and a branch to a second return. The
 * parameters rotate through I64, REF and PTR. Nothing here is run, so the hooks,
 * the slots and the poll helper are made-up addresses. */

constexpr unsigned kParams[] = {0, 1, 2, 3, 6, 7, 9, 16};
constexpr unsigned kArgs[] = {0, 1, 4, 6, 7, 10, 16};

Pin callable_pin(unsigned * pointer_calls) {
  Pin pin;
  unsigned id = 0;
  unsigned checks = 0;
  GRJIT_CallHooks hooks{};
  hooks.push = reinterpret_cast<decltype(hooks.push)>(0x30000);
  hooks.pop = reinterpret_cast<decltype(hooks.pop)>(0x30100);
  hooks.compile = reinterpret_cast<decltype(hooks.compile)>(0x30200);
  hooks.deopt = reinterpret_cast<decltype(hooks.deopt)>(0x30300);
  for (unsigned params : kParams) {
    for (unsigned args : kArgs) {
      for (unsigned form = 0; form < 3; form++) {
        for (unsigned variant = 0; variant < 2; variant++, id++) {
          B b("pin", 3);
          b.callable(hooks);
          EXPECT_EQ(grjit_builder_set_token(b.b, 1000 + id), GRJIT_OK);
          b.poll_helper(kFakePoll);
          const GRJIT_Type rotate[3] = {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_PTR};
          std::vector<GRJIT_VReg> p;
          for (unsigned i = 0; i < params; i++) {
            p.push_back(b.param(rotate[i % 3]));
          }
          GRJIT_VReg r = b.reg(variant == 0 && id % 4 == 2 ? GRJIT_TYPE_REF : GRJIT_TYPE_I64);
          GRJIT_VReg d = GRJIT_NO_VREG;
          if (variant == 1 && params >= 2) {
            d = b.reg(GRJIT_TYPE_PTR);
            b.derived(d, p[1], 24);
          }
          GRJIT_BlockId b0 = b.block();
          GRJIT_BlockId b1 = variant == 1 ? b.block() : 0;
          GRJIT_BlockId b2 = variant == 1 ? b.block() : 0;
          b.at(b0);
          const GRCORE_PollIdentity at{1, id};
          std::vector<GRJIT_FrameSlot> state = {grjit_frame_slot_constant(id),
              params > 0 ? grjit_frame_slot_vreg(p[0]) : grjit_frame_slot_dead(),
              params > 1 ? grjit_frame_slot_vreg(p[1]) : grjit_frame_slot_dead()};
          if (variant == 1) {
            if (params > 0) {
              b.guard(V(p[0]), at, state);
            }
            b.poll(at, state);
            if (d != GRJIT_NO_VREG) {
              b.bitcast(d, p[1]);
              b.bin(GRJIT_OP_ADD, d, V(d), I(24));
            }
          }
          std::vector<GRJIT_Operand> ops;
          for (unsigned i = 0; i < args; i++) {
            if (i == 0 && d != GRJIT_NO_VREG) {
              ops.push_back(V(d));
            } else if (i < params) {
              ops.push_back(V(p[i]));
            } else {
              ops.push_back(I(static_cast<int64_t>(i) * 3 + 1));
            }
          }
          if (form == 0) {
            b.call_slot(r, reinterpret_cast<const void *>(0x40000 + 8 * id), 500 + id, ops, at,
                state, at, state);
          } else if (form == 1 && params >= 3) {
            b.call_ptr(r, V(p[2]), 500 + id, ops, at, state, at, state);
          } else {
            b.call_ptr(r, I(0x50000 + 16 * id), 500 + id, ops, at, state, at, state);
          }
          if (variant == 1) {
            b.br_if(V(r), b1, b2);
            b.at(b1);
            b.ret(V(r));
            b.at(b2);
            b.ret(I(0));
          } else {
            b.ret(V(r));
          }
          Fn f(b.finish());
          char why[256];
          EXPECT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << id << why;
          GRJIT_Emitted e;
          GRJIT_Result res = grjit_emit_for(GRJIT_ARCH_X86_64, f, grjit_allocator_default(),
              nullptr, nullptr, kRequestOffset, &e);
          EXPECT_EQ(res, GRJIT_OK) << "function " << id << ": " << grjit_result_string(res);
          if (res != GRJIT_OK) {
            return pin;
          }
          /* A call through a pointer calls the library's own target check, whose
           * address is the one thing in the bytes that is the process's and not
           * the emitter's. It is replaced by a marker before hashing, and the
           * marker is counted: one for each such call, or the pin proved less. */
          const uint64_t check = reinterpret_cast<uint64_t>(&grjit_call_target_ok);
          std::vector<uint8_t> bytes(e.bytes, e.bytes + e.size);
          for (size_t i = 0; i + 8 <= bytes.size(); i++) {
            if (std::memcmp(&bytes[i], &check, 8) == 0) {
              const uint64_t marker = 0x7A7A7A7A7A7A7A7Aull;
              std::memcpy(&bytes[i], &marker, 8);
              checks++;
              i += 7;
            }
          }
          for (uint8_t byte : bytes) {
            pin.hash = fold(pin.hash, byte);
          }
          pin.hash = fold(pin.hash, 0xFF);
          pin.bytes += e.size;
          grjit_emitted_free(&e);
        }
      }
    }
  }
  *pointer_calls = checks;
  return pin;
}

} // namespace

TEST(Pin, TheX86_64CodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  Pin pin = pin_of(GRJIT_ARCH_X86_64);
  std::printf("pin x86-64: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0xa858ae113a4a5759ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 2094987u);
}

TEST(Pin, TheX86_64CodeOfCallableFunctionsWithCallsAndNoTailCallIsByteForByteWhatWasRecorded) {
  /* The bytes depend on the core's layout of the walk-start cell and the
   * native-stack limit (which the code stores to and reads); a change to those
   * offsets is a change to the code, and records a new pin. */
  unsigned pointer_calls = 0;
  Pin pin = callable_pin(&pointer_calls);
  EXPECT_EQ(pointer_calls, 8u * 7u * 2u * 2u) << "a call through a pointer per function of forms 1 and 2";
  std::printf("pin x86-64 callable: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0x799b28304ef83871ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 285044u);
}

TEST(Pin, TheArm64CodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  Pin pin = pin_of(GRJIT_ARCH_ARM64);
  std::printf("pin arm64: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0xf250e0a928cf9231ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes % 4, 0u);
}

TEST(Pin, TheWin64CodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  /* The Microsoft x64 flavour of the x86-64 emitter: emitted on every host,
   * run only on Windows. The SysV pin above is untouched by it. */
  Pin pin = pin_of(GRJIT_ARCH_X86_64_WIN64);
  std::printf("pin win64: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0x5ab7246d43cbfd33ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 2120441u);
}

TEST(Pin, WhatGrjitCompileMapsIsWhatTheEmitterProducedForTheNativeInstructionSet) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  for (uint64_t seed = 1; seed <= 200; seed++) {
    irgen::Gen gen(seed, kFakeHelpers, kFakePoll);
    Fn f(gen.build());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c) << "seed " << seed;
    GRJIT_Emitted e;
    ASSERT_EQ(grjit_emit_for(grjit_native_arch(), f, grjit_allocator_default(), nullptr,
                  nullptr, grcore_jit_layout()->request_word_offset, &e),
        GRJIT_OK);
    ASSERT_EQ(grjit_code_size(c.code), e.size) << seed;
    EXPECT_EQ(std::memcmp(grjit_code_address(c.code), e.bytes, e.size), 0) << seed;
    grjit_emitted_free(&e);
  }
}

GRJIT_TEST_MAIN()
