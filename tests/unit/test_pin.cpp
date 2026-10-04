/**
 * @file
 *
 * The machine code of the generated functions, pinned.
 *
 * The differential proves that compiled code means what the IR means; it does
 * not notice a refactor that changes which bytes mean it. This test compiles
 * the differential's 2000 generated functions (never running them, so the
 * helpers can be made-up addresses and the bytes do not depend on where the
 * process was loaded) and folds every byte of every function into one 64-bit
 * FNV-1a value, which must equal the one recorded here for the target.
 *
 * A change to the backend that is meant to change the code changes the pin in
 * the same commit and says why. A change that is not (moving the liveness pass
 * and the metadata builder into a directory they share with the arm64 backend,
 * which is what recorded these) must leave it alone.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../ir_gen.h"

namespace {

constexpr int kFunctions = 2000;

/* Nothing here is ever run, so the helpers can be made-up addresses: the
 * bytes then do not depend on where the process was loaded. */
const void * const kFakeHelpers[7] = {
    reinterpret_cast<const void *>(0x10000), reinterpret_cast<const void *>(0x10100),
    reinterpret_cast<const void *>(0x10200), reinterpret_cast<const void *>(0x10300),
    reinterpret_cast<const void *>(0x10400), reinterpret_cast<const void *>(0x10500),
    reinterpret_cast<const void *>(0x10600)};
const GRJIT_PollHelper kFakePoll = reinterpret_cast<GRJIT_PollHelper>(0x20000);

uint64_t fold(uint64_t h, uint8_t byte) { return (h ^ byte) * 0x100000001B3ull; }

} // namespace

#if defined(__x86_64__)
#define GRJIT_PIN_HASH 0xb612f8e624c40ac8ull
#else
#define GRJIT_PIN_HASH 0x0ull
#endif

TEST(Pin, TheCodeOfTheGeneratedFunctionsIsByteForByteWhatWasRecorded) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  uint64_t hash = 0xCBF29CE484222325ull;
  uint64_t total_bytes = 0;
  for (uint64_t seed = 1; seed <= kFunctions; seed++) {
    irgen::Gen gen(seed, kFakeHelpers, kFakePoll);
    Fn f(gen.build());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c) << "seed " << seed << ": " << grjit_result_string(c.result);
    const uint8_t * code = static_cast<const uint8_t *>(grjit_code_address(c.code));
    size_t n = grjit_code_size(c.code);
    for (size_t i = 0; i < n; i++) {
      hash = fold(hash, code[i]);
    }
    hash = fold(hash, 0xFF); // a function boundary
    total_bytes += n;
  }
  std::printf("pin: %llu bytes, hash %016llx\n",
      static_cast<unsigned long long>(total_bytes),
      static_cast<unsigned long long>(hash));
  EXPECT_EQ(hash, GRJIT_PIN_HASH)
      << "the generated code changed: " << total_bytes << " bytes";
}

GRJIT_TEST_MAIN()
