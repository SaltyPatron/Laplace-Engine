#!/usr/bin/env bash
# Real grammar admission: physical windows vary; all canonical rows and evidence
# must agree. Small files must share actual database applies when memory permits.
set -euo pipefail
L=${1:?usage: tests/stream-working-set.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/stream-working-set.XXXXXX")
small=laplace_stream_small_$$
shared=laplace_stream_shared_$$
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
cleanup(){
  "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$small"
  "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$shared"
  rm -rf "$scratch"
}
trap cleanup EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data LAPLACE_RECIPES=$scratch/recipes
cp -r "$here/../recipes" "$LAPLACE_RECIPES"
sed -i '/^after /d' "$LAPLACE_RECIPES/propbank/source"
frames=$LAPLACE_DATA/PropBank/propbank-fixture/frames
mkdir -p "$frames"
for ((i=0; i<8; i++)); do
  cat > "$frames/frame-$i.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<frameset><predicate lemma="shared">
<roleset id="shared.01" name="shared description">
<aliases><alias pos="v">shared</alias></aliases>
<roles><role n="0" descr="actor $i"/><role n="1" descr="theme"/></roles>
<example name="example $i"><text>They shared the result.</text></example>
</roleset></predicate></frameset>
XML
done
fingerprint(){
  "$psql" -XwAt -v ON_ERROR_STOP=1 -d "$1" <<'SQL'
SELECT 'entity',count(*),md5(string_agg(to_jsonb(e)::text,E'\n' ORDER BY id)) FROM public.entity e;
SELECT 'physicality',count(*),md5(string_agg(to_jsonb(p)::text,E'\n' ORDER BY entity)) FROM public.physicality p;
SELECT 'witness',count(*),md5(string_agg(to_jsonb(w)::text,E'\n' ORDER BY id)) FROM public.witness w;
SELECT 'consensus',count(*),md5(string_agg(to_jsonb(c)::text,E'\n' ORDER BY claim)) FROM public.consensus c;
SELECT 'series',count(*),md5(string_agg(to_jsonb(s)::text,E'\n' ORDER BY to_jsonb(s)::text))
  FROM laplace_contained_series(ARRAY(SELECT id FROM public.witness)) s;
SQL
}
for lane in small shared; do
  db=${!lane}
  conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
  "$L" deploy -d "$conn" > "$scratch/$lane-deploy.log" 2>&1 || { cat "$scratch/$lane-deploy.log"; exit 1; }
  # One byte forces a checkpoint after every grammar window. One GiB admits
  # this complete small fixture. These are qualification settings, not defaults.
  budget=1; [[ $lane == shared ]] && budget=$((1024*1024*1024))
  start=$(date +%s%N)
  LAPLACE_WORKING_SET_BYTES=$budget LAPLACE_RECORD_WINDOW_BYTES=97 \
    "$L" ingest -d "$conn" -j 2 propbank > "$scratch/$lane.log" 2>&1 || {
      cat "$scratch/$lane.log" "$LAPLACE_WORK/logs/ingest/propbank.log"; exit 1;
    }
  elapsed=$((($(date +%s%N)-start)/1000000))
  cp "$LAPLACE_WORK/logs/ingest/propbank.log" "$scratch/$lane-source.log"
  fingerprint "$conn" > "$scratch/$lane.fingerprint"
  grep -Eq '^entity\|[1-9][0-9]*\|' "$scratch/$lane.fingerprint"
  grep -Eq '^consensus\|[1-9][0-9]*\|' "$scratch/$lane.fingerprint"
  grep -Eq '^series\|[1-9][0-9]*\|' "$scratch/$lane.fingerprint"
  "$psql" -XwAt -v ON_ERROR_STOP=1 -d "$conn" -c \
    "SELECT coalesce(sum(calls),0) FROM pg_stat_statements
       WHERE dbid=(SELECT oid FROM pg_database WHERE datname=current_database())
         AND query LIKE 'SELECT * FROM laplace_merge_stage(%';" > "$scratch/$lane.merges"
  printf 'stream-working-set: %s budget=%s bytes, %s ms, %s real stage merges\n' \
    "$lane" "$budget" "$elapsed" "$(cat "$scratch/$lane.merges")"
done
if ! cmp "$scratch/small.fingerprint" "$scratch/shared.fingerprint"; then
  diff -u "$scratch/small.fingerprint" "$scratch/shared.fingerprint" || true
  for lane in small shared; do
    conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=${!lane}"
    "$psql" -XwAt -v ON_ERROR_STOP=1 -d "$conn" -c \
      "SELECT entity::text,tier,hilbert,mask,md5(ST_AsEWKB(path)) FROM public.physicality ORDER BY entity;" \
      > "$scratch/$lane.paths"
  done
  diff -u "$scratch/small.paths" "$scratch/shared.paths" || true
  exit 1
fi
[[ $(cat "$scratch/shared.merges") -gt 0 ]]
[[ $(cat "$scratch/small.merges") -gt $(cat "$scratch/shared.merges") ]]
grep -q 'streaming persistence: 1 shared working windows' "$scratch/shared-source.log"
echo 'stream-working-set: identical canonical rows, geometry, masks, witnesses, standings and cumulative testimony; small files shared database applies'
