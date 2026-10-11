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
#  15  the walk start stored after a native call,   -> testNatives
#      not before it: a collection under the native
#      finds the previous call's
#  16  the stack arguments of a native call not     -> testNatives
#      popped by the caller: rsp drifts
#  17  the stack-argument area not rounded to       -> testNatives
#      sixteen bytes: the native is entered
#      misaligned
#  18  the native-stack check that leaves out the   -> testNatives
#      stack-argument area
#  19  stack argument k of a native call one word   -> testNatives
#      too high: the native reads the wrong words
#
#  38  the verifier does not require the guards    -> testInt_ir (the negative
#      of a division or a remainder                   tests) and testWast (the
#                                                     unguarded variants must be refused)
#  39  `idiv` reached on MIN % -1 (a hardware fault)  -> testInt_ir, testWast
#  40  an i32 shift count masked to 64, not to 32     -> testInt_ir, testWast,
#                                                        testDifferential
#  41  an i32 addition done in 64 bits: the upper     -> testInt_ir, testWast,
#      half is not zero                                  testDifferential
#  42  the operands of an i32 subtraction and of an   -> testInt_ir, testWast,
#      i32 ordered comparison swapped                    testDifferential
#
#  43  an unordered float comparison read as ordered  -> testFloat_ir, testWast,
#      (no test of PF)                                   testDifferential
#  44  the verifier does not require the range guard  -> testFloat_ir (the negative
#      of a float-to-integer truncation                  tests), testWast (the unguarded
#                                                        variants must be refused)
#  45  the operands of a float subtraction swapped    -> testFloat_ir, testWast,
#                                                        testDifferential
#  46  the operands of a float division swapped       -> testFloat_ir, testWast,
#                                                        testDifferential
#  47  the operands of an ordered float comparison    -> testFloat_ir, testWast,
#      swapped the wrong way                             testDifferential
#  48  the second float argument of a helper in the   -> testFloat_ir (the helper ABI,
#      wrong xmm register                                checked against an assembly probe)
#  49  float and integer arguments numbered jointly   -> testFloat_ir (the same)
#  50  min and max by the bare minsd/maxsd           -> testFloat_ir, testWast,
#                                                        testDifferential
#  51  the unsigned 64-bit conversion with no sticky  -> testFloat_ir, testWast,
#      bit (rounds twice)                                testDifferential
#  52  nearest rounds halves away from zero          -> testFloat_ir, testWast,
#                                                        testDifferential
#  53  neg and abs by arithmetic (a signalling NaN    -> testFloat_ir, testWast
#      comes out quiet)
#  54  the saturating truncation of a NaN is not 0   -> testFloat_ir, testWast,
#                                                        testDifferential
#  55  an f32 taken out of its register as 64 bits    -> testFloat_ir, testWast (the
#      (not zero-extended)                               differential folds through a
#                                                        reinterpret, which cuts the word,
#                                                        so it cannot see it)
#
#  56  LZCNT decoded from leaf 1 ECX bit 5 (VMX)      -> testCpu_features (the decoder table);
#      and not from the extended leaf                    and, under a qemu CPU model, the model gate
#  57  leaf 7 read without the maximum basic leaf     -> testCpu_features (the decoder table)
#  58  the feature mask ignored                        -> testCpu_features (the mask and the record),
#                                                        testWast (the masked run compiled feature arms)
#  59  the mask applied without the detected set, so   -> testCpu_features (the algebra against a synthetic
#      a feature the CPU lacks appears                   empty set); and the qemu64 model gate
#  60  the recorded feature set always empty           -> testCpu_features (record, reuse, arm pins),
#                                                        testWast (the detected run recorded no arm)
#  61  the reuse predicate reversed                    -> testCpu_features
#  62  an arm chosen from the detected set, not the    -> testCpu_features (the arm pins), testWast
#      effective one                                     (the masked run compiled feature arms)
#  63  the feature arm never taken                     -> testCpu_features (the arm pins), testWast
#  64  `lzcnt` emitted without its F3 (that is bsr)    -> testAsm (the encoding), testCpu_features
#  65  roundsd's floor and ceil immediates swapped     -> testCpu_features, testWast
#  66  a wrong constant in the v1 popcnt only          -> testCpu_features, testInt_ir and testWast, all
#                                                        through the masked arm (this host has POPCNT)
#  67  a wrong zero case in the v1 clz only           -> the same
#  68  the stamp's names in the wrong order, one lost  -> testCpu_features
#
#  69  an i32 add, subtract and multiply emitted with  -> testDifferential (the arm64 code, executed
#      64-bit registers by the arm64 emitter               by the simulator, against the evaluator)
#  70  an i32 shift or rotate by the 64-bit form       -> the same
#      (the count taken modulo 64) in the arm64 emitter
#  71  an ordered i32 comparison on the whole          -> the same
#      register in the arm64 emitter
#  72  `ctz` without `rbit` in the arm64 emitter       -> the same
#  73  `div_u` and `rem_u` emitted signed by the       -> the same
#      arm64 emitter
#  74  a `GUARD` emitted as nothing by the arm64       -> the same (arm64 never faults in hardware:
#      emitter                                           a zero divisor gives 0 and no trap)
#  75  `rem_s` with the dividend and the divisor       -> the same
#      swapped in its `msub` in the arm64 emitter
#  76  the operands of a float subtraction swapped     -> testDifferential (the arm64 code, simulated)
#      in the arm64 emitter
#  77  the operands of a float division swapped        -> the same
#  78  float lt and le with the integer lt and le      -> the same (a NaN compares true)
#      conditions in the arm64 emitter
#  79  min and max by `fminnm` and `fmaxnm`            -> the same (a quiet NaN is ignored)
#  80  nearest by `frinta` (halves away from zero)     -> the same
#  81  an f32 result stored with `str s`: the upper    -> testArm64_numeric (the simulator, over a
#      half of its slot keeps what it held                stack full of a pattern), testFloat_ir
#  82  the saturating unsigned truncation by the       -> testDifferential (simulated)
#      signed instruction
#  83  an unsigned 64-bit conversion by `scvtf`        -> the same
#  84  the second float argument of a helper in the    -> testArm64_numeric (the simulator's typed
#      wrong register (numbered jointly)                  helper call)
#  85  an f32 helper result taken as `fmov x0, d0`     -> the same
#  86  a float sequence that changes d8 and does not   -> testDifferential (the simulator's check of the
#      restore it                                         callee-saved registers), testArm64_numeric (the
#                                                         structural scan of the registers encoded)
#  87  an i32 or f32 parameter not cut on entry to a   -> not on this host: a callable function's entry
#      callable function                                  is not run by the simulator (tools/arm64-plants.txt)
#  92  an i32 or f32 argument word of the plain entry  -> testDifferential (the simulator)
#      not cut to 32 bits
#  93  a helper's 32-bit result not cut                -> testArm64_numeric (the simulator's typed helper call)
#  94  a move of a 32-bit value not cut                -> not on this host (tools/arm64-plants.txt)
#  95  a left rotation as a right rotation            -> testDifferential
#  96  an i32 extend8_s / extend16_s in the 64-bit     -> testDifferential
#      form
#  97  extend32_s / extend_i32_s stopping at 16 bits   -> testDifferential
#  98  the sign mask of an f32 copysign taken as       -> testArm64_numeric, testDifferential
#      the f64 one
#  99  a demotion that does not convert                -> testDifferential
# 100  a promotion that does not convert               -> testDifferential
#
#  88  the SSE2 rounding sequence unreworked for      -> testWin64_numeric (the registers the
#      Win64: it names the callee-saved xmm6 and xmm7      assembler encoded), testWin64 (objdump)
#  89  a helper's float arguments numbered by class    -> testWin64_numeric (the bytes of the argument
#      as SysV numbers them, not by position (Win64)      loads)
#  90  a helper's stack-passed (fifth and sixth)       -> the same
#      arguments stored without the shadow space's
#      offset (Win64)
#  91  the Win64 flavour given no CPU feature again,   -> testCpu_features (the emission of its arms; under
#      so it takes no arm and records none                wine, the record tests that run the code)
#
# An existing defect in a sequence only the fallback runs is 52 (nearest rounds halves away from zero): on a host
# with SSE4.1 the v1 rounding is run only through a mask of ALL, which testFloat_ir's worked answers and boundary
# sweep, testWast and testDifferential (every subset of the host's features) do explicitly.
#
# Defects 20 to 29 are in the arm64 emitter's calls, tail calls and natives and only arm64 code run
# under qemu-aarch64 can show them; tools/arm64-plants.txt lists them (and 4 again) with the test that
# catches each, suite/tools/xarch/jit-arm64.sh runs them, and this script names them at its end without
# counting them.
#
# An entry is `n:test` or `n:test:filter`. With a filter the test binary runs
# only that gtest (control and planted alike), which is how defect 4 is caught
# on every host: by the differential that executes the arm64 emitter's output
# in the simulator (tests/a64_sim.h), while the same defect in the x86-64
# emitter's twin has no effect there. The catch by real arm64 code, under
# qemu-aarch64, is `suite/tools/xarch/jit-arm64.sh`'s; a host without it says so
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
  # What only arm64 code run under qemu-aarch64 can show, by name and not counted: tools/arm64-plants.txt
  # lists each (the defect, the test that catches it) and suite/tools/xarch/jit-arm64.sh runs them.
  list="$(dirname "$0")/arm64-plants.txt"
  if [ -f "$list" ]; then
    names="$(grep -v '^#' "$list" | grep -v '^$' | awk -F'|' '{printf "%s%s", sep, $1; sep=", "}')"
    printf 'check-planted: SKIPPED on this host, by name: defects %s, each caught by arm64 code run under qemu-aarch64 (needs a cross build; run by suite/tools/xarch/jit-arm64.sh, and not counted here). The simulated catch of 4 above is not that.\n' "$names"
  else
    printf 'check-planted: SKIPPED on this host: tools/arm64-plants.txt is missing, so the arm64 catches (run by suite/tools/xarch/jit-arm64.sh) are not listed\n'
  fi
fi
# What only Win64 code run under wine can show, by name and not counted: tools/win64-plants.txt lists each
# (the defect, the test that catches it) and suite/tools/xwin/m1-controls.sh runs them in a scratch copy of the
# cross-built tree, each with its control passing first. 5 to 7 are also caught structurally above, on any host.
wlist="$(dirname "$0")/win64-plants.txt"
if [ -f "$wlist" ]; then
  wnames="$(grep -v '^#' "$wlist" | grep -v '^$' | awk -F'|' '{printf "%s%s", sep, $1; sep=", "}')"
  printf 'check-planted: SKIPPED on this host, by name: defects %s, each caught by Win64 code run under wine (needs a cross build and a wine; run by suite/tools/xwin/m1-controls.sh, and not counted here).\n' "$wnames"
else
  printf 'check-planted: SKIPPED on this host: tools/win64-plants.txt is missing, so the Win64 catches (run by suite/tools/xwin/m1-controls.sh) are not listed\n'
fi
# What only a qemu CPU model can show, by name and not counted: tools/x86-cpu-plants.txt lists each (the defect, the test, the
# model) and suite/tools/xarch/jit-x86-cpu.sh in the workspace runs them, one tree each, with the control passing first. The
# native tables above catch the same defects, so each is seen to fail in two places.
xlist="$(dirname "$0")/x86-cpu-plants.txt"
if [ -f "$xlist" ]; then
  xnames="$(grep -v '^#' "$xlist" | grep -v '^$' | awk -F'|' '{printf "%s%s", sep, $1; sep=", "}')"
  printf 'check-planted: SKIPPED here, by name: defects %s, each also caught under a qemu CPU model (needs the xarch container and qemu-x86_64; run by suite/tools/xarch/jit-x86-cpu.sh, and not counted here).\n' "$xnames"
else
  printf 'check-planted: SKIPPED here: tools/x86-cpu-plants.txt is missing, so the qemu CPU-model catches (run by suite/tools/xarch/jit-x86-cpu.sh) are not listed\n'
fi
