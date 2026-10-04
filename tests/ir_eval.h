/**
 * @file
 *
 * A test-only evaluator of the IR (AD-9: the library never interprets it).
 *
 * It exists to be wrong in a different way from the backend: it reads the
 * operations from the function and does in C++ what each is documented to do
 * in `ir.h`, with vregs as 64-bit words, memory as real memory, calls to the
 * same helpers by address, and a guard failure as an exit with the same
 * reconstructed slots. The differential test compares the two.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRJIT_TESTS_IR_EVAL_H
#define GHOTI_IO_GRJIT_TESTS_IR_EVAL_H

#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/a/layout.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace ireval {

struct Result {
  uint32_t exit = 99;
  std::vector<uint64_t> out;
  GRCORE_PollIdentity guard_identity{0, 0};
  bool timed_out = false;
};

inline uint64_t call_native(uint64_t address, const uint64_t * a, size_t n) {
  auto fn = reinterpret_cast<void *>(address);
  switch (n) {
    case 0: return reinterpret_cast<uint64_t (*)()>(fn)();
    case 1: return reinterpret_cast<uint64_t (*)(uint64_t)>(fn)(a[0]);
    case 2: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(fn)(a[0], a[1]);
    case 3: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2]);
    case 4: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2], a[3]);
    case 5: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2], a[3], a[4]);
    default: return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
  }
}

inline uint64_t operand(const std::vector<uint64_t> & regs, const GRJIT_Operand & o) {
  return o.kind == GRJIT_OPERAND_VREG ? regs[o.vreg] : static_cast<uint64_t>(o.imm);
}

inline bool compare(GRJIT_Cmp c, uint64_t a, uint64_t b) {
  int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
  switch (c) {
    case GRJIT_CMP_EQ: return a == b;
    case GRJIT_CMP_NE: return a != b;
    case GRJIT_CMP_LT: return sa < sb;
    case GRJIT_CMP_LE: return sa <= sb;
    case GRJIT_CMP_GT: return sa > sb;
    case GRJIT_CMP_GE: return sa >= sb;
    case GRJIT_CMP_ULT: return a < b;
    case GRJIT_CMP_ULE: return a <= b;
    case GRJIT_CMP_UGT: return a > b;
    default: return a >= b;
  }
}

inline void deopt_slots(const GRJIT_Function * f, const GRJIT_Op & op,
    const std::vector<uint64_t> & regs, Result * r) {
  const GRJIT_FrameState * s = grjit_function_frame_state(f, op.state);
  size_t n = grjit_function_interp_slot_count(f);
  r->out.assign(n + 1, 0);
  for (size_t i = 0; i < n; i++) {
    const GRJIT_FrameSlot & slot = s->slots[i];
    r->out[i] = slot.kind == GRJIT_FRAME_SLOT_VREG ? regs[slot.vreg]
              : slot.kind == GRJIT_FRAME_SLOT_CONSTANT ? static_cast<uint64_t>(slot.constant)
                                                       : 0;
  }
  r->guard_identity = s->identity;
  r->exit = GRJIT_EXIT_DEOPT;
}

/* Runs `f` on `args`. `ctx` is the context compiled polls load through. */
inline Result eval(const GRJIT_Function * f, void * ctx, GRJIT_EntryHook hook,
    const std::vector<uint64_t> & args, size_t step_limit = 2000000) {
  Result r;
  size_t slots = grjit_function_interp_slot_count(f);
  r.out.assign(slots + 1, 0xDEADBEEFu);
  if (hook != nullptr) {
    uint32_t refused = hook(ctx);
    if (refused != 0) {
      r.out[0] = refused;
      r.exit = GRJIT_EXIT_REFUSED;
      return r;
    }
  }
  std::vector<uint64_t> regs(grjit_function_vreg_count(f), 0);
  for (size_t i = 0; i < grjit_function_param_count(f); i++) {
    regs[i] = i < args.size() ? args[i] : 0;
  }
  GRJIT_BlockId block = 0;
  size_t steps = 0;
  for (;;) {
    size_t n = 0;
    const GRJIT_Op * ops = grjit_function_block_ops(f, block, &n);
    GRJIT_BlockId next = block;
    bool jumped = false;
    for (size_t i = 0; i < n && !jumped; i++) {
      const GRJIT_Op & op = ops[i];
      if (++steps > step_limit) {
        r.timed_out = true;
        return r;
      }
      uint64_t a = op.a.kind == GRJIT_OPERAND_NONE ? 0 : operand(regs, op.a);
      uint64_t b = op.b.kind == GRJIT_OPERAND_NONE ? 0 : operand(regs, op.b);
      switch (op.kind) {
        case GRJIT_OP_CONST: regs[op.dst] = a; break;
        case GRJIT_OP_MOVE: regs[op.dst] = a; break;
        case GRJIT_OP_BITCAST: regs[op.dst] = a; break;
        case GRJIT_OP_ADD: regs[op.dst] = a + b; break;
        case GRJIT_OP_SUB: regs[op.dst] = a - b; break;
        case GRJIT_OP_MUL: regs[op.dst] = a * b; break;
        case GRJIT_OP_AND: regs[op.dst] = a & b; break;
        case GRJIT_OP_OR: regs[op.dst] = a | b; break;
        case GRJIT_OP_XOR: regs[op.dst] = a ^ b; break;
        case GRJIT_OP_SHL: regs[op.dst] = a << (b & 63); break;
        case GRJIT_OP_SHR: regs[op.dst] = a >> (b & 63); break;
        case GRJIT_OP_SAR:
          regs[op.dst] = static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63));
          break;
        case GRJIT_OP_NEG: regs[op.dst] = 0 - a; break;
        case GRJIT_OP_NOT: regs[op.dst] = ~a; break;
        case GRJIT_OP_CMP: regs[op.dst] = compare(op.cmp, a, b) ? 1 : 0; break;
        case GRJIT_OP_LOAD:
        case GRJIT_OP_LOAD_S: {
          uint64_t v = 0;
          std::memcpy(&v, reinterpret_cast<const unsigned char *>(a) + op.disp, op.width / 8);
          if (op.kind == GRJIT_OP_LOAD_S && op.width < 64 && ((v >> (op.width - 1)) & 1)) {
            v |= ~uint64_t{0} << op.width;
          }
          regs[op.dst] = v;
          break;
        }
        case GRJIT_OP_STORE:
          std::memcpy(reinterpret_cast<unsigned char *>(a) + op.disp, &b, op.width / 8);
          break;
        case GRJIT_OP_CALL: {
          uint64_t argv[6] = {0, 0, 0, 0, 0, 0};
          for (size_t k = 0; k < op.arg_count; k++) {
            argv[k] = operand(regs, op.args[k]);
          }
          uint64_t v = call_native(op.address, argv, op.arg_count);
          if (op.dst != GRJIT_NO_VREG) {
            regs[op.dst] = v;
          }
          break;
        }
        case GRJIT_OP_POLL: {
          uint64_t word;
          std::memcpy(&word,
              reinterpret_cast<const unsigned char *>(ctx) + grcore_jit_layout()->request_word_offset, 8);
          if (word != 0) {
            const GRJIT_FrameState * s = grjit_function_frame_state(f, op.state);
            uint32_t refused = grjit_function_poll_helper(f)(ctx, s->identity.function, s->identity.offset);
            if (refused != 0) {
              r.out.assign(slots + 1, 0xDEADBEEFu);
              r.out[0] = refused;
              r.exit = GRJIT_EXIT_REFUSED;
              return r;
            }
          }
          break;
        }
        case GRJIT_OP_GUARD:
          if (a == 0) {
            deopt_slots(f, op, regs, &r);
            return r;
          }
          break;
        case GRJIT_OP_BR: next = op.target; jumped = true; break;
        case GRJIT_OP_BR_IF: next = a != 0 ? op.target : op.target_else; jumped = true; break;
        case GRJIT_OP_RET:
          if (op.a.kind != GRJIT_OPERAND_NONE) {
            r.out[0] = a;
          }
          r.exit = GRJIT_EXIT_RETURNED;
          return r;
        default:
          return r;
      }
    }
    if (!jumped) {
      return r; // fell off a block: not a verified function
    }
    block = next;
  }
}

} // namespace ireval

#endif
