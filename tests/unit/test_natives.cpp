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
  return reinterpret_cast<uint64_t (*)(void *, W<Is>...)>(fn)(ctx, a[Is]...);
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
