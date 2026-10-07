/**
 * @file
 *
 * The callable functions the code-pinning test and the structural test of the arm64 convention
 * share: one function for every combination of a parameter count, an argument count and a form,
 * in three families, each emitted and never run (the hooks, the slots, the natives and the poll
 * helper are made-up addresses, so the bytes depend on nothing about the process):
 *
 *  - calls to other compiled functions and no tail call (`each_call_function`): through a slot,
 *    through a code pointer in a register and through one as an immediate, each in two shapes, a
 *    call and a return, and a guard, a poll, a derived pointer as an argument and a branch to a
 *    second return;
 *  - one tail call to each form of callee (`each_tail_function`);
 *  - calls to registered natives (`each_native_function`): every combination of a parameter
 *    count, the native's arity, its status and its result type, in two shapes.
 *
 * The parameters rotate through I64, REF and PTR. Each visitor is called with the verified
 * function and its id, and stops the family when it returns false.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_CALLABLE_GEN_H
#define GHOTI_IO_GRJIT_TESTS_CALLABLE_GEN_H

#include "test_helpers.h"

namespace cg {

inline const GRJIT_PollHelper kPollHelper = reinterpret_cast<GRJIT_PollHelper>(0x20000);

constexpr unsigned kParams[] = {0, 1, 2, 3, 6, 7, 9, 16};
constexpr unsigned kArgs[] = {0, 1, 4, 6, 7, 10, 16};
constexpr unsigned kNativeParams[] = {0, 1, 3, 7};
constexpr unsigned kNativeArgs[] = {0, 1, 4, 5, 6, 7, 10, 16};

/* Calls to other compiled functions, no tail call: 336 functions. */
template <class F>
void each_call_function(F && visit) {
  unsigned id = 0;
  GRJIT_CallHooks hooks{};
  hooks.push = reinterpret_cast<decltype(hooks.push)>(0x30000);
  hooks.pop = reinterpret_cast<decltype(hooks.pop)>(0x30100);
  hooks.compile = reinterpret_cast<decltype(hooks.compile)>(0x30200);
  hooks.deopt = reinterpret_cast<decltype(hooks.deopt)>(0x30300);
  for (unsigned params : kParams) {
    for (unsigned args : kArgs) {
      for (unsigned form = 0; form < 3; form++) {
        for (unsigned variant = 0; variant < 2; variant++, id++) {
          B b("pin", 3);
          b.callable(hooks);
          EXPECT_EQ(grjit_builder_set_token(b.b, 1000 + id), GRJIT_OK);
          b.poll_helper(kPollHelper);
          const GRJIT_Type rotate[3] = {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_PTR};
          std::vector<GRJIT_VReg> p;
          for (unsigned i = 0; i < params; i++) {
            p.push_back(b.param(rotate[i % 3]));
          }
          GRJIT_VReg r = b.reg(variant == 0 && id % 4 == 2 ? GRJIT_TYPE_REF : GRJIT_TYPE_I64);
          GRJIT_VReg d = GRJIT_NO_VREG;
          if (variant == 1 && params >= 2) {
            d = b.reg(GRJIT_TYPE_PTR);
            b.derived(d, p[1], 24);
          }
          GRJIT_BlockId b0 = b.block();
          GRJIT_BlockId b1 = variant == 1 ? b.block() : 0;
          GRJIT_BlockId b2 = variant == 1 ? b.block() : 0;
          b.at(b0);
          const GRCORE_PollIdentity at{1, id};
          std::vector<GRJIT_FrameSlot> state = {grjit_frame_slot_constant(id),
              params > 0 ? grjit_frame_slot_vreg(p[0]) : grjit_frame_slot_dead(),
              params > 1 ? grjit_frame_slot_vreg(p[1]) : grjit_frame_slot_dead()};
          if (variant == 1) {
            if (params > 0) {
              b.guard(V(p[0]), at, state);
            }
            b.poll(at, state);
            if (d != GRJIT_NO_VREG) {
              b.bitcast(d, p[1]);
              b.bin(GRJIT_OP_ADD, d, V(d), I(24));
            }
          }
          std::vector<GRJIT_Operand> ops;
          for (unsigned i = 0; i < args; i++) {
            if (i == 0 && d != GRJIT_NO_VREG) {
              ops.push_back(V(d));
            } else if (i < params) {
              ops.push_back(V(p[i]));
            } else {
              ops.push_back(I(static_cast<int64_t>(i) * 3 + 1));
            }
          }
          if (form == 0) {
            b.call_slot(r, reinterpret_cast<const void *>(0x40000 + 8 * id), 500 + id, ops, at,
                state, at, state);
          } else if (form == 1 && params >= 3) {
            b.call_ptr(r, V(p[2]), 500 + id, ops, at, state, at, state);
          } else {
            b.call_ptr(r, I(0x50000 + 16 * id), 500 + id, ops, at, state, at, state);
          }
          if (variant == 1) {
            b.br_if(V(r), b1, b2);
            b.at(b1);
            b.ret(V(r));
            b.at(b2);
            b.ret(I(0));
          } else {
            b.ret(V(r));
          }
          Fn f(b.finish());
          char why[256];
          EXPECT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << id << why;
          if (!visit(static_cast<const GRJIT_Function *>(f), id)) {
            return;
          }
        }
      }
    }
  }
}

/* One tail call, in each form: 168 functions. */
template <class F>
void each_tail_function(F && visit) {
  unsigned id = 0;
  GRJIT_CallHooks hooks{};
  hooks.push = reinterpret_cast<decltype(hooks.push)>(0x30000);
  hooks.pop = reinterpret_cast<decltype(hooks.pop)>(0x30100);
  hooks.compile = reinterpret_cast<decltype(hooks.compile)>(0x30200);
  hooks.deopt = reinterpret_cast<decltype(hooks.deopt)>(0x30300);
  hooks.tail = reinterpret_cast<decltype(hooks.tail)>(0x30400);
  for (unsigned params : kParams) {
    for (unsigned args : kArgs) {
      for (unsigned form = 0; form < 3; form++, id++) {
        B b("pin", 3);
        b.callable(hooks);
        EXPECT_EQ(grjit_builder_set_token(b.b, 3000 + id), GRJIT_OK);
        b.poll_helper(kPollHelper);
        const GRJIT_Type rotate[3] = {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_PTR};
        std::vector<GRJIT_VReg> p;
        for (unsigned i = 0; i < params; i++) {
          p.push_back(b.param(rotate[i % 3]));
        }
        b.at(b.block());
        const GRCORE_PollIdentity at{3, id};
        std::vector<GRJIT_FrameSlot> state = {grjit_frame_slot_constant(id),
            params > 0 ? grjit_frame_slot_vreg(p[0]) : grjit_frame_slot_dead(),
            params > 1 ? grjit_frame_slot_vreg(p[1]) : grjit_frame_slot_dead()};
        std::vector<GRJIT_Operand> ops;
        for (unsigned i = 0; i < args; i++) {
          ops.push_back(i < params ? V(p[i]) : I(static_cast<int64_t>(i) * 3 + 1));
        }
        if (form == 0) {
          b.tail_call_slot(reinterpret_cast<const void *>(0x40000 + 8 * id), 700 + id, ops, at, state);
        } else if (form == 1 && params >= 3) {
          b.tail_call_ptr(V(p[2]), 700 + id, ops, at, state);
        } else {
          b.tail_call_ptr(I(0x50000 + 16 * id), 700 + id, ops, at, state);
        }
        Fn f(b.finish());
        char why[256];
        EXPECT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << id << why;
        if (!visit(static_cast<const GRJIT_Function *>(f), id)) {
          return;
        }
      }
    }
  }
}

/* Calls to registered natives: 512 functions. */
template <class F>
void each_native_function(F && visit) {
  unsigned id = 0;
  GRJIT_CallHooks hooks{};
  hooks.push = reinterpret_cast<decltype(hooks.push)>(0x30000);
  hooks.pop = reinterpret_cast<decltype(hooks.pop)>(0x30100);
  hooks.compile = reinterpret_cast<decltype(hooks.compile)>(0x30200);
  hooks.deopt = reinterpret_cast<decltype(hooks.deopt)>(0x30300);
  for (unsigned params : kNativeParams) {
    for (unsigned args : kNativeArgs) {
      for (unsigned status = 0; status < 2; status++) {
        for (unsigned result = 0; result < 4; result++) { // none, I64, REF, PTR
          for (unsigned variant = 0; variant < 2; variant++, id++) {
            const GRJIT_Type rotate[3] = {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_PTR};
            std::vector<GRJIT_Type> types;
            for (unsigned i = 0; i < args; i++) {
              types.push_back(rotate[(i + id) % 3]);
            }
            NativeTab t;
            GRJIT_Type rt = result == 0 ? GRJIT_NATIVE_NO_RESULT : rotate[result - 1];
            uint32_t nid = t.add(reinterpret_cast<const void *>(0x60000 + 16 * static_cast<uintptr_t>(id)), types, rt,
                status != 0 ? GRJIT_NATIVE_STATUS : 0, (id % 5) * 100);
            B b("pin", 4);
            b.callable(hooks);
            b.natives(t);
            EXPECT_EQ(grjit_builder_set_token(b.b, 2000 + id), GRJIT_OK);
            b.poll_helper(kPollHelper);
            std::vector<GRJIT_VReg> p;
            for (unsigned i = 0; i < params; i++) {
              p.push_back(b.param(rotate[i % 3]));
            }
            GRJIT_VReg dst = rt == GRJIT_NATIVE_NO_RESULT ? GRJIT_NO_VREG : b.reg(rt);
            GRJIT_VReg flag = b.reg(GRJIT_TYPE_I64);
            GRJIT_VReg d = GRJIT_NO_VREG;
            if (variant == 1 && params >= 2) {
              d = b.reg(GRJIT_TYPE_PTR);
              b.derived(d, p[1], 24);
            }
            GRJIT_BlockId b0 = b.block();
            GRJIT_BlockId b1 = variant == 1 ? b.block() : 0;
            GRJIT_BlockId b2 = variant == 1 ? b.block() : 0;
            b.at(b0);
            b.cnst(flag, 1);
            const GRCORE_PollIdentity at{2, id};
            const GRCORE_PollIdentity after{2, id + 1};
            std::vector<GRJIT_FrameSlot> state = {grjit_frame_slot_constant(id),
                params > 0 ? grjit_frame_slot_vreg(p[0]) : grjit_frame_slot_dead(),
                params > 1 ? grjit_frame_slot_vreg(p[1]) : grjit_frame_slot_dead(),
                dst != GRJIT_NO_VREG && dst < params ? grjit_frame_slot_vreg(dst) : grjit_frame_slot_dead()};
            std::vector<GRJIT_FrameSlot> after_state = state;
            after_state[0] = grjit_frame_slot_constant(id + 1);
            if (dst != GRJIT_NO_VREG) {
              after_state[3] = grjit_frame_slot_vreg(dst);
            }
            if (variant == 1) {
              if (params > 0) {
                b.guard(V(flag), at, state);
              }
              b.poll(at, state);
              if (d != GRJIT_NO_VREG) {
                b.bitcast(d, p[1]);
                b.bin(GRJIT_OP_ADD, d, V(d), I(24));
              }
            }
            std::vector<GRJIT_Operand> ops;
            for (unsigned i = 0; i < args; i++) {
              // A register of the right type if there is one, or an immediate.
              GRJIT_Operand o = I(static_cast<int64_t>(i) * 5 + 2);
              for (unsigned q = 0; q < params; q++) {
                if (rotate[q % 3] == types[i] && (q + i) % 2 == 0) {
                  o = V(p[q]);
                  break;
                }
              }
              if (i == 0 && d != GRJIT_NO_VREG && types[i] == GRJIT_TYPE_PTR) {
                o = V(d);
              }
              ops.push_back(o);
            }
            if (status != 0) {
              b.call_native(dst, nid, ops, at, state, after, after_state);
            } else {
              b.call_native(dst, nid, ops, at, state);
            }
            if (variant == 1) {
              b.br_if(V(flag), b1, b2);
              b.at(b1);
              b.ret(dst != GRJIT_NO_VREG ? V(dst) : I(1));
              b.at(b2);
              b.ret(I(0));
            } else {
              b.ret(dst != GRJIT_NO_VREG ? V(dst) : I(1));
            }
            Fn f(b.finish());
            char why[256];
            EXPECT_EQ(grjit_function_verify(f, nullptr, why, sizeof why), GRJIT_OK) << id << why;
            if (!visit(static_cast<const GRJIT_Function *>(f), id)) {
              return;
            }
          }
        }
      }
    }
  }
}

} // namespace cg

#endif
