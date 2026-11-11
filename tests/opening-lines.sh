#!/bin/bash
# An opening is an incomplete game (recipes/openings/openings.recipe): its pgn field, movetext with no tags and no
# result, is read by chess (part pgn grammar pgn: NativeGrammar.field in src/chess.c) as the line a PGN game of the same
# moves holds, [P0, M0, P1, ...]. The same content is the same id: a row that names nothing holds its line alone as its
# record, so that record's id is the line's, and it must be the id of the record chess-com's recipe gives a one-game PGN
# of the same moves and no tags (and the ids tests/chess-identity.sh pins).
# An opening names a position: a named row says [P, "opening name", name] and [P, "Encyclopaedia of Chess Openings
# code", eco] of the position P its line ends in (thing opening pgn product position), no move and no outcome, and its
# record holds its line, the route, beside those claims. The reader's chess (src/chess_rules.c, tests/chess-positions.c,
# what Laplace-MCP links) gives each row's line and final position: its lines must be the ingest's, so its positions are
# the ingest's, and each named row's record must be [line, the two claims of that final position].
#   tests/opening-lines.sh path/to/laplace [path/to/chess-positions]
set -euo pipefail
L=${1:?usage: tests/opening-lines.sh path/to/laplace [path/to/chess-positions]}; here=$(cd "$(dirname "$0")" && pwd)
P=${2:-}
if [ -z "$P" ]; then for c in chess-positions chess_positions chess-positions.exe chess_positions.exe; do [ -x "$(dirname "$L")/$c" ] && P=$(dirname "$L")/$c && break; done; fi
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/opening-lines.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "opening-lines: $*" >&2; exit 1; }
[ -n "$P" ] && [ -x "$P" ] || fail "no chess-positions program beside $L (cmake --build ... --target chess_positions), or name it"

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

# the reader's chess, row by row (the file's own fields, header off): its line, its final position, a named row's record
read=$(tail -n +2 "$here/chess/openings/a.tsv" | tr -d '\r' | awk -F'\t' '{ if ($1 == "") print $3; else print $3 "\t" $2 "\t" $1 }' | "$P" | tr -d '\r') \
  || fail "chess-positions did not read the rows: $read"
[ "$(cut -f1 <<<"$read" | head -3)" = "$want" ] || fail "the reader's chess gives other lines than the ingest's:
$read"
# the final positions: after 1. e4, after 1. e4 e5 2. Nf3 Nc6, and the castling game's (which the PGN game above ends in)
finals='4945af98e097c053d2dbe99e0394d5f3
65d8da92ca93b0d895fd095fc66cb1a2
7cacd24e8ea61722815fbe6dc0610e5a'
[ "$(cut -f2 <<<"$read" | head -3)" = "$finals" ] || fail "a line ends in another position:
$read"
[ "$(sed -n '4p;5p' <<<"$read" | cut -f2)" = "$(sed -n '1p;2p' <<<"$finals")" ] || fail "a named row's line does not end where the same moves do:
$read"
# a named row's record is [its line, its claims]: the subject of its claims is the position its line ends in
records=$(awk -F'\t' '$1 == "record" { print $4 }' <<<"$book" | sed -n '4p;5p')
[ "$records" = "$(sed -n '4p;5p' <<<"$read" | cut -f3)" ] || fail "a named row's record is not [line, [final position, opening name, name], [final position, ECO, eco]]:
got:
$records
want:
$(sed -n '4p;5p' <<<"$read" | cut -f3)"

# the named rows: two claims each, of the final position, and nothing else
e4_text='Ra1Nb1Bc1Qd1Ke1Bf1Ng1Rh1Pa2Pb2Pc2Pd2Pf2Pg2Ph2Pe4pa7pb7pc7pd7pe7pf7pg7ph7ra8nb8bc8qd8ke8bf8ng8rh8b137137-'
knight_text='Ra1Nb1Bc1Qd1Ke1Bf1Rh1Pa2Pb2Pc2Pd2Pf2Pg2Ph2Nf3Pe4pe5nc6pa7pb7pc7pd7pf7pg7ph7ra8bc8qd8ke8bf8ng8rh8w137137-'
claims=$(awk -F'\t' '$1 == "claim" { print $4 "\t" $5 "\t" $6 }' <<<"$book")
expected=$(printf '%s\t%s\t%s\n' "$e4_text" 'opening name' "King's Pawn Game" "$e4_text" 'Encyclopaedia of Chess Openings code' B00 \
  "$knight_text" 'opening name' 'King'"'"'s Knight Opening: Normal Variation' "$knight_text" 'Encyclopaedia of Chess Openings code' C44)
[ "$claims" = "$expected" ] || fail "the openings' claims are not [final position, opening name, name] and [final position, ECO code, eco] alone:
got:
$claims
want:
$expected"
echo "opening-lines: an opening is the line its moves play, the same id as a game of them, and names the position it ends in"
