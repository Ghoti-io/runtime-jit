/**
 * @file
 *
 * Polls and the entry hook: the fast path costs a load, a test and a branch
 * and calls nothing; a pending request takes the slow path with the poll's
 * identity; a non-zero answer from the helper or the hook ends the function
 * as REFUSED.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../ir_eval.h"

namespace {

struct PollLog {
  int calls = 0;
  uint64_t function = 0, offset = 0;
  void * ctx = nullptr;
  uint32_t answer = 0;
  bool clear = false;
  GRCORE_RequestKind kind = 0;
};
PollLog g_poll;
int g_hook_calls = 0;
uint32_t g_hook_answer = 0;
int g_body_calls = 0;

uint32_t poll_helper(void * ctx, uint64_t fn, uint64_t off) {
  g_poll.calls++;
  g_poll.function = fn;
  g_poll.offset = off;
  g_poll.ctx = ctx;
  if (g_poll.clear) {
    EXPECT_EQ(grcore_context_clear_request(static_cast<GRCORE_Context *>(ctx), g_poll.kind),
        GRCORE_OK);
  }
  return g_poll.answer;
}

uint32_t entry_hook(void *) {
  g_hook_calls++;
  return g_hook_answer;
}

uint64_t body_helper() {
  g_body_calls++;
  return 0;
}

const GRCORE_Key kKey = {"poll", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_NONE, nullptr, nullptr};

/* sum(n): a counting loop with a poll on every iteration. */
GRJIT_Function * loop_with_poll() {
  B b("pollsum", 1);
  GRJIT_VReg n = b.param(GRJIT_TYPE_I64);
  GRJIT_VReg i = b.reg(), sum = b.reg(), t = b.reg();
  GRJIT_BlockId entry = b.block(), head = b.block(), body = b.block(), done = b.block();
  b.poll_helper(poll_helper);
  b.at(entry);
  b.cnst(i, 0);
  b.cnst(sum, 0);
  b.br(head);
  b.at(head);
  b.cmp(GRJIT_CMP_LT, t, V(i), V(n));
  b.br_if(V(t), body, done);
  b.at(body);
  b.poll({7, 3}, {grjit_frame_slot_vreg(i)});
  b.bin(GRJIT_OP_ADD, sum, V(sum), V(i));
  b.bin(GRJIT_OP_ADD, i, V(i), I(1));
  b.br(head);
  b.at(done);
  b.ret(V(sum));
  return b.finish();
}

struct PostWorld : JitWorld {
  GRCORE_RequestKind kind = 0;
  GRCORE_Port * port = nullptr;
  PostWorld() {
    EXPECT_EQ(grcore_context_request_kind(ctx, &kKey, &kind), GRCORE_OK);
    EXPECT_EQ(grcore_context_port(ctx, &port), GRCORE_OK);
    g_poll = PollLog();
    g_poll.kind = kind;
  }
  ~PostWorld() { grcore_port_release(port); }
  void post() { EXPECT_EQ(grcore_port_post(port, kind), GRCORE_OK); }
};

} // namespace

TEST(Poll, WithNothingPendingTheSlowPathIsNeverCalledAndTheResultIsRight) {
  GRJIT_REQUIRE_BACKEND();
  PostWorld w;
  Fn f(loop_with_poll());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c) << grjit_result_string(c.result);
  auto r = c.run(w.ctx, {1000});
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(r.out[0], 499500u);
  EXPECT_EQ(g_poll.calls, 0);
}

TEST(Poll, APendingRequestTakesTheSlowPathOncePerPollWithTheIdentity) {
  GRJIT_REQUIRE_BACKEND();
  PostWorld w;
  Fn f(loop_with_poll());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  w.post(); // left posted: every iteration's poll sees it
  auto r = c.run(w.ctx, {10});
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(r.out[0], 45u); // the same result as without the request
  EXPECT_EQ(g_poll.calls, 10);
  EXPECT_EQ(g_poll.function, 7u);
  EXPECT_EQ(g_poll.offset, 3u);
  EXPECT_EQ(g_poll.ctx, static_cast<void *>(w.ctx));
}

TEST(Poll, AHelperThatServesTheRequestIsCalledOnce) {
  GRJIT_REQUIRE_BACKEND();
  PostWorld w;
  Fn f(loop_with_poll());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  g_poll.clear = true;
  w.post();
  auto r = c.run(w.ctx, {10});
  EXPECT_EQ(r.out[0], 45u);
  EXPECT_EQ(g_poll.calls, 1);
}

TEST(Poll, ANonZeroAnswerEndsTheFunctionAsRefusedAndNothingAfterItRuns) {
  GRJIT_REQUIRE_BACKEND();
  PostWorld w;
  B b("refuse", 0);
  b.poll_helper(poll_helper);
  b.at(b.block());
  b.poll({1, 1});
  b.call(GRJIT_NO_VREG, reinterpret_cast<const void *>(body_helper));
  b.ret(I(5));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  g_poll.answer = 7;
  g_body_calls = 0;
  w.post();
  auto r = c.run(w.ctx);
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(r.out[0], 7u);
  EXPECT_EQ(g_body_calls, 0);
  // The evaluator agrees.
  ireval::Result e = ireval::eval(f, w.ctx, nullptr, {});
  EXPECT_EQ(e.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(e.out[0], 7u);
}

TEST(Poll, ARefusalValueKeepsAllThirtyTwoBitsOfTheHelpersAnswer) {
  GRJIT_REQUIRE_BACKEND();
  PostWorld w;
  B b("refuse", 0);
  b.poll_helper(poll_helper);
  b.at(b.block());
  b.poll({1, 1});
  b.ret(I(5));
  Fn f(b.finish());
  Compiled c(f, w.pages());
  ASSERT_TRUE(c);
  g_poll.answer = 0x80000001u;
  w.post();
  auto r = c.run(w.ctx);
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(r.out[0], 0x80000001u);
}

TEST(EntryHook, AHookThatReturnsZeroLetsTheFunctionRun) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("hook", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I64);
  b.at(b.block());
  b.ret(V(x));
  Fn f(b.finish());
  Compiled c(f, w.pages(), entry_hook);
  ASSERT_TRUE(c);
  g_hook_calls = 0;
  g_hook_answer = 0;
  auto r = c.run(w.ctx, {123});
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(r.out[0], 123u);
  EXPECT_EQ(g_hook_calls, 1);
  c.run(w.ctx, {1});
  EXPECT_EQ(g_hook_calls, 2); // once per call
}

TEST(EntryHook, AHookThatRefusesStopsTheFunctionBeforeItsBody) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  B b("hook", 0);
  b.at(b.block());
  b.call(GRJIT_NO_VREG, reinterpret_cast<const void *>(body_helper));
  b.ret(I(1));
  Fn f(b.finish());
  Compiled c(f, w.pages(), entry_hook);
  ASSERT_TRUE(c);
  g_hook_calls = 0;
  g_hook_answer = 9;
  g_body_calls = 0;
  auto r = c.run(w.ctx);
  EXPECT_EQ(r.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(r.out[0], 9u);
  EXPECT_EQ(g_body_calls, 0);
  EXPECT_EQ(g_hook_calls, 1);
  ireval::Result e = ireval::eval(f, w.ctx, entry_hook, {});
  EXPECT_EQ(e.exit, static_cast<uint32_t>(GRJIT_EXIT_REFUSED));
  EXPECT_EQ(e.out[0], 9u);
  // With the hook gone the body runs.
  Compiled plain(f, w.pages());
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain.run(w.ctx).exit, static_cast<uint32_t>(GRJIT_EXIT_RETURNED));
  EXPECT_EQ(g_body_calls, 1);
}

TEST(EntryHook, TheHookRunsBeforeTheParametersAreReadAndGetsTheContext) {
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  static void * seen;
  seen = nullptr;
  auto hook = [](void * ctx) -> uint32_t {
    seen = ctx;
    return 0;
  };
  B b("hook", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_PTR);
  GRJIT_VReg v = b.reg();
  b.at(b.block());
  b.load(v, x, 0, 64);
  b.ret(V(v));
  Fn f(b.finish());
  Compiled c(f, w.pages(), hook);
  ASSERT_TRUE(c);
  uint64_t word = 0xABCDEF;
  auto r = c.run(w.ctx, {reinterpret_cast<uint64_t>(&word)});
  EXPECT_EQ(r.out[0], 0xABCDEFu);
  EXPECT_EQ(seen, static_cast<void *>(w.ctx));
}

GRJIT_TEST_MAIN()
