/**
 * @file
 *
 * A fixture engine for calls between compiled functions (AD-28): a tiny guest
 * language with an interpreter and a compiler to the IR, and the hooks, the
 * entry slots, the registry, the reservation and the heap an engine supplies.
 *
 * It stands in for lang-tang so that the protocol is proved in `runtime-jit`
 * before an engine depends on it; story 8 replaces its hooks with Tang's. What
 * it is for:
 *
 *  - the same program runs interpreted and compiled, and its result is checked
 *    against a C++ reference, so "no tier differs" is a measurement and not a
 *    claim;
 *  - a deoptimization anywhere in a chain leaves guest frames that the
 *    interpreter finishes, with the uninterrupted output as the expected one;
 *  - the heap is a moving collector over the roots core reports, that poisons
 *    what it moves, so a reference the walk left out, or reported and not
 *    updated, is a poisoned read, which is how the planted defects are seen;
 *  - every hook counts what it is asked, so a test can say what was and was not
 *    called.
 *
 * The guest frame is `[pc, local 0, local 1, ...]`. A call is the instruction at
 * `pc`: while the callee runs (compiled or not) the caller's frame is at that
 * `pc`, and when the callee returns the interpreter completes the call at it,
 * which is why the call's two frame states are the same here.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_CALLS_FIXTURE_H
#define GHOTI_IO_GRJIT_TESTS_CALLS_FIXTURE_H

#include "test_helpers.h"

#include <ghoti.io/runtime-core/a/compiled.h>
#include <ghoti.io/runtime-core/a/deopt.h>
#include <ghoti.io/runtime-core/a/layout.h>
#include <ghoti.io/runtime-core/a/registry.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fx {

using u64 = uint64_t;

/* ---- The guest language --------------------------------------------------- */

enum class K {
  CONST,     // d = imm
  MOV,       // d = a
  ADD,       // d = a + b
  SUB,       // d = a - b
  MUL,       // d = a * b
  AND,       // d = a & b
  LT,        // d = a < b (signed)
  EQ,        // d = a == b
  BRZ,       // if a == 0 goto t
  BR,        // goto t
  CALL,      // d = fn(args...) through fn's entry slot
  CALLP,     // d = fn(args...) through the code pointer in local a
  RET,       // return a
  GUARD,     // compiled: deoptimize when a == 0; interpreted: nothing
  POLL,      // a poll
  NEW,       // d = a new heap object holding imm
  GET,       // d = the value of the object in a
  COLLECT,   // a collection
  CLEARSLOT, // clear fn's entry slot (retiring the code a frame may be in)
  ENTRYOF,   // d = the internal entry of fn's compiled code, or 0
  PROBE,     // record local a (and the native stack pointer, compiled)
  REFSLOT,   // refuse fn's entry slot, as if it could not be compiled
  DERIVE,    // d (a PTR) = the address a (a REF) holds plus imm: a derived pointer
  LOAD,      // d = the 64-bit word at the address in a (a PTR or REF)
  TAILCALL,  // return fn(args...): replaces this frame, through fn's entry slot
  TAILCALLP  // the same through the code pointer in local a
};

struct Ins {
  K k = K::RET;
  int d = -1, a = -1, b = -1;
  int64_t imm = 0;
  int t = 0;
  int fn = -1;
  std::vector<int> args;
};

struct Func {
  std::string name;
  int nparams = 0;
  std::vector<GRJIT_Type> type; // one per local, parameters first
  std::vector<Ins> code;
  /* The frame owns a budget scope or an engine call record: a tail call from it
   * is refused by the engine's hook, goes through an exit, and the interpreter
   * makes it as a call whose result this frame returns (AD-28). */
  bool owns_scope = false;
  /* Immediate operands of call arguments (and of a tail call's code pointer): a
   * negative `-1 - k` in an argument list is `imms[k]`. */
  std::vector<int64_t> imms;
  int locals() const { return static_cast<int>(type.size()); }
};

/* A small assembler, so programs read as programs. */
struct P {
  Func f;
  explicit P(const char * name, std::vector<GRJIT_Type> params = {}) {
    f.name = name;
    f.nparams = static_cast<int>(params.size());
    f.type = std::move(params);
  }
  int local(GRJIT_Type t = GRJIT_TYPE_I64) {
    f.type.push_back(t);
    return static_cast<int>(f.type.size()) - 1;
  }
  int here() const { return static_cast<int>(f.code.size()); }
  /* An immediate, for a call's argument list or a tail call's code pointer. */
  int imm(int64_t v) {
    f.imms.push_back(v);
    return -static_cast<int>(f.imms.size());
  }
  Ins & add(K k) {
    f.code.emplace_back();
    f.code.back().k = k;
    return f.code.back();
  }
  void cnst(int d, int64_t v) { auto & i = add(K::CONST); i.d = d; i.imm = v; }
  void mov(int d, int a) { auto & i = add(K::MOV); i.d = d; i.a = a; }
  void bin(K k, int d, int a, int b) { auto & i = add(k); i.d = d; i.a = a; i.b = b; }
  /* Returns the index of the branch, to patch its target. */
  int brz(int a) { auto & i = add(K::BRZ); i.a = a; return here() - 1; }
  int br() { add(K::BR); return here() - 1; }
  void patch(int at, int target) { f.code[at].t = target; }
  void call(int d, int fn, std::vector<int> args) {
    auto & i = add(K::CALL); i.d = d; i.fn = fn; i.args = std::move(args);
  }
  void callp(int d, int ptr, int fn, std::vector<int> args) {
    auto & i = add(K::CALLP); i.d = d; i.a = ptr; i.fn = fn; i.args = std::move(args);
  }
  void tailcall(int fn, std::vector<int> args) {
    auto & i = add(K::TAILCALL); i.fn = fn; i.args = std::move(args);
  }
  void tailcallp(int ptr, int fn, std::vector<int> args) {
    auto & i = add(K::TAILCALLP); i.a = ptr; i.fn = fn; i.args = std::move(args);
  }
  void ret(int a) { auto & i = add(K::RET); i.a = a; }
  void guard(int a) { auto & i = add(K::GUARD); i.a = a; }
  void poll() { add(K::POLL); }
  void nw(int d, int64_t v) { auto & i = add(K::NEW); i.d = d; i.imm = v; }
  void get(int d, int a) { auto & i = add(K::GET); i.d = d; i.a = a; }
  void collect() { add(K::COLLECT); }
  void clearslot(int fn) { auto & i = add(K::CLEARSLOT); i.fn = fn; }
  void refslot(int fn) { auto & i = add(K::REFSLOT); i.fn = fn; }
  void derive(int d, int a, int64_t off) { auto & i = add(K::DERIVE); i.d = d; i.a = a; i.imm = off; }
  void load(int d, int a) { auto & i = add(K::LOAD); i.d = d; i.a = a; }
  void entryof(int d, int fn) { auto & i = add(K::ENTRYOF); i.d = d; i.fn = fn; }
  void probe(int a) { auto & i = add(K::PROBE); i.a = a; }
  Func done() { return f; }
};

/* ---- The heap: a moving collector that poisons what it moves --------------- */

struct Obj {
  u64 magic;
  int64_t value;
};
constexpr u64 kLive = 0x4C4956454C495645ull;
constexpr u64 kDead = 0xDEADDEADDEADDEADull;
constexpr int64_t kPoisonValue = -777777777;

struct Heap {
  std::unordered_set<u64> live;                // addresses of live objects
  std::vector<std::unique_ptr<Obj>> graveyard; // quarantined: reads stay safe
  std::vector<std::unique_ptr<Obj>> store;
  long poisoned_reads = 0;
  long collections = 0;
  long moved = 0;
  bool moving = true;

  u64 alloc(int64_t v) {
    store.push_back(std::make_unique<Obj>(Obj{kLive, v}));
    u64 a = reinterpret_cast<u64>(store.back().get());
    live.insert(a);
    return a;
  }
  /* The value of a reference; a poisoned one is counted, and gives the poison. */
  int64_t read(u64 ref) {
    if (ref == 0) {
      poisoned_reads++;
      return kPoisonValue;
    }
    const Obj * o = reinterpret_cast<const Obj *>(ref);
    if (o->magic != kLive) {
      poisoned_reads++;
    }
    return o->value;
  }
};

/* ---- Compiled code the engine owns ----------------------------------------- */

struct CodePayload {
  GRJIT_Code * code = nullptr;
  std::shared_ptr<int> released;
};

struct CompiledFn {
  GRJIT_Code * code = nullptr;     // owned by the handle's payload once made
  GRCORE_Code * handle = nullptr;  // the registry's and the slot's references
  uintptr_t internal = 0;
  size_t max_converting = 0;       // the callee's maximum, for the reservation
  GRCORE_CodeMeta meta{};          // the registered table (a copy when patched)
  std::vector<GRCORE_CodeSite> sites;
  std::vector<std::vector<GRCORE_CodeLocation>> states;
  uintptr_t start = 0;
};

class Engine;
inline Engine * g_engine = nullptr;

/* How the run ended. */
struct Outcome {
  bool finished = false;   // a value was produced
  u64 value = 0;
  bool limit = false;      // the guest-depth budget refused a call
  uint32_t exit = 99;      // the adapter's exit, for a compiled run
  bool interpreted_rest = false; // the interpreter finished after a deopt
  size_t frames_left = 0;  // guest frames still on the stack (a failure)
  size_t limit_depth = 0;  // guest frames on the stack when a call was refused
  bool failed = false;     // the interpreter met a state no rebuild leaves
  bool rebuild_failed = false; // the adapter reported GRJIT_EXIT_REBUILD_FAILED
  u64 failed_with = 0;     // and the hook's answer, out[0]
  std::vector<u64> pcs_after_failure; // each guest frame's pc slot, before the unwind
};

struct Stats {
  long pushes = 0, pops = 0, compiles = 0, compile_refusals = 0, deopts = 0;
  long polls = 0, poll_slow = 0, news = 0, collects = 0, probes = 0, compile_calls = 0;
  long extends = 0, retracts = 0;
  long deopt_frames = 0;
  GRCORE_Result rebuild = GRCORE_OK;
  GRCORE_Result extend_result = GRCORE_OK;
  uint64_t last_cause = 0;
  long refused_pushes = 0;
  int last_innermost_fn = -1;      // the function the deopt started in
  bool last_was_call_exit = false; // and whether it was an exit at a call site
  std::map<int, int> counted;      // deopts counted against each function
  std::vector<u64> probed;       // values PROBE saw
  std::vector<uintptr_t> sps;    // the native stack pointer at each compiled PROBE
  std::vector<uintptr_t> bases;  // the compiled frame base at each compiled PROBE
  std::vector<uintptr_t> saved;  // the word at the probing frame's base: its caller's base
  std::vector<uintptr_t> rets;   // and the word above it: its return address
  std::vector<size_t> frames;    // guest frames on the stack at each PROBE
  std::vector<uint64_t> depths;  // the context's guest depth at each PROBE
  std::vector<size_t> caps;      // the reservation's capacity at each PROBE
  long tails = 0;                // the tail hook's calls
  long tail_refusals = 0;        // ... that it refused
  long reserve_refusals = 0;     // ... of those, for want of room on the guest stack
};

/* An engine whose I64 locals are held boxed in its guest frames (the converting
 * representation of AD-27), so that a compiled frame's raw words are converted
 * when it is rebuilt: how the reservation is exercised, since the IR has no
 * converting types of its own. */
constexpr u64 kBoxTag = 1;
inline u64 box(u64 raw) { return (raw << 1) | kBoxTag; }
inline u64 unbox(u64 b) { return b >> 1; }

class Engine {
 public:
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  GRCORE_Stack * stack = nullptr;
  GRCORE_EngineId engine = 0;
  std::vector<Func> funcs;
  std::vector<GRCORE_EntrySlot *> slots;
  /* The registry keeps a pointer to each code's meta for as long as the range is
   * registered, retired or not, so a CompiledFn is never moved or reset: each is
   * made in `arena`, and `cur` says which one a function has now. */
  std::deque<CompiledFn> arena;
  std::vector<CompiledFn *> cur;
  CompiledFn none;
  CompiledFn & C(int fn) { return cur[static_cast<size_t>(fn)] != nullptr ? *cur[static_cast<size_t>(fn)] : none; }
  const CompiledFn & C(int fn) const { return cur[static_cast<size_t>(fn)] != nullptr ? *cur[static_cast<size_t>(fn)] : none; }
  std::set<int> uncompilable;
  std::set<int> never_compile; // discarded for deoptimizing too often
  Heap heap;
  Stats st;
  std::shared_ptr<int> released = std::make_shared<int>(0);
  GRCORE_DeoptReservation * reservation = nullptr;
  std::vector<size_t> extensions; // what each open push extended by
  bool boxed = false;             // I64 locals are converting in the frames
  long short_by = 0;              // planted: each extension is this many short
  bool torture = false;           // collect at every GC point
  long poll_slow_calls_to_fail = 0; // the Nth slow poll answers non-zero
  long poll_count_for_fail = 0;
  std::function<void(Engine &)> on_probe;
  std::function<void(Engine &)> on_collect; // runs at a COLLECT, a GC point
  size_t base_frames = 0;         // guest frames below the current entry
  uintptr_t last_native_limit = 0;   // the limit word the last compiled run set
  bool interpreter_cannot_recover = false; // a refused rebuild: do not try to finish
  int discard_limit = 8;           // deopts after which a function's code is discarded
  bool lie_about_installing = false; // the compile hook says it installed and does not
  TrackingAllocator * refuse_extend_with = nullptr; // refuses the allocation of the Nth extension
  long refuse_extend_at = 0;
  long extend_calls = 0;
  uint32_t refuse_rebuild_with = 0; // the deopt hook refuses without rebuilding
  long refuse_push_at = 0;        // the Nth push is refused (an exit at the call site)
  long push_calls = 0;
  long refuse_tail_at = 0;        // the Nth tail hook call is refused (an exit)
  long tail_calls = 0;
  bool tail_keeps_frame = false;  // planted: the tail hook pushes and does not replace
  bool tail_keeps_extension = false; // planted: it does not give back the caller's extension
  /* The interpreter has no derived pointers: a raw pointer it holds (a PTR slot,
   * as a rebuild leaves one) would be stale after a moving collection it makes. A
   * test whose programs derive pointers and whose interpreter finishes a run
   * sets this, and a collection the interpreter makes does not move anything. */
  bool interpreter_never_moves = false;

  explicit Engine(uint64_t guest_depth = GRCORE_UNLIMITED,
      uint64_t native_bytes = GRCORE_UNLIMITED, bool conv = false,
      const GRCORE_Allocator * allocator = nullptr, uint64_t memory_bytes = GRCORE_UNLIMITED);
  ~Engine();
  Engine(const Engine &) = delete;
  Engine & operator=(const Engine &) = delete;

  int add(Func f) {
    funcs.push_back(std::move(f));
    GRCORE_EntrySlot * s = nullptr;
    EXPECT_EQ(grcore_entry_slot_create(ctx, &s), GRCORE_OK);
    slots.push_back(s);
    cur.push_back(nullptr);
    return static_cast<int>(funcs.size()) - 1;
  }
  /* Reserves an index before the body exists, for recursion. */
  int reserve() { return add(Func{}); }
  void set(int fn, Func f) { funcs[fn] = std::move(f); }

  // ---- slots: what the guest frame holds ----
  u64 rd(const uint64_t * S, int fn, int local) const {
    u64 w = S[1 + local];
    return boxed && funcs[fn].type[local] == GRJIT_TYPE_I64 ? unbox(w) : w;
  }
  void wr(uint64_t * S, int fn, int local, u64 v) const {
    S[1 + local] = boxed && funcs[fn].type[local] == GRJIT_TYPE_I64 ? box(v) : v;
  }

  // ---- compiling ----
  GRJIT_Function * build_ir(int fn);
  bool compile_fn(int fn);                 // compile, register, install in the slot
  void install(int fn);
  const CompiledFn & code_of(int fn) const { return C(fn); }

  // ---- running ----
  Outcome run_interpreted(int fn, const std::vector<u64> & args);
  Outcome run_compiled(int fn, const std::vector<u64> & args);
  void interpret(Outcome & out);           // finish the guest frames
  bool push_frame(int fn, const u64 * args, size_t n, bool compiled_call);
  void pop_frame();
  void collect();
  void reset_reservation();
  /* What an engine does to replace or drop a function's code: empty the slot (the
   * code is retired while a frame may return into it) and unregister the range. */
  void discard(int fn);

  // ---- hooks (C ABI) ----
  static uint32_t h_push(void *, uint64_t, const uint64_t *, uint64_t);
  static uint32_t h_tail(void *, uint64_t, const uint64_t *, uint64_t);
  static void h_pop(void *);
  static uint32_t h_compile(void *, uint64_t);
  static uint32_t h_deopt(void *, uint64_t);
  static uint32_t h_poll(void *, uint64_t, uint64_t);
  static uint64_t h_new(uint64_t);
  static uint64_t h_collect();
  static uint64_t h_clear(uint64_t);
  static uint64_t h_entryof(uint64_t);
  static uint64_t h_probe(uint64_t);
  static uint64_t h_refslot(uint64_t);
  static GRCORE_SlotKind slot_kind(const GRCORE_AbstractFrame *, size_t);
  static void convert(GRCORE_Context *, GRCORE_Representation, uint64_t, uint64_t *);
  static bool reverse(GRCORE_Context *, GRCORE_Representation, uint64_t, uint64_t *);

  const GRCORE_PageProvider * pages() const { return grcore_context_page_provider(ctx); }
};

inline const GRCORE_EngineDescriptor kPlainDescriptor = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "fixture", Engine::slot_kind, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);
inline const GRCORE_EngineDescriptor kConvDescriptor = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "fixture-conv", Engine::slot_kind, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, Engine::convert,
    Engine::reverse);

/* ---- Construction and teardown --------------------------------------------- */

inline Engine::Engine(uint64_t guest_depth, uint64_t native_bytes, bool conv,
    const GRCORE_Allocator * allocator, uint64_t memory_bytes) {
  boxed = conv;
  GRCORE_Options * o = nullptr;
  EXPECT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
  grcore_options_set_guest_depth(o, guest_depth);
  grcore_options_set_native_stack_bytes(o, native_bytes);
  if (memory_bytes != GRCORE_UNLIMITED) {
    grcore_options_set_memory_bytes(o, memory_bytes);
    grcore_options_set_memory_reserve(o, 0);
  }
  EXPECT_EQ(grcore_group_create(allocator, nullptr, &group), GRCORE_OK);
  EXPECT_EQ(grcore_context_create(group, o, &ctx), GRCORE_OK);
  grcore_options_destroy(o);
  EXPECT_EQ(grcore_engine_register(ctx, conv ? &kConvDescriptor : &kPlainDescriptor,
                &engine),
      GRCORE_OK);
  stack = grcore_context_stack(ctx);
  EXPECT_EQ(grcore_deopt_reserve(ctx, 0, &reservation), GRCORE_OK);
  g_engine = this;
}

inline Engine::~Engine() {
  // Frames left over (a failed test) are unwound, and the code is let go while the
  // context still has its page provider to give the pages back to.
  grcore_unwind_all(stack, nullptr);
  for (size_t fn = 0; fn < cur.size(); fn++) {
    if (cur[fn] != nullptr) {
      grcore_entry_slot_clear(ctx, slots[fn]);
      grcore_code_unregister(ctx, cur[fn]->start);
    }
  }
  grcore_deopt_release(ctx, reservation);
  EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  g_engine = nullptr;
}

inline GRCORE_SlotKind Engine::slot_kind(const GRCORE_AbstractFrame * f, size_t index) {
  Engine * e = g_engine;
  if (index == 0) {
    return GRCORE_SLOT_RAW; // the pc
  }
  size_t fn = static_cast<size_t>(f->identity.function);
  if (e == nullptr || fn >= e->funcs.size() || index - 1 >= e->funcs[fn].type.size()) {
    return GRCORE_SLOT_RAW;
  }
  GRJIT_Type t = e->funcs[fn].type[index - 1];
  if (t == GRJIT_TYPE_REF) {
    return GRCORE_SLOT_VALUE;
  }
  // A boxed I64 is a converted value, which the engine's roots may be told is
  // one; the collector filters by the heap, so it is harmless to say so.
  return e->boxed && t == GRJIT_TYPE_I64 ? GRCORE_SLOT_VALUE : GRCORE_SLOT_RAW;
}

inline void Engine::convert(GRCORE_Context *, GRCORE_Representation rep, uint64_t raw,
    uint64_t * out_root) {
  (void)rep;
  *out_root = box(raw);
}

inline bool Engine::reverse(GRCORE_Context *, GRCORE_Representation, uint64_t value,
    uint64_t * out_raw) {
  if ((value & kBoxTag) == 0) {
    return false;
  }
  *out_raw = unbox(value);
  return true;
}

/* ---- Frames --------------------------------------------------------------- */

inline bool Engine::push_frame(int fn, const u64 * args, size_t n, bool compiled_call) {
  (void)compiled_call;
  const Func & F = funcs[fn];
  GRCORE_FrameRef ref;
  GRCORE_Result r = grcore_stack_push(stack, engine, 1 + F.locals(), &ref);
  if (r != GRCORE_OK) {
    return false;
  }
  grcore_stack_set_identity(stack, ref, GRCORE_PollIdentity{static_cast<u64>(fn), 0});
  uint64_t * S = grcore_stack_slots(stack, ref);
  S[0] = 0;
  for (int i = 0; i < F.locals(); i++) {
    wr(S, fn, i, static_cast<size_t>(i) < n ? args[i] : 0);
  }
  st.pushes++;
  return true;
}

inline void Engine::pop_frame() {
  EXPECT_EQ(grcore_stack_pop(stack), GRCORE_OK);
  st.pops++;
}

inline void Engine::discard(int fn) {
  CompiledFn * c = cur[static_cast<size_t>(fn)];
  if (c == nullptr) {
    return;
  }
  EXPECT_EQ(grcore_entry_slot_clear(ctx, slots[static_cast<size_t>(fn)]), GRCORE_OK);
  EXPECT_EQ(grcore_code_unregister(ctx, c->start), GRCORE_OK);
  cur[static_cast<size_t>(fn)] = nullptr; // the object stays, for the retired range
}

inline void Engine::reset_reservation() {
  while (!extensions.empty()) {
    grcore_deopt_reservation_retract(reservation, extensions.back());
    extensions.pop_back();
  }
}

/* ---- The heap's collector -------------------------------------------------- */

inline void Engine::collect() {
  heap.collections++;
  struct Ctx {
    Engine * e;
    std::unordered_map<u64, u64> fwd;
  } c{this, {}};
  GRCORE_RootVisitor v = {};
  v.user = &c;
  v.slot = [](void * user, uint64_t * slot) {
    auto * c = static_cast<Ctx *>(user);
    u64 w = *slot;
    // A boxed integer or a plain one is no reference: only a heap address is.
    if (c->e->heap.live.count(w) == 0) {
      return;
    }
    if (c->e->heap.moving) {
      auto it = c->fwd.find(w);
      if (it == c->fwd.end()) {
        auto copy = std::make_unique<Obj>(*reinterpret_cast<Obj *>(w));
        u64 n = reinterpret_cast<u64>(copy.get());
        c->e->heap.store.push_back(std::move(copy));
        c->e->heap.live.insert(n);
        it = c->fwd.emplace(w, n).first;
      }
      *slot = it->second;
    } else {
      c->fwd.emplace(w, w);
    }
  };
  std::vector<u64> before(heap.live.begin(), heap.live.end());
  EXPECT_EQ(grcore_context_enumerate_roots(ctx, &v), GRCORE_OK);
  // Whatever nothing reached is dead, and what moved left its old cell behind:
  // either way the old cell is poisoned, so a reference that was not updated
  // reads the poison.
  std::vector<u64> doomed;
  for (u64 a : before) {
    auto it = c.fwd.find(a);
    if (it == c.fwd.end() || it->second != a) {
      doomed.push_back(a);
    }
  }
  for (u64 a : doomed) {
    Obj * o = reinterpret_cast<Obj *>(a);
    o->magic = kDead;
    o->value = kPoisonValue;
    heap.live.erase(a);
    heap.moved++;
  }
}

/* ---- Compiling a guest function to the IR ---------------------------------- */

inline GRJIT_Function * Engine::build_ir(int fn) {
  const Func & F = funcs[fn];
  const int L = F.locals();
  B b(F.name.c_str(), 1 + static_cast<size_t>(L));
  GRJIT_CallHooks hooks{};
  hooks.push = Engine::h_push;
  hooks.pop = Engine::h_pop;
  hooks.compile = Engine::h_compile;
  hooks.deopt = Engine::h_deopt;
  hooks.tail = Engine::h_tail;
  b.callable(hooks);
  EXPECT_EQ(grjit_builder_set_token(b.b, static_cast<u64>(fn)), GRJIT_OK);
  b.poll_helper(Engine::h_poll);
  for (int i = 0; i < F.nparams; i++) {
    b.param(F.type[i]);
  }
  for (int i = F.nparams; i < L; i++) {
    b.reg(F.type[i]);
  }
  for (const Ins & dv : F.code) {
    if (dv.k == K::DERIVE) {
      b.derived(static_cast<GRJIT_VReg>(dv.d), static_cast<GRJIT_VReg>(dv.a), dv.imm);
    }
  }
  // Basic blocks: every branch target, and what follows a branch or a return.
  std::set<int> leaders = {0};
  const int n = static_cast<int>(F.code.size());
  for (int pc = 0; pc < n; pc++) {
    const Ins & in = F.code[pc];
    if (in.k == K::BRZ || in.k == K::BR) {
      leaders.insert(in.t);
      leaders.insert(pc + 1);
    } else if (in.k == K::RET || in.k == K::TAILCALL || in.k == K::TAILCALLP) {
      leaders.insert(pc + 1);
    }
  }
  std::map<int, GRJIT_BlockId> blk;
  for (int pc : leaders) {
    if (pc < n) {
      blk[pc] = b.block();
    }
  }
  auto state = [&](int pc) {
    std::vector<GRJIT_FrameSlot> s;
    s.push_back(grjit_frame_slot_constant(pc));
    for (int i = 0; i < L; i++) {
      s.push_back(grjit_frame_slot_vreg(static_cast<GRJIT_VReg>(i)));
    }
    return s;
  };
  auto operand = [&](int a) {
    return a < 0 ? I(F.imms[static_cast<size_t>(-1 - a)]) : V(static_cast<GRJIT_VReg>(a));
  };
  auto operands = [&](const std::vector<int> & args) {
    std::vector<GRJIT_Operand> v;
    for (int a : args) {
      v.push_back(operand(a));
    }
    return v;
  };
  bool terminated = true;
  for (int pc = 0; pc < n; pc++) {
    if (blk.count(pc) != 0) {
      if (!terminated) {
        b.br(blk[pc]);
      }
      b.at(blk[pc]);
      terminated = false;
      if (pc == 0) {
        for (int i = F.nparams; i < L; i++) {
          b.cnst(static_cast<GRJIT_VReg>(i), 0);
        }
      }
    }
    const Ins & in = F.code[pc];
    const GRCORE_PollIdentity id{static_cast<u64>(fn), static_cast<u64>(pc)};
    switch (in.k) {
      case K::CONST: b.cnst(in.d, in.imm); break;
      case K::MOV: b.mov(in.d, V(in.a)); break;
      case K::ADD: b.bin(GRJIT_OP_ADD, in.d, V(in.a), V(in.b)); break;
      case K::SUB: b.bin(GRJIT_OP_SUB, in.d, V(in.a), V(in.b)); break;
      case K::MUL: b.bin(GRJIT_OP_MUL, in.d, V(in.a), V(in.b)); break;
      case K::AND: b.bin(GRJIT_OP_AND, in.d, V(in.a), V(in.b)); break;
      case K::LT: b.cmp(GRJIT_CMP_LT, in.d, V(in.a), V(in.b)); break;
      case K::EQ: b.cmp(GRJIT_CMP_EQ, in.d, V(in.a), V(in.b)); break;
      case K::BRZ: b.br_if(V(in.a), blk[pc + 1], blk[in.t]); terminated = true; break;
      case K::BR: b.br(blk[in.t]); terminated = true; break;
      case K::RET: b.ret(V(in.a)); terminated = true; break;
      case K::CALL:
        b.call_slot(in.d, &slots[in.fn]->entry, static_cast<u64>(in.fn), operands(in.args), id,
            state(pc), id, state(pc));
        break;
      case K::CALLP:
        b.call_ptr(in.d, V(in.a), static_cast<u64>(in.fn), operands(in.args), id, state(pc), id,
            state(pc));
        break;
      case K::TAILCALL:
        b.tail_call_slot(&slots[in.fn]->entry, static_cast<u64>(in.fn), operands(in.args), id,
            state(pc));
        terminated = true;
        break;
      case K::TAILCALLP:
        b.tail_call_ptr(operand(in.a), static_cast<u64>(in.fn), operands(in.args), id, state(pc));
        terminated = true;
        break;
      case K::GUARD: b.guard(V(in.a), id, state(pc)); break;
      case K::POLL: b.poll(id, state(pc)); break;
      case K::NEW:
        b.call_gc(in.d, reinterpret_cast<const void *>(Engine::h_new), {I(in.imm)}, id, state(pc),
            GRCORE_SITE_GC_POINT_ALLOC_SLOW);
        break;
      case K::GET: b.load(in.d, in.a, 8, 64); break;
      case K::LOAD: b.load(in.d, in.a, 0, 64); break;
      case K::DERIVE:
        b.bitcast(in.d, in.a);
        b.bin(GRJIT_OP_ADD, in.d, V(in.d), I(in.imm));
        break;
      case K::COLLECT:
        b.call_gc(GRJIT_NO_VREG, reinterpret_cast<const void *>(Engine::h_collect), {}, id,
            state(pc));
        break;
      case K::CLEARSLOT:
        b.call(GRJIT_NO_VREG, reinterpret_cast<const void *>(Engine::h_clear), {I(in.fn)});
        break;
      case K::REFSLOT:
        b.call(GRJIT_NO_VREG, reinterpret_cast<const void *>(Engine::h_refslot), {I(in.fn)});
        break;
      case K::ENTRYOF:
        b.call(in.d, reinterpret_cast<const void *>(Engine::h_entryof), {I(in.fn)});
        break;
      case K::PROBE:
        b.call(GRJIT_NO_VREG, reinterpret_cast<const void *>(Engine::h_probe), {V(in.a)});
        break;
    }
  }
  return b.finish();
}

inline bool Engine::compile_fn(int fn) {
  if (cur[static_cast<size_t>(fn)] != nullptr) {
    return true;
  }
  GRJIT_Function * f = build_ir(fn);
  char why[256] = {0};
  GRJIT_Result verified = grjit_function_verify(f, nullptr, why, sizeof why);
  if (verified != GRJIT_OK) {
    ADD_FAILURE() << "verifying " << funcs[fn].name << ": " << why;
  }
  GRJIT_CompileOptions o{};
  o.pages = pages();
  GRJIT_Code * code = nullptr;
  GRJIT_Result r = grjit_compile(&o, f, &code);
  grjit_function_destroy(f);
  if (r != GRJIT_OK) {
    ADD_FAILURE() << "compiling " << funcs[fn].name << ": " << r;
    return false;
  }
  arena.emplace_back();
  cur[static_cast<size_t>(fn)] = &arena.back();
  CompiledFn & c = *cur[static_cast<size_t>(fn)];
  c.code = code;
  c.start = reinterpret_cast<uintptr_t>(grjit_code_address(code));
  c.internal = grjit_code_internal_entry(code);
  // The table the registry keeps: the code's own, or, for a converting engine, a
  // copy in which the I64 locals' frame-state locations are raw I64 values the
  // engine converts when the frame is rebuilt (the IR has no converting type).
  const GRCORE_CodeMeta * m = grjit_code_meta(code);
  c.meta = *m;
  if (boxed) {
    c.sites.assign(m->sites, m->sites + m->site_count);
    c.states.resize(c.sites.size());
    for (size_t s = 0; s < c.sites.size(); s++) {
      c.states[s].assign(c.sites[s].frame_state,
          c.sites[s].frame_state + c.sites[s].frame_state_count);
      for (size_t i = 1; i < c.states[s].size(); i++) {
        if (funcs[fn].type[i - 1] == GRJIT_TYPE_I64 &&
            c.states[s][i].kind == GRCORE_LOC_FRAME_SLOT) {
          c.states[s][i].representation = GRCORE_REPR_I64;
        }
      }
      c.sites[s].frame_state = c.states[s].data();
    }
    c.meta.sites = c.sites.data();
  }
  c.max_converting = 0;
  for (size_t s = 0; s < c.meta.site_count; s++) {
    c.max_converting =
        std::max(c.max_converting, grcore_deopt_converting_count(&c.meta.sites[s]));
  }
  CodePayload * payload = new CodePayload{code, released};
  EXPECT_EQ(grcore_code_create(nullptr, payload,
                [](void * p) {
                  auto * pl = static_cast<CodePayload *>(p);
                  grjit_code_destroy(pl->code);
                  ++*pl->released;
                  delete pl;
                },
                &c.handle),
      GRCORE_OK);
  EXPECT_EQ(grcore_code_register(ctx, engine, c.handle, c.start, grjit_code_size(code), &c.meta),
      GRCORE_OK);
  EXPECT_EQ(grjit_entry_slot_install(ctx, slots[fn], c.handle, code, static_cast<u64>(fn),
                static_cast<size_t>(funcs[fn].nparams)),
      GRJIT_OK);
  // The registry's and the slot's references are the code's life now.
  grcore_code_release(c.handle);
  st.compiles++;
  return true;
}

/* ---- Hooks ------------------------------------------------------------------ */

inline uint32_t Engine::h_push(void *, uint64_t callee, const uint64_t * args, uint64_t n) {
  Engine & e = *g_engine;
  if (e.torture) {
    e.collect(); // the frame-push GC point: the arguments are read afterwards
  }
  if (e.refuse_push_at != 0 && ++e.push_calls == e.refuse_push_at) {
    e.st.refused_pushes++;
    return 1;
  }
  // The reservation first: a refusal here is an exit at the call site.
  size_t ext = e.C(callee).max_converting;
  size_t cut = static_cast<size_t>(e.short_by) < ext ? static_cast<size_t>(e.short_by) : ext;
  ext -= cut; // planted: a short extension
  bool refuse_alloc = e.refuse_extend_with != nullptr && ++e.extend_calls == e.refuse_extend_at;
  if (refuse_alloc) {
    e.refuse_extend_with->calls = 0;
    e.refuse_extend_with->fail_at = 1; // the first allocation the extension makes
  }
  GRCORE_Result r = grcore_deopt_reservation_extend(e.ctx, e.reservation, ext);
  if (refuse_alloc) {
    e.refuse_extend_with->fail_at = 0;
  }
  if (r != GRCORE_OK) {
    e.st.extend_result = r;
    e.st.refused_pushes++;
    return 1;
  }
  if (!e.push_frame(static_cast<int>(callee), args, n, true)) {
    grcore_deopt_reservation_retract(e.reservation, ext);
    e.st.refused_pushes++;
    return 1;
  }
  e.extensions.push_back(ext);
  e.st.extends++;
  return 0;
}

/* The tail hook (AD-28): replaces the top guest frame, the caller's, with the
 * callee's, and its reservation extension, as one step. Everything that can fail
 * or collect is done before the guest stack is touched, so a refusal leaves
 * everything as it was; the pop and push after it cannot fail. */
inline uint32_t Engine::h_tail(void *, uint64_t callee, const uint64_t * args, uint64_t n) {
  Engine & e = *g_engine;
  e.st.tails++;
  if (e.torture) {
    e.collect(); // a GC point: the arguments are read afterwards
  }
  GRCORE_FrameRef top = grcore_stack_top(e.stack);
  GRCORE_PollIdentity id;
  grcore_stack_identity(e.stack, top, &id);
  bool refuse = e.funcs[id.function].owns_scope ||
                (e.refuse_tail_at != 0 && ++e.tail_calls == e.refuse_tail_at);
  if (refuse) {
    e.st.tail_refusals++;
    return 1;
  }
  size_t ext = e.C(callee).max_converting;
  size_t cut = static_cast<size_t>(e.short_by) < ext ? static_cast<size_t>(e.short_by) : ext;
  ext -= cut;
  bool refuse_alloc = e.refuse_extend_with != nullptr && ++e.extend_calls == e.refuse_extend_at;
  if (refuse_alloc) {
    e.refuse_extend_with->calls = 0;
    e.refuse_extend_with->fail_at = 1;
  }
  GRCORE_Result r = grcore_deopt_reservation_extend(e.ctx, e.reservation, ext);
  if (refuse_alloc) {
    e.refuse_extend_with->fail_at = 0;
  }
  if (r != GRCORE_OK) {
    e.st.extend_result = r;
    e.st.tail_refusals++;
    return 1;
  }
  // Room for the callee's frame in place of the caller's: the growth, if any.
  size_t have = 0;
  EXPECT_EQ(grcore_stack_slot_count(e.stack, top, &have), GRCORE_OK);
  size_t want = 1 + static_cast<size_t>(e.funcs[callee].locals());
  if (want > have && grcore_stack_reserve(e.stack, 8 * (want - have)) != GRCORE_OK) {
    grcore_deopt_reservation_retract(e.reservation, ext);
    e.st.tail_refusals++;
    e.st.reserve_refusals++;
    return 1;
  }
  // Committed: nothing below can fail or collect.
  if (e.tail_keeps_frame) {
    // Planted: the caller's frame stays, as a call would leave it. The depth
    // budget, which it does not respect, may refuse it: an exit, as any refusal.
    if (!e.push_frame(static_cast<int>(callee), args, n, true)) {
      grcore_deopt_reservation_retract(e.reservation, ext);
      e.st.tail_refusals++;
      return 1;
    }
    e.extensions.push_back(ext);
    return 0;
  }
  if (!e.extensions.empty() && !e.tail_keeps_extension) {
    grcore_deopt_reservation_retract(e.reservation, e.extensions.back());
    e.extensions.back() = ext;
  } else {
    e.extensions.push_back(ext); // planted: the caller's is not given back
  }
  EXPECT_EQ(grcore_stack_pop(e.stack), GRCORE_OK);
  e.st.pops++;
  EXPECT_TRUE(e.push_frame(static_cast<int>(callee), args, n, true));
  return 0;
}

inline void Engine::h_pop(void *) {
  Engine & e = *g_engine;
  e.pop_frame();
  if (!e.extensions.empty()) {
    grcore_deopt_reservation_retract(e.reservation, e.extensions.back());
    e.extensions.pop_back();
    e.st.retracts++;
  }
}

inline uint32_t Engine::h_compile(void *, uint64_t callee) {
  Engine & e = *g_engine;
  e.st.compile_calls++;
  if (e.uncompilable.count(static_cast<int>(callee)) != 0) {
    e.st.compile_refusals++;
    EXPECT_EQ(grcore_entry_slot_refuse(e.ctx, e.slots[callee]), GRCORE_OK);
    return 1;
  }
  if (e.lie_about_installing) {
    return 0;
  }
  return e.compile_fn(static_cast<int>(callee)) ? 0 : 1;
}

inline uint32_t Engine::h_deopt(void *, uint64_t cause) {
  Engine & e = *g_engine;
  e.st.deopts++;
  e.st.last_cause = cause;
  // Where it started: the innermost compiled frame's site. A site at a call
  // instruction is an exit at a call site (a callee that cannot be compiled, a
  // refused push, a bad code pointer), which is not the caller's fault and is
  // not counted against it; a guard's or a poll's is.
  e.st.last_innermost_fn = -1;
  e.st.last_was_call_exit = false;
  {
    GRCORE_CompiledWalk w;
    GRCORE_CompiledFrame f;
    if (grcore_compiled_walk_begin(e.ctx, &w) == GRCORE_OK &&
        grcore_compiled_walk_next(&w, &f) == GRCORE_CWALK_FRAME) {
      int fn = static_cast<int>(f.identity.function);
      size_t pc = static_cast<size_t>(f.identity.offset);
      e.st.last_innermost_fn = fn;
      if (pc < e.funcs[fn].code.size()) {
        K k = e.funcs[fn].code[pc].k;
        e.st.last_was_call_exit =
            k == K::CALL || k == K::CALLP || k == K::TAILCALL || k == K::TAILCALLP;
      }
    }
  }
  size_t n = 0;
  if (e.refuse_rebuild_with != 0) {
    e.st.rebuild = static_cast<GRCORE_Result>(e.refuse_rebuild_with);
    return e.refuse_rebuild_with;
  }
  GRCORE_Result r = grcore_compiled_rebuild(e.ctx, e.reservation, SIZE_MAX, &n);
  e.st.rebuild = r;
  e.st.deopt_frames += static_cast<long>(n);
  return r == GRCORE_OK ? 0u : static_cast<uint32_t>(r);
}

inline uint32_t Engine::h_poll(void *, uint64_t, uint64_t) {
  Engine & e = *g_engine;
  e.st.poll_slow++;
  if (e.torture) {
    e.collect();
  }
  if (e.poll_slow_calls_to_fail > 0 && ++e.poll_count_for_fail == e.poll_slow_calls_to_fail) {
    return 1;
  }
  return 0;
}

inline uint64_t Engine::h_new(uint64_t v) {
  Engine & e = *g_engine;
  e.st.news++;
  if (e.torture) {
    e.collect();
  }
  return e.heap.alloc(static_cast<int64_t>(v));
}

inline uint64_t Engine::h_collect() {
  Engine & e = *g_engine;
  e.st.collects++;
  if (e.on_collect) {
    e.on_collect(e);
  }
  e.collect();
  return 0;
}

inline uint64_t Engine::h_clear(uint64_t fn) {
  Engine & e = *g_engine;
  e.discard(static_cast<int>(fn));
  return 0;
}

inline uint64_t Engine::h_refslot(uint64_t fn) {
  Engine & e = *g_engine;
  EXPECT_EQ(grcore_entry_slot_refuse(e.ctx, e.slots[fn]), GRCORE_OK);
  return 0;
}

inline uint64_t Engine::h_entryof(uint64_t fn) {
  Engine & e = *g_engine;
  return e.C(fn).handle != nullptr ? e.C(fn).internal : 0;
}

__attribute__((noinline)) inline uint64_t Engine::h_probe(uint64_t v) {
  Engine & e = *g_engine;
  e.st.probes++;
  e.st.probed.push_back(v);
  /* This function keeps a frame pointer, so its own frame address is the native
   * stack pointer of its caller, less a constant. (The address of a local would be
   * on ASan's fake stack, which says nothing about the native one.) */
  e.st.sps.push_back(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
  /* This function keeps a frame pointer, so its caller's frame base is the word
   * at its own: the compiled frame that called it. */
  e.st.bases.push_back(*reinterpret_cast<const uintptr_t *>(__builtin_frame_address(0)));
  {
    const uintptr_t * base = *reinterpret_cast<const uintptr_t * const *>(__builtin_frame_address(0));
    e.st.saved.push_back(base[0]);
    e.st.rets.push_back(base[1]);
  }
  e.st.frames.push_back(grcore_stack_frame_count(e.stack));
  e.st.depths.push_back(grcore_context_depth(e.ctx, GRCORE_DEPTH_GUEST));
  e.st.caps.push_back(grcore_deopt_reservation_capacity(e.reservation));
  if (e.on_probe) {
    e.on_probe(e);
  }
  return 0;
}

/* ---- The interpreter -------------------------------------------------------- */

/* Runs the top guest frame, and the frames it calls and returns into, until the
 * frame count falls to `base_frames`. The interpreter never runs as a C callee of
 * compiled code: this is only called with no compiled frame on the native stack. */
inline void Engine::interpret(Outcome & out) {
  struct Restore {
    Heap & h;
    bool was;
    ~Restore() { h.moving = was; }
  } restore{heap, heap.moving};
  if (interpreter_never_moves) {
    heap.moving = false;
  }
  for (;;) {
    size_t count = grcore_stack_frame_count(stack);
    if (count <= base_frames) {
      return;
    }
    GRCORE_FrameRef top = grcore_stack_top(stack);
    GRCORE_PollIdentity id;
    grcore_stack_identity(stack, top, &id);
    int fn = static_cast<int>(id.function);
    const Func & F = funcs[fn];
    uint64_t * S = grcore_stack_slots(stack, top);
    int pc = static_cast<int>(S[0]);
    if (pc >= static_cast<int>(F.code.size())) {
      ADD_FAILURE() << "ran off the end of " << F.name;
      out.frames_left = count;
      return;
    }
    const Ins & in = F.code[pc];
    auto L = [&](int local) { return rd(S, fn, local); };
    switch (in.k) {
      case K::CONST: wr(S, fn, in.d, static_cast<u64>(in.imm)); S[0] = pc + 1; break;
      case K::MOV: wr(S, fn, in.d, L(in.a)); S[0] = pc + 1; break;
      case K::ADD: wr(S, fn, in.d, L(in.a) + L(in.b)); S[0] = pc + 1; break;
      case K::SUB: wr(S, fn, in.d, L(in.a) - L(in.b)); S[0] = pc + 1; break;
      case K::MUL: wr(S, fn, in.d, L(in.a) * L(in.b)); S[0] = pc + 1; break;
      case K::AND: wr(S, fn, in.d, L(in.a) & L(in.b)); S[0] = pc + 1; break;
      case K::LT:
        wr(S, fn, in.d, static_cast<int64_t>(L(in.a)) < static_cast<int64_t>(L(in.b)) ? 1 : 0);
        S[0] = pc + 1;
        break;
      case K::EQ: wr(S, fn, in.d, L(in.a) == L(in.b) ? 1 : 0); S[0] = pc + 1; break;
      case K::BRZ: S[0] = L(in.a) == 0 ? in.t : pc + 1; break;
      case K::BR: S[0] = in.t; break;
      case K::GUARD: case K::POLL: S[0] = pc + 1; break;
      case K::NEW: {
        u64 r = h_new(static_cast<u64>(in.imm));
        S = grcore_stack_slots(stack, top);
        wr(S, fn, in.d, r);
        S[0] = pc + 1;
        break;
      }
      case K::GET: wr(S, fn, in.d, static_cast<u64>(heap.read(L(in.a)))); S[0] = pc + 1; break;
      case K::LOAD: wr(S, fn, in.d, *reinterpret_cast<const u64 *>(L(in.a))); S[0] = pc + 1; break;
      case K::DERIVE: wr(S, fn, in.d, L(in.a) + static_cast<u64>(in.imm)); S[0] = pc + 1; break;
      case K::COLLECT:
        h_collect();
        S = grcore_stack_slots(stack, top);
        S[0] = pc + 1;
        break;
      case K::CLEARSLOT: h_clear(static_cast<u64>(in.fn)); S[0] = pc + 1; break;
      case K::REFSLOT: h_refslot(static_cast<u64>(in.fn)); S[0] = pc + 1; break;
      case K::ENTRYOF: wr(S, fn, in.d, h_entryof(static_cast<u64>(in.fn))); S[0] = pc + 1; break;
      case K::PROBE: st.probes++; st.probed.push_back(L(in.a)); S[0] = pc + 1; break;
      case K::CALL:
      case K::CALLP: {
        // The call is the instruction at pc: the callee's frame is pushed above
        // this one, and the call is completed when the callee returns.
        u64 args[16];
        size_t n = in.args.size();
        for (size_t i = 0; i < n; i++) {
          args[i] = in.args[i] < 0 ? static_cast<u64>(F.imms[static_cast<size_t>(-1 - in.args[i])])
                                  : L(in.args[i]);
        }
        if (!push_frame(in.fn, args, n, false)) {
          out.limit = true;
          out.limit_depth = grcore_stack_frame_count(stack) - base_frames;
          return;
        }
        break;
      }
      case K::TAILCALL:
      case K::TAILCALLP: {
        u64 args[16];
        size_t n = in.args.size();
        for (size_t i = 0; i < n; i++) {
          args[i] = in.args[i] < 0 ? static_cast<u64>(F.imms[static_cast<size_t>(-1 - in.args[i])])
                                  : L(in.args[i]);
        }
        if (!F.owns_scope) {
          // A tail call replaces the frame, at the same guest depth.
          pop_frame();
        }
        // From a frame that owns a scope it is a call, and the callee's result
        // is returned from this frame when it comes back (the RET below).
        if (!push_frame(in.fn, args, n, false)) {
          out.limit = true;
          out.limit_depth = grcore_stack_frame_count(stack) - base_frames;
          return;
        }
        break;
      }
      case K::RET: {
        u64 v = L(in.a);
        for (;;) {
          pop_frame();
          if (grcore_stack_frame_count(stack) <= base_frames) {
            out.finished = true;
            out.value = v;
            return;
          }
          GRCORE_FrameRef caller = grcore_stack_top(stack);
          GRCORE_PollIdentity cid;
          grcore_stack_identity(stack, caller, &cid);
          uint64_t * CS = grcore_stack_slots(stack, caller);
          int cpc = static_cast<int>(CS[0]);
          const Ins & CI = funcs[cid.function].code[cpc];
          if ((CI.k == K::TAILCALL || CI.k == K::TAILCALLP) && funcs[cid.function].owns_scope) {
            continue; // the caller made its tail call as a call: it returns this
          }
          if (CI.k != K::CALL && CI.k != K::CALLP) {
            // The caller was waiting at a call when the callee returned into the
            // interpreter, so its frame must have been rebuilt to that call.
            ADD_FAILURE() << "a caller's frame was not rebuilt: " << funcs[cid.function].name
                          << " is at pc " << cpc;
            out.failed = true;
            return;
          }
          wr(CS, static_cast<int>(cid.function), CI.d, v);
          CS[0] = cpc + 1;
          break;
        }
        break;
      }
    }
  }
}

inline Outcome Engine::run_interpreted(int fn, const std::vector<u64> & args) {
  Outcome out;
  base_frames = grcore_stack_frame_count(stack);
  if (!push_frame(fn, args.data(), args.size(), false)) {
    out.limit = true;
    return out;
  }
  interpret(out);
  if (out.limit) {
    // The budget refused a call: unwind to where the run began.
    while (grcore_stack_frame_count(stack) > base_frames) {
      grcore_stack_pop(stack);
    }
  }
  out.frames_left = grcore_stack_frame_count(stack) - base_frames;
  return out;
}

/* Enters compiled code the way an engine's interpreter does: the guest frame is
 * pushed, a JIT record is entered, and the adapter is called with the arguments
 * from the frame. A deoptimization leaves guest frames the interpreter finishes. */
inline Outcome Engine::run_compiled(int fn, const std::vector<u64> & args) {
  Outcome out;
  base_frames = grcore_stack_frame_count(stack);
  if (!compile_fn(fn)) {
    return out;
  }
  if (!push_frame(fn, args.data(), args.size(), false)) {
    out.limit = true;
    return out;
  }
  // The reservation is taken when compiled code is entered, for the entry
  // function itself; each call extends it for its callee (AD-27, AD-28).
  EXPECT_EQ(grcore_deopt_reservation_extend(ctx, reservation, C(fn).max_converting),
      GRCORE_OK);
  extensions.push_back(C(fn).max_converting);
  grcore_context_native_limit_here(ctx);
  last_native_limit = grcore_context_native_limit(ctx);
  GRCORE_ActivationRef rec;
  EXPECT_EQ(grcore_activation_enter(stack, GRCORE_ACTIVATION_JIT, engine, false, nullptr, &rec),
      GRCORE_OK);
  const GRJIT_Code * code = C(fn).code;
  std::vector<u64> in(std::max<size_t>(args.size(), grjit_code_param_count(code)), 0);
  for (size_t i = 0; i < args.size(); i++) {
    in[i] = args[i];
  }
  std::vector<u64> res(grjit_code_out_words(code), 0xDEADBEEFu);
  uint32_t exit = grjit_code_call(code, ctx, in.data(), res.data());
  out.exit = exit;
  GRCORE_Result left = grcore_activation_leave(stack, rec);
  if (!interpreter_cannot_recover && exit != GRJIT_EXIT_REBUILD_FAILED) {
    EXPECT_EQ(left, GRCORE_OK) << "frames: " << grcore_stack_frame_count(stack)
                               << " base: " << base_frames + 1;
  }
  if (exit == GRJIT_EXIT_RETURNED) {
    out.finished = true;
    out.value = res[0];
    reset_reservation();
    pop_frame();
    out.frames_left = grcore_stack_frame_count(stack) - base_frames;
    return out;
  }
  if (exit == GRJIT_EXIT_REBUILD_FAILED) {
    // The engine's contract: nothing was rebuilt, so unwind what the run pushed
    // and report an internal error; never continue in the interpreter.
    out.rebuild_failed = true;
    out.failed_with = res[0];
    for (size_t k = base_frames; k < grcore_stack_frame_count(stack); k++) {
      GRCORE_FrameRef fr = grcore_stack_top(stack);
      for (size_t up = grcore_stack_frame_count(stack) - 1; up > k; up--) {
        fr = grcore_stack_caller(stack, fr);
      }
      out.pcs_after_failure.push_back(grcore_stack_slots(stack, fr)[0]);
    }
    reset_reservation();
    grcore_unwind_all(stack, nullptr);
    return out;
  }
  EXPECT_EQ(exit, GRJIT_EXIT_DEOPT);
  reset_reservation();
  if (st.last_innermost_fn >= 0 && !st.last_was_call_exit &&
      ++st.counted[st.last_innermost_fn] >= discard_limit) {
    // Deoptimized too often to be worth having: the code is let go (the frames
    // that were in it have returned) and the function is not compiled again by
    // this fixture's policy.
    discard(st.last_innermost_fn);
    never_compile.insert(st.last_innermost_fn);
  }
  out.interpreted_rest = true;
  if (interpreter_cannot_recover) {
    // The rebuild was refused, so the frames are as the compiled calls left them:
    // nothing sound can be finished from them.
    grcore_unwind_all(stack, nullptr);
    return out;
  }
  interpret(out);
  if (out.limit) {
    while (grcore_stack_frame_count(stack) > base_frames) {
      grcore_stack_pop(stack);
    }
  }
  out.frames_left = grcore_stack_frame_count(stack) - base_frames;
  return out;
}

} // namespace fx

#endif
