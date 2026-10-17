#!/bin/bash
# Where the data is kept is the machine's to say (Laplace-Operations laplace.env), never the code's (Laplace-Engine#59):
# a source whose root names a variable the environment does not set is nowhere, and an ingest of it stops before it
# begins, naming the variable. And a recipe names a native grammar as it names any grammar: the PGN recipe loads and
# reads its games through grammars.c, with nothing in recipe.c naming it.
#   tests/data-root.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/data-root.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/data-root.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "data-root: $*" >&2; exit 1; }

set +e; out=$(env -u LAPLACE_DATA "$L" ingest --plan iso-639 2>&1); code=$?; set -e
[ "$code" = 2 ] || fail "LAPLACE_DATA unset: exit $code, not 2: $out"
grep -q 'its root names \$LAPLACE_DATA, which the environment does not set' <<<"$out" || fail "LAPLACE_DATA unset: not said: $out"
set +e; out=$(LAPLACE_DATA= "$L" ingest --plan iso-639 2>&1); code=$?; set -e
[ "$code" = 2 ] || fail "LAPLACE_DATA empty: exit $code, not 2: $out"

set +e; out=$(env -u LAPLACE_MODELS "$L" ingest --plan tokenizers 2>&1); code=$?; set -e
[ "$code" = 2 ] || fail "LAPLACE_MODELS unset: exit $code, not 2: $out"
grep -q '\$LAPLACE_MODELS' <<<"$out" || fail "LAPLACE_MODELS unset: not said: $out"

# the PGN recipe, read through the native grammar it names: every game of the fixture a record
out=$(LAPLACE_DATA=$scratch "$L" ingest --claims -s chess-com "$here/chess/kinds.pgn" 2>&1) || fail "chess-com's recipe did not read kinds.pgn: $out"
grep -q 'games:' <<<"$out" || fail "kinds.pgn: no games read: $out"
echo "data-root: unset roots stop the ingest; the PGN recipe reads through its native grammar"
