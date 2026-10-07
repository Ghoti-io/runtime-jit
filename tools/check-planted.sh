#!/bin/sh
#
# Prove that the backend's two instruments catch the defects they exist to
# catch.
#
# A differential that has never been seen to disagree, and a read-back that has
# never been seen to read the wrong slot, may measure nothing. So each defect is
# compiled into the library objects of a tree of its own, by the macro
# GRJIT_TEST_PLANT_BUG (see Makefile's "Planted defects"), and the test meant to
# catch it is built and run there:
#
#   1  SHR and SAR swapped in the x86-64 emitter   -> testDifferential
#   2  every stack-map slot recorded 8 bytes off   -> testReadback
#   3  one live REF left out of every stack map    -> testReadback
#   4  SHR and SAR swapped in the arm64 emitter    -> testDifferential, the
#                                                     test that runs arm64 code
#   5  a callee-saved register (rsi) used by the   -> testWin64
#      Windows x86-64 flavour
#   6  the Windows x86-64 flavour with no           -> testWin64
#      outgoing area (shadow space)
#   7  the Windows x86-64 unwind table written      -> testWin64
#      and never registered
#   8  one live REF left out of the stack map of    -> testCalls
#      the site a call to compiled code returns to
#   9  a callee-saved register (rbx, r12) used      -> testCalls
#      and not restored by callable code
#  10  the native-stack check that leaves out the   -> testCalls
#      frame it is about to make
#  11  the last stack argument of a tail call is    -> testTail
#      not copied to the callee's place
#  12  the tail hook's site leaves its arguments    -> testTail
#      area out of the stack map
#  13  no padding between a frame's registers and   -> testTail
#      its arguments area (the staging area
#      overlaps the place the stack arguments go)
#  14  a tail call made as a call, its frame kept:  -> testTail
#      the answer is right and the native stack
#      grows with every tail call
#
# An entry is `n:test` or `n:test:filter`. With a filter the test binary runs
# only that gtest (control and planted alike), which is how defect 4 is caught
# on every host: by the differential that executes the arm64 emitter's output
# in the simulator (tests/a64_sim.h), while the same defect in the x86-64
# emitter's twin has no effect there. The catch by real arm64 code, under
# qemu-aarch64, is `tools/xarch/jit-arm64.sh`'s; a host without it says so
# below, by name, and does not count it.
#
# For each one, three things must hold, in this order:
#
#   - the control passes: the same test, built without the macro, passes (so a
#     test that fails for any reason, a bad build or a missing tool, cannot be
#     mistaken for a catch);
#   - the planted tree builds (a build failure is not a catch either);
#   - the planted test exits non-zero AND says FAILED (a crash or a signal is
#     not a verdict).
#
# Usage: check-planted.sh <make> "<n:test ...>" <ld-dir> <apps-dir> <build-dir>

set -u

MAKE_CMD="${1:?usage: check-planted.sh <make> <defects> <ld-dir> <apps-dir> <build-dir>}"
DEFECTS="${2:?missing defect list}"
LD_DIR="${3:?missing library directory}"
APPS="${4:?missing apps directory}"
BUILD="${5:?missing build directory}"

if [ -z "$DEFECTS" ]; then
  printf 'check-planted: no defects listed; this gate is measuring nothing\n' >&2
  exit 1
fi

failures=0
count=0
for entry in $DEFECTS; do
  n="${entry%%:*}"
  rest="${entry#*:}"
  test_name="${rest%%:*}"
  filter=""
  case "$rest" in
    *:*) filter="--gtest_filter=${rest#*:}" ;;
  esac
  count=$((count + 1))
  tree="$BUILD-plant-$n"

  # The control.
  if ! $MAKE_CMD --no-print-directory "$APPS/$test_name" >/dev/null 2>&1; then
    printf 'check-planted: FAIL: the control %s did not build\n' "$test_name" >&2
    failures=$((failures + 1))
    continue
  fi
  if ! LD_LIBRARY_PATH="$LD_DIR:$APPS" "$APPS/$test_name" --gtest_brief=1 $filter >/dev/null 2>&1; then
    printf 'check-planted: FAIL: the control %s does not pass without the planted defect\n' \
      "$test_name" >&2
    failures=$((failures + 1))
    continue
  fi

  # The planted tree.
  if ! $MAKE_CMD --no-print-directory "$tree/apps/$test_name" \
      BUILD_DIR="$tree" EXTRA_CFLAGS="-DGRJIT_TEST_PLANT_BUG=$n" >"$tree.log" 2>&1; then
    printf 'check-planted: FAIL: defect %s: the planted tree did not build (see %s.log)\n' \
      "$n" "$tree" >&2
    failures=$((failures + 1))
    continue
  fi
  out="$(LD_LIBRARY_PATH="$LD_DIR:$tree/apps" "$tree/apps/$test_name" --gtest_brief=1 $filter 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    printf 'check-planted: FAIL: defect %s was not caught by %s\n' "$n" "$test_name" >&2
    failures=$((failures + 1))
  elif ! printf '%s' "$out" | grep -q 'FAILED'; then
    printf 'check-planted: FAIL: defect %s: %s exited %s without reporting a failure\n' \
      "$n" "$test_name" "$rc" >&2
    failures=$((failures + 1))
  else
    printf '  ok   defect %s is caught by %s (control passes, planted fails)\n' "$n" "$test_name"
  fi
done

if [ "$failures" -ne 0 ]; then
  exit 1
fi
printf 'check-planted: all %d planted defects were caught, and each control passes\n' "$count"
if [ "$(uname -m)" != "aarch64" ]; then
  printf 'check-planted: SKIPPED on this host, by name: defect 4 caught by arm64 code run under qemu-aarch64 (needs a cross build; run by tools/xarch/jit-arm64.sh). The simulated catch above is not that.\n'
fi
