#!/usr/bin/env bash
# A curated record holds its named thing and claims, rather than another
# composition of its packaging pointers and JSON keys beside that thing.
set -euo pipefail
engine=${1:?usage: record-thing.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/record-thing.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data
mkdir -p "$LAPLACE_DATA/ConceptNet"
file=$LAPLACE_DATA/ConceptNet/assertions.csv
printf '%s\t%s\t%s\t%s\t%s\n' \
  '/a/[/r/Antonym/,/c/en/cat/,/c/en/dog/]' /r/Antonym /c/en/cat /c/en/dog \
  '{"dataset":"/d/verbosity","license":"cc:by/4.0","sources":[{"contributor":"/s/resource/verbosity"}],"surfaceEnd":"dog","surfaceStart":"cat","surfaceText":"[[cat]] is the opposite of [[dog]]","weight":1.759}' \
  '/a/[/r/IsA/,/c/en/dog/n/,/c/en/animal/n/]' /r/IsA /c/en/dog/n /c/en/animal/n \
  '{"dataset":"/d/wordnet/3.1","license":"cc:by/4.0","sources":[{"contributor":"/s/resource/wordnet/rdf/3.1"}],"weight":2.0}' > "$file"
output=$("$engine" ingest --claims-tsv -s conceptnet "$file" 2>&1) || { printf '%s\n' "$output" >&2; exit 1; }
records=$(grep $'^record\t' <<< "$output")
[[ $(wc -l <<< "$records") == 2 ]] || { printf 'expected two records: %s\n' "$output" >&2; exit 1; }
if grep -Eq 'weight|surfaceText|/r/|/c/' <<< "$records"; then
  printf 'packaging was composed beside the named thing: %s\n' "$records" >&2
  exit 1
fi
grep -q $'^claim\t.*Antonym' <<< "$output"
grep -q $'^claim\t.*IsA' <<< "$output"
grep -q 'the strength with which this edge expresses this assertion' <<< "$output"
dump=$("$engine" ingest --no-load --entities -s conceptnet "$file" 2>&1) || { printf '%s\n' "$dump" >&2; exit 1; }
entities=$(grep '^  tier ' <<< "$dump")
if grep -Eq 'parts +(weight|surfaceText|rAntonym|rIsA)([[:space:]]|$)' <<< "$entities"; then
  printf 'a packaging key or relation URI became an entity: %s\n' "$entities" >&2
  exit 1
fi
echo 'record-thing: named records retain their claims without packaging entities'
