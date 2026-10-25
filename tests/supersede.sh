#!/bin/bash
# A source that is a working tree says `supersede` (workspace-code): each whole reading of it is what it says now, and
# the readings before it are forgotten once it is the witness, as `laplace forget` takes a witness out. A file changed
# since keeps none of its former content that only the earlier reading held; a file removed since goes; a file the
# readings share stays. A disposable database (lp_supersede_*) is deployed and dropped after.
#   tests/supersede.sh path/to/laplace        (PGHOST, PGUSER as the runner's; PSQL, DROPDB: the client programs)
set -euo pipefail
L=${1:?usage: tests/supersede.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/supersede.XXXXXX")
db=lp_supersede_$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
dropdb=${DROPDB:-$(dirname "${PSQL:-$(command -v psql)}")/dropdb}
trap '"$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db" >/dev/null 2>&1 || true; rm -rf "$scratch"' EXIT
fail(){ echo "supersede: $*" >&2; exit 1; }
export LAPLACE_WORK=$scratch/work LAPLACE_RECIPES=$scratch/recipes
cp -r "${LAPLACE_RECIPES_FROM:-$here/../recipes}" "$LAPLACE_RECIPES"
tree=$scratch/tree; mkdir -p "$LAPLACE_RECIPES/supersede-tree" "$tree"
printf 'name supersede-tree\nwitness Supersede Tree\nclass UserCuratedResource\nroot %s\nsupersede\nreads python\n' "$tree" > "$LAPLACE_RECIPES/supersede-tree/source"

"$L" deploy -d "$conn" >"$scratch/deploy.log" 2>&1 || fail "deploy: $(tail -5 "$scratch/deploy.log")"
held(){ "$L" hop -d "$conn" "$1" 2>&1 | grep -q 'nothing recorded' && echo no || echo yes; }
# a named source is ingested by a run of its own, which logs to $LAPLACE_WORK/logs/ingest/NAME.log
ingest(){ "$L" ingest -d "$conn" -j 2 supersede-tree >"$scratch/$1.run" 2>&1 || fail "ingest $1: $(tail -20 "$scratch/$1.run")"
          cp "$LAPLACE_WORK/logs/ingest/supersede-tree.log" "$scratch/$1.log"; }

printf 'def alpha_version_one():\n    return 1\n' > "$tree/a.py"
printf 'def kept_unchanged():\n    return 0\n' > "$tree/b.py"
printf 'def removed_since():\n    return 9\n' > "$tree/gone.py"
ingest first
[ "$(held alpha_version_one)" = yes ] && [ "$(held removed_since)" = yes ] || fail "the first reading is not held"
witness_id(){ "${PSQL:-psql}" -XwAt -v ON_ERROR_STOP=1 -d "$conn" -c 'SELECT id::text FROM public.witness ORDER BY id'; }
first=$(witness_id)
[[ "$first" =~ ^[0-9a-f]{32}$ ]] || fail "first reading did not register exactly one witness: $first"

printf 'def beta_version_two():\n    return 2\n' > "$tree/a.py"; rm "$tree/gone.py"
ingest second
second=$(witness_id)
[[ "$second" =~ ^[0-9a-f]{32}$ && "$second" != "$first" ]] || fail "changed reading did not replace the earlier witness: $second"
[ "$(held beta_version_two)" = yes ] || fail "the new content is not held"
[ "$(held kept_unchanged)" = yes ] || fail "a file both readings hold was taken out"
[ "$(held alpha_version_one)" = no ] || fail "a changed file's former content is still held"
[ "$(held removed_since)" = no ] || fail "a removed file is still held"

ingest third
[ "$(witness_id)" = "$second" ] || fail "an unchanged reading changed the registered witness"
[ "$(held beta_version_two)" = yes ] && [ "$(held kept_unchanged)" = yes ] || fail "an unchanged reading took out what it holds"
echo "supersede: a changed file's former content and a removed file go with the earlier reading; a shared file and an unchanged reading stay"
