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

/* The exit the evaluator reports for a partial operation reached with operands its guards
 * should have kept out: no compiled run has it. */
constexpr uint32_t kPartialFault = 0xBAD0;

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

/* ---- The integer operations (numeric-operations.md), written to be wrong differently from the
 * backend: no builtins, no instruction a compiler would turn into the backend's own, and the
 * width carried in a plain number. An i32 is the low 32 bits of a word and every result of
 * width 32 is cut to them, so the invariant the backend keeps (zero-extended) is stated here
 * by construction. */

inline uint64_t cut(unsigned width, uint64_t v) { return width == 32 ? (v & 0xFFFFFFFFu) : v; }

inline int64_t sext(unsigned width, uint64_t v) {
  if (width == 32) {
    uint64_t low = v & 0xFFFFFFFFu;
    return static_cast<int64_t>(low >= 0x80000000u ? low | 0xFFFFFFFF00000000u : low);
  }
  return static_cast<int64_t>(v);
}

inline uint64_t count_leading_zeros(unsigned width, uint64_t v) {
  v = cut(width, v);
  uint64_t n = 0;
  for (int bit = static_cast<int>(width) - 1; bit >= 0 && ((v >> bit) & 1) == 0; bit--) {
    n++;
  }
  return n;
}

inline uint64_t count_trailing_zeros(unsigned width, uint64_t v) {
  v = cut(width, v);
  uint64_t n = 0;
  for (unsigned bit = 0; bit < width && ((v >> bit) & 1) == 0; bit++) {
    n++;
  }
  return n;
}

inline uint64_t population(unsigned width, uint64_t v) {
  uint64_t n = 0;
  for (unsigned bit = 0; bit < width; bit++) {
    n += (v >> bit) & 1;
  }
  return n;
}

inline uint64_t rotate_left(unsigned width, uint64_t v, uint64_t count) {
  v = cut(width, v);
  unsigned k = static_cast<unsigned>(count % width);
  if (k == 0) {
    return v;
  }
  return cut(width, (v << k) | (v >> (width - k)));
}

inline uint64_t sign_extend_from(unsigned from, unsigned width, uint64_t v) {
  uint64_t low = from == 64 ? v : (v & ((uint64_t{1} << from) - 1));
  if ((low >> (from - 1)) & 1) {
    low |= from == 64 ? 0 : ~uint64_t{0} << from;
  }
  return cut(width, low);
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
  std::vector<unsigned> width(regs.size(), 64);
  for (size_t v = 0; v < regs.size(); v++) {
    width[v] = grjit_function_vreg_type(f, static_cast<GRJIT_VReg>(v)) == GRJIT_TYPE_I32 ? 32 : 64;
  }
  for (size_t i = 0; i < grjit_function_param_count(f); i++) {
    regs[i] = cut(width[i], i < args.size() ? args[i] : 0);
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
      const unsigned w = op.dst != GRJIT_NO_VREG ? width[op.dst] : 64;
      switch (op.kind) {
        case GRJIT_OP_CONST: regs[op.dst] = cut(w, a); break;
        case GRJIT_OP_MOVE: regs[op.dst] = cut(w, a); break;
        case GRJIT_OP_BITCAST: regs[op.dst] = a; break;
        case GRJIT_OP_ADD: regs[op.dst] = cut(w, a + b); break;
        case GRJIT_OP_SUB: regs[op.dst] = cut(w, a - b); break;
        case GRJIT_OP_MUL: regs[op.dst] = cut(w, a * b); break;
        case GRJIT_OP_AND: regs[op.dst] = cut(w, a & b); break;
        case GRJIT_OP_OR: regs[op.dst] = cut(w, a | b); break;
        case GRJIT_OP_XOR: regs[op.dst] = cut(w, a ^ b); break;
        case GRJIT_OP_SHL: regs[op.dst] = cut(w, cut(w, a) << (b % w)); break;
        case GRJIT_OP_SHR: regs[op.dst] = cut(w, cut(w, a) >> (b % w)); break;
        case GRJIT_OP_SAR:
          regs[op.dst] = cut(w, static_cast<uint64_t>(sext(w, a) >> (b % w)));
          break;
        case GRJIT_OP_NEG: regs[op.dst] = cut(w, 0 - a); break;
        case GRJIT_OP_NOT: regs[op.dst] = cut(w, ~a); break;
        case GRJIT_OP_CLZ: regs[op.dst] = count_leading_zeros(w, a); break;
        case GRJIT_OP_CTZ: regs[op.dst] = count_trailing_zeros(w, a); break;
        case GRJIT_OP_POPCNT: regs[op.dst] = population(w, a); break;
        case GRJIT_OP_ROTL: regs[op.dst] = rotate_left(w, a, b); break;
        case GRJIT_OP_ROTR: regs[op.dst] = rotate_left(w, a, w - (b % w)); break;
        case GRJIT_OP_EXT8_S: regs[op.dst] = sign_extend_from(8, w, a); break;
        case GRJIT_OP_EXT16_S: regs[op.dst] = sign_extend_from(16, w, a); break;
        case GRJIT_OP_EXT32_S: regs[op.dst] = sign_extend_from(32, 64, a); break;
        case GRJIT_OP_WRAP: regs[op.dst] = cut(32, a); break;
        case GRJIT_OP_EXTEND_S: regs[op.dst] = sign_extend_from(32, 64, a); break;
        case GRJIT_OP_EXTEND_U: regs[op.dst] = cut(32, a); break;
        case GRJIT_OP_DIV_S:
        case GRJIT_OP_DIV_U:
        case GRJIT_OP_REM_S:
        case GRJIT_OP_REM_U: {
          const uint64_t ua = cut(w, a), ub = cut(w, b);
          const int64_t sa = sext(w, a), sb = sext(w, b);
          const int64_t min = w == 32 ? INT32_MIN : INT64_MIN;
          if (ub == 0 || (op.kind == GRJIT_OP_DIV_S && sa == min && sb == -1)) {
            /* A partial operation reached with the operands its guards keep out: the
             * program is wrong, not the operation. Reported as an exit no run has. */
            r.exit = kPartialFault;
            return r;
          }
          uint64_t v;
          if (op.kind == GRJIT_OP_DIV_U) {
            v = ua / ub;
          } else if (op.kind == GRJIT_OP_REM_U) {
            v = ua % ub;
          } else if (op.kind == GRJIT_OP_DIV_S) {
            v = static_cast<uint64_t>(sa / sb);
          } else {
            v = sb == -1 ? 0 : static_cast<uint64_t>(sa % sb);
          }
          regs[op.dst] = cut(w, v);
          break;
        }
        case GRJIT_OP_CMP: {
          /* A comparison of 32-bit registers compares their low 32 bits; the signed ones
           * read them as signed. */
          const bool narrow = (op.a.kind == GRJIT_OPERAND_VREG && width[op.a.vreg] == 32) ||
                              (op.b.kind == GRJIT_OPERAND_VREG && width[op.b.vreg] == 32);
          if (narrow) {
            regs[op.dst] = compare(op.cmp, static_cast<uint64_t>(sext(32, a)),
                               static_cast<uint64_t>(sext(32, b)))
                               ? 1 : 0;
            if (op.cmp >= GRJIT_CMP_ULT) {
              regs[op.dst] = compare(op.cmp, cut(32, a), cut(32, b)) ? 1 : 0;
            }
          } else {
            regs[op.dst] = compare(op.cmp, a, b) ? 1 : 0;
          }
          break;
        }
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
            regs[op.dst] = cut(width[op.dst], v);
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
