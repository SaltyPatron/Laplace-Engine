#!/usr/bin/env bash
# tools/ci/parity-needed.sh: the paths that can move an ingest run the parity harness, and only those.
set -euo pipefail
n="$(dirname "$0")/../tools/ci/parity-needed.sh"
fail=0
expect() { local want=$1; shift; local got; got=$(printf '%s\n' "$@" | bash "$n"); [[ $got == "$want" ]] || { echo "FAIL: $* -> $got, wanted $want"; fail=1; }; }
expect true  src/ingest.c
expect true  recipes/iso-639/source
expect true  firmware/translate
expect true  tools/parity/parity.sh
expect true  CMakeLists.txt
expect true  .github/workflows/laplace.yml
expect true  '*'
expect true  README.md src/forget.c
expect false README.md
expect false tests/supersede.sh tests/data-root.sh
expect false .gitignore
expect false docs/src/x.md
expect false ''
(( fail == 0 )) && echo "parity-needed: ok"
exit $fail
