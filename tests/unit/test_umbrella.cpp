/**
 * @file
 *
 * The umbrella header, included first and alone from C++.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <gtest/gtest.h>

TEST(Umbrella, DeclaresTheWholeSurfaceFromCxx) {
  EXPECT_STRNE(grjit_version_string(), "");
  EXPECT_STREQ(grjit_result_string(GRJIT_OK), "No error");
  GRJIT_Limits limits;
  grjit_limits_default(&limits);
  EXPECT_EQ(limits.max_blocks, 4096u);
  EXPECT_NE(grjit_allocator_default(), nullptr);
  // Every area has an entry point that is reachable and refuses a NULL.
  GRJIT_Builder * b = nullptr;
  EXPECT_EQ(grjit_builder_create("f", 0, nullptr, nullptr, nullptr),
      GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_builder_create("f", 0, nullptr, nullptr, &b), GRJIT_OK);
  grjit_builder_destroy(b);
  EXPECT_EQ(grjit_function_verify(nullptr, nullptr, nullptr, 0),
      GRJIT_ERR_INVALID);
  size_t n = 0;
  EXPECT_EQ(grjit_function_print(nullptr, nullptr, 0, &n), GRJIT_ERR_INVALID);
  GRJIT_CompileOptions o{};
  GRJIT_Code * code = nullptr;
  EXPECT_EQ(grjit_compile(&o, nullptr, &code), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_code_meta(nullptr), nullptr);
  EXPECT_EQ(GRJIT_EXIT_REFUSED, 2);
  EXPECT_EQ(GRJIT_EXIT_DEOPT, 1);
  EXPECT_EQ(GRJIT_EXIT_RETURNED, 0);
  // The runtime-core formats the code carries come through the same headers.
  EXPECT_EQ(GRCORE_CODEMETA_FORMAT_VERSION, 1u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
