#!/bin/sh
#
# Fail if any of the Makefile's compile-flag sets lacks -ffp-contract=off.
#
# Guest floating point must be the same on every host and in both tiers. A
# compiler that contracts a*b+c into a fused multiply-add rounds once where the
# interpreter's C rounds twice, so the same source gives different bits. GCC
# contracts by default in the GNU modes and not in the ISO ones (-std=c17), so
# it would start the day someone writes gnu17; clang's default is "on"; arm64
# has the instruction in the baseline and x86-64 SSE2 does not. Nothing warns
# when a contraction happens, so the flag is named and checked here.
#
# What is read is make's own database (make -pn), which holds each variable
# after the Makefile has expanded it, so a set built from CFLAGS (the sanitizer
# trees) is judged by what it holds. A variable is a compile-flag set when its
# name ends in CFLAGS or CXXFLAGS and its value names a language standard
# (-std=): that picks LIB_CFLAGS, ASAN_CXXFLAGS and the fuzz sets and leaves the
# pkg-config ones (CUTIL_CFLAGS) alone. CFLAGS, CXXFLAGS and LIB_CFLAGS must
# exist, so that a gate which finds nothing cannot pass. The last
# -ffp-contract= on a line wins, as the compiler reads it, so that is the one
# that has to be "off". -ffast-math is refused wherever it appears.
#
# Usage: check-fp-contract.sh [Makefile [directory]]
#   defaults: ./Makefile, run in the Makefile's own directory. check-gates gives it
#   an altered copy and the library's directory, which is how a removal is planted
#   in the real Makefile.

set -u

mk="${1:-Makefile}"
case "$mk" in /*) ;; *) mk="$(pwd)/$mk" ;; esac
dir="${2:-$(dirname "$mk")}"

db="$(make -s -C "$dir" -f "$mk" -pn help 2>/dev/null)"
if [ -z "$db" ]; then
  printf 'check-fp-contract: make printed no variable database for %s; this gate is measuring nothing\n' "$mk" >&2
  exit 1
fi
library="$(printf '%s\n' "$db" | sed -n 's/^PROJECT :\{0,1\}= *//p' | head -n 1)"
[ -n "$library" ] || library="$(basename "$dir")"

printf '%s\n' "$db" | awk -v lib="$library" '
  /^[A-Za-z_][A-Za-z0-9_]* *(:|::)?= / {
    name = $1
    line = $0
    sub(/^[A-Za-z_][A-Za-z0-9_]* *(:|::)?= */, "", line)
    if (name ~ /(CFLAGS|CXXFLAGS)$/ && line ~ /-std=/) {
      seen[name] = 1
      count++
      last = ""
      n = split(line, w, /[ \t]+/)
      for (i = 1; i <= n; i++) {
        if (w[i] ~ /^-ffp-contract=/) last = w[i]
        if (w[i] == "-ffast-math" || w[i] == "-Ofast") fast[name] = w[i]
      }
      if (last != "-ffp-contract=off") {
        bad[name] = (last == "") ? "does not name -ffp-contract=off" : "ends with " last ", not -ffp-contract=off"
      }
    }
  }
  END {
    rc = 0
    split("CFLAGS CXXFLAGS LIB_CFLAGS", req, " ")
    for (i = 1; i <= 3; i++) {
      if (!(req[i] in seen)) {
        printf "check-fp-contract: %s: %s is not a compile-flag set in the Makefile; this gate is measuring nothing\n", lib, req[i] > "/dev/stderr"
        rc = 1
      }
    }
    for (v in bad) {
      printf "check-fp-contract: %s: %s %s\n", lib, v, bad[v] > "/dev/stderr"
      rc = 1
    }
    for (v in fast) {
      printf "check-fp-contract: %s: %s uses %s, which makes guest floating point host-dependent\n", lib, v, fast[v] > "/dev/stderr"
      rc = 1
    }
    if (rc == 0) {
      printf "check-fp-contract: %s: %d compile-flag sets, each ends with -ffp-contract=off\n", lib, count
    }
    exit rc
  }'
