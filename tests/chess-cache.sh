#!/bin/bash
# The chess perf-cache (src/chess_cache.c): laplace chess writes chess's vocabulary and the positions and moves a source
# plays, every record the law's composition with chess_rules' ID; --verify recomputes every record of a written module,
# the vocabulary slot by slot and every move replayed from the standard start; the loader refuses a changed module;
# and a cache that is there changes no ID the decomposer gives.
#   tests/chess-cache.sh path/to/laplace
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/chess-cache.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/chess-cache.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch LAPLACE_DATA=$scratch
fail(){ echo "chess-cache: $*" >&2; exit 1; }
fingerprint(){ awk '/^fingerprint/ { print $NF }' <<<"$1"; }

# the vocabulary alone: complete, and each record the law's
out=$("$L" chess -o "$scratch/vocabulary.chess" 2>&1 | tr -d '\r') || fail "the vocabulary was not written: $out"
grep -qF '16164 vocabulary: 64 squares, 768 piece-squares, 256 numbers, 7225 castlings, 17 en passant fields, 7832 moves; 0 boards, 0 positions, 0 transitions' <<<"$out" \
  || fail "the vocabulary is not the law's domains: $out"
check=$("$L" chess -o "$scratch/vocabulary.chess" --verify 2>&1 | tr -d '\r') || fail "the vocabulary does not verify: $check"
grep -qF 'verified: 16164 dense records' <<<"$check" || fail "not every vocabulary record was recomputed: $check"

# a source's games: every position and move they play, admitted, and replayed from the start under the rules
games=$scratch/games.chess
out=$("$L" chess -o "$games" -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r') || fail "identity.pgn was not admitted: $out"
grep -qE '[1-9][0-9]* positions, [1-9][0-9]* transitions\), [1-9][0-9]* \(position, move\) -> position' <<<"$out" || fail "no positions were admitted: $out"
first=$(fingerprint "$out")
again=$("$L" chess -o "$scratch/again.chess" -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r') || fail "the second generation failed: $again"
[ "$(fingerprint "$again")" = "$first" ] || fail "the same sources gave another module"
cmp -s "$games" "$scratch/again.chess" || fail "the same sources gave other bytes"
check=$("$L" chess -o "$games" --verify 2>&1 | tr -d '\r') || fail "the admitted positions do not verify: $check"
read -r positions held moves pairs < <(sed -nE 's/.* ([0-9]+) of ([0-9]+) positions and ([0-9]+) of ([0-9]+) transitions.*/\1 \2 \3 \4/p' <<<"$check")
[ -n "${pairs:-}" ] && [ "$positions" = "$held" ] && [ "$moves" = "$pairs" ] && [ "$pairs" -gt 0 ] \
  || fail "not every admitted position and move replays from the start: $check"
[ "$(fingerprint "$("$L" chess -o "$games" --fingerprint 2>&1 | tr -d '\r')")" = "$first" ] || fail "--fingerprint reads another fingerprint"

# a changed byte anywhere is refused whole (a record's coordinate, then a transition's target)
size=$(wc -c <"$games")
for at in 4000 $((size - 20)); do
  cp "$games" "$scratch/changed.chess"
  printf '\xff' | dd of="$scratch/changed.chess" bs=1 seek="$at" conv=notrunc status=none
  if "$L" chess -o "$scratch/changed.chess" --verify >/dev/null 2>&1; then fail "a module changed at byte $at was read"; fi
done

# a cache that is there changes nothing a load-free reading gives: every id the decomposer composes is the same
plain=$("$L" ingest --claims-tsv -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r' | awk -F'\t' '$1 == "record" { print $4 }')
cached=$(LAPLACE_CHESS=$games "$L" ingest --claims-tsv -s chess-com "$here/chess/identity.pgn" 2>&1 | tr -d '\r' | awk -F'\t' '$1 == "record" { print $4 }')
[ -n "$plain" ] && [ "$plain" = "$cached" ] || fail "the chess cache changed what the decomposer composes"
echo "chess-cache: the vocabulary and the admitted positions are the law's compositions; a changed module is refused"
