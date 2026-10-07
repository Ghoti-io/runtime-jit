/**
 * @file
 *
 * The assembly stubs the call, tail-call and native-call tests need, in the one
 * place that knows the target: Linux x86-64 (SysV) and Linux arm64 (AAPCS64).
 * Each is a thing C++ cannot say, so it is written for the instruction set:
 *
 *  - `fx_call_with_sentinels` calls a compiled entry with the callee-saved
 *    registers set to known values and reads them back, so a register that
 *    compiled code used and did not restore shows in its own sentinel. On
 *    x86-64 those are `rbx`, `r12`-`r15`; on arm64 `x19`-`x28`, `x29` and the
 *    low halves of `d8`-`d15`, the registers AAPCS64 makes the callee's to keep
 *    (and `x29`, which the entry adapter sets to the chain-end marker and must
 *    give back);
 *  - `grjit_test_align_stub` and `grjit_test_align_status_stub` are natives that
 *    record the stack pointer at their entry, touching only scratch registers;
 *  - `grjit_test_status_garbage_zero` and `_three` are natives that return a
 *    status with garbage in the half of the register above it, which is padding
 *    for a 32-bit status under both ABIs;
 *  - `grjit_test_tail_garbage` is a hook that calls the real tail hook and
 *    returns its answer with garbage above the 32 bits the answer is;
 *  - `fx::n_clobber` is a native that trashes every caller-saved register,
 *    vector registers included.
 *
 * A test asks for the families it uses by defining `FX_ASM_<FAMILY>` before
 * including this file, so that a stub no test of a binary uses is not linked into
 * it (an assembly symbol that names a variable only another file defines would
 * not link). The target is selected here, and a target with neither form has
 * none of them: `FX_HAVE_CALLS_ASM` is 0 there, which is also where no call is
 * emitted.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_CALLS_ASM_H
#define GHOTI_IO_GRJIT_TESTS_CALLS_ASM_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#if defined(__x86_64__) && defined(__linux__)
#define FX_ASM_X86_64 1
#define FX_ASM_AARCH64 0
#define FX_HAVE_CALLS_ASM 1
#elif defined(__aarch64__) && defined(__linux__)
#define FX_ASM_X86_64 0
#define FX_ASM_AARCH64 1
#define FX_HAVE_CALLS_ASM 1
#else
#define FX_ASM_X86_64 0
#define FX_ASM_AARCH64 0
#define FX_HAVE_CALLS_ASM 0
#endif

namespace fx {

/** What the stack pointer is at a native's first instruction, plus this, is a
 *  multiple of sixteen: the call pushed an eight-byte return address on x86-64
 *  and left the pointer where it was on arm64 (the return address is in `x30`).
 *  It is also the adjustment in the native-stack budget, whose one formula is
 *  `sp_before_call - S - declared`: the lowest address a native may reach is its
 *  entry stack pointer plus this, less what it declared, so on arm64 a native has
 *  the eight bytes a return address takes on x86-64 more than it declared. */
#if FX_ASM_X86_64
constexpr uint64_t kNativeEntrySpBias = 8;
#else
constexpr uint64_t kNativeEntrySpBias = 0;
#endif

/** The register arguments of the internal convention between compiled functions
 *  on this target (the library's `GRJIT_*_INTERNAL_REG_ARGS`; the tests that include
 *  `backend_internal.h` assert they agree). */
#if FX_ASM_AARCH64
constexpr unsigned kInternalRegArgs = 8;
#else
constexpr unsigned kInternalRegArgs = 6;
#endif

/** The register words an AAPCS64 or SysV C callee can receive in registers,
 *  counting the context (which is a native's first): six on x86-64 SysV, eight on
 *  arm64. */
#if FX_ASM_X86_64
constexpr size_t kCRegisterWords = 6;
#else
constexpr size_t kCRegisterWords = 8;
#endif

/** The stack-argument area of a native with `n` IR arguments, in bytes: the words
 *  past the register words, with the context counted first, in whole sixteens. */
constexpr uint64_t native_stack_area(size_t n) {
  const size_t words = n + 1;
  const size_t stack_words = words > kCRegisterWords ? words - kCRegisterWords : 0;
  return (stack_words * 8 + 15) / 16 * 16;
}

} // namespace fx

/* ---- The callee-saved sentinels ------------------------------------------------ */

#if FX_HAVE_CALLS_ASM && defined(FX_ASM_SENTINELS)

namespace fx {

#if FX_ASM_X86_64
constexpr size_t kSentinelCount = 5;
inline const char * sentinel_name(size_t i) {
  static const char * const names[kSentinelCount] = {"rbx", "r12", "r13", "r14", "r15"};
  return names[i];
}
#else
constexpr size_t kSentinelCount = 19;
inline const char * sentinel_name(size_t i) {
  static const char * const names[kSentinelCount] = {"x19", "x20", "x21", "x22", "x23", "x24", "x25",
      "x26", "x27", "x28", "x29", "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15"};
  return names[i];
}
#endif

/** The value the stub puts in callee-saved register `i`: distinct, and none a
 *  pointer, a small integer or the chain-end marker. */
inline uint64_t sentinel_value(size_t i) {
  return UINT64_C(0x1111111111111111) * ((i % 15) + 1) ^ (static_cast<uint64_t>(i / 15) << 56);
}

} // namespace fx

/* `regs` holds `fx::kSentinelCount` words: the values to load before the call, and,
 * after it, the registers as the call left them. */
extern "C" uint32_t fx_call_with_sentinels(
    GRJIT_EntryFn entry, void * ctx, const uint64_t * args, uint64_t * out, uint64_t * regs);

namespace fx {

/** Enters function `fn` of the fixture engine `e` through the sentinel trampoline the way the
 *  engine's own run does (a guest frame, a JIT activation record, the native-stack limit), and
 *  checks every callee-saved register afterwards against `sentinel_value`, naming the one that
 *  changed and `what` it was doing. Returns the entry's exit. The frames the run left are
 *  unwound, as the engine's run leaves them. (A template, so that this header need not know the
 *  fixture, which is included after it.) */
template <class E>
uint32_t call_with_sentinels_and_check(E & e, int fn, std::vector<uint64_t> in, const char * what) {
  const auto * code = e.code_of(fn).code;
  e.base_frames = grcore_stack_frame_count(e.stack);
  EXPECT_TRUE(e.push_frame(fn, in.data(), in.size(), false)) << what;
  grcore_context_native_limit_here(e.ctx);
  GRCORE_ActivationRef rec;
  EXPECT_EQ(grcore_activation_enter(e.stack, GRCORE_ACTIVATION_JIT, e.engine, false, nullptr, &rec), GRCORE_OK)
      << what;
  std::vector<uint64_t> out(grjit_code_out_words(code), 0);
  in.resize(std::max<size_t>(in.size(), grjit_code_param_count(code)));
  uint64_t regs[kSentinelCount];
  for (size_t i = 0; i < kSentinelCount; i++) {
    regs[i] = sentinel_value(i);
  }
  // Through a trampoline that sets the registers and reads them back: calling a callee that does
  // not preserve them directly would corrupt this very function, which is what the test is for.
  const uint32_t exit = fx_call_with_sentinels(grjit_code_entry(code), e.ctx, in.data(), out.data(), regs);
  for (size_t i = 0; i < kSentinelCount; i++) {
    EXPECT_EQ(regs[i], sentinel_value(i)) << sentinel_name(i) << " after " << what;
  }
  EXPECT_EQ(grcore_activation_leave(e.stack, rec), GRCORE_OK) << what;
  grcore_unwind_all(e.stack, nullptr);
  e.reset_reservation();
  return exit;
}

} // namespace fx

#if FX_ASM_X86_64
asm(R"(
  .text
  .globl fx_call_with_sentinels
  .type fx_call_with_sentinels, @function
fx_call_with_sentinels:
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
  mov 0(%r8), %rbx
  mov 8(%r8), %r12
  mov 16(%r8), %r13
  mov 24(%r8), %r14
  mov 32(%r8), %r15
  call *%rax
  pop %r8
  mov %rbx, 0(%r8)
  mov %r12, 8(%r8)
  mov %r13, 16(%r8)
  mov %r14, 24(%r8)
  mov %r15, 32(%r8)
  pop %r15
  pop %r14
  pop %r13
  pop %r12
  pop %rbx
  pop %rbp
  ret
  .size fx_call_with_sentinels, .-fx_call_with_sentinels
)");
#else
/* x0 entry, x1 context, x2 args, x3 out, x4 regs: x19-x28 at words 0 to 9, x29 at 10,
 * d8-d15 at 11 to 18. Everything after the call is addressed from `sp`, which the
 * callee gives back, because `x29` is a sentinel by then. */
asm(R"(
  .text
  .globl fx_call_with_sentinels
  .type fx_call_with_sentinels, %function
fx_call_with_sentinels:
  stp x29, x30, [sp, #-176]!
  mov x29, sp
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  str x4, [sp, #160]
  mov x16, x0
  mov x0, x1
  mov x1, x2
  mov x2, x3
  ldp x19, x20, [x4, #0]
  ldp x21, x22, [x4, #16]
  ldp x23, x24, [x4, #32]
  ldp x25, x26, [x4, #48]
  ldp x27, x28, [x4, #64]
  ldr d8, [x4, #88]
  ldr d9, [x4, #96]
  ldr d10, [x4, #104]
  ldr d11, [x4, #112]
  ldr d12, [x4, #120]
  ldr d13, [x4, #128]
  ldr d14, [x4, #136]
  ldr d15, [x4, #144]
  ldr x29, [x4, #80]
  blr x16
  ldr x15, [sp, #160]
  stp x19, x20, [x15, #0]
  stp x21, x22, [x15, #16]
  stp x23, x24, [x15, #32]
  stp x25, x26, [x15, #48]
  stp x27, x28, [x15, #64]
  str x29, [x15, #80]
  str d8, [x15, #88]
  str d9, [x15, #96]
  str d10, [x15, #104]
  str d11, [x15, #112]
  str d12, [x15, #120]
  str d13, [x15, #128]
  str d14, [x15, #136]
  str d15, [x15, #144]
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x23, x24, [sp, #48]
  ldp x25, x26, [sp, #64]
  ldp x27, x28, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldp x29, x30, [sp], #176
  ret
  .size fx_call_with_sentinels, .-fx_call_with_sentinels
)");
#endif

#endif // FX_ASM_SENTINELS

/* ---- Natives that record their entry stack pointer ----------------------------- */

#if FX_HAVE_CALLS_ASM && defined(FX_ASM_ENTRY_SP)

extern "C" {
uint64_t grjit_test_entry_rsp[256];
uint64_t grjit_test_entry_count = 0;
uint64_t grjit_test_align_stub(void *, ...);
GRJIT_NativeResult grjit_test_align_status_stub(void *, ...);
}

namespace fx {
/** The recording native, with or without a status (and then with no result: a native
 *  may have a status and nothing else). */
inline const void * align_stub(bool status) {
  return status ? reinterpret_cast<const void *>(grjit_test_align_status_stub)
                : reinterpret_cast<const void *>(grjit_test_align_stub);
}
} // namespace fx

#if FX_ASM_X86_64
/* Records the stack pointer at its entry (after the return address is pushed), and
 * touches only rax and rcx. */
asm(R"(
.text
.globl grjit_test_align_stub
.type grjit_test_align_stub, @function
grjit_test_align_stub:
  movq grjit_test_entry_count(%rip), %rax
  leaq grjit_test_entry_rsp(%rip), %rcx
  movq %rsp, (%rcx,%rax,8)
  incq %rax
  movq %rax, grjit_test_entry_count(%rip)
  xorl %eax, %eax
  ret
.size grjit_test_align_stub, .-grjit_test_align_stub
.globl grjit_test_align_status_stub
.type grjit_test_align_status_stub, @function
grjit_test_align_status_stub:
  movq grjit_test_entry_count(%rip), %rax
  leaq grjit_test_entry_rsp(%rip), %rcx
  movq %rsp, (%rcx,%rax,8)
  incq %rax
  movq %rax, grjit_test_entry_count(%rip)
  xorl %eax, %eax
  xorl %edx, %edx
  ret
.size grjit_test_align_status_stub, .-grjit_test_align_status_stub
)");
#else
/* Records `sp` at its entry, touching only x9-x12 (the status stub also zeroes x1). */
asm(R"(
.text
.globl grjit_test_align_stub
.type grjit_test_align_stub, %function
grjit_test_align_stub:
  adrp x9, grjit_test_entry_count
  ldr x10, [x9, :lo12:grjit_test_entry_count]
  adrp x11, grjit_test_entry_rsp
  add x11, x11, :lo12:grjit_test_entry_rsp
  mov x12, sp
  str x12, [x11, x10, lsl #3]
  add x10, x10, #1
  str x10, [x9, :lo12:grjit_test_entry_count]
  mov x0, #0
  ret
.size grjit_test_align_stub, .-grjit_test_align_stub
.globl grjit_test_align_status_stub
.type grjit_test_align_status_stub, %function
grjit_test_align_status_stub:
  adrp x9, grjit_test_entry_count
  ldr x10, [x9, :lo12:grjit_test_entry_count]
  adrp x11, grjit_test_entry_rsp
  add x11, x11, :lo12:grjit_test_entry_rsp
  mov x12, sp
  str x12, [x11, x10, lsl #3]
  add x10, x10, #1
  str x10, [x9, :lo12:grjit_test_entry_count]
  mov x0, #0
  mov x1, #0
  ret
.size grjit_test_align_status_stub, .-grjit_test_align_status_stub
)");
#endif

#endif // FX_ASM_ENTRY_SP

/* ---- Natives that return garbage above a 32-bit status -------------------------- */

#if FX_HAVE_CALLS_ASM && defined(FX_ASM_STATUS_GARBAGE)

extern "C" {
GRJIT_NativeResult grjit_test_status_garbage_zero(void *);
GRJIT_NativeResult grjit_test_status_garbage_three(void *);
}

/* A value of 7 and a status of zero, or three, with `0xDEADBEEF` in the half of the
 * register that carries the status above its 32 bits: padding that the call must not
 * read (x86-64: the upper half of `rdx`; arm64: of `x1`). */
#if FX_ASM_X86_64
asm(R"(
.text
.globl grjit_test_status_garbage_zero
.type grjit_test_status_garbage_zero, @function
grjit_test_status_garbage_zero:
  movl $7, %eax
  movabsq $0xDEADBEEF00000000, %rdx
  ret
.size grjit_test_status_garbage_zero, .-grjit_test_status_garbage_zero
.globl grjit_test_status_garbage_three
.type grjit_test_status_garbage_three, @function
grjit_test_status_garbage_three:
  movl $7, %eax
  movabsq $0xDEADBEEF00000003, %rdx
  ret
.size grjit_test_status_garbage_three, .-grjit_test_status_garbage_three
)");
#else
asm(R"(
.text
.globl grjit_test_status_garbage_zero
.type grjit_test_status_garbage_zero, %function
grjit_test_status_garbage_zero:
  mov x0, #7
  movz x1, #0xBEEF, lsl #32
  movk x1, #0xDEAD, lsl #48
  ret
.size grjit_test_status_garbage_zero, .-grjit_test_status_garbage_zero
.globl grjit_test_status_garbage_three
.type grjit_test_status_garbage_three, %function
grjit_test_status_garbage_three:
  mov x0, #7
  movz x1, #3
  movk x1, #0xBEEF, lsl #32
  movk x1, #0xDEAD, lsl #48
  ret
.size grjit_test_status_garbage_three, .-grjit_test_status_garbage_three
)");
#endif

#endif // FX_ASM_STATUS_GARBAGE

/* ---- A hook that answers with garbage above its 32 bits ------------------------- */

#if FX_HAVE_CALLS_ASM && defined(FX_ASM_TAIL_GARBAGE)

extern "C" {
uint32_t (*grjit_test_real_tail)(void *, uint64_t, const uint64_t *, uint64_t) = nullptr;
/* Calls the real hook and returns what it returned in the low 32 bits with the high half
 * of the register set to garbage, as the ABI allows for a function returning 32 bits. */
uint32_t grjit_test_tail_garbage(void *, uint64_t, const uint64_t *, uint64_t);
}

#if FX_ASM_X86_64
asm(".text\n"
    ".globl grjit_test_tail_garbage\n"
    ".type grjit_test_tail_garbage,@function\n"
    "grjit_test_tail_garbage:\n"
    "  subq $8, %rsp\n"
    "  movq grjit_test_real_tail(%rip), %r11\n"
    "  call *%r11\n"
    "  addq $8, %rsp\n"
    "  movl %eax, %eax\n"
    "  movabsq $0x1357924600000000, %rcx\n"
    "  orq %rcx, %rax\n"
    "  ret\n"
    ".size grjit_test_tail_garbage, .-grjit_test_tail_garbage\n");
#else
asm(".text\n"
    ".globl grjit_test_tail_garbage\n"
    ".type grjit_test_tail_garbage, %function\n"
    "grjit_test_tail_garbage:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  adrp x16, grjit_test_real_tail\n"
    "  ldr x16, [x16, :lo12:grjit_test_real_tail]\n"
    "  blr x16\n"
    "  mov w0, w0\n"
    "  movz x1, #0x9246, lsl #32\n"
    "  movk x1, #0x1357, lsl #48\n"
    "  orr x0, x0, x1\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    ".size grjit_test_tail_garbage, .-grjit_test_tail_garbage\n");
#endif

#endif // FX_ASM_TAIL_GARBAGE

/* ---- Every other hook, answering with garbage above its 32 bits ------------------ */

/* A hook returns a uint32_t, so the upper half of its return register is the callee's to leave as it
 * likes (the ABI says nothing of it). `grjit_test_garbage_<hook>` calls the real function held in
 * `grjit_test_real_<hook>` with the arguments it was given (all in registers, for every hook) and returns
 * its answer in the low 32 bits with garbage above: a test installs it in the hook's place and sets the
 * pointer. Code that tests all 64 bits of a hook's answer reads a zero as a refusal on every call. */
#if FX_HAVE_CALLS_ASM && defined(FX_ASM_HOOK_GARBAGE)

extern "C" {
void * grjit_test_real_push = nullptr;
void * grjit_test_real_compile = nullptr;
void * grjit_test_real_deopt = nullptr;
void * grjit_test_real_poll = nullptr;
void * grjit_test_real_entry = nullptr;
uint32_t grjit_test_garbage_push(void *, uint64_t, const uint64_t *, uint64_t);
uint32_t grjit_test_garbage_compile(void *, uint64_t);
uint32_t grjit_test_garbage_deopt(void *, uint64_t);
uint32_t grjit_test_garbage_poll(void *, uint64_t, uint64_t);
uint32_t grjit_test_garbage_entry(void *);
}

#if FX_ASM_X86_64
#define FX_GARBAGE_STUB(hook)                                                    \
  asm(".text\n"                                                                  \
      ".globl grjit_test_garbage_" #hook "\n"                                    \
      ".type grjit_test_garbage_" #hook ",@function\n"                           \
      "grjit_test_garbage_" #hook ":\n"                                          \
      "  subq $8, %rsp\n"                                                        \
      "  movq grjit_test_real_" #hook "(%rip), %r11\n"                           \
      "  call *%r11\n"                                                           \
      "  addq $8, %rsp\n"                                                        \
      "  movl %eax, %eax\n"                                                      \
      "  movabsq $0x1357924600000000, %rcx\n"                                    \
      "  orq %rcx, %rax\n"                                                       \
      "  ret\n"                                                                  \
      ".size grjit_test_garbage_" #hook ", .-grjit_test_garbage_" #hook "\n")
#else
#define FX_GARBAGE_STUB(hook)                                                    \
  asm(".text\n"                                                                  \
      ".globl grjit_test_garbage_" #hook "\n"                                    \
      ".type grjit_test_garbage_" #hook ", %function\n"                          \
      "grjit_test_garbage_" #hook ":\n"                                          \
      "  stp x29, x30, [sp, #-16]!\n"                                            \
      "  mov x29, sp\n"                                                          \
      "  adrp x16, grjit_test_real_" #hook "\n"                                  \
      "  ldr x16, [x16, :lo12:grjit_test_real_" #hook "]\n"                      \
      "  blr x16\n"                                                              \
      "  mov w0, w0\n"                                                           \
      "  movz x1, #0x9246, lsl #32\n"                                            \
      "  movk x1, #0x1357, lsl #48\n"                                            \
      "  orr x0, x0, x1\n"                                                       \
      "  ldp x29, x30, [sp], #16\n"                                              \
      "  ret\n"                                                                  \
      ".size grjit_test_garbage_" #hook ", .-grjit_test_garbage_" #hook "\n")
#endif

FX_GARBAGE_STUB(push);
FX_GARBAGE_STUB(compile);
FX_GARBAGE_STUB(deopt);
FX_GARBAGE_STUB(poll);
FX_GARBAGE_STUB(entry);

#endif // FX_ASM_HOOK_GARBAGE

/* ---- A native that trashes every caller-saved register -------------------------- */

#if FX_HAVE_CALLS_ASM && defined(FX_ASM_CLOBBER)

namespace fx {

/** Takes the value it is given, trashes every caller-saved register (and the
 *  vector registers), and returns the value plus one. The compiler is told the
 *  registers are clobbered, so it saves what it needs; the point is that the
 *  *caller* may not assume anything survives. */
__attribute__((noinline)) inline uint64_t n_clobber(void *, uint64_t v) {
#if FX_ASM_X86_64
  asm volatile(
      "movabsq $0x5A5A5A5A5A5A5A5A, %%rax\n"
      "movabsq $0x5A5A5A5A5A5A5A5B, %%rcx\n"
      "movabsq $0x5A5A5A5A5A5A5A5C, %%rdx\n"
      "movabsq $0x5A5A5A5A5A5A5A5D, %%rsi\n"
      "movabsq $0x5A5A5A5A5A5A5A5E, %%rdi\n"
      "movabsq $0x5A5A5A5A5A5A5A5F, %%r8\n"
      "movabsq $0x5A5A5A5A5A5A5A60, %%r9\n"
      "movabsq $0x5A5A5A5A5A5A5A61, %%r10\n"
      "movabsq $0x5A5A5A5A5A5A5A62, %%r11\n"
      "pcmpeqd %%xmm0, %%xmm0\n pcmpeqd %%xmm1, %%xmm1\n pcmpeqd %%xmm2, %%xmm2\n pcmpeqd %%xmm3, %%xmm3\n"
      "pcmpeqd %%xmm4, %%xmm4\n pcmpeqd %%xmm5, %%xmm5\n pcmpeqd %%xmm6, %%xmm6\n pcmpeqd %%xmm7, %%xmm7\n"
      "pcmpeqd %%xmm8, %%xmm8\n pcmpeqd %%xmm9, %%xmm9\n pcmpeqd %%xmm10, %%xmm10\n pcmpeqd %%xmm11, %%xmm11\n"
      "pcmpeqd %%xmm12, %%xmm12\n pcmpeqd %%xmm13, %%xmm13\n pcmpeqd %%xmm14, %%xmm14\n pcmpeqd %%xmm15, %%xmm15\n"
      :
      :
      : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
        "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
#else
  asm volatile(
      "movz x0, #0x5A5A\n movz x1, #0x5A5B\n movz x2, #0x5A5C\n movz x3, #0x5A5D\n"
      "movz x4, #0x5A5E\n movz x5, #0x5A5F\n movz x6, #0x5A60\n movz x7, #0x5A61\n"
      "movz x8, #0x5A62\n movz x9, #0x5A63\n movz x10, #0x5A64\n movz x11, #0x5A65\n"
      "movz x12, #0x5A66\n movz x13, #0x5A67\n movz x14, #0x5A68\n movz x15, #0x5A69\n"
      "movz x16, #0x5A6A\n movz x17, #0x5A6B\n"
      "movi v0.16b, #0x5A\n movi v1.16b, #0x5A\n movi v2.16b, #0x5A\n movi v3.16b, #0x5A\n"
      "movi v4.16b, #0x5A\n movi v5.16b, #0x5A\n movi v6.16b, #0x5A\n movi v7.16b, #0x5A\n"
      "movi v16.16b, #0x5A\n movi v17.16b, #0x5A\n movi v18.16b, #0x5A\n movi v19.16b, #0x5A\n"
      "movi v20.16b, #0x5A\n movi v21.16b, #0x5A\n movi v22.16b, #0x5A\n movi v23.16b, #0x5A\n"
      "movi v24.16b, #0x5A\n movi v25.16b, #0x5A\n movi v26.16b, #0x5A\n movi v27.16b, #0x5A\n"
      "movi v28.16b, #0x5A\n movi v29.16b, #0x5A\n movi v30.16b, #0x5A\n movi v31.16b, #0x5A\n"
      :
      :
      : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
        "x17", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
        "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory");
#endif
  return v + 1;
}

} // namespace fx

#endif // FX_ASM_CLOBBER

#endif // GHOTI_IO_GRJIT_TESTS_CALLS_ASM_H
