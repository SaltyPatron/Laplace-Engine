#!/bin/bash
# Producer receipts describe actual committed rows and the actual source trunk.
set -euo pipefail
L=${1:?usage: tests/ingest-progress.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ingest-progress.XXXXXX")
db=laplace_progress_$$
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
cleanup(){ "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"; rm -rf "$scratch"; }
trap cleanup EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data
export LAPLACE_RECIPES=$scratch/recipes LAPLACE_INGEST_PROGRESS_PATH=$scratch/progress.jsonl
cp -r "$here/../recipes" "$LAPLACE_RECIPES"
sed -i '/^tier0$/d' "$LAPLACE_RECIPES/unicode/source"
mkdir -p "$LAPLACE_DATA/UCD/Public/UCD/latest"
printf '0041 ; Yes\n0042 ; No\n' > "$LAPLACE_DATA/UCD/Public/UCD/latest/LinkEmail.txt"
"$L" deploy -d "$conn" > "$scratch/deploy.log" 2>&1 || { cat "$scratch/deploy.log"; exit 1; }
"$L" ingest -d "$conn" -j 2 unicode > "$scratch/ingest.log" 2>&1 || { cat "$scratch/ingest.log"; cat "$LAPLACE_WORK/logs/ingest/unicode.log"; exit 1; }
"$psql" -Xw -v ON_ERROR_STOP=1 -d "$conn" <<SQL
CREATE TEMP TABLE progress_lines(line text);
\copy progress_lines FROM '$scratch/progress.jsonl' WITH (FORMAT csv, DELIMITER E'\x1f', QUOTE E'\x1e');
CREATE TEMP VIEW progress AS SELECT line::jsonb AS j FROM progress_lines;
DO \$\$
DECLARE last_frame jsonb;
BEGIN
  SELECT j INTO STRICT last_frame FROM progress WHERE j->>'event'='complete';
  IF (last_frame->>'entities')::bigint <> (SELECT count(*) FROM public.entity)
     OR (last_frame->>'physicalities')::bigint <> (SELECT count(*) FROM public.physicality)
     OR last_frame->>'files_total' <> '1' OR last_frame->>'files_done' <> '1'
     OR NOT EXISTS (SELECT FROM public.witness WHERE id::text=last_frame->>'source_id')
     OR NOT EXISTS (SELECT FROM progress WHERE j->>'event'='file-start')
     OR NOT EXISTS (SELECT FROM progress WHERE j->>'event'='file-composed' AND j ? 'file_id') THEN
    RAISE EXCEPTION 'producer progress disagrees with persisted source: %', last_frame;
  END IF;
END \$\$;
SQL
echo 'ingest-progress: real committed rows, file receipts and published witness agree'
