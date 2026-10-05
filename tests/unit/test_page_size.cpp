/**
 * @file
 *
 * The compiler reads the page provider it is given (AD-13), and a provider
 * states its own size (runtime-core's `b/page.h`), so that the struct can grow
 * at the end. The cases here are those of runtime-core's test_page_size.cpp as
 * the compiler sees them: a provider that predates `protect` is unsupported and
 * its `protect` is never read, one whose size is not valid is refused before
 * anything is mapped, and each has a full-size control, so that a compile that
 * refuses every provider cannot pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

GRJIT_Function * tiny() {
  B b("tiny");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.ret(V(x));
  return b.finish();
}

// The provider as the first generation had it: everything through `unmap`,
// and nothing after. A copy of the layout, on the heap, exactly as large as
// its size says, so that a read of `protect` is a finding under ASan or
// Valgrind and not a silent zero.
struct OldProvider {
  size_t size;
  void * ctx;
  size_t page_size;
  void * (*map)(void *, size_t);
  void (*unmap)(void *, void *, size_t);
};
static_assert(sizeof(OldProvider) == GRCORE_PAGE_PROVIDER_MIN_SIZE,
    "the old layout is the minimum layout");

const GRCORE_PageProvider * old_provider_over(const FakePages & fake) {
  auto * p = static_cast<OldProvider *>(std::malloc(sizeof(OldProvider)));
  p->size = sizeof(OldProvider);
  p->ctx = fake.vtable.ctx;
  p->page_size = fake.vtable.page_size;
  p->map = fake.vtable.map;
  p->unmap = fake.vtable.unmap;
  return reinterpret_cast<const GRCORE_PageProvider *>(p);
}

GRJIT_Result compile_over(const GRCORE_PageProvider * pages) {
  Fn f(tiny());
  GRJIT_CompileOptions o{};
  o.pages = pages;
  GRJIT_Code * out = nullptr;
  GRJIT_Result r = grjit_compile(&o, f, &out);
  grjit_code_destroy(out);
  return r;
}

} // namespace

TEST(PageSizeJit, AnOlderShorterProviderIsUnsupportedAndItsAbsentProtectIsNeverRead) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  FakePages fake(w.pages());
  const GRCORE_PageProvider * old = old_provider_over(fake);
  EXPECT_EQ(compile_over(old), GRJIT_ERR_UNSUPPORTED);
  EXPECT_EQ(fake.maps, 0) << "nothing is mapped for a provider that cannot protect";
  EXPECT_EQ(fake.protects, 0);
  std::free(const_cast<GRCORE_PageProvider *>(old));
}

TEST(PageSizeJit, AProtectBeyondTheStatedSizeIsNotCalledAndAtFullSizeIs) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  {
    FakePages fake(w.pages());
    fake.vtable.size = GRCORE_PAGE_PROVIDER_MIN_SIZE;
    EXPECT_EQ(compile_over(&fake.vtable), GRJIT_ERR_UNSUPPORTED);
    EXPECT_EQ(fake.maps, 0);
    EXPECT_EQ(fake.protects, 0);
  }
  // Control: the same provider at its full size compiles, through `protect`.
  {
    FakePages fake(w.pages());
    EXPECT_EQ(compile_over(&fake.vtable), GRJIT_OK);
    EXPECT_EQ(fake.protects, 1);
    EXPECT_EQ(fake.live, 0);
  }
}

TEST(PageSizeJit, ASizeBelowTheMinimumOrOffAlignmentIsInvalidAndNothingIsMapped) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  const size_t bad[] = {0, 1, GRCORE_PAGE_PROVIDER_MIN_SIZE - sizeof(void *),
      GRCORE_PAGE_PROVIDER_MIN_SIZE - 1, GRCORE_PAGE_PROVIDER_MIN_SIZE + 1,
      sizeof(GRCORE_PageProvider) + 1};
  for (size_t size : bad) {
    FakePages fake(w.pages());
    fake.vtable.size = size;
    EXPECT_EQ(compile_over(&fake.vtable), GRJIT_ERR_INVALID) << "size " << size;
    EXPECT_EQ(fake.maps, 0) << "size " << size;
    EXPECT_EQ(fake.protects, 0) << "size " << size;
  }
  // Control: a provider at its full size is accepted, so the refusals above are
  // for the size.
  FakePages fake(w.pages());
  EXPECT_EQ(compile_over(&fake.vtable), GRJIT_OK);
}

TEST(PageSizeJit, AProviderFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  FakePages fake(w.pages());
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRCORE_PageProvider) + extra));
  std::memcpy(block, &fake.vtable, sizeof(GRCORE_PageProvider));
  std::memset(block + sizeof(GRCORE_PageProvider), 0xA5, extra);
  auto * p = reinterpret_cast<GRCORE_PageProvider *>(block);
  p->size = sizeof(GRCORE_PageProvider) + extra;
  EXPECT_EQ(compile_over(p), GRJIT_OK);
  EXPECT_EQ(fake.protects, 1) << "the members it knows are used";
  std::free(block);
}

GRJIT_TEST_MAIN()
