#!/usr/bin/env bash
# CILI's published release maps use the same offset for different concepts.
# Resolve them from one self-contained cache built without the corpus suite.
set -euo pipefail
engine=${1:?usage: cili-versions.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
recipes=${LAPLACE_RECIPES:-$here/../recipes}
manifest=${LAPLACE_MANIFEST:-$here/../../Laplace-Native/manifest}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/cili-versions.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
fail() { echo "cili-versions: $*" >&2; exit 1; }
export LAPLACE_DATA=$scratch/data LAPLACE_WORK=$scratch/work
mkdir -p "$LAPLACE_DATA/CILI" "$LAPLACE_DATA/CiliProbe" "$LAPLACE_WORK" "$scratch/recipes/cili-probe" "$scratch/recipes/cili"
for source in unicode iso-639; do ln -s "$recipes/$source" "$scratch/recipes/$source"; done
for file in "$recipes/cili/"*; do ln -s "$file" "$scratch/recipes/cili/"; done
ln -s "$recipes/formats" "$scratch/recipes/formats"
# Keep every frozen canonical slot, but let these source fixtures supply their
# own release pointers independently of the installed full mapping registry.
cp -as "$(cd "$manifest" && pwd)/." "$scratch/manifest"
gzip -dc "$manifest/slots/keys.tsv.gz" | awk -F '\t' '$1!="wordnet-3.0" && $1!="wordnet-3.1" && $1!="odwn-1.3"' | gzip -n > "$scratch/fixture-keys.tsv.gz"
rm "$scratch/manifest/slots/keys.tsv.gz"
mv "$scratch/fixture-keys.tsv.gz" "$scratch/manifest/slots/keys.tsv.gz"
export LAPLACE_MANIFEST=$scratch/manifest
printf 'unicode\niso-639\ncili\n' > "$scratch/recipes/order"
export LAPLACE_RECIPES=$scratch/recipes
# Lines from CILI's maps. 00029677-n changes concept between the two releases;
# 00186320-s belongs to 3.1 and has no 3.0 entry in this map.
printf 'i1000\t00185020-s\ni35586\t00029378-n\ni35587\t00029677-n\ni69801\t06341431-n\n' > "$LAPLACE_DATA/CILI/ili-map-pwn30.tab"
printf 'i1000\t00186320-s\ni35586\t00029677-n\ni35587\t00029976-n\ni69801\t06352801-n\ni118\t00493366-s\ni2763\t00493366-s\n' > "$LAPLACE_DATA/CILI/ili-map-pwn31.tab"
cat > "$LAPLACE_DATA/CILI/ili-map-odwn13.ttl" <<'TTL'
@prefix owl: <http://www.w3.org/2002/07/owl#> .
@prefix odwn13: <http://odwn-rdf.vu.nl/odwn13/> .
@prefix ili: <http://globalwordnet.org/cili/> .
ili:i69801 owl:sameAs odwn13:06341431-n .
ili:i35586 owl:sameAs odwn13:00029378-n .
ili:i35587 owl:sameAs odwn13:00029677-n .
TTL
cat > "$scratch/recipes/cili/aliases.recipe" <<'RECIPE'
name cili-test-aliases
match aliases.tab
tier row by \n
names pointer target
tier field by \t
alias wordnet-3.1 row pointer to target
RECIPE
printf 'ambiguous-alias\t00493366-s\n' > "$LAPLACE_DATA/CILI/aliases.tab"
cache=$scratch/cache.highway
LAPLACE_HIGHWAY=$cache "$engine" highway --refresh -o "$cache" > "$scratch/highway.log" 2>&1 || fail "cache build: $(tail -n 20 "$scratch/highway.log")"
[[ $(head -c 8 "$cache") == LPHW0003 ]] || fail "release pointers did not select the extended envelope: $(tail -n 30 "$scratch/highway.log")"
for suffix in refs keys layout nodes; do [[ ! -e $cache.$suffix ]] || fail "runtime sidecar $suffix was created"; done
grep -q '15 source pointer keys resolve to existing records' "$scratch/highway.log" || fail "the fifteen source pointer targets did not all resolve: $(tail -n 20 "$scratch/highway.log")"
cat > "$scratch/recipes/cili-probe/source" <<'SOURCE'
name cili-probe
witness CILI release pointer regression
class AcademicCurated
root $LAPLACE_DATA/CiliProbe
after cili
SOURCE
cat > "$scratch/recipes/cili-probe/probe.recipe" <<'RECIPE'
name cili-probe
match probe.tab
tier row by \n
names word wn30 wn31 odwn ordinary
tier field by \t
empty NULL
thing row word
content word
type wn30 wordnet-3.0
type wn31 wordnet-3.1
type odwn odwn-1.3
type ordinary ili
relation "in WordNet 3.0" wn30
relation "in WordNet 3.1" wn31
relation "in ODWN 1.3" odwn
relation "ordinary type lookup" ordinary
attest row wn30 wn31 odwn ordinary
RECIPE
printf 'cili-probe\n' >> "$scratch/recipes/order"
printf 'process\t00029677-n\t00029677-n\t06341431-n\ti35587\navailable\t00186320-s\t00186320-s\tNULL\tNULL\nambiguous\tNULL\t00493366-s\tNULL\tNULL\naliased\tNULL\tambiguous-alias\tNULL\tNULL\n' > "$LAPLACE_DATA/CiliProbe/probe.tab"
output=$(LAPLACE_HIGHWAY=$cache "$engine" ingest --claims-tsv -s cili-probe "$LAPLACE_DATA/CiliProbe/probe.tab" 2>&1) || fail "probe: $output"
# The ordinary column supplies the literal i35587 content too, so the printer
# can spell that shared canonical entity directly instead of as LIST:KEY.
grep -q $'\tprocess\tin WordNet 3.0\ti35587$' <<< "$output" || fail "3.0's offset lost its concept: $output"
grep -q $'\tprocess\tin WordNet 3.1\tili:i35586$' <<< "$output" || fail "3.1 reused 3.0's offset: $output"
grep -q $'\tprocess\tin ODWN 1.3\tili:i69801$' <<< "$output" || fail "ODWN's pointer lost its concept: $output"
grep -q $'\tprocess\tordinary type lookup\ti35587$' <<< "$output" || fail "ordinary list lookup changed: $output"
grep -q $'\tavailable\tin WordNet 3.1\tili:i1000$' <<< "$output" || fail "the 3.1-only pointer did not resolve: $output"
if grep -q $'\tavailable\tin WordNet 3.0\t' <<< "$output"; then fail "a missing 3.0 pointer was invented: $output"; fi
if grep -q $'\tambiguous\tin WordNet 3.1\t' <<< "$output"; then fail "one target of an ambiguous pointer was selected: $output"; fi
if grep -q $'\taliased\tin WordNet 3.1\t' <<< "$output"; then fail "an alias selected one target of an ambiguous pointer: $output"; fi
grep -q 'ambiguous highway pointer: wn31' <<< "$output" || fail "the ambiguity was not reported: $output"
echo 'PASS release-scoped pointers resolve to their canonical ILIs from one cache, without sidecars or a full corpus suite'
