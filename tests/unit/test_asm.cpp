/**
 * @file
 *
 * The assembler: encodings against reference byte sequences written out here
 * (from the Intel SDM), against the system disassembler where there is one,
 * and the label, fixup and failure behaviour.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/x86_64/asm_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

struct Case {
  const char * name;
  std::function<void(GRJIT_Asm *)> emit;
  std::vector<uint8_t> bytes;
  const char * text; /* objdump -M intel, whitespace collapsed */
};

std::vector<Case> cases() {
  using A = GRJIT_Asm *;
  return {
      {"mov rax, rcx", [](A a) { grjit_asm_mov_rr(a, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x89, 0xC8}, "mov rax,rcx"},
      {"mov r8, r15", [](A a) { grjit_asm_mov_rr(a, GRJIT_R8, GRJIT_R15); },
          {0x4D, 0x89, 0xF8}, "mov r8,r15"},
      {"mov rbp, rsp", [](A a) { grjit_asm_mov_rr(a, GRJIT_RBP, GRJIT_RSP); },
          {0x48, 0x89, 0xE5}, "mov rbp,rsp"},
      {"mov eax, eax", [](A a) { grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX); },
          {0x89, 0xC0}, "mov eax,eax"},
      {"mov r9d, r10d", [](A a) { grjit_asm_mov32_rr(a, GRJIT_R9, GRJIT_R10); },
          {0x45, 0x89, 0xD1}, "mov r9d,r10d"},
      {"mov rax, imm64", [](A a) { grjit_asm_mov_ri64(a, GRJIT_RAX, 0x1122334455667788ull); },
          {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
          "movabs rax,0x1122334455667788"},
      {"mov r9, imm64", [](A a) { grjit_asm_mov_ri64(a, GRJIT_R9, 0x1122334455667788ull); },
          {0x49, 0xB9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11},
          "movabs r9,0x1122334455667788"},
      {"mov eax, 5 (short form)", [](A a) { grjit_asm_mov_ri(a, GRJIT_RAX, 5); },
          {0xB8, 0x05, 0x00, 0x00, 0x00}, "mov eax,0x5"},
      {"mov r10d, 5", [](A a) { grjit_asm_mov_ri(a, GRJIT_R10, 5); },
          {0x41, 0xBA, 0x05, 0x00, 0x00, 0x00}, "mov r10d,0x5"},
      {"mov rax, -1 (sign-extended imm32)", [](A a) { grjit_asm_mov_ri(a, GRJIT_RAX, ~uint64_t{0}); },
          {0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}, "mov rax,0xffffffffffffffff"},
      {"mov rax, big via mov_ri", [](A a) { grjit_asm_mov_ri(a, GRJIT_RAX, 0x100000000ull); },
          {0x48, 0xB8, 0, 0, 0, 0, 1, 0, 0, 0}, "movabs rax,0x100000000"},
      {"mov rax, [rbp-8] (disp8)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, -8); },
          {0x48, 0x8B, 0x45, 0xF8}, "mov rax,QWORD PTR [rbp-0x8]"},
      {"mov rax, [rbp-256] (disp32)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, -256); },
          {0x48, 0x8B, 0x85, 0x00, 0xFF, 0xFF, 0xFF}, "mov rax,QWORD PTR [rbp-0x100]"},
      {"mov rax, [rbp+127] (largest disp8)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, 127); },
          {0x48, 0x8B, 0x45, 0x7F}, "mov rax,QWORD PTR [rbp+0x7f]"},
      {"mov rax, [rbp+128] (smallest disp32)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, 128); },
          {0x48, 0x8B, 0x85, 0x80, 0x00, 0x00, 0x00}, "mov rax,QWORD PTR [rbp+0x80]"},
      {"mov r8, [rbp-8] (r8 in the reg field)", [](A a) { grjit_asm_load64(a, GRJIT_R8, GRJIT_RBP, -8); },
          {0x4C, 0x8B, 0x45, 0xF8}, "mov r8,QWORD PTR [rbp-0x8]"},
      {"mov rax, [rcx] (no disp)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RCX, 0); },
          {0x48, 0x8B, 0x01}, "mov rax,QWORD PTR [rcx]"},
      {"mov rax, [r13+0] (rbp-like base needs a disp)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_R13, 0); },
          {0x49, 0x8B, 0x45, 0x00}, "mov rax,QWORD PTR [r13+0x0]"},
      {"mov rax, [rbp+0]", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, 0); },
          {0x48, 0x8B, 0x45, 0x00}, "mov rax,QWORD PTR [rbp+0x0]"},
      {"mov rax, [rsp+8] (rsp base needs a SIB)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_RSP, 8); },
          {0x48, 0x8B, 0x44, 0x24, 0x08}, "mov rax,QWORD PTR [rsp+0x8]"},
      {"mov rax, [r12] (r12 base needs a SIB)", [](A a) { grjit_asm_load64(a, GRJIT_RAX, GRJIT_R12, 0); },
          {0x49, 0x8B, 0x04, 0x24}, "mov rax,QWORD PTR [r12]"},
      {"mov [rbp-8], rax", [](A a) { grjit_asm_store64(a, GRJIT_RBP, -8, GRJIT_RAX); },
          {0x48, 0x89, 0x45, 0xF8}, "mov QWORD PTR [rbp-0x8],rax"},
      {"mov [rcx+0x100], r9", [](A a) { grjit_asm_store64(a, GRJIT_RCX, 0x100, GRJIT_R9); },
          {0x4C, 0x89, 0x89, 0x00, 0x01, 0x00, 0x00}, "mov QWORD PTR [rcx+0x100],r9"},
      {"movzx eax, byte [rcx+8]", [](A a) { grjit_asm_load8u(a, GRJIT_RAX, GRJIT_RCX, 8); },
          {0x0F, 0xB6, 0x41, 0x08}, "movzx eax,BYTE PTR [rcx+0x8]"},
      {"movzx eax, word [rcx+8]", [](A a) { grjit_asm_load16u(a, GRJIT_RAX, GRJIT_RCX, 8); },
          {0x0F, 0xB7, 0x41, 0x08}, "movzx eax,WORD PTR [rcx+0x8]"},
      {"mov eax, dword [rcx+8]", [](A a) { grjit_asm_load32u(a, GRJIT_RAX, GRJIT_RCX, 8); },
          {0x8B, 0x41, 0x08}, "mov eax,DWORD PTR [rcx+0x8]"},
      {"movsx rax, byte [rcx+8]", [](A a) { grjit_asm_load8s(a, GRJIT_RAX, GRJIT_RCX, 8); },
          {0x48, 0x0F, 0xBE, 0x41, 0x08}, "movsx rax,BYTE PTR [rcx+0x8]"},
      {"movsx rax, word [rcx+8]", [](A a) { grjit_asm_load16s(a, GRJIT_RAX, GRJIT_RCX, 8); },
          {0x48, 0x0F, 0xBF, 0x41, 0x08}, "movsx rax,WORD PTR [rcx+0x8]"},
      {"movsxd rax, dword [rcx+8]", [](A a) { grjit_asm_load32s(a, GRJIT_RAX, GRJIT_RCX, 8); },
          {0x48, 0x63, 0x41, 0x08}, "movsxd rax,DWORD PTR [rcx+0x8]"},
      {"mov [rcx+8], al", [](A a) { grjit_asm_store8(a, GRJIT_RCX, 8, GRJIT_RAX); },
          {0x88, 0x41, 0x08}, "mov BYTE PTR [rcx+0x8],al"},
      {"mov [rcx+8], sil (needs REX)", [](A a) { grjit_asm_store8(a, GRJIT_RCX, 8, GRJIT_RSI); },
          {0x40, 0x88, 0x71, 0x08}, "mov BYTE PTR [rcx+0x8],sil"},
      {"mov [rcx+8], r9b", [](A a) { grjit_asm_store8(a, GRJIT_RCX, 8, GRJIT_R9); },
          {0x44, 0x88, 0x49, 0x08}, "mov BYTE PTR [rcx+0x8],r9b"},
      {"mov [rcx+8], ax", [](A a) { grjit_asm_store16(a, GRJIT_RCX, 8, GRJIT_RAX); },
          {0x66, 0x89, 0x41, 0x08}, "mov WORD PTR [rcx+0x8],ax"},
      {"mov [rcx+8], eax", [](A a) { grjit_asm_store32(a, GRJIT_RCX, 8, GRJIT_RAX); },
          {0x89, 0x41, 0x08}, "mov DWORD PTR [rcx+0x8],eax"},
      {"add rax, rcx", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x01, 0xC8}, "add rax,rcx"},
      {"sub rax, rcx", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_SUB, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x29, 0xC8}, "sub rax,rcx"},
      {"and rax, rcx", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_AND, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x21, 0xC8}, "and rax,rcx"},
      {"or rax, rcx", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_OR, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x09, 0xC8}, "or rax,rcx"},
      {"xor rax, rcx", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x31, 0xC8}, "xor rax,rcx"},
      {"cmp rax, rcx", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_CMP, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x39, 0xC8}, "cmp rax,rcx"},
      {"add r11, r12", [](A a) { grjit_asm_alu_rr(a, GRJIT_ALU_ADD, GRJIT_R11, GRJIT_R12); },
          {0x4D, 0x01, 0xE3}, "add r11,r12"},
      {"imul rax, rcx", [](A a) { grjit_asm_imul_rr(a, GRJIT_RAX, GRJIT_RCX); },
          {0x48, 0x0F, 0xAF, 0xC1}, "imul rax,rcx"},
      {"neg rax", [](A a) { grjit_asm_neg(a, GRJIT_RAX); }, {0x48, 0xF7, 0xD8}, "neg rax"},
      {"not rax", [](A a) { grjit_asm_not(a, GRJIT_RAX); }, {0x48, 0xF7, 0xD0}, "not rax"},
      {"shl rax, cl", [](A a) { grjit_asm_shl_cl(a, GRJIT_RAX); }, {0x48, 0xD3, 0xE0}, "shl rax,cl"},
      {"shr rax, cl", [](A a) { grjit_asm_shr_cl(a, GRJIT_RAX); }, {0x48, 0xD3, 0xE8}, "shr rax,cl"},
      {"sar rax, cl", [](A a) { grjit_asm_sar_cl(a, GRJIT_RAX); }, {0x48, 0xD3, 0xF8}, "sar rax,cl"},
      {"test rax, rax", [](A a) { grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX); },
          {0x48, 0x85, 0xC0}, "test rax,rax"},
      {"sete al", [](A a) { grjit_asm_setcc(a, GRJIT_COND_E, GRJIT_RAX); },
          {0x0F, 0x94, 0xC0}, "sete al"},
      {"setl al", [](A a) { grjit_asm_setcc(a, GRJIT_COND_L, GRJIT_RAX); },
          {0x0F, 0x9C, 0xC0}, "setl al"},
      {"seta r9b", [](A a) { grjit_asm_setcc(a, GRJIT_COND_A, GRJIT_R9); },
          {0x41, 0x0F, 0x97, 0xC1}, "seta r9b"},
      {"movzx eax, al", [](A a) { grjit_asm_movzx_r8(a, GRJIT_RAX, GRJIT_RAX); },
          {0x0F, 0xB6, 0xC0}, "movzx eax,al"},
      {"push rbp", [](A a) { grjit_asm_push(a, GRJIT_RBP); }, {0x55}, "push rbp"},
      {"push r12", [](A a) { grjit_asm_push(a, GRJIT_R12); }, {0x41, 0x54}, "push r12"},
      {"pop rbp", [](A a) { grjit_asm_pop(a, GRJIT_RBP); }, {0x5D}, "pop rbp"},
      {"pop r15", [](A a) { grjit_asm_pop(a, GRJIT_R15); }, {0x41, 0x5F}, "pop r15"},
      {"sub rsp, 32", [](A a) { grjit_asm_sub_rsp(a, 32); }, {0x48, 0x83, 0xEC, 0x20}, "sub rsp,0x20"},
      {"sub rsp, 0x1000", [](A a) { grjit_asm_sub_rsp(a, 0x1000); },
          {0x48, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00}, "sub rsp,0x1000"},
      {"add rsp, 32", [](A a) { grjit_asm_add_rsp(a, 32); }, {0x48, 0x83, 0xC4, 0x20}, "add rsp,0x20"},
      {"call rax", [](A a) { grjit_asm_call_r(a, GRJIT_RAX); }, {0xFF, 0xD0}, "call rax"},
      {"call r11", [](A a) { grjit_asm_call_r(a, GRJIT_R11); }, {0x41, 0xFF, 0xD3}, "call r11"},
      {"jmp rax", [](A a) { grjit_asm_jmp_r(a, GRJIT_RAX); }, {0xFF, 0xE0}, "jmp rax"},
      {"jmp r11", [](A a) { grjit_asm_jmp_r(a, GRJIT_R11); }, {0x41, 0xFF, 0xE3}, "jmp r11"},
      {"ret", [](A a) { grjit_asm_ret(a); }, {0xC3}, "ret"},
      {"leave", [](A a) { grjit_asm_leave(a); }, {0xC9}, "leave"},
      {"ud2", [](A a) { grjit_asm_ud2(a); }, {0x0F, 0x0B}, "ud2"},
  };
}

std::string collapse(const std::string & s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      space = true;
    } else {
      if (space && !out.empty()) {
        out += ' ';
      }
      space = false;
      out += c;
    }
  }
  return out;
}

struct Asm {
  GRJIT_Asm a;
  explicit Asm(size_t limit = 1 << 20) {
    grjit_asm_init(&a, grjit_allocator_default(), limit);
  }
  ~Asm() { grjit_asm_free(&a); }
  std::vector<uint8_t> bytes() const {
    return std::vector<uint8_t>(grjit_asm_bytes(&a), grjit_asm_bytes(&a) + grjit_asm_size(&a));
  }
};

} // namespace

TEST(Asm, EveryInstructionEncodesToTheReferenceBytes) {
  for (const Case & c : cases()) {
    Asm as;
    c.emit(&as.a);
    EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_OK) << c.name;
    EXPECT_EQ(as.bytes(), c.bytes) << c.name;
  }
}

TEST(Asm, TheSystemDisassemblerReadsTheSameInstructions) {
#ifdef _WIN32
  // The test writes a temporary file, reads objdump's output through popen and
  // unlinks the file: POSIX calls, and an objdump the host may not have. The
  // encodings are still checked byte for byte by the test above.
  GTEST_SKIP() << "needs POSIX mkstemp/popen and a host objdump";
#endif
  // objdump is required on the gated target: its absence fails this test,
  // it is not skipped (a skipped differential reports success over nothing).
  char path[] = "/tmp/grjit-asm-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  std::vector<Case> all = cases();
  std::vector<uint8_t> blob;
  for (const Case & c : all) {
    Asm as;
    c.emit(&as.a);
    auto b = as.bytes();
    blob.insert(blob.end(), b.begin(), b.end());
  }
  ASSERT_EQ(write(fd, blob.data(), blob.size()), static_cast<ssize_t>(blob.size()));
  close(fd);
  std::string cmd = std::string("objdump -D -b binary -mi386:x86-64 -M intel ") + path + " 2>&1";
  FILE * p = popen(cmd.c_str(), "r");
  ASSERT_NE(p, nullptr);
  std::string text;
  char buf[512];
  while (fgets(buf, sizeof buf, p) != nullptr) {
    text += buf;
  }
  int status = pclose(p);
  unlink(path);
  ASSERT_EQ(status, 0) << "objdump is not available or failed; on the gated target that is a failure:\n"
                       << text;
  std::vector<std::string> lines;
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? text.size() : nl + 1;
    // "   0:\t48 89 c8             \tmov    rax,rcx"
    size_t colon = line.find(":\t");
    if (colon == std::string::npos || line.find("file format") != std::string::npos) {
      continue;
    }
    size_t second = line.find('\t', colon + 2);
    if (second == std::string::npos) {
      continue; // a continuation line of raw bytes
    }
    lines.push_back(collapse(line.substr(second + 1)));
  }
  ASSERT_EQ(lines.size(), all.size()) << text;
  for (size_t i = 0; i < all.size(); i++) {
    EXPECT_EQ(lines[i], all[i].text) << all[i].name;
  }
}

TEST(Asm, AForwardBranchIsPatchedWhenItsLabelIsBound) {
  Asm as;
  GRJIT_Label l = grjit_asm_label(&as.a);
  grjit_asm_jmp(&as.a, l);       // E9 rel32, patched
  grjit_asm_ud2(&as.a);          // 2 bytes skipped
  grjit_asm_bind(&as.a, l);
  grjit_asm_ret(&as.a);
  grjit_asm_finish(&as.a);
  EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_OK);
  EXPECT_EQ(as.bytes(), (std::vector<uint8_t>{0xE9, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x0B, 0xC3}));
  EXPECT_EQ(grjit_asm_label_offset(&as.a, l), 7u);
}

TEST(Asm, AForwardConditionalBranchUsesTheLongForm) {
  Asm as;
  GRJIT_Label l = grjit_asm_label(&as.a);
  grjit_asm_jcc(&as.a, GRJIT_COND_NE, l);
  grjit_asm_bind(&as.a, l);
  grjit_asm_finish(&as.a);
  EXPECT_EQ(as.bytes(), (std::vector<uint8_t>{0x0F, 0x85, 0x00, 0x00, 0x00, 0x00}));
}

TEST(Asm, ABackwardBranchWithinRel8IsShort) {
  Asm as;
  GRJIT_Label top = grjit_asm_label(&as.a);
  grjit_asm_bind(&as.a, top);
  grjit_asm_ud2(&as.a);
  grjit_asm_jmp(&as.a, top);              // EB rel8: -4
  grjit_asm_jcc(&as.a, GRJIT_COND_E, top); // 74 rel8: -6
  grjit_asm_finish(&as.a);
  EXPECT_EQ(as.bytes(), (std::vector<uint8_t>{0x0F, 0x0B, 0xEB, 0xFC, 0x74, 0xFA}));
}

TEST(Asm, ABranchAcrossMoreThanOneHundredTwentySevenBytesUsesRel32BothWays) {
  // Forward: a jmp over 200 bytes.
  {
    Asm as;
    GRJIT_Label end = grjit_asm_label(&as.a);
    grjit_asm_jmp(&as.a, end);
    for (int i = 0; i < 100; i++) {
      grjit_asm_ud2(&as.a); // 200 bytes
    }
    grjit_asm_bind(&as.a, end);
    grjit_asm_ret(&as.a);
    grjit_asm_finish(&as.a);
    auto b = as.bytes();
    ASSERT_EQ(b[0], 0xE9);
    uint32_t rel;
    std::memcpy(&rel, &b[1], 4);
    EXPECT_EQ(rel, 200u);
  }
  // Backward: a jmp back over 200 bytes cannot be rel8, so it is rel32.
  {
    Asm as;
    GRJIT_Label top = grjit_asm_label(&as.a);
    grjit_asm_bind(&as.a, top);
    for (int i = 0; i < 100; i++) {
      grjit_asm_ud2(&as.a);
    }
    grjit_asm_jmp(&as.a, top);
    grjit_asm_jcc(&as.a, GRJIT_COND_L, top);
    grjit_asm_finish(&as.a);
    auto b = as.bytes();
    ASSERT_EQ(b[200], 0xE9);
    int32_t rel;
    std::memcpy(&rel, &b[201], 4);
    EXPECT_EQ(rel, -205);
    ASSERT_EQ(b[205], 0x0F);
    ASSERT_EQ(b[206], 0x8C);
    std::memcpy(&rel, &b[207], 4);
    EXPECT_EQ(rel, -211);
  }
  // Exactly at the edge: -128 is still rel8, -129 is not.
  {
    Asm as;
    GRJIT_Label top = grjit_asm_label(&as.a);
    grjit_asm_bind(&as.a, top);
    for (int i = 0; i < 63; i++) {
      grjit_asm_ud2(&as.a); // 126 bytes
    }
    grjit_asm_jmp(&as.a, top); // end of the jmp is at 128: rel = -128
    EXPECT_EQ(grjit_asm_size(&as.a), 128u);
    grjit_asm_ud2(&as.a);      // now 130
    grjit_asm_jmp(&as.a, top); // rel = -(130 + 2) is past rel8
    EXPECT_EQ(grjit_asm_size(&as.a), 128u + 2 + 5);
  }
}

TEST(Asm, RawBytesAreAppendedAndCountAgainstTheCap) {
  Asm as(8);
  const uint8_t pad[4] = {0x90, 0x90, 0xCC, 0xCC};
  grjit_asm_raw(&as.a, pad, sizeof pad);
  grjit_asm_ret(&as.a);
  EXPECT_EQ(as.bytes(), (std::vector<uint8_t>{0x90, 0x90, 0xCC, 0xCC, 0xC3}));
  grjit_asm_raw(&as.a, pad, sizeof pad); // 9 bytes: past the cap of 8
  EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_LIMIT);
  EXPECT_EQ(grjit_asm_size(&as.a), 5u);
}

TEST(Asm, AnUnboundLabelFailsTheFinish) {
  Asm as;
  GRJIT_Label l = grjit_asm_label(&as.a);
  grjit_asm_jmp(&as.a, l);
  grjit_asm_finish(&as.a);
  EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_BAD);
  EXPECT_EQ(grjit_asm_label_offset(&as.a, 99), SIZE_MAX);
}

TEST(Asm, ManyLabelsAndFixupsGrowTheTables) {
  Asm as;
  std::vector<GRJIT_Label> labels;
  for (int i = 0; i < 100; i++) {
    labels.push_back(grjit_asm_label(&as.a));
    grjit_asm_jmp(&as.a, labels.back());
  }
  for (GRJIT_Label l : labels) {
    grjit_asm_bind(&as.a, l);
  }
  grjit_asm_finish(&as.a);
  EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_OK);
  EXPECT_EQ(grjit_asm_size(&as.a), 500u);
}

TEST(Asm, TheByteCapStopsEmissionAndIsRemembered) {
  Asm as(10);
  grjit_asm_mov_ri64(&as.a, GRJIT_RAX, 1); // 10 bytes: exactly the cap
  EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_OK);
  grjit_asm_ret(&as.a);                    // the eleventh
  EXPECT_EQ(grjit_asm_status(&as.a), GRJIT_ASM_LIMIT);
  EXPECT_EQ(grjit_asm_size(&as.a), 10u);
  grjit_asm_ud2(&as.a);                    // a no-op now
  EXPECT_EQ(grjit_asm_size(&as.a), 10u);
}

TEST(Asm, AllocationFailureIsRememberedAndLeaksNothing) {
  for (long fail = 1; fail <= 6; fail++) {
    TrackingAllocator t;
    t.fail_at = fail;
    GRJIT_Asm a;
    grjit_asm_init(&a, t.get(), 1 << 20);
    GRJIT_Label l = grjit_asm_label(&a);
    grjit_asm_jmp(&a, l);
    grjit_asm_ret(&a);
    grjit_asm_bind(&a, l);
    grjit_asm_finish(&a);
    if (t.fail_at != 0 && t.calls >= fail) {
      EXPECT_EQ(grjit_asm_status(&a), GRJIT_ASM_OOM) << fail;
    }
    grjit_asm_free(&a);
    EXPECT_EQ(t.live, 0) << fail;
  }
}

GRJIT_TEST_MAIN()
