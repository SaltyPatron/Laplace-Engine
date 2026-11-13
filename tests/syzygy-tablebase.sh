#!/bin/bash
# A tablebase verdict is a read of the tables (laplace tablebase, src/chess_tablebase.c), never a record: the position a
# FEN gives (chess_rules' ID, the one a game standing there holds), its win/draw/loss and distance to zeroing, its moves
# ranked, and its best-play line. What the syzygy source records is that line, written as the PGN a game of it is and
# read as a game is (chess.c), so its [P, M, P'] transitions are the ones games claim: a game that reaches the table and
# plays the tables' move claims the same transition, by identity alone.
# KQvK (tests/chess/syzygy, the tables of the lichess mirror) pins it.
#   tests/syzygy-tablebase.sh path/to/laplace
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/syzygy-tablebase.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=$here/../recipes
scratch=$(mktemp -d "${TMPDIR:-/tmp}/syzygy-tablebase.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "syzygy-tablebase: $*" >&2; exit 1; }
tb(){ "$L" tablebase -T "$here/chess" "$@" 2>"$scratch/tb.err" | tr -d '\r'; }

# pinned verdicts: the position's ID, its WDL and DTZ for the side to move
pin(){ # FEN ID WDL DTZ
    local got; got=$(tb "$1" | awk -F'\t' '$1 == "position" { print $2 " " $3 " " $4 }') || true
    [ "$got" = "$2 $3 $4" ] || fail "$1 is not $2 $3 $4: $got $(cat "$scratch/tb.err")"
}
pin '8/8/8/8/8/8/1Q6/K6k w - -' 23177bb0004139c5384d47fafc6f1061 win 11       # White to move wins
pin '8/8/8/8/8/8/1Q6/K6k b - -' 2908875bd2c911cdf96348c199b46476 loss -14     # Black to move loses
pin '7k/1Q6/8/8/8/8/8/K7 b - -' ba7239ecffb26a06aca87a451b8fea07 loss -12     # after 1. Qxb7 in reaches.pgn
pin 'k7/8/1Q6/8/8/8/8/7K b - -' 0936833e576948f3a99ae5ff66415004 draw 0       # stalemate
pin '8/8/8/8/8/8/1q6/k6K b - -' e27abab78168b38005fa8f501b03a0b6 win 11       # KvKQ: the same table, colours swapped
# a position no table holds is said so, its ID all the same
[ "$(tb 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -' | awk -F'\t' '$1 == "position" { print $3 }')" = none ] || fail "the start position has a verdict"

# the moves ranked: the winner's first move zeroes soonest; a stalemating move is a draw, ranked below every win
moves=$(tb '8/8/8/8/8/8/1Q6/K6k w - -')
[ "$(awk -F'\t' '$1 == "move" { print $2; exit }' <<<"$moves")" = Kb1 ] || fail "Kb1 is not the best move of KQvK's pinned position: $moves"
[ "$(awk -F'\t' '$1 == "move" && $4 == "draw" { print $2 }' <<<"$moves" | tr '\n' ' ')" = "Qf2 Qg2+ Qh2+ " ] || fail "the drawing moves are not Qf2, Qg2+ and Qh2+ (stalemates): $moves"
[ "$(awk -F'\t' '$1 == "move" { print $4 }' <<<"$moves" | uniq | tr '\n' ' ')" = "win draw " ] || fail "the moves are not wins first, then draws"

# the best-play line: to mate, its result under the 50-move rule
line=$(tb --line '7k/1Q6/8/8/8/8/8/K7 b - -' | awk -F'\t' '$1 == "line"')
[ "$(cut -f3,4,5 <<<"$line")" = "$(printf '1-0\t12\tKg8 Kb2 Kh8 Kc3 Kg8 Kd4 Kh8 Ke5 Kg8 Kf6 Kh8 Qg7#')" ] || fail "the best-play line is not 12 plies to Qg7#: $line"
[ "$(cut -f2 <<<"$line")" = 6554c79ccee9acdcb4aa4adfa1b4a6d3 ] || fail "the line is no longer [P0, M0, ..., P12] 6554c79c...: $line"
[ -z "$(tb --line 'k7/8/1Q6/8/8/8/8/7K b - -' | awk -F'\t' '$1 == "line" && $4 != 0')" ] || fail "a drawn position has a line"

# the line as the source records it, read as a game is: each ply a [P, M, P'] claim scored for the side that moves
tb --pgn '7k/1Q6/8/8/8/8/8/K7 b - -' >"$scratch/line.pgn"
grep -q '^\[FEN "7k/1Q6/8/8/8/8/8/K7 b - -"\]$' "$scratch/line.pgn" || fail "the PGN does not begin at its FEN: $(cat "$scratch/line.pgn")"
lines=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv "$scratch/line.pgn" 2>&1 | tr -d '\r') || fail "the line's PGN did not read as a game: $lines"
[ "$(awk -F'\t' '$1 == "claim"' <<<"$lines" | wc -l)" = 12 ] || fail "the line does not claim its 12 moves: $lines"
[ "$(awk -F'\t' '$1 == "claim" { print $3 }' <<<"$lines" | sort | uniq -c | awk '{ printf "%s:%s ", $2, $1 }')" = "0:6 1:6 " ] || fail "the winner's moves are not scored 1 and the loser's 0"

# a game that reaches the table (1. Qxb7 takes Black's last pawn) and plays the tables' move (1... Kg8) claims the
# transition the tablebase line claims: the same [P, M, P'], the same score
game=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$here/chess/syzygy/reaches.pgn" 2>&1 | tr -d '\r') || fail "chess-com's recipe did not read reaches.pgn: $game"
shared=$(comm -12 <(awk -F'\t' '$1 == "claim" { print $3 "\t" $4 "\t" $5 "\t" $6 }' <<<"$game" | sort) <(awk -F'\t' '$1 == "claim" { print $3 "\t" $4 "\t" $5 "\t" $6 }' <<<"$lines" | sort))
[ "$shared" = "$(printf '0\tKa1Qb7kh8b00-\tkh8g8\tKa1Qb7kg8w00-')" ] || fail "the game and the tablebase line do not share 1... Kg8: $shared"

# the source's recipe reads the tables as games (grammar pgn) through the decoder
plan=$("$L" ingest --plan -s syzygy "$here/chess/syzygy/KQvK.rtbw" 2>&1 | tr -d '\r') || fail "the syzygy source does not plan: $plan"
grep -qE '^ +syzygy-lines +1 files$' <<<"$plan" || fail "syzygy-lines does not take KQvK.rtbw: $plan"
echo "syzygy-tablebase: verdicts read from the tables; the line recorded is a game's line, sharing its transitions with the games that play it"
