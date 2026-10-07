/**
 * @file
 *
 * Calls from compiled code to natives (AD-28, AD-17, CAP-7) on x86-64 SysV. This
 * part of the suite calls real C functions through the C ABI from compiled code
 * and compares with the same function called from C: every argument count and
 * type, the alignment at the callee's entry and the stack pointer across calls,
 * registers a native clobbers, the walk start the call stores first, the native
 * stack check and the sites the call makes. The engine's side (collections under
 * a chain, statuses, re-entry) is run through the fixture engine below.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../calls_fixture.h"

#include "../../src/backend/backend_internal.h"
#include "../../src/code/code_internal.h"

#include <array>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <utility>

using namespace fx;

/* Native calls are emitted for x86-64 SysV only (arm64 and Win64 are story 7's).
 * Where they are not, the suite does not skip, which would count as tests that
 * proved nothing: it shows the refusal instead, which is what those targets
 * promise. */
#if defined(__x86_64__) && defined(__linux__)

namespace {

constexpr GRJIT_Type TI = GRJIT_TYPE_I64;
constexpr GRJIT_Type TR = GRJIT_TYPE_REF;
constexpr GRJIT_Type TP = GRJIT_TYPE_PTR;
constexpr GRJIT_Type kNoResult = GRJIT_NATIVE_NO_RESULT;

/* ---- Natives of every arity ----------------------------------------------- */

template <size_t>
using W = uint64_t;

void * g_seen_ctx = nullptr; // the context a native was last called with
long g_calls = 0;

/* The reference: the sum of (i + 1) * a_i and a constant, in 64-bit words. */
template <size_t... Is>
uint64_t sum_n(void * ctx, W<Is>... a) {
  g_seen_ctx = ctx;
  g_calls++;
  const uint64_t v[] = {a..., 0};
  uint64_t s = 0x1234;
  for (size_t i = 0; i < sizeof...(Is); i++) {
    s += (i + 1) * v[i];
  }
  return s;
}

template <size_t... Is>
const void * sum_ptr(std::index_sequence<Is...>) {
  uint64_t (*f)(void *, W<Is>...) = &sum_n<Is...>;
  return reinterpret_cast<const void *>(f);
}

template <size_t... Ns>
std::array<const void *, sizeof...(Ns)> make_sums(std::index_sequence<Ns...>) {
  return {sum_ptr(std::make_index_sequence<Ns>{})...};
}

const std::array<const void *, 17> & sums() {
  static const auto table = make_sums(std::make_index_sequence<17>{});
  return table;
}

/* Calls a native of `n` arguments from C, the context first. */
template <size_t... Is>
uint64_t call_c_n(const void * fn, void * ctx, const uint64_t * a, std::index_sequence<Is...>) {
  return reinterpret_cast<uint64_t (*)(void *, W<Is>...)>(reinterpret_cast<uintptr_t>(fn))(ctx, a[Is]...);
}

template <size_t N>
uint64_t call_c_at(const void * fn, void * ctx, const uint64_t * a) {
  return call_c_n(fn, ctx, a, std::make_index_sequence<N>{});
}

uint64_t call_c(size_t n, const void * fn, void * ctx, const uint64_t * a) {
  static const std::array<uint64_t (*)(const void *, void *, const uint64_t *), 17> by = [] {
    return std::array<uint64_t (*)(const void *, void *, const uint64_t *), 17>{
        call_c_at<0>, call_c_at<1>, call_c_at<2>, call_c_at<3>, call_c_at<4>, call_c_at<5>,
        call_c_at<6>, call_c_at<7>, call_c_at<8>, call_c_at<9>, call_c_at<10>, call_c_at<11>,
        call_c_at<12>, call_c_at<13>, call_c_at<14>, call_c_at<15>, call_c_at<16>};
  }();
  return by[n](fn, ctx, a);
}

/* ---- A callable function the tests build without the fixture ------------------- */

uint32_t h_unused_push(void *, uint64_t, const uint64_t *, uint64_t) { return 1; }
void h_unused_pop(void *) {}
uint32_t h_unused_compile(void *, uint64_t) { return 1; }
long g_deopts = 0;
uint64_t g_last_cause = 0;
uint32_t h_deopt(void *, uint64_t cause) {
  g_deopts++;
  g_last_cause = cause;
  return 0; // no guest frames: nothing to rebuild
}

GRJIT_CallHooks hooks() {
  GRJIT_CallHooks h{};
  h.push = h_unused_push;
  h.pop = h_unused_pop;
  h.compile = h_unused_compile;
  h.deopt = h_deopt;
  return h;
}

const GRCORE_PollIdentity kId{1, 5};

/* f(a0 .. a{n-1}) = native(a0 .. a{n-1}), the parameters of the types given. */
struct Bare {
  Fn f;
  Bare(const GRJIT_NativeTable * t, uint32_t id, const std::vector<GRJIT_Type> & types, GRJIT_Type result,
      bool with_status = false)
      : f(build(t, id, types, result, with_status)) {}
  static GRJIT_Function * build(const GRJIT_NativeTable * t, uint32_t id,
      const std::vector<GRJIT_Type> & types, GRJIT_Type result, bool with_status) {
    B b("bare", types.size() + 1);
    std::vector<GRJIT_VReg> ps;
    for (GRJIT_Type ty : types) {
      ps.push_back(b.param(ty));
    }
    GRJIT_VReg out = result == kNoResult ? GRJIT_NO_VREG : b.reg(result);
    b.callable(hooks());
    b.natives(t);
    b.at(b.block());
    std::vector<GRJIT_Operand> args;
    std::vector<GRJIT_FrameSlot> st;
    for (GRJIT_VReg p : ps) {
      args.push_back(V(p));
      st.push_back(grjit_frame_slot_vreg(p));
    }
    st.push_back(grjit_frame_slot_dead());
    if (with_status) {
      b.call_native(out, id, args, kId, st, kId, st);
    } else {
      b.call_native(out, id, args, kId, st);
    }
    if (out == GRJIT_NO_VREG) {
      b.ret(I(0));
    } else {
      b.ret(V(out));
    }
    return b.finish();
  }
};

} // namespace

/* ---- Arguments --------------------------------------------------------------------- */

TEST(Natives, EveryArityFromZeroToSixteenArrivesInTheOrderOfTheCAbiAndTheResultIsTheNativesOwn) {
  JitWorld w;
  NativeTab t;
  for (size_t n = 0; n <= 16; n++) {
    std::vector<GRJIT_Type> types(n, TI);
    uint32_t id = t.add(sums()[n], types, TI);
    Bare bare(t, id, types, TI);
    Compiled c(bare.f, w.pages());
    ASSERT_TRUE(c) << n;
    uint64_t a[16];
    for (size_t i = 0; i < 16; i++) {
      a[i] = 0x1000000000000001ull * (i + 3) + 7 * i; // distinct, and wide enough to catch a truncation
    }
    g_calls = 0;
    g_seen_ctx = nullptr;
    auto run = c.run(w.ctx, std::vector<uint64_t>(a, a + n));
    const long calls = g_calls; // before the reference below, which is the same function
    void * const seen = g_seen_ctx;
    EXPECT_EQ(run.exit, uint32_t{GRJIT_EXIT_RETURNED}) << n;
    EXPECT_EQ(run.out[0], call_c(n, sums()[n], w.ctx, a))
        << n << " arguments: a misplaced stack argument is a wrong sum";
    EXPECT_EQ(calls, 1) << n << ": the native is called once and compiled code never leaves";
    EXPECT_EQ(seen, static_cast<void *>(w.ctx)) << "the context is the implicit first argument";
  }
}

TEST(Natives, ArgumentsOfEveryTypeAndImmediatesArriveIntactForEveryCountAndResultType) {
  JitWorld w;
  NativeTab t;
  const GRJIT_Type cycle[3] = {TI, TR, TP};
  for (size_t n = 0; n <= 16; n++) {
    for (int shift = 0; shift < 3; shift++) {
      std::vector<GRJIT_Type> types;
      for (size_t i = 0; i < n; i++) {
        types.push_back(cycle[(i + static_cast<size_t>(shift)) % 3]);
      }
      for (GRJIT_Type result : {TI, TR, TP}) {
        uint32_t id = t.add(sums()[n], types, result);
        Bare bare(t, id, types, result);
        Compiled c(bare.f, w.pages());
        ASSERT_TRUE(c);
        uint64_t a[16];
        for (size_t i = 0; i < 16; i++) {
          a[i] = 0x0123456789abcdefull ^ (0x1111111111111111ull * (i + 1));
        }
        auto run = c.run(w.ctx, std::vector<uint64_t>(a, a + n));
        EXPECT_EQ(run.exit, uint32_t{GRJIT_EXIT_RETURNED});
        EXPECT_EQ(run.out[0], call_c(n, sums()[n], w.ctx, a)) << n << " arguments, shift " << shift;
      }
    }
  }
  // Immediates, in every position: the call is made with constants, mixed with registers.
  for (size_t n = 1; n <= 16; n++) {
    std::vector<GRJIT_Type> types(n, TI);
    uint32_t id = t.add(sums()[n], types, TI);
    B b("imm", 1);
    GRJIT_VReg x = b.param(TI);
    GRJIT_VReg out = b.reg(TI);
    b.callable(hooks());
    b.natives(t);
    b.at(b.block());
    std::vector<GRJIT_Operand> args;
    uint64_t a[16];
    for (size_t i = 0; i < n; i++) {
      bool use_reg = i % 3 == 1;
      a[i] = use_reg ? 0x7777000000000000ull + n : 0xFFFFFFFF00000000ull + 31 * i; // negative and wide
      args.push_back(use_reg ? V(x) : I(static_cast<int64_t>(a[i])));
    }
    b.call_native(out, id, args, kId, {grjit_frame_slot_vreg(x)});
    b.ret(V(out));
    Fn f(b.finish());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    auto run = c.run(w.ctx, {0x7777000000000000ull + n});
    EXPECT_EQ(run.out[0], call_c(n, sums()[n], w.ctx, a)) << n;
  }
}

/* ---- Alignment, the stack pointer, clobbered registers ---------------------------------- */

extern "C" {
uint64_t grjit_test_entry_rsp[256];
uint64_t grjit_test_entry_count = 0;
uint64_t grjit_test_align_stub(void *, ...);
}
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
)");

TEST(Natives, TheStackIsSixteenAlignedAtTheNativesEntryAndTheSameAfterEveryCallForOddAndEvenStackArguments) {
  JitWorld w;
  NativeTab t;
  for (size_t n = 0; n <= 16; n++) {
    std::vector<GRJIT_Type> types(n, TI);
    uint32_t id = t.add(reinterpret_cast<const void *>(grjit_test_align_stub), types, kNoResult);
    // Ten calls in a row, with a probe of the native stack pointer before and between.
    B b("align", 1);
    GRJIT_VReg x = b.param(TI);
    b.callable(hooks());
    b.natives(t);
    b.at(b.block());
    std::vector<GRJIT_Operand> args(n, V(x));
    for (int k = 0; k < 10; k++) {
      b.call_native(GRJIT_NO_VREG, id, args, kId, {grjit_frame_slot_vreg(x)});
    }
    b.ret(V(x));
    Fn f(b.finish());
    Compiled c(f, w.pages());
    ASSERT_TRUE(c);
    grjit_test_entry_count = 0;
    auto run = c.run(w.ctx, {42});
    ASSERT_EQ(run.exit, uint32_t{GRJIT_EXIT_RETURNED});
    ASSERT_EQ(grjit_test_entry_count, 10u);
    for (int k = 0; k < 10; k++) {
      EXPECT_EQ((grjit_test_entry_rsp[k] + 8) % 16, 0u) << n << " arguments, call " << k;
      EXPECT_EQ(grjit_test_entry_rsp[k], grjit_test_entry_rsp[0])
          << n << " arguments: a missing pop drifts rsp by the area with every call";
    }
    // The area is what the ABI says: the words past the sixth, in whole sixteens, so the
    // entry stack pointer is the same function of the count.
    static uint64_t first_entry = 0;
    if (n == 0) {
      first_entry = grjit_test_entry_rsp[0];
    }
    size_t stack_words = n + 1 > 6 ? n + 1 - 6 : 0;
    EXPECT_EQ(first_entry - grjit_test_entry_rsp[0], (stack_words * 8 + 15) / 16 * 16)
        << n << " arguments leave exactly the rounded area below the frame";
  }
}

namespace {

/* Takes the value it is given, trashes every caller-saved register (and the
 * vector registers), and returns the value plus one. The compiler is told the
 * registers are clobbered, so it saves what it needs; the point is that the
 * *caller* may not assume anything survives. */
__attribute__((noinline)) uint64_t n_clobber(void *, uint64_t v) {
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
  return v + 1;
}

} // namespace

TEST(Natives, ANativeThatClobbersEveryCallerSavedRegisterLeavesTheCallersValuesAndTheNextContextIntact) {
  JitWorld w;
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(n_clobber), {TI}, TI);
  // v = x; for 20 iterations: v = clobber(v); keep a second and third value live across all of them.
  B b("clobber", 1);
  GRJIT_VReg x = b.param(TI);
  GRJIT_VReg keep = b.reg(TI);
  GRJIT_VReg keep2 = b.reg(TI);
  GRJIT_VReg v = b.reg(TI);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.bin(GRJIT_OP_MUL, keep, V(x), I(3));
  b.bin(GRJIT_OP_ADD, keep2, V(x), I(0x1000));
  b.mov(v, V(x));
  for (int k = 0; k < 20; k++) {
    b.call_native(v, id, {V(v)}, kId, {grjit_frame_slot_vreg(x)});
  }
  GRJIT_VReg r1 = b.reg(TI);
  GRJIT_VReg r2 = b.reg(TI);
  b.bin(GRJIT_OP_ADD, r1, V(v), V(keep));
  b.bin(GRJIT_OP_ADD, r2, V(r1), V(keep2));
  b.ret(V(r2));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  for (uint64_t input : {uint64_t{5}, uint64_t{0x123456789}, ~uint64_t{0}}) {
    auto run = c.run(w.ctx, {input});
    EXPECT_EQ(run.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(run.out[0], (input + 20) + input * 3 + (input + 0x1000)) << input;
  }
}

/* ---- The walk start ------------------------------------------------------------------ */

namespace {

uintptr_t g_cell_base = 0, g_cell_ret = 0;
uintptr_t g_sp_in_native = 0;

/* Reads the walk-start cell the call stored, as a collection in the native would
 * find it. */
__attribute__((noinline)) uint64_t n_read_cell(void * ctx, uint64_t) {
  const uintptr_t * cell = reinterpret_cast<const uintptr_t *>(
      static_cast<char *>(ctx) + grcore_jit_layout()->walk_cell_offset);
  g_cell_base = cell[0];
  g_cell_ret = cell[1];
  g_sp_in_native = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  return 0;
}

} // namespace

TEST(Natives, TheCallStoresItsFrameBaseAndReturnAddressBeforeTheNativeRunsOverAStaleCell) {
  JitWorld w;
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(n_read_cell), {TI}, TI);
  Bare bare(t, id, {TI}, TI);
  Compiled c(bare.f, w.pages());
  ASSERT_TRUE(c);
  uintptr_t * cell = reinterpret_cast<uintptr_t *>(
      reinterpret_cast<char *>(w.ctx) + grcore_jit_layout()->walk_cell_offset);
  cell[0] = 0xDEAD0000DEAD0008ull; // another chain's value, from the previous call
  cell[1] = 0xBEEF0000BEEF0000ull;
  g_cell_base = g_cell_ret = 0;
  auto run = c.run(w.ctx, {1});
  ASSERT_EQ(run.exit, uint32_t{GRJIT_EXIT_RETURNED});
  EXPECT_NE(g_cell_base, uintptr_t{0xDEAD0000DEAD0008ull}) << "the native finds this call's start, not the stale one";
  EXPECT_NE(g_cell_ret, uintptr_t{0xBEEF0000BEEF0000ull});
  // The return address is inside this code, and the frame base is a word-aligned address
  // above the native's own frame (the native is called from that frame).
  const uintptr_t start = reinterpret_cast<uintptr_t>(grjit_code_address(c.code));
  EXPECT_GE(g_cell_ret, start);
  EXPECT_LT(g_cell_ret, start + grjit_code_size(c.code));
  EXPECT_EQ(g_cell_base % 8, 0u);
  EXPECT_GT(g_cell_base, g_sp_in_native);
  // And it is the call's site: the stack map at that offset is the call's, which the
  // metadata names with the poll identity the call was given.
  const GRCORE_CodeSite * site = grcore_codemeta_find(grjit_code_meta(c.code), g_cell_ret - start);
  ASSERT_NE(site, nullptr) << "the return address is a site";
  EXPECT_EQ(site->kind, GRCORE_SITE_GC_POINT_CALL);
  EXPECT_EQ(site->identity.function, kId.function);
  EXPECT_EQ(site->identity.offset, kId.offset);
}

/* ---- The sites ------------------------------------------------------------------------ */

TEST(Natives, ANativeCallIsASiteWhoseMapNamesWhatIsLiveAfterItButNotItsResultOrItsArguments) {
  JitWorld w;
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(n_read_cell), {TR}, TR);
  B b("sites", 4);
  GRJIT_VReg arg_only = b.param(TR);
  GRJIT_VReg live_after = b.param(TR);
  GRJIT_VReg raw = b.param(TI);
  GRJIT_VReg res = b.reg(TR);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.cnst(res, 0);
  b.call_native(res, id, {V(arg_only)}, kId,
      {grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead(), grjit_frame_slot_dead()});
  GRJIT_VReg sum = b.reg(TI);
  b.bin(GRJIT_OP_ADD, sum, V(raw), I(1));
  b.cmp(GRJIT_CMP_EQ, sum, V(live_after), V(res));
  b.ret(V(sum));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  const GRCORE_CodeMeta * meta = grjit_code_meta(c.code);
  const GRCORE_CodeSite * call = nullptr;
  const GRCORE_CodeSite * exit_before = nullptr;
  size_t calls = 0, exits = 0;
  for (size_t i = 0; i < meta->site_count; i++) {
    if (meta->sites[i].kind == GRCORE_SITE_GC_POINT_CALL) {
      call = &meta->sites[i];
      calls++;
    } else if (meta->sites[i].kind == GRCORE_SITE_GUARD) {
      exit_before = &meta->sites[i];
      exits++;
    }
  }
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(exits, 1u) << "a native with no status has the call and the exit before it, and no more";
  ASSERT_EQ(meta->site_count, 2u);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(exit_before, nullptr);
  // The map of the call: live_after only. arg_only is an argument and not live after; the
  // result is not assigned until the call returns; raw is no reference.
  ASSERT_EQ(call->live_count, 1u);
  EXPECT_EQ(call->live[0].value, grjit_emit_slot(live_after));
  EXPECT_EQ(call->live[0].slot_kind, GRCORE_SLOT_VALUE);
  EXPECT_EQ(call->derived_count, 0u);
  EXPECT_EQ(exit_before->live_count, 0u) << "the exit leaves the frame, and its state names nothing";
}

/* ---- The native stack ----------------------------------------------------------------- */

TEST(Natives, TheNativeStackIsCheckedAtTheCallSiteForTheStackArgumentsAndTheNativesOwnUseToTheByte) {
  JitWorld w;
  NativeTab t;
  uintptr_t * limit = reinterpret_cast<uintptr_t *>(
      reinterpret_cast<char *>(w.ctx) + grcore_jit_layout()->native_limit_offset);
  for (size_t n : {size_t{0}, size_t{4}, size_t{5}, size_t{6}, size_t{9}, size_t{16}}) {
    for (uint32_t declared : {0u, 100u, 4096u}) {
      std::vector<GRJIT_Type> types(n, TI);
      uint32_t id = t.add(reinterpret_cast<const void *>(grjit_test_align_stub), types, kNoResult, 0, declared);
      Bare bare(t, id, types, kNoResult);
      Compiled c(bare.f, w.pages());
      ASSERT_TRUE(c);
      std::vector<uint64_t> args(n, 1);
      // Where the native's entry is, measured: the stack pointer it records, with no limit.
      *limit = 0;
      grjit_test_entry_count = 0;
      ASSERT_EQ(c.run(w.ctx, args).exit, uint32_t{GRJIT_EXIT_RETURNED});
      ASSERT_EQ(grjit_test_entry_count, 1u);
      const uintptr_t entry = grjit_test_entry_rsp[0];
      // The call is made if the lowest address the native may use (its entry stack pointer,
      // less its own use, counting the return address the entry stack pointer is past) is not
      // below the limit, and is an exit before the call if it is one byte below.
      const uintptr_t lowest = entry + 8 - declared;
      for (long delta : {-1L, 0L, 1L}) {
        *limit = static_cast<uintptr_t>(static_cast<long>(lowest) + delta);
        g_deopts = 0;
        grjit_test_entry_count = 0;
        auto run = c.run(w.ctx, args);
        if (delta <= 0) {
          EXPECT_EQ(run.exit, uint32_t{GRJIT_EXIT_RETURNED}) << n << " arguments, " << declared << " declared, limit " << delta;
          EXPECT_EQ(grjit_test_entry_count, 1u);
          EXPECT_EQ(g_deopts, 0);
        } else {
          EXPECT_EQ(run.exit, uint32_t{GRJIT_EXIT_DEOPT})
              << n << " arguments, " << declared << " declared: one byte past what fits is an exit";
          EXPECT_EQ(grjit_test_entry_count, 0u) << "the native is not called";
          EXPECT_EQ(g_deopts, 1);
          EXPECT_EQ(g_last_cause, 0u) << "an exit before the call is not a native's status";
        }
      }
      *limit = 0;
    }
  }
}

/* ===== The engine's side, played by the fixture ===================================== */

namespace {

Engine & E() { return *g_engine; }

void check_ctx(void * ctx) {
  if (ctx != static_cast<void *>(E().ctx)) {
    E().bad_native_ctx = true;
  }
}

/* What a native saw when it walked the compiled frames below it. */
struct WalkSeen {
  std::vector<u64> fns;          // innermost first
  std::vector<size_t> depths;
  std::vector<uintptr_t> bases;
  std::vector<uintptr_t> saved;  // the word at each frame's base: its caller's base
  size_t roots = 0;              // precise roots reported, each slot once
  size_t duplicate_roots = 0;
  bool broken = false;
  long walks = 0;
};
WalkSeen g_walk;                 // the last walk
std::vector<WalkSeen> g_walks;   // every walk, in order

void walk_now() {
  Engine & e = E();
  g_walk.walks++;
  g_walk.fns.clear();
  g_walk.depths.clear();
  g_walk.bases.clear();
  g_walk.saved.clear();
  g_walk.roots = g_walk.duplicate_roots = 0;
  GRCORE_CompiledWalk w;
  GRCORE_CompiledFrame f;
  if (grcore_compiled_walk_begin(e.ctx, &w) != GRCORE_OK) {
    g_walk.broken = true;
    return;
  }
  GRCORE_CompiledWalkStatus r;
  while ((r = grcore_compiled_walk_next(&w, &f)) == GRCORE_CWALK_FRAME) {
    g_walk.fns.push_back(f.identity.function);
    g_walk.depths.push_back(f.depth);
    g_walk.bases.push_back(f.frame_base);
    g_walk.saved.push_back(*reinterpret_cast<const uintptr_t *>(f.frame_base));
  }
  g_walk.broken = r == GRCORE_CWALK_BROKEN;
  std::set<u64 *> seen;
  struct Cnt {
    std::set<u64 *> * seen;
    size_t * dups;
  } cnt{&seen, &g_walk.duplicate_roots};
  GRCORE_RootVisitor rv = {};
  rv.user = &cnt;
  rv.slot = [](void * user, uint64_t * slot) {
    auto * c = static_cast<Cnt *>(user);
    if (!c->seen->insert(slot).second) {
      (*c->dups)++;
    }
  };
  EXPECT_EQ(grcore_context_enumerate_roots(e.ctx, &rv), GRCORE_OK);
  g_walk.roots = seen.size();
  g_walks.push_back(g_walk);
}

/* ---- The natives ---- */

uint64_t nat_log(void * ctx, uint64_t v) {
  check_ctx(ctx);
  E().trace.push_back(static_cast<int64_t>(v));
  return v + 1000;
}
uint64_t nat_add(void * ctx, uint64_t a, uint64_t b) {
  check_ctx(ctx);
  return a + b;
}
/* An allocation: a GC point (the fixture's torture collects first), and a new object. */
uint64_t nat_new(void * ctx, uint64_t v) {
  check_ctx(ctx);
  Engine & e = E();
  e.st.news++;
  if (e.torture) {
    e.collect();
  }
  return e.heap.alloc(static_cast<int64_t>(v));
}
/* A collection, with nothing held: the call's walk start is all the collector has. */
uint64_t nat_collect(void * ctx) {
  check_ctx(ctx);
  E().collect();
  return 0;
}
/* The same after walking and counting the roots, as a debugger would. */
uint64_t nat_walk_collect(void * ctx) {
  check_ctx(ctx);
  walk_now();
  E().collect();
  return 0; // the interpreter has no compiled frames to count: the answer must not depend on the tier
}
uint64_t nat_walk(void * ctx) {
  check_ctx(ctx);
  walk_now();
  return 0;
}
/* Holds the reference it was given only in its own C frame, under a NATIVE record that
 * gives that frame as a conservative segment: the object is pinned, and intact after
 * the collection. */
__attribute__((noinline)) uint64_t nat_keep(void * ctx, uint64_t ref) {
  check_ctx(ctx);
  Engine & e = E();
  FX_NATIVE_SCOPE(scope, e, GRCORE_ACTIVATION_NATIVE);
  volatile uint64_t mine = ref;
  e.collect();
  return static_cast<uint64_t>(e.heap.read(mine));
}
/* The same without the record: it relies on its argument being updated, which it is not. */
__attribute__((noinline)) uint64_t nat_nokeep(void * ctx, uint64_t ref) {
  check_ctx(ctx);
  Engine & e = E();
  volatile uint64_t mine = ref;
  e.collect();
  return static_cast<uint64_t>(e.heap.read(mine));
}
/* A native with a status: value v * 2 + 1, the status the guest chose. */
GRJIT_NativeResult nat_status(void * ctx, uint64_t v, uint64_t status) {
  check_ctx(ctx);
  E().trace.push_back(static_cast<int64_t>(v));
  return {v * 2 + 1, status};
}
GRJIT_NativeResult nat_unwind(void * ctx, uint64_t v) {
  check_ctx(ctx);
  E().trace.push_back(static_cast<int64_t>(v));
  return {v + 7, GRJIT_NATIVE_UNWIND};
}
GRJIT_NativeResult nat_pause(void * ctx, uint64_t v) {
  check_ctx(ctx);
  E().trace.push_back(static_cast<int64_t>(v));
  E().pause_pending = true;
  return {v + 3, GRJIT_NATIVE_DEOPT};
}
/* Status natives that allocate and collect: the exit the status takes must find the
 * references the call's site kept. */
GRJIT_NativeResult nat_status_collect(void * ctx, uint64_t v, uint64_t status) {
  check_ctx(ctx);
  E().collect();
  return {v * 2 + 1, status};
}
/* Re-entry: a guest function run from inside the native, interpreted or compiled, in its own
 * record. Whatever ends the nested run other than a value is an unwind the caller must see. */
GRJIT_NativeResult reenter(void * ctx, uint64_t fn, uint64_t arg, bool compiled) {
  check_ctx(ctx);
  Engine & e = E();
  FX_NATIVE_SCOPE(scope, e, GRCORE_ACTIVATION_REENTRY);
  if (!scope.ok()) {
    return {0, GRJIT_NATIVE_UNWIND};
  }
  Outcome o = e.run_nested(static_cast<int>(fn), {arg}, compiled);
  if (!o.finished) {
    return {0, GRJIT_NATIVE_UNWIND};
  }
  return {o.value, GRJIT_NATIVE_OK};
}
GRJIT_NativeResult nat_reenter_interp(void * ctx, uint64_t fn, uint64_t arg) {
  return reenter(ctx, fn, arg, false);
}
GRJIT_NativeResult nat_reenter_compiled(void * ctx, uint64_t fn, uint64_t arg) {
  return reenter(ctx, fn, arg, true);
}

/* The ids a test uses. */
struct Nat {
  int log, add, newobj, collect, walk, walk_collect, keep, nokeep, status, unwind, pause,
      status_collect, reenter_i, reenter_c;
};

Nat register_natives(Engine & e) {
  constexpr GRJIT_Type I = GRJIT_TYPE_I64;
  constexpr GRJIT_Type R = GRJIT_TYPE_REF;
  constexpr uint32_t ST = GRJIT_NATIVE_STATUS;
  constexpr uint32_t RE = GRJIT_NATIVE_STATUS | GRJIT_NATIVE_REENTERS;
  Nat n{};
  n.log = e.add_native(reinterpret_cast<const void *>(nat_log), {I}, I, 0, 64, "log");
  n.add = e.add_native(reinterpret_cast<const void *>(nat_add), {I, I}, I, 0, 64, "add");
  n.newobj = e.add_native(reinterpret_cast<const void *>(nat_new), {I}, R, 0, 512, "new");
  n.collect = e.add_native(reinterpret_cast<const void *>(nat_collect), {}, I, 0, 2048, "collect");
  n.walk = e.add_native(reinterpret_cast<const void *>(nat_walk), {}, I, 0, 2048, "walk");
  n.walk_collect = e.add_native(reinterpret_cast<const void *>(nat_walk_collect), {}, I, 0, 4096, "walk_collect");
  n.keep = e.add_native(reinterpret_cast<const void *>(nat_keep), {R}, I, 0, 4096, "keep");
  n.nokeep = e.add_native(reinterpret_cast<const void *>(nat_nokeep), {R}, I, 0, 4096, "nokeep");
  n.status = e.add_native(reinterpret_cast<const void *>(nat_status), {I, I}, I, ST, 64, "status");
  n.unwind = e.add_native(reinterpret_cast<const void *>(nat_unwind), {I}, I, ST, 64, "unwind");
  n.pause = e.add_native(reinterpret_cast<const void *>(nat_pause), {I}, I, ST, 64, "pause");
  n.status_collect = e.add_native(reinterpret_cast<const void *>(nat_status_collect), {I, I}, I, ST, 4096, "status_collect");
  n.reenter_i = e.add_native(reinterpret_cast<const void *>(nat_reenter_interp), {I, I}, I, RE, 1024, "reenter_i");
  n.reenter_c = e.add_native(reinterpret_cast<const void *>(nat_reenter_compiled), {I, I}, I, RE, 1024, "reenter_c");
  return n;
}

/* A program run in both tiers on one engine, with the trace the natives logged in each. */
struct Pair {
  Outcome i, c;
  std::vector<int64_t> ti, tc;
};

Pair run_both(Engine & e, int fn, const std::vector<u64> & args) {
  Pair p;
  e.trace.clear();
  bool torture = e.torture;
  bool moving = e.heap.moving;
  e.torture = false;
  e.heap.moving = false; // nothing in the interpreted run moves: its derived pointers are raw
  p.i = e.run_interpreted(fn, args);
  e.torture = torture;
  e.heap.moving = moving;
  p.ti = e.trace;
  e.trace.clear();
  p.c = e.run_compiled(fn, args);
  p.tc = e.trace;
  return p;
}

} // namespace

/* ---- Compiled code calls natives without leaving it ---------------------------------------- */

namespace {

/* leaf(x) = add(log(x), x); mid(x) = log(leaf(x + 1)); top(x) = add(mid(x), 5) */
int add_native_chain(Engine & e, const Nat & n) {
  int leaf = e.reserve(), mid = e.reserve(), top = e.reserve();
  {
    P p("leaf", {GRJIT_TYPE_I64});
    int r = p.local(), s = p.local();
    p.native(r, n.log, {0});
    p.native(s, n.add, {r, 0});
    p.ret(s);
    e.set(leaf, p.done());
  }
  {
    P p("mid", {GRJIT_TYPE_I64});
    int y = p.local(), one = p.local(), r = p.local(), t = p.local();
    p.cnst(one, 1);
    p.bin(K::ADD, y, 0, one);
    p.call(r, leaf, {y});
    p.native(t, n.log, {r});
    p.ret(t);
    e.set(mid, p.done());
  }
  {
    P p("top", {GRJIT_TYPE_I64});
    int r = p.local(), u = p.local(), five = p.local();
    p.call(r, mid, {0});
    p.cnst(five, 5);
    p.native(u, n.add, {r, five});
    p.ret(u);
    e.set(top, p.done());
  }
  return top;
}

} // namespace

TEST(Natives, NativesCalledFromACompiledChainRunInTheInterpretersOrderAndNoCallLeavesCompiledCode) {
  Engine e;
  Nat n = register_natives(e);
  int top = add_native_chain(e, n);
  for (u64 x : {u64{0}, u64{5}, u64{1234}}) {
    Pair p = run_both(e, top, {x});
    ASSERT_TRUE(p.i.finished);
    ASSERT_TRUE(p.c.finished);
    EXPECT_EQ(p.c.value, p.i.value);
    // log(x + 1) + (x + 1) = 2x + 1002 ; log of that = 2x + 2002 ; plus 5
    EXPECT_EQ(p.c.value, 2 * x + 2002 + 5) << "against the reference worked by hand";
    EXPECT_EQ(p.tc, p.ti) << "the natives ran in the same order, once each";
    EXPECT_EQ(p.tc.size(), 2u);
    EXPECT_EQ(p.c.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(p.c.frames_left, 0u);
  }
  EXPECT_EQ(e.st.deopts, 0) << "no call, guest or native, left compiled code";
  EXPECT_FALSE(e.bad_native_ctx) << "every native got the context the code was called with";
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

namespace {

/* deep(n): a frame holding a reference and a pointer derived from it, over a chain of
 * calls, with natives at the bottom: one that holds the bottom object only in its own C
 * frame, and one that walks. The sum over the frames of twice each object's value, plus
 * what the native read. */
int add_deep(Engine & e, const Nat & n, bool with_keep) {
  int deep = e.reserve();
  P p("deep", {GRJIT_TYPE_I64});
  int obj = p.local(GRJIT_TYPE_REF), dp = p.local(GRJIT_TYPE_PTR), zero = p.local(), one = p.local(),
      t = p.local(), nn = p.local(), r = p.local(), v = p.local(), w = p.local(), sum = p.local(),
      k = p.local();
  p.nw(obj, 1);
  p.derive(dp, obj, 8);
  p.cnst(zero, 0);
  p.cnst(one, 1);
  p.bin(K::EQ, t, 0, zero);
  int br = p.brz(t);
  if (with_keep) {
    p.native(k, n.keep, {obj});
  } else {
    p.cnst(k, 0);
  }
  p.native(w, n.walk_collect, {});
  p.bin(K::ADD, r, k, w);
  int join = p.br();
  p.patch(br, p.here());
  p.bin(K::SUB, nn, 0, one);
  p.call(r, deep, {nn});
  p.patch(join, p.here());
  p.get(v, obj);
  p.load(w, dp);
  p.bin(K::ADD, sum, r, v);
  p.bin(K::ADD, sum, sum, w);
  p.ret(sum);
  e.set(deep, p.done());
  return deep;
}

} // namespace

TEST(Natives, ANativeThatAllocatesAndCollectsUnderAFiftyDeepChainFindsEveryFrameAndUpdatesEveryReferenceOnce) {
  Engine e;
  e.torture = true;
  e.interpreter_never_moves = true;
  Nat n = register_natives(e);
  int deep = add_deep(e, n, true);
  g_walks.clear();
  Pair p = run_both(e, deep, {49});
  ASSERT_TRUE(p.i.finished);
  ASSERT_TRUE(p.c.finished);
  // Each frame reads its own object twice (the reference and the derived pointer): the object
  // of frame n holds 1, whatever the frame's n; the bottom native read 1 and walked.
  EXPECT_EQ(p.c.value, p.i.value) << "the same as the interpreted run's";
  EXPECT_EQ(p.c.value, uint64_t{50 * 2 + 1});
  EXPECT_EQ(e.heap.poisoned_reads, 0) << "a reference left out of a site's map reads the poison";
  EXPECT_GT(e.heap.moved, 100) << "the collector moved things";
  ASSERT_FALSE(g_walks.empty());
  const WalkSeen & w = g_walks.back();
  EXPECT_FALSE(w.broken);
  ASSERT_EQ(w.fns.size(), 50u) << "every compiled frame, once, from inside the native";
  for (size_t k = 0; k < w.fns.size(); k++) {
    EXPECT_EQ(w.fns[k], static_cast<u64>(deep));
    EXPECT_EQ(w.depths[k], k) << "the innermost frame is the caller of the native";
    if (k + 1 < w.fns.size()) {
      EXPECT_EQ(w.saved[k], w.bases[k + 1]);
    } else {
      EXPECT_EQ(w.saved[k], uintptr_t{GRCORE_COMPILED_CHAIN_END});
    }
  }
  EXPECT_EQ(w.roots, 50u) << "one reference per frame, reported once";
  EXPECT_EQ(w.duplicate_roots, 0u);
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_FALSE(e.bad_native_ctx);
}

TEST(Natives, ANativeThatRelaysOnItsArgumentAfterACollectionReadsPoisonAndOneThatKeepsItInItsRecordDoesNot) {
  // The control for the pin: without the record the object moves and the stale argument reads the
  // poison, so the pin in the test above is what kept it intact and not luck.
  for (bool keep : {false, true}) {
    Engine e;
    e.torture = false;
    Nat n = register_natives(e);
    int f = e.reserve();
    P p("holder", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), r = p.local(), v = p.local(), sum = p.local();
    p.nw(obj, 41);
    p.native(r, keep ? n.keep : n.nokeep, {obj});
    p.get(v, obj);
    p.bin(K::ADD, sum, r, v);
    p.ret(sum);
    e.set(f, p.done());
    Outcome o = e.run_compiled(f, {0});
    ASSERT_TRUE(o.finished);
    if (keep) {
      EXPECT_EQ(o.value, 82u) << "the pinned object is intact, and the frame's own reference still names it";
      EXPECT_EQ(e.heap.poisoned_reads, 0);
    } else {
      EXPECT_GT(e.heap.poisoned_reads, 0) << "an argument is not updated: the native that relied on it read the poison";
      EXPECT_EQ(o.value, static_cast<u64>(kPoisonValue) + 41u);
    }
  }
}

namespace {

/* A, which calls the walking native, then a chain of B, C, D whose bottom walks, then walks
 * again: the third walk must start at A's frame and not at the frame the second left in the
 * cell. */
int add_walks(Engine & e, const Nat & n, int * a_out, int * d_out) {
  int a = e.reserve(), b = e.reserve(), c = e.reserve(), d = e.reserve();
  {
    P p("D", {GRJIT_TYPE_I64});
    int r = p.local();
    p.native(r, n.walk, {});
    p.ret(r);
    e.set(d, p.done());
  }
  {
    P p("C", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, d, {0});
    p.ret(r);
    e.set(c, p.done());
  }
  {
    P p("B", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, c, {0});
    p.ret(r);
    e.set(b, p.done());
  }
  {
    P p("A", {GRJIT_TYPE_I64});
    int r1 = p.local(), r2 = p.local(), r3 = p.local(), s = p.local();
    p.native(r1, n.walk, {});
    p.call(r2, b, {0});
    p.native(r3, n.walk, {});
    p.bin(K::ADD, s, r1, r2);
    p.bin(K::ADD, s, s, r3);
    p.ret(s);
    e.set(a, p.done());
  }
  *a_out = a;
  *d_out = d;
  return a;
}

} // namespace

TEST(Natives, TheWalkStartIsThisCallsAndNotTheStaleOneAnotherChainLeftInTheCell) {
  Engine e;
  Nat n = register_natives(e);
  int a, d;
  add_walks(e, n, &a, &d);
  g_walks.clear();
  Outcome o = e.run_compiled(a, {0});
  ASSERT_TRUE(o.finished);
  ASSERT_EQ(g_walks.size(), 3u);
  EXPECT_EQ(g_walks[0].fns, (std::vector<u64>{static_cast<u64>(a)})) << "A's own frame, and nothing below it";
  EXPECT_EQ(g_walks[1].fns,
      (std::vector<u64>{static_cast<u64>(d), static_cast<u64>(a + 2), static_cast<u64>(a + 1), static_cast<u64>(a)}))
      << "D, C, B, A";
  EXPECT_EQ(g_walks[2].fns, (std::vector<u64>{static_cast<u64>(a)}))
      << "after the deeper chain returned, the cell is A's again and not D's";
  for (const WalkSeen & w : g_walks) {
    EXPECT_FALSE(w.broken);
    EXPECT_EQ(w.saved.back(), uintptr_t{GRCORE_COMPILED_CHAIN_END});
  }
  EXPECT_EQ(o.value, 0u) << "the walking native answers 0 in both tiers";
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- Status ----------------------------------------------------------------------------------- */

namespace {

/* f1(x, s) calls f2(x + 1, s) calls f3(x + 2, s), which logs the native's call and returns
 * log(status(x, s)) + 1; f2 and f1 add their own x on the way up, so a frame that was not
 * rebuilt shows in the result. */
int add_status_chain(Engine & e, const Nat & n, int status_native, int * f3_out = nullptr) {
  int f1 = e.reserve(), f2 = e.reserve(), f3 = e.reserve();
  {
    P p("f3", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int v = p.local(), w = p.local(), one = p.local(), r = p.local();
    p.native(v, status_native, {0, 1});
    p.native(w, n.log, {v});
    p.cnst(one, 1);
    p.bin(K::ADD, r, w, one);
    p.ret(r);
    e.set(f3, p.done());
  }
  {
    P p("f2", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int y = p.local(), one = p.local(), r = p.local(), t = p.local();
    p.cnst(one, 1);
    p.bin(K::ADD, y, 0, one);
    p.call(r, f3, {y, 1});
    p.bin(K::ADD, t, r, 0);
    p.ret(t);
    e.set(f2, p.done());
  }
  {
    P p("f1", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int y = p.local(), one = p.local(), r = p.local(), t = p.local();
    p.cnst(one, 1);
    p.bin(K::ADD, y, 0, one);
    p.call(r, f2, {y, 1});
    p.bin(K::ADD, t, r, 0);
    p.ret(t);
    e.set(f1, p.done());
  }
  if (f3_out != nullptr) {
    *f3_out = f3;
  }
  return f1;
}

} // namespace

TEST(Natives, AStatusOfZeroContinuesInCompiledCodeAndAnyOtherLeavesThroughTheChainWithTheNativeBitAndTheNativeRunOnce) {
  struct Case {
    u64 status;
    u64 cause_low; // the low thirty-two bits the cause carries
    bool leaves;
  };
  const Case cases[] = {
      {0, 0, false},
      {GRJIT_NATIVE_DEOPT, 1, true},
      {GRJIT_NATIVE_UNWIND + 100, 102, true}, // a status the library does not know: the engine's
      {77, 77, true},
      {(u64{1} << 40) | 9, 9, true},          // the cause keeps the low thirty-two bits
      {u64{1} << 40, 0, true},                // the test for leaving is on all sixty-four
      {0xFFFFFFFFull, 0xFFFFFFFFull, true},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.status);
    Engine e2;
    Nat n2 = register_natives(e2);
    int g1 = add_status_chain(e2, n2, n2.status);
    // The status is an argument all the way down; f3 is the one that calls the native.
    Pair p = run_both(e2, g1, {10, c.status});
    ASSERT_TRUE(p.i.finished);
    ASSERT_TRUE(p.c.finished);
    // x = 10: f3 has x = 12: status(12, s) = 25; log -> 1025; + 1; the callers add their own x.
    EXPECT_EQ(p.c.value, p.i.value);
    EXPECT_EQ(p.c.value, 1026u + 11u + 10u);
    EXPECT_EQ(p.tc, p.ti) << "the native ran exactly once: the exit used the state after it";
    EXPECT_EQ(p.tc, (std::vector<int64_t>{12, 25}));
    if (c.leaves) {
      EXPECT_EQ(p.c.exit, uint32_t{GRJIT_EXIT_DEOPT});
      EXPECT_EQ(e2.st.deopts, 1);
      EXPECT_EQ(e2.st.status_exits, 1);
      EXPECT_EQ(e2.st.last_cause, GRJIT_CAUSE_NATIVE | c.cause_low) << "the native bit and the status";
      EXPECT_EQ(e2.st.deopt_frames, 3) << "every frame of the chain is rebuilt";
      EXPECT_TRUE(p.c.interpreted_rest);
      EXPECT_EQ(e2.st.native_exits, 0) << "this is the exit after the call and not the one before it";
    } else {
      EXPECT_EQ(p.c.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(e2.st.deopts, 0);
    }
    EXPECT_EQ(e2.st.pushes, e2.st.pops);
    EXPECT_EQ(p.c.frames_left, 0u);
  }
}

TEST(Natives, TheResultIsInPlaceBeforeTheStatusIsLookedAtSoTheInterpreterContinuesWithItAndAPendingReferenceIsUpdated) {
  // A native that allocates returns a reference and a status: the exit rebuilds the frame with
  // the reference in its destination slot, and a collection the exit's rebuild is preceded by
  // (the native's own) has updated every reference the state names.
  Engine e;
  e.torture = true;
  e.interpreter_never_moves = true;
  Nat n = register_natives(e);
  int status_alloc = e.add_native(reinterpret_cast<const void *>(+[](void * ctx, uint64_t v, uint64_t s) -> GRJIT_NativeResult {
                                    check_ctx(ctx);
                                    Engine & en = E();
                                    en.collect();
                                    return {en.heap.alloc(static_cast<int64_t>(v) + 5), s};
                                  }),
      {GRJIT_TYPE_I64, GRJIT_TYPE_I64}, GRJIT_TYPE_REF, GRJIT_NATIVE_STATUS, 4096, "status_alloc");
  (void)n;
  int f = e.reserve();
  P p("f", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
  int keep = p.local(GRJIT_TYPE_REF), dp = p.local(GRJIT_TYPE_PTR), res = p.local(GRJIT_TYPE_REF), a = p.local(),
      b = p.local(), sum = p.local();
  p.nw(keep, 100);
  p.derive(dp, keep, 8);
  p.native(res, status_alloc, {0, 1});
  p.get(a, res);
  p.get(b, keep);
  p.load(sum, dp);
  p.bin(K::ADD, a, a, b);
  p.bin(K::ADD, a, a, sum);
  p.ret(a);
  e.set(f, p.done());
  for (u64 status : {u64{0}, u64{1}}) {
    Pair r = run_both(e, f, {20, status});
    ASSERT_TRUE(r.i.finished);
    ASSERT_TRUE(r.c.finished);
    EXPECT_EQ(r.c.value, r.i.value);
    EXPECT_EQ(r.c.value, 25u + 100u + 100u) << "the result's object, the reference kept across the call and the derived pointer";
    EXPECT_EQ(r.c.exit, status == 0 ? uint32_t{GRJIT_EXIT_RETURNED} : uint32_t{GRJIT_EXIT_DEOPT});
  }
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 0);
}

namespace {

/* A (catches) calls B calls C, which calls the unwinding native; each has `i64s` integer
 * locals besides its parameter, so that conversions say how many frames were rebuilt. */
int add_unwind_chain(Engine & e, const Nat & n, int native, bool a_catches) {
  int a = e.reserve(), b = e.reserve(), c = e.reserve();
  {
    P p("C", {GRJIT_TYPE_I64});
    int v = p.local();
    p.native(v, native, {0});
    p.ret(v);
    e.set(c, p.done());
  }
  {
    P p("B", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, c, {0});
    p.ret(r);
    e.set(b, p.done());
  }
  {
    P p("A", {GRJIT_TYPE_I64});
    int r = p.local(), hundred = p.local(), s = p.local();
    p.call(r, b, {0});
    p.cnst(hundred, 100);
    p.bin(K::ADD, s, r, hundred);
    p.ret(s);
    Func f = p.done();
    f.catches = a_catches;
    e.set(a, f);
  }
  (void)n;
  return a;
}

} // namespace

TEST(Natives, AnUnwindRebuildsOnlyTheSurvivorsAndPopsTheRestWithoutConvertingThem) {
  // A converting engine: every I64 local is boxed in a guest frame, so a rebuilt frame converts
  // each of its raw words. A has four I64 locals (the parameter, r, the constant and s), B and C
  // two each.
  for (bool with_scope : {true, false}) {
    SCOPED_TRACE(with_scope);
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true);
    Nat n = register_natives(e);
    int a = add_unwind_chain(e, n, n.unwind, with_scope);
    long conv_i = 0;
    e.trace.clear();
    Outcome oi = e.run_interpreted(a, {5});
    auto ti = e.trace;
    conv_i = e.st.conversions;
    EXPECT_EQ(conv_i, 0) << "the interpreter converts nothing";
    e.trace.clear();
    Outcome oc = e.run_compiled(a, {5});
    auto tc = e.trace;
    EXPECT_EQ(tc, ti) << "the native ran once";
    EXPECT_EQ(oc.frames_left, 0u);
    EXPECT_EQ(oi.frames_left, 0u);
    if (with_scope) {
      ASSERT_TRUE(oi.finished);
      ASSERT_TRUE(oc.finished);
      EXPECT_EQ(oc.value, oi.value);
      EXPECT_EQ(oc.value, kUnwound + 100u) << "A resumed after its call with the scope's value";
      EXPECT_EQ(e.st.conversions, 4) << "only the survivor, A's four locals, was converted; B and C were popped";
      EXPECT_EQ(e.st.deopt_frames, 1);
    } else {
      EXPECT_TRUE(oi.unwound);
      EXPECT_TRUE(oc.unwound) << "no scope caught it: the run ended";
      EXPECT_FALSE(oc.finished);
      EXPECT_EQ(e.st.conversions, 0) << "nothing survived, nothing was converted";
      EXPECT_EQ(e.st.deopt_frames, 0);
    }
    EXPECT_EQ(e.st.unwinds, 2) << "one in each tier";
    EXPECT_EQ(e.st.status_exits, 1);
    EXPECT_EQ(e.st.last_cause, GRJIT_CAUSE_NATIVE | GRJIT_NATIVE_UNWIND);
    EXPECT_EQ(e.st.pushes, e.st.pops) << "every guest frame is popped, converted or not";
  }
  // The control for the count: a DEOPT status rebuilds the whole chain.
  {
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, true);
    Nat n = register_natives(e);
    int st = e.add_native(reinterpret_cast<const void *>(+[](void * ctx, uint64_t v) -> GRJIT_NativeResult {
                            check_ctx(ctx);
                            return {v, GRJIT_NATIVE_DEOPT};
                          }),
        {GRJIT_TYPE_I64}, GRJIT_TYPE_I64, GRJIT_NATIVE_STATUS, 64, "deopt_only");
    int a = add_unwind_chain(e, n, st, true);
    Outcome oc = e.run_compiled(a, {5});
    ASSERT_TRUE(oc.finished);
    EXPECT_EQ(oc.value, 105u);
    EXPECT_EQ(e.st.conversions, 4 + 2 + 2) << "all three frames were converted";
    EXPECT_EQ(e.st.deopt_frames, 3);
  }
}

TEST(Natives, ANativeThatLeavesAPausePendingAndReturnsDeoptRebuildsTheChainAndTheRunResumesOnAnotherThread) {
  Engine e;
  Nat n = register_natives(e);
  int f = e.reserve(), g = e.reserve();
  {
    P p("g", {GRJIT_TYPE_I64});
    int v = p.local(), w = p.local(), one = p.local();
    p.native(v, n.pause, {0});
    p.poll();
    p.native(w, n.log, {v});
    p.cnst(one, 1);
    p.bin(K::ADD, w, w, one);
    p.ret(w);
    e.set(g, p.done());
  }
  {
    P p("f", {GRJIT_TYPE_I64});
    int r = p.local(), two = p.local();
    p.call(r, g, {0});
    p.cnst(two, 2);
    p.bin(K::ADD, r, r, two);
    p.ret(r);
    e.set(f, p.done());
  }
  // The uninterrupted run, interpreted, paused where the poll is and resumed at once.
  e.trace.clear();
  Outcome ref = e.run_interpreted(f, {9});
  ASSERT_TRUE(ref.paused) << "the native left a pause pending, and the interpreter pauses at its next poll";
  EXPECT_GT(ref.frames_left, 0u);
  Outcome ref_done = e.resume();
  ASSERT_TRUE(ref_done.finished);
  const auto ref_trace = e.trace;
  // The compiled run: the native returns DEOPT, the chain is rebuilt, the interpreter pauses.
  e.trace.clear();
  Outcome o = e.run_compiled(f, {9});
  ASSERT_TRUE(o.paused);
  EXPECT_FALSE(o.finished);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT});
  EXPECT_EQ(e.st.deopt_frames, 2) << "both frames were rebuilt";
  EXPECT_EQ(o.frames_left, 2u) << "the guest frames are what the host holds while paused";
  EXPECT_EQ(e.trace, (std::vector<int64_t>{9})) << "the native has run, once";
  // Resumed on another thread (under TSan, the hand-over is what is checked).
  ASSERT_EQ(grcore_context_release(e.ctx), GRCORE_OK);
  Outcome resumed;
  std::thread t([&] {
    EXPECT_EQ(grcore_context_acquire(e.ctx), GRCORE_OK);
    resumed = e.resume();
    EXPECT_EQ(grcore_context_release(e.ctx), GRCORE_OK);
  });
  t.join();
  ASSERT_EQ(grcore_context_acquire(e.ctx), GRCORE_OK);
  ASSERT_TRUE(resumed.finished);
  EXPECT_EQ(resumed.value, ref_done.value) << "what the uninterrupted run prints";
  EXPECT_EQ(e.trace, ref_trace);
  EXPECT_EQ(resumed.frames_left, 0u);
  EXPECT_EQ(e.st.pushes, e.st.pops);
}

/* ---- Re-entry: a native that runs guest code, nested ----------------------------------------- */

namespace {

/* outer(x) = reenter(G, x) + its own object twice (the reference and a derived pointer into it);
 * top(x) = outer(x) + 1000. The chain top -> outer is compiled, and the native under outer
 * re-enters. */
int add_reenter_chain(Engine & e, const Nat & n, int reenter_native, int g) {
  int outer = e.reserve(), top = e.reserve();
  {
    P p("outer", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), dp = p.local(GRJIT_TYPE_PTR), gid = p.local(), r = p.local(),
        v = p.local(), t = p.local(), sum = p.local();
    p.nw(obj, 40);
    p.derive(dp, obj, 8);
    p.cnst(gid, g);
    p.native(r, reenter_native, {gid, 0});
    p.get(v, obj);
    p.load(t, dp);
    p.bin(K::ADD, sum, r, v);
    p.bin(K::ADD, sum, sum, t);
    p.ret(sum);
    e.set(outer, p.done());
  }
  {
    P p("top", {GRJIT_TYPE_I64});
    int r = p.local(), k = p.local(), s = p.local();
    p.call(r, outer, {0});
    p.cnst(k, 1000);
    p.bin(K::ADD, s, r, k);
    p.ret(s);
    e.set(top, p.done());
  }
  (void)n;
  return top;
}

} // namespace

TEST(Natives, ANativeThatRunsTheInterpreterInItsOwnRecordLeavesTheOuterChainDescribedAndUpdatedAcrossItsCollections) {
  Engine e;
  e.torture = true;
  // G has an object it fills from its parameter: a second version of it, built with NEW of an
  // immediate and a native that allocates from the argument.
  Nat n = register_natives(e);
  int g = e.reserve();
  {
    P p("G", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), w = p.local(), v = p.local(), r = p.local();
    p.native(obj, n.newobj, {0});
    p.native(w, n.walk, {});
    p.collect();
    p.get(v, obj);
    p.bin(K::ADD, r, v, 0);
    p.ret(r);
    e.set(g, p.done());
  }
  int top = add_reenter_chain(e, n, n.reenter_i, g);
  for (u64 x : {u64{0}, u64{7}}) {
    g_walks.clear();
    // The interpreter's own run is the reference; it nests the same way.
    Pair p = run_both(e, top, {x});
    ASSERT_TRUE(p.i.finished);
    ASSERT_TRUE(p.c.finished);
    EXPECT_EQ(p.c.value, p.i.value);
    EXPECT_EQ(p.c.value, 1000u + 2u * 40u + 2u * x) << "G(x) = 2x, and outer adds its object twice";
    EXPECT_EQ(p.c.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(p.c.frames_left, 0u);
    // The walk from inside the nested interpreted run sees the outer compiled chain, through the
    // record below the native's: outer, then top.
    ASSERT_GE(g_walks.size(), 2u);
    const WalkSeen & w = g_walks.back();
    EXPECT_FALSE(w.broken);
    EXPECT_EQ(w.fns.size(), 2u);
    ASSERT_EQ(w.fns.size(), 2u);
    EXPECT_EQ(w.fns[0], static_cast<u64>(top - 1));
    EXPECT_EQ(w.fns[1], static_cast<u64>(top));
    EXPECT_EQ(w.saved.back(), uintptr_t{GRCORE_COMPILED_CHAIN_END});
  }
  EXPECT_EQ(e.heap.poisoned_reads, 0) << "the outer frames' references and the derived pointer were updated";
  EXPECT_GT(e.heap.moved, 0);
  EXPECT_EQ(e.st.deopts, 0) << "the outer chain never left compiled code";
  EXPECT_FALSE(e.bad_native_ctx);
  EXPECT_EQ(grcore_activation_count(e.stack), 0u) << "every record is left";
}

TEST(Natives, ANativeThatRunsCompiledCodeAndWhoseNestedRunDeoptimizesRebuildsOnlyTheNestedChain) {
  Engine e;
  e.torture = true;
  Nat n = register_natives(e);
  // G -> H -> I, where I's guard fails: the nested run rebuilds G, H and I, the nested wrapper
  // finishes them in the interpreter, and the native returns the value.
  int g = e.reserve(), h = e.reserve(), i = e.reserve(), after = e.reserve(), bump = e.reserve(),
      start = e.reserve();
  {
    P p("I", {GRJIT_TYPE_I64});
    int c = p.local(), k = p.local(), r = p.local();
    p.cnst(k, 5);
    p.bin(K::LT, c, 0, k); // x < 5: fails for the arguments below, which are 9 and up
    p.guard(c);
    p.cnst(r, 1000);
    p.bin(K::ADD, r, r, 0);
    p.ret(r);
    e.set(i, p.done());
  }
  {
    P p("H", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, i, {0});
    p.ret(r);
    e.set(h, p.done());
  }
  {
    P p("G", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), r = p.local(), v = p.local(), s = p.local();
    p.nw(obj, 3);
    p.call(r, h, {0});
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.ret(s);
    e.set(g, p.done());
  }
  {
    // after(x) runs once the native returned: it is called from the outer compiled frame.
    P p("after", {GRJIT_TYPE_I64});
    int one = p.local(), r = p.local();
    p.cnst(one, 1);
    p.bin(K::ADD, r, 0, one);
    p.ret(r);
    e.set(after, p.done());
  }
  {
    P p("outer", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), gid = p.local(), arg = p.local(), nine = p.local(), r = p.local(),
        v = p.local(), a = p.local(), sum = p.local();
    p.nw(obj, 10);
    p.cnst(gid, g);
    p.cnst(nine, 9);
    p.bin(K::ADD, arg, 0, nine);
    p.native(r, n.reenter_c, {gid, arg});
    p.call(a, after, {r});
    p.get(v, obj);
    p.bin(K::ADD, sum, a, v);
    p.ret(sum);
    e.set(bump, p.done());
  }
  {
    P p("start", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, bump, {0});
    p.ret(r);
    e.set(start, p.done());
  }
  for (u64 x : {u64{0}, u64{4}}) {
    // x = 0 passes 9 to G and x = 4 passes 13: the guard rejects both. The reference run nests the
    // same compiled run (the native is the same), so it deoptimizes once too.
    e.trace.clear();
    const long before_ref = e.st.deopts;
    Outcome ref = e.run_interpreted(start, {x});
    ASSERT_TRUE(ref.finished);
    EXPECT_EQ(e.st.deopts - before_ref, 1) << "the interpreted outer run nests the same compiled run";
    const long d1 = e.st.deopts, f1 = e.st.deopt_frames;
    Outcome o = e.run_compiled(start, {x});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, ref.value);
    // I returns 1000 + (9 + x), G adds its object's 3, after adds 1, outer adds its own object's 10.
    EXPECT_EQ(o.value, 1000u + 9u + x + 3u + 1u + 10u);
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "the outer run never left compiled code";
    EXPECT_EQ(e.st.deopts - d1, 1) << "one deoptimization: the nested run's guard";
    EXPECT_EQ(e.st.deopt_frames - f1, 3) << "the nested chain, G, H and I, and not the outer one";
  }
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_EQ(e.st.native_exits, 0);
  EXPECT_EQ(grcore_activation_count(e.stack), 0u);
}

/* ---- Two levels, a pause inside nested code, code freed under a native ------------------------ */

namespace {

/* A is three frames (a1 catches, a2, a3), whose bottom re-enters compiled code at B, three frames
 * (b1, b2, b3) whose bottom calls `inner`, a native with a status. Every frame has one object, so
 * a collection moves things in both runs. */
struct TwoLevels {
  int a1, a2, a3, b1, b2, b3;
};

TwoLevels add_two_levels(Engine & e, const Nat & n, int inner_native, bool a1_catches) {
  TwoLevels t{};
  t.a1 = e.reserve();
  t.a2 = e.reserve();
  t.a3 = e.reserve();
  t.b1 = e.reserve();
  t.b2 = e.reserve();
  t.b3 = e.reserve();
  auto frame = [&](const char * name, int self, int callee, bool catches) {
    P p(name, {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), r = p.local(), v = p.local(), s = p.local(), one = p.local();
    p.nw(obj, 1);
    p.cnst(one, 1);
    p.call(r, callee, {0});
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.bin(K::ADD, s, s, one);
    p.ret(s);
    Func f = p.done();
    f.catches = catches;
    e.set(self, f);
  };
  frame("a1", t.a1, t.a2, a1_catches);
  frame("a2", t.a2, t.a3, false);
  frame("b1", t.b1, t.b2, false);
  frame("b2", t.b2, t.b3, false);
  {
    P p("a3", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), r = p.local(), v = p.local(), s = p.local(), bid = p.local(), one = p.local();
    p.nw(obj, 1);
    p.cnst(one, 1);
    p.cnst(bid, t.b1);
    p.native(r, n.reenter_c, {bid, 0});
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.bin(K::ADD, s, s, one);
    p.ret(s);
    e.set(t.a3, p.done());
  }
  {
    P p("b3", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), r = p.local(), v = p.local(), s = p.local(), one = p.local();
    p.nw(obj, 1);
    p.cnst(one, 1);
    p.native(r, inner_native, {0});
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.bin(K::ADD, s, s, one);
    p.ret(s);
    e.set(t.b3, p.done());
  }
  return t;
}

} // namespace

TEST(Natives, ATwoLevelCaseRebuildsOnlyTheNestedChainForADeoptStatusAndTheOuterSurvivorsForAnUnwind) {
  // DEOPT in the nested run: B's three frames are rebuilt and finished by the nested interpreter, the
  // native under A returns OK, and A's three frames finish compiled.
  {
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, true);
    e.torture = true;
    Nat n = register_natives(e);
    int inner = e.add_native(reinterpret_cast<const void *>(+[](void * ctx, uint64_t v) -> GRJIT_NativeResult {
                               check_ctx(ctx);
                               E().trace.push_back(static_cast<int64_t>(v));
                               return {v + 20, GRJIT_NATIVE_DEOPT};
                             }),
        {GRJIT_TYPE_I64}, GRJIT_TYPE_I64, GRJIT_NATIVE_STATUS, 64, "inner_deopt");
    TwoLevels t = add_two_levels(e, n, inner, true);
    e.trace.clear();
    Outcome ref = e.run_interpreted(t.a1, {5});
    ASSERT_TRUE(ref.finished);
    const auto ref_trace = e.trace;
    const long d0 = e.st.deopts, f0 = e.st.deopt_frames, c0 = e.st.conversions;
    e.trace.clear();
    Outcome o = e.run_compiled(t.a1, {5});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, ref.value);
    EXPECT_EQ(e.trace, ref_trace) << "the native ran once";
    EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "the outer run never left compiled code";
    // The reference's own nested compiled run deoptimizes (one DEOPT status, three frames) and so
    // does the compiled run's: the counters since d0 include only the compiled outer run's.
    EXPECT_EQ(e.st.deopts - d0, 1);
    EXPECT_EQ(e.st.deopt_frames - f0, 3) << "B only; A's frames are untouched until the native returns";
    EXPECT_EQ(e.st.conversions - c0, 3 * 5) << "each of B's three frames converts its five I64 locals";
    EXPECT_EQ(e.st.status_exits, 2);
  }
  // UNWIND in the nested run: no scope in B, so the nested run is unwound whole, the native under A
  // returns UNWIND, and A's survivors, a1 alone (it catches), are rebuilt.
  {
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, true);
    e.torture = true;
    Nat n = register_natives(e);
    TwoLevels t = add_two_levels(e, n, n.unwind, true);
    e.trace.clear();
    Outcome ref = e.run_interpreted(t.a1, {5});
    ASSERT_TRUE(ref.finished);
    const auto ref_trace = e.trace;
    const long d0 = e.st.deopts, f0 = e.st.deopt_frames, c0 = e.st.conversions;
    e.trace.clear();
    Outcome o = e.run_compiled(t.a1, {5});
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.value, ref.value);
    // a1 resumed after its call with kUnwound, plus its object's 1 and 1.
    EXPECT_EQ(o.value, kUnwound + 1u + 1u);
    EXPECT_EQ(e.trace, ref_trace);
    EXPECT_EQ(e.st.deopts - d0, 2) << "the nested run's, and A's";
    EXPECT_EQ(e.st.deopt_frames - f0, 1) << "the nested run had nothing to keep; A kept a1";
    EXPECT_EQ(e.st.conversions - c0, 1 * 5) << "a1 alone is converted: its five I64 locals";
    EXPECT_EQ(e.st.pushes, e.st.pops);
    EXPECT_EQ(e.heap.poisoned_reads, 0);
  }
}

TEST(Natives, APauseVerdictInsideANestedRunIsALimitUnwindAndNoPauseReachesTheHost) {
  for (bool compiled : {false, true}) {
    SCOPED_TRACE(compiled);
    Engine e;
    Nat n = register_natives(e);
    int g = e.reserve(), outer = e.reserve(), top = e.reserve();
    {
      P p("G", {GRJIT_TYPE_I64});
      int r = p.local(), one = p.local();
      p.poll();
      p.cnst(one, 1);
      p.bin(K::ADD, r, 0, one);
      p.ret(r);
      e.set(g, p.done());
    }
    {
      P p("outer", {GRJIT_TYPE_I64});
      int gid = p.local(), r = p.local(), s = p.local(), ten = p.local();
      p.cnst(gid, g);
      p.native(r, compiled ? n.reenter_c : n.reenter_i, {gid, 0});
      p.cnst(ten, 10);
      p.bin(K::ADD, s, r, ten);
      p.ret(s);
      e.set(outer, p.done());
    }
    {
      P p("top", {GRJIT_TYPE_I64});
      int r = p.local(), h = p.local(), s = p.local();
      p.call(r, outer, {0});
      p.cnst(h, 100);
      p.bin(K::ADD, s, r, h);
      p.ret(s);
      Func f = p.done();
      f.catches = true; // the scope the limit unwind ends at
      e.set(top, f);
    }
    // A pause is asked for, and the poll in the nested run answers it. (The request word is set so
    // that a compiled poll takes its slow path; the fixture's helper then returns a verdict.)
    e.pause_pending = true;
    *reinterpret_cast<uint64_t *>(reinterpret_cast<unsigned char *>(e.ctx) + grcore_jit_layout()->request_word_offset) = 1;
    Outcome ref = e.run_interpreted(top, {0});
    EXPECT_FALSE(ref.paused) << "inside a nested run a pause becomes a limit unwind, in the interpreter";
    EXPECT_FALSE(e.pause_pending);
    EXPECT_EQ(ref.frames_left, 0u);
    e.pause_pending = true;
    Outcome o = e.run_compiled(top, {0});
    *reinterpret_cast<uint64_t *>(reinterpret_cast<unsigned char *>(e.ctx) + grcore_jit_layout()->request_word_offset) = 0;
    EXPECT_FALSE(o.paused) << "no pause reaches the host from inside";
    EXPECT_TRUE(o.finished) << "top catches the unwind";
    EXPECT_TRUE(ref.finished);
    EXPECT_EQ(o.value, ref.value);
    EXPECT_EQ(o.value, kUnwound + 100u) << "top resumed after outer's call, with the scope's value";
    EXPECT_EQ(o.frames_left, 0u);
    EXPECT_EQ(e.st.pushes, e.st.pops);
    EXPECT_EQ(grcore_activation_count(e.stack), 0u);
  }
}

TEST(Natives, NestedGuestCodeThatClearsTheEntrySlotOfTheOuterFunctionLeavesItsCodeAliveUntilTheLastJitRecordLeaves) {
  Engine e;
  Nat n = register_natives(e);
  int g = e.reserve(), outer = e.reserve(), top = e.reserve();
  std::vector<long> released_during;
  std::vector<size_t> retired_during;
  e.on_collect = [&](Engine & en) {
    released_during.push_back(*en.released);
    retired_during.push_back(grcore_code_retired_count(en.ctx));
  };
  {
    // G clears `outer`'s slot (retiring the code a frame of the outer run waits in, under this very
    // native), collects, and returns.
    P p("G", {GRJIT_TYPE_I64});
    int r = p.local(), one = p.local();
    p.clearslot(outer);
    p.collect();
    p.cnst(one, 1);
    p.bin(K::ADD, r, 0, one);
    p.ret(r);
    e.set(g, p.done());
  }
  {
    P p("outer", {GRJIT_TYPE_I64});
    int obj = p.local(GRJIT_TYPE_REF), gid = p.local(), r = p.local(), v = p.local(), s = p.local();
    p.nw(obj, 30);
    p.cnst(gid, g);
    p.native(r, n.reenter_c, {gid, 0});
    p.get(v, obj);
    p.bin(K::ADD, s, r, v);
    p.ret(s);
    e.set(outer, p.done());
  }
  {
    P p("top", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, outer, {0});
    p.ret(r);
    e.set(top, p.done());
  }
  Outcome o = e.run_compiled(top, {4});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 5u + 30u) << "outer returned into its retired code, correctly";
  ASSERT_FALSE(released_during.empty());
  for (long r : released_during) {
    EXPECT_EQ(r, 0) << "nothing is released while a JIT record is open and a frame waits in the code";
  }
  ASSERT_EQ(retired_during.size(), released_during.size());
  for (size_t r : retired_during) {
    EXPECT_EQ(r, 2u) << "the slot's reference and the registry's, both retired and held while the frame waits";
  }
  EXPECT_EQ(grcore_code_retired_count(e.ctx), 0u) << "and gone once the record left";
  EXPECT_EQ(e.st.deopts, 0);
  EXPECT_EQ(*e.released, 1) << "the retired code is released once the last JIT record has left";
  EXPECT_EQ(e.heap.poisoned_reads, 0);
}

/* ---- The native stack ------------------------------------------------------------------------ */

TEST(Natives, ABudgetOneByteEitherSideOfTheCallsNeedMakesTheCallOrAnExitBeforeItAndNeverAFault) {
  for (size_t nargs : {size_t{0}, size_t{4}, size_t{5}, size_t{9}, size_t{16}}) {
    for (uint32_t declared : {uint32_t{4000}, uint32_t{9000}}) {
      SCOPED_TRACE(nargs);
      SCOPED_TRACE(declared);
      std::vector<GRJIT_Type> types(nargs, GRJIT_TYPE_I64);
      auto build = [&](Engine & e, int * top) {
        int id = e.add_native(reinterpret_cast<const void *>(grjit_test_align_stub), types, GRJIT_NATIVE_NO_RESULT, 0,
            declared, "stub");
        int mid = e.reserve();
        {
          P p("mid", {GRJIT_TYPE_I64});
          std::vector<int> args(nargs, 0);
          p.native(-1, id, args);
          int one = p.local(), r = p.local();
          p.cnst(one, 1);
          p.bin(K::ADD, r, 0, one);
          p.ret(r);
          e.set(mid, p.done());
        }
        P p("top", {GRJIT_TYPE_I64});
        int r = p.local();
        p.call(r, mid, {0});
        p.ret(r);
        *top = e.add(p.done());
      };
      // Where the native's entry is, measured with a budget wide enough for anything, and the limit word
      // that run set: the stack pointer the run began at is the limit plus the budget.
      constexpr uint64_t kWide = 1u << 20;
      uintptr_t entry = 0, sp_run = 0;
      {
        Engine e(GRCORE_UNLIMITED, kWide);
        int top;
        build(e, &top);
        grjit_test_entry_count = 0;
        Outcome o = e.run_compiled(top, {1});
        ASSERT_TRUE(o.finished);
        ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
        ASSERT_EQ(grjit_test_entry_count, 1u);
        entry = grjit_test_entry_rsp[0];
        sp_run = e.last_native_limit + kWide;
      }
      // The lowest address the native may use is its entry stack pointer, past the return address, less
      // what it declared; the call is made iff that is not below the limit, sp_run - budget.
      const uintptr_t lowest = entry + 8 - declared;
      const uint64_t exact = sp_run - lowest;
      ASSERT_GT(exact, uint64_t{100}) << "the run's own frames fit in less";
      for (int64_t delta : {-1, 0, 1}) {
        const uint64_t budget = exact + static_cast<uint64_t>(delta);
        Engine e(GRCORE_UNLIMITED, budget);
        int top;
        build(e, &top);
        grjit_test_entry_count = 0;
        Outcome ref = e.run_interpreted(top, {1});
        ASSERT_TRUE(ref.finished);
        Outcome o = e.run_compiled(top, {1});
        ASSERT_TRUE(o.finished) << "never a fault, and the interpreter finishes after an exit";
        EXPECT_EQ(o.value, ref.value);
        EXPECT_EQ(o.value, 2u);
        // The native is called once by the interpreter and once more in the compiled run, by compiled code
        // or, after the exit, by the interpreter.
        EXPECT_EQ(grjit_test_entry_count, 2u);
        if (delta < 0) {
          EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_DEOPT}) << "one byte short: an exit before the call";
          EXPECT_EQ(e.st.native_exits, 1);
          EXPECT_EQ(e.st.last_cause, 0u);
        } else {
          EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED}) << "at the need, and above it: the call is made";
          EXPECT_EQ(e.st.native_exits, 0);
          EXPECT_EQ(e.st.deopts, 0);
        }
        EXPECT_EQ(o.frames_left, 0u);
      }
    }
  }
}

/* ---- The native-depth budget ------------------------------------------------------------------ */

TEST(Natives, ARefusedRecordMakesTheNativeReturnUnwindAndTheVerdictIsTheInterpretedRunsAtTheSameNesting) {
  // R(x) logs x and, below zero, re-enters R(x - 1) through a native that opens a REENTRY record, which
  // enters the native-depth budget; T catches the unwind a refusal ends in. A compiled outer run has one
  // more record open (its own JIT record) than an interpreted one, so the same nesting is reached with
  // a budget one larger, and with the same budget one level sooner.
  auto levels_entered = [](bool compiled_outer, uint64_t depth_budget, u64 * value, bool * finished) {
    Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, false, nullptr, GRCORE_UNLIMITED, depth_budget);
    Nat n = register_natives(e);
    int r = e.reserve(), t = e.reserve();
    {
      P p("R", {GRJIT_TYPE_I64});
      int l = p.local(), z = p.local(), c = p.local(), rid = p.local(), x1 = p.local(), one = p.local(), res = p.local();
      p.native(l, n.log, {0});
      p.cnst(z, 0);
      p.bin(K::EQ, c, 0, z);
      int more = p.brz(c);
      p.ret(z);
      p.patch(more, p.here());
      p.cnst(one, 1);
      p.bin(K::SUB, x1, 0, one);
      p.cnst(rid, r);
      p.native(res, n.reenter_i, {rid, x1});
      p.ret(res);
      e.set(r, p.done());
    }
    {
      P p("T", {GRJIT_TYPE_I64});
      int v = p.local(), h = p.local(), s = p.local();
      p.call(v, r, {0});
      p.cnst(h, 7);
      p.bin(K::ADD, s, v, h);
      p.ret(s);
      Func f = p.done();
      f.catches = true;
      e.set(t, f);
    }
    e.trace.clear();
    Outcome o = compiled_outer ? e.run_compiled(t, {20}) : e.run_interpreted(t, {20});
    *value = o.value;
    *finished = o.finished;
    EXPECT_EQ(grcore_activation_count(e.stack), 0u);
    EXPECT_EQ(e.st.pushes, e.st.pops);
    return e.trace.size();
  };
  for (uint64_t budget : {uint64_t{2}, uint64_t{5}, uint64_t{9}}) {
    SCOPED_TRACE(budget);
    u64 vi, vc, vc_same;
    bool fi, fc, fc_same;
    const size_t interpreted = levels_entered(false, budget, &vi, &fi);
    const size_t compiled_plus_one = levels_entered(true, budget + 1, &vc, &fc);
    const size_t compiled_same = levels_entered(true, budget, &vc_same, &fc_same);
    EXPECT_EQ(interpreted, budget + 1) << "the budget is the number of records the nesting may open, and R(20)'s first call is level zero";
    EXPECT_EQ(compiled_plus_one, interpreted) << "the same nesting, which is the interpreted run's verdict";
    EXPECT_EQ(vc, vi);
    EXPECT_EQ(fc, fi);
    EXPECT_EQ(vi, kUnwound + 7u) << "the refusal ended in T's scope";
    EXPECT_EQ(compiled_same, interpreted - 1) << "a compiled outer run's JIT record is one the interpreted run does not have";
    EXPECT_EQ(vc_same, vi);
  }
}

/* ---- Generated programs: guest calls, tail calls and natives, under torture ------------------- */

namespace {

struct GSig {
  std::vector<GRJIT_Type> types;
};

/* A random program of `n` functions, f_i calling, tail-calling and re-entering only f_j with j < i, so
 * that it terminates. Each takes a random number of parameters of random types (the last, the entry,
 * two integers), reads every one, allocates an object and keeps a pointer derived from it, may collect,
 * may call natives of every kind (logging, adding, allocating, collecting, a status that leaves, an
 * unwind), may re-enter a lower function through a native (interpreted or compiled), may call or tail
 * call a lower function, may hit a guard that fails for some inputs, and polls. Some functions are
 * scopes (they catch an unwind). */
void generate_natives(Engine & e, const Nat & nat, std::mt19937 & rng, int n, std::vector<int> * fns,
    std::vector<GSig> * sigs) {
  auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  for (int i = 0; i < n; i++) {
    GSig sig;
    if (i == n - 1) {
      sig.types = {GRJIT_TYPE_I64, GRJIT_TYPE_I64};
    } else {
      int np = pick(0, 8);
      for (int k = 0; k < np; k++) {
        int r = pick(0, 19);
        sig.types.push_back(r < 12 ? GRJIT_TYPE_I64 : r < 17 ? GRJIT_TYPE_REF : GRJIT_TYPE_PTR);
      }
    }
    sigs->push_back(sig);
    fns->push_back(e.reserve());
  }
  for (int i = 0; i < n; i++) {
    const GSig & sg = (*sigs)[static_cast<size_t>(i)];
    P p("gn", sg.types);
    int obj = p.local(GRJIT_TYPE_REF), dp = p.local(GRJIT_TYPE_PTR), s = p.local(), c = p.local(), t = p.local(),
        r = p.local(), v = p.local(), k = p.local(), m = p.local(), ptr = p.local(GRJIT_TYPE_PTR),
        o2 = p.local(GRJIT_TYPE_REF), fid = p.local();
    p.cnst(s, 100 + i);
    for (size_t q = 0; q < sg.types.size(); q++) {
      int param = static_cast<int>(q);
      if (sg.types[q] == GRJIT_TYPE_I64) {
        p.mov(t, param);
      } else if (sg.types[q] == GRJIT_TYPE_REF) {
        p.get(t, param);
      } else {
        p.load(t, param);
      }
      p.cnst(c, static_cast<int64_t>(q) + 1);
      p.bin(K::MUL, t, t, c);
      p.bin(K::ADD, s, s, t);
    }
    p.nw(obj, pick(1, 50));
    p.derive(dp, obj, 8);
    auto make_args = [&](int j) {
      std::vector<int> args;
      for (GRJIT_Type ty : (*sigs)[static_cast<size_t>(j)].types) {
        if (ty == GRJIT_TYPE_I64) {
          int w = pick(0, 4);
          if (w == 0) {
            args.push_back(p.imm(pick(-20, 20)));
          } else if (w == 1) {
            args.push_back(s);
          } else {
            int ip = -1;
            for (size_t q = 0; q < sg.types.size(); q++) {
              if (sg.types[q] == GRJIT_TYPE_I64 && pick(0, 1) == 0) {
                ip = static_cast<int>(q);
              }
            }
            args.push_back(ip >= 0 ? ip : s);
          }
        } else if (ty == GRJIT_TYPE_REF) {
          int rp = -1;
          for (size_t q = 0; q < sg.types.size(); q++) {
            if (sg.types[q] == GRJIT_TYPE_REF && pick(0, 1) == 0) {
              rp = static_cast<int>(q);
            }
          }
          args.push_back(rp >= 0 ? rp : obj);
        } else {
          args.push_back(dp);
        }
      }
      return args;
    };
    int steps = pick(1, 5);
    for (int stp = 0; stp < steps; stp++) {
      switch (pick(0, 12)) {
        case 0: p.native(r, nat.log, {s}); p.bin(K::ADD, s, s, r); break;
        case 1: p.native(r, nat.add, {s, s}); p.bin(K::ADD, s, s, r); break;
        case 2:
          p.native(o2, nat.newobj, {p.imm(pick(1, 9))});
          p.get(v, o2);
          p.bin(K::ADD, s, s, v);
          break;
        case 3: p.native(r, nat.collect, {}); p.bin(K::ADD, s, s, r); break;
        case 4:
          // A status that leaves compiled code (or not, for zero): the guest picks it.
          p.cnst(m, pick(0, 3) == 0 ? 1 : 0);
          p.native(r, nat.status, {s, m});
          p.bin(K::ADD, s, s, r);
          break;
        case 5:
          if (pick(0, 4) == 0) {
            p.native(r, nat.unwind, {s});
            p.bin(K::ADD, s, s, r);
          }
          break;
        case 6: {
          // A re-entry passes one integer: the target's parameters are all integers (a reference or a
          // pointer parameter would be handed zero, which a function reads).
          std::vector<int> targets;
          for (int j = 0; j < i; j++) {
            bool ints = true;
            for (GRJIT_Type ty : (*sigs)[static_cast<size_t>(j)].types) {
              ints = ints && ty == GRJIT_TYPE_I64;
            }
            if (ints) {
              targets.push_back(j);
            }
          }
          if (!targets.empty()) {
            int j = targets[static_cast<size_t>(pick(0, static_cast<int>(targets.size()) - 1))];
            p.cnst(fid, (*fns)[static_cast<size_t>(j)]);
            p.native(r, pick(0, 1) == 0 ? nat.reenter_i : nat.reenter_c, {fid, s});
            p.bin(K::ADD, s, s, r);
          }
          break;
        }
        case 7:
          if (i > 0) {
            int j = pick(0, i - 1);
            std::vector<int> args = make_args(j);
            if (pick(0, 1) == 0) {
              p.entryof(ptr, (*fns)[static_cast<size_t>(j)]);
              p.callp(r, ptr, (*fns)[static_cast<size_t>(j)], args);
            } else {
              p.call(r, (*fns)[static_cast<size_t>(j)], args);
            }
            p.bin(K::ADD, s, s, r);
          }
          break;
        case 8:
          p.cnst(m, 63);
          p.bin(K::AND, t, s, m);
          p.cnst(k, pick(0, 70));
          p.bin(K::LT, t, t, k);
          p.guard(t);
          break;
        case 9: p.poll(); break;
        case 10: p.collect(); break;
        case 11: p.native(r, nat.keep, {obj}); p.bin(K::ADD, s, s, r); break;
        default: break;
      }
    }
    if (i > 0 && pick(0, 3) == 0) {
      int j = pick(0, i - 1);
      std::vector<int> args = make_args(j);
      if (pick(0, 1) == 0) {
        p.entryof(ptr, (*fns)[static_cast<size_t>(j)]);
        p.tailcallp(ptr, (*fns)[static_cast<size_t>(j)], args);
      } else {
        p.tailcall((*fns)[static_cast<size_t>(j)], args);
      }
    } else {
      p.get(v, obj);
      p.bin(K::ADD, s, s, v);
      p.load(v, dp);
      p.bin(K::ADD, s, s, v);
      p.ret(s);
    }
    Func f = p.done();
    f.catches = pick(0, 4) == 0;
    e.set((*fns)[static_cast<size_t>(i)], f);
  }
}

} // namespace

TEST(Natives, GeneratedProgramsOfGuestCallsTailCallsNativesAndReentryAgreeWithTheInterpreterFrameForFrameAtEveryPoll) {
  long finished = 0, unwound = 0, deopted = 0, direct = 0, status_exits = 0, polls_compared = 0, unwinds = 0;
  for (unsigned seed = 0; seed < 150; seed++) {
    SCOPED_TRACE(seed);
    if (std::getenv("FX_TRACE_SEEDS") != nullptr) {
      fprintf(stderr, "seed %u\n", seed);
    }
    std::mt19937 rng(seed * 7919u + 13u);
    Engine e;
    e.torture = seed % 2 == 0;
    e.interpreter_never_moves = true; // the interpreter has no derived pointers: nothing moves while it runs
    e.record_polls = true;
    Nat nat = register_natives(e);
    std::vector<int> fns;
    std::vector<GSig> sigs;
    int n = 3 + static_cast<int>(seed % 6);
    generate_natives(e, nat, rng, n, &fns, &sigs);
    for (int fn : fns) {
      ASSERT_TRUE(e.compile_fn(fn)); // so every ENTRYOF has an entry to take
    }
    // Every compiled poll takes its slow path (a snapshot) and the interpreter's snapshots are the same.
    *reinterpret_cast<uint64_t *>(reinterpret_cast<unsigned char *>(e.ctx) + grcore_jit_layout()->request_word_offset) = 1;
    for (u64 x : {u64{0}, u64{3}, u64{15}, u64{40}}) {
      e.trace.clear();
      e.poll_snaps.clear();
      bool torture = e.torture;
      e.torture = false;
      Outcome i = e.run_interpreted(fns[static_cast<size_t>(n) - 1], {x, x + 1});
      e.torture = torture;
      const auto trace_i = e.trace;
      const auto snaps_i = e.poll_snaps;
      e.trace.clear();
      e.poll_snaps.clear();
      const long status_before = e.st.status_exits;
      Outcome c = e.run_compiled(fns[static_cast<size_t>(n) - 1], {x, x + 1});
      ASSERT_EQ(c.finished, i.finished) << "failed=" << c.failed;
      EXPECT_EQ(c.unwound, i.unwound);
      if (i.finished) {
        EXPECT_EQ(c.value, i.value);
        finished++;
      } else {
        ASSERT_TRUE(i.unwound) << "a run ends in a value or in an unwind";
        unwound++;
      }
      EXPECT_EQ(e.trace, trace_i) << "the natives ran in the same order, once each, in both tiers";
      ASSERT_EQ(e.poll_snaps.size(), snaps_i.size()) << "the same polls were reached";
      for (size_t q = 0; q < snaps_i.size(); q++) {
        ASSERT_EQ(e.poll_snaps[q].size(), snaps_i[q].size()) << "poll " << q << ": the same frames";
        for (size_t fr = 0; fr < snaps_i[q].size(); fr++) {
          EXPECT_TRUE(e.poll_snaps[q][fr] == snaps_i[q][fr])
              << "poll " << q << " frame " << fr << " (" << e.funcs[static_cast<size_t>(snaps_i[q][fr].fn)].name << ")";
        }
      }
      polls_compared += static_cast<long>(snaps_i.size());
      EXPECT_EQ(c.frames_left, 0u);
      EXPECT_EQ(e.st.rebuild, GRCORE_OK);
      EXPECT_EQ(grcore_activation_count(e.stack), 0u);
      (c.exit == GRJIT_EXIT_DEOPT ? deopted : direct)++;
      status_exits += e.st.status_exits - status_before;
      e.st.rebuild = GRCORE_OK;
    }
    unwinds += e.st.unwinds;
    EXPECT_EQ(e.heap.poisoned_reads, 0);
    EXPECT_EQ(e.st.pushes, e.st.pops);
    EXPECT_FALSE(e.bad_native_ctx);
  }
  std::printf("natives: 150 generated programs, %ld runs finished and %ld unwound, %ld left compiled code and %ld did "
              "not, %ld exits through a native's status, %ld unwinds, %ld polls compared frame for frame\n",
      finished, unwound, deopted, direct, status_exits, unwinds, polls_compared);
  // Every kind of run happened, in numbers: the generator reaches the paths it exists to reach.
  EXPECT_GT(finished, 300);
  EXPECT_GT(unwound, 5);
  EXPECT_GT(deopted, 100);
  EXPECT_GT(direct, 100);
  EXPECT_GT(status_exits, 50);
  EXPECT_GT(polls_compared, 200);
  EXPECT_GT(unwinds, 20);
}


TEST(Natives, ANativeOfEveryArityCalledFromTheBottomOfACompiledChainGivesTheCReferenceAndTheInterpretersResult) {
  for (size_t n = 0; n <= 16; n++) {
    SCOPED_TRACE(n);
    Engine e;
    // Parameters of every type (a reference or a pointer that is never dereferenced, and never
    // collected over), the native's own descriptor types the same.
    const GRJIT_Type cycle[3] = {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_PTR};
    std::vector<GRJIT_Type> types;
    for (size_t i = 0; i < n; i++) {
      types.push_back(cycle[(i + n) % 3]);
    }
    int id = e.add_native(sums()[n], types, GRJIT_TYPE_I64, 0, 256, "sum");
    int leaf = e.reserve(), mid = e.reserve(), top = e.reserve();
    {
      P p("leaf", types);
      std::vector<int> args;
      for (size_t i = 0; i < n; i++) {
        args.push_back(static_cast<int>(i));
      }
      int r = p.local();
      p.native(r, id, args);
      p.ret(r);
      e.set(leaf, p.done());
    }
    auto relay = [&](const char * name, int self, int callee) {
      P p(name, types);
      std::vector<int> args;
      for (size_t i = 0; i < n; i++) {
        args.push_back(static_cast<int>(i));
      }
      int r = p.local();
      p.call(r, callee, args);
      p.ret(r);
      e.set(self, p.done());
    };
    relay("mid", mid, leaf);
    relay("top", top, mid);
    std::vector<u64> a(16);
    for (size_t i = 0; i < 16; i++) {
      a[i] = 0x0123456789abcdefull * (i + 1) + 5 * i;
    }
    const long calls_before = g_calls;
    Pair p = run_both(e, top, std::vector<u64>(a.begin(), a.begin() + static_cast<long>(n)));
    ASSERT_TRUE(p.i.finished);
    ASSERT_TRUE(p.c.finished);
    EXPECT_EQ(p.c.value, p.i.value);
    EXPECT_EQ(p.c.value, call_c(n, sums()[n], e.ctx, a.data()));
    EXPECT_EQ(g_calls - calls_before, 3) << "the C reference, the interpreter's call and the compiled call";
    EXPECT_EQ(p.c.exit, uint32_t{GRJIT_EXIT_RETURNED});
    EXPECT_EQ(e.st.deopts, 0);
  }
}

/* ---- What a native call is not: no reservation, no depth, no guest frame ----------------------- */

TEST(Natives, ANativeCallPushesNoGuestFrameCountsNoDepthAndExtendsNoReservation) {
  // A converting engine, so that the reservation has a capacity to compare, and probes before and
  // after ten native calls: guest frames, guest depth and capacity are the same.
  Engine e(GRCORE_UNLIMITED, GRCORE_UNLIMITED, /*conv=*/true);
  Nat n = register_natives(e);
  int f = e.reserve(), g = e.reserve();
  {
    P p("g", {GRJIT_TYPE_I64});
    int r = p.local(), t = p.local();
    p.probe(0);
    p.mov(r, 0);
    for (int k = 0; k < 10; k++) {
      p.native(t, n.add, {r, 0});
      p.mov(r, t);
    }
    p.probe(0);
    p.ret(r);
    e.set(g, p.done());
  }
  {
    P p("f", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, g, {0});
    p.ret(r);
    e.set(f, p.done());
  }
  Outcome o = e.run_compiled(f, {3});
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.value, 3u + 10u * 3u);
  ASSERT_EQ(e.st.frames.size(), 2u);
  EXPECT_EQ(e.st.frames[0], e.st.frames[1]) << "no guest frame was pushed";
  EXPECT_EQ(e.st.depths[0], e.st.depths[1]) << "no guest depth was counted";
  EXPECT_GT(e.st.depths[0], 0u);
  EXPECT_EQ(e.st.caps[0], e.st.caps[1]) << "no reservation was extended";
  EXPECT_GT(e.st.caps[0], 0u) << "a converting engine: the capacity is not nothing, so equal is a measurement";
  EXPECT_EQ(e.st.extends, 1) << "the push of g (f's is the run's own), and nothing for the ten natives";
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- Every hook failure is observable ----------------------------------------------------------- */

TEST(Natives, ARebuildTheEngineRefusesAtANativesStatusExitIsTheFatalExitWithNothingWrittenAndTheNativeRanOnce) {
  Engine e;
  Nat n = register_natives(e);
  int g = e.reserve(), f = e.reserve();
  {
    P p("g", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int v = p.local(), w = p.local();
    p.native(v, n.status, {0, 1});
    p.native(w, n.log, {v});
    p.ret(w);
    e.set(g, p.done());
  }
  {
    P p("f", {GRJIT_TYPE_I64, GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, g, {0, 1});
    p.ret(r);
    e.set(f, p.done());
  }
  e.refuse_rebuild_with = 22; // the engine's own answer
  e.interpreter_cannot_recover = true;
  Outcome o = e.run_compiled(f, {5, 1});
  EXPECT_TRUE(o.rebuild_failed) << "a refused rebuild is the fatal exit, never a deoptimization";
  EXPECT_EQ(o.failed_with, 22u) << "the engine is told what it said";
  EXPECT_FALSE(o.finished);
  EXPECT_EQ(o.exit, uint32_t{GRJIT_EXIT_REBUILD_FAILED});
  ASSERT_EQ(o.pcs_after_failure.size(), 2u);
  for (u64 pc : o.pcs_after_failure) {
    EXPECT_EQ(pc, 0u) << "nothing was written into a guest frame";
  }
  EXPECT_EQ(e.st.deopts, 1);
  EXPECT_EQ(e.st.status_exits, 1);
  EXPECT_EQ(e.trace, (std::vector<int64_t>{5})) << "the native ran, once, and the interpreter never ran";
  EXPECT_EQ(e.st.pushes, 2);
  EXPECT_EQ(e.st.pops, 0) << "the engine unwound the two frames; nothing returned through them";
}

TEST(Natives, ARebuildTheEngineRefusesAtAnExitBeforeANativeCallIsTheFatalExitAndTheNativeIsNeverCalled) {
  constexpr uint64_t kWide = 1u << 20;
  auto build = [](Engine & e, int * top) {
    int stub = e.add_native(reinterpret_cast<const void *>(grjit_test_align_stub), {GRJIT_TYPE_I64},
        GRJIT_NATIVE_NO_RESULT, 0, 4000, "stub");
    int g = e.reserve();
    {
      P p("g", {GRJIT_TYPE_I64});
      p.native(-1, stub, {0});
      p.ret(0);
      e.set(g, p.done());
    }
    P p("f", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, g, {0});
    p.ret(r);
    *top = e.add(p.done());
  };
  uint64_t exact;
  {
    Engine e(GRCORE_UNLIMITED, kWide);
    int top;
    build(e, &top);
    grjit_test_entry_count = 0;
    Outcome o = e.run_compiled(top, {1});
    ASSERT_TRUE(o.finished);
    ASSERT_EQ(grjit_test_entry_count, 1u);
    exact = (e.last_native_limit + kWide) - (grjit_test_entry_rsp[0] + 8 - 4000);
  }
  Engine e(GRCORE_UNLIMITED, exact - 1);
  int top;
  build(e, &top);
  e.refuse_rebuild_with = 22;
  e.interpreter_cannot_recover = true;
  grjit_test_entry_count = 0;
  Outcome o = e.run_compiled(top, {1});
  EXPECT_TRUE(o.rebuild_failed);
  EXPECT_EQ(o.failed_with, 22u);
  EXPECT_FALSE(o.finished);
  EXPECT_EQ(e.st.native_exits, 1);
  EXPECT_EQ(grjit_test_entry_count, 0u) << "the exit was before the call: the native was never called, by anyone";
  for (u64 pc : o.pcs_after_failure) {
    EXPECT_EQ(pc, 0u);
  }
}

/* ---- A padded frame ---------------------------------------------------------------------------------- */

TEST(Natives, AReferenceAndADerivedPointerLiveAcrossANativeInAFramePaddedForAWiderTailCalleeAreUpdated) {
  Engine e;
  e.torture = true;
  e.interpreter_never_moves = true;
  Nat n = register_natives(e);
  // wide(a0 .. a15) = sum of (i + 1) * a_i: a tail callee with ten stack arguments.
  int wide = e.reserve(), narrow = e.reserve();
  {
    std::vector<GRJIT_Type> types(16, GRJIT_TYPE_I64);
    P p("wide", types);
    int acc = p.local(), c = p.local(), t = p.local();
    p.cnst(acc, 0);
    for (int i = 0; i < 16; i++) {
      p.cnst(c, i + 1);
      p.bin(K::MUL, t, i, c);
      p.bin(K::ADD, acc, acc, t);
    }
    p.ret(acc);
    e.set(wide, p.done());
  }
  {
    // narrow has no parameters and few registers: the frame is padded so that the staging area clears
    // the place the callee's stack arguments go. Its reference and its derived pointer are live across
    // collecting natives, and read afterwards to make the callee's arguments.
    P p("narrow", {});
    int obj = p.local(GRJIT_TYPE_REF), dp = p.local(GRJIT_TYPE_PTR), a = p.local(), b = p.local(), z = p.local();
    p.nw(obj, 6);
    p.derive(dp, obj, 8);
    p.native(z, n.collect, {});
    p.native(z, n.collect, {});
    p.get(a, obj);
    p.load(b, dp);
    std::vector<int> args;
    for (int i = 0; i < 16; i++) {
      args.push_back(i % 2 == 0 ? a : b);
    }
    p.tailcall(wide, args);
    e.set(narrow, p.done());
  }
  int top = e.reserve();
  {
    P p("top", {GRJIT_TYPE_I64});
    int r = p.local();
    p.call(r, narrow, {});
    p.ret(r);
    e.set(top, p.done());
  }
  // The frame really is padded.
  {
    GRJIT_Function * f = e.build_ir(narrow);
    GRJIT_CallableShape shape;
    grjit_callable_shape(f, &shape);
    EXPECT_GT(shape.pad, 0u) << "a no-parameter caller of a sixteen-argument callee needs padding";
    grjit_function_destroy(f);
  }
  Pair p = run_both(e, top, {0});
  ASSERT_TRUE(p.i.finished);
  ASSERT_TRUE(p.c.finished);
  EXPECT_EQ(p.c.value, p.i.value);
  EXPECT_EQ(p.c.value, 6u * (136u)) << "6 * (1 + 2 + ... + 16)";
  EXPECT_EQ(e.heap.poisoned_reads, 0);
  EXPECT_GT(e.heap.moved, 0);
  EXPECT_EQ(e.st.deopts, 0);
}

/* ---- The other backends ---------------------------------------------------------------- */

TEST(Natives, OnlyTheX86_64SysVBackendEmitsANativeCallAndTheOthersRefuseBeforeAByte) {
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(sums()[1]), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
  B b("native", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.callable(hooks());
  b.natives(t);
  b.at(b.block());
  b.call_native(x, id, {V(x)}, GRCORE_PollIdentity{0, 0}, {grjit_frame_slot_vreg(x)});
  b.ret(V(x));
  Fn f(b.finish());
  GRJIT_Emitted e;
  EXPECT_EQ(grjit_emit_for(GRJIT_ARCH_X86_64, f, grjit_allocator_default(), nullptr, nullptr, 0x40, &e), GRJIT_OK);
  EXPECT_GT(e.size, 0u);
  grjit_emitted_free(&e);
  for (GRJIT_Arch arch : {GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    EXPECT_EQ(grjit_emit_for(arch, f, grjit_allocator_default(), nullptr, nullptr, 0x40, &e), GRJIT_ERR_UNSUPPORTED);
    EXPECT_EQ(e.size, 0u);
    EXPECT_EQ(e.bytes, nullptr);
  }
  EXPECT_TRUE(grjit_backend_calls_available());
}

#else // not x86-64 Linux

namespace {
uint64_t refused_native(void *, uint64_t) { return 0; }
} // namespace

TEST(Natives, ANativeCallIsRefusedWithAClearErrorWhereItIsNotEmitted) {
  NativeTab t;
  uint32_t id = t.add(reinterpret_cast<const void *>(refused_native), {GRJIT_TYPE_I64}, GRJIT_TYPE_I64);
  B b("native", 1);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  GRJIT_CallHooks h{};
  h.deopt = [](void *, uint64_t) -> uint32_t { return 0; };
  b.callable(h);
  b.natives(t);
  b.at(b.block());
  b.call_native(x, id, {V(x)}, GRCORE_PollIdentity{0, 0}, {grjit_frame_slot_vreg(x)});
  b.ret(V(x));
  Fn f(b.finish());
  JitWorld w;
  Compiled c(f, w.pages());
  EXPECT_EQ(c.result, GRJIT_ERR_UNSUPPORTED);
  EXPECT_FALSE(c);
  EXPECT_FALSE(grjit_backend_calls_available());
}

#endif

GRJIT_TEST_MAIN()
