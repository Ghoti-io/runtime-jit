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

} // namespace

TEST(Pin, TheX86_64CodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  Pin pin = pin_of(GRJIT_ARCH_X86_64);
  std::printf("pin x86-64: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0xa858ae113a4a5759ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 2094987u);
}

TEST(Pin, TheArm64CodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  Pin pin = pin_of(GRJIT_ARCH_ARM64);
  std::printf("pin arm64: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0xf250e0a928cf9231ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes % 4, 0u);
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
