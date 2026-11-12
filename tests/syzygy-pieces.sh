#!/bin/bash
# Every piece count the decoder reads (src/chess_syzygy.c, TB_PIECES 7), proven on the tables the source holds: for each
# table named, the first LINES positions decode, each line FEN \t value; the FEN's board holds exactly the table's
# material count, and the value is a WDL verdict word (.rtbw) or a whole number of plies (.rtbz). A table's captures
# reach the smaller tables in its own directory, as the ingest reads them, so the directory must hold that reach.
#   tests/syzygy-pieces.sh path/to/syzygy_decode TABLE_DIRECTORY [LINES]
set -euo pipefail
D=${1:?usage: tests/syzygy-pieces.sh path/to/syzygy_decode TABLE_DIRECTORY [LINES]}
T=${2:?usage: tests/syzygy-pieces.sh path/to/syzygy_decode TABLE_DIRECTORY [LINES]}
N=${3:-100000}
fail(){ echo "syzygy-pieces: $*" >&2; exit 1; }
[ -x "$D" ] || fail "no syzygy_decode at $D (cmake --build ... --target syzygy_decode)"
# 3, 4 and 5 pieces, both kinds of table, a balanced ending and an unbalanced one at each count that has both
for t in KQvK.rtbw KQvK.rtbz KQvKR.rtbw KQvKR.rtbz KRvKR.rtbw KRvKR.rtbz KQRvKR.rtbw KQRvKR.rtbz; do
  [ -f "$T/$t" ] || fail "$T holds no $t"
  material=${t%%.*}; material=${material/v/}; pieces=${#material}
  out=$("$D" "$T/$t" "$N" | tr -d '\r') || fail "$t did not decode"
  report=$(awk -F'\t' -v n="$pieces" -v t="$t" '
    { split($1, f, " "); b = f[1]; gsub(/[^KQRBNPkqrbnp]/, "", b); if (length(b) != n) wrong++
      if (t ~ /rtbw$/) { if ($2 != "win" && $2 != "draw" && $2 != "loss" && $2 != "cursed win" && $2 != "blessed loss") bad++ }
      else if ($2 !~ /^-?[0-9]+$/) bad++
      lines++ }
    END { printf "%d %d %d", lines, wrong + 0, bad + 0 }' <<<"$out")
  read -r lines wrong bad <<<"$report"
  [ "$lines" = "$N" ] || fail "$t gave $lines lines, not $N"
  [ "$wrong" = 0 ] || fail "$t: $wrong lines whose board is not $pieces pieces"
  [ "$bad" = 0 ] || fail "$t: $bad lines whose value is no $([[ $t == *.rtbw ]] && echo verdict || echo ply count)"
done
echo "syzygy-pieces: 3, 4 and 5 pieces, WDL and DTZ, $N positions each: every board its material, every value well formed"
