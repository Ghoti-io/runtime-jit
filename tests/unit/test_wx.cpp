/**
 * @file
 *
 * W^X (AD-13): JIT memory is mapped read-write, filled, then made
 * read-execute, and is never writable and executable at once. Proved from the
 * process's own mappings, so it is Linux-only; on another target the backend
 * is absent and the test says so.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

struct Mapping {
  uintptr_t lo, hi;
  std::string perms;
};

std::vector<Mapping> maps() {
  std::vector<Mapping> out;
  FILE * f = std::fopen("/proc/self/maps", "r");
  EXPECT_NE(f, nullptr);
  if (f == nullptr) {
    return out;
  }
  char line[1024];
  while (std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long lo, hi;
    char perms[8];
    if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3) {
      out.push_back({lo, hi, perms});
    }
  }
  std::fclose(f);
  return out;
}

size_t writable_and_executable() {
  size_t n = 0;
  for (const Mapping & m : maps()) {
    if (m.perms.size() >= 3 && m.perms[1] == 'w' && m.perms[2] == 'x') {
      n++;
    }
  }
  return n;
}

std::string perms_at(uintptr_t address) {
  for (const Mapping & m : maps()) {
    if (address >= m.lo && address < m.hi) {
      return m.perms;
    }
  }
  return "";
}

/* Valgrind presents the client a /proc/self/maps of its own making, in which
 * its tool's mappings are writable and executable and a client's read-execute
 * mapping is reported as rwx, so that file cannot answer the question under
 * it. The tests that read it say so and step aside there; the test that
 * writes to the code and requires a SIGSEGV does not read it, and still runs
 * (Valgrind enforces the protection it was asked for, as the kernel does). */
bool under_valgrind() {
  const char * preload = std::getenv("LD_PRELOAD");
  return preload != nullptr && std::strstr(preload, "vgpreload") != nullptr;
}

#define GRJIT_MAPS_UNAVAILABLE_UNDER_VALGRIND()                                \
  if (under_valgrind()) {                                                      \
    GTEST_SKIP() << "/proc/self/maps is Valgrind's, not the kernel's";         \
  }

GRJIT_Function * tiny() {
  B b("tiny");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.ret(V(x));
  return b.finish();
}

} // namespace

TEST(WX, NoMappingIsEverWritableAndExecutableBeforeDuringOrAfter) {
  GRJIT_REQUIRE_BACKEND();
  GRJIT_MAPS_UNAVAILABLE_UNDER_VALGRIND();
  JitWorld w;
  EXPECT_EQ(writable_and_executable(), 0u);
  Fn f(tiny());
  {
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    EXPECT_EQ(writable_and_executable(), 0u);
    uintptr_t at = reinterpret_cast<uintptr_t>(grjit_code_address(c.code));
    EXPECT_EQ(perms_at(at), "r-xp") << "the code mapping must be read-execute";
    EXPECT_EQ(c.run(w.ctx, {41}).out[0], 41u);
    EXPECT_EQ(writable_and_executable(), 0u);
    // A compile while another code object is alive.
    Compiled d(f, w.pages());
    ASSERT_TRUE(d);
    EXPECT_EQ(writable_and_executable(), 0u);
  }
  EXPECT_EQ(writable_and_executable(), 0u);
}

TEST(WX, DestroyUnmapsTheCodeAndReturnsTheMeter) {
  GRJIT_REQUIRE_BACKEND();
  GRJIT_MAPS_UNAVAILABLE_UNDER_VALGRIND();
  JitWorld w;
  Fn f(tiny());
  uintptr_t at;
  uint64_t bytes = w.bytes_in_use();
  {
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    at = reinterpret_cast<uintptr_t>(grjit_code_address(c.code));
    EXPECT_GT(w.bytes_in_use(), bytes);
    EXPECT_NE(perms_at(at), "");
  }
  EXPECT_EQ(w.bytes_in_use(), bytes);
  EXPECT_EQ(w.blocks_in_use(), 0u);
  EXPECT_EQ(perms_at(at), "") << "the mapping must be gone after destroy";
}

namespace {
sigjmp_buf g_fault;
void on_fault(int) {
  siglongjmp(g_fault, 1);
}
} // namespace

TEST(WX, TheCodeCannotBeWrittenAfterItIsMadeExecutable) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(tiny());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  volatile unsigned char * code = const_cast<volatile unsigned char *>(
      static_cast<const unsigned char *>(grjit_code_address(c.code)));
  unsigned char before = code[0];
  // Catch the fault in this process (a forked child would carry the whole
  // test, and a sanitizer's own handler would hide the signal from a parent).
  struct sigaction act;
  struct sigaction old;
  std::memset(&act, 0, sizeof act);
  act.sa_handler = on_fault;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_NODEFER;
  ASSERT_EQ(sigaction(SIGSEGV, &act, &old), 0);
  volatile bool faulted = false;
  if (sigsetjmp(g_fault, 1) == 0) {
    code[0] = 0x90; // must fault: the page is read-execute
  } else {
    faulted = true;
  }
  sigaction(SIGSEGV, &old, nullptr);
  EXPECT_TRUE(faulted) << "a write to the code page succeeded";
  EXPECT_EQ(code[0], before);
}

TEST(WX, ACompileThatFailsAfterMappingLeavesNoMappingAndTheMeterAtItsStart) {
  GRJIT_REQUIRE_BACKEND();
  GRJIT_MAPS_UNAVAILABLE_UNDER_VALGRIND();
  JitWorld w;
  Fn f(tiny());
  FakePages fake(w.pages());
  fake.fail_protect = true;
  uint64_t bytes = w.bytes_in_use();
  uint64_t blocks = w.blocks_in_use();
  GRJIT_CompileOptions o{};
  o.pages = &fake.vtable;
  GRJIT_Code * out = reinterpret_cast<GRJIT_Code *>(1);
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_IO);
  EXPECT_EQ(out, reinterpret_cast<GRJIT_Code *>(1));
  EXPECT_EQ(fake.maps, 1);
  EXPECT_EQ(fake.live, 0) << "the mapping must be unmapped when protect fails";
  EXPECT_EQ(fake.protects, 1);
  EXPECT_EQ(w.bytes_in_use(), bytes);
  EXPECT_EQ(w.blocks_in_use(), blocks);
  EXPECT_EQ(writable_and_executable(), 0u);
}

TEST(WX, AProviderWithNoProtectIsUnsupportedAndNothingIsMapped) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(tiny());
  FakePages fake(w.pages());
  fake.vtable.protect = nullptr;
  GRJIT_CompileOptions o{};
  o.pages = &fake.vtable;
  GRJIT_Code * out = nullptr;
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(fake.maps, 0);
  EXPECT_EQ(fake.live, 0);
}

TEST(WX, CodeMemoryIsChargedToTheContextProviderItCameFrom) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(tiny());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  EXPECT_EQ(w.bytes_in_use(), grjit_code_mapped_size(c.code));
  EXPECT_EQ(w.blocks_in_use(), 1u);
  EXPECT_GE(grcore_context_memory_peak(w.ctx), grjit_code_mapped_size(c.code));
}

#if !(defined(__x86_64__) && defined(__linux__))
TEST(WX, OnAnotherTargetTheBackendSaysItIsAbsent) {
  EXPECT_FALSE(grjit_backend_available());
  JitWorld w;
  Fn f(tiny());
  GRJIT_CompileOptions o{};
  o.pages = w.pages();
  GRJIT_Code * out = nullptr;
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_UNSUPPORTED);
}
#endif

GRJIT_TEST_MAIN()
