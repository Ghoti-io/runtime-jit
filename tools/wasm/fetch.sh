#!/bin/sh
#
# Fetch the WebAssembly spec suite's integer test files, which the harness in
# tests/unit/test_wast.cpp runs the integer IR against.
#
# The suite is not committed here: it is somebody else's corpus, reproducible from a commit and a
# URL, and a copy in this repository would be a snapshot that quietly stops being the thing every
# other implementation is measured against. What is committed is the commit, in
# tools/wasm/WASM_SPEC_COMMIT, and the SHA-256 of each file in tools/wasm/SHA256SUMS, so that a
# published pass count names the corpus it was measured over and a changed file is noticed, not
# absorbed.
#
# The commit is the one the tag `wg-3.0` of github.com/WebAssembly/spec names (checked with
# `git ls-remote https://github.com/WebAssembly/spec refs/tags/wg-3.0` on 2026-10-10). A tag can
# be moved; the commit cannot, so the commit is what is pinned and what is fetched.
#
# Licence: the repository's top-level LICENSE says test/ is under the Apache License 2.0
# (document/ is under the W3C Software and Document Notice and License, interpreter/ under
# Apache-2.0, papers/ under CC BY 4.0). Only files from test/ are fetched, with test/LICENSE
# beside them, and nothing of them is copied into a repository of ours.
#
# `make test` needs the files, and the harness fails (it never skips) on a host that runs the
# compiled code without them. `suite/install.sh --test` runs this first, with the network.
#
# Usage:  tools/wasm/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
commit=$(cat "$root/tools/wasm/WASM_SPEC_COMMIT")
dest="$root/third_party/wasm-spec/$commit"
base="https://raw.githubusercontent.com/WebAssembly/spec/$commit"

files="
test/core/i32.wast
test/core/i64.wast
test/LICENSE
"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

for path in $files; do
  want=$(awk -v p="$path" '$2 == p {print $1}' "$root/tools/wasm/SHA256SUMS")
  if [ -z "$want" ]; then
    printf 'fetch.sh: %s has no digest in tools/wasm/SHA256SUMS\n' "$path" >&2
    exit 1
  fi
  if [ -s "$dest/$path" ] && [ "$(sha256 "$dest/$path")" = "$want" ]; then
    printf 'have    %s\n' "$path"
    continue
  fi
  printf 'fetch   %s\n' "$path"
  mkdir -p "$dest/$(dirname "$path")"
  # --fail so that an HTML error page never lands on disk looking like data.
  curl --fail --silent --show-error --location \
      --output "$dest/$path.partial" "$base/$path"
  got=$(sha256 "$dest/$path.partial")
  if [ "$got" != "$want" ]; then
    rm -f "$dest/$path.partial"
    printf 'fetch.sh: %s at %s has digest %s, not the pinned %s\n' "$path" "$commit" "$got" "$want" >&2
    exit 1
  fi
  mv "$dest/$path.partial" "$dest/$path"
done

printf '\nWebAssembly spec suite %s (wg-3.0) is in %s\n' "$commit" "$dest"
