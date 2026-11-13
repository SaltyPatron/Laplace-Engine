#!/bin/bash
# The tables the syzygy source holds (3, 4 and 5 pieces), read as a probe reads them: a table is mapped when a probe
# first needs it, never read whole, and no more are mapped at once than the bound (laplace tablebase -n). For a position
# of each piece count, with pawns and without, the best-play line runs to mate through every zeroing move into the
# smaller tables, and reads the same with two tables mapped at most as with the default bound.
#   tests/syzygy-pieces.sh path/to/laplace TABLES   (TABLES: the source's root, or its 3-4-5 directory)
set -euo pipefail
L=${1:?usage: tests/syzygy-pieces.sh path/to/laplace TABLES}
T=${2:?usage: tests/syzygy-pieces.sh path/to/laplace TABLES}
fail(){ echo "syzygy-pieces: $*" >&2; exit 1; }
scratch=$(mktemp -d "${TMPDIR:-/tmp}/syzygy-pieces.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
# FEN, pieces, the result the line ends in
cases=$(cat <<'EOF'
8/4P3/8/3k4/8/8/8/4K3 w - -	3	1-0
8/8/8/8/8/2k5/8/K1R5 b - -	3	1-0
8/8/8/3k4/8/8/2R5/K3N3 w - -	4	1-0
8/2p5/8/8/8/3k4/8/K1R5 w - -	4	1-0
8/4k3/8/8/8/3PK3/8/2R2r2 w - -	5	1-0
8/8/8/4k3/8/1q6/2Q5/K6R w - -	5	1-0
8/6pk/8/8/8/8/1Q5P/K7 b - -	5	1-0
EOF
)
n=0
while IFS=$'\t' read -r fen pieces result; do
  [ -n "$fen" ] || continue
  board=${fen%% *}; count=$(tr -cd 'KQRBNPkqrbnp' <<<"$board" | wc -c)
  [ "$count" = "$pieces" ] || fail "$fen is not $pieces pieces"
  wide=$("$L" tablebase -T "$T" --line "$fen" 2>"$scratch/wide.err" | tr -d '\r') || fail "$fen: no verdict: $(cat "$scratch/wide.err")"
  narrow=$("$L" tablebase -T "$T" -n 2 --line "$fen" 2>"$scratch/narrow.err" | tr -d '\r') || fail "$fen: no verdict with two tables mapped: $(cat "$scratch/narrow.err")"
  [ "$wide" = "$narrow" ] || fail "$fen reads otherwise with two tables mapped"
  peak=$(sed -nE 's/.* ([0-9]+) at once at most \(bound 2\).*/\1/p' "$scratch/narrow.err")
  [ -n "$peak" ] && [ "$peak" -le 2 ] || fail "$fen: more than two tables mapped at once: $(cat "$scratch/narrow.err")"
  line=$(awk -F'\t' '$1 == "line"' <<<"$wide")
  [ "$(cut -f3 <<<"$line")" = "$result" ] || fail "$fen: the line's result is not $result: $line"
  case "$(cut -f5 <<<"$line")" in *'#') ;; *) fail "$fen: the line does not end in mate: $line" ;; esac
  plies=$(cut -f4 <<<"$line"); [ "$(awk -F'\t' '$1 == "claim"' <<<"$wide" | wc -l)" = "$plies" ] || fail "$fen: not a claim a ply"
  n=$((n + 1))
done <<<"$cases"
[ "$n" = 7 ] || fail "$n cases read, not 7"
echo "syzygy-pieces: 3, 4 and 5 pieces, with pawns and without: every best-play line to mate, the same with two tables mapped at most"
