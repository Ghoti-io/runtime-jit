/**
 * @file
 *
 * The Wasm spec-suite harness (spec-runtime-float, story 2): reads the WebAssembly spec suite's
 * `i32.wast` and `i64.wast` at the `wg-3.0` tag, turns each exported single-instruction function
 * into an IR function (with the guards of a partial operation written in front of it), compiles it
 * and runs every `assert_return` and `assert_trap` against the suite's own expectations. This is
 * the independent oracle of the integer IR: the suite is not ours, and says what Wasm means.
 *
 * It is a reader for exactly the forms these two files use, not a Wasm frontend and no API. What it
 * cannot run is counted as excluded, with its reason, and never as passed: `assert_invalid`,
 * `assert_malformed`, `assert_exhaustion`, `assert_unlinkable`, `register`, `module binary` and
 * `module quote`, and a function whose body is not one supported instruction over its parameters
 * (with every assertion that invokes it). A top-level form that is neither handled nor on that list
 * fails the run, so a form the reader does not know cannot vanish. A missing suite fails the test on
 * a host that runs it (tools/wasm/fetch.sh fetches it); it is never skipped there.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../int_fixture.h"

#include "../../src/code/code_internal.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

using namespace intfx;

/* The harness runs the compiled code where the integer operations are emitted: x86-64 SysV. Elsewhere
 * (arm64, Win64) the compile is refused by name (testInt_ir shows it) and what is left to do is the
 * reader, the mapping into the IR and the verifier's guard rule, which run wherever the suite is. */
#if defined(__x86_64__) && defined(__linux__)
constexpr bool kExecutes = true;
#else
constexpr bool kExecutes = false;
#endif

namespace wast {

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
  Harness(JitWorld * world, std::string file) : world_(world), file_(std::move(file)) {}
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
  Report report_;
  std::map<std::string, Instance> instances_; // by module id; "" for an anonymous one
  std::string current_;                       // the id of the latest module
};

/* A numeric constant node `(i32.const N)` / `(i64.const N)`; false if it is neither. */
bool constant(const Node & n, std::string * type, uint64_t * word) {
  if (!n.list || n.kids.size() != 2 || n.kids[1].list) {
    return false;
  }
  const std::string & h = n.head();
  if (h != "i32.const" && h != "i64.const") {
    return false;
  }
  *type = h.substr(0, 3);
  return parse_int(n.kids[1].atom, h == "i32.const" ? 32 : 64, word);
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
  WasmOp op = build_wasm_op(instr);
  if (op.function == nullptr) {
    exclude("an instruction outside the integer table");
    add(true, "an instruction outside the integer table", instr);
    return true;
  }
  // The suite's signature and the table's must be the same one.
  const bool wide_result = op.result == GRJIT_TYPE_I64;
  const std::string want_result = wide_result ? "i64" : "i32";
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
    if ((t == GRJIT_TYPE_I32 ? "i32" : "i64") != param_types[k]) {
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
  if (op.partial) {
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
    e->op = first ? op : build_wasm_op(instr);
    first = false;
    if (world_ != nullptr) {
      GRJIT_CompileOptions o{};
      o.pages = world_->pages();
      GRJIT_Result r = grjit_compile(&o, e->op.function, &e->code);
      if (r != GRJIT_OK) {
        e->compile_failure = std::string("the compile of ") + instr + " failed: " + grjit_result_string(r);
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
    std::string type;
    uint64_t word = 0;
    if (!constant(n.kids[k + 1 + a], &type, &word)) {
      *why = "an argument that is not an i32.const or i64.const";
      return false;
    }
    if (type != (*exp)->params[a]) {
      *why = "an argument of type " + type + " for a parameter of type " + (*exp)->params[a];
      return false;
    }
    args->push_back(word);
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
  std::vector<uint64_t> want;
  std::vector<std::string> want_types;
  for (size_t k = 2; k < n.kids.size(); k++) {
    std::string type;
    uint64_t word = 0;
    if (!constant(n.kids[k], &type, &word)) {
      fail(n, "an expected result that is not an i32.const or i64.const (a form this reader does not read)");
      return;
    }
    want.push_back(word);
    want_types.push_back(type);
  }
  if (want.size() != 1 || want_types[0] != e->result) {
    fail(n, "the expected result does not match the function's result type " + e->result);
    return;
  }
  if (!kExecutes) {
    exclude("not run on this host (the integer operations are emitted for x86-64 SysV)");
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
    fail(n, "\"" + e->name + "\" trapped (" + (s != nullptr ? trap_name(s->identity.offset) : "?") +
                "), the suite expects a value");
  } else if (o.exit != GRJIT_EXIT_RETURNED) {
    fail(n, "\"" + e->name + "\" exited " + std::to_string(o.exit));
  } else if (o.out[0] != want[0]) {
    // An i32 is compared as a whole word: its upper half must be zero.
    std::snprintf(buf, sizeof buf, " got 0x%" PRIx64 ", expected 0x%" PRIx64, o.out[0], want[0]);
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
  } else {
    fail(n, "a trap message this harness has no guard for: \"" + text + "\"");
    return;
  }
  if (!kExecutes) {
    exclude("not run on this host (the integer operations are emitted for x86-64 SysV)");
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
      fail(n, "\"" + e->name + "\" trapped with \"" + trap_name(s->identity.offset) +
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
        exclude("not run on this host (the integer operations are emitted for x86-64 SysV)");
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
  return o.str();
}

/* Where the suite is: the commit is the pin file's, the data is fetched by tools/wasm/fetch.sh. */
std::string suite_dir() {
  if (const char * d = std::getenv("GRJIT_WASM_SUITE_DIR")) {
    return d;
  }
  std::ifstream pin("tools/wasm/WASM_SPEC_COMMIT");
  std::string commit;
  std::getline(pin, commit);
  while (!commit.empty() && std::isspace(static_cast<unsigned char>(commit.back()))) {
    commit.pop_back();
  }
  return "third_party/wasm-spec/" + commit;
}

} // namespace wast

namespace {

/* Runs `text` and returns the report; the world is only made where the code is run. */
wast::Report run_text(const std::string & text, const char * name = "inline.wast") {
  if (kExecutes) {
    JitWorld w;
    wast::Harness h(&w, name);
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
      {"a value whose upper half is not zero", "(assert_return (invoke \"add\" (i32.const -1) (i32.const 1)) (i32.const 0x100000000))", false},
      {"a value where a trap is the answer", "(assert_return (invoke \"div_s\" (i32.const 1) (i32.const 0)) (i32.const 0))", true},
      {"a trap where a value is the answer", "(assert_trap (invoke \"add\" (i32.const 1) (i32.const 1)) \"integer divide by zero\")", true},
      {"the wrong trap", "(assert_trap (invoke \"div_s\" (i32.const 1) (i32.const 0)) \"integer overflow\")", true},
      {"the other wrong trap", "(assert_trap (invoke \"div_s\" (i32.const 0x80000000) (i32.const -1)) \"integer divide by zero\")", true},
      {"a trap message it has no guard for", "(assert_trap (invoke \"div_s\" (i32.const 1) (i32.const 0)) \"unreachable\")", false},
      {"an argument of the wrong type", "(assert_return (invoke \"add\" (i64.const 1) (i32.const 1)) (i32.const 2))", false},
      {"a result of the wrong type", "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (i64.const 2))", false},
      {"too few arguments", "(assert_return (invoke \"add\" (i32.const 1)) (i32.const 2))", false},
      {"an export that does not exist", "(assert_return (invoke \"nope\" (i32.const 1)) (i32.const 2))", false},
      {"a result form it cannot read", "(assert_return (invoke \"add\" (i32.const 1) (i32.const 1)) (f32.const 2))", false},
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
  EXPECT_EQ(r.excluded["an instruction outside the integer table"], 1u);
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
  EXPECT_FALSE(wast::load("third_party/wasm-spec/does-not-exist/i32.wast", &text, &why));
  EXPECT_NE(why.find("tools/wasm/fetch.sh"), std::string::npos) << why;
}

/* ---- The suite ---------------------------------------------------------------------------- */

namespace {

void run_suite_file(const char * name, size_t want_passed, size_t want_excluded, size_t want_traps) {
  const std::string path = wast::suite_dir() + "/test/core/" + name;
  std::string text, why;
  if (!wast::load(path, &text, &why)) {
    if (kExecutes) {
      FAIL() << why << " (a missing suite is a failure, never a skip)";
    }
    GTEST_SKIP() << why << " (this host does not run the compiled code, so it does not insist; an x86-64 SysV host fails)";
  }
  wast::Report r = run_text(text, name);
  std::printf("%s\n", wast::summary(name, r).c_str());
  for (size_t i = 0; i < r.failures.size() && i < 20; i++) {
    ADD_FAILURE() << r.failures[i];
  }
  EXPECT_EQ(r.failed, 0u);
  EXPECT_TRUE(r.unknown_forms.empty());
  EXPECT_EQ(r.traps, want_traps);
  if (kExecutes) {
    EXPECT_EQ(r.passed, want_passed) << "the number of assertions that ran and held, pinned to wg-3.0";
    EXPECT_EQ(r.excluded_total(), want_excluded);
  } else {
    EXPECT_EQ(r.passed, 0u);
  }
  EXPECT_GT(r.guard_checks, 8u) << "the verifier's guard rule was shown to refuse the unguarded variants";
}

} // namespace

TEST(Wast, TheI32SuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  // 364 assert_return and 10 assert_trap run; 83 assert_invalid and 2 assert_malformed do not.
  run_suite_file("i32.wast", 374, 85, 10);
}

TEST(Wast, TheI64SuiteFilePassesEveryAssertionItHoldsAndCountsTheRest) {
  // 374 assert_return and 10 assert_trap run; 29 assert_invalid and 2 assert_malformed do not.
  run_suite_file("i64.wast", 384, 31, 10);
}

GRJIT_TEST_MAIN()
