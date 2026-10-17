#!/usr/bin/env bash
# What one database holds, in the parity harness's canonical form (Laplace#1885 step 1): three sorted TSVs any two
# databases can be compared by, whichever pipeline wrote them.
#
#   export.sh engine|mono DBNAME OUTDIR
#
#   entities.tsv   id, tier                    every entity (engine: public.entity; mono: laplace.entities)
#   claims.tsv     claim                       every claim with a standing (engine: consensus.claim; mono: laplace.consensus.id)
#   testimony.tsv  claim, witness, games       who said each claim, how many times (engine: laplace replay --tsv, the
#                                              trunk; mono: laplace.attestations by source_id, observation_count)
# IDs are lowercase hex. The connection is PGHOST/PGUSER (default /tmp, laplace); the database must be a scratch one,
# laplace_parity_*, or one named in LAPLACE_PARITY_DB_OK (a monorepo isolate, laplace_d_*, say). LAPLACE_ENGINE_BIN is
# the engine for the engine side's testimony.
set -euo pipefail
kind=${1:?usage: export.sh engine|mono DBNAME OUTDIR}; db=${2:?database}; out=${3:?output directory}
: "${PGHOST:=/tmp}" "${PGUSER:=laplace}"; export PGHOST PGUSER
[[ "$db" =~ ^laplace_parity_[a-z0-9_]+$ || "$db" =~ ^${LAPLACE_PARITY_DB_OK:-laplace_parity_}[a-z0-9_]*$ ]] \
  || { echo "export.sh: $db is not a scratch database (laplace_parity_*, or LAPLACE_PARITY_DB_OK)" >&2; exit 2; }
psql=${PSQL:-psql}
mkdir -p "$out"
q() { "$psql" -XqAt -v ON_ERROR_STOP=1 -d "$db" -c "$1"; }
case "$kind" in
  engine)
    q "\\copy (SELECT id::text, tier FROM entity) TO '$out/entities.raw'"
    q "\\copy (SELECT claim::text FROM consensus) TO '$out/claims.raw'"
    : "${LAPLACE_ENGINE_BIN:?the engine (laplace) to read the engine side testimony}"
    "$LAPLACE_ENGINE_BIN" replay -d "host=$PGHOST user=$PGUSER dbname=$db" --tsv "$out/replay.raw" > "$out/replay.log" 2>&1 \
      || { tail -20 "$out/replay.log" >&2; exit 1; }
    cut -f1-3 "$out/replay.raw" > "$out/testimony.raw" ;;
  mono)
    q "\\copy (SELECT encode(id, 'hex'), tier FROM laplace.entities) TO '$out/entities.raw'"
    q "\\copy (SELECT DISTINCT encode(id, 'hex') FROM laplace.consensus) TO '$out/claims.raw'"
    q "\\copy (SELECT encode(id, 'hex'), encode(source_id, 'hex'), sum(observation_count) FROM laplace.attestations GROUP BY 1, 2) TO '$out/testimony.raw'" ;;
  *) echo "export.sh: kind is engine or mono, not $kind" >&2; exit 2 ;;
esac
for t in entities claims testimony; do LC_ALL=C sort -u "$out/$t.raw" > "$out/$t.tsv"; rm -f "$out/$t.raw"; done
printf '%s\t%s entities, %s claims, %s testimony rows\n' "$db" "$(wc -l < "$out/entities.tsv")" "$(wc -l < "$out/claims.tsv")" "$(wc -l < "$out/testimony.tsv")"
