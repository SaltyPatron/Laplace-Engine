#!/usr/bin/env bash
# Real PostgreSQL readback of canonical product and retained packaging, followed
# by an unchanged-source precheck and a relational altitude audit.
set -euo pipefail
L=${1:?usage: tests/rich-text-db.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/rich-text-db.XXXXXX")
db=laplace_rich_text_$$
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
cleanup(){
  "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"
  local target base; target=$(cd "$scratch" && pwd -P); base=$(cd "${TMPDIR:-/tmp}" && pwd -P)
  case "$target" in "$base"/rich-text-db.*) rm -rf -- "$target";; *) return 1;; esac
}
trap cleanup EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
export LAPLACE_TRUNKS=1
mkdir "$scratch/inputs"
printf 'mississippi' > "$scratch/inputs/plain.txt"
printf '<b>mississippi</b>' > "$scratch/inputs/bold.html"
printf 'mi<b>ss</b>issippi' > "$scratch/inputs/split.html"
printf 'mis&#115;issippi' > "$scratch/inputs/escaped.html"
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
sql(){ "$psql" -XwAt -v ON_ERROR_STOP=1 -d "$conn" "$@" | tr -d '\r'; }
"$L" deploy -d "$conn" > "$scratch/deploy.log" 2>&1 || { cat "$scratch/deploy.log"; exit 1; }
"$L" ingest -d "$conn" -j 2 "$scratch/inputs" > "$scratch/first.log" 2>&1 || { cat "$scratch/first.log"; exit 1; }
result=$(sql <<'SQL'
SELECT count(*) FROM entity WHERE id=laplace_text_id('mississippi');
SELECT count(*) FROM physicality p WHERE p.tier>0
 AND laplace_path_ends(p.path,laplace_text_id('mississippi'))
 AND ((st_m(st_pointn(p.path,1))::bigint>>30)&7)=4;
SELECT count(*) FROM physicality p JOIN entity e ON e.id=p.entity
 CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) v(id) JOIN entity c ON c.id=v.id
 WHERE e.tier>0 AND c.tier>=e.tier;
SELECT string_agg(laplace_text(v.id),E'\n' ORDER BY laplace_text(v.id))
 FROM physicality p CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) v(id)
 WHERE p.path @> ARRAY[laplace_text_id('packaging')] AND v.id<>laplace_text_id('packaging');
SQL
)
expected=$(printf '1\n4\n0\n<b>mississippi</b>\nmi<b>ss</b>issippi\nmis&#115;issippi')
[[ $result == "$expected" ]] || { printf 'rich-text-db: unexpected readback\n%s\n' "$result"; exit 1; }
fingerprint(){ sql -c "SELECT md5(string_agg(to_jsonb(e)::text,E'\n' ORDER BY id)) FROM entity e WHERE tier>0; SELECT md5(string_agg(to_jsonb(p)::text,E'\n' ORDER BY entity)) FROM physicality p WHERE tier>0; SELECT md5(string_agg(to_jsonb(w)::text,E'\n' ORDER BY id)) FROM witness w;"; }
before=$(fingerprint)
"$L" ingest -d "$conn" -j 2 "$scratch/inputs" > "$scratch/again.log" 2>&1 || { cat "$scratch/again.log"; exit 1; }
[[ $(fingerprint) == "$before" ]] || { echo 'rich-text-db: unchanged source changed stored canonical state'; exit 1; }
grep -q '4 files recorded already' "$scratch/again.log"
echo 'rich-text-db: one shared content ID, four file trunks, exact PostgreSQL packaging readback, zero altitude violations and unchanged-source pruning passed'
