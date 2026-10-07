/**
 * @file
 *
 * The arm64 convention for calls between compiled functions, tail calls and natives (AD-28),
 * read from the code the emitter produces for every callable function the pinning test generates:
 * what registers it names, what moves the stack pointer, where its tag and internal entry are,
 * what an `adr` points at and what the metadata's sites are. Nothing here runs arm64 code, so it
 * runs on every host; the executing counterpart is `testCalls`, `testTail` and `testNatives` under
 * qemu-aarch64 (tools/xarch/jit-arm64.sh), and each is kept because a decoder written by the
 * emitter's own author agreeing with it is not an execution, and an execution that passes does not
 * say which claim of the convention it leaned on.
 *
 * The decoder knows exactly the instruction classes the assembler emits, by their encodings (the
 * Arm Architecture Reference Manual's, which the assembler's own test checks against the reference
 * assembler), and fails on a word it does not know: a new instruction class in the emitter is a
 * change to this decoder, not a word that passes unseen.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../callable_gen.h"

#include "../../src/arm64/asm_internal.h"
#include "../../src/code/code_internal.h"
#include "../../src/ir/ir_internal.h"

#include <cstring>
#include <set>

namespace {

constexpr uint32_t kRequestOffset = 0x40;

enum class C {
  MOVWIDE, ORR_REG, ORN_REG, ALU_REG, SUBS_REG, MUL, SHIFTV, CSINC, ADDSUB_IMM, SUBS_IMM, LDST_IMM, LDST_REG,
  STP_PRE, LDP_POST, B, BCOND, CBZ, ADR, ADRP, BLR, BR, RET, BRK, UNKNOWN
};

struct Insn {
  C c = C::UNKNOWN;
  uint32_t w = 0;
  std::vector<int> regs; // the general registers 0..30 the word names, as a source or a destination
  bool writes_sp = false;
  int sp_rn = -1;        // for an add/sub on sp: the register it reads (31 = sp)
  int64_t imm = 0;       // add/sub immediate (shifted), branch distance in words, adr bytes
  bool pre_index_sp = false;
};

int rd_of(uint32_t w) { return static_cast<int>(w & 31u); }
int rn_of(uint32_t w) { return static_cast<int>((w >> 5) & 31u); }
int rm_of(uint32_t w) { return static_cast<int>((w >> 16) & 31u); }

void name(Insn & i, int r) {
  if (r != 31) {
    i.regs.push_back(r);
  }
}

Insn decode(uint32_t w) {
  Insn i;
  i.w = w;
  if (w == 0xA9BF7BFDu) { // stp x29, x30, [sp, #-16]!
    i.c = C::STP_PRE;
    i.writes_sp = true;
    i.pre_index_sp = true;
    name(i, 29);
    name(i, 30);
    return i;
  }
  if (w == 0xA8C17BFDu) { // ldp x29, x30, [sp], #16
    i.c = C::LDP_POST;
    i.writes_sp = true;
    name(i, 29);
    name(i, 30);
    return i;
  }
  if ((w & 0xFF800000u) == 0xD2800000u || (w & 0xFF800000u) == 0x92800000u || (w & 0xFF800000u) == 0xF2800000u) {
    i.c = C::MOVWIDE;
    name(i, rd_of(w));
    return i;
  }
  if ((w & 0xFF200000u) == 0xAA000000u || (w & 0xFF200000u) == 0x2A000000u) { // orr (mov), 64 or 32 bit
    i.c = C::ORR_REG;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFF200000u) == 0xAA200000u) { // orn (mvn)
    i.c = C::ORN_REG;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFF200000u) == 0x8B000000u || (w & 0xFF200000u) == 0xCB000000u || (w & 0xFF200000u) == 0x8A000000u ||
      (w & 0xFF200000u) == 0xCA000000u) { // add, sub, and, eor (shifted register)
    i.c = C::ALU_REG;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFF200000u) == 0xEB000000u) { // subs (cmp)
    i.c = C::SUBS_REG;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFFE0FC00u) == 0x9B007C00u) { // madd with xzr (mul)
    i.c = C::MUL;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFFE0FC00u) == 0x9AC02000u || (w & 0xFFE0FC00u) == 0x9AC02400u || (w & 0xFFE0FC00u) == 0x9AC02800u) {
    i.c = C::SHIFTV;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0x7FE00C00u) == 0x1A800400u) { // csinc (cset)
    i.c = C::CSINC;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFF000000u) == 0x91000000u || (w & 0xFF000000u) == 0xD1000000u) { // add/sub immediate, 64-bit, no flags
    i.c = C::ADDSUB_IMM;
    const int64_t imm = static_cast<int64_t>((w >> 10) & 0xFFFu) << (((w >> 22) & 1u) != 0 ? 12 : 0);
    i.imm = (w & 0xFF000000u) == 0xD1000000u ? -imm : imm;
    if (rd_of(w) == 31) {
      i.writes_sp = true;
    } else {
      name(i, rd_of(w));
    }
    i.sp_rn = rn_of(w);
    name(i, rn_of(w));
    return i;
  }
  if ((w & 0xFF000000u) == 0xF1000000u) { // subs immediate (cmp)
    i.c = C::SUBS_IMM;
    name(i, rn_of(w));
    return i;
  }
  if ((w & 0x3B200C00u) == 0x38000000u || (w & 0x3B000000u) == 0x39000000u) { // ldr/str, immediate forms
    i.c = C::LDST_IMM;
    name(i, rd_of(w));
    name(i, rn_of(w));
    return i;
  }
  if ((w & 0x3B200C00u) == 0x38200800u) { // ldr/str, register offset
    i.c = C::LDST_REG;
    name(i, rd_of(w));
    name(i, rn_of(w));
    name(i, rm_of(w));
    return i;
  }
  if ((w & 0xFC000000u) == 0x14000000u) {
    i.c = C::B;
    i.imm = static_cast<int32_t>(w << 6) >> 6;
    return i;
  }
  if ((w & 0xFF000010u) == 0x54000000u) {
    i.c = C::BCOND;
    i.imm = static_cast<int32_t>((w >> 5) << 13) >> 13;
    return i;
  }
  if ((w & 0xFE000000u) == 0xB4000000u) {
    i.c = C::CBZ;
    i.imm = static_cast<int32_t>((w >> 5) << 13) >> 13;
    name(i, rd_of(w));
    return i;
  }
  if ((w & 0x9F000000u) == 0x10000000u || (w & 0x9F000000u) == 0x90000000u) {
    i.c = (w & 0x80000000u) != 0 ? C::ADRP : C::ADR;
    int64_t imm = (static_cast<int64_t>((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
    imm = (imm ^ (int64_t{1} << 20)) - (int64_t{1} << 20);
    i.imm = imm;
    name(i, rd_of(w));
    return i;
  }
  if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) {
    i.c = C::BLR;
    name(i, rn_of(w));
    return i;
  }
  if ((w & 0xFFFFFC1Fu) == 0xD61F0000u) {
    i.c = C::BR;
    name(i, rn_of(w));
    return i;
  }
  if (w == 0xD65F03C0u) {
    i.c = C::RET;
    name(i, 30);
    return i;
  }
  if ((w & 0xFFE0001Fu) == 0xD4200000u) {
    i.c = C::BRK;
    return i;
  }
  return i;
}

uint32_t word_at(const GRJIT_Emitted & e, size_t offset) {
  uint32_t w;
  std::memcpy(&w, e.bytes + offset, 4);
  return w;
}

/* Every function of the three families, emitted for arm64, handed to `check` with its bytes, its
 * metadata and the function. */
template <class F>
void for_every_callable_function(F && check) {
  auto emit = [&](const GRJIT_Function * f, unsigned id, const char * family) {
    GRJIT_Emitted e;
    GRJIT_Result r = grjit_emit_for(GRJIT_ARCH_ARM64, f, grjit_allocator_default(), nullptr, nullptr, kRequestOffset, &e);
    EXPECT_EQ(r, GRJIT_OK) << family << " function " << id;
    if (r != GRJIT_OK) {
      return false;
    }
    check(e, f, id, family);
    grjit_emitted_free(&e);
    return true;
  };
  cg::each_call_function([&](const GRJIT_Function * f, unsigned id) { return emit(f, id, "call"); });
  cg::each_tail_function([&](const GRJIT_Function * f, unsigned id) { return emit(f, id, "tail"); });
  cg::each_native_function([&](const GRJIT_Function * f, unsigned id) { return emit(f, id, "native"); });
}

/* Where the tag is: the sixteen bytes before the internal entry. The words between the adapter and
 * the tag are `brk #0` padding. */
bool in_data(const GRJIT_Emitted & e, size_t offset) {
  return offset >= e.internal_offset - 16 && offset < e.internal_offset;
}

} // namespace

TEST(Arm64Calls, EveryWordOfEveryCallableFunctionDecodesAndNoCalleeSavedRegisterNorX18IsNamed) {
  size_t functions = 0, words = 0;
  std::set<int> used;
  for_every_callable_function([&](const GRJIT_Emitted & e, const GRJIT_Function *, unsigned id, const char * family) {
    functions++;
    ASSERT_EQ(e.size % 4, 0u);
    for (size_t off = 0; off < e.size; off += 4) {
      if (in_data(e, off)) {
        continue;
      }
      Insn i = decode(word_at(e, off));
      ASSERT_NE(i.c, C::UNKNOWN) << family << " " << id << " at +" << off << ": word " << std::hex << i.w
                                 << " is not an instruction class the emitter makes";
      words++;
      for (int r : i.regs) {
        used.insert(r);
        EXPECT_FALSE(r >= 19 && r <= 28) << family << " " << id << " at +" << off << ": x" << r
                                         << " is callee-saved and compiled code uses none";
        EXPECT_NE(r, 18) << family << " " << id << " at +" << off << ": x18 is the platform register";
      }
    }
  });
  EXPECT_GE(functions, 200u) << "the corpus is every function the pinning test generates";
  EXPECT_GT(words, 10000u);
  // The registers that are used are the ones the convention names: the arguments x0-x7, the context x9,
  // the temporaries x10 and x15-x17, the frame record's x29 and x30, and the scratch x1-x2 of the
  // operations. The check above is worth something only if they are all seen.
  for (int r : {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 15, 16, 17, 29, 30}) {
    EXPECT_TRUE(used.count(r) != 0) << "x" << r << " is in none of the corpus's code: the check above did not see it";
  }
  for (int r : used) {
    EXPECT_TRUE(r <= 17 || r == 29 || r == 30) << "x" << r << " is used";
  }
}

TEST(Arm64Calls, TheStackPointerIsMovedOnlyByThePrologueTheEpilogueAndTheArgumentAreasAndAlwaysBySixteens) {
  size_t movers = 0;
  for_every_callable_function([&](const GRJIT_Emitted & e, const GRJIT_Function * f, unsigned id, const char * family) {
    size_t pushes = 0, pops = 0, adapter_pops = 0, frame_alloc = 0, restores = 0;
    for (size_t off = 0; off < e.size; off += 4) {
      if (in_data(e, off)) {
        continue;
      }
      Insn i = decode(word_at(e, off));
      if (!i.writes_sp) {
        continue;
      }
      movers++;
      switch (i.c) {
        case C::STP_PRE:
          pushes++;
          break;
        case C::LDP_POST:
          pops++;
          if (off < e.internal_offset) {
            adapter_pops++; // the adapter gives its frame record (and so the caller's x29) back
          }
          break;
        case C::ADDSUB_IMM:
          EXPECT_EQ(i.imm % 16, 0) << family << " " << id << " at +" << off << ": sp moves by " << i.imm;
          if (i.sp_rn == 31) {
            frame_alloc++; // sub sp, sp, #n or add sp, sp, #n: a frame, an area, an area given back
          } else {
            EXPECT_EQ(i.sp_rn, 29) << family << " " << id << " at +" << off << ": sp is set from x" << i.sp_rn;
            restores++; // mov sp, x29 or add/sub sp, x29, #n: an epilogue, or a tail call's replacement
          }
          break;
        default:
          ADD_FAILURE() << family << " " << id << " at +" << off << ": an instruction writes sp that is none of the allowed forms";
      }
    }
    // The adapter's frame record and the internal entry's: exactly two pushes; and each is given back on
    // every way out (the adapter's two exits, the internal function's epilogues, one per return, deopt
    // and failure path and none for a tail call).
    EXPECT_EQ(pushes, 2u) << family << " " << id;
    EXPECT_GE(pops, 2u) << family << " " << id;
    EXPECT_EQ(adapter_pops, 2u) << family << " " << id
        << ": the adapter restores x29 from its own frame record on both its exits (the normal one and the refusal)";
    EXPECT_GE(frame_alloc, 2u) << family << " " << id << ": the adapter's frame and the function's";
    EXPECT_GE(restores, 1u) << family << " " << id << ": at least one epilogue";
    (void)f;
  });
  EXPECT_GT(movers, 5000u);
}

TEST(Arm64Calls, TheTagIsTheMagicAndTheParameterCountThenTheTokenOnASixteenByteBoundaryBeforeTheInternalEntry) {
  for_every_callable_function([&](const GRJIT_Emitted & e, const GRJIT_Function * f, unsigned id, const char * family) {
    ASSERT_GE(e.internal_offset, 16u) << family << " " << id;
    ASSERT_LE(e.internal_offset, e.size);
    EXPECT_EQ(e.internal_offset % 16, 0u) << family << " " << id << ": the code starts on a page, so the entry is aligned";
    uint64_t tag[2];
    std::memcpy(tag, e.bytes + e.internal_offset - 16, sizeof tag);
    EXPECT_EQ(tag[0], (uint64_t{0x4752494E} << 32) | f->param_count) << family << " " << id;
    EXPECT_EQ(tag[1], f->token) << family << " " << id;
    // The padding between the adapter and the tag is trap words, so a branch into it never executes it.
    size_t pad = e.internal_offset - 16;
    while (pad >= 4 && word_at(e, pad - 4) == 0xD4200000u) {
      pad -= 4;
    }
    // The adapter ends in a `ret` and the padding follows it.
    ASSERT_GE(pad, 4u);
    EXPECT_EQ(word_at(e, pad - 4), 0xD65F03C0u) << family << " " << id << ": the adapter's last word is a ret";
    // The entry is a frame record, a frame pointer set from sp, and the native-stack check before the frame
    // is made: sub x16, sp, #alloc; ldr x15, [x9, limit]; cmp x16, x15; b.lo overflow.
    EXPECT_EQ(word_at(e, e.internal_offset), 0xA9BF7BFDu) << family << " " << id;
    EXPECT_EQ(word_at(e, e.internal_offset + 4), 0x910003FDu) << family << " " << id << ": mov x29, sp";
    bool cmp_seen = false;
    size_t cmp_at = 0;
    for (size_t k = 8; k < 40 && e.internal_offset + k + 4 <= e.size; k += 4) {
      if (word_at(e, e.internal_offset + k) == 0xEB0F021Fu) { // cmp x16, x15
        cmp_seen = true;
        cmp_at = k;
        break;
      }
    }
    ASSERT_TRUE(cmp_seen) << family << " " << id << ": the prologue compares the lowest address of the frame with the limit";
    Insn br = decode(word_at(e, e.internal_offset + cmp_at + 4));
    EXPECT_TRUE(br.c == C::BCOND) << family << " " << id << ": a conditional branch to the overflow exit follows";
    // The check precedes the frame: the first `sub sp, sp` comes after the compare.
    for (size_t k = 8; k < cmp_at; k += 4) {
      Insn i = decode(word_at(e, e.internal_offset + k));
      EXPECT_FALSE(i.writes_sp) << family << " " << id << ": sp moves before the stack check";
    }
  });
}

TEST(Arm64Calls, EveryAdrNamesASiteOrTheInternalEntryAndTheCellsTwoWordsAreStoredRightAfterIt) {
  // The walk start is an address the walk can resolve: the return address of the call about to be made,
  // which is the metadata's site. So every `adr x16` of a callable function (but the adapter's, which
  // calls the internal entry through it) names a site, and is followed within a few instructions by the
  // two stores of the cell: the frame base `x29` and `x16`, through the context in x15.
  size_t walk_adrs = 0;
  for_every_callable_function([&](const GRJIT_Emitted & e, const GRJIT_Function *, unsigned id, const char * family) {
    std::set<uint32_t> sites;
    for (size_t k = 0; k < e.meta.meta.site_count; k++) {
      sites.insert(e.meta.meta.sites[k].code_offset);
    }
    size_t adapter_adrs = 0;
    for (size_t off = 0; off < e.size; off += 4) {
      if (in_data(e, off)) {
        continue;
      }
      Insn i = decode(word_at(e, off));
      EXPECT_NE(i.c, C::ADRP) << family << " " << id << ": the long form is not needed for a call's own return address";
      if (i.c != C::ADR) {
        continue;
      }
      const size_t target = static_cast<size_t>(static_cast<int64_t>(off) + i.imm);
      ASSERT_LE(target, e.size) << family << " " << id;
      if (target == e.internal_offset) {
        adapter_adrs++;
        continue;
      }
      walk_adrs++;
      EXPECT_TRUE(sites.count(static_cast<uint32_t>(target)) != 0)
          << family << " " << id << " at +" << off << ": the walk start +" << target << " is no site";
      // x29 and x16 are stored through x15 in the next few instructions: the cell's two words.
      bool base = false, ret = false;
      for (size_t k = off + 4; k < off + 24 && k < e.size; k += 4) {
        const uint32_t w = word_at(e, k);
        Insn st = decode(w);
        if (st.c == C::LDST_IMM && (w & 0x3F400000u) == 0x39000000u && rn_of(w) == 15) { // str x, [x15, #off]
          base = base || rd_of(w) == 29;
          ret = ret || rd_of(w) == 16;
        }
      }
      EXPECT_TRUE(base) << family << " " << id << " at +" << off << ": x29 is not stored through x15 after the adr";
      EXPECT_TRUE(ret) << family << " " << id << " at +" << off << ": x16 is not stored through x15 after the adr";
    }
    EXPECT_EQ(adapter_adrs, 1u) << family << " " << id << ": one adr to the internal entry, in the adapter";
  });
  EXPECT_GT(walk_adrs, 2000u);
}

TEST(Arm64Calls, EverySiteOfACallPollOrFramePushIsTheAddressAfterABlrAndEveryExitIsAfterAHookCall) {
  size_t checked = 0, exits = 0;
  for_every_callable_function([&](const GRJIT_Emitted & e, const GRJIT_Function *, unsigned id, const char * family) {
    for (size_t k = 0; k < e.meta.meta.site_count; k++) {
      const GRCORE_CodeSite & s = e.meta.meta.sites[k];
      switch (s.kind) {
        case GRCORE_SITE_GC_POINT_CALL:
        case GRCORE_SITE_GC_POINT_POLL:
        case GRCORE_SITE_GC_POINT_FRAME_PUSH:
          ASSERT_GE(s.code_offset, 4u);
          ASSERT_LE(s.code_offset, e.size);
          EXPECT_EQ(decode(word_at(e, s.code_offset - 4)).c, C::BLR)
              << family << " " << id << " site at +" << s.code_offset << ": a site is a return address";
          checked++;
          break;
        case GRCORE_SITE_GUARD: {
          // An exit's site is the walk start the stub stored before it called the deopt hook: after the
          // call and the test of its answer, never before the call.
          bool blr_before = false;
          for (size_t back = 4; back <= 16 && s.code_offset >= back; back += 4) {
            blr_before = blr_before || decode(word_at(e, s.code_offset - back)).c == C::BLR;
          }
          EXPECT_TRUE(blr_before) << family << " " << id << " exit at +" << s.code_offset << " follows no hook call";
          exits++;
          break;
        }
        default:
          break;
      }
    }
  });
  EXPECT_GT(checked, 1500u);
  EXPECT_GT(exits, 1000u);
}

TEST(Arm64Calls, ACallToACompiledCalleeIsEnteredThroughTheSlotsWordAndAnyOtherCallHasItsWalkStartStored) {
  // A site of kind CALL is either the return address of a call to a compiled callee (entered through
  // `ldr x16, [x29, entry]`, whose walk start the push stored before the hook) or of a native or a helper
  // (entered through an address made in x16, which has its own `adr` naming the site). One that is
  // neither would be a call whose frame the walk could not find.
  size_t guest = 0, other = 0;
  for_every_callable_function([&](const GRJIT_Emitted & e, const GRJIT_Function *, unsigned id, const char * family) {
    std::set<size_t> named;
    for (size_t off = 0; off < e.size; off += 4) {
      if (in_data(e, off)) {
        continue;
      }
      Insn i = decode(word_at(e, off));
      if (i.c == C::ADR) {
        named.insert(static_cast<size_t>(static_cast<int64_t>(off) + i.imm));
      }
    }
    for (size_t k = 0; k < e.meta.meta.site_count; k++) {
      const GRCORE_CodeSite & s = e.meta.meta.sites[k];
      if (s.kind != GRCORE_SITE_GC_POINT_CALL) {
        continue;
      }
      Insn before = decode(word_at(e, s.code_offset - 8));
      const bool entered_from_slot = (before.c == C::LDST_IMM || before.c == C::LDST_REG) &&
          rd_of(word_at(e, s.code_offset - 8)) == 16;
      if (entered_from_slot) {
        guest++;
      } else {
        other++;
        EXPECT_TRUE(named.count(s.code_offset) != 0)
            << family << " " << id << " call site +" << s.code_offset << " has no walk start stored for it";
      }
    }
  });
  EXPECT_GT(guest, 300u);
  EXPECT_GT(other, 300u);
}

GRJIT_TEST_MAIN()
