/**
 * @file
 *
 * The Wasm spec-suite harness (spec-runtime-float, stories 2 and 3): reads the WebAssembly spec
 * suite's scalar numeric files at the `wg-3.0` tag (`i32`, `i64`, and the eight float and conversion
 * files `f32`, `f64`, `f32_cmp`, `f64_cmp`, `f32_bitwise`, `f64_bitwise`, `conversions`,
 * `float_misc`), turns each exported single-instruction function into an IR function (with the
 * guards of a partial operation written in front of it), compiles it and runs every `assert_return`
 * and `assert_trap` against the suite's own expectations. This is the independent oracle of the
 * integer and float IR: the suite is not ours, and says what Wasm means.
 *
 * A float result is compared as the suite says: `nan:canonical` and `nan:arithmetic` are classes of
 * NaN (the quiet bit and no other payload bit; the quiet bit and any payload), and everything else,
 * a NaN with a stated payload included, is bit for bit. A float literal is converted with `strtof`
 * for an `f32` (never `strtod` and a cast, which rounds twice).
 *
 * It is a reader for exactly the forms these files use, not a Wasm frontend and no API. What it
 * cannot run is counted as excluded, with its reason, and never as passed: `assert_invalid`,
 * `assert_malformed`, `assert_exhaustion`, `assert_unlinkable`, `register`, `module binary` and
 * `module quote`, and a function whose body is not one supported instruction over its parameters
 * (with every assertion that invokes it). A top-level form that is neither handled nor on that list
 * fails the run, so a form the reader does not know cannot vanish. A missing suite fails the test on
 * a host that runs it (tools/wasm/fetch.sh fetches it); it is never skipped there.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../float_fixture.h"
#include "../feature_fixture.h"

#include "../../src/code/code_internal.h"

#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

using namespace intfx;

/* The harness runs the compiled code where there is a native host for a backend that emits the integer and
 * float operations: x86-64 SysV and arm64 (Linux), and Win64 (under wine). Elsewhere the compile still happens for
 * the part that needs no execution: the reader, the mapping into the IR and the verifier's guard rule run wherever the
 * suite is, and an assertion that needs the code run is counted as excluded with the reason below, never as passed.
 * The backends with CPU-feature arms (x86-64, SysV and Win64) run each file twice, with the features the host has and
 * with every optional feature masked off; arm64 has no feature arm, so it runs each file once and the recorded set of
 * every compile is asserted empty. */
#if ((defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)) || (defined(_WIN64) && defined(__x86_64__))
constexpr bool kExecutes = true;
#else
constexpr bool kExecutes = false;
#endif
#if defined(__x86_64__) && (defined(__linux__) || defined(_WIN64))
constexpr bool kFeatureArms = true;
#else
constexpr bool kFeatureArms = false;
#endif
constexpr const char * kNotRunHere = "not run on this host (it has no backend that runs the integer and float operations)";

namespace wast {

/* Why an instruction is left out, when neither table has it. */
const char * const kOutsideTables = "an instruction outside the integer and float tables";

/* ---- S-expressions ------------------------------------------------------------------------ */

struct Node {
  bool list = false;
  bool is_string = false;
  std::string atom; // an atom's text, or a string's contents
  int line = 0;
  std::vector<Node> kids;
  const std::string & head() const {
    static const std::string none;
    return list && !kids.empty() && !kids[0].list ? kids[0].atom : none;
  }
};

struct Reader {
  const std::string & s;
  size_t i = 0;
  int line = 1;
  std::string error;

  explicit Reader(const std::string & text) : s(text) {}

  bool skip() {
    for (;;) {
      if (i >= s.size()) {
        return true;
      }
      char c = s[i];
      if (c == '\n') {
        line++;
        i++;
      } else if (std::isspace(static_cast<unsigned char>(c))) {
        i++;
      } else if (c == ';' && i + 1 < s.size() && s[i + 1] == ';') {
        while (i < s.size() && s[i] != '\n') {
          i++;
        }
      } else if (c == '(' && i + 1 < s.size() && s[i + 1] == ';') {
        int depth = 1;
        i += 2;
        while (i < s.size() && depth > 0) {
          if (s[i] == '\n') {
            line++;
          }
          if (s[i] == '(' && i + 1 < s.size() && s[i + 1] == ';') {
            depth++;
            i += 2;
          } else if (s[i] == ';' && i + 1 < s.size() && s[i + 1] == ')') {
            depth--;
            i += 2;
          } else {
            i++;
          }
        }
        if (depth > 0) {
          error = "line " + std::to_string(line) + ": an unterminated block comment";
          return false;
        }
      } else {
        return true;
      }
    }
  }

  bool node(Node * out) {
    if (!skip()) {
      return false;
    }
    if (i >= s.size()) {
      error = "unexpected end of file";
      return false;
    }
    out->line = line;
    char c = s[i];
    if (c == '(') {
      i++;
      out->list = true;
      for (;;) {
        if (!skip()) {
          return false;
        }
        if (i >= s.size()) {
          error = "line " + std::to_string(out->line) + ": a list is never closed";
          return false;
        }
        if (s[i] == ')') {
          i++;
          return true;
        }
        out->kids.emplace_back();
        if (!node(&out->kids.back())) {
          return false;
        }
      }
    }
    if (c == ')') {
      error = "line " + std::to_string(line) + ": a stray )";
      return false;
    }
    if (c == '"') {
      out->is_string = true;
      i++;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
          out->atom += s[i + 1];
          i += 2;
        } else {
          if (s[i] == '\n') {
            line++;
          }
          out->atom += s[i++];
        }
      }
      if (i >= s.size()) {
        error = "line " + std::to_string(out->line) + ": an unterminated string";
        return false;
      }
      i++;
      return true;
    }
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' &&
           s[i] != ')' && s[i] != '"' && s[i] != ';') {
      out->atom += s[i++];
    }
    return true;
  }

  /* Every top-level node. */
  bool all(std::vector<Node> * out) {
    for (;;) {
      if (!skip()) {
        return false;
      }
      if (i >= s.size()) {
        return true;
      }
      out->emplace_back();
      if (!node(&out->back())) {
        return false;
      }
    }
  }
};

/* A number as the suite writes one: decimal or 0x hexadecimal, a sign, and underscores. Answers the
 * low `bits` bits as a word; false for text that is not a number or does not fit `bits` as signed
 * or unsigned. */
bool parse_int(const std::string & text, unsigned bits, uint64_t * out) {
  std::string t;
  for (char c : text) {
    if (c != '_') {
      t += c;
    }
  }
  size_t k = 0;
  bool negative = false;
  if (k < t.size() && (t[k] == '+' || t[k] == '-')) {
    negative = t[k] == '-';
    k++;
  }
  unsigned base = 10;
  if (t.size() >= k + 2 && t[k] == '0' && (t[k + 1] == 'x' || t[k + 1] == 'X')) {
    base = 16;
    k += 2;
  }
  if (k >= t.size()) {
    return false;
  }
  uint64_t mag = 0;
  for (; k < t.size(); k++) {
    int d;
    char c = t[k];
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      return false;
    }
    if (mag > (UINT64_MAX - static_cast<uint64_t>(d)) / base) {
      return false;
    }
    mag = mag * base + static_cast<uint64_t>(d);
  }
  const uint64_t limit = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
  if (negative) {
    if (mag > (limit >> 1) + 1) {
      return false;
    }
    *out = (0 - mag) & limit;
  } else {
    if (mag > limit) {
      return false;
    }
    *out = mag;
  }
  return true;
}

/* A float as the suite writes one, for a float of `bits` (32 or 64) bits: decimal, hexadecimal
 * (`0x1.8p1`), `inf`, `nan`, `nan:0xHEX` (the payload, exact bits) and, in an expected result only
 * (`allow_class`), `nan:canonical` and `nan:arithmetic`; a sign, and underscores. Answers the bits,
 * and for a class the NaN class asked for. The decimal and the hexadecimal forms go through `strtof`
 * for an f32 and `strtod` for an f64: converting an f32 literal by `strtod` and a cast rounds twice
 * (a decimal just above the midpoint of two floats is that midpoint as a double, and the cast then
 * rounds the tie to even), and is not the number the suite means. */
enum class NanClass { None, Canonical, Arithmetic };

bool parse_float(const std::string & text, unsigned bits, bool allow_class, uint64_t * word,
    NanClass * cls) {
  std::string t;
  for (char c : text) {
    if (c != '_') {
      t += c;
    }
  }
  *cls = NanClass::None;
  size_t k = 0;
  bool negative = false;
  if (k < t.size() && (t[k] == '+' || t[k] == '-')) {
    negative = t[k] == '-';
    k++;
  }
  const std::string body = t.substr(k);
  const uint64_t sign = negative ? (bits == 32 ? 0x80000000ull : 0x8000000000000000ull) : 0;
  const uint64_t exp = bits == 32 ? 0x7F800000ull : 0x7FF0000000000000ull;
  const uint64_t quiet = bits == 32 ? 0x00400000ull : 0x0008000000000000ull;
  const uint64_t fraction = bits == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull;
  if (body == "inf") {
    *word = sign | exp;
    return true;
  }
  if (body == "nan") {
    *word = sign | exp | quiet;
    return true;
  }
  if (body == "nan:canonical" || body == "nan:arithmetic") {
    if (!allow_class) {
      return false;
    }
    *cls = body == "nan:canonical" ? NanClass::Canonical : NanClass::Arithmetic;
    *word = sign | exp | quiet;
    return true;
  }
  if (body.rfind("nan:0x", 0) == 0) {
    uint64_t payload = 0;
    if (!parse_int(body.substr(4), 64, &payload) || payload == 0 || payload > fraction) {
      return false;
    }
    *word = sign | exp | payload;
    return true;
  }
  // A number: it starts with a digit (or a point), and strtof/strtod must use all of it.
  if (body.empty() || !(std::isdigit(static_cast<unsigned char>(body[0])) || body[0] == '.')) {
    return false;
  }
  for (char c : body) {
    if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == '.' || c == 'x' || c == 'X' ||
            c == 'p' || c == 'P' || c == '+' || c == '-')) {
      return false;
    }
  }
  const char * begin = t.c_str();
  char * end = nullptr;
  if (bits == 32) {
    float f = std::strtof(begin, &end);
    if (end == begin || *end != '\0') {
      return false;
    }
    uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    *word = b;
  } else {
    double d = std::strtod(begin, &end);
    if (end == begin || *end != '\0') {
      return false;
    }
    std::memcpy(word, &d, sizeof d);
  }
  return true;
}

/* What the suite wrote for a value: its type, its bits, and a NaN class for an expected result. */
struct Value {
  std::string type;
  uint64_t word = 0;
  NanClass nan = NanClass::None;
};

/* The expected result `want` against `got`, a word of `type`: bit for bit, except a class. */
bool value_matches(const Value & want, uint64_t got) {
  if (want.nan == NanClass::None) {
    return got == want.word;
  }
  const unsigned width = want.type == "f32" ? 32 : 64;
  const uint64_t quiet = width == 32 ? 0x00400000ull : 0x0008000000000000ull;
  const uint64_t fraction = width == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull;
  if (width == 32 && (got >> 32) != 0) {
    return false; // an f32 is zero-extended in its word
  }
  if (!ireval::is_nan_bits(width, got) || (got & quiet) == 0) {
    return false;
  }
  return want.nan == NanClass::Arithmetic || (got & fraction) == quiet;
}

/* ---- The harness -------------------------------------------------------------------------- */

struct Export {
  std::string name;
  std::string instr;
  std::vector<std::string> params;
  std::string result;
  bool excluded = false;
  std::string reason; // why it is excluded
  WasmOp op;
  GRJIT_Code * code = nullptr;
  std::string compile_failure;
};

struct Report {
  size_t passed = 0, failed = 0, returns = 0, traps = 0, invokes = 0, modules = 0, exports = 0;
  size_t guard_checks = 0;
  size_t compiled = 0;      ///< Functions compiled (when the code is run).
  size_t with_arm = 0;      ///< ... of them with a non-empty recorded feature set.
  size_t expected_arm = 0;  ///< ... that should have one: the instruction has a feature arm the host has.
  std::map<std::string, size_t> excluded;
  std::vector<std::string> failures;
  std::vector<std::string> unknown_forms;
  size_t excluded_total() const {
    size_t n = 0;
    for (auto & e : excluded) {
      n += e.second;
    }
    return n;
  }
};

class Harness {
 public:
  /* `world` is null where the code is not run. */
  Harness(JitWorld * world, std::string file, GRJIT_Features disabled = GRJIT_FEATURES_NONE)
      : world_(world), file_(std::move(file)), disabled_(disabled) {}
  ~Harness() {
    for (auto & inst : instances_) {
      for (auto & e : inst.second) {
        grjit_code_destroy(e.second->code);
        grjit_function_destroy(e.second->op.function);
      }
    }
  }
  Harness(const Harness &) = delete;
  Harness & operator=(const Harness &) = delete;

  Report run(const std::string & text);

  /* Adds a hand-built export to the anonymous module, compiled when the code is run: it lets a test
   * give the comparison a function the table of Wasm instructions has no name for (one whose result
   * word has bits above its declared type). The harness owns `function`. */
  void inject(const std::string & name, GRJIT_Function * function, std::vector<std::string> params,
      const std::string & result) {
    auto e = std::make_unique<Export>();
    e->name = name;
    e->params = std::move(params);
    e->result = result;
    e->op.function = function;
    e->op.params = e->params.size();
    if (world_ != nullptr) {
      GRJIT_CompileOptions o{};
      o.pages = world_->pages();
      o.disabled_features = disabled_;
      GRJIT_Result r = grjit_compile(&o, function, &e->code);
      if (r != GRJIT_OK) {
        e->compile_failure = std::string("the compile failed: ") + grjit_result_string(r);
      }
    }
    instances_[""][name] = std::move(e);
    current_ = "";
  }

 private:
  using Instance = std::map<std::string, std::unique_ptr<Export>>;

  void fail(const Node & n, const std::string & what) {
    report_.failed++;
    report_.failures.push_back(file_ + ":" + std::to_string(n.line) + ": " + what);
  }
  void exclude(const std::string & reason) { report_.excluded[reason]++; }

  void module(const Node & n);
  bool function(const Node & n, Instance * inst);
  Export * find(const Node & invoke, std::string * why);
  bool invocation(const Node & invoke, std::vector<uint64_t> * args, std::string * why,
      Export ** exp);
  bool call(Export * e, const std::vector<uint64_t> & args, Outcome * out);
  void assert_return(const Node & n);
  void assert_trap(const Node & n);

  JitWorld * world_;
  std::string file_;
  GRJIT_Features disabled_; ///< The optional CPU features every compile of this run switches off.
  Report report_;
  std::map<std::string, Instance> instances_; // by module id; "" for an anonymous one
  std::string current_;                       // the id of the latest module
};

/* A numeric constant node `(i32.const N)`, `(i64.const N)`, `(f32.const X)` or `(f64.const X)`; false if it
 * is none of them or its text is not a value of its type. */
bool constant(const Node & n, bool allow_class, Value * out) {
  if (!n.list || n.kids.size() != 2 || n.kids[1].list) {
    return false;
  }
  const std::string & h = n.head();
  if (h != "i32.const" && h != "i64.const" && h != "f32.const" && h != "f64.const") {
    return false;
  }
  out->type = h.substr(0, 3);
  out->nan = NanClass::None;
  if (h[0] == 'i') {
    return parse_int(n.kids[1].atom, h == "i32.const" ? 32 : 64, &out->word);
  }
  return parse_float(n.kids[1].atom, h == "f32.const" ? 32 : 64, allow_class, &out->word, &out->nan);
}

const char * type_word(GRJIT_Type t) {
  return t == GRJIT_TYPE_I32 ? "i32" : t == GRJIT_TYPE_I64 ? "i64" : t == GRJIT_TYPE_F32 ? "f32"
         : t == GRJIT_TYPE_F64 ? "f64" : "?";
}

/* `(func $id? (export "n")* (param $x T)* (result T)? (i32.add (local.get $x) (local.get $y)))`. */
bool Harness::function(const Node & n, Instance * inst) {
  std::vector<std::string> names, param_ids, param_types;
  std::string result;
  std::vector<const Node *> body;
  bool locals = false;
  for (size_t k = 1; k < n.kids.size(); k++) {
    const Node & c = n.kids[k];
    if (!c.list) {
      continue; // the function's own $id
    }
    const std::string & h = c.head();
    if (h == "export" && c.kids.size() == 2 && c.kids[1].is_string) {
      names.push_back(c.kids[1].atom);
    } else if (h == "param") {
      if (c.kids.size() == 3) {
        param_ids.push_back(c.kids[1].atom);
        param_types.push_back(c.kids[2].atom);
      } else if (c.kids.size() == 2) {
        param_ids.push_back("");
        param_types.push_back(c.kids[1].atom);
      } else {
        for (size_t t = 1; t < c.kids.size(); t++) {
          param_ids.push_back("");
          param_types.push_back(c.kids[t].atom);
        }
      }
    } else if (h == "result" && c.kids.size() == 2) {
      result = c.kids[1].atom;
    } else if (h == "local") {
      locals = true;
    } else {
      body.push_back(&c);
    }
  }
  if (names.empty()) {
    exclude("a function with no export");
    return true;
  }
  auto add = [&](bool excluded, const std::string & reason, const std::string & instr) {
    for (const std::string & name : names) {
      auto e = std::make_unique<Export>();
      e->name = name;
      e->instr = instr;
      e->params = param_types;
      e->result = result;
      e->excluded = excluded;
      e->reason = reason;
      (*inst)[name] = std::move(e);
      report_.exports++;
    }
  };
  if (locals || body.size() != 1 || body[0]->kids.empty() || body[0]->kids[0].list) {
    exclude("a function body that is not a single instruction over its parameters");
    add(true, "a function body that is not a single instruction over its parameters", "");
    return true;
  }
  const Node & b = *body[0];
  const std::string instr = b.head();
  // The operands must be the parameters, in order.
  bool shape = b.kids.size() == 1 + param_ids.size();
  for (size_t k = 0; shape && k < param_ids.size(); k++) {
    const Node & o = b.kids[1 + k];
    shape = o.list && o.head() == "local.get" && o.kids.size() == 2 &&
            (o.kids[1].atom == param_ids[k] || o.kids[1].atom == std::to_string(k));
  }
  if (!shape) {
    exclude("a function body that is not a single instruction over its parameters");
    add(true, "a function body that is not a single instruction over its parameters", instr);
    return true;
  }
  bool float_table = true;
  WasmOp op = floatfx::build_float_op(instr);
  if (op.function == nullptr) {
    float_table = false;
    op = build_wasm_op(instr);
  }
  if (op.function == nullptr) {
    exclude(kOutsideTables);
    add(true, kOutsideTables, instr);
    return true;
  }
  // The suite's signature and the table's must be the same one.
  const std::string want_result = type_word(op.result);
  if (op.params != param_types.size() || want_result != result) {
    grjit_function_destroy(op.function);
    fail(n, "the harness's table gives " + instr + " " + std::to_string(op.params) + " parameters and " +
                want_result + ", the suite's function has " + std::to_string(param_types.size()) +
                " and " + result);
    add(true, "a signature the table disagrees with", instr);
    return true;
  }
  for (size_t k = 0; k < param_types.size(); k++) {
    const GRJIT_Type t = grjit_function_vreg_type(op.function, static_cast<GRJIT_VReg>(k));
    if (std::string(type_word(t)) != param_types[k]) {
      grjit_function_destroy(op.function);
      fail(n, "the harness's table gives parameter " + std::to_string(k) + " of " + instr +
                  " another type than the suite");
      add(true, "a signature the table disagrees with", instr);
      return true;
    }
  }
  GRJIT_Result verified = grjit_function_verify(op.function, nullptr, nullptr, 0);
  if (verified != GRJIT_OK) {
    grjit_function_destroy(op.function);
    fail(n, "the verifier refuses the function built for " + instr);
    add(true, "a function the verifier refuses", instr);
    return true;
  }
  // The control of the guard rule: the same operation with a guard left out must be refused.
  if (op.partial && float_table) {
    // A truncation: with no range guard, and with the NaN guard alone, the verifier refuses it.
    // (The range guard alone is accepted: ordered compares keep a NaN out too.)
    floatfx::TruncGuards variants[2] = {{false, false}, {true, false}};
    for (int v = 0; v < 2; v++) {
      WasmOp bare = floatfx::build_float_op(instr, variants[v]);
      char why[256] = {0};
      GRJIT_Result r = grjit_function_verify(bare.function, nullptr, why, sizeof why);
      report_.guard_checks++;
      if (r == GRJIT_OK) {
        fail(n, "the verifier accepts " + instr + " with its range guard left out (" +
                    (v == 0 ? "no guard at all" : "the NaN guard alone") + ")");
      }
      grjit_function_destroy(bare.function);
    }
  } else if (op.partial) {
    Guards variants[3] = {{false, false}, {false, true}, {true, false}};
    for (int v = 0; v < 3; v++) {
      if (v == 2 && instr.find("div_s") == std::string::npos) {
        continue; // only div_s has an overflow guard to leave out
      }
      WasmOp bare = build_wasm_op(instr, variants[v]);
      char why[256] = {0};
      GRJIT_Result r = grjit_function_verify(bare.function, nullptr, why, sizeof why);
      report_.guard_checks++;
      if (r == GRJIT_OK) {
        fail(n, "the verifier accepts " + instr + " with a guard left out (" +
                    (v == 0 ? "both" : v == 1 ? "the divisor's" : "the overflow") + ")");
      }
      grjit_function_destroy(bare.function);
    }
  }
  add(false, "", instr);
  // One function per export: the first is the one built; further names get their own.
  bool first = true;
  for (const std::string & name : names) {
    Export * e = (*inst)[name].get();
    e->op = first ? op : (float_table ? floatfx::build_float_op(instr) : build_wasm_op(instr));
    first = false;
    if (world_ != nullptr) {
      GRJIT_CompileOptions o{};
      o.pages = world_->pages();
      o.disabled_features = disabled_;
      GRJIT_Result r = grjit_compile(&o, e->op.function, &e->code);
      if (r != GRJIT_OK) {
        e->compile_failure = std::string("the compile of ") + instr + " failed: " + grjit_result_string(r);
      } else {
        // What the compile recorded, against what the instruction and the effective set say it should.
        report_.compiled++;
        const GRJIT_Features want = featfx::detected() & ~disabled_ & featfx::need_of(instr);
        report_.expected_arm += want != GRJIT_FEATURES_NONE;
        report_.with_arm += grjit_code_features(e->code) != GRJIT_FEATURES_NONE;
        if (grjit_code_features(e->code) != want) {
          fail(n, std::string("the recorded feature set of ") + instr + " is not the one its arm and the mask give");
        }
      }
    }
  }
  return true;
}

void Harness::module(const Node & n) {
  // `(module binary ...)` and `(module quote ...)`: text this reader does not read.
  for (size_t k = 1; k < n.kids.size(); k++) {
    if (!n.kids[k].list && (n.kids[k].atom == "binary" || n.kids[k].atom == "quote")) {
      exclude("module binary/quote");
      return;
    }
  }
  report_.modules++;
  std::string id;
  Instance inst;
  for (size_t k = 1; k < n.kids.size(); k++) {
    const Node & c = n.kids[k];
    if (!c.list) {
      if (!c.atom.empty() && c.atom[0] == '$') {
        id = c.atom;
      }
      continue;
    }
    if (c.head() == "func") {
      function(c, &inst);
    } else {
      exclude("a module field that is not a function (" + c.head() + ")");
    }
  }
  // A module replaces an earlier one of the same id (the suite's own `module` after `module`).
  for (auto & old : instances_[id]) {
    grjit_code_destroy(old.second->code);
    grjit_function_destroy(old.second->op.function);
    old.second->code = nullptr;
    old.second->op.function = nullptr;
  }
  instances_[id] = std::move(inst);
  current_ = id;
}

/* `(invoke "name" args...)` or `(invoke $M "name" args...)`. */
bool Harness::invocation(const Node& n, std::vector<uint64_t> * args, std::string * why, Export ** exp) {
  if (!n.list || n.head() != "invoke") {
    *why = "an action that is not an invoke (" + n.head() + ")";
    return false;
  }
  size_t k = 1;
  std::string module_id = current_;
  if (k < n.kids.size() && !n.kids[k].list && !n.kids[k].is_string && !n.kids[k].atom.empty() &&
      n.kids[k].atom[0] == '$') {
    module_id = n.kids[k].atom;
    k++;
  }
  if (k >= n.kids.size() || !n.kids[k].is_string) {
    *why = "an invoke with no export name";
    return false;
  }
  const std::string name = n.kids[k].atom;
  auto inst = instances_.find(module_id);
  if (inst == instances_.end() || inst->second.find(name) == inst->second.end()) {
    *why = "no export named \"" + name + "\"";
    return false;
  }
  *exp = inst->second[name].get();
  if ((*exp)->excluded) {
    *why = "";
    return true; // the caller counts it excluded
  }
  args->clear();
  size_t given = n.kids.size() - (k + 1);
  if (given != (*exp)->params.size()) {
    *why = "\"" + name + "\" is invoked with " + std::to_string(given) + " arguments, it takes " +
           std::to_string((*exp)->params.size());
    return false;
  }
  for (size_t a = 0; a < given; a++) {
    Value v;
    if (!constant(n.kids[k + 1 + a], false, &v)) {
      *why = "an argument that is not an i32, i64, f32 or f64 constant (a form this reader does not read)";
      return false;
    }
    if (v.type != (*exp)->params[a]) {
      *why = "an argument of type " + v.type + " for a parameter of type " + (*exp)->params[a];
      return false;
    }
    args->push_back(v.word);
  }
  return true;
}

bool Harness::call(Export * e, const std::vector<uint64_t> & args, Outcome * out) {
  if (!e->compile_failure.empty() || e->code == nullptr) {
    return false;
  }
  *out = intfx::run(e->code, world_->ctx, args);
  return true;
}

void Harness::assert_return(const Node & n) {
  report_.returns++;
  if (n.kids.size() < 2) {
    fail(n, "an assert_return with no action");
    return;
  }
  std::vector<uint64_t> args;
  std::string why;
  Export * e = nullptr;
  if (!invocation(n.kids[1], &args, &why, &e)) {
    if (e == nullptr && n.kids[1].head() != "invoke") {
      exclude("an assertion about " + n.kids[1].head() + ", not an invoke");
    } else {
      fail(n, why);
    }
    return;
  }
  if (e->excluded) {
    exclude("an assertion about an excluded function");
    return;
  }
  std::vector<Value> want;
  for (size_t k = 2; k < n.kids.size(); k++) {
    Value v;
    if (!constant(n.kids[k], true, &v)) {
      fail(n, "an expected result that is not an i32, i64, f32 or f64 constant (a form this reader does not read)");
      return;
    }
    want.push_back(v);
  }
  if (want.size() != 1 || want[0].type != e->result) {
    fail(n, "the expected result does not match the function's result type " + e->result);
    return;
  }
  if (!kExecutes) {
    exclude(kNotRunHere);
    return;
  }
  Outcome o;
  if (!call(e, args, &o)) {
    fail(n, e->compile_failure.empty() ? "the function was not compiled" : e->compile_failure);
    return;
  }
  char buf[160];
  if (o.fault != 0) {
    fail(n, "\"" + e->name + "\" faulted in hardware (signal " + std::to_string(o.fault) + ")");
  } else if (o.exit == GRJIT_EXIT_DEOPT) {
    const GRCORE_CodeSite * s =
        grcore_codemeta_find(grjit_code_meta(e->code), static_cast<uint32_t>(o.out[0]));
    fail(n, "\"" + e->name + "\" trapped (" + (s != nullptr ? floatfx::trap_name(s->identity.offset) : "?") +
                "), the suite expects a value");
  } else if (o.exit != GRJIT_EXIT_RETURNED) {
    fail(n, "\"" + e->name + "\" exited " + std::to_string(o.exit));
  } else if (!value_matches(want[0], o.out[0])) {
    // A value is compared as a whole word: an i32 or f32 has a zero upper half.
    std::snprintf(buf, sizeof buf, " got 0x%" PRIx64 ", expected %s0x%" PRIx64, o.out[0],
        want[0].nan == NanClass::Canonical    ? "nan:canonical, as "
        : want[0].nan == NanClass::Arithmetic ? "nan:arithmetic, as "
                                              : "",
        want[0].word);
    fail(n, "\"" + e->name + "\"" + buf);
  } else {
    report_.passed++;
  }
}

void Harness::assert_trap(const Node & n) {
  report_.traps++;
  if (n.kids.size() != 3 || !n.kids[2].is_string) {
    fail(n, "an assert_trap that is not (assert_trap action \"message\")");
    return;
  }
  if (n.kids[1].list && n.kids[1].head() == "module") {
    exclude("an assert_trap of a module");
    return;
  }
  std::vector<uint64_t> args;
  std::string why;
  Export * e = nullptr;
  if (!invocation(n.kids[1], &args, &why, &e)) {
    if (e == nullptr && n.kids[1].head() != "invoke") {
      exclude("an assertion about " + n.kids[1].head() + ", not an invoke");
    } else {
      fail(n, why);
    }
    return;
  }
  if (e->excluded) {
    exclude("an assertion about an excluded function");
    return;
  }
  const std::string & text = n.kids[2].atom;
  uint64_t site;
  if (text == "integer divide by zero") {
    site = kSiteDivideByZero;
  } else if (text == "integer overflow") {
    site = kSiteIntegerOverflow;
  } else if (text == "invalid conversion to integer") {
    site = floatfx::kSiteInvalidConversion;
  } else {
    fail(n, "a trap message this harness has no guard for: \"" + text + "\"");
    return;
  }
  if (!kExecutes) {
    exclude(kNotRunHere);
    return;
  }
  Outcome o;
  if (!call(e, args, &o)) {
    fail(n, e->compile_failure.empty() ? "the function was not compiled" : e->compile_failure);
    return;
  }
  if (o.fault != 0) {
    fail(n, "\"" + e->name + "\" faulted in hardware (signal " + std::to_string(o.fault) +
                "), where the suite expects: " + text);
  } else if (o.exit != GRJIT_EXIT_DEOPT) {
    fail(n, "\"" + e->name + "\" returned, where the suite expects the trap: " + text);
  } else {
    const GRCORE_CodeSite * s =
        grcore_codemeta_find(grjit_code_meta(e->code), static_cast<uint32_t>(o.out[0]));
    if (s == nullptr || s->kind != GRCORE_SITE_GUARD || s->identity.function != kTrapFunction) {
      fail(n, "\"" + e->name + "\" left through something that is not one of its guards");
    } else if (s->identity.offset != site) {
      fail(n, "\"" + e->name + "\" trapped with \"" + floatfx::trap_name(s->identity.offset) +
                  "\", the suite expects \"" + text + "\"");
    } else {
      report_.passed++;
    }
  }
}

Report Harness::run(const std::string & text) {
  std::vector<Node> forms;
  Reader r(text);
  if (!r.all(&forms)) {
    Node n;
    n.line = r.line;
    fail(n, "unreadable: " + r.error);
    return report_;
  }
  for (const Node & n : forms) {
    if (!n.list) {
      report_.unknown_forms.push_back(n.atom);
      fail(n, "a top-level form that is not a list: " + n.atom);
      continue;
    }
    const std::string & h = n.head();
    if (h == "module") {
      module(n);
    } else if (h == "assert_return") {
      assert_return(n);
    } else if (h == "assert_trap") {
      assert_trap(n);
    } else if (h == "invoke") {
      report_.invokes++;
      std::vector<uint64_t> args;
      std::string why;
      Export * e = nullptr;
      if (!invocation(n, &args, &why, &e)) {
        fail(n, why);
      } else if (e->excluded) {
        exclude("an invoke of an excluded function");
      } else if (!kExecutes) {
        exclude(kNotRunHere);
      } else {
        Outcome o;
        if (!call(e, args, &o)) {
          fail(n, e->compile_failure.empty() ? "the function was not compiled" : e->compile_failure);
        } else if (o.fault != 0 || o.exit != GRJIT_EXIT_RETURNED) {
          fail(n, "a bare invoke of \"" + e->name + "\" did not return");
        } else {
          report_.passed++;
        }
      }
    } else if (h == "assert_invalid" || h == "assert_malformed" || h == "assert_exhaustion" ||
               h == "assert_unlinkable" || h == "register") {
      exclude(h);
    } else {
      report_.unknown_forms.push_back(h);
      fail(n, "a top-level form this harness neither handles nor excludes: (" + h + " ...)");
    }
  }
  return report_;
}

/* The text of a file, or the reason there is none. */
bool load(const std::string & path, std::string * text, std::string * why) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *why = "cannot read " + path + ": the Wasm spec suite is not fetched; run libs/runtime-jit/tools/wasm/fetch.sh";
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  *text = ss.str();
  if (text->empty()) {
    *why = path + " is empty; run libs/runtime-jit/tools/wasm/fetch.sh";
    return false;
  }
  return true;
}

std::string summary(const std::string & file, const Report & r) {
  std::ostringstream o;
  o << file << ": " << r.passed << " passed (" << r.returns << " assert_return and " << r.traps
    << " assert_trap seen), " << r.excluded_total() << " excluded";
  const char * sep = " (";
  for (auto & e : r.excluded) {
    o << sep << e.second << " " << e.first;
    sep = "; ";
  }
  if (!r.excluded.empty()) {
    o << ")";
  }
  o << ", " << r.failed << " failed; " << r.exports << " exported functions in " << r.modules
    << " module(s), " << r.guard_checks << " unguarded variants refused by the verifier";
  if (r.compiled != 0) {
    o << "; " << r.compiled << " compiled, " << r.with_arm << " with a feature arm recorded";
  }
  return o.str();
}

/* The library's root, fixed when the test is compiled (the Makefile defines it), so that the suite is
 * found whatever directory the program runs in. */
#ifndef GRJIT_LIBRARY_ROOT
#define GRJIT_LIBRARY_ROOT "."
#endif

std::string library_root() {
  return GRJIT_LIBRARY_ROOT;
}

/* Where the suite is: the commit is the pin file's, the data is fetched by tools/wasm/fetch.sh.
 * GRJIT_WASM_SUITE_DIR names the directory that holds `test/core/` instead. */
std::string suite_dir() {
  if (const char * d = std::getenv("GRJIT_WASM_SUITE_DIR")) {
    return d;
  }
  std::ifstream pin(library_root() + "/tools/wasm/WASM_SPEC_COMMIT");
  std::string commit;
  std::getline(pin, commit);
  while (!commit.empty() && std::isspace(static_cast<unsigned char>(commit.back()))) {
    commit.pop_back();
  }
  return library_root() + "/third_party/wasm-spec/" + commit;
}

/* SHA-256 (FIPS 180-4), so that the file read is the file pinned in tools/wasm/SHA256SUMS. */
std::string sha256_hex(const std::string & data) {
  static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
      0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6,
      0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
      0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
      0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585,
      0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
      0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
      0x1f83d9ab, 0x5be0cd19};
  std::string m = data;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  m += static_cast<char>(0x80);
  while (m.size() % 64 != 56) {
    m += '\0';
  }
  for (int i = 7; i >= 0; i--) {
    m += static_cast<char>((bits >> (8 * i)) & 0xFF);
  }
  auto rotr = [](uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[64];
    for (int t = 0; t < 16; t++) {
      w[t] = (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * t])) << 24) |
             (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * t + 1])) << 16) |
             (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * t + 2])) << 8) |
             static_cast<uint32_t>(static_cast<uint8_t>(m[off + 4 * t + 3]));
    }
    for (int t = 16; t < 64; t++) {
      uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
      uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t v[8];
    std::memcpy(v, h, sizeof v);
    for (int t = 0; t < 64; t++) {
      uint32_t S1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      uint32_t t1 = v[7] + S1 + ch + K[t] + w[t];
      uint32_t S0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      uint32_t t2 = S0 + mj;
      v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
      v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) {
      h[i] += v[i];
    }
  }
  char out[65];
  for (int i = 0; i < 8; i++) {
    std::snprintf(out + 8 * i, 9, "%08x", h[i]);
  }
  return out;
}

/* The digest tools/wasm/SHA256SUMS pins for `test/core/<name>`, or "" if it lists none. */
std::string pinned_digest(const std::string & name) {
  std::ifstream in(library_root() + "/tools/wasm/SHA256SUMS");
  std::string digest, path;
  while (in >> digest >> path) {
    if (path == "test/core/" + name) {
      return digest;
    }
  }
  return "";
}

} // namespace wast

namespace {

/* Runs `text` and returns the report; the world is only made where the code is run. */
wast::Report run_text(const std::string & text, const char * name = "inline.wast",
    GRJIT_Features disabled = GRJIT_FEATURES_NONE) {
  if (kExecutes) {
    JitWorld w;
    wast::Harness h(&w, name, disabled);
    return h.run(text);
  }
  wast::Harness h(nullptr, name);
  return h.run(text);
}

const char kModule[] = R"wast((module
  (func (export "add") (param $x i32) (param $y i32) (result i32) (i32.add (local.get $x) (local.get $y)))
  (func (export "div_s") (param $x i32) (param $y i32) (result i32) (i32.div_s (local.get $x) (local.get $y)))
  (func (export "rem_s") (param $x i64) (param $y i64) (result i64) (i64.rem_s (local.get $x) (local.get $y)))
  (func (export "clz") (param $x i32) (result i32) (i32.clz (local.get $x)))
)
)wast";

} // namespace

/* ---- The harness shown able to fail ------------------------------------------------------- */

TEST(Wast, ARightExpectationPassesAndACorruptedOneFails) {
  const std::string good = std::string(kModule) +
      "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (i32.const 2))\n"
      "(assert_return (invoke \"add\" (i32.const -1) (i32.const 1)) (i32.const 0))\n"
      "(assert_return (invoke \"clz\" (i32.const 0)) (i32.const 32))\n"
      "(assert_trap (invoke \"div_s\" (i32.const 1) (i32.const 0)) \"integer divide by zero\")\n"
      "(assert_trap (invoke \"div_s\" (i32.const 0x80000000) (i32.const -1)) \"integer overflow\")\n"
      "(assert_return (invoke \"rem_s\" (i64.const 0x8000000000000000) (i64.const -1)) (i64.const 0))\n";
  wast::Report ok = run_text(good);
  if (kExecutes) {
    EXPECT_EQ(ok.passed, 6u);
  }
  EXPECT_EQ(ok.failed, 0u) << (ok.failures.empty() ? "" : ok.failures[0]);
  EXPECT_GE(ok.guard_checks, 4u);

  struct Corrupt {
    const char * what;
    const char * form;
    bool decided_by_running; // the verdict needs the code run, which only an x86-64 SysV host does
  };
  const Corrupt bad[] = {
      {"a wrong value", "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (i32.const 3))", true},
      {"a value that does not fit its type", "(assert_return (invoke \"add\" (i32.const -1) (i32.const 1)) (i32.const 0x100000000))", false},
      {"a value where a trap is the answer", "(assert_return (invoke \"div_s\" (i32.const 1) (i32.const 0)) (i32.const 0))", true},
      {"a trap where a value is the answer", "(assert_trap (invoke \"add\" (i32.const 1) (i32.const 1)) \"integer divide by zero\")", true},
      {"the wrong trap", "(assert_trap (invoke \"div_s\" (i32.const 1) (i32.const 0)) \"integer overflow\")", true},
      {"the other wrong trap", "(assert_trap (invoke \"div_s\" (i32.const 0x80000000) (i32.const -1)) \"integer divide by zero\")", true},
      {"a trap message it has no guard for", "(assert_trap (invoke \"div_s\" (i32.const 1) (i32.const 0)) \"unreachable\")", false},
      {"an argument of the wrong type", "(assert_return (invoke \"add\" (i64.const 1) (i32.const 1)) (i32.const 2))", false},
      {"a result of the wrong type", "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (i64.const 2))", false},
      {"too few arguments", "(assert_return (invoke \"add\" (i32.const 1)) (i32.const 2))", false},
      {"an export that does not exist", "(assert_return (invoke \"nope\" (i32.const 1)) (i32.const 2))", false},
      {"a result of the wrong type (a float)", "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (f32.const 2))", false},
      {"a result form it cannot read", "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (v128.const i32x4 0 0 0 0))", false},
  };
  for (const Corrupt & c : bad) {
    SCOPED_TRACE(c.what);
    if (c.decided_by_running && !kExecutes) {
      continue;
    }
    wast::Report r = run_text(std::string(kModule) + c.form + "\n");
    EXPECT_EQ(r.failed, 1u) << c.form;
    EXPECT_EQ(r.passed, 0u);
  }
}

TEST(Wast, EveryExcludedFormIsCountedWithItsReasonAndNeverAsPassed) {
  const std::string text = std::string(kModule) + R"wast(
(assert_invalid (module (func (i32.eqz) (drop))) "type mismatch")
(assert_malformed (module quote "(func (result i32) (i32.const nan:arithmetic))") "unexpected token")
(assert_exhaustion (invoke "add" (i32.const 1) (i32.const 1)) "call stack exhausted")
(assert_unlinkable (module (import "m" "f" (func))) "unknown import")
(register "M")
(module binary "\00asm\01\00\00\00")
(module quote "(func)")
)wast";
  wast::Report r = run_text(text);
  EXPECT_EQ(r.passed, 0u) << "an excluded assertion is not a pass";
  EXPECT_EQ(r.failed, 0u) << (r.failures.empty() ? "" : r.failures[0]);
  EXPECT_EQ(r.excluded["assert_invalid"], 1u);
  EXPECT_EQ(r.excluded["assert_malformed"], 1u);
  EXPECT_EQ(r.excluded["assert_exhaustion"], 1u);
  EXPECT_EQ(r.excluded["assert_unlinkable"], 1u);
  EXPECT_EQ(r.excluded["register"], 1u);
  EXPECT_EQ(r.excluded["module binary/quote"], 2u);
  EXPECT_EQ(r.excluded_total(), 7u);
}

TEST(Wast, AFunctionThatIsNotOneSupportedInstructionIsExcludedAndSoAreItsAssertions) {
  const std::string text = R"wast((module
  (func (export "two") (param $x i32) (result i32) (i32.add (local.get $x) (i32.const 1)))
  (func (export "swapped") (param $x i32) (param $y i32) (result i32) (i32.sub (local.get $y) (local.get $x)))
  (func (export "three") (param $x i32) (result i32) (i32.add (i32.add (local.get $x) (local.get $x)) (local.get $x)))
  (func (export "load") (param $x i32) (result i32) (i32.load (local.get $x)))
  (func (export "local") (param $x i32) (result i32) (local $t i32) (i32.eqz (local.get $x)))
  (func (export "empty") (param $x i32) (result i32))
  (func (export "ok") (param $x i32) (result i32) (i32.eqz (local.get $x)))
  (func (param $x i32) (result i32) (i32.eqz (local.get $x)))
)
(assert_return (invoke "two" (i32.const 1)) (i32.const 2))
(assert_return (invoke "swapped" (i32.const 1) (i32.const 3)) (i32.const 2))
(assert_trap (invoke "load" (i32.const 1)) "out of bounds memory access")
(assert_return (invoke "ok" (i32.const 0)) (i32.const 1))
)wast";
  wast::Report r = run_text(text);
  EXPECT_EQ(r.failed, 0u) << (r.failures.empty() ? "" : r.failures[0]);
  EXPECT_EQ(r.excluded["a function body that is not a single instruction over its parameters"], 5u)
      << "two (an operand that is not a parameter), swapped, three (nested), local, empty";
  EXPECT_EQ(r.excluded[wast::kOutsideTables], 1u);
  EXPECT_EQ(r.excluded["a function with no export"], 1u);
  EXPECT_EQ(r.excluded["an assertion about an excluded function"], 3u);
  if (kExecutes) {
    EXPECT_EQ(r.passed, 1u);
  }
}

TEST(Wast, ATopLevelFormNeitherHandledNorExcludedFailsTheRunAndIsNamed) {
  for (const char * form : {"(frobnicate \"x\")", "(assert_return_nan (invoke \"add\"))",
           "(assert_trap_function)", "(script)", "stray"}) {
    SCOPED_TRACE(form);
    wast::Report r = run_text(std::string(kModule) + form + "\n");
    EXPECT_GE(r.failed, 1u);
    ASSERT_EQ(r.unknown_forms.size(), 1u);
    EXPECT_NE(r.failures.back().find(r.unknown_forms[0]), std::string::npos)
        << "the failure names the form";
  }
}

TEST(Wast, TheNumbersTheSuiteWritesAreRead) {
  struct Case {
    const char * text;
    unsigned bits;
    bool ok;
    uint64_t want;
  };
  const Case cases[] = {
      {"0", 32, true, 0}, {"1", 32, true, 1}, {"-1", 32, true, 0xFFFFFFFFu},
      {"0x7fffffff", 32, true, 0x7FFFFFFFu}, {"0x80000000", 32, true, 0x80000000u},
      {"-2147483648", 32, true, 0x80000000u}, {"0xffffffff", 32, true, 0xFFFFFFFFu},
      {"0x100000000", 32, false, 0}, {"-2147483649", 32, false, 0}, {"4294967295", 32, true, 0xFFFFFFFFu},
      {"-0x80000000", 32, true, 0x80000000u}, {"0xFFFFFFFFFFFFFFFF", 64, true, UINT64_MAX},
      {"-1", 64, true, UINT64_MAX}, {"-9223372036854775808", 64, true, 0x8000000000000000ull},
      {"9223372036854775807", 64, true, 0x7FFFFFFFFFFFFFFFull}, {"0x1_0000", 32, true, 0x10000},
      {"1_000", 32, true, 1000}, {"18446744073709551615", 64, true, UINT64_MAX},
      {"18446744073709551616", 64, false, 0}, {"-0x8000000000000001", 64, false, 0},
      {"", 32, false, 0}, {"0x", 32, false, 0}, {"1.5", 32, false, 0}, {"nan:canonical", 32, false, 0},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.text);
    uint64_t v = 0;
    EXPECT_EQ(wast::parse_int(c.text, c.bits, &v), c.ok);
    if (c.ok) {
      EXPECT_EQ(v, c.want);
    }
  }
}

TEST(Wast, TheReaderSkipsCommentsAndKeepsTrackOfLines) {
  const std::string text = R"wast(;; a line comment
(; a block (; nested ;) comment ;)
(module
  (func (export "eqz") (param i32) (result i32) (i32.eqz (local.get 0))) ;; trailing
)
(assert_return (invoke "eqz" (i32.const 0)) (i32.const 2))
)wast";
  wast::Report r = run_text(text, "lines.wast");
  EXPECT_EQ(r.modules, 1u);
  if (kExecutes) {
    ASSERT_EQ(r.failed, 1u) << "the expectation is wrong";
    EXPECT_NE(r.failures[0].find("lines.wast:6:"), std::string::npos) << r.failures[0];
  }
  wast::Report unreadable = run_text("(module (func", "broken.wast");
  EXPECT_EQ(unreadable.failed, 1u);
  EXPECT_NE(unreadable.failures[0].find("never closed"), std::string::npos) << unreadable.failures[0];
}

TEST(Wast, AMissingSuiteFileIsAFailureThatNamesTheFetchScript) {
  std::string text, why;
  EXPECT_FALSE(wast::load(wast::library_root() + "/third_party/wasm-spec/does-not-exist/i32.wast", &text, &why));
  EXPECT_NE(why.find("tools/wasm/fetch.sh"), std::string::npos) << why;
}

TEST(Wast, TheSha256IsTheStandardOne) {
  EXPECT_EQ(wast::sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(wast::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(wast::sha256_hex(std::string(1000, 'a')),
      "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

/* The comparison is of the whole result word. The suite's own files cannot show it (an i32 operation
 * of the table never returns bits above 31), so the control is an export built by hand whose word
 * has them: the reader accepts the assertion, the comparison must refuse it. */
TEST(Wast, TheComparisonIsOfTheWholeWordNotOfTheLowHalf) {
  if (!kExecutes) {
    GTEST_SKIP() << "the compare is made on the result of running the code, which this host does not do";
  }
  auto wide = [](int64_t value) {
    B b("wide", 0);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I64);
    b.at(b.block());
    b.cnst(r, value);
    b.ret(V(r));
    return b.finish();
  };
  struct Case {
    const char * form;
    const char * type;
    size_t failed;
  };
  const Case cases[] = {
      // declared i32, but the word is 0x100000002: the low half is 2 and the word is not
      {"(assert_return (invoke \"w\") (i32.const 2))", "i32", 1},
      // declared i64: the same word is what is expected, and it passes
      {"(assert_return (invoke \"w\") (i64.const 0x100000002))", "i64", 0},
      // declared i64: the low half alone is not the word
      {"(assert_return (invoke \"w\") (i64.const 2))", "i64", 1},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.form);
    JitWorld w;
    wast::Harness h(&w, "hand.wast");
    h.inject("w", wide(0x100000002ll), {}, c.type);
    wast::Report r = h.run(c.form);
    EXPECT_EQ(r.failed, c.failed) << (r.failures.empty() ? "" : r.failures[0]);
    EXPECT_EQ(r.passed, c.failed == 0 ? 1u : 0u);
    if (c.failed != 0) {
      EXPECT_NE(r.failures[0].find("got 0x100000002"), std::string::npos) << r.failures[0];
    }
  }
}

/* ---- The float forms: the reader, and the harness shown able to fail on them ---------------------- */

TEST(Wast, TheFloatsTheSuiteWritesAreRead) {
  struct Case {
    const char * text;
    unsigned bits;
    bool allow_class;
    bool ok;
    uint64_t want;
    wast::NanClass cls;
  };
  using wast::NanClass;
  const NanClass N = NanClass::None;
  const Case cases[] = {
      {"1.5", 32, false, true, 0x3FC00000u, N},
      {"+1.5", 32, false, true, 0x3FC00000u, N},
      {"-1.5", 32, false, true, 0xBFC00000u, N},
      {"0x1.8p1", 32, false, true, 0x40400000u, N},
      {"0x1.8p+1", 32, false, true, 0x40400000u, N},
      {"-0x1p-149", 32, false, true, 0x80000001u, N},
      {"0x1p-149", 32, false, true, 0x00000001u, N},
      {"0x1p-150", 32, false, true, 0u, N},
      {"0x1.8p-149", 32, false, true, 2u, N},
      {"0x1.fffffep+127", 32, false, true, 0x7F7FFFFFu, N},
      {"0x1.fffffep-127", 32, false, true, 0x00800000u, N}, // halfway to the smallest normal: ties to even
      {"0x1.fffffcp-127", 32, false, true, 0x007FFFFFu, N},
      {"0x1p-126", 32, false, true, 0x00800000u, N},
      {"0", 32, false, true, 0u, N},
      {"-0", 32, false, true, 0x80000000u, N},
      {"-0.0", 32, false, true, 0x80000000u, N},
      {"0x0p+0", 32, false, true, 0u, N},
      {"1_000.5", 32, false, true, 0x447A2000u, N},
      {"100", 32, false, true, 0x42C80000u, N},
      {"16777217", 32, false, true, 0x4B800000u, N},
      {"1e20", 32, false, true, 0x60AD78ECu, N},
      {"inf", 32, false, true, 0x7F800000u, N},
      {"-inf", 32, false, true, 0xFF800000u, N},
      {"nan", 32, false, true, 0x7FC00000u, N},
      {"-nan", 32, false, true, 0xFFC00000u, N},
      {"nan:0x200000", 32, false, true, 0x7FA00000u, N},
      {"-nan:0x200000", 32, false, true, 0xFFA00000u, N},
      {"nan:0x7fffff", 32, false, true, 0x7FFFFFFFu, N},
      {"nan:0x1", 32, false, true, 0x7F800001u, N},
      {"nan:0x0", 32, false, false, 0, N},
      {"nan:0x800000", 32, false, false, 0, N},
      {"nan:canonical", 32, false, false, 0, N},
      {"nan:canonical", 32, true, true, 0x7FC00000u, NanClass::Canonical},
      {"nan:arithmetic", 32, true, true, 0x7FC00000u, NanClass::Arithmetic},
      {"nan:arithmetic", 64, true, true, 0x7FF8000000000000ull, NanClass::Arithmetic},
      {"nan:0x4000000000000", 64, false, true, 0x7FF4000000000000ull, N},
      {"-nan:0xfffffffffffff", 64, false, true, 0xFFFFFFFFFFFFFFFFull, N},
      {"nan:0x10000000000000", 64, false, false, 0, N},
      {"0.1", 64, false, true, 0x3FB999999999999Aull, N},
      {"0x1.999999999999ap-4", 64, false, true, 0x3FB999999999999Aull, N},
      {"0.1", 32, false, true, 0x3DCCCCCDu, N},
      {"0x1.99999ap-4", 32, false, true, 0x3DCCCCCDu, N},
      {"0x1p-1074", 64, false, true, 1u, N},
      {"-0x1.fffffffffffffp+1023", 64, false, true, 0xFFEFFFFFFFFFFFFFull, N},
      {"1.7976931348623157e308", 64, false, true, 0x7FEFFFFFFFFFFFFFull, N},
      {"", 32, false, false, 0, N},
      {"x", 32, false, false, 0, N},
      {"infinity", 32, false, false, 0, N},
      {"nan(0x1)", 32, false, false, 0, N},
      {"1.5f", 32, false, false, 0, N},
      {"1.0e", 32, false, false, 0, N},
      {"0x", 32, false, false, 0, N},
      {"--1", 32, false, false, 0, N},
      {"1 2", 32, false, false, 0, N},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.text);
    uint64_t v = 0;
    wast::NanClass cls;
    EXPECT_EQ(wast::parse_float(c.text, c.bits, c.allow_class, &v, &cls), c.ok);
    if (c.ok) {
      EXPECT_EQ(v, c.want);
      EXPECT_EQ(static_cast<int>(cls), static_cast<int>(c.cls));
    }
  }
}

/* The conversion of an f32 literal is one rounding. A decimal a hair above the midpoint of two floats is
 * that midpoint as a double, and converting by `strtod` and a cast then rounds the tie to even, to the
 * lower of the two: a different float. The control shows the trap is real and the reader avoids it. */
TEST(Wast, AnF32LiteralIsRoundedOnceNotTwice) {
  // 1 + 2^-24 is the midpoint of 1.0f and 1.0f + 2^-23; the decimal is just above it.
  const std::string above = "1.00000005960464477539062501";
  const std::string below = "1.00000005960464477539062499";
  uint64_t v = 0;
  wast::NanClass cls;
  ASSERT_TRUE(wast::parse_float(above, 32, false, &v, &cls));
  EXPECT_EQ(v, 0x3F800001u) << "above the midpoint rounds up";
  ASSERT_TRUE(wast::parse_float(below, 32, false, &v, &cls));
  EXPECT_EQ(v, 0x3F800000u) << "below the midpoint rounds down";
  // The wrong way gives the wrong float for the first one: the control is able to fail.
  const float twice = static_cast<float>(std::strtod(above.c_str(), nullptr));
  uint32_t twice_bits;
  std::memcpy(&twice_bits, &twice, sizeof twice_bits);
  EXPECT_EQ(twice_bits, 0x3F800000u) << "strtod and a cast rounds twice, to the lower float";
  // The same with the hexadecimal float of more bits than a double holds.
  ASSERT_TRUE(wast::parse_float("0x1.00000100000000000001p+0", 32, false, &v, &cls));
  EXPECT_EQ(v, 0x3F800001u);
}

/* The suite spells one number several ways: hexadecimal and decimal must agree. */
TEST(Wast, TheHexAndDecimalSpellingsOfOneNumberAgree) {
  struct Pair {
    const char * decimal;
    const char * hex;
    unsigned bits;
  };
  const Pair pairs[] = {
      {"16777220.0", "0x1.000004p+24", 32}, {"2147483520.0", "0x1.fffffep+30", 32},
      {"18446742974197923840.0", "0x1.fffffep+63", 32}, {"1.5", "0x1.8p+0", 32},
      {"0.1", "0x1.99999ap-4", 32}, {"1.1234567890", "1.123456789", 32},
      {"1.9", "0x1.e66666p+0", 32}, {"1024.25", "0x1.001p+10", 32},
      {"0.1", "0x1.999999999999ap-4", 64}, {"1.1234567890", "1.123456789", 64},
      {"9007199254740993.0", "0x1p+53", 64}, {"1e23", "0x1.52d02c7e14af6p+76", 64},
      {"4503599627370495.5", "0x1.fffffffffffffp+51", 64},
  };
  for (const Pair & p : pairs) {
    SCOPED_TRACE(std::string(p.decimal) + " vs " + p.hex);
    uint64_t a = 0, b = 1;
    wast::NanClass cls;
    ASSERT_TRUE(wast::parse_float(p.decimal, p.bits, false, &a, &cls));
    ASSERT_TRUE(wast::parse_float(p.hex, p.bits, false, &b, &cls));
    EXPECT_EQ(a, b);
  }
}

namespace {

const char kFloatModule[] = R"wast((module
  (func (export "f32.add") (param $x f32) (param $y f32) (result f32) (f32.add (local.get $x) (local.get $y)))
  (func (export "f32.abs") (param $x f32) (result f32) (f32.abs (local.get $x)))
  (func (export "f32.neg") (param $x f32) (result f32) (f32.neg (local.get $x)))
  (func (export "f32.sqrt") (param $x f32) (result f32) (f32.sqrt (local.get $x)))
  (func (export "f64.neg") (param $x f64) (result f64) (f64.neg (local.get $x)))
  (func (export "f64.eq") (param $x f64) (param $y f64) (result i32) (f64.eq (local.get $x) (local.get $y)))
  (func (export "i32.trunc_f64_s") (param $x f64) (result i32) (i32.trunc_f64_s (local.get $x)))
  (func (export "i64.trunc_sat_f32_u") (param $x f32) (result i64) (i64.trunc_sat_f32_u (local.get $x)))
  (func (export "f64.promote_f32") (param $x f32) (result f64) (f64.promote_f32 (local.get $x)))
)
)wast";

} // namespace

TEST(Wast, TheFloatExpectationsPassWhenRightAndEachCorruptedOneFails) {
  const std::string good = std::string(kFloatModule) +
      "(assert_return (invoke \"f32.add\" (f32.const 1.0) (f32.const 1.0)) (f32.const 2.0))\n"
      "(assert_return (invoke \"f32.add\" (f32.const 1.0) (f32.const 0x1p-24)) (f32.const 1.0))\n"
      "(assert_return (invoke \"f32.neg\" (f32.const 0.0)) (f32.const -0.0))\n"
      "(assert_return (invoke \"f32.sqrt\" (f32.const -1.0)) (f32.const nan:canonical))\n"
      "(assert_return (invoke \"f32.sqrt\" (f32.const -1.0)) (f32.const nan:arithmetic))\n"
      "(assert_return (invoke \"f32.abs\" (f32.const -nan:0x400001)) (f32.const nan:arithmetic))\n"
      "(assert_return (invoke \"f32.abs\" (f32.const -nan:0x200001)) (f32.const nan:0x200001))\n"
      "(assert_return (invoke \"f64.neg\" (f64.const nan:0x4000000000000)) (f64.const -nan:0x4000000000000))\n"
      "(assert_return (invoke \"f64.eq\" (f64.const nan) (f64.const nan)) (i32.const 0))\n"
      "(assert_return (invoke \"f64.eq\" (f64.const -0.0) (f64.const 0.0)) (i32.const 1))\n"
      "(assert_return (invoke \"f64.promote_f32\" (f32.const 0x1.8p+0)) (f64.const 1.5))\n"
      "(assert_return (invoke \"i64.trunc_sat_f32_u\" (f32.const nan)) (i64.const 0))\n"
      "(assert_trap (invoke \"i32.trunc_f64_s\" (f64.const nan)) \"invalid conversion to integer\")\n"
      "(assert_trap (invoke \"i32.trunc_f64_s\" (f64.const inf)) \"integer overflow\")\n"
      "(assert_return (invoke \"i32.trunc_f64_s\" (f64.const -2147483648.9)) (i32.const 0x80000000))\n";
  wast::Report ok = run_text(good);
  if (kExecutes) {
    EXPECT_EQ(ok.passed, 15u);
  }
  EXPECT_EQ(ok.failed, 0u) << (ok.failures.empty() ? "" : ok.failures[0]);
  EXPECT_EQ(ok.guard_checks, 2u) << "the one truncation's two unguarded variants";

  struct Corrupt {
    const char * what;
    const char * form;
    bool decided_by_running;
  };
  const Corrupt bad[] = {
      {"one ulp off", "(assert_return (invoke \"f32.add\" (f32.const 1.0) (f32.const 1.0)) (f32.const 0x1.000002p+1))", true},
      {"one ulp off, in an f64 promotion", "(assert_return (invoke \"f64.promote_f32\" (f32.const 0x1.8p+0)) (f64.const 0x1.8000000000001p+0))", true},
      {"a NaN where a number is expected", "(assert_return (invoke \"f32.sqrt\" (f32.const -1.0)) (f32.const 0.0))", true},
      {"a NaN where infinity is expected", "(assert_return (invoke \"f32.sqrt\" (f32.const -1.0)) (f32.const inf))", true},
      {"a number where a NaN is expected", "(assert_return (invoke \"f32.add\" (f32.const 1.0) (f32.const 1.0)) (f32.const nan:arithmetic))", true},
      {"a number where a canonical NaN is expected", "(assert_return (invoke \"f32.add\" (f32.const 1.0) (f32.const 1.0)) (f32.const nan:canonical))", true},
      {"the wrong NaN class: an arithmetic NaN that is not canonical", "(assert_return (invoke \"f32.abs\" (f32.const -nan:0x400001)) (f32.const nan:canonical))", true},
      {"the wrong NaN class: a NaN that is not quiet", "(assert_return (invoke \"f32.abs\" (f32.const -nan:0x200001)) (f32.const nan:arithmetic))", true},
      {"+0 for -0", "(assert_return (invoke \"f32.neg\" (f32.const 0.0)) (f32.const 0.0))", true},
      {"-0 for +0", "(assert_return (invoke \"f32.neg\" (f32.const -0.0)) (f32.const -0.0))", true},
      {"a stated payload that is not the one", "(assert_return (invoke \"f32.abs\" (f32.const -nan:0x200001)) (f32.const nan:0x200002))", true},
      {"a stated payload with the wrong sign", "(assert_return (invoke \"f32.abs\" (f32.const -nan:0x200001)) (f32.const -nan:0x200001))", true},
      // (A NaN of a stated payload, not a bare `nan`: which sign the hardware gives the default NaN differs (x86-64's is
      // negative, arm64's positive), and a corrupted expectation must fail on either.)
      {"a stated NaN where a canonical one is produced", "(assert_return (invoke \"f32.sqrt\" (f32.const -1.0)) (f32.const nan:0x400001))", true},
      {"a comparison answered wrongly", "(assert_return (invoke \"f64.eq\" (f64.const nan) (f64.const nan)) (i32.const 1))", true},
      {"the wrong trap for a NaN", "(assert_trap (invoke \"i32.trunc_f64_s\" (f64.const nan)) \"integer overflow\")", true},
      {"the wrong trap for a value out of range", "(assert_trap (invoke \"i32.trunc_f64_s\" (f64.const inf)) \"invalid conversion to integer\")", true},
      {"a trap where a value is the answer", "(assert_trap (invoke \"i32.trunc_f64_s\" (f64.const 1.5)) \"integer overflow\")", true},
      {"a value where a trap is the answer", "(assert_return (invoke \"i32.trunc_f64_s\" (f64.const inf)) (i32.const 0))", true},
      {"a trap message it has no guard for", "(assert_trap (invoke \"i32.trunc_f64_s\" (f64.const nan)) \"unreachable\")", false},
      {"a class given as an argument", "(assert_return (invoke \"f32.abs\" (f32.const nan:canonical)) (f32.const nan:arithmetic))", false},
      {"an argument of the wrong float width", "(assert_return (invoke \"f32.abs\" (f64.const 1.0)) (f32.const 1.0))", false},
      {"an argument of an integer type for a float parameter", "(assert_return (invoke \"f32.abs\" (i32.const 1)) (f32.const 1.0))", false},
      {"a result of the wrong float width", "(assert_return (invoke \"f32.abs\" (f32.const 1.0)) (f64.const 1.0))", false},
      {"a float literal it cannot read", "(assert_return (invoke \"f32.abs\" (f32.const 1.0e)) (f32.const 1.0))", false},
      {"a NaN payload that is zero", "(assert_return (invoke \"f32.abs\" (f32.const nan:0x0)) (f32.const nan:arithmetic))", false},
      {"a NaN payload that does not fit", "(assert_return (invoke \"f32.abs\" (f32.const nan:0x800000)) (f32.const nan:arithmetic))", false},
      {"a result form it cannot read", "(assert_return (invoke \"f32.abs\" (f32.const 1.0)) (v128.const i32x4 0 0 0 0))", false},
  };
  for (const Corrupt & c : bad) {
    SCOPED_TRACE(c.what);
    if (c.decided_by_running && !kExecutes) {
      continue;
    }
    wast::Report r = run_text(std::string(kFloatModule) + c.form + "\n");
    EXPECT_EQ(r.failed, 1u) << c.form;
    EXPECT_EQ(r.passed, 0u) << c.form;
  }
}

/* The result word of an f32 is compared whole: a function whose word has bits above 31 is not an f32, whatever
 * its low half says, and the NaN classes refuse such a word as well. */
TEST(Wast, AnF32WithBitsAboveThirtyOneIsNotWhatTheSuiteExpects) {
  if (!kExecutes) {
    GTEST_SKIP() << "the compare is made on the result of running the code, which this host does not do";
  }
  auto wide = [](int64_t value) {
    B b("wide", 0);
    GRJIT_VReg r = b.reg(GRJIT_TYPE_I64);
    b.at(b.block());
    b.cnst(r, value);
    b.ret(V(r));
    return b.finish();
  };
  struct Case {
    const char * form;
    int64_t word;
    size_t failed;
  };
  const Case cases[] = {
      {"(assert_return (invoke \"w\") (f32.const 1.5))", 0x3FC00000, 0},
      {"(assert_return (invoke \"w\") (f32.const 1.5))", 0x100000000ll | 0x3FC00000, 1},
      {"(assert_return (invoke \"w\") (f32.const nan:arithmetic))", 0x7FC00000, 0},
      {"(assert_return (invoke \"w\") (f32.const nan:arithmetic))", 0x100000000ll | 0x7FC00000, 1},
      {"(assert_return (invoke \"w\") (f32.const nan:canonical))", 0x100000000ll | 0x7FC00000, 1},
      {"(assert_return (invoke \"w\") (f32.const nan:canonical))", 0x7FC00000, 0},
      {"(assert_return (invoke \"w\") (f32.const nan:canonical))", 0xFFC00000ll, 0},
      {"(assert_return (invoke \"w\") (f32.const nan:canonical))", 0x7FC00001, 1},
      {"(assert_return (invoke \"w\") (f32.const nan:arithmetic))", 0x7F800001, 1},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.form) + " with the word " + std::to_string(c.word));
    JitWorld w;
    wast::Harness h(&w, "hand.wast");
    h.inject("w", wide(c.word), {}, "f32");
    wast::Report r = h.run(c.form);
    EXPECT_EQ(r.failed, c.failed) << (r.failures.empty() ? "" : r.failures[0]);
    EXPECT_EQ(r.passed, c.failed == 0 ? 1u : 0u);
  }
}

/* ---- The suite ---------------------------------------------------------------------------- */

namespace {

/* The suite file `name`, checked against the digest tools/wasm/SHA256SUMS pins. A missing or altered file
 * is a failure on every host, the ones that do not run the code included. */
bool read_suite_file(const char * name, std::string * text) {
  const std::string path = wast::suite_dir() + "/test/core/" + name;
  std::string why;
  if (!wast::load(path, text, &why)) {
    ADD_FAILURE() << why << " (a missing suite is a failure, never a skip)";
    return false;
  }
  const std::string want = wast::pinned_digest(name);
  const std::string got = wast::sha256_hex(*text);
  if (want.empty() || got != want) {
    ADD_FAILURE() << path << " has SHA-256 " << got << ", tools/wasm/SHA256SUMS pins "
                  << (want.empty() ? "nothing" : want) << "; run tools/wasm/fetch.sh";
    return false;
  }
  return true;
}

/* One file, one arm: the report is checked against the pinned counts. */
wast::Report run_suite_arm(const char * name, const std::string & text, GRJIT_Features disabled,
    size_t want_passed, size_t want_traps, std::map<std::string, size_t> want_excluded, size_t want_guard_checks) {
  const char * arm = disabled == GRJIT_FEATURES_NONE ? "the features the host has" : "every optional feature masked off";
  wast::Report r = run_text(text, name, disabled);
  std::printf("%s [%s]\n", wast::summary(name, r).c_str(), arm);
  const bool requested = floatfx::memcheck_double_rounding_requested();
  EXPECT_FALSE(requested && !floatfx::running_under_valgrind())
      << "the Valgrind-only conversion accommodation requires an active Valgrind client";
  const uint32_t probe = requested ? floatfx::memcheck_double_rounding_probe_bits() : 0x5F000001u;
  EXPECT_TRUE(probe == 0x5F000000u || probe == 0x5F000001u)
      << "the Valgrind u64->f32 conversion probe returned an unrecognized result";
  const bool vex_rounding = requested && floatfx::running_under_valgrind() && probe == 0x5F000000u;
  const bool vex_conversions = vex_rounding && std::strcmp(name, "conversions.wast") == 0;
  if (vex_conversions) {
    // GCC 16.2 produces the spec's bits for all eight inputs outside Valgrind. Valgrind 3.24/VEX
    // double-rounds each under Memcheck; the exact inputs/outputs were independently reproduced by
    // cutil-valgrind-wast-convert-probe-20261018-0441. Keep executing every assertion, count every
    // semantic divergence, and fail on any changed line, function, output, or extra failure.
    const std::vector<std::string> want_vex_failures = {
        "conversions.wast:471: \"f32.convert_i64_s\" got 0x5efffffe, expected 0x5effffff",
        "conversions.wast:472: \"f32.convert_i64_s\" got 0xdf000000, expected 0xdeffffff",
        "conversions.wast:473: \"f32.convert_i64_s\" got 0x5a000000, expected 0x5a000001",
        "conversions.wast:474: \"f32.convert_i64_s\" got 0xda000000, expected 0xda000001",
        "conversions.wast:520: \"f32.convert_i64_u\" got 0x5a000000, expected 0x5a000001",
        "conversions.wast:521: \"f32.convert_i64_u\" got 0x5f000000, expected 0x5effffff",
        "conversions.wast:522: \"f32.convert_i64_u\" got 0x5f000000, expected 0x5f000001",
        "conversions.wast:523: \"f32.convert_i64_u\" got 0x5f7ffffe, expected 0x5f7fffff",
    };
    EXPECT_EQ(r.failures, want_vex_failures) << arm;
    EXPECT_EQ(r.failed, 8u) << arm;
    EXPECT_EQ(r.passed, 585u) << arm;
    EXPECT_EQ(r.passed + r.failed, want_passed) << arm;
    std::printf("Memcheck VEX f32.convert_i64: 8 exact semantic divergences observed [%s]; "
                "ordinary execution checks all 593 assertions\n", arm);
  } else {
    for (size_t i = 0; i < r.failures.size() && i < 20; i++) {
      ADD_FAILURE() << r.failures[i];
    }
    EXPECT_EQ(r.failed, 0u) << arm;
  }
  EXPECT_TRUE(r.unknown_forms.empty());
  EXPECT_EQ(r.traps, want_traps);
  if (kExecutes) {
    EXPECT_EQ(r.passed, want_passed - (vex_conversions ? 8u : 0u))
        << "the number of assertions that ran and held, pinned to wg-3.0 (" << arm << ")";
  } else {
    EXPECT_EQ(r.passed, 0u);
    want_excluded[kNotRunHere] = want_passed;
  }
  // The excluded forms, by reason, exactly: a reason that appears or goes away is a change of what is checked.
  EXPECT_EQ(r.excluded, want_excluded) << arm;
  EXPECT_EQ(r.guard_checks, want_guard_checks)
      << "the verifier's guard rule was shown to refuse the unguarded variants";
  return r;
}

/* Runs a suite file twice where the code is run: with the features the host has, and with every optional
 * feature masked off. The counts of the two are the same pinned ones. The detected run must have compiled
 * feature-arm code on a host that has features (`want_arm_functions` exported functions of the file have a
 * feature arm, and each records it), and the masked run must have compiled none: the fallback sequences were
 * run by this file, not assumed. */
void run_suite_file(const char * name, size_t want_passed, size_t want_traps,
    std::map<std::string, size_t> want_excluded, size_t want_guard_checks, size_t want_arm_functions = 0) {
  std::string text;
  if (!read_suite_file(name, &text)) {
    return;
  }
  wast::Report detected_run =
      run_suite_arm(name, text, GRJIT_FEATURES_NONE, want_passed, want_traps, want_excluded, want_guard_checks);
  if (!kExecutes) {
    return; // nothing is compiled here, so there is no second arm to run
  }
  if (!kFeatureArms) {
    // A backend with no feature arm: nothing to mask. The set every compile recorded is empty, the arm count zero, and
    // the host's detected set is empty too.
    EXPECT_EQ(detected_run.with_arm, 0u) << "a backend with no feature arm recorded one";
    EXPECT_EQ(detected_run.expected_arm, 0u);
    EXPECT_EQ(featfx::detected(), GRJIT_FEATURES_NONE) << "this host's detected feature set is not empty";
    return;
  }
  wast::Report masked_run =
      run_suite_arm(name, text, GRJIT_FEATURES_ALL, want_passed, want_traps, want_excluded, want_guard_checks);
  EXPECT_EQ(detected_run.passed, masked_run.passed) << "the two arms agree on every count";
  EXPECT_EQ(detected_run.excluded, masked_run.excluded);
  EXPECT_EQ(detected_run.returns, masked_run.returns);
  EXPECT_EQ(detected_run.traps, masked_run.traps);
  EXPECT_EQ(detected_run.exports, masked_run.exports);
  EXPECT_EQ(detected_run.compiled, masked_run.compiled);
  EXPECT_EQ(masked_run.with_arm, 0u) << "the masked run compiled feature-arm code";
  EXPECT_EQ(masked_run.expected_arm, 0u);
  // Which exported functions have an arm, and whether the host has the feature it uses.
  EXPECT_EQ(detected_run.with_arm, detected_run.expected_arm)
      << "every function with a feature arm the host has recorded it";
  if (featfx::detected() == GRJIT_FEATURES_ALL) {
    EXPECT_EQ(detected_run.with_arm, want_arm_functions)
        << "the detected run compiled a different number of feature-arm functions than the file has";
    if (want_arm_functions != 0) {
      EXPECT_GT(detected_run.with_arm, 0u) << "a count of zero on a host with every feature fails";
    }
  }
}
} // namespace

TEST(Wast, TheI32SuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  // 364 assert_return and 10 assert_trap run; 83 assert_invalid and 2 assert_malformed do not.
  run_suite_file("i32.wast", 374, 10, {{"assert_invalid", 83}, {"assert_malformed", 2}}, 9, 3);
}

TEST(Wast, TheI64SuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  // 374 assert_return and 10 assert_trap run; 29 assert_invalid and 2 assert_malformed do not.
  run_suite_file("i64.wast", 384, 10, {{"assert_invalid", 29}, {"assert_malformed", 2}}, 9, 3);
}

/* The eight scalar float and conversion files of the same commit. The counts are what the harness reads
 * in them: every assertion is one instruction over its parameters, so each assert_return and
 * assert_trap runs, and only assert_invalid and assert_malformed are left out. The unguarded variants are
 * the two refused for each of the eight partial truncations of conversions.wast. */
TEST(Wast, TheF32SuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  run_suite_file("f32.wast", 2500, 0, {{"assert_invalid", 11}, {"assert_malformed", 2}}, 0, 4);
}

TEST(Wast, TheF64SuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  run_suite_file("f64.wast", 2500, 0, {{"assert_invalid", 11}, {"assert_malformed", 2}}, 0, 4);
}

TEST(Wast, TheF32CmpSuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  run_suite_file("f32_cmp.wast", 2400, 0, {{"assert_invalid", 6}}, 0);
}

TEST(Wast, TheF64CmpSuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  run_suite_file("f64_cmp.wast", 2400, 0, {{"assert_invalid", 6}}, 0);
}

TEST(Wast, TheF32BitwiseSuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  run_suite_file("f32_bitwise.wast", 360, 0, {{"assert_invalid", 3}}, 0);
}

TEST(Wast, TheF64BitwiseSuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  run_suite_file("f64_bitwise.wast", 360, 0, {{"assert_invalid", 3}}, 0);
}

TEST(Wast, TheConversionsSuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  // 526 assert_return and 67 assert_trap run (35 "integer overflow" and 32 "invalid conversion to integer").
  run_suite_file("conversions.wast", 593, 67, {{"assert_invalid", 25}}, 16);
}

TEST(Wast, TheFloatMiscSuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  // Every function of this file at wg-3.0 is one instruction, so none is left out; the survey is in
  // design.md.
  run_suite_file("float_misc.wast", 470, 0, {}, 0, 8);
}

GRJIT_TEST_MAIN()
