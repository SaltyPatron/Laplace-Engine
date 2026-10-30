#!/usr/bin/env bash
# The parity harness (Laplace#1885 step 1): one source ingested by the monorepo's pipeline (laplace-mono ingest KEY)
# and by the Engine (laplace ingest SOURCE), each into its own scratch database, then what each recorded compared:
# entity and claim ID sets, entities by tier, testimony by claim, and the Engine's forget round trip.
#
#   parity.sh [--out DIR] [--keep] [--accept FILE] [--require-equal] [--no-forget] [--after ENGINE=MONO ...] ENGINE=MONO
#
#   ENGINE=MONO    the source's Engine name and monorepo key, e.g. iso-639=iso639
#   --after        a source that goes in first on both sides (recipes/order's after; the monorepo's layer order), e.g.
#                  --after unicode=unicode. What is compared is the target's own contribution: each database after the
#                  target, less the same database before it.
#   --out          where the exports, logs and the report go (default $TMPDIR/parity-ENGINE)
#   --keep         keep the two databases (laplace_parity_RUN_engine, laplace_parity_RUN_mono); default: dropped
#   --accept, --require-equal   diff.py's: accepted difference classes, and fail on any other
#
# Environment: LAPLACE_ENGINE_BIN (the engine; default laplace on PATH), LAPLACE_MONO_CLI_DIR (the published monorepo
# CLI, default /opt/laplace/app/cli), LAPLACE_RECIPES (the Engine's recipes), LAPLACE_DATA (the corpus, both sides),
# PGHOST/PGUSER (default /tmp, laplace: a role that creates databases and extensions), TMPDIR.
# Either side alone, against a database made some other way: export.sh, then diff.py (the monorepo's decomposer
# removals compare its C# decomposer against its recipe that way, both sides mono).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out="" keep=0 forget=1 accept="" require=""; afters=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) out=$2; shift 2 ;;
    --keep) keep=1; shift ;;
    --no-forget) forget=0; shift ;;
    --accept) accept=$2; shift 2 ;;
    --require-equal) require=--require-equal; shift ;;
    --after) afters+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    -*) echo "parity.sh: unknown option $1" >&2; exit 2 ;;
    *) target=$1; shift ;;
  esac
done
[[ "${target:-}" == *=* ]] || { echo "usage: parity.sh [options] ENGINE=MONO   (see parity.sh --help)" >&2; exit 2; }
: "${PGHOST:=/tmp}" "${PGUSER:=laplace}" "${TMPDIR:?a scratch directory}" "${LAPLACE_DATA:?the corpus directory}"
export PGHOST PGUSER
engine=${LAPLACE_ENGINE_BIN:-$(command -v laplace || true)}
[[ -x "$engine" ]] || { echo "parity.sh: no engine (LAPLACE_ENGINE_BIN)" >&2; exit 2; }
export LAPLACE_ENGINE_BIN=$engine
mono_dir=${LAPLACE_MONO_CLI_DIR:-/opt/laplace/app/cli}
[[ -f "$mono_dir/Laplace.Cli.dll" ]] || { echo "parity.sh: no monorepo CLI at $mono_dir (LAPLACE_MONO_CLI_DIR)" >&2; exit 2; }
psql=${PSQL:-psql}; export PSQL=$psql
src_e=${target%%=*} src_m=${target#*=}
out=${out:-$TMPDIR/parity-$src_e}
run=$(date +%Y%m%d%H%M%S)_$$
db_e=laplace_parity_${run}_engine db_m=laplace_parity_${run}_mono
mkdir -p "$out"/{engine,mono,logs}
conn_e="host=$PGHOST user=$PGUSER dbname=$db_e"
stamp() { printf '[%(%H:%M:%S)T] %s\n' -1 "$*"; }
drop() {
  [[ "$1" =~ ^laplace_parity_[a-z0-9_]+$ ]] || { echo "parity.sh: refusing to drop a non-parity database: $1" >&2; return 2; }
  if ! "$psql" -XqAt -v ON_ERROR_STOP=1 -d postgres -c "DROP DATABASE IF EXISTS \"$1\" WITH (FORCE)"; then
    echo "parity.sh: database cleanup failed: $1 (retained; see PostgreSQL error above)" >&2
    return 1
  fi
}
cleanup() {
  local result=$? failed=0
  trap - EXIT
  if [[ $keep == 1 ]]; then
    stamp "kept $db_e and $db_m (--keep)"
  else
    drop "$db_e" || failed=1
    drop "$db_m" || failed=1
  fi
  # Try both databases and preserve the original failure; cleanup cannot turn
  # either a failed run or a failed deletion into success.
  if [[ $result == 0 && $failed == 1 ]]; then result=1; fi
  exit "$result"
}
trap cleanup EXIT
timed() {   # timed LOG CMD...: run, log, and print the wall time; the log's tail on failure
  local log=$1; shift; local t0=$SECONDS rc=0
  "$@" > "$log" 2>&1 || rc=$?
  printf '    %ss  %s\n' "$((SECONDS - t0))" "$(basename "$log")"
  [[ $rc == 0 ]] || { tail -40 "$log" >&2; echo "parity.sh: failed ($rc): $*" >&2; exit "$rc"; }
}

stamp "engine side: $db_e"
timed "$out/logs/engine-deploy.log" "$engine" deploy -d "$conn_e"
export LAPLACE_INGEST_ONE=1
for pair in "${afters[@]}"; do timed "$out/logs/engine-ingest-${pair%%=*}.log" "$engine" ingest -d "$conn_e" "${pair%%=*}"; done
bash "$here/export.sh" engine "$db_e" "$out/engine/before"
timed "$out/logs/engine-ingest-$src_e.log" "$engine" ingest -d "$conn_e" "$src_e"
bash "$here/export.sh" engine "$db_e" "$out/engine/after"
if [[ $forget == 1 ]]; then
  witness=$(sed -n 's/^witness[[:space:]]\+//p' "${LAPLACE_RECIPES:?the Engine recipes}/$src_e/source" | head -1)
  [[ -n "$witness" ]] || { echo "parity.sh: $src_e/source names no witness" >&2; exit 2; }
  timed "$out/logs/engine-forget.log" "$engine" forget -d "$conn_e" "$witness"
  bash "$here/export.sh" engine "$db_e" "$out/engine/forgotten"
fi
unset LAPLACE_INGEST_ONE

stamp "monorepo side: $db_m"
# The monorepo's extensions are the published install's, found through the database's own settings (as laplace-mono's
# database has them): its library and control directories, and laplace_substrate preloaded. A template does not carry
# a database's settings, so they are set on every database made here.
prefix=${LAPLACE_INSTALL_PREFIX:-/opt/laplace}
pgmajor=$(( $("$psql" -XqAt -d postgres -c 'SHOW server_version_num') / 10000 ))
settings() { "$psql" -XqAt -v ON_ERROR_STOP=1 -d postgres \
  -c "ALTER DATABASE \"$1\" SET dynamic_library_path = '$prefix/lib/postgresql/$pgmajor:\$libdir'" \
  -c "ALTER DATABASE \"$1\" SET extension_control_path = '$prefix/share/postgresql/$pgmajor:\$system'" \
  -c "ALTER DATABASE \"$1\" SET session_preload_libraries = 'laplace_substrate'"; }
# What goes in first (--after) is the same on every run with the same published CLI: the monorepo's unicode alone is
# about 17 minutes under the host lock, which holds a deployment off. It is kept as a template database
# (laplace_parity_tmpl_mono_KEY, KEY of the CLI's bytes and the afters) and each run starts from a copy of it; a
# template of another key is dropped when a new one is made.
key=$({ sha256sum "$mono_dir/Laplace.Cli.dll" "$mono_dir"/*.so 2>/dev/null | cut -d' ' -f1; printf '%s\n' "${afters[@]}"; } | sha256sum | cut -c1-12)
tmpl=laplace_parity_tmpl_mono_$key
has_db() { [[ $("$psql" -XqAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname = '$1'") == 1 ]]; }
export LAPLACE_DB="Host=$PGHOST;Username=$PGUSER;Database=$db_m;Command Timeout=0"
export LD_LIBRARY_PATH="$mono_dir:${LAPLACE_INSTALL_PREFIX:-/opt/laplace}/lib:${LD_LIBRARY_PATH:-}"
export LAPLACE_OPS_LOG_DIR=$out/logs/mono-ops LAPLACE_INGEST_CONSOLE=ci; mkdir -p "$LAPLACE_OPS_LOG_DIR"
# The published CLI is replaced under the host deployment lock; hold it shared while it runs, as ingest-source.sh does.
lock=${LAPLACE_HOST_LOCK:-${LAPLACE_LOCKS:-/run/lock/laplace}/host-resource.lock}
lease=""; if [[ -w "$lock" ]]; then exec {lease}>>"$lock"; flock -s "$lease"; fi
mono() { (cd "$mono_dir" && dotnet "$mono_dir/Laplace.Cli.dll" ingest "$@"); }
if [[ ${#afters[@]} -gt 0 ]] && has_db "$tmpl"; then
  stamp "  from the template $tmpl"
  "$psql" -XqAt -v ON_ERROR_STOP=1 -d postgres -c "CREATE DATABASE \"$db_m\" TEMPLATE \"$tmpl\" STRATEGY FILE_COPY"; settings "$db_m"
else
  "$psql" -XqAt -v ON_ERROR_STOP=1 -d postgres -c "CREATE DATABASE \"$db_m\""; settings "$db_m"
  "$psql" -XqAt -v ON_ERROR_STOP=1 -d "$db_m" -c 'CREATE EXTENSION IF NOT EXISTS postgis' \
    -c 'CREATE EXTENSION IF NOT EXISTS laplace_geom' -c 'CREATE EXTENSION IF NOT EXISTS laplace_substrate'
  for pair in "${afters[@]}"; do timed "$out/logs/mono-ingest-${pair#*=}.log" mono "${pair#*=}"; done
  if [[ ${#afters[@]} -gt 0 ]]; then
    for old in $("$psql" -XqAt -d postgres -c "SELECT datname FROM pg_database WHERE datname LIKE 'laplace\\_parity\\_tmpl\\_mono\\_%'"); do drop "$old"; done
    "$psql" -XqAt -v ON_ERROR_STOP=1 -d postgres -c "CREATE DATABASE \"$tmpl\" TEMPLATE \"$db_m\" STRATEGY FILE_COPY" && stamp "  kept as the template $tmpl"
  fi
fi
bash "$here/export.sh" mono "$db_m" "$out/mono/before"
timed "$out/logs/mono-ingest-$src_m.log" mono "$src_m"
[[ -n "$lease" ]] && exec {lease}>&-    # the CLI is done: the deployment lock is let go before the reading that is left
bash "$here/export.sh" mono "$db_m" "$out/mono/after"

stamp "compare"
scope="Source: \`$src_e\` (Engine) / \`$src_m\` (monorepo)${afters:+, after ${afters[*]}}: each side's contribution is its database after the source less before it."
python3 "$here/diff.py" "$out/mono" "$out/engine" --names "laplace-mono,engine" --scope "$scope" \
  --report "$out/report.md" --json "$out/summary.json" ${accept:+--accept "$accept"} $require

# The Engine's provenance is containment: a source that adds claims has its witness's trunk say them (laplace replay,
# the walk down from the trunk). A harness run where it says none is the walk's defect, not a difference.
lines() { wc -l < "$1"; }
if [[ $forget == 1 ]] && (( $(lines "$out/engine/forgotten/entities.tsv") != $(lines "$out/engine/before/entities.tsv") )); then
  echo "parity.sh: forgetting $src_e left $(( $(lines "$out/engine/forgotten/entities.tsv") - $(lines "$out/engine/before/entities.tsv") )) entities more or fewer than before it went in" >&2; exit 1
fi
if (( $(lines "$out/engine/after/claims.tsv") > $(lines "$out/engine/before/claims.tsv") )) \
   && (( $(lines "$out/engine/after/testimony.tsv") <= $(lines "$out/engine/before/testimony.tsv") )); then
  echo "parity.sh: $src_e added claims but its witness's trunk says none of them (laplace replay finds no testimony)" >&2; exit 1
fi
