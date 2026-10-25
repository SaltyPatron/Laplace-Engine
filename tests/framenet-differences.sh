#!/usr/bin/env bash
set -euo pipefail
L=${1:?usage: tests/framenet-differences.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/framenet-differences.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
export LAPLACE_DATA=$scratch/data LAPLACE_RECIPES=$here/../recipes
export LAPLACE_CONNINFO='host=127.0.0.1 port=1 user=unavailable dbname=unavailable connect_timeout=1'
report=$LAPLACE_DATA/FrameNet/framenet_v17/miscXML/DifferencesR1.6-R1.7.xml
mkdir -p "$(dirname "$report")"
# The publisher's actual malformed opening and closing lines. The Native text
# reader must retain the spelling; the XML semantic recipe must stay strict.
cat > "$report" <<'REPORT'
<?xml version="1.0" encoding="UTF-8"?>
<FrameNet_Diff old=r1.6 new=r1.7>
</FrameNet_Diff>
REPORT
"$L" ingest --no-load -j 2 -s framenet "$report" > "$scratch/result.log" 2>&1 || {
  cat "$scratch/result.log"; exit 1;
}
grep -Eq 'decomposition: 1 files,.*curated 1, mismatched 0' "$scratch/result.log"
echo 'framenet-differences: publisher report admitted as literal Native text, no XML repair or database access'
