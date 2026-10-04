#!/bin/sh
#
# Prove that each layering gate fails on the defect it exists to catch.
#
# A gate that has never been seen to fail may be measuring nothing. So this
# runs the real scripts - not copies, not mocks - against tests/gates:
#
#   control/   a tree that is correct; the gate must pass it, so that a gate
#              which rejects everything cannot pass this self-test
#   planted-*  a tree with one defect; the gate must exit non-zero AND name
#              the offending file, line or edge, so that a gate which fails
#              for the wrong reason (a typo, a missing tool) does not count
#   (empty)    an empty directory; the gate must fail rather than report
#              success over a population of zero
#
# The link-line check cannot use a committed fixture, because what it reads is
# a built shared object's NEEDED list. So it builds real ones in a
# temporary directory, against stub libraries: one that links a forbidden
# library, one that links the stubs of the two allowed dependencies (cutil and
# runtime-core), and one that links a BRANCH-suffixed cutil.
#
# Usage: make check-gates   (or CC=cc tools/check-gates.sh)

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
FIX="$ROOT/tests/gates"
CC="${CC:-cc}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# expect_pass <label> <command...>
expect_pass() {
  label="$1"; shift
  checks=$((checks + 1))
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s passes\n' "$label"
  else
    fail "$label should pass but failed:
$out"
  fi
}

# expect_fail <label> <needle> <command...>
# The needle is what the failure message must name.
expect_fail() {
  label="$1"; needle="$2"; shift 2
  checks=$((checks + 1))
  out="$("$@" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif ! printf '%s' "$out" | grep -qF -- "$needle"; then
    fail "$label failed, but without naming '$needle':
$out"
  else
    printf '  ok   %s fails, naming %s\n' "$label" "$needle"
  fi
}

mkdir "$work/empty"

printf 'check-labels\n'
L="$HERE/check-labels.sh"
expect_pass 'labels/control' "$L" "$FIX/labels/control"
expect_fail 'labels/planted-missing' 'nolabel.h' "$L" "$FIX/labels/planted-missing"
expect_fail 'labels/planted-bogus' '"experimental"' "$L" "$FIX/labels/planted-bogus"
expect_fail 'labels/planted-prose-only' 'top.h has no @stability' \
  "$L" "$FIX/labels/planted-prose-only"
expect_fail 'labels/planted-stable' 'top.h is labelled stable' \
  "$L" "$FIX/labels/planted-stable"
expect_fail 'labels/planted-two-labels' 'more than one @stability' \
  "$L" "$FIX/labels/planted-two-labels"
expect_fail 'labels/empty' 'measuring nothing' "$L" "$work/empty"

printf 'check-edges --includes\n'
E="$HERE/check-edges.sh"
expect_pass 'edges/control' "$E" --includes "$FIX/edges/control"
expect_fail 'edges/planted-heap-src' 'runtime-jit -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-heap-src"
expect_fail 'edges/planted-heap-src names the file' 'x.c' \
  "$E" --includes "$FIX/edges/planted-heap-src"
expect_fail 'edges/planted-heap-header' 'runtime-jit -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-heap-header"
expect_fail 'edges/planted-lang-tang-src' 'runtime-jit -> lang-tang' \
  "$E" --includes "$FIX/edges/planted-lang-tang-src"
expect_fail 'edges/planted-lang-tang-header' 'runtime-jit -> lang-tang' \
  "$E" --includes "$FIX/edges/planted-lang-tang-header"
expect_fail 'edges/planted-debug' 'runtime-jit -> runtime-debug' \
  "$E" --includes "$FIX/edges/planted-debug"
expect_fail 'edges/planted-ctang' 'runtime-jit -> tang' \
  "$E" --includes "$FIX/edges/planted-ctang"
expect_fail 'edges/planted-text' 'runtime-jit -> text' \
  "$E" --includes "$FIX/edges/planted-text"
expect_fail 'edges/planted-quoted' 'runtime-jit -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-quoted"
expect_fail 'edges/empty (includes)' 'measuring nothing' \
  "$E" --includes "$work/empty"

expect_fail 'edges/planted-relative' 'runtime-jit -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-relative"
expect_fail 'edges/planted-example' 'runtime-jit -> runtime-heap' \
  "$E" --includes "$FIX/edges/planted-example"

printf 'check-edges --links\n'
# The .dll arm (objdump -p is the reader there) has run under wine, cross-built
# (tools/xwin in the workspace); it has not run on a Windows machine.
case "$(uname -s)" in
  MINGW* | MSYS*) SHEXT=dll; SHFLAGS="-shared" ;;
  Darwin) SHEXT=dylib; SHFLAGS="-dynamiclib" ;;
  *) SHEXT=so; SHFLAGS="-shared -fPIC" ;;
esac
stubs="$work/stubs"
mkdir -p "$stubs" "$work/planted" "$work/control" "$work/dev" "$work/heap" "$work/text"
printf 'int stub_debug(void) { return 1; }\n' > "$work/debug.c"
printf 'int stub_cutil(void) { return 2; }\n' > "$work/cutil.c"
printf 'int stub_core(void) { return 3; }\n' > "$work/core.c"
printf 'int stub_text(void) { return 4; }\n' > "$work/textstub.c"
printf 'int stub_heap(void) { return 5; }\n' > "$work/heapstub.c"
printf 'int stub_debug(void);\nint planted(void) { return stub_debug(); }\n' \
  > "$work/planted.c"
printf 'int stub_heap(void);\nint planted(void) { return stub_heap(); }\n' \
  > "$work/heap.c"
printf 'int stub_text(void);\nint planted(void) { return stub_text(); }\n' \
  > "$work/textuse.c"
printf 'int stub_cutil(void);\nint stub_core(void);\nint control(void) { return stub_cutil() + stub_core(); }\n' \
  > "$work/control.c"
printf 'int stub_cutil(void);\nint control(void) { return stub_cutil(); }\n' \
  > "$work/dev.c"

built=1
# shellcheck disable=SC2086
{
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-debug-0.$SHEXT" "$work/debug.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-heap-0.$SHEXT" "$work/heapstub.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-text-0.$SHEXT" "$work/textstub.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-0.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-core-0.$SHEXT" "$work/core.c" &&
  $CC $SHFLAGS -o "$work/planted/libplanted.$SHEXT" "$work/planted.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-debug-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/heap/libheap.$SHEXT" "$work/heap.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-heap-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/text/libtext.$SHEXT" "$work/textuse.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-text-0.$SHEXT &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-dev.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$work/dev/libdev.$SHEXT" "$work/dev.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-dev.$SHEXT &&
  $CC $SHFLAGS -o "$work/control/libcontrol.$SHEXT" "$work/control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-runtime-core-0.$SHEXT
} >"$work/build.log" 2>&1 || built=0
if [ "$built" -eq 0 ]; then
  fail "could not build the link-line fixtures:
$(cat "$work/build.log")"
else
  expect_pass 'links/control (cutil and runtime-core)' "$E" --links "$work/control"
  expect_pass 'links/cutil with a BRANCH suffix (-dev)' "$E" --links "$work/dev"
  expect_fail 'links/planted-debug' 'runtime-jit -> runtime-debug' \
    "$E" --links "$work/planted"
  expect_fail 'links/planted-debug names the object' 'libplanted' \
    "$E" --links "$work/planted"
  expect_fail 'links/planted-heap' 'runtime-jit -> runtime-heap' \
    "$E" --links "$work/heap"
  expect_fail 'links/planted-text (a dependency of the debugger, not of this library)' \
    'runtime-jit -> text' "$E" --links "$work/text"
fi
expect_fail 'links/empty' 'measuring nothing' "$E" --links "$work/empty"

# A name that merely begins with an allowed one is another library, not that
# library with a branch suffix.
extra="$work/extra"
mkdir -p "$extra" "$work/extra-stubs"
printf 'int stub_extra(void) { return 4; }\n' > "$work/extra-stub.c"
printf 'int stub_extra(void);\nint extended(void) { return stub_extra(); }\n' > "$work/extended.c"
# shellcheck disable=SC2086
if $CC $SHFLAGS -o "$work/extra-stubs/libghoti.io-cutil-extra-0.$SHEXT" "$work/extra-stub.c" &&
  $CC $SHFLAGS -o "$extra/libextended.$SHEXT" "$work/extended.c" \
    -L"$work/extra-stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-extra-0.$SHEXT \
    >"$work/extra-build.log" 2>&1; then
  expect_fail 'links/planted-name-extending-an-allowed-one' 'runtime-jit -> cutil-extra' \
    "$E" --links "$extra"
else
  fail "could not build the extended-name fixture"
fi

if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: all %d checks behaved: each gate fails on its planted defect, passes its control, and fails on an empty population\n' \
  "$checks"
