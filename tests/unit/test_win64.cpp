/**
 * @file
 *
 * The Windows x86-64 flavour of the x86-64 backend: the Microsoft calling
 * convention for the entry and for helper calls (arguments in rcx, rdx, r8, r9
 * and two stack words above a 32-byte shadow space, only caller-saved scratch
 * registers), and the unwind information (`UNWIND_INFO` and a
 * `RUNTIME_FUNCTION`) kept in the code's own pages and registered with the
 * operating system.
 *
 * The flavour is an architecture value of its own (`GRJIT_ARCH_X86_64_WIN64`),
 * so what it emits, the unwind bytes it builds, and where it puts them and
 * when it registers them are all tested here on every host, with the bytes of
 * a function pinned. Only *running* it needs Windows: the tests at the end are
 * compiled on Windows x86-64 alone (under wine, until a Windows machine has
 * run them), and the callee-saved test also runs the SysV flavour on Linux.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../ir_gen.h"

#include "../../src/code/code_internal.h"
#include "../../src/x86_64/asm_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>

#if defined(_WIN64) && defined(__x86_64__)
#include <windows.h>
#define GRJIT_TEST_WIN64 1
#endif

#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef _WIN32
/* Whether the host has an objdump that disassembles x86-64: it runs, and it
 * lists the machine. A host whose objdump cannot (a cross binutils for another
 * architecture, or a machine where nothing x86-64 can be executed, such as a
 * test binary run under an emulator for another instruction set) cannot give
 * the independent reading these tests exist for; they are reported as skipped,
 * with the reason, and never as passed or as failed for the tool. */
bool host_objdump_reads_x86_64(std::string * why) {
  /* Ask it the question the tests will: disassemble one NOP as x86-64. */
  char path[] = "/tmp/grjit-w64-probe-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    *why = "no temporary file could be made";
    return false;
  }
  const unsigned char nop = 0x90;
  bool wrote = write(fd, &nop, 1) == 1;
  close(fd);
  std::string text;
  int status = -1;
  if (wrote) {
    std::string cmd = std::string("objdump -D -b binary -mi386:x86-64 -M intel ") + path + " 2>&1";
    FILE * p = popen(cmd.c_str(), "r");
    if (p != nullptr) {
      char buf[512];
      while (fgets(buf, sizeof buf, p) != nullptr) {
        text += buf;
      }
      status = pclose(p);
    }
  }
  unlink(path);
  if (status != 0) {
    *why = "objdump did not run or does not know the machine (exit status " + std::to_string(status) + ")";
    return false;
  }
  if (text.find("nop") == std::string::npos) {
    *why = "objdump ran but did not disassemble a NOP as x86-64";
    return false;
  }
  return true;
}
#endif

namespace {

constexpr uint32_t kRequestOffset = 0x40;

/* The registers a Win64 callee must preserve and this backend must therefore
 * never touch: rbx, rsi, rdi, r12-r15 (rbp is saved and restored by the
 * prologue and epilogue and is the frame register). */
constexpr uint32_t kCalleeSaved = (1u << GRJIT_RBX) | (1u << GRJIT_RSI) | (1u << GRJIT_RDI) |
    (1u << GRJIT_R12) | (1u << GRJIT_R13) | (1u << GRJIT_R14) | (1u << GRJIT_R15);

/* The only scratch registers the Win64 flavour may name, beside rsp and rbp. */
constexpr uint32_t kScratch = (1u << GRJIT_RAX) | (1u << GRJIT_RCX) | (1u << GRJIT_RDX) |
    (1u << GRJIT_R8) | (1u << GRJIT_R9) | (1u << GRJIT_R10) | (1u << GRJIT_R11);

using Bytes = std::vector<uint8_t>;

struct Emit {
  GRJIT_Emitted e{};
  GRJIT_Result result = GRJIT_OK;
  explicit Emit(const GRJIT_Function * f, GRJIT_Arch arch = GRJIT_ARCH_X86_64_WIN64,
      const GRJIT_Limits * limits = nullptr, GRJIT_EntryHook hook = nullptr) {
    result = grjit_emit_for(arch, f, grjit_allocator_default(), limits, hook, kRequestOffset, &e);
  }
  Emit(const Emit &) = delete;
  Emit & operator=(const Emit &) = delete;
  ~Emit() { grjit_emitted_free(&e); }
  bool ok() const { return result == GRJIT_OK; }
  Bytes bytes() const { return Bytes(e.bytes, e.bytes + e.size); }
  /* Whether the code holds `want` anywhere, and where first. */
  long find(const Bytes & want, size_t from = 0) const {
    if (e.size < want.size()) {
      return -1;
    }
    for (size_t i = from; i + want.size() <= e.size; i++) {
      if (std::memcmp(e.bytes + i, want.data(), want.size()) == 0) {
        return static_cast<long>(i);
      }
    }
    return -1;
  }
  size_t count(const Bytes & want) const {
    size_t n = 0;
    for (long at = find(want); at >= 0; at = find(want, static_cast<size_t>(at) + 1)) {
      n++;
    }
    return n;
  }
};

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes & p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

Bytes le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
      static_cast<uint8_t>(v >> 24)};
}

const GRJIT_PollHelper kFakePoll = reinterpret_cast<GRJIT_PollHelper>(0x20000);
const void * const kFakeHelper6 = reinterpret_cast<const void *>(0x10600);

/* f(a0..a5): poll; r = helper(a0..a5) (six arguments); guard(r != 0) with a
 * two-slot state; return r. Eight registers, so the base frame is 96 bytes and
 * the Win64 frame 144. */
GRJIT_Function * six_calls_a_poll_and_a_guard() {
  B b("w64", 2);
  GRJIT_VReg a[6];
  for (auto & v : a) {
    v = b.param(GRJIT_TYPE_I64);
  }
  GRJIT_VReg r = b.reg(), c = b.reg();
  b.poll_helper(kFakePoll);
  b.at(b.block());
  b.poll({4, 5}, {grjit_frame_slot_vreg(a[0]), grjit_frame_slot_vreg(a[1])});
  b.call(r, kFakeHelper6, {V(a[0]), V(a[1]), V(a[2]), V(a[3]), V(a[4]), V(a[5])});
  b.cmp(GRJIT_CMP_NE, c, V(r), I(0));
  b.guard(V(c), {6, 7}, {grjit_frame_slot_vreg(a[0]), grjit_frame_slot_vreg(r)});
  b.ret(V(r));
  return b.finish();
}

/* A function of `registers` registers and nothing else: the frame is the
 * point. */
GRJIT_Function * frame_of(size_t registers, const GRJIT_Limits * limits = nullptr) {
  B b("frame", 0, limits);
  GRJIT_VReg first = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg last = first;
  for (size_t i = 1; i < registers; i++) {
    last = b.reg();
  }
  b.at(b.block());
  b.cnst(last, 5);
  b.ret(V(last));
  return b.finish();
}

/* The base frame of `registers` registers, as the library computes it, and the
 * Win64 frame: that plus the 48-byte outgoing area. */
uint64_t base_frame(size_t registers) {
  return ((registers + 3) * 8 + 15) / 16 * 16;
}

} // namespace

/* ---- What is emitted --------------------------------------------------------------------------- */

TEST(Win64Shape, ThePrologueTheOutgoingAreaAndTheEntryRegistersAreAsPinned) {
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok()) << grjit_result_string(em.result);
  /* push rbp; mov rbp,rsp; sub rsp,144 (96 of frame and 48 of outgoing area);
   * then the context from rcx, `out` from r8 and `args` from rdx into their
   * fixed slots; then parameter 0 through r10 (rsi is callee-saved here). */
  Bytes want = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x89,
      0x4D, 0xF8, 0x4C, 0x89, 0x45, 0xF0, 0x48, 0x89, 0x55, 0xE8, 0x4C, 0x8B, 0x55, 0xE8, 0x49,
      0x8B, 0x02, 0x48, 0x89, 0x45, 0xE0};
  ASSERT_GE(em.e.size, want.size());
  EXPECT_EQ(Bytes(em.e.bytes, em.e.bytes + want.size()), want);
  EXPECT_EQ(base_frame(8), 96u);
  EXPECT_EQ(em.e.prologue.alloc_bytes, 96u + 48u) << "the outgoing area is 48 bytes";
  EXPECT_EQ(em.e.prologue.push_end, 1u);
  EXPECT_EQ(em.e.prologue.setfp_end, 4u);
  EXPECT_EQ(em.e.prologue.alloc_end, 11u);
  EXPECT_EQ(em.e.meta.meta.frame_bytes, 96u) << "the metadata names the base frame, as on Linux";
}

TEST(Win64Shape, ASixArgumentCallStoresTheFifthAndSixthAboveTheShadowSpaceAndLoadsFourRegisters) {
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  /* Arguments 5 and 6 (slots -64 and -72) through rax into [rsp+32] and
   * [rsp+40]; then rcx, rdx, r8, r9 from slots -32, -40, -48, -56; then the
   * address in eax and `call rax`. */
  Bytes call = {0x48, 0x8B, 0x45, 0xC0, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0x45, 0xB8,
      0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8B, 0x4D, 0xE0, 0x48, 0x8B, 0x55, 0xD8, 0x4C, 0x8B,
      0x45, 0xD0, 0x4C, 0x8B, 0x4D, 0xC8, 0xB8, 0x00, 0x06, 0x01, 0x00, 0xFF, 0xD0};
  EXPECT_EQ(em.count(call), 1u);
}

TEST(Win64Shape, ThePollHelperIsCalledWithContextFunctionAndOffsetInRcxRdxAndR8) {
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  /* mov rcx,[rbp-8]; mov edx,4; mov r8d,5; mov eax,<helper>; call rax; and the
   * answer tested as 32 bits. */
  Bytes stub = {0x48, 0x8B, 0x4D, 0xF8, 0xBA, 0x04, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x05, 0x00,
      0x00, 0x00, 0xB8, 0x00, 0x00, 0x02, 0x00, 0xFF, 0xD0, 0x89, 0xC0, 0x48, 0x85, 0xC0};
  EXPECT_EQ(em.count(stub), 1u);
}

TEST(Win64Shape, TheEntryHookIsCalledWithTheContextStillInRcx) {
  B b("hook", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.ret(V(x));
  Fn f(b.finish());
  auto hook = reinterpret_cast<GRJIT_EntryHook>(0x30000);
  Emit em(f, GRJIT_ARCH_X86_64_WIN64, nullptr, hook);
  ASSERT_TRUE(em.ok());
  /* The three entry registers are stored, then the hook is called with rcx
   * untouched: the next instructions are the address and the call. */
  Bytes stores_then_call = {0x48, 0x89, 0x4D, 0xF8, 0x4C, 0x89, 0x45, 0xF0, 0x48, 0x89, 0x55,
      0xE8, 0xB8, 0x00, 0x00, 0x03, 0x00, 0xFF, 0xD0};
  EXPECT_EQ(em.count(stores_then_call), 1u);
}

TEST(Win64Shape, EveryExitUsesTheEpilogueTheWindowsUnwinderRecognises) {
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  /* lea rsp,[rbp]; pop rbp; ret: the return, the guard's exit and the refusal. */
  EXPECT_EQ(em.count({0x48, 0x8D, 0x65, 0x00, 0x5D, 0xC3}), 3u);
  EXPECT_EQ(em.count({0xC9}), 0u) << "no `leave`";
}

TEST(Win64Shape, TheGuardsExitStillBeginsByLoadingOutIntoRdx) {
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  const GRCORE_CodeSite * site = nullptr;
  for (size_t i = 0; i < em.e.meta.meta.site_count; i++) {
    if (em.e.meta.meta.sites[i].kind == GRCORE_SITE_GUARD) {
      site = &em.e.meta.meta.sites[i];
    }
  }
  ASSERT_NE(site, nullptr);
  ASSERT_LT(site->code_offset + 4u, em.e.size);
  EXPECT_EQ(Bytes(em.e.bytes + site->code_offset, em.e.bytes + site->code_offset + 4),
      (Bytes{0x48, 0x8B, 0x55, 0xF0}));
}

TEST(Win64Shape, TheSysVFlavourOfTheSameFunctionIsUntouched) {
  Fn f(six_calls_a_poll_and_a_guard());
  Emit sysv(f, GRJIT_ARCH_X86_64);
  ASSERT_TRUE(sysv.ok());
  /* push rbp; mov rbp,rsp; sub rsp,96; rdi, rdx, rsi stored; `leave; ret`. */
  Bytes want = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x89, 0x7D, 0xF8, 0x48,
      0x89, 0x55, 0xF0, 0x48, 0x89, 0x75, 0xE8};
  EXPECT_EQ(Bytes(sysv.e.bytes, sysv.e.bytes + want.size()), want);
  EXPECT_EQ(sysv.count({0xC9, 0xC3}), 3u);
  EXPECT_EQ(sysv.e.prologue.alloc_bytes, 96u) << "no outgoing area on SysV";
}

TEST(Win64Shape, AFrameOverThePageSizeIsProbedADownwardsPageAtATimeBeforeRspMoves) {
  /* 600 registers: a base frame of 4816 and a Win64 frame of 4864. */
  Fn large(frame_of(600));
  Emit em(large);
  ASSERT_TRUE(em.ok());
  uint64_t n = base_frame(600) + 48;
  ASSERT_GT(n + 8, 4096u);
  ASSERT_EQ(em.e.prologue.alloc_bytes, n);
  /* lea r10,[rsp-N]; lea r11,[rsp+8]; then the loop: lea r11,[r11-4096];
   * cmp r11,r10; jbe tail; test [r11],al; jmp loop; tail: test [r10],al;
   * and only then `sub rsp,N`. */
  Bytes probe = cat({{0x4C, 0x8D, 0x94, 0x24}, le32(static_cast<uint32_t>(-static_cast<int64_t>(n))),
      {0x4C, 0x8D, 0x5C, 0x24, 0x08}, {0x4D, 0x8D, 0x9B, 0x00, 0xF0, 0xFF, 0xFF},
      {0x4D, 0x39, 0xD3}, {0x0F, 0x86}, le32(5), {0x41, 0x84, 0x03}, {0xEB, 0xEB},
      {0x41, 0x84, 0x02}, {0x48, 0x81, 0xEC}, le32(static_cast<uint32_t>(n))});
  ASSERT_GE(em.e.size, 4 + probe.size());
  EXPECT_EQ(Bytes(em.e.bytes + 4, em.e.bytes + 4 + probe.size()), probe);
  EXPECT_EQ(em.e.prologue.setfp_end, 4u);
  EXPECT_EQ(em.e.prologue.alloc_end, 4 + probe.size());
  EXPECT_LE(em.e.prologue.alloc_end, 255u) << "SizeOfProlog is a byte";
  /* A frame that fits the page is not probed, and neither is SysV's. */
  Fn modest(frame_of(100));
  Emit sm(modest);
  ASSERT_TRUE(sm.ok());
  EXPECT_EQ(sm.count({0x4D, 0x8D, 0x9B, 0x00, 0xF0, 0xFF, 0xFF}), 0u);
  Emit sysv(large, GRJIT_ARCH_X86_64);
  ASSERT_TRUE(sysv.ok());
  EXPECT_EQ(sysv.count({0x4D, 0x8D, 0x9B, 0x00, 0xF0, 0xFF, 0xFF}), 0u);
}

TEST(Win64Shape, TheProbeThresholdIsExactlyWhereTheFramePlusAReturnAddressPassesAPage) {
  const Bytes lea_page = {0x4D, 0x8D, 0x9B, 0x00, 0xF0, 0xFF, 0xFF};
  /* 501 registers: a Win64 frame of 4080, and 4080 + 8 fits the page. */
  Fn below(frame_of(501));
  Emit b(below);
  ASSERT_TRUE(b.ok());
  ASSERT_EQ(b.e.prologue.alloc_bytes, 4080u);
  EXPECT_EQ(b.count(lea_page), 0u);
  /* 502 registers: 4096, and 4096 + 8 passes it. */
  Fn above(frame_of(502));
  Emit a(above);
  ASSERT_TRUE(a.ok());
  ASSERT_EQ(a.e.prologue.alloc_bytes, 4096u);
  EXPECT_EQ(a.count(lea_page), 1u);
}

TEST(Win64Shape, TheProbeLoopDisassemblesAsTheSequenceItIsMeantToBe) {
#ifdef _WIN32
  GTEST_SKIP() << "needs POSIX mkstemp/popen and a host objdump";
#else
  std::string why;
  if (!host_objdump_reads_x86_64(&why)) {
    GTEST_SKIP() << "no objdump that reads x86-64 here: " << why;
  }
  Fn large(frame_of(600));
  Emit em(large);
  ASSERT_TRUE(em.ok());
  char path[] = "/tmp/grjit-w64-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  size_t prologue = em.e.prologue.alloc_end;
  ASSERT_EQ(write(fd, em.e.bytes, prologue), static_cast<ssize_t>(prologue));
  close(fd);
  std::string cmd = std::string("objdump -D -b binary -mi386:x86-64 -M intel ") + path + " 2>&1";
  FILE * p = popen(cmd.c_str(), "r");
  ASSERT_NE(p, nullptr);
  std::string text;
  char buf[512];
  while (fgets(buf, sizeof buf, p) != nullptr) {
    text += buf;
  }
  int status = pclose(p);
  unlink(path);
  ASSERT_EQ(status, 0) << "objdump is not available or failed; on the gated target that is a failure:\n"
                       << text;
  /* In order: the final rsp, the entry rsp, one page down, the comparison, the
   * exit, the probe, the jump back, the probe of the final rsp, the allocation. */
  const char * in_order[] = {"push   rbp", "mov    rbp,rsp", "lea    r10,[rsp-0x", "lea    r11,[rsp+0x8]",
      "lea    r11,[r11-0x1000]", "cmp    r11,r10", "jbe", "test   BYTE PTR [r11],al", "jmp",
      "test   BYTE PTR [r10],al", "sub    rsp,0x"};
  size_t at = 0;
  for (const char * line : in_order) {
    size_t found = text.find(line, at);
    ASSERT_NE(found, std::string::npos) << line << " not found in order in:\n" << text;
    at = found + 1;
  }
  EXPECT_EQ(text.find("(bad)"), std::string::npos) << text;
#endif
}

TEST(Win64Shape, TheFrameCapAppliesToTheBaseFrameExactlyAsOnLinux) {
  /* 125 registers is a base frame of exactly 1024 bytes; one more is 1040. */
  GRJIT_Limits limits;
  grjit_limits_default(&limits);
  limits.max_frame_bytes = 1024;
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_X86_64_WIN64}) {
    Fn at(frame_of(125));
    Emit ok(at, arch, &limits);
    EXPECT_TRUE(ok.ok()) << arch << ": " << grjit_result_string(ok.result);
    Fn over(frame_of(126));
    Emit no(over, arch, &limits);
    EXPECT_EQ(no.result, GRJIT_ERR_LIMIT) << arch;
  }
  Fn at(frame_of(125));
  Emit win(at, GRJIT_ARCH_X86_64_WIN64, &limits);
  ASSERT_TRUE(win.ok());
  EXPECT_EQ(win.e.prologue.alloc_bytes, 1024u + 48u) << "the outgoing area is not charged to the cap";
}

/* ---- The registers --------------------------------------------------------------------------- */

namespace {
const void * const kHelpers[7] = {reinterpret_cast<const void *>(0x10000),
    reinterpret_cast<const void *>(0x10100), reinterpret_cast<const void *>(0x10200),
    reinterpret_cast<const void *>(0x10300), reinterpret_cast<const void *>(0x10400),
    reinterpret_cast<const void *>(0x10500), reinterpret_cast<const void *>(0x10600)};
} // namespace

TEST(Win64Registers, NoCalleeSavedRegisterIsEncodedAndOnlyCallerSavedScratchIsUsed) {
  uint32_t seen = 0;
  for (uint64_t seed = 1; seed <= 2000; seed++) {
    irgen::Gen gen(seed, kHelpers, kFakePoll);
    Fn f(gen.build());
    Emit em(f);
    ASSERT_TRUE(em.ok()) << "seed " << seed;
    EXPECT_EQ(em.e.regs_used & kCalleeSaved, 0u)
        << "seed " << seed << ": a callee-saved register (rbx, rsi, rdi, r12-r15) was encoded";
    EXPECT_EQ(em.e.regs_used & ~(kScratch | (1u << GRJIT_RSP) | (1u << GRJIT_RBP)), 0u) << seed;
    seen |= em.e.regs_used;
  }
  /* A frame of more than a page, whose prologue probes the stack with r10 and
   * r11: the generated functions are all small. */
  Fn large(frame_of(600));
  Emit probed(large);
  ASSERT_TRUE(probed.ok());
  EXPECT_EQ(probed.e.regs_used & kCalleeSaved, 0u);
  EXPECT_EQ(probed.e.regs_used & ~(kScratch | (1u << GRJIT_RSP) | (1u << GRJIT_RBP)), 0u);
  seen |= probed.e.regs_used;
  /* The scan is not vacuous: every scratch register is used by some function. */
  EXPECT_EQ(seen & kScratch, kScratch);
}

TEST(Win64Registers, TheSysVFlavourDoesNameRsiAndRdiSoTheScanCanFail) {
  /* The control: the same functions in the SysV flavour use rsi and rdi (the
   * arguments), which are exactly what the test above forbids. */
  uint32_t seen = 0;
  for (uint64_t seed = 1; seed <= 200; seed++) {
    irgen::Gen gen(seed, kHelpers, kFakePoll);
    Fn f(gen.build());
    Emit em(f, GRJIT_ARCH_X86_64);
    ASSERT_TRUE(em.ok());
    seen |= em.e.regs_used;
  }
  EXPECT_NE(seen & (1u << GRJIT_RSI), 0u);
  EXPECT_NE(seen & (1u << GRJIT_RDI), 0u);
}

TEST(Win64Registers, TheDisassemblerAgreesThatNoCalleeSavedRegisterAppears) {
#ifdef _WIN32
  GTEST_SKIP() << "needs POSIX mkstemp/popen and a host objdump";
#else
  std::string why;
  if (!host_objdump_reads_x86_64(&why)) {
    GTEST_SKIP() << "no objdump that reads x86-64 here: " << why;
  }
  /* Independent of the assembler's own record: the bytes of 200 functions,
   * read back by objdump. */
  Bytes blob;
  for (uint64_t seed = 1; seed <= 200; seed++) {
    irgen::Gen gen(seed, kHelpers, kFakePoll);
    Fn f(gen.build());
    Emit em(f);
    ASSERT_TRUE(em.ok());
    blob.insert(blob.end(), em.e.bytes, em.e.bytes + em.e.size);
  }
  char path[] = "/tmp/grjit-w64r-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(write(fd, blob.data(), blob.size()), static_cast<ssize_t>(blob.size()));
  close(fd);
  std::string cmd = std::string("objdump -D -b binary -mi386:x86-64 -M intel ") + path + " 2>&1";
  FILE * p = popen(cmd.c_str(), "r");
  ASSERT_NE(p, nullptr);
  std::string text;
  char buf[512];
  while (fgets(buf, sizeof buf, p) != nullptr) {
    text += buf;
  }
  int status = pclose(p);
  unlink(path);
  ASSERT_EQ(status, 0) << "objdump is not available or failed; on the gated target that is a failure";
  /* Only the instruction text is read: after the second tab of each line,
   * a word at a time (a register name is a whole word). */
  const std::set<std::string> forbidden = {"rbx", "ebx", "bx", "bl", "rsi", "esi", "si", "sil",
      "rdi", "edi", "di", "dil", "r12", "r12d", "r12w", "r12b", "r13", "r13d", "r13w", "r13b",
      "r14", "r14d", "r14w", "r14b", "r15", "r15d", "r15w", "r15b"};
  size_t instructions = 0;
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? text.size() : nl + 1;
    size_t colon = line.find(":\t");
    if (colon == std::string::npos) {
      continue;
    }
    size_t second = line.find('\t', colon + 2);
    if (second == std::string::npos) {
      continue;
    }
    std::string insn = line.substr(second + 1);
    instructions++;
    for (size_t k = 0; k < insn.size();) {
      if (!std::isalnum(static_cast<unsigned char>(insn[k]))) {
        k++;
        continue;
      }
      size_t e = k;
      while (e < insn.size() && std::isalnum(static_cast<unsigned char>(insn[e]))) {
        e++;
      }
      EXPECT_EQ(forbidden.count(insn.substr(k, e - k)), 0u) << insn;
      k = e;
    }
  }
  EXPECT_GT(instructions, 10000u) << "the scan must have read the code";
#endif
}

/* ---- The unwind information ----------------------------------------------------------------- */

namespace {

struct Unwind {
  bool ok = false;
  uint8_t version = 0, flags = 0, size_of_prolog = 0, count = 0, frame_reg = 0, frame_off = 0;
  uint64_t alloc = 0;
  uint8_t alloc_at = 0, setfp_at = 0, push_at = 0;
  int alloc_slots = 0;
  bool set_fpreg = false, push_rbp = false;
  size_t size = 0;
};

/* An independent reading of the bytes, written from the Microsoft x64 unwind
 * information format and not from the builder. */
Unwind decode(const uint8_t * b, size_t size) {
  Unwind u;
  if (size < 4) {
    return u;
  }
  u.version = b[0] & 7;
  u.flags = b[0] >> 3;
  u.size_of_prolog = b[1];
  u.count = b[2];
  u.frame_reg = b[3] & 15;
  u.frame_off = b[3] >> 4;
  size_t slots = (static_cast<size_t>(u.count) + 1) & ~static_cast<size_t>(1);
  u.size = 4 + 2 * slots;
  if (size < u.size) {
    return u;
  }
  for (size_t i = 0; i < u.count;) {
    uint8_t off = b[4 + 2 * i];
    uint8_t op = b[5 + 2 * i] & 15;
    uint8_t info = b[5 + 2 * i] >> 4;
    switch (op) {
      case 0: // PUSH_NONVOL
        if (info == 5) {
          u.push_rbp = true;
          u.push_at = off;
        }
        i += 1;
        break;
      case 1: // ALLOC_LARGE
        if (info == 0) {
          u.alloc = 8ull * (b[6 + 2 * i] | (b[7 + 2 * i] << 8));
          u.alloc_slots = 2;
          i += 2;
        } else {
          u.alloc = static_cast<uint64_t>(b[6 + 2 * i]) | (static_cast<uint64_t>(b[7 + 2 * i]) << 8) |
              (static_cast<uint64_t>(b[8 + 2 * i]) << 16) | (static_cast<uint64_t>(b[9 + 2 * i]) << 24);
          u.alloc_slots = 3;
          i += 3;
        }
        u.alloc_at = off;
        break;
      case 2: // ALLOC_SMALL
        u.alloc = 8ull * info + 8;
        u.alloc_slots = 1;
        u.alloc_at = off;
        i += 1;
        break;
      case 3: // SET_FPREG
        u.set_fpreg = true;
        u.setfp_at = off;
        i += 1;
        break;
      default:
        return u;
    }
  }
  u.ok = true;
  return u;
}

} // namespace

TEST(Win64Unwind, TheBytesOfAFrameAreExactlyTheVersionOneLayout) {
  uint8_t out[GRJIT_UNWIND_INFO_MAX];
  /* A small allocation (80 bytes, three codes, one slot of padding). */
  GRJIT_Prologue p{1, 4, 8, 80};
  ASSERT_EQ(grjit_unwind_info_build(&p, out), 12u);
  EXPECT_EQ(Bytes(out, out + 12), (Bytes{0x01, 0x08, 0x03, 0x05,
                                      0x08, 0x92,    // ALLOC_SMALL, (80 - 8) / 8 = 9
                                      0x04, 0x03,    // SET_FPREG
                                      0x01, 0x50,    // PUSH_NONVOL rbp
                                      0x00, 0x00})); // padding
  /* The emitter's own: 144 bytes, ALLOC_LARGE with a 16-bit count (144 / 8 = 18). */
  GRJIT_Prologue q{1, 4, 11, 144};
  ASSERT_EQ(grjit_unwind_info_build(&q, out), 12u);
  EXPECT_EQ(Bytes(out, out + 12), (Bytes{0x01, 0x0B, 0x04, 0x05,
                                      0x0B, 0x01, 0x12, 0x00, // ALLOC_LARGE, info 0, 18 units
                                      0x04, 0x03, 0x01, 0x50}));
  /* Past 512 KiB: ALLOC_LARGE with a 32-bit size, five codes and a padded sixth. */
  GRJIT_Prologue r{1, 4, 11, 1048624};
  ASSERT_EQ(grjit_unwind_info_build(&r, out), 16u);
  EXPECT_EQ(Bytes(out, out + 16), (Bytes{0x01, 0x0B, 0x05, 0x05,
                                      0x0B, 0x11, 0x30, 0x00, 0x10, 0x00, // ALLOC_LARGE, info 1, 0x100030
                                      0x04, 0x03, 0x01, 0x50, 0x00, 0x00}));
}

TEST(Win64Unwind, EveryAllocationFromNothingToTwoMebibytesGetsExactlyOneFormOfTheRightSize) {
  uint8_t out[GRJIT_UNWIND_INFO_MAX];
  for (uint32_t n = 0; n <= (2u << 20); n += 8) {
    GRJIT_Prologue p{1, 4, 11, n};
    size_t size = grjit_unwind_info_build(&p, out);
    ASSERT_NE(size, 0u) << n;
    ASSERT_LE(size, static_cast<size_t>(GRJIT_UNWIND_INFO_MAX));
    ASSERT_EQ(size % 4, 0u) << n << ": the information is a whole number of DWORDs";
    Unwind u = decode(out, size);
    ASSERT_TRUE(u.ok) << n;
    ASSERT_EQ(u.version, 1) << n;
    ASSERT_EQ(u.flags, 0) << n;
    ASSERT_EQ(u.size_of_prolog, 11) << n;
    ASSERT_EQ(u.frame_reg, 5) << n << ": rbp";
    ASSERT_EQ(u.frame_off, 0) << n;
    ASSERT_EQ(u.size, size) << n;
    ASSERT_TRUE(u.set_fpreg) << n;
    ASSERT_EQ(u.setfp_at, 4) << n;
    ASSERT_TRUE(u.push_rbp) << n;
    ASSERT_EQ(u.push_at, 1) << n;
    ASSERT_EQ(u.alloc, n) << n;
    if (n == 0) {
      ASSERT_EQ(u.alloc_slots, 0);
    } else {
      ASSERT_EQ(u.alloc_at, 11) << n;
      int form = n <= 128 ? 1 : (n / 8 <= 0xFFFF ? 2 : 3);
      ASSERT_EQ(u.alloc_slots, form) << n;
    }
    ASSERT_EQ(u.count, (n == 0 ? 0 : u.alloc_slots) + 2) << n;
  }
}

TEST(Win64Unwind, APrologueThatCannotBeDescribedIsRefused) {
  uint8_t out[GRJIT_UNWIND_INFO_MAX];
  GRJIT_Prologue odd{1, 4, 11, 81};
  EXPECT_EQ(grjit_unwind_info_build(&odd, out), 0u) << "not a multiple of 8";
  GRJIT_Prologue distant{1, 4, 256, 80};
  EXPECT_EQ(grjit_unwind_info_build(&distant, out), 0u) << "SizeOfProlog is a byte";
  GRJIT_Prologue backwards{4, 1, 11, 80};
  EXPECT_EQ(grjit_unwind_info_build(&backwards, out), 0u);
  GRJIT_Prologue none{0, 0, 0, 0};
  EXPECT_EQ(grjit_unwind_info_build(&none, out), 0u);
}

TEST(Win64Unwind, TheOffsetsNameTheEndOfEachInstructionOfThePrologueThatWasEmitted) {
  /* Frames from the smallest to the largest the default cap allows: the
   * three allocation forms, a probed and an unprobed prologue. */
  GRJIT_Limits limits;
  grjit_limits_default(&limits);
  limits.max_vregs = 140000;
  for (size_t registers : {size_t{1}, size_t{2}, size_t{7}, size_t{8}, size_t{9}, size_t{10}, size_t{100}, size_t{600},
           size_t{1000}, size_t{60000}, size_t{70000}, size_t{131069}}) {
    SCOPED_TRACE(registers);
    Fn f(frame_of(registers, &limits));
    Emit em(f, GRJIT_ARCH_X86_64_WIN64, &limits);
    ASSERT_TRUE(em.ok()) << grjit_result_string(em.result);
    const GRJIT_Prologue & p = em.e.prologue;
    uint64_t n = base_frame(registers) + 48;
    ASSERT_EQ(p.alloc_bytes, n);
    EXPECT_EQ(p.push_end, 1u);
    EXPECT_EQ(p.setfp_end, 4u);
    EXPECT_EQ(em.e.bytes[0], 0x55) << "push rbp";
    EXPECT_EQ(Bytes(em.e.bytes + 1, em.e.bytes + 4), (Bytes{0x48, 0x89, 0xE5})) << "mov rbp,rsp";
    /* The allocation is the last instruction of the prologue, and it ends at
     * alloc_end. */
    Bytes sub = n <= 127 ? Bytes{0x48, 0x83, 0xEC, static_cast<uint8_t>(n)}
                         : cat({{0x48, 0x81, 0xEC}, le32(static_cast<uint32_t>(n))});
    ASSERT_GE(p.alloc_end, sub.size());
    EXPECT_EQ(Bytes(em.e.bytes + p.alloc_end - sub.size(), em.e.bytes + p.alloc_end), sub);
    /* And the unwind information built from them is what the decoder reads
     * back, with the right form. */
    uint8_t info[GRJIT_UNWIND_INFO_MAX];
    size_t size = grjit_unwind_info_build(&p, info);
    ASSERT_NE(size, 0u);
    Unwind u = decode(info, size);
    ASSERT_TRUE(u.ok);
    EXPECT_EQ(u.size_of_prolog, p.alloc_end);
    EXPECT_EQ(u.alloc, n);
    EXPECT_EQ(u.alloc_slots, n <= 128 ? 1 : (n / 8 <= 0xFFFF ? 2 : 3));
    /* The first instruction after the prologue is the entry's first store. */
    EXPECT_EQ(Bytes(em.e.bytes + p.alloc_end, em.e.bytes + p.alloc_end + 4),
        (Bytes{0x48, 0x89, 0x4D, 0xF8}));
  }
}

/* ---- Where the unwind information lives, when it is registered, and what leaks ---------------- */

namespace {

struct OpsLog {
  std::vector<std::string> events;
  FakePages * pages = nullptr;
  bool fail_add = false;
  long protects_at_add = -1;
  long live_at_add = -1;
  long live_at_remove = -1;
  void * table = nullptr;
  uintptr_t base = 0;
  uint32_t count = 0;
  uint32_t begin = 0, end = 0, unwind = 0;
};
OpsLog g_ops;

bool fake_add(void * table, uint32_t count, uintptr_t base) {
  g_ops.events.push_back("add");
  g_ops.protects_at_add = g_ops.pages->protects;
  g_ops.live_at_add = g_ops.pages->live;
  g_ops.table = table;
  g_ops.base = base;
  g_ops.count = count;
  const uint32_t * rf = static_cast<const uint32_t *>(table);
  g_ops.begin = rf[0];
  g_ops.end = rf[1];
  g_ops.unwind = rf[2];
  return !g_ops.fail_add;
}

void fake_remove(void * table) {
  g_ops.events.push_back("remove");
  g_ops.live_at_remove = g_ops.pages->live;
  EXPECT_EQ(table, g_ops.table);
}

/* Installs the recording ops for a scope. */
struct OpsScope {
  const GRJIT_UnwindOps * previous;
  OpsScope(FakePages * pages, bool fail = false) {
    g_ops = OpsLog();
    g_ops.pages = pages;
    g_ops.fail_add = fail;
    static const GRJIT_UnwindOps ops = {fake_add, fake_remove};
    previous = grjit_unwind_set_ops(&ops);
  }
  ~OpsScope() { grjit_unwind_set_ops(previous); }
};

} // namespace

TEST(Win64Memory, TheTableAndTheInformationAreWrittenAfterTheCodeAndRegisteredAfterTheFlip) {
  JitWorld w;
  FakePages pages(w.pages());
  OpsScope scope(&pages);
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  void * mapping = nullptr;
  size_t mapped = 0;
  void * table = nullptr;
  ASSERT_EQ(grjit_memory_create(&pages.vtable, GRJIT_ARCH_X86_64_WIN64, em.e.bytes, em.e.size,
                &em.e.prologue, &mapping, &mapped, &table),
      GRJIT_OK);
  const uint8_t * m = static_cast<const uint8_t *>(mapping);
  size_t table_at = (em.e.size + 3) / 4 * 4;
  /* The code is where it was; the table is 4-byte aligned straight after it. */
  EXPECT_EQ(std::memcmp(m, em.e.bytes, em.e.size), 0);
  EXPECT_EQ(table, m + table_at);
  EXPECT_EQ(table_at % 4, 0u);
  /* One RUNTIME_FUNCTION for [0, code size), RVAs from the start of the
   * mapping, naming the UNWIND_INFO that follows it, 4-byte aligned. */
  uint32_t rf[3];
  std::memcpy(rf, m + table_at, sizeof rf);
  EXPECT_EQ(rf[0], 0u);
  EXPECT_EQ(rf[1], em.e.size);
  EXPECT_EQ(rf[2], table_at + GRJIT_RUNTIME_FUNCTION_BYTES);
  EXPECT_EQ(rf[2] % 4, 0u);
  uint8_t info[GRJIT_UNWIND_INFO_MAX];
  size_t info_size = grjit_unwind_info_build(&em.e.prologue, info);
  ASSERT_NE(info_size, 0u);
  EXPECT_EQ(std::memcmp(m + rf[2], info, info_size), 0);
  EXPECT_LE(rf[2] + info_size, mapped) << "inside the mapping";
  /* Registered once, after the flip to read-execute (one protect had run),
   * with the mapping as the base, one entry, and the mapping still there. */
  ASSERT_EQ(g_ops.events, (std::vector<std::string>{"add"}));
  EXPECT_EQ(g_ops.protects_at_add, 1);
  EXPECT_EQ(g_ops.live_at_add, 1);
  EXPECT_EQ(g_ops.base, reinterpret_cast<uintptr_t>(mapping));
  EXPECT_EQ(g_ops.count, 1u);
  EXPECT_EQ(g_ops.table, table);
  EXPECT_EQ(g_ops.begin, 0u);
  EXPECT_EQ(g_ops.end, em.e.size) << "the table was complete when it was registered";
  /* Destroy removes the registration while the pages are still mapped. */
  grjit_memory_destroy(&pages.vtable, mapping, mapped, table);
  ASSERT_EQ(g_ops.events, (std::vector<std::string>{"add", "remove"}));
  EXPECT_EQ(g_ops.live_at_remove, 1) << "deleted before the unmap";
  EXPECT_EQ(pages.live, 0);
}

TEST(Win64Memory, ARefusedRegistrationFailsTheMappingWithNothingLeftMappedOrRegistered) {
  JitWorld w;
  FakePages pages(w.pages());
  OpsScope scope(&pages, /*fail=*/true);
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  uint64_t bytes = w.bytes_in_use();
  uint64_t blocks = w.blocks_in_use();
  void * mapping = reinterpret_cast<void *>(1);
  size_t mapped = 7;
  void * table = reinterpret_cast<void *>(2);
  GRJIT_Result r = grjit_memory_create(&pages.vtable, GRJIT_ARCH_X86_64_WIN64, em.e.bytes, em.e.size,
      &em.e.prologue, &mapping, &mapped, &table);
  EXPECT_EQ(r, GRJIT_ERR_IO);
  EXPECT_EQ(g_ops.events, (std::vector<std::string>{"add"})) << "tried once, and nothing to remove";
  EXPECT_EQ(pages.live, 0) << "the mapping is unmapped";
  EXPECT_EQ(w.bytes_in_use(), bytes) << "AD-13: the meter is where it started";
  EXPECT_EQ(w.blocks_in_use(), blocks);
  EXPECT_EQ(mapping, reinterpret_cast<void *>(1)) << "outputs are written only on success";
  EXPECT_EQ(mapped, 7u);
}

TEST(Win64Memory, OnlyTheWin64FlavourWritesAndRegistersAnUnwindTable) {
  JitWorld w;
  FakePages pages(w.pages());
  OpsScope scope(&pages);
  Fn f(six_calls_a_poll_and_a_guard());
  for (GRJIT_Arch arch : {GRJIT_ARCH_X86_64, GRJIT_ARCH_ARM64}) {
    Emit em(f, arch);
    ASSERT_TRUE(em.ok());
    void * mapping = nullptr;
    size_t mapped = 0;
    ASSERT_EQ(grjit_memory_create(&pages.vtable, arch, em.e.bytes, em.e.size, nullptr, &mapping,
                  &mapped, nullptr),
        GRJIT_OK);
    EXPECT_EQ(mapped, (em.e.size + pages.vtable.page_size - 1) / pages.vtable.page_size *
                          pages.vtable.page_size);
    grjit_memory_destroy(&pages.vtable, mapping, mapped, nullptr);
  }
  EXPECT_TRUE(g_ops.events.empty());
  EXPECT_EQ(pages.live, 0);
}

TEST(Win64Memory, WithNothingToRegisterWithTheMappingIsMadeAndTheTableIsNotClaimed) {
#ifdef GRJIT_TEST_WIN64
  GTEST_SKIP() << "this target registers with the operating system";
#else
  /* A host that is not Windows has no unwinder to tell: registration says so,
   * and a Win64 mapping is still made (it can be inspected, never run). */
  JitWorld w;
  const GRJIT_UnwindOps * previous = grjit_unwind_set_ops(nullptr);
  EXPECT_EQ(grjit_unwind_register(nullptr, nullptr), GRJIT_ERR_UNSUPPORTED);
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  void * mapping = nullptr;
  size_t mapped = 0;
  void * table = reinterpret_cast<void *>(1);
  ASSERT_EQ(grjit_memory_create(w.pages(), GRJIT_ARCH_X86_64_WIN64, em.e.bytes, em.e.size,
                &em.e.prologue, &mapping, &mapped, &table),
      GRJIT_OK);
  EXPECT_EQ(table, nullptr);
  grjit_memory_destroy(w.pages(), mapping, mapped, table);
  grjit_unwind_set_ops(previous);
#endif
}

TEST(Win64Memory, AnArchitectureThatNeedsAPrologueRefusesToBeMappedWithout) {
  JitWorld w;
  Fn f(six_calls_a_poll_and_a_guard());
  Emit em(f);
  ASSERT_TRUE(em.ok());
  void * mapping = nullptr;
  size_t mapped = 0;
  void * table = nullptr;
  EXPECT_EQ(grjit_memory_create(w.pages(), GRJIT_ARCH_X86_64_WIN64, em.e.bytes, em.e.size, nullptr,
                &mapping, &mapped, &table),
      GRJIT_ERR_INVALID);
  EXPECT_EQ(w.blocks_in_use(), 0u);
}

/* ---- Running it ---------------------------------------------------------------------------- */

#if (defined(__x86_64__) && defined(__linux__)) || defined(GRJIT_TEST_WIN64)

/* A caller that fills the callee-saved registers with sentinels, calls the
 * compiled code with the target's own argument registers, and reports what the
 * registers hold on return. Top-level assembly, because C++ cannot name the
 * registers across a call: rbx and r12-r15 on SysV; also rsi and rdi on Win64. */
extern "C" uint32_t grjit_test_call_sentinels(GRJIT_EntryFn fn, void * ctx, const uint64_t * args,
    uint64_t * out, uint64_t * registers);

#if defined(GRJIT_TEST_WIN64)
asm(R"(
  .text
  .globl grjit_test_call_sentinels
grjit_test_call_sentinels:
  push %rbp
  mov %rsp, %rbp
  push %rbx
  push %rsi
  push %rdi
  push %r12
  push %r13
  push %r14
  push %r15
  sub $40, %rsp
  mov %rcx, %rax
  mov %rdx, %rcx
  mov %r8, %rdx
  mov %r9, %r8
  movabs $0x1111111111111111, %rbx
  movabs $0x2222222222222222, %rsi
  movabs $0x3333333333333333, %rdi
  movabs $0x4444444444444444, %r12
  movabs $0x5555555555555555, %r13
  movabs $0x6666666666666666, %r14
  movabs $0x7777777777777777, %r15
  call *%rax
  mov 48(%rbp), %r9
  mov %rbx, 0(%r9)
  mov %rsi, 8(%r9)
  mov %rdi, 16(%r9)
  mov %r12, 24(%r9)
  mov %r13, 32(%r9)
  mov %r14, 40(%r9)
  mov %r15, 48(%r9)
  add $40, %rsp
  pop %r15
  pop %r14
  pop %r13
  pop %r12
  pop %rdi
  pop %rsi
  pop %rbx
  pop %rbp
  ret
)");
constexpr size_t kSentinelCount = 7;
constexpr uint64_t kSentinels[kSentinelCount] = {0x1111111111111111ull, 0x2222222222222222ull,
    0x3333333333333333ull, 0x4444444444444444ull, 0x5555555555555555ull, 0x6666666666666666ull,
    0x7777777777777777ull};
const char * const kSentinelNames[kSentinelCount] = {"rbx", "rsi", "rdi", "r12", "r13", "r14", "r15"};
#else
asm(R"(
  .pushsection .text
  .globl grjit_test_call_sentinels
  .type grjit_test_call_sentinels, @function
grjit_test_call_sentinels:
  push %rbp
  mov %rsp, %rbp
  push %rbx
  push %r12
  push %r13
  push %r14
  push %r15
  push %r8
  mov %rdi, %rax
  mov %rsi, %rdi
  mov %rdx, %rsi
  mov %rcx, %rdx
  movabs $0x1111111111111111, %rbx
  movabs $0x2222222222222222, %r12
  movabs $0x3333333333333333, %r13
  movabs $0x4444444444444444, %r14
  movabs $0x5555555555555555, %r15
  call *%rax
  mov -48(%rbp), %r8
  mov %rbx, 0(%r8)
  mov %r12, 8(%r8)
  mov %r13, 16(%r8)
  mov %r14, 24(%r8)
  mov %r15, 32(%r8)
  pop %r8
  pop %r15
  pop %r14
  pop %r13
  pop %r12
  pop %rbx
  pop %rbp
  ret
  .size grjit_test_call_sentinels, .-grjit_test_call_sentinels
  .popsection
)");
constexpr size_t kSentinelCount = 5;
constexpr uint64_t kSentinels[kSentinelCount] = {0x1111111111111111ull, 0x2222222222222222ull,
    0x3333333333333333ull, 0x4444444444444444ull, 0x5555555555555555ull};
const char * const kSentinelNames[kSentinelCount] = {"rbx", "r12", "r13", "r14", "r15"};
#endif

namespace {

uint32_t g_hook_answer = 0;
uint32_t sentinel_hook(void *) { return g_hook_answer; }

uint32_t g_poll_answer = 0;
int g_poll_calls = 0;
GRCORE_RequestKind g_poll_kind = 0;
uint32_t sentinel_poll(void * ctx, uint64_t, uint64_t) {
  g_poll_calls++;
  EXPECT_EQ(grcore_context_clear_request(static_cast<GRCORE_Context *>(ctx), g_poll_kind), GRCORE_OK);
  return g_poll_answer;
}

uint64_t six_sum(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
  return a + b + c + d + e + f;
}

const GRCORE_Key kSentinelKey = GRCORE_KEY_INIT("w64", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE, nullptr,
    nullptr, nullptr, nullptr, nullptr);

/* f(x): poll; t = six_sum(x, x, x, x, x, x); guard(x != 0); return t + 1. */
GRJIT_Function * sentinel_function() {
  B b("sentinels", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg t = b.reg(), c = b.reg(), r = b.reg();
  b.poll_helper(sentinel_poll);
  b.at(b.block());
  b.poll({1, 1}, {grjit_frame_slot_vreg(x)});
  b.call(t, reinterpret_cast<const void *>(six_sum), {V(x), V(x), V(x), V(x), V(x), V(x)});
  b.cmp(GRJIT_CMP_NE, c, V(x), I(0));
  b.guard(V(c), {1, 2}, {grjit_frame_slot_vreg(x)});
  b.bin(GRJIT_OP_ADD, r, V(t), I(1));
  b.ret(V(r));
  return b.finish();
}

struct SentinelRun {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
  uint64_t registers[8] = {0};
};

SentinelRun run_with_sentinels(const Compiled & c, void * ctx, uint64_t x) {
  SentinelRun r;
  r.out.assign(grjit_code_out_words(c.code), 0xDEADBEEFu);
  uint64_t args[1] = {x};
  r.exit = grjit_test_call_sentinels(
      grjit_code_entry(c.code), ctx, args, r.out.data(), r.registers);
  return r;
}

void expect_sentinels(const SentinelRun & r, const char * what) {
  for (size_t i = 0; i < kSentinelCount; i++) {
    EXPECT_EQ(r.registers[i], kSentinels[i]) << what << ": " << kSentinelNames[i] << " was changed";
  }
}

} // namespace

TEST(CalleeSaved, TheCallersCalleeSavedRegistersSurviveAReturnADeoptARefusalAndAPoll) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  EXPECT_EQ(grcore_context_request_kind(w.ctx, &kSentinelKey, &g_poll_kind), GRCORE_OK);
  GRCORE_Port * port = nullptr;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  Fn f(sentinel_function());
  Compiled c(f, w.pages(), sentinel_hook);
  ASSERT_TRUE(c) << grjit_result_string(c.result);

  g_hook_answer = 0;
  g_poll_answer = 0;
  g_poll_calls = 0;
  /* Returned (the call and the poll's fast path ran). */
  SentinelRun ret = run_with_sentinels(c, w.ctx, 4);
  EXPECT_EQ(ret.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(ret.out[0], 4u * 6 + 1);
  expect_sentinels(ret, "return");
  /* Deoptimised by the guard. */
  SentinelRun deopt = run_with_sentinels(c, w.ctx, 0);
  EXPECT_EQ(deopt.exit, static_cast<uint32_t>(GRJIT_EXIT_DEOPT));
  expect_sentinels(deopt, "deopt");
  /* A poll whose slow path runs and lets the function go on. */
  ASSERT_EQ(grcore_port_post(port, g_poll_kind), GRCORE_OK);
  SentinelRun poll = run_with_sentinels(c, w.ctx, 4);
  EXPECT_EQ(g_poll_calls, 1);
  EXPECT_EQ(poll.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(poll.out[0], 4u * 6 + 1);
  expect_sentinels(poll, "poll");
  /* A poll whose helper refuses. */
  g_poll_answer = 9;
  ASSERT_EQ(grcore_port_post(port, g_poll_kind), GRCORE_OK);
  SentinelRun refused_by_poll = run_with_sentinels(c, w.ctx, 4);
  EXPECT_EQ(refused_by_poll.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(refused_by_poll.out[0], 9u);
  expect_sentinels(refused_by_poll, "poll refusal");
  /* The entry hook refuses. */
  g_hook_answer = 3;
  SentinelRun refused_by_hook = run_with_sentinels(c, w.ctx, 4);
  EXPECT_EQ(refused_by_hook.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(refused_by_hook.out[0], 3u);
  expect_sentinels(refused_by_hook, "hook refusal");
  grcore_port_release(port);
}

/* The largest frame the cap allows: 131069 registers is a base frame of exactly
 * 1 MiB (and, on Win64, 48 bytes more), which a Windows stack commits a page
 * at a time and so has to be probed in order. It must compile and run. */
TEST(LargestFrame, TheFrameTheCapAllowsCompilesAndRuns) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  GRJIT_Limits limits;
  grjit_limits_default(&limits);
  limits.max_vregs = 140000;
  Fn f(frame_of(131069, &limits));
  Compiled c(f, w.pages(), nullptr, &limits);
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  /* The first and the last slot of the frame are both used, and the frame is
   * walked from the top to the bottom. */
  auto r = c.run(w.ctx, {123});
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(r.out[0], 5u);
  /* One register over the cap is refused, as on Linux. */
  Fn over(frame_of(131070, &limits));
  Compiled d(over, w.pages(), nullptr, &limits);
  EXPECT_FALSE(d);
  EXPECT_EQ(d.result, GRJIT_ERR_LIMIT);
}

#endif // x86-64 Linux or Windows x86-64

#if defined(GRJIT_TEST_WIN64)

/* The helper of the six-argument test: it records rcx, rdx, r8, r9 and the two
 * stack words at [rsp+40] and [rsp+48] (rsp pointing at its return address),
 * overwrites its own shadow space, [rsp+8, rsp+40), with a pattern, and
 * returns 7. Written in assembly because C++ cannot see the registers. */
extern "C" uint64_t grjit_test_win64_probe(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t grjit_test_probe_seen[6];
extern "C" { uint64_t grjit_test_probe_seen[6]; }
asm(R"(
  .text
  .globl grjit_test_win64_probe
grjit_test_win64_probe:
  lea grjit_test_probe_seen(%rip), %rax
  mov %rcx, 0(%rax)
  mov %rdx, 8(%rax)
  mov %r8, 16(%rax)
  mov %r9, 24(%rax)
  mov 40(%rsp), %r10
  mov %r10, 32(%rax)
  mov 48(%rsp), %r10
  mov %r10, 40(%rax)
  movabs $0x5AD05AD05AD05AD0, %r10
  mov %r10, 8(%rsp)
  mov %r10, 16(%rsp)
  mov %r10, 24(%rsp)
  mov %r10, 32(%rsp)
  mov $7, %eax
  ret
)");

TEST(Win64Run, ASixArgumentCallPassesRegistersAndStackWordsAndKeepsEveryLiveSlot) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("six", 0);
  GRJIT_VReg p[6];
  for (auto & v : p) {
    v = b.param(GRJIT_TYPE_I64);
  }
  GRJIT_VReg t[4];
  for (auto & v : t) {
    v = b.reg();
  }
  GRJIT_VReg r = b.reg(), sum = b.reg();
  b.at(b.block());
  for (int i = 0; i < 4; i++) {
    b.cnst(t[i], 1000 * (i + 1));
  }
  b.call(r, reinterpret_cast<const void *>(grjit_test_win64_probe),
      {V(p[0]), V(p[1]), V(p[2]), V(p[3]), V(p[4]), V(p[5])});
  b.mov(sum, V(r));
  for (auto v : p) {
    b.bin(GRJIT_OP_ADD, sum, V(sum), V(v));
  }
  for (auto v : t) {
    b.bin(GRJIT_OP_ADD, sum, V(sum), V(v));
  }
  b.ret(V(sum));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  std::memset(grjit_test_probe_seen, 0, sizeof grjit_test_probe_seen);
  auto run = c.run(w.ctx, {11, 22, 33, 44, 55, 66});
  EXPECT_EQ(run.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  /* All six arguments arrived: four registers and two words above the shadow. */
  EXPECT_EQ(grjit_test_probe_seen[0], 11u) << "rcx";
  EXPECT_EQ(grjit_test_probe_seen[1], 22u) << "rdx";
  EXPECT_EQ(grjit_test_probe_seen[2], 33u) << "r8";
  EXPECT_EQ(grjit_test_probe_seen[3], 44u) << "r9";
  EXPECT_EQ(grjit_test_probe_seen[4], 55u) << "[rsp+40]";
  EXPECT_EQ(grjit_test_probe_seen[5], 66u) << "[rsp+48]";
  /* The helper wrote its own shadow space; every live slot of the caller is intact. */
  EXPECT_EQ(run.out[0], 7u + (11 + 22 + 33 + 44 + 55 + 66) + (1000 + 2000 + 3000 + 4000));
}

namespace {

struct Walk {
  bool ran = false;
  bool found = false;
  DWORD64 base = 0;
  DWORD begin = 0;
  DWORD end = 0;
  bool rsp_is_above_the_frame = false;
  bool reached_the_caller = false;
  int frames = 0;
};
Walk g_walk;
const void * g_code_at = nullptr;
size_t g_code_size = 0;
DWORD64 g_target = 0;

/* Walks up from here with the operating system's unwinder, as a debugger or an
 * exception dispatcher does. Called from compiled code. */
__attribute__((noinline)) uint64_t walk_helper() {
  g_walk = Walk();
  g_walk.ran = true;
  CONTEXT c;
  RtlCaptureContext(&c);
  DWORD64 code_lo = reinterpret_cast<DWORD64>(g_code_at);
  DWORD64 code_hi = code_lo + g_code_size;
  for (int i = 0; i < 64; i++) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (rf == nullptr) {
      break;
    }
    g_walk.frames++;
    DWORD64 rbp_in_frame = c.Rbp;
    if (c.Rip >= code_lo && c.Rip < code_hi) {
      g_walk.found = true;
      g_walk.base = image;
      g_walk.begin = rf->BeginAddress;
      g_walk.end = rf->EndAddress;
      PVOID data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, rf, &c, &data, &establisher, nullptr);
      /* Past `pop rbp; ret` of this frame: rsp is just above the saved rbp and
       * the return address. */
      g_walk.rsp_is_above_the_frame = c.Rsp == rbp_in_frame + 16;
      continue;
    }
    if (image + rf->BeginAddress == g_target) {
      g_walk.reached_the_caller = true;
      break;
    }
    PVOID data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, rf, &c, &data, &establisher, nullptr);
  }
  return 0;
}

__attribute__((noinline)) uint32_t call_from_a_known_function(const Compiled & c, void * ctx) {
  return c.run(ctx, {}).exit;
}

} // namespace

TEST(Win64Run, AHelperCalledFromCompiledCodeWalksUpThroughItsFrameWithTheWindowsUnwinder) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("walk", 0);
  GRJIT_VReg r = b.reg();
  b.at(b.block());
  b.call(r, reinterpret_cast<const void *>(walk_helper));
  b.ret(V(r));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  g_code_at = grjit_code_address(c.code);
  g_code_size = grjit_code_size(c.code);
  g_target = reinterpret_cast<DWORD64>(&call_from_a_known_function);
  EXPECT_EQ(call_from_a_known_function(c, w.ctx), static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  ASSERT_TRUE(g_walk.ran);
  ASSERT_TRUE(g_walk.found) << "without registration the lookup finds nothing";
  EXPECT_EQ(g_walk.base, reinterpret_cast<DWORD64>(g_code_at)) << "RVAs are from the mapping";
  EXPECT_EQ(g_walk.begin, 0u);
  EXPECT_EQ(g_walk.end, g_code_size);
  EXPECT_TRUE(g_walk.rsp_is_above_the_frame) << "the unwinder undid the whole frame";
  EXPECT_TRUE(g_walk.reached_the_caller) << "the walk reached the test's own function";
}

TEST(Win64Run, DestroyDeletesTheFunctionTableAndTheLookupOfTheOldAddressFindsNothing) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  Fn f(six_calls_a_poll_and_a_guard());
  DWORD64 at;
  uint64_t bytes = w.bytes_in_use();
  {
    Compiled c(f, w.pages());
    ASSERT_TRUE(c) << grjit_result_string(c.result);
    at = reinterpret_cast<DWORD64>(grjit_code_address(c.code)) + 3;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(at, &image, nullptr);
    ASSERT_NE(rf, nullptr);
    EXPECT_EQ(image, reinterpret_cast<DWORD64>(grjit_code_address(c.code)));
  }
  DWORD64 image = 0;
  EXPECT_EQ(RtlLookupFunctionEntry(at, &image, nullptr), nullptr)
      << "the table must not outlive the pages";
  EXPECT_EQ(w.bytes_in_use(), bytes);
}

TEST(Win64Run, ARefusedRegistrationFailsTheCompileAndLeavesNothingBehind) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  FakePages pages(w.pages());
  OpsScope scope(&pages, /*fail=*/true);
  Fn f(six_calls_a_poll_and_a_guard());
  GRJIT_CompileOptions o{};
  o.pages = &pages.vtable;
  GRJIT_Code * out = reinterpret_cast<GRJIT_Code *>(1);
  uint64_t bytes = w.bytes_in_use();
  EXPECT_EQ(grjit_compile(&o, f, &out), GRJIT_ERR_IO);
  EXPECT_EQ(out, reinterpret_cast<GRJIT_Code *>(1));
  EXPECT_EQ(pages.live, 0);
  EXPECT_EQ(w.bytes_in_use(), bytes);
  EXPECT_EQ(g_ops.events, (std::vector<std::string>{"add"}));
}

#endif // GRJIT_TEST_WIN64

GRJIT_TEST_MAIN()
