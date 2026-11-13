#!/usr/bin/env bash
# tools/ci/deploy-needed.sh: the paths that can move the deployed system or its database checks deploy, and only those.
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
n="$(dirname "$0")/../tools/ci/deploy-needed.sh"
fail=0
expect() { local want=$1; shift; local got; got=$(printf '%s\n' "$@" | bash "$n"); [[ $got == "$want" ]] || { echo "FAIL: $* -> $got, wanted $want"; fail=1; }; }
expect true  src/ingest.c
expect true  recipes/iso-639/source
expect true  firmware/translate
expect true  tools/build_grammars.sh
expect true  CMakeLists.txt
expect true  cmake/LaplaceNative.cmake
expect true  .github/workflows/laplace.yml
expect true  tests/deployed/supersede.py
expect true  '*'
expect true  README.md src/forget.c
expect false README.md
expect false tests/data-root.sh tests/rich-text.sh
expect false .gitignore
expect false docs/src/x.md
expect false ''
(( fail == 0 )) && echo "deploy-needed: ok"
exit $fail
