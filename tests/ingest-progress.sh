#!/bin/bash
# Producer receipts describe actual committed rows and the actual source trunk.
set -euo pipefail
L=${1:?usage: tests/ingest-progress.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ingest-progress.XXXXXX")
db=laplace_progress_$$
cold=laplace_progress_cold_$$
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
cleanup(){ "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"; "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$cold"; rm -rf "$scratch"; }
trap cleanup EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data
export LAPLACE_RECIPES=$scratch/recipes LAPLACE_INGEST_PROGRESS_PATH=$scratch/progress.jsonl
cp -r "$here/../recipes" "$LAPLACE_RECIPES"
sed -i '/^tier0$/d' "$LAPLACE_RECIPES/unicode/source"
mkdir -p "$LAPLACE_DATA/UCD/Public/UCD/latest"
printf '0041\n0042\n0041\n' > "$LAPLACE_DATA/UCD/Public/UCD/latest/LinkEmail.txt"
# Like the real NamedSequencesProv.txt: fully read, with no provisional rows.
printf '# Provisional named sequences\n# No provisional entries are currently defined.\n' \
    > "$LAPLACE_DATA/UCD/Public/UCD/latest/NamedSequencesProv.txt"
"$L" deploy -d "$conn" > "$scratch/deploy.log" 2>&1 || { cat "$scratch/deploy.log"; exit 1; }
# Cross grammar-framed windows so the last row updates an earlier cumulative
# series with a NULL lineage rather than duplicating or adding its total again.
LAPLACE_RECORD_WINDOW_BYTES=1 "$L" ingest -d "$conn" -j 2 unicode > "$scratch/ingest.log" 2>&1 || { cat "$scratch/ingest.log"; cat "$LAPLACE_WORK/logs/ingest/unicode.log"; exit 1; }
"$psql" -Xw -v ON_ERROR_STOP=1 -d "$conn" <<SQL
CREATE TEMP TABLE progress_lines(line text);
\copy progress_lines FROM '$scratch/progress.jsonl' WITH (FORMAT csv, DELIMITER E'\x1f', QUOTE E'\x1e');
CREATE TEMP VIEW progress AS SELECT line::jsonb AS j FROM progress_lines;
DO \$\$
DECLARE last_frame jsonb;
BEGIN
  IF EXISTS (SELECT FROM pg_stat_statements
      WHERE dbid=(SELECT oid FROM pg_database WHERE datname=current_database())
        AND query LIKE 'SELECT c.entity, st_asewkb(c.path), c.tier%') THEN
    RAISE EXCEPTION 'absent metadata IDs reached reverse-path discovery';
  END IF;
  SELECT j INTO STRICT last_frame FROM progress WHERE j->>'event'='complete';
  IF (last_frame->>'entities')::bigint <> (SELECT count(*) FROM public.entity)
     OR (last_frame->>'physicalities')::bigint <> (SELECT count(*) FROM public.physicality)
     OR last_frame->>'files_total' <> '2' OR last_frame->>'files_done' <> '2'
     OR NOT EXISTS (SELECT FROM public.witness WHERE id::text=last_frame->>'source_id')
     OR NOT EXISTS (SELECT FROM progress WHERE j->>'event'='file-start')
     OR NOT EXISTS (SELECT FROM progress WHERE j->>'event'='file-composed'
         AND j->>'file' LIKE '%/NamedSequencesProv.txt' AND NOT j ? 'file_id')
     OR NOT EXISTS (SELECT FROM progress WHERE j->>'event'='file-composed' AND j ? 'file_id') THEN
    RAISE EXCEPTION 'producer progress disagrees with persisted source: %', last_frame;
  END IF;
  IF NOT EXISTS (SELECT FROM laplace_contained_series(ARRAY[(last_frame->>'source_id')::blake3])
       WHERE claim=laplace_compose(ARRAY[laplace_cp_id(65),laplace_id('Link_Email')]) AND games=2 AND score=2)
     OR (SELECT count(*) FROM progress WHERE j->>'event'='progress' AND j->>'file' LIKE '%/LinkEmail.txt')<3 THEN
    RAISE EXCEPTION 'streaming windows lost or duplicated cumulative testimony';
  END IF;
END \$\$;
SQL
root_reference(){
  local selected=""
  if [[ -n ${2:-} ]]; then
    [[ $2 =~ ^[0-9a-f]{32}$ ]] || return 1
    selected="WHERE w.id='$2'::blake3"
  fi
  "$psql" -XwAt -v ON_ERROR_STOP=1 -d "$1" -c \
    "SELECT jsonb_agg(jsonb_build_array(w.id::text,e.tier,e.hilbert,encode(ST_AsEWKB(e.coord),'hex')) ORDER BY w.id)
       FROM public.witness w JOIN public.entity e ON e.id=w.id $selected;"
}
before=$(root_reference "$conn")
"$L" ingest -d "$conn" -j 2 unicode > "$scratch/replay.log" 2>&1 || { cat "$scratch/replay.log"; exit 1; }
grep -q 'files recorded already' "$LAPLACE_WORK/logs/ingest/unicode.log"
[[ "$before" == "$(root_reference "$conn")" ]] || { echo 'unchanged precheck changed the source reference'; exit 1; }
# Reuse an unread recorded file while adding a new one. Its locally composed
# or indexed reference must give the same source geometry as a cold reading.
printf 'Unicode fixture description\n' > "$LAPLACE_DATA/UCD/Public/UCD/latest/ReadMe.txt"
"$L" ingest -d "$conn" -j 2 unicode > "$scratch/partial-replay.log" 2>&1 || { cat "$scratch/partial-replay.log"; exit 1; }
grep -q 'files recorded already' "$LAPLACE_WORK/logs/ingest/unicode.log"
cold_conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$cold"
"$L" deploy -d "$cold_conn" > "$scratch/cold-deploy.log" 2>&1 || { cat "$scratch/cold-deploy.log"; exit 1; }
"$L" ingest -d "$cold_conn" -j 2 unicode > "$scratch/cold.log" 2>&1 || { cat "$scratch/cold.log"; exit 1; }
cold_id=$("$psql" -XwAt -v ON_ERROR_STOP=1 -d "$cold_conn" -c 'SELECT id::text FROM public.witness')
# Earlier release witnesses may legitimately remain. Compare the new release,
# rather than requiring the warm database to discard its admitted history.
[[ "$(root_reference "$conn" "$cold_id")" == "$(root_reference "$cold_conn" "$cold_id")" ]] || { echo 'partial replay disagrees with cold source ID/coordinates/tier/Hilbert'; exit 1; }
echo 'ingest-progress: committed receipts, unchanged reuse and warm/cold source reference parity passed'
