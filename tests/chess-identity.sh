#!/bin/bash
# A chess position and line are content (src/chess.c): the same ids on every machine and in every repo that composes
# them, with nothing salted into a hash. This pins them. A PGN game of no tags and no result holds its pure board-state
# trajectory [P0, P1, ..., Pn] as its record (a line of no moves is its start position P0). A record is never the
# subject of a claim and a game of no result claims no transition, so the record the Engine holds is the line itself:
# each id printed below is a line's own content id and fixes every position on it.
#   tests/chess-identity.sh path/to/laplace
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/chess-identity.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/chess-identity.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "chess-identity: $*" >&2; exit 1; }

out=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r') || fail "chess-com's recipe did not read identity.pgn: $out"
got=$(awk -F'\t' '$1 == "record" && $2 > 0 { print $4 }' <<<"$out")
want='6546b7964667fe459eead69e9ef46c87
aa9a24415d37db0497aa2bf1f8e99f8e
193e5b5676f2c6fbec93eb4070dafba3
6271dcd6936759e26540d38a5cb99e7d
5fce7d0b28094d3c4c9e41d2c6cdbd90
a94bc1ab79b97c852a250fa0166b0cec
cdbe2059c4113476f296a37298bc1d2a'
[ "$got" = "$want" ] || fail "a position or line has another id (start, 1.e4, two transposing lines, en passant, castlings):
got:
$got
want:
$want"
# Laplace's own games (laplace-chess) and a PGN of no corpus (the generic pgn recipe) are read as chess-com's are: the
# same game is the same content whoever witnesses it.
for how in "-s laplace-chess" ""; do
  copy=$scratch/brought.pgn; cp "$here/chess/identity.pgn" "$copy"
  again=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv $how "$copy" 2>&1 | tr -d '') || fail "${how:-the generic pgn recipe} did not read identity.pgn: $again"
  [ "$(awk -F'	' '$1 == "record" && $2 > 0 { print $4 }' <<<"$again")" = "$want" ] || fail "${how:-the generic pgn recipe} gives another id"
done
# the start position's own text, so a change of id is told from a change of content
grep -qF 'Ra1Nb1Bc1Qd1Ke1Bf1Ng1Rh1Pa2Pb2Pc2Pd2Pe2Pf2Pg2Ph2pa7pb7pc7pd7pe7pf7pg7ph7ra8nb8bc8qd8ke8bf8ng8rh8w137137-' <<<"$out" || fail "the start position is not [board, w, castling 137 137, -]"
echo "chess-identity: positions, moves and lines keep their content ids"
