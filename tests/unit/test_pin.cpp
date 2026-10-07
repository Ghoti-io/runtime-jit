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
 * was. It is the x86-64 SysV one; arm64 has its own (below).
 *
 * A fifth covers callable functions with calls to natives (story 6): one for
 * every combination of a parameter count, a native's arity, its status and its
 * result type, in two shapes, measured after the emitter landed. It holds the
 * claim that the bytes of a native call, and of everything around one, do not
 * change without a commit that says why. Win64 refuses such a function before
 * a byte, which the same test shows for each.
 *
 * Three more are arm64's (story 7 of the calls spec), each measured when its part of the
 * emitter landed and held from then on: the same callable functions with calls, one tail
 * call to each form of callee for every combination of a parameter count and an argument
 * count, and the same functions with calls to natives. On arm64 an address is materialised
 * by `movz` and `movk`, so the one thing in the bytes that is the process's, the library's
 * own target check, is found as that instruction sequence and replaced by a marker, which
 * is counted.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../callable_gen.h"
#include "../ir_gen.h"

#include "../../src/arm64/asm_internal.h"
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

/* ---- Callable functions (callable_gen.h) ---------------------------------------------------
 *
 * Calls to other compiled functions and no tail call, tail calls, and calls to natives: the
 * generators are shared with the structural test of the arm64 convention. Each function is emitted
 * for `arch` and its bytes folded in. */

/* The instruction words that materialise `value` in x16, for finding it in arm64 code. */
std::vector<uint8_t> a64_address_words(uint64_t value) {
  GRJIT_A64Asm a;
  grjit_a64_init(&a, grjit_allocator_default(), 64);
  grjit_a64_mov_ri(&a, GRJIT_A64_X16, value);
  std::vector<uint8_t> out(grjit_a64_bytes(&a), grjit_a64_bytes(&a) + grjit_a64_size(&a));
  grjit_a64_free(&a);
  return out;
}

/* Replaces every occurrence of the library's own target check's address in `bytes` by a marker,
 * and returns how many there were: the one thing in the code that is the process's and not the
 * emitter's, so the hash would otherwise change with the load address. On arm64 the address is
 * the `movz`/`movk` sequence, whatever its length, and the marker is three words. */
unsigned mask_target_check(GRJIT_Arch arch, std::vector<uint8_t> * bytes) {
  const uint64_t check = reinterpret_cast<uint64_t>(&grjit_call_target_ok);
  unsigned found = 0;
  if (arch == GRJIT_ARCH_ARM64) {
    const std::vector<uint8_t> seq = a64_address_words(check);
    const std::vector<uint8_t> marker(12, 0x7A);
    for (size_t i = 0; i + seq.size() <= bytes->size(); i += 4) {
      if (std::memcmp(bytes->data() + i, seq.data(), seq.size()) == 0) {
        bytes->erase(bytes->begin() + static_cast<long>(i), bytes->begin() + static_cast<long>(i + seq.size()));
        bytes->insert(bytes->begin() + static_cast<long>(i), marker.begin(), marker.end());
        i += marker.size() - 4;
        found++;
      }
    }
    return found;
  }
  for (size_t i = 0; i + 8 <= bytes->size(); i++) {
    if (std::memcmp(&(*bytes)[i], &check, 8) == 0) {
      const uint64_t marker = 0x7A7A7A7A7A7A7A7Aull;
      std::memcpy(&(*bytes)[i], &marker, 8);
      found++;
      i += 7;
    }
  }
  return found;
}

/* Win64 refuses a callable function, and every call-machinery operation in one, before a byte: the error is
 * GRJIT_ERR_UNSUPPORTED, nothing is emitted and nothing was even allocated. */
bool win64_refuses(const GRJIT_Function * f) {
  TrackingAllocator t;
  GRJIT_Emitted other;
  const GRJIT_Result r = grjit_emit_for(GRJIT_ARCH_X86_64_WIN64, f, t.get(), nullptr, nullptr, kRequestOffset, &other);
  return r == GRJIT_ERR_UNSUPPORTED && other.size == 0 && other.bytes == nullptr && t.calls == 0 && t.live == 0;
}

Pin callable_pin(GRJIT_Arch arch, unsigned * pointer_calls, unsigned * functions, bool * win64_refused,
    GRJIT_EntryHook hook = nullptr) {
  Pin pin;
  unsigned checks = 0;
  *win64_refused = true;
  cg::each_call_function([&](const GRJIT_Function * f, unsigned id) {
    GRJIT_Emitted e;
    GRJIT_Result res = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, hook, kRequestOffset, &e);
    EXPECT_EQ(res, GRJIT_OK) << "function " << id << ": " << grjit_result_string(res);
    if (res != GRJIT_OK) {
      return false;
    }
    /* A call through a pointer calls the library's own target check, whose address is the one thing
     * in the bytes that is the process's and not the emitter's. It is replaced by a marker before
     * hashing, and the marker is counted: one for each such call, or the pin proved less. */
    std::vector<uint8_t> bytes(e.bytes, e.bytes + e.size);
    checks += mask_target_check(arch, &bytes);
    for (uint8_t byte : bytes) {
      pin.hash = fold(pin.hash, byte);
    }
    pin.hash = fold(pin.hash, 0xFF);
    pin.bytes += bytes.size();
    grjit_emitted_free(&e);
    *win64_refused = *win64_refused && win64_refuses(f);
    *functions += 1;
    return true;
  });
  *pointer_calls = checks;
  return pin;
}

Pin tail_pin(GRJIT_Arch arch, unsigned * pointer_calls, unsigned * functions, bool * win64_refused) {
  Pin pin;
  unsigned checks = 0;
  *win64_refused = true;
  cg::each_tail_function([&](const GRJIT_Function * f, unsigned id) {
    GRJIT_Emitted e;
    GRJIT_Result res = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, kRequestOffset, &e);
    EXPECT_EQ(res, GRJIT_OK) << "tail function " << id << ": " << grjit_result_string(res);
    if (res != GRJIT_OK) {
      return false;
    }
    std::vector<uint8_t> bytes(e.bytes, e.bytes + e.size);
    checks += mask_target_check(arch, &bytes);
    for (uint8_t byte : bytes) {
      pin.hash = fold(pin.hash, byte);
    }
    pin.hash = fold(pin.hash, 0xFF);
    pin.bytes += bytes.size();
    grjit_emitted_free(&e);
    *win64_refused = *win64_refused && win64_refuses(f);
    *functions += 1;
    return true;
  });
  *pointer_calls = checks;
  return pin;
}

Pin native_pin(GRJIT_Arch arch, unsigned * functions, bool * all_refused_elsewhere) {
  Pin pin;
  *all_refused_elsewhere = true;
  cg::each_native_function([&](const GRJIT_Function * f, unsigned id) {
    GRJIT_Emitted e;
    GRJIT_Result res = grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, kRequestOffset, &e);
    EXPECT_EQ(res, GRJIT_OK) << "function " << id << ": " << grjit_result_string(res);
    if (res != GRJIT_OK) {
      return false;
    }
    for (size_t i = 0; i < e.size; i++) {
      pin.hash = fold(pin.hash, e.bytes[i]);
    }
    pin.hash = fold(pin.hash, 0xFF);
    pin.bytes += e.size;
    grjit_emitted_free(&e);
    *all_refused_elsewhere = *all_refused_elsewhere && win64_refuses(f);
    *functions = id + 1;
    return true;
  });
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
  unsigned pointer_calls = 0, functions = 0;
  bool refused = false;
  Pin pin = callable_pin(GRJIT_ARCH_X86_64, &pointer_calls, &functions, &refused);
  EXPECT_EQ(functions, 8u * 7u * 3u * 2u);
  EXPECT_TRUE(refused) << "Win64 refuses every one of them, allocating nothing";
  EXPECT_EQ(pointer_calls, 8u * 7u * 2u * 2u) << "a call through a pointer per function of forms 1 and 2";
  std::printf("pin x86-64 callable: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0x799b28304ef83871ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 285044u);
}

TEST(Pin, TheX86_64CodeOfCallableFunctionsWithCallsToNativesIsByteForByteWhatWasRecorded) {
  /* Measured after the emitter of calls to natives landed (story 6 of the calls
   * spec), and held from then on. */
  unsigned functions = 0;
  bool refused = false;
  Pin pin = native_pin(GRJIT_ARCH_X86_64, &functions, &refused);
  std::printf("pin x86-64 natives: %u functions, %llu bytes, hash %016llx\n", functions,
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(functions, 4u * 8u * 2u * 4u * 2u);
  EXPECT_TRUE(refused) << "Win64 refuses every one of them before a byte";
  EXPECT_EQ(pin.hash, 0xd2d04cb10df6dce8ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 355876u);
}

TEST(Pin, TheArm64CodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  Pin pin = pin_of(GRJIT_ARCH_ARM64);
  std::printf("pin arm64: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0xf250e0a928cf9231ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes % 4, 0u);
}

TEST(Pin, TheArm64CodeOfCallableFunctionsWithCallsIsByteForByteWhatWasRecorded) {
  /* Measured after the arm64 emitter of calls landed (story 7 of the calls spec), and held from
   * then on: the code depends on the core's layout of the walk-start cell and the native-stack
   * limit, which it stores to and reads. The same functions as the x86-64 pin above. */
  unsigned pointer_calls = 0, functions = 0;
  bool refused = false;
  Pin pin = callable_pin(GRJIT_ARCH_ARM64, &pointer_calls, &functions, &refused);
  EXPECT_EQ(functions, 8u * 7u * 3u * 2u);
  EXPECT_TRUE(refused) << "Win64 refuses every one of them, allocating nothing";
  EXPECT_EQ(pointer_calls, 8u * 7u * 2u * 2u) << "a call through a pointer per function of forms 1 and 2";
  std::printf("pin arm64 callable: %u functions, %llu bytes, hash %016llx\n", functions,
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.bytes % 4, 0u);
  EXPECT_EQ(pin.hash, 0x8ff93d28f8d8ec19ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 266492u);
}

/* An entry hook is an address the emitter is handed and never calls here: a constant that is not any
 * process's, so the code is as deterministic as without one. The adapter calls it and narrows its 32-bit answer
 * before it tests it, which no other pin's corpus has (it has no entry hook); the tests of
 * tests/unit/test_calls.cpp and test_poll.cpp catch the same edit by an assertion and this pins the bytes. */
GRJIT_EntryHook fake_entry_hook() { return reinterpret_cast<GRJIT_EntryHook>(uintptr_t{0x0000123456789ab0ull}); }

TEST(Pin, TheX86_64CodeOfCallableFunctionsWithAnEntryHookIsByteForByteWhatWasRecorded) {
  unsigned pointer_calls = 0, functions = 0;
  bool refused = false;
  Pin pin = callable_pin(GRJIT_ARCH_X86_64, &pointer_calls, &functions, &refused, fake_entry_hook());
  EXPECT_EQ(functions, 8u * 7u * 3u * 2u);
  EXPECT_TRUE(refused);
  std::printf("pin x86-64 callable with an entry hook: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0xc5e0be7b7ef3cea1ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 293108u);
}

TEST(Pin, TheArm64CodeOfCallableFunctionsWithAnEntryHookIsByteForByteWhatWasRecorded) {
  unsigned pointer_calls = 0, functions = 0;
  bool refused = false;
  Pin pin = callable_pin(GRJIT_ARCH_ARM64, &pointer_calls, &functions, &refused, fake_entry_hook());
  EXPECT_EQ(functions, 8u * 7u * 3u * 2u);
  EXPECT_TRUE(refused);
  std::printf("pin arm64 callable with an entry hook: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.hash, 0x979ab308f05b25c3ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 273884u);
}

TEST(Pin, TheArm64CodeOfCallableFunctionsWithTailCallsIsByteForByteWhatWasRecorded) {
  /* Measured after the arm64 emitter of tail calls landed. */
  unsigned pointer_calls = 0, functions = 0;
  bool refused = false;
  Pin pin = tail_pin(GRJIT_ARCH_ARM64, &pointer_calls, &functions, &refused);
  EXPECT_EQ(functions, 8u * 7u * 3u);
  EXPECT_TRUE(refused) << "Win64 refuses every one of them, allocating nothing";
  EXPECT_EQ(pointer_calls, 8u * 7u * 2u) << "a target check per tail call through a pointer";
  std::printf("pin arm64 tail calls: %u functions, %llu bytes, hash %016llx\n", functions,
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(pin.bytes % 4, 0u);
  EXPECT_EQ(pin.hash, 0x2d55af8cac9d87baull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 110712u);
}

TEST(Pin, TheArm64CodeOfCallableFunctionsWithCallsToNativesIsByteForByteWhatWasRecorded) {
  unsigned functions = 0;
  bool refused = false;
  Pin pin = native_pin(GRJIT_ARCH_ARM64, &functions, &refused);
  std::printf("pin arm64 natives: %u functions, %llu bytes, hash %016llx\n", functions,
      static_cast<unsigned long long>(pin.bytes), static_cast<unsigned long long>(pin.hash));
  EXPECT_EQ(functions, 4u * 8u * 2u * 4u * 2u);
  EXPECT_TRUE(refused) << "Win64 refuses every one of them before a byte";
  EXPECT_EQ(pin.bytes % 4, 0u);
  EXPECT_EQ(pin.hash, 0xfc567e8f41a66ba7ull) << pin.bytes << " bytes";
  EXPECT_EQ(pin.bytes, 332284u);
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
