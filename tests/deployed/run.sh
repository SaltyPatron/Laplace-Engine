#!/usr/bin/env bash
# The Engine's database checks, run by Laplace-Operations after it has deployed this revision (laplace.yml, job tests;
# verify.sh tests), against the database it deployed through the program it installed. Nothing here makes, drops or
# empties a database: each check ingests its own sources, asserts on what is under its own witnesses, and forgets
# them (tests/deployed/check.py).
#   tests/deployed/run.sh [CHECK...]     every check in tests/deployed/, or those named (file names without .py)
# Environment: LAPLACE_CONNINFO (the deployed database), LAPLACE_PG_DIR, LAPLACE_PROGRAM (the deployed laplace),
# LAPLACE_ENGINE_BUILD (its build tree, where the check programs are built beside it), the installed artifacts
# (LAPLACE_TIER0, LAPLACE_FLAGS, LAPLACE_HIGHWAY, LAPLACE_GRAMMARS, LAPLACE_FIRMWARE), TMPDIR.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${LAPLACE_CONNINFO:?the deployed database (laplace.env)}" "${LAPLACE_PROGRAM:?the deployed laplace}"
: "${LAPLACE_ENGINE_BUILD:?the deployed Engine build tree}"
[[ -x $LAPLACE_PROGRAM ]] || { echo "run.sh: $LAPLACE_PROGRAM is not a program" >&2; exit 2; }
# The check programs, built against the deployed configuration of this revision.
cmake --build "$LAPLACE_ENGINE_BUILD" --target reader_depth presence_frontier pg_plan_lifecycle || exit 1
export LAPLACE_CHECK_BIN=$LAPLACE_ENGINE_BUILD
export LAPLACE_RECIPES_FROM=$here/../../recipes
export TMPDIR=${TMPDIR:-/tmp}
if (( $# )); then checks=("$@"); else
  checks=(); for f in "$here"/*.py; do n=$(basename "$f" .py); [[ $n == check ]] || checks+=("$n"); done
fi
failed=()
for c in "${checks[@]}"; do
  start=$(date +%s)
  if python3 "$here/$c.py"; then status=passed; else status=FAILED; failed+=("$c"); fi
  printf 'engine check %-32s %s in %ss\n' "$c" "$status" "$(( $(date +%s) - start ))"
done
if (( ${#failed[@]} )); then echo "engine checks failed: ${failed[*]}"; exit 1; fi
echo "engine checks: ${#checks[@]} passed"
