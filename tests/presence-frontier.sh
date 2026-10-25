#!/usr/bin/env bash
set -euo pipefail
L=${1:?usage: tests/presence-frontier.sh path/to/laplace path/to/presence-frontier}
probe=${2:?missing presence-frontier executable}
db=laplace_presence_frontier_$$
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/presence-frontier.XXXXXX")
cleanup(){
  dropdb -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"
  rm -rf "$scratch"
}
trap cleanup EXIT
printf 'Presence qualification with committed canonical higher references.\n' > "$scratch/input.txt"
"$L" deploy -d "$conn" > "$scratch/deploy.log" 2>&1 || { cat "$scratch/deploy.log"; exit 1; }
"$L" ingest -d "$conn" -j 2 "$scratch/input.txt" > "$scratch/ingest.log" 2>&1 || { cat "$scratch/ingest.log"; exit 1; }
"$probe" "$conn" "${LAPLACE_TIER0:?missing Native floor path}"
