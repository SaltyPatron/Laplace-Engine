#!/usr/bin/env bash
# A partial source replay must reuse the backend that restores known-file keys.
# Compare real connection counts as the cached estate grows, then check its
# resulting canonical source reference against a cold read of the same files.
set -euo pipefail
L=${1:?usage: tests/cached-key-reader.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/cached-key-reader.XXXXXX")
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
databases=()
cleanup(){
  for db in "${databases[@]}"; do
    "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"
  done
  rm -rf "$scratch"
}
trap cleanup EXIT
export LAPLACE_RECIPES=$scratch/recipes
unset LAPLACE_INGEST_ONE
cp -r "$here/../recipes" "$LAPLACE_RECIPES"
sed -i '/^after /d' "$LAPLACE_RECIPES/framenet/source"
sql(){ "$psql" -XwAt -v ON_ERROR_STOP=1 -d "$conn" "$@"; }
sessions(){ sql -c 'SELECT sessions FROM pg_stat_database WHERE datname=current_database()'; }
source_reference(){
  sql -c "SELECT w.id::text,e.tier,e.hilbert,encode(ST_AsEWKB(e.coord),'hex')
     FROM public.witness w JOIN public.entity e ON e.id=w.id
     WHERE w.id='$1'::blake3;"
}
frame(){
  cat > "$frames/frame-$1.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<frame ID="$1" name="Reader_$1"><definition>Recorded frame $1.</definition>
<FE ID="$((1000+$1))" name="Agent" coreType="Core"/></frame>
XML
}
for count in 4 12; do
  export LAPLACE_WORK=$scratch/work-$count LAPLACE_DATA=$scratch/data-$count
  frames=$LAPLACE_DATA/FrameNet/framenet_v17/frame
  mkdir -p "$frames"
  for ((i=0;i<count;i++)); do frame "$i"; done
  db=laplace_key_reader_${count}_$$
  databases+=("$db")
  conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
  "$L" deploy -d "$conn" > "$scratch/deploy-$count.log" 2>&1 || { cat "$scratch/deploy-$count.log"; exit 1; }
  "$L" ingest -d "$conn" -j 2 framenet > "$scratch/initial-$count.log" 2>&1 || {
    cat "$scratch/initial-$count.log" "$LAPLACE_WORK/logs/ingest/framenet.log"; exit 1;
  }
  frame "$count"
  before=$(sessions)
  "$L" ingest -d "$conn" -j 2 framenet > "$scratch/replay-$count.log" 2>&1 || {
    cat "$scratch/replay-$count.log" "$LAPLACE_WORK/logs/ingest/framenet.log"; exit 1;
  }
  after=$(sessions)
  delta=$((after-before))
  cp "$LAPLACE_WORK/logs/ingest/framenet.log" "$scratch/source-$count.log"
  grep -q "  $count files recorded already" "$scratch/source-$count.log"
  restored=$(grep -c 'compiled reference bindings from recorded metadata:' "$scratch/source-$count.log")
  [[ $restored == "$count" ]]
  printf '%s\n' "$delta" > "$scratch/sessions-$count"
  printf 'cached-key-reader: %s known files, %s restores, %s sessions\n' "$count" "$restored" "$delta"
  warm_conn=$conn
  cold=laplace_key_reader_cold_${count}_$$
  databases+=("$cold")
  conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$cold"
  "$L" deploy -d "$conn" > "$scratch/cold-deploy-$count.log" 2>&1 || { cat "$scratch/cold-deploy-$count.log"; exit 1; }
  "$L" ingest -d "$conn" -j 2 framenet > "$scratch/cold-$count.log" 2>&1 || {
    cat "$scratch/cold-$count.log" "$LAPLACE_WORK/logs/ingest/framenet.log"; exit 1;
  }
  source_id=$(sql -c 'SELECT id::text FROM public.witness')
  source_reference "$source_id" > "$scratch/cold-$count.reference"
  conn=$warm_conn
  source_reference "$source_id" > "$scratch/warm-$count.reference"
  [[ -s "$scratch/cold-$count.reference" ]]
  diff -u "$scratch/cold-$count.reference" "$scratch/warm-$count.reference"
done
[[ $(cat "$scratch/sessions-4") == "$(cat "$scratch/sessions-12")" ]] || {
  echo 'cached-file count increased the number of database sessions' >&2; exit 1;
}
echo 'cached-key-reader: constant connection count and identical warm/cold canonical source references'
