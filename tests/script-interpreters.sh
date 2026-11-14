#!/bin/bash
# A file no pattern matches is read by what it holds (src/recipe.c sniff_generic_recipe). A script's #! line names the
# command that runs it, read as the kernel reads the line (the interpreter's path, or through env, with its options and
# assignments, the command env runs); the recipe that declares that command an interpreter reads it. A command no recipe
# declares is not guessed at from its spelling: the script is text.
#   tests/script-interpreters.sh path/to/laplace
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/script-interpreters.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/script-interpreters.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch LAPLACE_DATA=$scratch
fail(){ echo "script-interpreters: $*" >&2; exit 1; }
mkdir -p "$scratch/scripts"
while IFS='|' read -r name line want; do
  printf '%s\nbody\n' "$line" >"$scratch/scripts/$name"
  got=$("$L" ingest --plan "$scratch/scripts/$name" 2>&1 | tr -d '\r' | awk 'NR == 2 { print $1 }')
  [ "$got" = "$want" ] || fail "$name ($line): read by ${got:-nothing}, not $want"
done <<'CASES'
env-bash|#!/usr/bin/env bash|bash
sh|#!/bin/sh|bash
env-options|#!/usr/bin/env -S python3 -u|python
assignment|#!/usr/bin/env PYTHONUTF8=1 python3|python
path-pwsh|#!/usr/bin/pwsh|powershell
spaced-node|#! /usr/bin/env node|javascript
undeclared|#!/usr/bin/env perl|text
spelled-like|#!/bin/bashful|text
CASES
echo "script-interpreters: a #! line's command selects the recipe that declares it, and nothing else"
