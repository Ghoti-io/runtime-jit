#!/bin/sh
#
# Prove that what `make install` leaves behind is usable by a consumer that
# knows nothing about this source tree.
#
# Everything else this project checks is checked in-tree, against headers in
# include/ and a library in build/, reached by paths the Makefile already
# knows. A consumer reaches none of those: it asks pkg-config for a module
# name, compiles against whatever -I it is handed, and links against whatever
# -L and -l it is handed. That is a different code path, and CONVENTIONS.md
# section 1 records three defects that lived in it while every in-tree build
# stayed green - a .pc that emitted "-I <path>", which pkg-config splits into
# two arguments; one that omitted a "/" and named a directory that does not
# exist; and six of seven that emitted an empty Version:, because VERSION was
# substituted into the template but defined in only one Makefile.
#
# So each check below is named for the defect it would have caught, and the
# last one is the only check that proves the whole path at once: compile a
# program that includes only the installed umbrella header, link it with only
# the flags pkg-config gives, and run it.
#
# Usage:
#   tools/check-install.sh <prefix> [module-name]
#
# The module name defaults to ghoti.io-runtime-jit-0, which is what an ordinary
# build of 0.x installs; pass it explicitly for a build that overrode BRANCH.

set -eu

PREFIX="${1:?usage: check-install.sh <prefix> [module-name]}"
MODULE="${2:-ghoti.io-runtime-jit-0}"

# Where `make install PREFIX=...` writes the .pc file. Prepended rather than
# replacing the caller's, so a prefix holding only this library still resolves
# the Requires: on cutil from wherever that was installed.
PKG_CONFIG_PATH="$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

CC="${CC:-cc}"
CXX="${CXX:-g++}"

fail() {
  printf '\033[0;31mcheck-install: %s\033[0m\n' "$*" >&2
  exit 1
}

pass() {
  printf '  \033[0;32mok\033[0m  %s\n' "$*"
}

printf '\033[0;36m\n'
printf '###################################################\n'
printf '### Consuming the installed library, as a caller ###\n'
printf '###################################################\n'
printf '\033[0m\n'
printf 'module:  %s\n' "$MODULE"
printf 'prefix:  %s\n\n' "$PREFIX"

# ---------------------------------------------------------------------------
# 1. The module is findable at all.
# ---------------------------------------------------------------------------
if ! pkg-config --exists "$MODULE"; then
  fail "pkg-config cannot find $MODULE.
  Searched: $PKG_CONFIG_PATH
  An install writes \$PREFIX/share/pkgconfig/<module>.pc; if that file is
  there, pkg-config is failing on it - run: pkg-config --print-errors --exists $MODULE"
fi
pass "pkg-config finds $MODULE"

# ---------------------------------------------------------------------------
# 2. Version: is not empty.
#
# An empty Version: makes every version constraint a consumer writes fail -
# `Requires: ghoti.io-runtime-jit-0 >= 0.0.0` stops resolving - and pkg-config
# reports it as a missing module rather than as a malformed one.
# ---------------------------------------------------------------------------
version="$(pkg-config --modversion "$MODULE" 2>/dev/null || true)"
if [ -z "$version" ]; then
  fail "$MODULE declares an empty Version:.
  The .pc template's (VERSION) placeholder was not substituted at install."
fi
pass "Version: $version"

# ---------------------------------------------------------------------------
# 3. No placeholder survived substitution.
#
# The template carries (SUITE), (PROJECT), (BRANCH), (VERSION), (LIB),
# (INCLUDE) and (REQUIRES). A missed one is not a build failure anywhere: it
# becomes a literal directory name that simply does not exist.
# ---------------------------------------------------------------------------
flags="$(pkg-config --cflags --libs "$MODULE")"
case "$flags" in
  *'('*)
    fail "the installed .pc still contains an unsubstituted placeholder:
  $flags"
    ;;
esac
pass "no unsubstituted placeholders"

# ---------------------------------------------------------------------------
# 4. No flag was split from its argument.
#
# `-I <path>` in a .pc is two arguments to every consumer, and the compiler
# then reads the path as a source file. A bare -I or -L token is the signature.
# ---------------------------------------------------------------------------
for tok in $flags; do
  case "$tok" in
    -I|-L|-l)
      fail "the installed .pc emits a bare '$tok' with its argument separated:
  $flags"
      ;;
  esac
done
pass "every flag carries its argument"

# ---------------------------------------------------------------------------
# 5. Every directory named actually exists.
#
# This is the defect that a single missing "/" produces, and the one an
# in-tree build can never see.
# ---------------------------------------------------------------------------
for tok in $flags; do
  case "$tok" in
    -I*)
      dir="${tok#-I}"
      [ -d "$dir" ] || fail "include directory does not exist: $dir"
      ;;
    -L*)
      dir="${tok#-L}"
      [ -d "$dir" ] || fail "library directory does not exist: $dir"
      ;;
  esac
done
pass "every -I and -L directory exists"

# ---------------------------------------------------------------------------
# 6. A consumer compiles, links, runs, and gets its answers back.
#
# The in-tree tests cannot see a symbol the shared library fails to export:
# they link the static archive with --whole-archive.
# ---------------------------------------------------------------------------
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

cat > "$work/consumer.c" <<'EOF'
// A consumer of the installed library: the umbrella header, runtime-core's
// (a host makes its own contexts), and nothing that knows where the source
// tree is. It builds a function, compiles it, runs it, and reads the emitted
// metadata through runtime-core's format.
#include <ghoti.io/runtime-jit/runtime-jit.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdio.h>
#include <string.h>

static uint32_t poll_helper(void * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  (void)offset;
  return 0;
}

int main(void) {
  // Every result code has a description.
  for (int i = 0; i < (int)GRJIT_RESULT_COUNT; i++) {
    const char * text = grjit_result_string((GRJIT_Result)i);
    if (text == NULL || strcmp(text, "Unknown error") == 0) {
      fprintf(stderr, "consumer: result %d has no description\n", i);
      return 1;
    }
  }
  if (grjit_allocator_default() == NULL) {
    fprintf(stderr, "consumer: no default allocator\n");
    return 1;
  }
  GRJIT_Limits limits;
  grjit_limits_default(&limits);
  if (limits.max_blocks != 4096) {
    fprintf(stderr, "consumer: the limits are not the defaults\n");
    return 1;
  }
  if (!grjit_backend_available()) {
    fprintf(stderr, "consumer: no backend on this target\n");
    return 1;
  }
  // The whole path: build f(x) = x + 41 with a poll, compile it into pages
  // from a context's page provider, run it, and read back its metadata. This is
  // what the in-tree tests cannot see, because they link the static archive.
  GRJIT_Builder * b;
  GRJIT_VReg x, d;
  GRJIT_BlockId entry;
  if (grjit_builder_create("f", 0, NULL, NULL, &b) != GRJIT_OK ||
      grjit_builder_param(b, GRJIT_TYPE_I64, &x) != GRJIT_OK ||
      grjit_builder_vreg(b, GRJIT_TYPE_I64, &d) != GRJIT_OK ||
      grjit_builder_block(b, &entry) != GRJIT_OK ||
      grjit_builder_set_block(b, entry) != GRJIT_OK ||
      grjit_builder_set_poll_helper(b, poll_helper) != GRJIT_OK) {
    fprintf(stderr, "consumer: could not start a function\n");
    return 1;
  }
  GRCORE_PollIdentity identity = {1, 2};
  if (grjit_builder_poll(b, identity, NULL, 0) != GRJIT_OK ||
      grjit_builder_binary(b, GRJIT_OP_ADD, d, grjit_operand_vreg(x), grjit_operand_imm(41)) != GRJIT_OK ||
      grjit_builder_ret(b, grjit_operand_vreg(d)) != GRJIT_OK) {
    fprintf(stderr, "consumer: could not build a function\n");
    return 1;
  }
  GRJIT_Function * f;
  if (grjit_builder_finish(b, &f) != GRJIT_OK ||
      grjit_function_verify(f, NULL, NULL, 0) != GRJIT_OK) {
    fprintf(stderr, "consumer: the function did not verify\n");
    return 1;
  }
  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    fprintf(stderr, "consumer: no context\n");
    return 1;
  }
  GRJIT_CompileOptions options;
  memset(&options, 0, sizeof options);
  options.pages = grcore_context_page_provider(context);
  GRJIT_Code * code;
  if (grjit_compile(&options, f, &code) != GRJIT_OK) {
    fprintf(stderr, "consumer: the function did not compile\n");
    return 1;
  }
  uint64_t args[1] = {1};
  uint64_t out[2] = {0, 0};
  if (grjit_code_call(code, context, args, out) != GRJIT_EXIT_RETURNED || out[0] != 42) {
    fprintf(stderr, "consumer: the compiled function answered %llu\n", (unsigned long long)out[0]);
    return 1;
  }
  const GRCORE_CodeMeta * meta = grjit_code_meta(code);
  if (meta == NULL || meta->site_count != 1 || meta->sites[0].kind != GRCORE_SITE_GC_POINT_POLL ||
      meta->sites[0].identity.offset != 2 ||
      grcore_codemeta_find(meta, meta->sites[0].code_offset) != &meta->sites[0]) {
    fprintf(stderr, "consumer: the metadata is not what a poll emits\n");
    return 1;
  }
  grjit_code_destroy(code);
  grjit_function_destroy(f);
  if (grcore_context_memory_blocks(context) != 0 ||
      grcore_context_destroy(context) != GRCORE_OK ||
      grcore_group_destroy(group) != GRCORE_OK) {
    fprintf(stderr, "consumer: teardown failed\n");
    return 1;
  }
  printf("%s\n", grjit_version_string());
  return 0;
}
EOF

# The prefix is not on the default loader path, so the consumer is given an
# rpath to it - the same thing the Makefile does for everything it builds, and
# what a consumer of a non-system prefix has to do. A system install has its
# directory in ld.so.conf instead and needs neither.
rpath=""
for tok in $flags; do
  case "$tok" in
    -L*) rpath="$rpath -Wl,-rpath,${tok#-L}" ;;
  esac
done

# shellcheck disable=SC2086
$CC -std=c17 -Wall -Wextra -Werror -o "$work/consumer" "$work/consumer.c" \
    $flags $rpath || fail "the consumer did not compile or link against the installed library"
pass "a C consumer compiles and links"

"$work/consumer" || fail "the consumer did not run"
pass "the installed library answers through the installed headers"

# The public headers are compiled as C++ in-tree because that is how the tests
# are built; this proves the *installed* copies are C++-consumable too, which
# is a different set of files.
# shellcheck disable=SC2086
# -x c++ before the file, not after: it applies to the inputs that follow it,
# and the suffix is .c.
$CXX -std=c++20 -Wall -Wextra -Werror -o "$work/consumer++" \
    -x c++ "$work/consumer.c" $flags $rpath \
    || fail "the installed headers are not C++-consumable"
pass "a C++ consumer compiles and links"

# ---------------------------------------------------------------------------
# 7. The library and its .pc agree about what version this is.
#
# They come from the same Makefile variables, so a disagreement means one of
# the two was substituted from a stale value.
# ---------------------------------------------------------------------------
reported="$("$work/consumer" | tail -n 1)"
case "$reported" in
  "$version"*) ;;
  *)
    fail "the library reports version '$reported' but its .pc says '$version'"
    ;;
esac
pass "the library agrees with its .pc about the version"

printf '\n\033[0;32mThe installed library is consumable.\033[0m\n\n'
