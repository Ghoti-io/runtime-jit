/**
 * @file
 *
 * Versions, result strings, the allocator and the limits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

TEST(Core, VersionStringAndNumberAgreeWithTheGeneratedHeader) {
  EXPECT_STREQ(grjit_version_string(), GRJIT_VERSION_STRING);
  EXPECT_EQ(grjit_version_number(), GRJIT_VERSION_NUMBER);
  EXPECT_EQ(GRJIT_VERSION_NUMBER,
      GRJIT_MAKE_VERSION(GRJIT_VERSION_MAJOR, GRJIT_VERSION_MINOR, GRJIT_VERSION_PATCH));
  EXPECT_EQ(GRJIT_MAKE_VERSION(1, 2, 3), 0x010203u);
}

TEST(Core, EveryResultHasADistinctNonEmptyString) {
  std::vector<std::string> seen;
  for (int r = 0; r < GRJIT_RESULT_COUNT; ++r) {
    const char * s = grjit_result_string(static_cast<GRJIT_Result>(r));
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "");
    EXPECT_STRNE(s, "Unknown error") << "result " << r << " has no string";
    for (const std::string & other : seen) {
      EXPECT_NE(other, s);
    }
    seen.push_back(s);
  }
  EXPECT_STREQ(grjit_result_string(static_cast<GRJIT_Result>(GRJIT_RESULT_COUNT)), "Unknown error");
  EXPECT_STREQ(grjit_result_string(static_cast<GRJIT_Result>(-1)), "Unknown error");
}

TEST(Core, TheResultVocabularyIsTheSuitesAndKeepsItsNumbers) {
  // CONVENTIONS.md section 5; the same numbering as runtime-core up to
  // INTERNAL, and no ERR_GUEST: a JIT has no guest of its own.
  EXPECT_EQ(GRJIT_OK, 0);
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_IO), static_cast<int>(GRCORE_ERR_IO));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_FORMAT), static_cast<int>(GRCORE_ERR_FORMAT));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_UNSUPPORTED), static_cast<int>(GRCORE_ERR_UNSUPPORTED));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_LIMIT), static_cast<int>(GRCORE_ERR_LIMIT));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_CORRUPT), static_cast<int>(GRCORE_ERR_CORRUPT));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_OOM), static_cast<int>(GRCORE_ERR_OOM));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_INVALID), static_cast<int>(GRCORE_ERR_INVALID));
  EXPECT_EQ(static_cast<int>(GRJIT_ERR_INTERNAL), static_cast<int>(GRCORE_ERR_INTERNAL));
  EXPECT_EQ(static_cast<int>(GRJIT_RESULT_COUNT), static_cast<int>(GRJIT_ERR_INTERNAL) + 1);
}

TEST(Core, TheDefaultAllocatorIsCutils) {
  EXPECT_EQ(grjit_allocator_default(), gcu_allocator_default());
}

TEST(Limits, DefaultsAreTheDocumentedOnes) {
  GRJIT_Limits limits;
  std::memset(&limits, 0xff, sizeof limits);
  grjit_limits_default(&limits);
  EXPECT_EQ(limits.max_blocks, 4096u);
  EXPECT_EQ(limits.max_vregs, 65536u);
  EXPECT_EQ(limits.max_operations, 262144u);
  EXPECT_EQ(limits.max_call_arguments, 6u);
  EXPECT_EQ(limits.max_frame_state_slots, 4096u);
  EXPECT_EQ(limits.max_frame_bytes, 1u << 20);
  EXPECT_EQ(limits.max_code_bytes, 16u << 20);
  EXPECT_EQ(limits.max_site_entries, 4u << 20);
  grjit_limits_default(nullptr); // ignored
}

TEST(Limits, AZeroFieldIsThatFieldsDefaultAndANullStructIsAllDefaults) {
  // A builder created with a partly-filled struct takes the rest from the
  // defaults: only max_blocks is set, so a fifth block is refused.
  GRJIT_Limits limits{};
  limits.max_blocks = 4;
  B b("f", 0, &limits);
  for (int i = 0; i < 4; i++) {
    b.block();
  }
  GRJIT_BlockId id;
  EXPECT_EQ(grjit_builder_block(b.b, &id), GRJIT_ERR_LIMIT);
  B other("g", 0, nullptr);
  for (int i = 0; i < 5; i++) {
    other.block();
  }
}

TEST(Limits, CallArgumentsAndFrameStateSlotsAreClampedToWhatTheBackendCanAddress) {
  // A limit above the backend's six argument registers is six, and a frame
  // state may not be so long that a guard exit's 8 * i displacement wraps.
  GRJIT_Limits limits{};
  limits.max_call_arguments = 100;
  limits.max_frame_state_slots = SIZE_MAX;
  B b("f", 0, &limits);
  b.at(b.block());
  std::vector<GRJIT_Operand> args(7, I(1));
  EXPECT_EQ(grjit_builder_call(b.b, GRJIT_NO_VREG, 0x1000, GRJIT_CALL_NO_GC,
                GRCORE_SITE_GC_POINT_CALL, args.data(), 7, {0, 0}, nullptr, 0),
      GRJIT_ERR_LIMIT);
  GRJIT_Builder * huge = nullptr;
  EXPECT_EQ(grjit_builder_create("g", (size_t{1} << 30) / 8 + 1, &limits, nullptr, &huge),
      GRJIT_ERR_LIMIT);
}

GRJIT_TEST_MAIN()
