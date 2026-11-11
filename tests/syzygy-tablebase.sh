#!/bin/bash
# A Syzygy table (recipes/syzygy) is decoded by the engine (input decode syzygy, src/chess_syzygy.c) to a line a legal
# position of its material, FEN and verdict, and each line says [P, "win/draw/loss", verdict] (.rtbw) or, where P is no
# draw, [P, "distance to zeroing", plies] (.rtbz) of the position P its FEN is (grammar fen): the position a game holds
# when it stands there, the same ID, so a game's position meets the tablebase's claim by identity alone.
# KQvK (tests/chess/syzygy, the tables of the lichess mirror) pins: every legal position of K+Q against K either way
# round, White's or Black's move; verdicts the reader's chess (tests/chess-positions.c, what Laplace-MCP links) composes
# into the same claim and record IDs; and a game that takes Black's last pawn and leaves KQvK stands in a position the
# tablebase holds.
#   tests/syzygy-tablebase.sh path/to/laplace [path/to/chess-positions]
set -euo pipefail
L=${1:?usage: tests/syzygy-tablebase.sh path/to/laplace [path/to/chess-positions]}; here=$(cd "$(dirname "$0")" && pwd)
P=${2:-}
if [ -z "$P" ]; then for c in chess-positions chess_positions chess-positions.exe chess_positions.exe; do [ -x "$(dirname "$L")/$c" ] && P=$(dirname "$L")/$c && break; done; fi
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/syzygy-tablebase.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "syzygy-tablebase: $*" >&2; exit 1; }
[ -n "$P" ] && [ -x "$P" ] || fail "no chess-positions program beside $L (cmake --build ... --target chess_positions), or name it"

wdl=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s syzygy "$here/chess/syzygy/KQvK.rtbw" 2>"$scratch/wdl.err" | tr -d '\r') || fail "the syzygy recipe did not read KQvK.rtbw: $(tail -5 "$scratch/wdl.err")"
dtz=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s syzygy "$here/chess/syzygy/KQvK.rtbz" 2>"$scratch/dtz.err" | tr -d '\r') || fail "the syzygy recipe did not read KQvK.rtbz: $(tail -5 "$scratch/dtz.err")"

# every legal position: Ks, Q, ks on three squares, White's move or Black's, the side not to move not in check, the
# queen White's or Black's; 46,096 draws (stalemates and the queen lost at once); the DTZ table says nothing of a draw
counts=$(awk -F'\t' '$1 == "claim" { n[$6]++ } END { printf "%d %d %d\n", n["win"], n["draw"], n["loss"] }' <<<"$wdl")
[ "$counts" = "289016 46096 401792" ] || fail "KQvK's verdicts are not 289016 wins, 46096 draws, 401792 losses: $counts"
[ "$(awk -F'\t' '$1 == "claim" && $5 != "win/draw/loss"' <<<"$wdl" | wc -l)" = 0 ] || fail "a WDL line says something other than win/draw/loss"
[ "$(awk -F'\t' '$1 == "claim" && $5 == "distance to zeroing"' <<<"$dtz" | wc -l)" = 690808 ] || fail "KQvK's DTZ lines are not the 690,808 positions that are no draw"

# pinned positions, each a FEN, the verdict the table gives, the IDs the reader's chess composes, the ingest's record
pin(){ # FEN RELATION VALUE POSITION_ID
    local ids; ids=$(printf 'claim %s\t%s\t%s\n' "$1" "$2" "$3" | "$P" | tr -d '\r')
    [ "$(cut -f1 <<<"$ids")" = "$4" ] || fail "$1 is no longer position $4: $(cut -f1 <<<"$ids")"
    local out; out=$([ "$2" = "win/draw/loss" ] && echo "$wdl" || echo "$dtz")
    grep -qP "^record\t\d+\t\d+\t$(cut -f3 <<<"$ids")\t" <<<"$out" || fail "$1: no record [P, [P, $2, $3]] ($(cut -f3 <<<"$ids")) in the ingest"
}
pin '8/8/8/8/8/8/1Q6/K6k w - -' 'win/draw/loss' win    23177bb0004139c5384d47fafc6f1061     # White to move wins
pin '8/8/8/8/8/8/1Q6/K6k b - -' 'win/draw/loss' loss   2908875bd2c911cdf96348c199b46476     # Black to move loses
pin '7k/1Q6/8/8/8/8/8/K7 b - -' 'win/draw/loss' loss   ba7239ecffb26a06aca87a451b8fea07     # after 1. Qxb7 below
pin 'k7/8/1Q6/8/8/8/8/7K b - -' 'win/draw/loss' draw   0936833e576948f3a99ae5ff66415004 # stalemate
pin '8/8/8/8/8/8/1q6/k6K b - -' 'win/draw/loss' win    e27abab78168b38005fa8f501b03a0b6 # KvKQ: the same table
pin '7k/1Q6/8/8/8/8/8/K7 b - -' 'distance to zeroing' -12   ba7239ecffb26a06aca87a451b8fea07

# a game that reaches the table: 1. Qxb7 takes Black's last pawn, and the position it makes is one the tablebase holds
game=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$here/chess/syzygy/reaches.pgn" 2>&1 | tr -d '\r') || fail "chess-com's recipe did not read reaches.pgn: $game"
after=$(printf 'moves 7k/1p6/8/3Q4/8/8/8/K7 w - -\n' | "$P" | tr -d '\r' | awk -F'\t' '{ for (i = 2; i < NF; i += 2) if ($i == "Qxb7") print $(i + 1) }')
[ "$after" = ba7239ecffb26a06aca87a451b8fea07 ] || fail "1. Qxb7 from the game's FEN makes $after, not the position the tablebase pins"
text='Ka1Qb7kh8b00-'
grep -qF "$text" <<<"$game" || fail "the game's line does not hold the position after 1. Qxb7 ($text)"
awk -F'\t' -v t="$text" '$1 == "claim" && $4 == t && $5 == "win/draw/loss" && $6 == "loss" { f = 1 } END { exit !f }' <<<"$wdl" \
  || fail "the tablebase says nothing of the position the game reaches ($text)"
echo "syzygy-tablebase: a tablebase's positions are the positions games stand in, each with its verdict"
