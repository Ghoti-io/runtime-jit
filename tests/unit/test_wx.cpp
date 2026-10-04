/**
 * @file
 *
 * W^X (AD-13): JIT memory is mapped read-write, filled, then made
 * read-execute, and is never writable and executable at once. Proved on Linux
 * from the process's own mappings (/proc/self/maps) and by catching the
 * SIGSEGV of a write to the code; on Windows x86-64 from VirtualQuery over the
 * whole address space and by catching the access violation of a write with a
 * vectored exception handler (under wine, until a Windows machine has run it).
 * On another target the backend is absent and the test says so.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <unistd.h>
#endif
#if defined(_WIN64) && defined(__x86_64__)
#include <windows.h>
#endif

namespace {
GRJIT_Function * tiny() {
  B b("tiny");
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.ret(V(x));
  return b.finish();
}
} // namespace

#if defined(__linux__)
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

#elif defined(_WIN64) && defined(__x86_64__)
namespace {

/* Every committed region of the address space whose protection is writable
 * and executable at once. The count is taken before the code exists and again
 * at each stage, and has to be what it was: the loader or the runtime may own
 * a region of that kind (the count is reported if it is not zero), and the
 * code must never add one. */
size_t writable_and_executable() {
  size_t n = 0;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const char * at = static_cast<const char *>(si.lpMinimumApplicationAddress);
  const char * end = static_cast<const char *>(si.lpMaximumApplicationAddress);
  MEMORY_BASIC_INFORMATION m;
  while (at < end && VirtualQuery(at, &m, sizeof m) == sizeof m) {
    if (m.State == MEM_COMMIT &&
        (m.Protect & (PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0) {
      n++;
    }
    const char * next = static_cast<const char *>(m.BaseAddress) + m.RegionSize;
    if (next <= at) {
      break;
    }
    at = next;
  }
  return n;
}

/* The protection of the region holding `address`, 0 if it is not committed. */
DWORD protection_at(const void * address) {
  MEMORY_BASIC_INFORMATION m;
  if (VirtualQuery(address, &m, sizeof m) != sizeof m || m.State != MEM_COMMIT) {
    return 0;
  }
  return m.Protect;
}

volatile LONG g_faulted;
volatile LONG g_fault_was_write;
LONG CALLBACK on_fault(EXCEPTION_POINTERS * e) {
  if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    g_faulted = 1;
    g_fault_was_write = e->ExceptionRecord->ExceptionInformation[0] == 1;
    /* Resume in ExitThread on the faulting thread's own stack. */
    e->ContextRecord->Rip = reinterpret_cast<DWORD64>(&ExitThread);
    e->ContextRecord->Rcx = 0;
    e->ContextRecord->Rsp = (e->ContextRecord->Rsp & ~static_cast<DWORD64>(15)) - 8 - 32;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI write_thread(void * p) {
  *static_cast<volatile unsigned char *>(p) = 0x90;
  return 0;
}

} // namespace

TEST(WX, NoMappingIsEverWritableAndExecutableBeforeDuringOrAfter) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  size_t baseline = writable_and_executable();
  Fn f(tiny());
  {
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    EXPECT_EQ(writable_and_executable(), baseline);
    const void * at = grjit_code_address(c.code);
    EXPECT_EQ(protection_at(at), static_cast<DWORD>(PAGE_EXECUTE_READ))
        << "the code mapping must be read-execute";
    EXPECT_EQ(c.run(w.ctx, {41}).out[0], 41u);
    EXPECT_EQ(writable_and_executable(), baseline);
    Compiled d(f, w.pages());
    ASSERT_TRUE(d);
    EXPECT_EQ(writable_and_executable(), baseline);
  }
  EXPECT_EQ(writable_and_executable(), baseline);
}

TEST(WX, DestroyUnmapsTheCodeAndReturnsTheMeter) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(tiny());
  const void * at;
  uint64_t bytes = w.bytes_in_use();
  {
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    at = grjit_code_address(c.code);
    EXPECT_GT(w.bytes_in_use(), bytes);
    EXPECT_NE(protection_at(at), 0u);
  }
  EXPECT_EQ(w.bytes_in_use(), bytes);
  EXPECT_EQ(w.blocks_in_use(), 0u);
  EXPECT_EQ(protection_at(at), 0u) << "the mapping must be gone after destroy";
}

TEST(WX, TheCodeCannotBeWrittenAfterItIsMadeExecutable) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(tiny());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  void * code = const_cast<void *>(grjit_code_address(c.code));
  unsigned char before = *static_cast<volatile unsigned char *>(code);
  /* The write is made on a thread of its own, and a vectored handler ends that
   * thread when it faults, so the fault is observed and the test goes on. */
  PVOID handler = AddVectoredExceptionHandler(1, on_fault);
  g_faulted = 0;
  g_fault_was_write = 0;
  HANDLE t = CreateThread(nullptr, 0, write_thread, code, 0, nullptr);
  ASSERT_NE(t, nullptr);
  WaitForSingleObject(t, 10000);
  CloseHandle(t);
  RemoveVectoredExceptionHandler(handler);
  EXPECT_EQ(g_faulted, 1) << "a write to the code page succeeded";
  EXPECT_EQ(g_fault_was_write, 1);
  EXPECT_EQ(*static_cast<volatile unsigned char *>(code), before);
}

TEST(WX, ACompileThatFailsAfterMappingLeavesNoMappingAndTheMeterAtItsStart) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(tiny());
  FakePages fake(w.pages());
  fake.fail_protect = true;
  uint64_t bytes = w.bytes_in_use();
  uint64_t blocks = w.blocks_in_use();
  size_t baseline = writable_and_executable();
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
  EXPECT_EQ(writable_and_executable(), baseline);
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

#else // neither Linux nor Windows x86-64
/* The mappings are read from /proc/self/maps or VirtualQuery and the write to
 * read-execute code is caught by a signal or a vectored handler, none of which
 * exists on this target. One skipped test stands in for them, so that the
 * suite's count shows they were not run instead of showing nothing. */
TEST(WX, IsProvedFromProcSelfMapsOrVirtualQueryAndSoOnlyOnLinuxAndWindowsX86_64) {
  GTEST_SKIP() << "W^X is proved from /proc/self/maps and SIGSEGV (Linux) or "
                  "VirtualQuery and a vectored handler (Windows x86-64)";
}
#endif

#if !GRJIT_TEST_HAVE_BACKEND
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
