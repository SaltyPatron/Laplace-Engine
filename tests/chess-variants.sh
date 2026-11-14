#!/bin/bash
# Every law firmware/chess.firmware declares replays the games played under it (src/chess_rules.c). The games in
# tests/chess/variants are chess.com's, as their site wrote them (Crazyhouse, 3-check, King of the Hill: drops, a piece
# taken to hand, a promoted piece taken as a pawn, checks counted, the hill), with chess-variant-reference.json, each
# game's plies and the FEN of the position it ends in as the Laplace monorepo's own rules replay it. Each game must be
# read as played, never kept as text, end where the reference says in as many plies, and the ingest must give the same.
#   tests/chess-variants.sh path/to/laplace [path/to/chess-positions]
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/chess-variants.sh path/to/laplace [path/to/chess-positions]}; P=${2:-}
if [ -z "$P" ]; then for c in chess-positions chess_positions chess-positions.exe chess_positions.exe; do [ -x "$(dirname "$L")/$c" ] && P=$(dirname "$L")/$c && break; done; fi
here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes} LAPLACE_CHESS_FIRMWARE=${LAPLACE_CHESS_FIRMWARE:-$here/../firmware/chess.firmware}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/chess-variants.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "chess-variants: $*" >&2; exit 1; }
[ -n "$P" ] && [ -x "$P" ] || fail "no chess-positions program beside $L (cmake --build ... --target chess_positions), or name it"
dir=$here/chess/variants; native=$dir; if command -v cygpath >/dev/null; then native=$(cygpath -m "$dir"); fi     # stdin carries the path to the program as is

# the reference, in its own order: file, variant, finalFen, plies
sed -n 's/^ *"\(file\|variant\|finalFen\|plies\)": "\{0,1\}\([^",]*\)"\{0,1\},\{0,1\}$/\2/p' "$dir/chess-variant-reference.json" | paste - - - - >"$scratch/reference.tsv"
[ "$(wc -l <"$scratch/reference.tsv")" -eq 13 ] || fail "the reference does not name 13 games"

queries=$scratch/queries; : >"$queries"
while IFS=$'\t' read -r file variant fen plies; do
  printf 'game %s\nlaw %s\t%s\n' "$native/$file" "$variant" "$fen" >>"$queries"
done <"$scratch/reference.tsv"
answers=$("$P" <"$queries" | tr -d '\r') || fail "chess_positions did not answer"
i=0; by_law=0
while IFS=$'\t' read -r file variant fen plies; do
  i=$((i + 1)); game=$(sed -n "$((2 * i - 1))p" <<<"$answers"); want=$(sed -n "$((2 * i))p" <<<"$answers")
  [ "$want" != "-" ] || fail "$file: the reference FEN is no position under $variant"
  ended=${game##*$'\t'}; game=${game%$'\t'*}
  [ "$game" = "$want	$plies" ] || fail "$file ($variant): ends at $game, the reference at $want in $plies plies"
  # the law's own verdict where the game ended by it (mate, the third check, the hill), else play goes on (*): never
  # another result than the game's Result tag
  tag=$(tr -d '\r' <"$dir/$file" | sed -n 's/^\[Result "\(.*\)"\]$/\1/p')
  [ "$ended" = "*" ] || [ "$ended" = "$tag" ] || fail "$file ($variant): its law says $ended, its Result $tag"
  [ "$ended" = "*" ] || by_law=$((by_law + 1))
done <"$scratch/reference.tsv"
[ "$by_law" -eq 8 ] || fail "$by_law games end by their law; their Terminations say 8 (checkmate, the third check, the hill)"

# the laws no held game plays: every legal sequence four plies deep from each start, against the published perft
# counts the open variant engines (python-chess, Fairy-Stockfish) test themselves with
perft=$(printf '%s\n' \
  $'perft Standard\t4\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1' \
  $'perft Standard\t3\tr3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1' \
  $'perft Atomic\t4\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1' \
  $'perft Antichess\t4\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1' \
  $'perft Horde\t4\trnbqkbnr/pppppppp/8/1PP2PP1/PPPPPPPP/PPPPPPPP/PPPPPPPP/PPPPPPPP w kq - 0 1' \
  $'perft Racing Kings\t4\t8/8/8/8/8/8/krbnNBRK/qrbnNBRQ w - - 0 1' \
  $'perft Crazyhouse\t4\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[] w KQkq - 0 1' \
  $'perft King of the Hill\t4\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1' \
  $'perft Three-check\t4\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 3+3 0 1' | "$P" | tr -d '\r' | paste -sd' ')
[ "$perft" = "197281 97862 197326 153299 23310 296242 197281 197281 197281" ] || fail "a law's moves are not the published perft counts: $perft"

# the ingest replays every game as played, and its lines end where the reader's do
out=$(LAPLACE_DATA=$scratch "$L" ingest --claims-tsv -s chess-com "$dir"/chess-variant-*.pgn 2>&1 | tr -d '\r') || fail "the ingest did not read the variant games: $out"
grep -q 'kept as its text' <<<"$out" && fail "a variant game was kept as text: $(grep 'kept as its text' <<<"$out")"
[ "$(grep -c '1 played, 0 studies, 0 puzzles, 0 kept as text' <<<"$out")" -eq 13 ] || fail "not every variant game was read as played"
echo "chess-variants: 13 games under Crazyhouse, 3-check and King of the Hill replay to their reference positions, each of the 8 that ended by its law with its Result; every law gives the published perft counts"
