#!/usr/bin/env bash
# Prepare the private schema while the service still owns its old cache paths.
set -euo pipefail
L=${1:?usage: deploy-prepare.sh path/to/laplace}
work=$(mktemp -d "${TMPDIR:?}/deploy-prepare.XXXXXX")
psql=${PSQL:-psql}
db=laplace_deploy_prepare_$$
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
admin="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=postgres"
cleanup(){ "$psql" -Xw -v ON_ERROR_STOP=1 -d "$admin" -c "DROP DATABASE IF EXISTS $db" >/dev/null; rm -r -- "$work"; }
trap cleanup EXIT
if "$L" deploy --prepare --configure-server-cache -d 'host=127.0.0.1 port=1 dbname=unused connect_timeout=1' >"$work/refusal.log" 2>&1; then
    echo 'prepare accepted server-cache mutation' >&2; exit 1
else
    result=$?; test "$result" = 2
fi
grep -q 'cannot configure them' "$work/refusal.log"
"$L" deploy --prepare -d "$conn" >"$work/initial.log" 2>&1 || { cat "$work/initial.log"; exit 1; }
"$psql" -Xw -v ON_ERROR_STOP=1 -d "$admin" -c "ALTER DATABASE $db SET laplace.highway TO '$work/unselected-highway.bin'" >/dev/null
"$L" deploy --prepare -d "$conn" >"$work/prepared.log" 2>&1 || { cat "$work/prepared.log"; exit 1; }
grep -q 'runtime activation and compatibility readback remain required' "$work/prepared.log"
observed=$("$psql" -XwAt -v ON_ERROR_STOP=1 -d "$conn" -c 'SHOW laplace.highway')
test "$observed" = "$work/unselected-highway.bin"
if "$L" status -d "$conn" >"$work/status.log" 2>&1; then
    echo 'status accepted an unavailable server Highway after schema preparation' >&2; exit 1
fi
"$psql" -Xw -v ON_ERROR_STOP=1 -d "$admin" -c "ALTER DATABASE $db RESET laplace.highway" >/dev/null
"$L" status -d "$conn" >"$work/selected.log" 2>&1 || { cat "$work/selected.log"; exit 1; }
echo 'deploy-prepare: preserves cache bindings, defers readback and retains status refusal'
