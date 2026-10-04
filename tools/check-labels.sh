#!/bin/sh
#
# Fail if a public header carries no stability label, or the wrong one.
#
# AD-14: every public header says whether it is `stable` (frozen at the major
# version once released) or `free` (a consumer requires the exact version it
# was built against). The label is a Doxygen tag in the header's @file block:
#
#     @stability stable
#
# Every header of this library is `free`: the IR, the backend and the readers
# of the emitted metadata are JIT internals, and a consumer requires the exact
# version it was built against (AD-14). There is no a/ or b/ split to choose a
# label by directory: the headers are flat, and a `stable` one here would
# quietly promise more than the spine says. A header that needs the other
# label is a decision to take on purpose, and this gate is where it would be
# noticed.
#
# Usage: check-labels.sh <root>
#   <root> holds include/ghoti.io/runtime-jit/. The self-test runs this same
#   script against tests/gates fixtures.
#
# Fails on an empty population: no headers found means the gate measured
# nothing, and a gate that measures nothing reports success.

set -eu

ROOT="${1:?usage: check-labels.sh <root>}"
BASE="$ROOT/include/ghoti.io/runtime-jit"

if [ ! -d "$BASE" ]; then
  printf 'check-labels: %s does not exist; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

headers="$(find "$BASE" -type f -name '*.h' | sort)"
if [ -z "$headers" ]; then
  printf 'check-labels: no headers under %s; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

status=0
count=0
for h in $headers; do
  count=$((count + 1))
  want=free
  # Anchored to a comment line so that prose mentioning the tag does not count
  # as carrying it.
  labels="$(grep -E '^[[:space:]]*(\*|//)[[:space:]]*@stability[[:space:]]' "$h" \
    | sed 's/.*@stability[[:space:]]*//; s/[[:space:]]*$//' || true)"
  if [ -z "$labels" ]; then
    printf 'check-labels: %s has no @stability label (want %s)\n' \
      "$h" "$want" >&2
    status=1
    continue
  fi
  if [ "$(printf '%s\n' "$labels" | wc -l)" -ne 1 ]; then
    printf 'check-labels: %s has more than one @stability label\n' "$h" >&2
    status=1
    continue
  fi
  case "$labels" in
    stable | free) ;;
    *)
      printf 'check-labels: %s: "%s" is not stable or free\n' "$h" "$labels" >&2
      status=1
      continue
      ;;
  esac
  if [ "$labels" != "$want" ]; then
    printf 'check-labels: %s is labelled %s but every header here must be %s\n' \
      "$h" "$labels" "$want" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  exit 1
fi
printf 'check-labels: %d public headers, each labelled free\n' \
  "$count"
