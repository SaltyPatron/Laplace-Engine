#!/bin/bash
# An opening is an incomplete game (recipes/openings/openings.recipe): its pgn field, movetext with no tags and no
# result, is read by chess (part pgn grammar pgn: NativeGrammar.field in src/chess.c) as the line a PGN game of the same
# moves holds, [P0, M0, P1, ...]. The same content is the same id: a row that names nothing holds its line alone as its
# record, so that record's id is the line's, and it must be the id of the record chess-com's recipe gives a one-game PGN
# of the same moves and no tags (and the ids tests/chess-identity.sh pins). A named row says only [line, "opening name",
# name] and [line, "Encyclopaedia of Chess Openings code", eco] of that line: no move, no outcome.
#   tests/opening-lines.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/opening-lines.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/opening-lines.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "opening-lines: $*" >&2; exit 1; }

book=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s openings "$here/chess/openings/a.tsv" 2>&1 | tr -d '\r') || fail "the openings recipe did not read a.tsv: $book"
game=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$here/chess/opening-line.pgn" 2>&1 | tr -d '\r') || fail "chess-com's recipe did not read opening-line.pgn: $game"

# the rows that name nothing: each record is its line (1. e4; 1. e4 e5 2. Nf3 Nc6; the castling line)
lines=$(awk -F'\t' '$1 == "record" { print $4 }' <<<"$book" | head -3)
want='b17034a030605a7b729131993c5ca42d
2559928d51a3fa236734fbb1a71ac18b
bd4dc4877779bb570ebebe65c5b474dc'
[ "$lines" = "$want" ] || fail "an opening's line is not the line chess-identity pins for the same moves:
got:
$lines
want:
$want"
# the one-game PGN of the castling line's moves, no tags, no result: the same id as the opening's
played=$(awk -F'\t' '$1 == "record" && $2 > 0 { print $4 }' <<<"$game")
[ "$played" = "$(sed -n 3p <<<"$lines")" ] || fail "the PGN game ($played) and the opening of the same moves ($(sed -n 3p <<<"$lines")) are not one line"

# the named rows: two claims each, of the line, and nothing else
text(){ awk -F'\t' -v id="$1" '$1 == "record" && $4 == id { print $5 }' <<<"$book"; }
e4=$(text b17034a030605a7b729131993c5ca42d); knight=$(text 2559928d51a3fa236734fbb1a71ac18b)
claims=$(awk -F'\t' '$1 == "claim" { print $4 "\t" $5 "\t" $6 }' <<<"$book")
expected=$(printf '%s\t%s\t%s\n' "$e4" 'opening name' "King's Pawn Game" "$e4" 'Encyclopaedia of Chess Openings code' B00 \
  "$knight" 'opening name' 'King'"'"'s Knight Opening: Normal Variation' "$knight" 'Encyclopaedia of Chess Openings code' C44)
[ "$claims" = "$expected" ] || fail "the openings' claims are not [line, opening name, name] and [line, ECO code, eco] alone:
got:
$claims
want:
$expected"
echo "opening-lines: an opening is the line its moves play, the same id as a game of them"
