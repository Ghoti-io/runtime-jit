#!/usr/bin/env python3
"""Run every selected GTest once, with a full-completion and skip audit.

Copyright 2026 by Corey Pennycuff
SPDX-License-Identifier: LGPL-3.0-only
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path


START = re.compile(r"^\[==========\] Running (\d+) tests? from ", re.MULTILINE)
END = re.compile(r"^\[==========\] (\d+) tests? from .* ran\. ", re.MULTILINE)
PASSED = re.compile(r"^\[  PASSED  \] (\d+) tests?\.", re.MULTILINE)
RUN = re.compile(r"^\[ RUN      \] (\S+)", re.MULTILINE)
SKIP = re.compile(r"^\[  SKIPPED \] (\S+) \(\d+ ms\)", re.MULTILINE)
GTEST_OPTIONS = ("--gtest_repeat=1", "--gtest_shuffle=0", "--gtest_brief=0")

# /proc/self/maps reports VEX's client mappings under Memcheck, so these
# three map-inspection tests explicitly step aside there. The SIGSEGV W^X
# test still runs under Memcheck. No other additional skip is accepted.
VALGRIND_ONLY_SKIPS = {
    "testWx": {
        "WX.NoMappingIsEverWritableAndExecutableBeforeDuringOrAfter",
        "WX.DestroyUnmapsTheCodeAndReturnsTheMeter",
        "WX.ACompileThatFailsAfterMappingLeavesNoMappingAndTheMeterAtItsStart",
    },
}


def fail(message: str, output: str = "") -> None:
    print(f"GTest matrix: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    raise SystemExit(1)


def run(command: list[str], env: dict[str, str], timeout: int) -> str:
    try:
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=timeout,
                                check=False)
    except subprocess.TimeoutExpired as exc:
        fail(f"timeout after {timeout}s: {shlex.join(command)}",
             (exc.stdout or b"").decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or ""))
    if result.returncode != 0:
        fail(f"exit {result.returncode}: {shlex.join(command)}", result.stdout)
    return result.stdout


def summary(name: str, output: str) -> tuple[list[str], list[str]]:
    starts = START.findall(output)
    ends = END.findall(output)
    passed = PASSED.findall(output)
    if len(starts) != 1 or len(ends) != 1 or len(passed) != 1:
        fail(f"{name}: missing or duplicated GTest start/completion/pass summary", output)
    count = int(starts[0])
    runs = RUN.findall(output)
    skips = SKIP.findall(output)
    if count == 0 or int(ends[0]) != count or len(runs) != count or len(set(runs)) != count:
        fail(f"{name}: zero, partial, or repeated test selection", output)
    if len(set(skips)) != len(skips) or not set(skips).issubset(runs):
        fail(f"{name}: invalid named skip inventory", output)
    if int(passed[0]) + len(skips) != count or "[  FAILED  ]" in output:
        fail(f"{name}: not every selected test passed or skipped by name", output)
    return runs, skips


def sanitized_environment(source: dict[str, str]) -> dict[str, str]:
    cleaned = {key: value for key, value in source.items() if not key.startswith("GTEST_")}
    cleaned.pop("GRJIT_MEMCHECK_F32_U64_DOUBLE_ROUNDING", None)
    return cleaned


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("ordinary", "valgrind"), required=True)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--selection-control", action="store_true",
                        help="prove inherited filters and shards cannot narrow the run")
    parser.add_argument("--filter", default="*", help="intentional exact companion test filter")
    parser.add_argument("--expected-count", type=int, help="required selected count")
    parser.add_argument("--valgrind-flags", default="")
    parser.add_argument("--watchdog", default="600")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("executables", nargs="+")
    args = parser.parse_args()

    env = sanitized_environment(dict(os.environ))
    options = [f"--gtest_filter={args.filter}", *GTEST_OPTIONS]
    for executable in args.executables:
        path = str(Path(executable))
        name = Path(path).stem
        ordinary = run([path, *options], env, args.timeout)
        ordinary_runs, ordinary_skips = summary(f"{name} ordinary", ordinary)
        if args.expected_count is not None and len(ordinary_runs) != args.expected_count:
            fail(f"{name}: expected {args.expected_count} selected tests, "
                 f"got {len(ordinary_runs)}", ordinary)
        if args.selection_control:
            hostile_env = dict(os.environ)
            hostile_env.update({"GTEST_FILTER": "-*", "GTEST_TOTAL_SHARDS": "2",
                                "GTEST_SHARD_INDEX": "1", "GTEST_REPEAT": "2",
                                "GTEST_ALSO_RUN_DISABLED_TESTS": "1"})
            hostile = run([path, *options], sanitized_environment(hostile_env), args.timeout)
            hostile_runs, hostile_skips = summary(f"{name} hostile inherited selection", hostile)
            if (hostile_runs, hostile_skips) != (ordinary_runs, ordinary_skips):
                fail(f"{name}: inherited GTest settings changed the test inventory", hostile)
            print(f"{name}: inherited-filter/shard/repeat control preserved "
                  f"{len(ordinary_runs)} selected tests", flush=True)
        if args.mode == "ordinary":
            observed = ordinary
            observed_skips = ordinary_skips
        else:
            instrumented_env = dict(env)
            instrumented_env["GRJIT_MEMCHECK_F32_U64_DOUBLE_ROUNDING"] = "1"
            instrumented_env["GRJIT_TEST_WATCHDOG_SECONDS"] = args.watchdog
            observed = run(["valgrind", *shlex.split(args.valgrind_flags), path, *options],
                           instrumented_env, args.timeout)
            instrumented_runs, observed_skips = summary(f"{name} Memcheck", observed)
            if instrumented_runs != ordinary_runs:
                fail(f"{name}: Memcheck changed the selected test inventory", observed)
            expected = set(ordinary_skips) | VALGRIND_ONLY_SKIPS.get(name, set())
            if set(observed_skips) != expected:
                fail(f"{name}: Memcheck changed the named skip inventory; "
                     f"expected {sorted(expected)}, got {sorted(observed_skips)}", observed)
            no_leaks = ("definitely lost: 0 bytes" in observed or
                        "All heap blocks were freed -- no leaks are possible" in observed)
            if "ERROR SUMMARY: 0 errors" not in observed or not no_leaks:
                fail(f"{name}: missing zero-error or zero-definite-leak Memcheck evidence", observed)
        if args.quiet:
            print(f"{name}: {len(ordinary_runs)} selected/completed, "
                  f"{len(observed_skips)} named skips, PASS", flush=True)
            for line in observed.splitlines():
                if ("Memcheck VEX u64->f32 double-rounding" in line or
                        "Memcheck VEX f32.convert_i64" in line or
                        "Memcheck u64->f32 probe" in line):
                    print(line, flush=True)
        else:
            print(f"\n### {name} {args.mode}: {len(ordinary_runs)} selected/completed; "
                  f"skips {observed_skips} ###\n", flush=True)
            print(observed, flush=True)
    print(f"GTest matrix {args.mode}: all {len(args.executables)} executables completed", flush=True)


if __name__ == "__main__":
    main()
