#!/bin/bash
# A chess position, move and line are content (src/chess.c): the same ids on every machine and in every repo that
# composes them, with nothing salted into a hash. This pins them. A PGN game of no tags and no result holds its line
# [P0, M0, P1, ...] as its record (a line of no moves is its start position). A record is never the subject of a
# claim and a game of no result claims no move, so the record the Engine holds is the line itself: each id printed
# below is a line's own content id and fixes every position and move on it.
#   tests/chess-identity.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/chess-identity.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/chess-identity.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "chess-identity: $*" >&2; exit 1; }

out=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r') || fail "chess-com's recipe did not read identity.pgn: $out"
got=$(awk -F'\t' '$1 == "record" && $2 > 0 { print $4 }' <<<"$out")
want='6546b7964667fe459eead69e9ef46c87
b17034a030605a7b729131993c5ca42d
2559928d51a3fa236734fbb1a71ac18b
eda2a5d7bf07835788bac9def6f122f5
0597f55a22171c49d3c144ed95e019c3
bd4dc4877779bb570ebebe65c5b474dc
78b2c74d8df5a0d64473fe5f79f6cc5f'
[ "$got" = "$want" ] || fail "a position, move or line has another id (start, 1.e4, two transposing lines, en passant, castlings):
got:
$got
want:
$want"
# the start position's own text, so a change of id is told from a change of content
grep -qF 'Ra1Nb1Bc1Qd1Ke1Bf1Ng1Rh1Pa2Pb2Pc2Pd2Pe2Pf2Pg2Ph2pa7pb7pc7pd7pe7pf7pg7ph7ra8nb8bc8qd8ke8bf8ng8rh8w137137-' <<<"$out" || fail "the start position is not [board, w, castling 137 137, -]"
echo "chess-identity: positions, moves and lines keep their content ids"
