#!/usr/bin/env bash
# Read-only admission boundary checks against an existing generated artifact.
set -euo pipefail
L=${1:?usage: highway-admission.sh /path/to/laplace}
: "${LAPLACE_HIGHWAY:?existing highway artifact required}"
: "${LAPLACE_TIER0:?existing tier-0 artifact required}"
: "${TMPDIR:?use the declared build scratch volume}"
scratch=$(mktemp -d "$TMPDIR/highway-admission.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
export LAPLACE_CONNINFO='host=127.0.0.1 port=1 connect_timeout=1 dbname=unused'
before=$(sha256sum "$LAPLACE_HIGHWAY" "$LAPLACE_HIGHWAY.layout" "$LAPLACE_HIGHWAY.keys" "$LAPLACE_HIGHWAY.nodes")
"$L" highway --fingerprint > "$scratch/fingerprint.log"
grep -Eq '^fingerprint [0-9a-f]{64}$' "$scratch/fingerprint.log"
[[ "$before" == "$(sha256sum "$LAPLACE_HIGHWAY" "$LAPLACE_HIGHWAY.layout" "$LAPLACE_HIGHWAY.keys" "$LAPLACE_HIGHWAY.nodes")" ]]
echo 'PASS fingerprint reads the artifact without rewriting it'
"$L" ingest --highway --no-load -j 2 > "$scratch/valid.log" 2>&1
grep -Eq 'validated; no database access' "$scratch/valid.log"
echo 'PASS existing artifact validates with an unreachable database'
reject() {
  local expected=$1; shift
  if "$@" > "$scratch/rejected.log" 2>&1; then cat "$scratch/rejected.log"; exit 1; fi
  grep -Eq "$expected" "$scratch/rejected.log" || { cat "$scratch/rejected.log"; exit 1; }
}
reject 'cannot read highway contents' env LAPLACE_HIGHWAY="$scratch/missing" "$L" ingest --highway -j 2
echo 'PASS missing artifact refuses admission before database access'
for suffix in '' .layout .keys; do ln -s "$LAPLACE_HIGHWAY$suffix" "$scratch/candidate$suffix"; done
head -n 1 "$LAPLACE_HIGHWAY.nodes" > "$scratch/candidate.nodes"
reject 'rejected before database access' env LAPLACE_HIGHWAY="$scratch/candidate" "$L" ingest --highway -j 2
echo 'PASS truncated artifact refuses admission before database access'
printf 'INVALID\tcontent\n' > "$scratch/candidate.nodes"
reject 'rejected before database access' env LAPLACE_HIGHWAY="$scratch/candidate" "$L" ingest --highway -j 2
echo 'PASS invalid artifact refuses admission before database access'
reject 'admits only the existing highway contents' "$L" ingest --highway --plan
reject 'admits only the existing highway contents' "$L" ingest --highway unrelated-source
echo 'PASS incompatible source and plan requests are rejected'
