#!/bin/bash
# A chess position, move and line are content (src/chess.c): the same ids on every machine and in every repo that
# composes them, with nothing salted into a hash. This pins them. A PGN game of no tags and no result holds its line
# [P0, M0, P1, ...] as its record (a line of no moves is its start position), attests the one claim [record, line],
# and the record the Engine holds is [record, [record, line]]; so each id printed below fixes every position and move
# on its line. The monorepo's Laplace.Chess.Tests ChessEngineIdentityTests pins the same ids.
#   tests/chess-identity.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/chess-identity.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/chess-identity.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "chess-identity: $*" >&2; exit 1; }

out=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r') || fail "chess-com's recipe did not read identity.pgn: $out"
got=$(awk -F'\t' '$1 == "record" && $2 > 0 { print $4 }' <<<"$out")
want='98522a8a1f8931ba76d1992a94e7dbcd
bab7041d3051e91c3f361ec0124aff8e
9a2ace38a08cddfc0ffcb944e6efcf44
48858d05244b68465d2c0f1e7bac323f
dead9d5c40c1fa81fed0b5f6f5dd254c
df5e2f9cf8fb64057b0d3328e1a90d84
da3caadfc3f2c4dc833aa46b44419add'
[ "$got" = "$want" ] || fail "a position, move or line has another id (start, 1.e4, two transposing lines, en passant, castlings):
got:
$got
want:
$want"
# the start position's own text, so a change of id is told from a change of content
grep -qF 'Ra1Nb1Bc1Qd1Ke1Bf1Ng1Rh1Pa2Pb2Pc2Pd2Pe2Pf2Pg2Ph2pa7pb7pc7pd7pe7pf7pg7ph7ra8nb8bc8qd8ke8bf8ng8rh8w137137-' <<<"$out" || fail "the start position is not [board, w, castling 137 137, -]"
echo "chess-identity: positions, moves and lines keep their content ids"
