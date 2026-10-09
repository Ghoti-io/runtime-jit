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
  GRCORE_PageProvider vtable =
      GRCORE_PAGE_PROVIDER_INIT(nullptr, 0, nullptr, nullptr, nullptr);
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
  void bitcast(GRJIT_VReg d, GRJIT_VReg s) {
    EXPECT_EQ(grjit_builder_bitcast(b, d, s), GRJIT_OK);
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
  void callable(const GRJIT_CallHooks & hooks) {
    EXPECT_EQ(grjit_builder_set_callable(b, &hooks), GRJIT_OK);
  }
  /* A call through an entry slot; the result goes in `d` or nowhere. */
  void call_slot(GRJIT_VReg d, const void * slot_entry, uint64_t callee,
      std::vector<GRJIT_Operand> args, GRCORE_PollIdentity id,
      std::vector<GRJIT_FrameSlot> state, GRCORE_PollIdentity exit_id,
      std::vector<GRJIT_FrameSlot> exit_state) {
    EXPECT_EQ(grjit_builder_call_slot(b, d, reinterpret_cast<uintptr_t>(slot_entry),
                  callee, args.data(), args.size(), id, state.data(),
                  state.size(), exit_id, exit_state.data(), exit_state.size()),
        GRJIT_OK);
  }
  void call_ptr(GRJIT_VReg d, GRJIT_Operand target, uint64_t callee,
      std::vector<GRJIT_Operand> args, GRCORE_PollIdentity id,
      std::vector<GRJIT_FrameSlot> state, GRCORE_PollIdentity exit_id,
      std::vector<GRJIT_FrameSlot> exit_state) {
    EXPECT_EQ(grjit_builder_call_ptr(b, d, target, callee, args.data(),
                  args.size(), id, state.data(), state.size(), exit_id,
                  exit_state.data(), exit_state.size()),
        GRJIT_OK);
  }
  /* A tail call through an entry slot, or through a code pointer; each ends the
   * block. */
  void tail_call_slot(const void * slot_entry, uint64_t callee,
      std::vector<GRJIT_Operand> args, GRCORE_PollIdentity id,
      std::vector<GRJIT_FrameSlot> state) {
    EXPECT_EQ(grjit_builder_tail_call_slot(b, reinterpret_cast<uintptr_t>(slot_entry),
                  callee, args.data(), args.size(), id, state.data(), state.size()),
        GRJIT_OK);
  }
  void tail_call_ptr(GRJIT_Operand target, uint64_t callee, std::vector<GRJIT_Operand> args,
      GRCORE_PollIdentity id, std::vector<GRJIT_FrameSlot> state) {
    EXPECT_EQ(grjit_builder_tail_call_ptr(b, target, callee, args.data(), args.size(), id,
                  state.data(), state.size()),
        GRJIT_OK);
  }
  void natives(const GRJIT_NativeTable * table) {
    EXPECT_EQ(grjit_builder_set_natives(b, table), GRJIT_OK);
  }
  /* A call to a registered native without a status: one frame state. */
  void call_native(GRJIT_VReg d, uint32_t id, std::vector<GRJIT_Operand> args,
      GRCORE_PollIdentity sid, std::vector<GRJIT_FrameSlot> state) {
    GRJIT_FrameState st{sid, state.size(), state.data()};
    EXPECT_EQ(grjit_builder_call_native(b, d, id, args.data(), args.size(), &st, nullptr),
        GRJIT_OK);
  }
  /* The same with a status: the state after the call as well. */
  void call_native(GRJIT_VReg d, uint32_t id, std::vector<GRJIT_Operand> args,
      GRCORE_PollIdentity sid, std::vector<GRJIT_FrameSlot> state, GRCORE_PollIdentity aid,
      std::vector<GRJIT_FrameSlot> after) {
    GRJIT_FrameState st{sid, state.size(), state.data()};
    GRJIT_FrameState af{aid, after.size(), after.data()};
    EXPECT_EQ(grjit_builder_call_native(b, d, id, args.data(), args.size(), &st, &af),
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

/* A table of natives the test owns. */
struct NativeTab {
  GRJIT_NativeTable * t = nullptr;
  NativeTab(const NativeTab &) = delete;
  NativeTab & operator=(const NativeTab &) = delete;
  explicit NativeTab(const GRJIT_Limits * limits = nullptr,
      const GRJIT_Allocator * allocator = nullptr) {
    EXPECT_EQ(grjit_native_table_create(limits, allocator, &t), GRJIT_OK);
  }
  ~NativeTab() { grjit_native_table_free(t); }
  /* Registers a native at `fn` (never called by a test that only builds IR) and
   * returns its id. */
  uint32_t add(const void * fn, std::vector<GRJIT_Type> params, GRJIT_Type result = GRJIT_NATIVE_NO_RESULT,
      uint32_t flags = 0, uint32_t stack_bytes = 0) {
    GRJIT_NativeDesc d{};
    d.address = reinterpret_cast<uintptr_t>(fn);
    d.params = params.data();
    d.param_count = params.size();
    d.result = result;
    d.flags = flags;
    d.stack_bytes = stack_bytes;
    uint32_t id = UINT32_MAX;
    EXPECT_EQ(grjit_native_table_add(t, &d, &id), GRJIT_OK);
    return id;
  }
  operator GRJIT_NativeTable *() { return t; }
  operator const GRJIT_NativeTable *() const { return t; }
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

/* The targets the library has a backend for, spelled as the library spells
 * them (src/code/code.c): Linux x86-64, Linux arm64 and Windows x86-64. */
#if ((defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)) || \
    (defined(_WIN64) && defined(__x86_64__))
#define GRJIT_TEST_HAVE_BACKEND 1
#else
#define GRJIT_TEST_HAVE_BACKEND 0
#endif

/* On a gated target (Linux x86-64, Linux arm64, Windows x86-64) the backend
 * must exist: a test that cannot run there fails, it is never skipped.
 * Elsewhere (Windows arm64, macOS) it must say it is absent, and the test is
 * reported as SKIPPED, so that a target without a backend shows a count of
 * what was not run and not a count of tests that passed having proved
 * nothing. The condition is the one the library compiles its backend under,
 * and a test in test_compile.cpp asserts that exactly these targets report
 * true. */
#if GRJIT_TEST_HAVE_BACKEND
#define GRJIT_REQUIRE_BACKEND() \
  ASSERT_TRUE(grjit_backend_available()) \
      << "the backend is unavailable on the gated target"
#else
#define GRJIT_REQUIRE_BACKEND() \
  do { \
    ASSERT_FALSE(grjit_backend_available()); \
    GTEST_SKIP() << "no native code backend on this target"; \
  } while (0)
#endif

/* ---- A child process where there is no fork ------------------------------------------------ */

/* Some tests need a process that may die: a walk that meets a corrupt frame aborts with a message, which is
 * the behaviour under test. On POSIX the test forks. Windows has no fork, so the test binary runs itself
 * again: `--gtest_filter` names the running test and the environment variable `GRJIT_TEST_CHILD` carries the
 * number of the child, counted from 1 in the order the test calls ::grjit_test::run_in_child. The child
 * runs the test from its start (state is rebuilt, not copied), and each call before its own number does
 * nothing; at its own it runs the body and leaves with status zero if the body came back, or by the
 * abort the body made. The parent reads the exit status and everything the child wrote to `stderr`.
 * `aborted` is true for the abort of the C runtime (status 3, or the fast-fail status newer runtimes
 * use), which is how `abort()` ends a process here. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
/* Names the platform header takes that the tests use for their own (the fixture's `K::CONST`). */
#undef CONST
#undef IN
#undef OUT
#undef ERROR
namespace grjit_test {

struct ChildOutcome {
  bool aborted = false;
  bool ran = false;       ///< The child started and ended (not that it was found).
  unsigned long pid = 0;  ///< The child's process id.
  bool timed_out = false; ///< It did not end in the time allowed and was terminated: a failure, never an abort.
  unsigned long status = 0;
  std::string err;
};

/* `timeout_ms` is how long the parent waits for the child; a child still running then is terminated and the
 * outcome says `timed_out`. That is a failure of the test, unless `expect_timeout` says it is the behaviour under test. */
inline ChildOutcome run_in_child(const std::function<void()> & body, unsigned timeout_ms = 120000,
    bool expect_timeout = false) {
  static std::string last_test;
  static int calls = 0;
  const ::testing::TestInfo * info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string name = std::string(info->test_suite_name()) + "." + info->name();
  if (name != last_test) {
    last_test = name;
    calls = 0;
  }
  const int mine = ++calls;
  ChildOutcome out;
  const char * which = std::getenv("GRJIT_TEST_CHILD");
  if (which != nullptr) {
    // This is the child: the body runs at its own number; the calls before it are not run.
    if (std::atoi(which) == mine) {
      std::fflush(nullptr);
      body();
      std::fflush(nullptr);
      std::_Exit(0);
    }
    return out;
  }
  char exe[MAX_PATH * 2];
  const DWORD len = GetModuleFileNameA(nullptr, exe, sizeof exe);
  if (len == 0 || len >= sizeof exe) {
    ADD_FAILURE() << "the path of the test binary is unknown or was truncated";
    return out;
  }
  // The child's stderr goes to a file, so that waiting for it never depends on it closing a pipe and a
  // hung child can be given up on.
  char tmp_dir[MAX_PATH], tmp_name[MAX_PATH];
  if (GetTempPathA(sizeof tmp_dir, tmp_dir) == 0 || GetTempFileNameA(tmp_dir, "gct", 0, tmp_name) == 0) {
    ADD_FAILURE() << "no temporary file for the child's stderr";
    return out;
  }
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE err = CreateFileA(tmp_name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  if (err == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) {
    ADD_FAILURE() << "could not open the child's output files";
    return out;
  }
  SetEnvironmentVariableA("GRJIT_TEST_CHILD", std::to_string(mine).c_str());
  STARTUPINFOA si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = nul;
  si.hStdError = err;
  PROCESS_INFORMATION pi{};
  std::string cmd = std::string("\"") + exe + "\" --gtest_filter=" + name;
  std::vector<char> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back('\0');
  std::fflush(nullptr);
  const BOOL started = CreateProcessA(exe, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
  EXPECT_TRUE(started) << "the test binary could not run itself";
  SetEnvironmentVariableA("GRJIT_TEST_CHILD", nullptr);
  CloseHandle(err);
  CloseHandle(nul);
  if (started) {
    out.pid = pi.dwProcessId;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 10000);
      out.timed_out = true;
      if (!expect_timeout) {
        ADD_FAILURE() << "the child did not end in " << timeout_ms << " ms and was terminated";
      }
    }
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
      out.status = code; // for a child that was terminated: the code it was terminated with
      if (!out.timed_out && code != STILL_ACTIVE) {
        out.ran = true;
        out.aborted = code == 3 || code == 0xC0000409u;
      }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  if (FILE * f = std::fopen(tmp_name, "rb")) {
    char buf[512];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
      out.err.append(buf, got);
    }
    std::fclose(f);
  }
  DeleteFileA(tmp_name);
  return out;
}

} // namespace grjit_test
#endif

/* A watchdog for every test: compiled code that is wrong can loop forever (a
 * frame base left pointing at the wrong frame does), and a hang is neither a
 * failure a test reports nor a result a harness can read as a catch. Each test
 * is given `GRJIT_TEST_WATCHDOG_SECONDS` seconds (default 120; zero or the
 * variable set to 0 turns it off, which tools/check-planted-calls.py does for its
 * own self-test of a hang); on SIGALRM it names the test and aborts, so the run
 * dies by a signal, which is a verdict. Not on Windows, which has no alarm. */
#ifndef _WIN32
#include <csignal>
#include <unistd.h>
namespace grjit_watchdog {
inline const char * g_current = "";
inline void on_alarm(int) {
  const char * a = "\nWATCHDOG: a test did not finish in time and is aborted: ";
  (void)!write(2, a, std::strlen(a));
  (void)!write(2, g_current, std::strlen(g_current));
  (void)!write(2, "\n", 1);
  std::abort();
}
class Listener : public ::testing::EmptyTestEventListener {
 public:
  explicit Listener(unsigned seconds) : seconds_(seconds) {}
  void OnTestStart(const ::testing::TestInfo & info) override {
    name_ = std::string(info.test_suite_name()) + "." + info.name();
    g_current = name_.c_str();
    alarm(seconds_);
  }
  void OnTestEnd(const ::testing::TestInfo &) override { alarm(0); }
 private:
  unsigned seconds_;
  std::string name_;
};
inline void install() {
  unsigned seconds = 120;
  if (const char * v = std::getenv("GRJIT_TEST_WATCHDOG_SECONDS")) {
    seconds = static_cast<unsigned>(std::atoi(v));
  }
  if (seconds == 0) {
    return;
  }
  std::signal(SIGALRM, on_alarm);
  ::testing::UnitTest::GetInstance()->listeners().Append(new Listener(seconds));
}
} // namespace grjit_watchdog
#define GRJIT_WATCHDOG_INSTALL() ::grjit_watchdog::install()
#else
#define GRJIT_WATCHDOG_INSTALL() ((void)0)
#endif

/// Every test file ends with this: each is its own executable.
#define GRJIT_TEST_MAIN()                                                      \
  int main(int argc, char ** argv) {                                           \
    ::testing::InitGoogleTest(&argc, argv);                                    \
    GRJIT_WATCHDOG_INSTALL();                                                  \
    return RUN_ALL_TESTS();                                                    \
  }

#endif
