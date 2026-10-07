/**
 * @file
 *
 * The AArch64 assembler: every encoding against the bytes `aarch64-linux-gnu-as`
 * produces for the same instruction (recorded in the table, with the
 * disassembly `aarch64-linux-gnu-objdump` gives back), the immediate shapes,
 * the offsets that do not fit an instruction, the branch forms and their
 * ranges, and the failure behaviour.
 *
 * Nothing here executes AArch64 code, so it runs on every host. The recorded
 * bytes are re-checked against the real disassembler by `tools/xarch/jit-arm64.sh`
 * (which sets GRJIT_AARCH64_OBJDUMP to the cross `objdump`); without that
 * variable the disassembly test says so and does nothing, which is the one
 * place this file depends on a tool this machine does not have.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/arm64/asm_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

struct Case {
  const char * name;
  std::function<void(GRJIT_A64Asm *)> emit;
  std::vector<uint8_t> bytes;
  const char * text; /* objdump -d, whitespace collapsed, ';' between lines */
};

std::vector<Case> cases() {
  using A = GRJIT_A64Asm *;
  return {
      {"mov x0, x1", [](A a) { grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0xE0, 0x03, 0x01, 0xAA},
          "mov x0, x1"},
      {"mov x17, x30", [](A a) { grjit_a64_mov_rr(a, GRJIT_A64_X17, GRJIT_A64_X30); },
          {0xF1, 0x03, 0x1E, 0xAA},
          "mov x17, x30"},
      {"mov w0, w0", [](A a) { grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0); },
          {0xE0, 0x03, 0x00, 0x2A},
          "mov w0, w0"},
      {"mov w5, w9", [](A a) { grjit_a64_mov32_rr(a, GRJIT_A64_X5, GRJIT_A64_X9); },
          {0xE5, 0x03, 0x09, 0x2A},
          "mov w5, w9"},
      {"mov x29, sp", [](A a) { grjit_a64_mov_sp(a, GRJIT_A64_X29, GRJIT_A64_SP); },
          {0xFD, 0x03, 0x00, 0x91},
          "mov x29, sp"},
      {"mov sp, x29", [](A a) { grjit_a64_mov_sp(a, GRJIT_A64_SP, GRJIT_A64_X29); },
          {0xBF, 0x03, 0x00, 0x91},
          "mov sp, x29"},
      {"movz x0, #0", [](A a) { grjit_a64_movz(a, GRJIT_A64_X0, 0, 0); },
          {0x00, 0x00, 0x80, 0xD2},
          "mov x0, #0x0 // #0"},
      {"movz x1, #0xffff", [](A a) { grjit_a64_movz(a, GRJIT_A64_X1, 0xffff, 0); },
          {0xE1, 0xFF, 0x9F, 0xD2},
          "mov x1, #0xffff // #65535"},
      {"movz x2, #0x1234, lsl #16", [](A a) { grjit_a64_movz(a, GRJIT_A64_X2, 0x1234, 1); },
          {0x82, 0x46, 0xA2, 0xD2},
          "mov x2, #0x12340000 // #305397760"},
      {"movz x3, #0xabcd, lsl #32", [](A a) { grjit_a64_movz(a, GRJIT_A64_X3, 0xabcd, 2); },
          {0xA3, 0x79, 0xD5, 0xD2},
          "mov x3, #0xabcd00000000 // #188896956645376"},
      {"movz x4, #0x8000, lsl #48", [](A a) { grjit_a64_movz(a, GRJIT_A64_X4, 0x8000, 3); },
          {0x04, 0x00, 0xF0, 0xD2},
          "mov x4, #0x8000000000000000 // #-9223372036854775808"},
      {"movn x0, #0", [](A a) { grjit_a64_movn(a, GRJIT_A64_X0, 0, 0); },
          {0x00, 0x00, 0x80, 0x92},
          "mov x0, #0xffffffffffffffff // #-1"},
      {"movn x3, #0x1234, lsl #48", [](A a) { grjit_a64_movn(a, GRJIT_A64_X3, 0x1234, 3); },
          {0x83, 0x46, 0xE2, 0x92},
          "mov x3, #0xedcbffffffffffff // #-1311673391471656961"},
      {"movk x16, #0xbeef, lsl #32", [](A a) { grjit_a64_movk(a, GRJIT_A64_X16, 0xbeef, 2); },
          {0xF0, 0xDD, 0xD7, 0xF2},
          "movk x16, #0xbeef, lsl #32"},
      {"movk x30, #1", [](A a) { grjit_a64_movk(a, GRJIT_A64_X30, 1, 0); },
          {0x3E, 0x00, 0x80, 0xF2},
          "movk x30, #0x1"},
      {"mov_ri 0", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, 0); },
          {0x00, 0x00, 0x80, 0xD2},
          "mov x0, #0x0 // #0"},
      {"mov_ri 1", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, 1); },
          {0x20, 0x00, 0x80, 0xD2},
          "mov x0, #0x1 // #1"},
      {"mov_ri 0xffff", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, 0xffff); },
          {0xE0, 0xFF, 0x9F, 0xD2},
          "mov x0, #0xffff // #65535"},
      {"mov_ri 0x10000", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, 0x10000); },
          {0x20, 0x00, 0xA0, 0xD2},
          "mov x0, #0x10000 // #65536"},
      {"mov_ri 0x100000001 (two pieces)", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, UINT64_C(0x100000001)); },
          {0x20, 0x00, 0x80, 0xD2, 0x20, 0x00, 0xC0, 0xF2},
          "mov x0, #0x1 // #1; movk x0, #0x1, lsl #32"},
      {"mov_ri 0x0001000200030000 (three pieces)", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, UINT64_C(0x0001000200030000)); },
          {0x60, 0x00, 0xA0, 0xD2, 0x40, 0x00, 0xC0, 0xF2, 0x20, 0x00, 0xE0, 0xF2},
          "mov x0, #0x30000 // #196608; movk x0, #0x2, lsl #32; movk x0, #0x1, lsl #48"},
      {"mov_ri 0x123456789abcdef0 (four pieces)", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, UINT64_C(0x123456789abcdef0)); },
          {0x00, 0xDE, 0x9B, 0xD2, 0x80, 0x57, 0xB3, 0xF2, 0x00, 0xCF, 0xCA, 0xF2, 0x80, 0x46, 0xE2, 0xF2},
          "mov x0, #0xdef0 // #57072; movk x0, #0x9abc, lsl #16; movk x0, #0x5678, lsl #32; movk x0, #0x1234, lsl #48"},
      {"mov_ri all ones", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, ~UINT64_C(0)); },
          {0x00, 0x00, 0x80, 0x92},
          "mov x0, #0xffffffffffffffff // #-1"},
      {"mov_ri -2", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, ~UINT64_C(1)); },
          {0x20, 0x00, 0x80, 0x92},
          "mov x0, #0xfffffffffffffffe // #-2"},
      {"mov_ri 0xffffffffffff0000", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, UINT64_C(0xffffffffffff0000)); },
          {0xE0, 0xFF, 0x9F, 0x92},
          "mov x0, #0xffffffffffff0000 // #-65536"},
      {"mov_ri INT64_MIN", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, UINT64_C(0x8000000000000000)); },
          {0x00, 0x00, 0xF0, 0xD2},
          "mov x0, #0x8000000000000000 // #-9223372036854775808"},
      {"mov_ri 0xffff0000ffff1234 (movn then movk)", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X0, UINT64_C(0xffff0000ffff1234)); },
          {0x60, 0xB9, 0x9D, 0x92, 0x00, 0x00, 0xC0, 0xF2},
          "mov x0, #0xffffffffffff1234 // #-60876; movk x0, #0x0, lsl #32"},
      {"mov_ri 0x00007f1234567890 (a pointer)", [](A a) { grjit_a64_mov_ri(a, GRJIT_A64_X16, UINT64_C(0x00007f1234567890)); },
          {0x10, 0x12, 0x8F, 0xD2, 0xD0, 0x8A, 0xA6, 0xF2, 0x50, 0xE2, 0xCF, 0xF2},
          "mov x16, #0x7890 // #30864; movk x16, #0x3456, lsl #16; movk x16, #0x7f12, lsl #32"},
      {"add x0, x1, #4095", [](A a) { grjit_a64_add_imm(a, GRJIT_A64_X0, GRJIT_A64_X1, 4095, false); },
          {0x20, 0xFC, 0x3F, 0x91},
          "add x0, x1, #0xfff"},
      {"sub sp, sp, #16", [](A a) { grjit_a64_sub_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, 16, false); },
          {0xFF, 0x43, 0x00, 0xD1},
          "sub sp, sp, #0x10"},
      {"sub sp, sp, #1, lsl #12", [](A a) { grjit_a64_sub_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, 1, true); },
          {0xFF, 0x07, 0x40, 0xD1},
          "sub sp, sp, #0x1, lsl #12"},
      {"add sp, sp, #4095, lsl #12", [](A a) { grjit_a64_add_imm(a, GRJIT_A64_SP, GRJIT_A64_SP, 4095, true); },
          {0xFF, 0xFF, 0x7F, 0x91},
          "add sp, sp, #0xfff, lsl #12"},
      {"add_uxtx sp, sp, x17", [](A a) { grjit_a64_add_uxtx(a, GRJIT_A64_SP, GRJIT_A64_SP, GRJIT_A64_X17); },
          {0xFF, 0x63, 0x31, 0x8B},
          "add sp, sp, x17"},
      {"sub_uxtx sp, sp, x17", [](A a) { grjit_a64_sub_uxtx(a, GRJIT_A64_SP, GRJIT_A64_SP, GRJIT_A64_X17); },
          {0xFF, 0x63, 0x31, 0xCB},
          "sub sp, sp, x17"},
      {"add_uxtx x16, x29, x17", [](A a) { grjit_a64_add_uxtx(a, GRJIT_A64_X16, GRJIT_A64_FP, GRJIT_A64_X17); },
          {0xB0, 0x63, 0x31, 0x8B},
          "add x16, x29, x17, uxtx"},
      {"sub_uxtx x16, sp, x17", [](A a) { grjit_a64_sub_uxtx(a, GRJIT_A64_X16, GRJIT_A64_SP, GRJIT_A64_X17); },
          {0xF0, 0x63, 0x31, 0xCB},
          "sub x16, sp, x17"},
      {"sub_sp 32", [](A a) { grjit_a64_sub_sp(a, 32); },
          {0xFF, 0x83, 0x00, 0xD1},
          "sub sp, sp, #0x20"},
      {"sub_sp 4096", [](A a) { grjit_a64_sub_sp(a, 4096); },
          {0xFF, 0x07, 0x40, 0xD1},
          "sub sp, sp, #0x1, lsl #12"},
      {"sub_sp 4112", [](A a) { grjit_a64_sub_sp(a, 4112); },
          {0xFF, 0x07, 0x40, 0xD1, 0xFF, 0x43, 0x00, 0xD1},
          "sub sp, sp, #0x1, lsl #12; sub sp, sp, #0x10"},
      {"sub_sp 1 MiB", [](A a) { grjit_a64_sub_sp(a, 1u << 20); },
          {0xFF, 0x03, 0x44, 0xD1},
          "sub sp, sp, #0x100, lsl #12"},
      {"sub_sp 1 MiB + 16", [](A a) { grjit_a64_sub_sp(a, (1u << 20) + 16); },
          {0xFF, 0x03, 0x44, 0xD1, 0xFF, 0x43, 0x00, 0xD1},
          "sub sp, sp, #0x100, lsl #12; sub sp, sp, #0x10"},
      {"add_sp 8000", [](A a) { grjit_a64_add_sp(a, 8000); },
          {0xFF, 0x07, 0x40, 0x91, 0xFF, 0x03, 0x3D, 0x91},
          "add sp, sp, #0x1, lsl #12; add sp, sp, #0xf40"},
      {"add x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_ADD, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x00, 0x01, 0x8B},
          "add x0, x0, x1"},
      {"add x17, x16, x2", [](A a) { grjit_a64_alu(a, GRJIT_A64_ADD, GRJIT_A64_X17, GRJIT_A64_X16, GRJIT_A64_X2); },
          {0x11, 0x02, 0x02, 0x8B},
          "add x17, x16, x2"},
      {"sub x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_SUB, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x00, 0x01, 0xCB},
          "sub x0, x0, x1"},
      {"sub x17, x16, x2", [](A a) { grjit_a64_alu(a, GRJIT_A64_SUB, GRJIT_A64_X17, GRJIT_A64_X16, GRJIT_A64_X2); },
          {0x11, 0x02, 0x02, 0xCB},
          "sub x17, x16, x2"},
      {"and x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_AND, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x00, 0x01, 0x8A},
          "and x0, x0, x1"},
      {"and x17, x16, x2", [](A a) { grjit_a64_alu(a, GRJIT_A64_AND, GRJIT_A64_X17, GRJIT_A64_X16, GRJIT_A64_X2); },
          {0x11, 0x02, 0x02, 0x8A},
          "and x17, x16, x2"},
      {"orr x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_ORR, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x00, 0x01, 0xAA},
          "orr x0, x0, x1"},
      {"orr x17, x16, x2", [](A a) { grjit_a64_alu(a, GRJIT_A64_ORR, GRJIT_A64_X17, GRJIT_A64_X16, GRJIT_A64_X2); },
          {0x11, 0x02, 0x02, 0xAA},
          "orr x17, x16, x2"},
      {"eor x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_EOR, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x00, 0x01, 0xCA},
          "eor x0, x0, x1"},
      {"eor x17, x16, x2", [](A a) { grjit_a64_alu(a, GRJIT_A64_EOR, GRJIT_A64_X17, GRJIT_A64_X16, GRJIT_A64_X2); },
          {0x11, 0x02, 0x02, 0xCA},
          "eor x17, x16, x2"},
      {"mul x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_MUL, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x7C, 0x01, 0x9B},
          "mul x0, x0, x1"},
      {"mul x5, x6, x7", [](A a) { grjit_a64_alu(a, GRJIT_A64_MUL, GRJIT_A64_X5, GRJIT_A64_X6, GRJIT_A64_X7); },
          {0xC5, 0x7C, 0x07, 0x9B},
          "mul x5, x6, x7"},
      {"lsl x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_LSLV, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x20, 0xC1, 0x9A},
          "lsl x0, x0, x1"},
      {"lsr x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_LSRV, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x24, 0xC1, 0x9A},
          "lsr x0, x0, x1"},
      {"asr x0, x0, x1", [](A a) { grjit_a64_alu(a, GRJIT_A64_ASRV, GRJIT_A64_X0, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x00, 0x28, 0xC1, 0x9A},
          "asr x0, x0, x1"},
      {"asr x9, x10, x11", [](A a) { grjit_a64_alu(a, GRJIT_A64_ASRV, GRJIT_A64_X9, GRJIT_A64_X10, GRJIT_A64_X11); },
          {0x49, 0x29, 0xCB, 0x9A},
          "asr x9, x10, x11"},
      {"neg x0, x0", [](A a) { grjit_a64_neg(a, GRJIT_A64_X0, GRJIT_A64_X0); },
          {0xE0, 0x03, 0x00, 0xCB},
          "neg x0, x0"},
      {"neg x3, x4", [](A a) { grjit_a64_neg(a, GRJIT_A64_X3, GRJIT_A64_X4); },
          {0xE3, 0x03, 0x04, 0xCB},
          "neg x3, x4"},
      {"mvn x0, x0", [](A a) { grjit_a64_mvn(a, GRJIT_A64_X0, GRJIT_A64_X0); },
          {0xE0, 0x03, 0x20, 0xAA},
          "mvn x0, x0"},
      {"mvn x3, x4", [](A a) { grjit_a64_mvn(a, GRJIT_A64_X3, GRJIT_A64_X4); },
          {0xE3, 0x03, 0x24, 0xAA},
          "mvn x3, x4"},
      {"cmp x0, x1", [](A a) { grjit_a64_cmp(a, GRJIT_A64_X0, GRJIT_A64_X1); },
          {0x1F, 0x00, 0x01, 0xEB},
          "cmp x0, x1"},
      {"cmp x16, x17", [](A a) { grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X17); },
          {0x1F, 0x02, 0x11, 0xEB},
          "cmp x16, x17"},
      {"cset w0, eq", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_EQ); },
          {0xE0, 0x17, 0x9F, 0x1A},
          "cset w0, eq // eq = none"},
      {"cset w0, ne", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_NE); },
          {0xE0, 0x07, 0x9F, 0x1A},
          "cset w0, ne // ne = any"},
      {"cset w0, hs", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_HS); },
          {0xE0, 0x37, 0x9F, 0x1A},
          "cset w0, cs // cs = hs, nlast"},
      {"cset w0, lo", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_LO); },
          {0xE0, 0x27, 0x9F, 0x1A},
          "cset w0, cc // cc = lo, ul, last"},
      {"cset w0, hi", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_HI); },
          {0xE0, 0x97, 0x9F, 0x1A},
          "cset w0, hi // hi = pmore"},
      {"cset w0, ls", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_LS); },
          {0xE0, 0x87, 0x9F, 0x1A},
          "cset w0, ls // ls = plast"},
      {"cset w0, ge", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_GE); },
          {0xE0, 0xB7, 0x9F, 0x1A},
          "cset w0, ge // ge = tcont"},
      {"cset w0, lt", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_LT); },
          {0xE0, 0xA7, 0x9F, 0x1A},
          "cset w0, lt // lt = tstop"},
      {"cset w0, gt", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_GT); },
          {0xE0, 0xD7, 0x9F, 0x1A},
          "cset w0, gt"},
      {"cset w0, le", [](A a) { grjit_a64_cset(a, GRJIT_A64_X0, GRJIT_A64_LE); },
          {0xE0, 0xC7, 0x9F, 0x1A},
          "cset w0, le"},
      {"cset w9, mi", [](A a) { grjit_a64_cset(a, GRJIT_A64_X9, GRJIT_A64_MI); },
          {0xE9, 0x57, 0x9F, 0x1A},
          "cset w9, mi // mi = first"},
      {"ldr x0, [x1]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 0); },
          {0x20, 0x00, 0x40, 0xF9},
          "ldr x0, [x1]"},
      {"ldr x0, [x1, #8]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 8); },
          {0x20, 0x04, 0x40, 0xF9},
          "ldr x0, [x1, #8]"},
      {"ldr x0, [x1, #32760]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 32760); },
          {0x20, 0xFC, 0x7F, 0xF9},
          "ldr x0, [x1, #32760]"},
      {"ldrb w0, [x1, #4095]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, 4095); },
          {0x20, 0xFC, 0x7F, 0x39},
          "ldrb w0, [x1, #4095]"},
      {"ldrh w0, [x1, #8190]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U16, GRJIT_A64_X0, GRJIT_A64_X1, 8190); },
          {0x20, 0xFC, 0x7F, 0x79},
          "ldrh w0, [x1, #8190]"},
      {"ldr w0, [x1, #16380]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, 16380); },
          {0x20, 0xFC, 0x7F, 0xB9},
          "ldr w0, [x1, #16380]"},
      {"ldrsb x0, [x1, #5]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S8, GRJIT_A64_X0, GRJIT_A64_X1, 5); },
          {0x20, 0x14, 0x80, 0x39},
          "ldrsb x0, [x1, #5]"},
      {"ldrsh x0, [x1, #6]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S16, GRJIT_A64_X0, GRJIT_A64_X1, 6); },
          {0x20, 0x0C, 0x80, 0x79},
          "ldrsh x0, [x1, #6]"},
      {"ldrsw x0, [x1, #12]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S32, GRJIT_A64_X0, GRJIT_A64_X1, 12); },
          {0x20, 0x0C, 0x80, 0xB9},
          "ldrsw x0, [x1, #12]"},
      {"str x0, [x2, #16]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X2, 16); },
          {0x40, 0x08, 0x00, 0xF9},
          "str x0, [x2, #16]"},
      {"strb w0, [x1, #3]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, 3); },
          {0x20, 0x0C, 0x00, 0x39},
          "strb w0, [x1, #3]"},
      {"strh w0, [x1, #2]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U16, GRJIT_A64_X0, GRJIT_A64_X1, 2); },
          {0x20, 0x04, 0x00, 0x79},
          "strh w0, [x1, #2]"},
      {"str w0, [x1, #4]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, 4); },
          {0x20, 0x04, 0x00, 0xB9},
          "str w0, [x1, #4]"},
      {"ldur x0, [x29, #-8]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -8); },
          {0xA0, 0x83, 0x5F, 0xF8},
          "ldur x0, [x29, #-8]"},
      {"ldur x0, [x29, #-256]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -256); },
          {0xA0, 0x03, 0x50, 0xF8},
          "ldur x0, [x29, #-256]"},
      {"ldur x0, [x1, #255]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 255); },
          {0x20, 0xF0, 0x4F, 0xF8},
          "ldur x0, [x1, #255]"},
      {"ldur x0, [x1, #1] (unaligned)", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 1); },
          {0x20, 0x10, 0x40, 0xF8},
          "ldur x0, [x1, #1]"},
      {"ldurb w0, [x1, #-1]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, -1); },
          {0x20, 0xF0, 0x5F, 0x38},
          "ldurb w0, [x1, #-1]"},
      {"ldurh w0, [x1, #-3]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U16, GRJIT_A64_X0, GRJIT_A64_X1, -3); },
          {0x20, 0xD0, 0x5F, 0x78},
          "ldurh w0, [x1, #-3]"},
      {"ldur w0, [x1, #-100]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, -100); },
          {0x20, 0xC0, 0x59, 0xB8},
          "ldur w0, [x1, #-100]"},
      {"ldursb x0, [x1, #-100]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S8, GRJIT_A64_X0, GRJIT_A64_X1, -100); },
          {0x20, 0xC0, 0x99, 0x38},
          "ldursb x0, [x1, #-100]"},
      {"ldursh x0, [x1, #-50]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S16, GRJIT_A64_X0, GRJIT_A64_X1, -50); },
          {0x20, 0xE0, 0x9C, 0x78},
          "ldursh x0, [x1, #-50]"},
      {"ldursw x0, [x1, #-20]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S32, GRJIT_A64_X0, GRJIT_A64_X1, -20); },
          {0x20, 0xC0, 0x9E, 0xB8},
          "ldursw x0, [x1, #-20]"},
      {"stur x0, [x29, #-24]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -24); },
          {0xA0, 0x83, 0x1E, 0xF8},
          "stur x0, [x29, #-24]"},
      {"sturb w0, [x1, #-1]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, -1); },
          {0x20, 0xF0, 0x1F, 0x38},
          "sturb w0, [x1, #-1]"},
      {"sturh w0, [x1, #-2]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U16, GRJIT_A64_X0, GRJIT_A64_X1, -2); },
          {0x20, 0xE0, 0x1F, 0x78},
          "sturh w0, [x1, #-2]"},
      {"stur w0, [x1, #-4]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, -4); },
          {0x20, 0xC0, 0x1F, 0xB8},
          "stur w0, [x1, #-4]"},
      {"ldr x0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x71, 0xF8},
          "ldr x0, [x1, x17]"},
      {"ldrb w0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x71, 0x38},
          "ldrb w0, [x1, x17]"},
      {"ldrh w0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_U16, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x71, 0x78},
          "ldrh w0, [x1, x17]"},
      {"ldr w0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x71, 0xB8},
          "ldr w0, [x1, x17]"},
      {"ldrsb x0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_S8, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0xB1, 0x38},
          "ldrsb x0, [x1, x17]"},
      {"ldrsh x0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_S16, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0xB1, 0x78},
          "ldrsh x0, [x1, x17]"},
      {"ldrsw x0, [x1, x17]", [](A a) { grjit_a64_load_reg(a, GRJIT_A64_MEM_S32, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0xB1, 0xB8},
          "ldrsw x0, [x1, x17]"},
      {"str x0, [x1, x17]", [](A a) { grjit_a64_store_reg(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x31, 0xF8},
          "str x0, [x1, x17]"},
      {"strb w0, [x1, x17]", [](A a) { grjit_a64_store_reg(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x31, 0x38},
          "strb w0, [x1, x17]"},
      {"strh w0, [x1, x17]", [](A a) { grjit_a64_store_reg(a, GRJIT_A64_MEM_U16, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x31, 0x78},
          "strh w0, [x1, x17]"},
      {"str w0, [x1, x17]", [](A a) { grjit_a64_store_reg(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, GRJIT_A64_X17); },
          {0x20, 0x68, 0x31, 0xB8},
          "str w0, [x1, x17]"},
      {"ldr x0, [x29, #-264] (just past ldur: x17)", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -264); },
          {0xF1, 0x20, 0x80, 0x92, 0xA0, 0x6B, 0x71, 0xF8},
          "mov x17, #0xfffffffffffffef8 // #-264; ldr x0, [x29, x17]"},
      {"ldr x0, [x29, #-4096] (33 registers on: x17)", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -4096); },
          {0xF1, 0xFF, 0x81, 0x92, 0xA0, 0x6B, 0x71, 0xF8},
          "mov x17, #0xfffffffffffff000 // #-4096; ldr x0, [x29, x17]"},
      {"ldr x0, [x29, #-40000]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -40000); },
          {0xF1, 0x87, 0x93, 0x92, 0xA0, 0x6B, 0x71, 0xF8},
          "mov x17, #0xffffffffffff63c0 // #-40000; ldr x0, [x29, x17]"},
      {"str x0, [x29, #-1048576] (1 MiB)", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X29, -1048576); },
          {0xF1, 0xFF, 0x9F, 0x92, 0x11, 0xFE, 0xBF, 0xF2, 0xA0, 0x6B, 0x31, 0xF8},
          "mov x17, #0xffffffffffff0000 // #-65536; movk x17, #0xfff0, lsl #16; str x0, [x29, x17]"},
      {"ldr x0, [x1, #32768] (one past scaled)", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 32768); },
          {0x11, 0x00, 0x90, 0xD2, 0x20, 0x68, 0x71, 0xF8},
          "mov x17, #0x8000 // #32768; ldr x0, [x1, x17]"},
      {"ldr x0, [x1, #260]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U32, GRJIT_A64_X0, GRJIT_A64_X1, 260); },
          {0x20, 0x04, 0x41, 0xB9},
          "ldr w0, [x1, #260]"},
      {"ldr x0, [x1, #257] (unaligned, past ldur)", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, 257); },
          {0x31, 0x20, 0x80, 0xD2, 0x20, 0x68, 0x71, 0xF8},
          "mov x17, #0x101 // #257; ldr x0, [x1, x17]"},
      {"ldrb w0, [x1, #4096] (one past scaled)", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, 4096); },
          {0x11, 0x00, 0x82, 0xD2, 0x20, 0x68, 0x71, 0x38},
          "mov x17, #0x1000 // #4096; ldrb w0, [x1, x17]"},
      {"ldrsw x0, [x1, INT32_MAX]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_S32, GRJIT_A64_X0, GRJIT_A64_X1, INT32_MAX); },
          {0xF1, 0xFF, 0x9F, 0xD2, 0xF1, 0xFF, 0xAF, 0xF2, 0x20, 0x68, 0xB1, 0xB8},
          "mov x17, #0xffff // #65535; movk x17, #0x7fff, lsl #16; ldrsw x0, [x1, x17]"},
      {"ldr x0, [x1, INT32_MIN]", [](A a) { grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_X1, INT32_MIN); },
          {0xF1, 0xFF, 0x9F, 0x92, 0x11, 0x00, 0xB0, 0xF2, 0x20, 0x68, 0x71, 0xF8},
          "mov x17, #0xffffffffffff0000 // #-65536; movk x17, #0x8000, lsl #16; ldr x0, [x1, x17]"},
      {"strb w0, [x1, -100000]", [](A a) { grjit_a64_store(a, GRJIT_A64_MEM_U8, GRJIT_A64_X0, GRJIT_A64_X1, -100000); },
          {0xF1, 0xD3, 0x90, 0x92, 0xD1, 0xFF, 0xBF, 0xF2, 0x20, 0x68, 0x31, 0x38},
          "mov x17, #0xffffffffffff7960 // #-34464; movk x17, #0xfffe, lsl #16; strb w0, [x1, x17]"},
      {"stp x29, x30, [sp, #-16]!", [](A a) { grjit_a64_push_frame_record(a); },
          {0xFD, 0x7B, 0xBF, 0xA9},
          "stp x29, x30, [sp, #-16]!"},
      {"ldp x29, x30, [sp], #16", [](A a) { grjit_a64_pop_frame_record(a); },
          {0xFD, 0x7B, 0xC1, 0xA8},
          "ldp x29, x30, [sp], #16"},
      {"ret", [](A a) { grjit_a64_ret(a); },
          {0xC0, 0x03, 0x5F, 0xD6},
          "ret"},
      {"blr x16", [](A a) { grjit_a64_blr(a, GRJIT_A64_X16); },
          {0x00, 0x02, 0x3F, 0xD6},
          "blr x16"},
      {"blr x0", [](A a) { grjit_a64_blr(a, GRJIT_A64_X0); },
          {0x00, 0x00, 0x3F, 0xD6},
          "blr x0"},
      {"brk #0", [](A a) { grjit_a64_brk(a, 0); },
          {0x00, 0x00, 0x20, 0xD4},
          "brk #0x0"},
      {"brk #0x1234", [](A a) { grjit_a64_brk(a, 0x1234); },
          {0x80, 0x46, 0x22, 0xD4},
          "brk #0x1234"},
      {"br x16", [](A a) { grjit_a64_br(a, GRJIT_A64_X16); },
          {0x00, 0x02, 0x1F, 0xD6},
          "br x16"},
      {"br x0", [](A a) { grjit_a64_br(a, GRJIT_A64_X0); },
          {0x00, 0x00, 0x1F, 0xD6},
          "br x0"},
      {"cmp x16, #1", [](A a) { grjit_a64_cmp_imm(a, GRJIT_A64_X16, 1); },
          {0x1F, 0x06, 0x00, 0xF1},
          "cmp x16, #0x1"},
      {"cmp x0, #4095", [](A a) { grjit_a64_cmp_imm(a, GRJIT_A64_X0, 4095); },
          {0x1F, 0xFC, 0x3F, 0xF1},
          "cmp x0, #0xfff"},
      {"adr x16, the instruction after next (a label ahead, bound)", [](A a) {
            GRJIT_Label l = grjit_a64_label(a);
            grjit_a64_adr(a, GRJIT_A64_X16, l);
            grjit_a64_word(a, 0xD503201Fu); // nop
            grjit_a64_bind(a, l);
            grjit_a64_finish(a);
          },
          {0x50, 0x00, 0x00, 0x10, 0x1F, 0x20, 0x03, 0xD5},
          "adr x16, 0x8; nop"},
      {"adr x16, the previous instruction (a label behind)", [](A a) {
            GRJIT_Label l = grjit_a64_label(a);
            grjit_a64_bind(a, l);
            grjit_a64_word(a, 0xD503201Fu); // nop
            grjit_a64_adr(a, GRJIT_A64_X16, l);
          },
          {0x1F, 0x20, 0x03, 0xD5, 0xF0, 0xFF, 0xFF, 0x10},
          "nop; adr x16, 0x0"},
      {"adrp and add to a label ahead (long mode, the label 12 bytes on)", [](A a) {
            grjit_a64_set_long_branches(a, true);
            GRJIT_Label l = grjit_a64_label(a);
            grjit_a64_adr(a, GRJIT_A64_X16, l);
            grjit_a64_word(a, 0xD503201Fu); // nop
            grjit_a64_bind(a, l);
            grjit_a64_finish(a);
          },
          {0x10, 0x00, 0x00, 0x90, 0x10, 0x32, 0x00, 0x91, 0x1F, 0x20, 0x03, 0xD5},
          "adrp x16, 0x0; add x16, x16, #0xc; nop"},
  };
}

struct Asm {
  GRJIT_A64Asm a;
  explicit Asm(size_t limit = 1u << 24) { grjit_a64_init(&a, grjit_allocator_default(), limit); }
  ~Asm() { grjit_a64_free(&a); }
  std::vector<uint8_t> bytes() const {
    return std::vector<uint8_t>(grjit_a64_bytes(&a), grjit_a64_bytes(&a) + grjit_a64_size(&a));
  }
  uint32_t word(size_t i) const {
    uint32_t w;
    std::memcpy(&w, grjit_a64_bytes(&a) + 4 * i, 4);
    return w;
  }
  size_t words() const { return grjit_a64_size(&a) / 4; }
};

std::string hex(const std::vector<uint8_t> & b) {
  std::string s;
  char buf[4];
  for (uint8_t x : b) {
    std::snprintf(buf, sizeof buf, "%02x", x);
    s += buf;
  }
  return s;
}

} // namespace

TEST(AsmArm64, EveryInstructionIsTheBytesTheReferenceAssemblerProduces) {
  for (const Case & c : cases()) {
    Asm as;
    c.emit(&as.a);
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK) << c.name;
    EXPECT_EQ(hex(as.bytes()), hex(c.bytes)) << c.name;
  }
}

TEST(AsmArm64, EveryInstructionIsOneLittleEndianWordOrAWholeNumberOfThem) {
  for (const Case & c : cases()) {
    EXPECT_EQ(c.bytes.size() % 4, 0u) << c.name;
    EXPECT_GT(c.bytes.size(), 0u) << c.name;
  }
}

TEST(AsmArm64, TheRecordedDisassemblyAgreesWithObjdumpWhenTheToolIsPresent) {
  const char * tool = std::getenv("GRJIT_AARCH64_OBJDUMP");
  if (tool == nullptr || *tool == 0) {
    GTEST_SKIP() << "GRJIT_AARCH64_OBJDUMP is not set: the bytes are checked "
                    "against the recorded reference, the disassembly only "
                    "where tools/xarch/jit-arm64.sh sets it";
  }
  size_t checked = 0;
  for (const Case & c : cases()) {
    Asm as;
    c.emit(&as.a);
    char path[] = "/tmp/grjit-a64-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ssize_t n = write(fd, grjit_a64_bytes(&as.a), grjit_a64_size(&as.a));
    close(fd);
    ASSERT_EQ(n, static_cast<ssize_t>(grjit_a64_size(&as.a)));
    std::string cmd = std::string(tool) + " -D -b binary -m aarch64 --no-show-raw-insn " + path;
    FILE * p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    std::string out;
    char line[512];
    while (std::fgets(line, sizeof line, p) != nullptr) {
      std::string l(line);
      auto colon = l.find(":\t");
      if (colon == std::string::npos || l.back() == ':') {
        continue;
      }
      std::string t = l.substr(colon + 2);
      std::string norm;
      bool space = false;
      for (char ch : t) {
        if (ch == ' ' || ch == '\t' || ch == '\n') {
          space = true;
        } else {
          if (space && !norm.empty()) {
            norm += ' ';
          }
          space = false;
          norm += ch;
        }
      }
      if (!out.empty()) {
        out += "; ";
      }
      out += norm;
    }
    int rc = pclose(p);
    unlink(path);
    EXPECT_EQ(rc, 0) << c.name;
    EXPECT_EQ(out, c.text) << c.name;
    checked++;
  }
  EXPECT_EQ(checked, cases().size());
}

/* ---- Immediates --------------------------------------------------------------- */

TEST(AsmArm64, AnImmediateTakesAsManyInstructionsAsItHasPiecesThatDifferFromTheStart) {
  struct Shape {
    uint64_t value;
    unsigned count;
  };
  const Shape shapes[] = {
      {0, 1}, {1, 1}, {0xFFFF, 1}, {0x10000, 1}, {UINT64_C(0x100000000), 1},
      {UINT64_C(0x100000001), 2}, {UINT64_C(0x1111000000000001), 2},
      {UINT64_C(0x0001000200030000), 3}, {UINT64_C(0x123456789abcdef0), 4},
      {~UINT64_C(0), 1}, {~UINT64_C(1), 1}, {UINT64_C(0xffffffffffff0000), 1},
      {UINT64_C(0xffff0000ffff1234), 2}, {UINT64_C(0x8000000000000000), 1},
      {UINT64_C(0x7fffffffffffffff), 1}, {UINT64_C(0x00007f1234567890), 3},
      {UINT64_C(0xfedcba9876543210), 4}, {UINT64_C(0xffffffff00000000), 2},
      {UINT64_C(0xffffffff12345678), 2}, {UINT64_C(0x0000ffff0000ffff), 2}};
  for (const Shape & s : shapes) {
    Asm as;
    grjit_a64_mov_ri(&as.a, GRJIT_A64_X0, s.value);
    EXPECT_EQ(as.words(), s.count) << std::hex << s.value;
    EXPECT_EQ(grjit_a64_mov_ri_count(s.value), s.count) << std::hex << s.value;
    // Four instructions is the most any 64-bit value needs.
    EXPECT_LE(as.words(), 4u);
  }
}

TEST(AsmArm64, SubSpAndAddSpAreAMultipleOfSixteenAtEveryStepAndHaveNoSizeTheyRefuse) {
  for (uint32_t bytes : {16u, 32u, 4080u, 4096u, 4112u, 65536u, 1u << 20, (1u << 20) + 16, (1u << 24) - 16}) {
    Asm as;
    grjit_a64_sub_sp(&as.a, bytes);
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK) << bytes;
    uint64_t total = 0;
    for (size_t i = 0; i < as.words(); i++) {
      uint32_t w = as.word(i);
      ASSERT_EQ(w & 0xFF8003FFu, 0xD10003FFu) << "not sub sp, sp, #imm";
      uint32_t imm = (w >> 10) & 0xFFF;
      total += (w & (1u << 22)) ? uint64_t{imm} << 12 : imm;
      EXPECT_EQ(total % 16, 0u) << "sp is misaligned between the steps";
    }
    EXPECT_EQ(total, bytes);
  }
  // From 16 MiB the amount is in x17 and sp moves once, so it is never misaligned and never refused.
  for (uint32_t bytes : {1u << 24, (1u << 24) + 16, 1u << 26, 1u << 29, 1u << 30, (1u << 30) + 4096}) {
    for (int add = 0; add < 2; add++) {
      Asm as;
      (add ? grjit_a64_add_sp : grjit_a64_sub_sp)(&as.a, bytes);
      EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK) << bytes;
      ASSERT_GE(as.words(), 2u);
      const uint32_t last = as.word(as.words() - 1);
      EXPECT_EQ(last, (add ? 0x8B3163FFu : 0xCB3163FFu)) << "the last word moves sp by x17, once";
      // The words before it build the amount in x17 (movz then movk's) and nothing else.
      uint64_t value = 0;
      for (size_t i = 0; i + 1 < as.words(); i++) {
        const uint32_t w = as.word(i);
        ASSERT_EQ(w & 0x1F, 17u) << "writes only x17";
        const unsigned hw = (w >> 21) & 3;
        const uint64_t imm16 = (w >> 5) & 0xFFFF;
        if ((w & 0xFF800000u) == 0xD2800000u) {
          value = imm16 << (16 * hw);
        } else {
          ASSERT_EQ(w & 0xFF800000u, 0xF2800000u) << "movz or movk";
          value = (value & ~(UINT64_C(0xFFFF) << (16 * hw))) | (imm16 << (16 * hw));
        }
      }
      EXPECT_EQ(value, bytes);
    }
  }
}

TEST(AsmArm64, AMemoryOffsetThatFitsNoImmediateGoesThroughX17AndNeverTruncates) {
  // Around every boundary of the three forms, for each width.
  for (GRJIT_A64Mem kind : {GRJIT_A64_MEM_U8, GRJIT_A64_MEM_U16, GRJIT_A64_MEM_U32,
           GRJIT_A64_MEM_X64, GRJIT_A64_MEM_S8, GRJIT_A64_MEM_S16, GRJIT_A64_MEM_S32}) {
    for (int32_t disp : {0, 1, 2, 4, 8, 255, 256, 257, 4095, 4096, 32760, 32768,
             65536, 1 << 20, INT32_MAX, -1, -255, -256, -257, -264, -4096, -32768,
             -(1 << 20), INT32_MIN}) {
      Asm as;
      grjit_a64_load(&as.a, kind, GRJIT_A64_X0, GRJIT_A64_X1, disp);
      EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
      size_t w = as.words();
      ASSERT_GE(w, 1u);
      ASSERT_LE(w, 5u) << disp;
      uint32_t last = as.word(w - 1);
      if (w == 1) {
        // One instruction: an immediate form, the displacement in range of it.
        bool unscaled = (last & 0x3B200C00u) == 0x38000000u && disp >= -256 && disp <= 255;
        bool scaled = (last & 0x3B000000u) == 0x39000000u && disp >= 0;
        EXPECT_TRUE(unscaled || scaled) << disp;
      } else {
        // More than one: the register form, with x17 holding the offset.
        EXPECT_EQ((last >> 16) & 31u, 17u) << disp;
        EXPECT_EQ((last >> 10) & 3u, 2u) << "register offset form";
      }
    }
  }
}

/* ---- Branches ------------------------------------------------------------------- */

TEST(AsmArm64, AForwardBranchIsPatchedWhenItsLabelIsBound) {
  Asm as;
  GRJIT_Label l = grjit_a64_label(&as.a);
  grjit_a64_b(&as.a, l);       // 0
  grjit_a64_brk(&as.a, 1);     // 1
  grjit_a64_cbnz(&as.a, GRJIT_A64_X0, l); // 2
  grjit_a64_cbz(&as.a, GRJIT_A64_X5, l);  // 3
  grjit_a64_bcond(&as.a, GRJIT_A64_NE, l); // 4
  grjit_a64_bind(&as.a, l);    // 5
  grjit_a64_ret(&as.a);
  grjit_a64_finish(&as.a);
  ASSERT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
  EXPECT_EQ(as.word(0), 0x14000000u | 5u);
  EXPECT_EQ(as.word(2), 0xB5000000u | (3u << 5) | 0u);
  EXPECT_EQ(as.word(3), 0xB4000000u | (2u << 5) | 5u);
  EXPECT_EQ(as.word(4), 0x54000000u | (1u << 5) | 1u);
  EXPECT_EQ(grjit_a64_label_offset(&as.a, l), 20u);
}

TEST(AsmArm64, ABackwardBranchIsEncodedAtOnceWithItsNegativeDistance) {
  Asm as;
  GRJIT_Label l = grjit_a64_label(&as.a);
  grjit_a64_bind(&as.a, l);
  grjit_a64_brk(&as.a, 0);
  grjit_a64_b(&as.a, l);                   // -1 word
  grjit_a64_cbnz(&as.a, GRJIT_A64_X1, l);  // -2
  grjit_a64_bcond(&as.a, GRJIT_A64_LO, l); // -3
  grjit_a64_finish(&as.a);
  ASSERT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
  EXPECT_EQ(as.word(1), 0x14000000u | 0x03FFFFFFu);
  EXPECT_EQ(as.word(2), 0xB5000000u | ((0x7FFFFu - 1u) << 5) | 1u);
  EXPECT_EQ(as.word(3), 0x54000000u | ((0x7FFFFu - 2u) << 5) | 3u);
}

TEST(AsmArm64, LongModeInvertsTheTestOverTheNextInstructionThenJumps) {
  Asm as;
  grjit_a64_set_long_branches(&as.a, true);
  GRJIT_Label l = grjit_a64_label(&as.a);
  grjit_a64_cbnz(&as.a, GRJIT_A64_X0, l);  // cbz x0, +8 ; b L
  grjit_a64_cbz(&as.a, GRJIT_A64_X3, l);   // cbnz x3, +8 ; b L
  grjit_a64_bcond(&as.a, GRJIT_A64_GE, l); // b.lt +8 ; b L
  grjit_a64_bind(&as.a, l);
  grjit_a64_ret(&as.a);
  grjit_a64_finish(&as.a);
  ASSERT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
  EXPECT_EQ(as.word(0), 0xB4000000u | (2u << 5) | 0u);
  EXPECT_EQ(as.word(1), 0x14000000u | 5u);
  EXPECT_EQ(as.word(2), 0xB5000000u | (2u << 5) | 3u);
  EXPECT_EQ(as.word(3), 0x14000000u | 3u);
  EXPECT_EQ(as.word(4), 0x54000000u | (2u << 5) | GRJIT_A64_LT);
  EXPECT_EQ(as.word(5), 0x14000000u | 1u);
}

TEST(AsmArm64, AShortForwardBranchThatCannotReachIsFarAndLongModeFixesIt) {
  for (bool long_mode : {false, true}) {
    Asm as(1u << 26);
    grjit_a64_set_long_branches(&as.a, long_mode);
    GRJIT_Label l = grjit_a64_label(&as.a);
    grjit_a64_cbnz(&as.a, GRJIT_A64_X0, l);
    // Just over 1 MiB of code between the branch and its label.
    for (size_t i = 0; i < (1u << 18) + 2; i++) {
      grjit_a64_brk(&as.a, 0);
    }
    grjit_a64_bind(&as.a, l);
    grjit_a64_ret(&as.a);
    grjit_a64_finish(&as.a);
    if (long_mode) {
      EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
    } else {
      EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_FAR);
    }
  }
}

TEST(AsmArm64, TheShortFormReachesExactlyTheLastWordOfItsRange) {
  // A forward cbnz reaches +(2^18 - 1) words and no further.
  for (int64_t extra : {0, 1}) {
    Asm as(1u << 26);
    GRJIT_Label l = grjit_a64_label(&as.a);
    grjit_a64_cbnz(&as.a, GRJIT_A64_X0, l);
    for (int64_t i = 0; i < (int64_t{1} << 18) - 2 + extra; i++) {
      grjit_a64_brk(&as.a, 0);
    }
    grjit_a64_bind(&as.a, l);
    grjit_a64_finish(&as.a);
    EXPECT_EQ(grjit_a64_status(&as.a), extra == 0 ? GRJIT_A64_OK : GRJIT_A64_FAR) << extra;
  }
}

TEST(AsmArm64, ABackwardConditionalBranchOutOfShortRangeIsEmittedInTheLongFormAtOnce) {
  Asm as(1u << 26);
  GRJIT_Label l = grjit_a64_label(&as.a);
  grjit_a64_bind(&as.a, l);
  for (size_t i = 0; i < (1u << 18) + 4; i++) {
    grjit_a64_brk(&as.a, 0);
  }
  size_t at = as.words();
  grjit_a64_cbnz(&as.a, GRJIT_A64_X2, l);
  grjit_a64_finish(&as.a);
  ASSERT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
  ASSERT_EQ(as.words(), at + 2);
  EXPECT_EQ(as.word(at), 0xB4000000u | (2u << 5) | 2u);          // cbz x2, +8
  uint32_t b = as.word(at + 1);                                    // b L
  EXPECT_EQ(b & 0xFC000000u, 0x14000000u);
  int32_t imm = static_cast<int32_t>(b << 6) >> 6;
  EXPECT_EQ(static_cast<int64_t>(at + 1) + imm, 0);
}

TEST(AsmArm64, ABranchPatchRefusesADistanceTheInstructionCannotEncode) {
  // Fabricate the state finish() reads, so a 128 MiB distance needs no 128 MiB
  // of code: one fixup, one label at a made-up offset.
  struct Probe {
    int64_t words;
    GRJIT_A64FixupKind kind;
    GRJIT_A64Status want;
  };
  const int64_t b_range = int64_t{1} << 25;
  const int64_t c_range = int64_t{1} << 18;
  const Probe probes[] = {
      {b_range - 1, GRJIT_A64_FIXUP_B, GRJIT_A64_OK},
      {b_range, GRJIT_A64_FIXUP_B, GRJIT_A64_LIMIT},
      {-b_range, GRJIT_A64_FIXUP_B, GRJIT_A64_OK},
      {-b_range - 1, GRJIT_A64_FIXUP_B, GRJIT_A64_LIMIT},
      {c_range - 1, GRJIT_A64_FIXUP_COND, GRJIT_A64_OK},
      {c_range, GRJIT_A64_FIXUP_COND, GRJIT_A64_FAR},
      {-c_range, GRJIT_A64_FIXUP_COND, GRJIT_A64_OK},
      {-c_range - 1, GRJIT_A64_FIXUP_COND, GRJIT_A64_FAR},
  };
  for (const Probe & p : probes) {
    GRJIT_A64Asm a;
    grjit_a64_init(&a, grjit_allocator_default(), SIZE_MAX / 2);
    // The instruction sits at offset 4 * 2^26 so a negative distance still lands
    // at a non-negative offset.
    const size_t at = 4u << 26;
    uint8_t insn[4] = {0, 0, 0, 0};
    a.buffer = static_cast<uint8_t *>(std::calloc(1, at + 8));
    std::memcpy(a.buffer + at, insn, 4);
    a.length = at + 4;
    a.capacity = at + 8;
    GRJIT_Label l = grjit_a64_label(&a);
    a.label_at[l] = static_cast<size_t>(static_cast<int64_t>(at) + 4 * p.words);
    a.fixups = static_cast<GRJIT_A64Fixup *>(std::malloc(sizeof(GRJIT_A64Fixup)));
    a.fixups[0] = {at, l, p.kind};
    a.fixup_count = 1;
    a.fixup_capacity = 1;
    grjit_a64_finish(&a);
    EXPECT_EQ(grjit_a64_status(&a), p.want) << p.words;
    grjit_a64_free(&a);
  }
}

TEST(AsmArm64, AnUnboundLabelAndABadOperandAreBadAndTheCapIsALimit) {
  {
    Asm as;
    GRJIT_Label l = grjit_a64_label(&as.a);
    grjit_a64_b(&as.a, l);
    grjit_a64_finish(&as.a);
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_BAD);
  }
  {
    Asm as;
    grjit_a64_b(&as.a, 12345); // a label that was never made
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_BAD);
  }
  {
    Asm as;
    grjit_a64_add_imm(&as.a, GRJIT_A64_X0, GRJIT_A64_X1, 4096, false);
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_BAD);
    EXPECT_EQ(as.words(), 0u);
  }
  {
    Asm as(8);
    grjit_a64_ret(&as.a);
    grjit_a64_ret(&as.a);
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
    grjit_a64_ret(&as.a);
    EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_LIMIT);
    EXPECT_EQ(as.words(), 2u);
    grjit_a64_ret(&as.a); // a failed assembler is a no-op
    EXPECT_EQ(as.words(), 2u);
  }
}

TEST(AsmArm64, EveryAllocationOfTheAssemblerCanFailAndIsReportedAsOom) {
  for (long n = 1; n <= 6; n++) {
    TrackingAllocator t;
    t.fail_at = n;
    GRJIT_A64Asm a;
    grjit_a64_init(&a, t.get(), 1 << 20);
    GRJIT_Label l = grjit_a64_label(&a);
    for (int i = 0; i < 3; i++) {
      grjit_a64_cbnz(&a, GRJIT_A64_X0, l);
    }
    grjit_a64_bind(&a, l);
    grjit_a64_finish(&a);
    bool failed = grjit_a64_status(&a) != GRJIT_A64_OK;
    if (failed) {
      EXPECT_EQ(grjit_a64_status(&a), GRJIT_A64_OOM) << n;
    }
    grjit_a64_free(&a);
    EXPECT_EQ(t.live, 0) << n;
  }
}

/* ---- adr ------------------------------------------------------------------------ */

TEST(AsmArm64, AdrReachesExactlyAMebibyteEitherWayThenIsAdrpAndAdd) {
  const int64_t reach = int64_t{1} << 20;
  // Behind: the label is bound, `bytes` before the adr. -1 MiB reaches, one word more does not.
  for (int64_t behind : {int64_t{4}, reach, reach + 4}) {
    Asm as(1u << 26);
    GRJIT_Label l = grjit_a64_label(&as.a);
    grjit_a64_bind(&as.a, l);
    for (int64_t i = 0; i < behind / 4; i++) {
      grjit_a64_brk(&as.a, 0);
    }
    const size_t at = as.words();
    grjit_a64_adr(&as.a, GRJIT_A64_X16, l);
    grjit_a64_finish(&as.a);
    ASSERT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK) << behind;
    if (behind <= reach) {
      ASSERT_EQ(as.words(), at + 1) << behind << ": one instruction while it reaches";
      const uint32_t w = as.word(at);
      EXPECT_EQ(w & 0x9F000000u, 0x10000000u) << "an adr, not an adrp";
      const int64_t imm = (static_cast<int64_t>((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
      const int64_t signed_imm = (imm ^ (int64_t{1} << 20)) - (int64_t{1} << 20);
      EXPECT_EQ(signed_imm, -behind);
    } else {
      ASSERT_EQ(as.words(), at + 2) << behind << ": adrp and add when it does not";
      const uint32_t p = as.word(at);
      const uint32_t add = as.word(at + 1);
      EXPECT_EQ(p & 0x9F000000u, 0x90000000u) << "an adrp";
      EXPECT_EQ(add & 0xFFC00000u, 0x91000000u) << "then an add of the low twelve bits";
      EXPECT_EQ(add & 31u, 16u);
      EXPECT_EQ((add >> 5) & 31u, 16u);
      const int64_t pages = (static_cast<int64_t>((p >> 5) & 0x7FFFFu) << 2) | ((p >> 29) & 3u);
      const int64_t signed_pages = (pages ^ (int64_t{1} << 20)) - (int64_t{1} << 20);
      EXPECT_EQ(signed_pages, -static_cast<int64_t>(at >> 10) + 0) << "the label is at page 0 (the code's own start)";
      EXPECT_EQ((add >> 10) & 0xFFFu, 0u);
    }
  }
}

TEST(AsmArm64, AForwardAdrIsFarWhenItsLabelIsOutOfReachAndLongModeEmitsAdrpAndAdd) {
  const int64_t reach = int64_t{1} << 20;
  // The label is `ahead` bytes past the adr. +1 MiB - 4 reaches; +1 MiB does not.
  for (int64_t ahead : {int64_t{8}, reach - 4, reach}) {
    for (bool long_mode : {false, true}) {
      SCOPED_TRACE(ahead);
      SCOPED_TRACE(long_mode);
      Asm as(1u << 26);
      grjit_a64_set_long_branches(&as.a, long_mode);
      GRJIT_Label l = grjit_a64_label(&as.a);
      grjit_a64_adr(&as.a, GRJIT_A64_X9, l);
      const size_t adr_words = long_mode ? 2 : 1;
      for (int64_t i = 0; i < (ahead - 4 * static_cast<int64_t>(adr_words)) / 4; i++) {
        grjit_a64_brk(&as.a, 0);
      }
      grjit_a64_bind(&as.a, l);
      grjit_a64_ret(&as.a);
      grjit_a64_finish(&as.a);
      if (!long_mode && ahead >= reach) {
        EXPECT_EQ(grjit_a64_status(&as.a), GRJIT_A64_FAR) << "assemble again in long mode";
        continue;
      }
      ASSERT_EQ(grjit_a64_status(&as.a), GRJIT_A64_OK);
      const size_t target = grjit_a64_label_offset(&as.a, l);
      const uint32_t w = as.word(0);
      if (!long_mode) {
        EXPECT_EQ(w & 0x9F000000u, 0x10000000u);
        const int64_t imm = (static_cast<int64_t>((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
        EXPECT_EQ(imm, static_cast<int64_t>(target)) << "the offset to the label from the adr at 0";
      } else {
        const uint32_t add = as.word(1);
        EXPECT_EQ(w & 0x9F000000u, 0x90000000u);
        EXPECT_EQ(add & 0xFFC00000u, 0x91000000u);
        const int64_t pages = (static_cast<int64_t>((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
        EXPECT_EQ(pages, static_cast<int64_t>(target >> 12));
        EXPECT_EQ((add >> 10) & 0xFFFu, static_cast<uint32_t>(target & 0xFFFu));
      }
    }
  }
}

TEST(AsmArm64, AnAdrFixupPatchesItsOwnFieldsAndRefusesWhatItCannotEncode) {
  struct Probe {
    int64_t delta;  // bytes for adr, pages for adrp, the target's low twelve bits for the add
    GRJIT_A64FixupKind kind;
    GRJIT_A64Status want;
  };
  const int64_t r = int64_t{1} << 20;
  const Probe probes[] = {
      {r - 4, GRJIT_A64_FIXUP_ADR, GRJIT_A64_OK},
      {r, GRJIT_A64_FIXUP_ADR, GRJIT_A64_FAR},
      {-r, GRJIT_A64_FIXUP_ADR, GRJIT_A64_OK},
      {-r - 4, GRJIT_A64_FIXUP_ADR, GRJIT_A64_FAR},
      {1, GRJIT_A64_FIXUP_ADR, GRJIT_A64_OK},
      {3, GRJIT_A64_FIXUP_ADR, GRJIT_A64_OK},
      {r - 1, GRJIT_A64_FIXUP_ADRP, GRJIT_A64_OK},
      {r, GRJIT_A64_FIXUP_ADRP, GRJIT_A64_LIMIT},
      {1, GRJIT_A64_FIXUP_ADRP, GRJIT_A64_OK},
      {-1, GRJIT_A64_FIXUP_ADRP, GRJIT_A64_OK},
      {0x123, GRJIT_A64_FIXUP_LO12, GRJIT_A64_OK},
      {0xFFF, GRJIT_A64_FIXUP_LO12, GRJIT_A64_OK},
  };
  for (const Probe & p : probes) {
    SCOPED_TRACE(p.delta);
    SCOPED_TRACE(p.kind);
    GRJIT_A64Asm a;
    grjit_a64_init(&a, grjit_allocator_default(), SIZE_MAX / 2);
    // The instruction sits 8 MiB up, so a distance behind it lands at a non-negative offset. (An adrp
    // 2^20 pages behind would need a 4 GiB buffer: the probes behind stop at one page.)
    const size_t at = size_t{8} << 20;
    a.buffer = static_cast<uint8_t *>(std::calloc(1, at + 8));
    ASSERT_NE(a.buffer, nullptr);
    a.length = at + 4;
    a.capacity = at + 8;
    GRJIT_Label l = grjit_a64_label(&a);
    int64_t target = static_cast<int64_t>(at);
    switch (p.kind) {
      case GRJIT_A64_FIXUP_ADR: target += p.delta; break;
      case GRJIT_A64_FIXUP_ADRP: target += p.delta * 4096; break;
      default: target += p.delta; break;
    }
    a.label_at[l] = static_cast<size_t>(target);
    a.fixups = static_cast<GRJIT_A64Fixup *>(std::malloc(sizeof(GRJIT_A64Fixup)));
    a.fixups[0] = {at, l, p.kind};
    a.fixup_count = 1;
    a.fixup_capacity = 1;
    grjit_a64_finish(&a);
    EXPECT_EQ(grjit_a64_status(&a), p.want);
    if (p.want == GRJIT_A64_OK) {
      uint32_t w;
      std::memcpy(&w, a.buffer + at, 4);
      if (p.kind == GRJIT_A64_FIXUP_LO12) {
        EXPECT_EQ((w >> 10) & 0xFFFu, static_cast<uint32_t>(target & 0xFFF));
        EXPECT_EQ(w & ~(0xFFFu << 10), 0u) << "nothing else is touched";
      } else {
        int64_t imm = (static_cast<int64_t>((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
        imm = (imm ^ (int64_t{1} << 20)) - (int64_t{1} << 20);
        EXPECT_EQ(imm, p.delta);
        EXPECT_EQ(w & ~((3u << 29) | (0x7FFFFu << 5)), 0u) << "nothing else is touched";
      }
    }
    grjit_a64_free(&a);
  }
}

TEST(AsmArm64, TheTagBetweenCodeIsTwoLittleEndianWordsOfTheValue) {
  Asm as;
  grjit_a64_data64(&as.a, UINT64_C(0x4752494E00000003));
  grjit_a64_data64(&as.a, UINT64_C(0x0123456789ABCDEF));
  EXPECT_EQ(hex(as.bytes()), "030000004e495247efcdab8967452301");
}

GRJIT_TEST_MAIN()
