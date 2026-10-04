/**
 * @file
 *
 * Shared helpers for the Ghoti.io Runtime-jit unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TEST_HELPERS_H
#define GHOTI_IO_GRJIT_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* A base allocator that counts, tracks live blocks, and can fail the Nth
 * allocation call (malloc, calloc or realloc, counted from 1), or every call
 * from the Nth on (`sticky`). The allocation-failure sweeps use it to reach
 * every arm. */
struct TrackingAllocator {
  GRJIT_Allocator vtable;
  long calls = 0;
  long fail_at = 0;    // 0: never fail
  bool sticky = false; // with fail_at: fail every call from the Nth on
  long live = 0;       // successful allocations not yet freed

  TrackingAllocator(const TrackingAllocator &) = delete;
  TrackingAllocator & operator=(const TrackingAllocator &) = delete;
  TrackingAllocator() {
    vtable.ctx = this;
    vtable.malloc_fn = [](void * c, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::malloc(n ? n : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1, m ? m : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * q = std::realloc(p, n ? n : 1);
      if (p == nullptr && q != nullptr) {
        t->live++;
      }
      return q;
    };
    vtable.free_fn = [](void * c, void * p) {
      if (p != nullptr) {
        static_cast<TrackingAllocator *>(c)->live--;
      }
      std::free(p);
    };
  }
  bool fails() {
    calls++;
    return fail_at != 0 && (sticky ? calls >= fail_at : calls == fail_at);
  }
  const GRJIT_Allocator * get() const { return &vtable; }
};

/* A page provider over a context's counting one that can fail the Nth map or
 * every protect, or have no protect at all. `live` counts mappings not yet
 * unmapped, which is the leak check for code memory. */
struct FakePages {
  GRCORE_PageProvider vtable{};
  const GRCORE_PageProvider * base;
  long maps = 0;
  long fail_map_at = 0;
  bool fail_protect = false;
  long live = 0;
  long protects = 0;

  FakePages(const FakePages &) = delete;
  FakePages & operator=(const FakePages &) = delete;
  explicit FakePages(const GRCORE_PageProvider * under) : base(under) {
    vtable.ctx = this;
    vtable.page_size = under->page_size;
    vtable.map = [](void * c, size_t n) -> void * {
      auto * f = static_cast<FakePages *>(c);
      f->maps++;
      if (f->fail_map_at != 0 && f->maps == f->fail_map_at) {
        return nullptr;
      }
      void * p = f->base->map(f->base->ctx, n);
      if (p != nullptr) {
        f->live++;
      }
      return p;
    };
    vtable.unmap = [](void * c, void * p, size_t n) {
      auto * f = static_cast<FakePages *>(c);
      f->base->unmap(f->base->ctx, p, n);
      f->live--;
    };
    vtable.protect = [](void * c, void * p, size_t n,
                         GRCORE_PageAccess a) -> bool {
      auto * f = static_cast<FakePages *>(c);
      f->protects++;
      if (f->fail_protect) {
        return false;
      }
      return f->base->protect(f->base->ctx, p, n, a);
    };
  }
};

/* A group and a context: the context supplies the counting page provider the
 * code is mapped from, and the request word that compiled polls load. */
struct JitWorld {
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;

  JitWorld(const JitWorld &) = delete;
  JitWorld & operator=(const JitWorld &) = delete;
  JitWorld() {
    EXPECT_EQ(grcore_group_create(nullptr, nullptr, &group), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, nullptr, &ctx), GRCORE_OK);
  }
  ~JitWorld() {
    EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  }
  const GRCORE_PageProvider * pages() const {
    return grcore_context_page_provider(ctx);
  }
  uint64_t bytes_in_use() const { return grcore_context_memory_in_use(ctx); }
  uint64_t blocks_in_use() const { return grcore_context_memory_blocks(ctx); }
};

/* ---- A thin recording wrapper over the builder ---------------------------- */

inline GRJIT_Operand V(GRJIT_VReg v) { return grjit_operand_vreg(v); }
inline GRJIT_Operand I(int64_t imm) { return grjit_operand_imm(imm); }
inline GRJIT_Operand NONE() { return grjit_operand_none(); }

/* Every builder call must succeed unless the test says otherwise. */
struct B {
  GRJIT_Builder * b = nullptr;

  B(const B &) = delete;
  B & operator=(const B &) = delete;
  explicit B(const char * name = "f", size_t slots = 0,
      const GRJIT_Limits * limits = nullptr,
      const GRJIT_Allocator * allocator = nullptr) {
    EXPECT_EQ(grjit_builder_create(name, slots, limits, allocator, &b),
        GRJIT_OK);
  }
  ~B() { grjit_builder_destroy(b); }

  GRJIT_VReg param(GRJIT_Type t) {
    GRJIT_VReg v = GRJIT_NO_VREG;
    EXPECT_EQ(grjit_builder_param(b, t, &v), GRJIT_OK);
    return v;
  }
  GRJIT_VReg reg(GRJIT_Type t = GRJIT_TYPE_I64) {
    GRJIT_VReg v = GRJIT_NO_VREG;
    EXPECT_EQ(grjit_builder_vreg(b, t, &v), GRJIT_OK);
    return v;
  }
  GRJIT_BlockId block() {
    GRJIT_BlockId id = 0;
    EXPECT_EQ(grjit_builder_block(b, &id), GRJIT_OK);
    return id;
  }
  void at(GRJIT_BlockId id) {
    EXPECT_EQ(grjit_builder_set_block(b, id), GRJIT_OK);
  }
  void poll_helper(GRJIT_PollHelper h) {
    EXPECT_EQ(grjit_builder_set_poll_helper(b, h), GRJIT_OK);
  }
  void derived(GRJIT_VReg v, GRJIT_VReg base, int64_t delta) {
    EXPECT_EQ(grjit_builder_derived(b, v, base, delta), GRJIT_OK);
  }
  void cnst(GRJIT_VReg d, int64_t imm) {
    EXPECT_EQ(grjit_builder_const(b, d, imm), GRJIT_OK);
  }
  void mov(GRJIT_VReg d, GRJIT_Operand s) {
    EXPECT_EQ(grjit_builder_move(b, d, s), GRJIT_OK);
  }
  void bin(GRJIT_OpKind k, GRJIT_VReg d, GRJIT_Operand x, GRJIT_Operand y) {
    EXPECT_EQ(grjit_builder_binary(b, k, d, x, y), GRJIT_OK);
  }
  void un(GRJIT_OpKind k, GRJIT_VReg d, GRJIT_Operand x) {
    EXPECT_EQ(grjit_builder_unary(b, k, d, x), GRJIT_OK);
  }
  void cmp(GRJIT_Cmp c, GRJIT_VReg d, GRJIT_Operand x, GRJIT_Operand y) {
    EXPECT_EQ(grjit_builder_cmp(b, c, d, x, y), GRJIT_OK);
  }
  void load(GRJIT_VReg d, GRJIT_VReg base, int32_t disp, uint32_t bits,
      bool sign = false) {
    EXPECT_EQ(grjit_builder_load(b, d, base, disp, bits, sign), GRJIT_OK);
  }
  void store(GRJIT_VReg base, int32_t disp, uint32_t bits, GRJIT_Operand v) {
    EXPECT_EQ(grjit_builder_store(b, base, disp, bits, v), GRJIT_OK);
  }
  void call(GRJIT_VReg d, const void * fn, std::vector<GRJIT_Operand> args = {}) {
    EXPECT_EQ(grjit_builder_call(b, d, reinterpret_cast<uintptr_t>(fn),
                  GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args.data(),
                  args.size(), GRCORE_PollIdentity{0, 0}, nullptr, 0),
        GRJIT_OK);
  }
  void call_gc(GRJIT_VReg d, const void * fn, std::vector<GRJIT_Operand> args,
      GRCORE_PollIdentity id, std::vector<GRJIT_FrameSlot> state,
      GRCORE_CodeSiteKind kind = GRCORE_SITE_GC_POINT_CALL) {
    EXPECT_EQ(grjit_builder_call(b, d, reinterpret_cast<uintptr_t>(fn),
                  GRJIT_CALL_GC_POINT, kind, args.data(), args.size(), id,
                  state.data(), state.size()),
        GRJIT_OK);
  }
  void poll(GRCORE_PollIdentity id, std::vector<GRJIT_FrameSlot> state = {}) {
    EXPECT_EQ(grjit_builder_poll(b, id, state.data(), state.size()), GRJIT_OK);
  }
  void guard(GRJIT_Operand cond, GRCORE_PollIdentity id,
      std::vector<GRJIT_FrameSlot> state = {}) {
    EXPECT_EQ(grjit_builder_guard(b, cond, id, state.data(), state.size()),
        GRJIT_OK);
  }
  void br(GRJIT_BlockId t) { EXPECT_EQ(grjit_builder_br(b, t), GRJIT_OK); }
  void br_if(GRJIT_Operand c, GRJIT_BlockId t, GRJIT_BlockId e) {
    EXPECT_EQ(grjit_builder_br_if(b, c, t, e), GRJIT_OK);
  }
  void ret(GRJIT_Operand v = grjit_operand_none()) {
    EXPECT_EQ(grjit_builder_ret(b, v), GRJIT_OK);
  }
  /* Ends construction; the caller owns the function. */
  GRJIT_Function * finish() {
    GRJIT_Function * f = nullptr;
    EXPECT_EQ(grjit_builder_finish(b, &f), GRJIT_OK);
    b = nullptr;
    return f;
  }
};

/* A function the test owns. */
struct Fn {
  GRJIT_Function * f;
  explicit Fn(GRJIT_Function * fn) : f(fn) {}
  Fn(const Fn &) = delete;
  Fn & operator=(const Fn &) = delete;
  ~Fn() { grjit_function_destroy(f); }
  operator const GRJIT_Function *() const { return f; }
};

inline std::string verify_reason(const GRJIT_Function * f, GRJIT_Result * result,
    const GRJIT_Limits * limits = nullptr) {
  char reason[512] = {0};
  *result = grjit_function_verify(f, limits, reason, sizeof reason);
  return reason;
}

inline std::string print(const GRJIT_Function * f) {
  size_t n = 0;
  EXPECT_EQ(grjit_function_print(f, nullptr, 0, &n), GRJIT_OK);
  std::string s(n + 1, '\0');
  size_t again = 0;
  EXPECT_EQ(grjit_function_print(f, s.data(), s.size(), &again), GRJIT_OK);
  EXPECT_EQ(n, again);
  s.resize(n);
  return s;
}

/* Compiled code the test owns, with a call wrapper. */
struct Compiled {
  GRJIT_Code * code = nullptr;
  GRJIT_Result result = GRJIT_OK;

  Compiled(const Compiled &) = delete;
  Compiled & operator=(const Compiled &) = delete;
  Compiled(const GRJIT_Function * f, const GRCORE_PageProvider * pages,
      GRJIT_EntryHook hook = nullptr, const GRJIT_Limits * limits = nullptr,
      const GRJIT_Allocator * allocator = nullptr) {
    GRJIT_CompileOptions o{};
    o.pages = pages;
    o.allocator = allocator;
    o.limits = limits;
    o.entry_hook = hook;
    result = grjit_compile(&o, f, &code);
  }
  ~Compiled() { grjit_code_destroy(code); }
  explicit operator bool() const { return code != nullptr; }

  struct Run {
    uint32_t exit = 99;
    std::vector<uint64_t> out;
  };
  Run run(void * ctx, std::vector<uint64_t> args = {}) const {
    Run r;
    r.out.assign(grjit_code_out_words(code), 0xDEADBEEFu);
    args.resize(std::max<size_t>(args.size(), grjit_code_param_count(code)));
    r.exit = grjit_code_call(code, ctx, args.data(), r.out.data());
    return r;
  }
};

/* On the gated target the backend must exist: a test that cannot run there
 * fails, it is never skipped. Elsewhere it must say it is absent. */
#if defined(__x86_64__) && defined(__linux__)
#define GRJIT_REQUIRE_BACKEND() \
  ASSERT_TRUE(grjit_backend_available()) \
      << "the backend is unavailable on the gated target"
#else
#define GRJIT_REQUIRE_BACKEND() \
  ASSERT_FALSE(grjit_backend_available())
#endif

/// Every test file ends with this: each is its own executable.
#define GRJIT_TEST_MAIN()                                                      \
  int main(int argc, char ** argv) {                                           \
    ::testing::InitGoogleTest(&argc, argv);                                    \
    return RUN_ALL_TESTS();                                                    \
  }

#endif
