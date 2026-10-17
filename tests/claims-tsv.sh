#!/bin/bash
# laplace ingest --claims-tsv: what --claims says, for a machine (the claim-level parity of tools/parity reads it). A claim
# a line: kind, position, score, then each part as text; a type of the highway, which has no text, is LIST:KEY (the
# first key beside the highway naming its slot); a tab, a line end or a backslash in a part is escaped. Read with the
# highway the environment names.
#   tests/claims-tsv.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/claims-tsv.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/claims-tsv.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "claims-tsv: $*" >&2; exit 1; }
run(){ LAPLACE_DATA=$scratch "$L" ingest "$1" -s "$2" "$3" 2>&1 | tr -d '\r'; }

# CILI: a definition holding a tab and a backslash (Turtle's \\) stays one part
mkdir -p "$scratch/cili"
printf '%s\n' '@prefix	dc:	<http://purl.org/dc/elements/1.1/> .' '@prefix	skos:	<http://www.w3.org/2004/02/skos/core#> .' \
  '@prefix pwn30: <http://wordnet-rdf.princeton.edu/wn30/> .' '@base <http://globalwordnet.org/cili/> .' '' \
  '<i21999>	a	<Concept> ;' '	skos:definition	"take off	weight \\ slim"@en ;' '	dc:source	pwn30:00045817-v .' >"$scratch/cili/ili.ttl"
c=$(run --claims-tsv cili "$scratch/cili/ili.ttl") || fail "cili did not read: $c"
d=$(grep -P '^claim\t[0-9]+\t[0-9.e+-]+\ti21999\tskos:definition\t' <<<"$c") || fail "the definition is not a claim line: $c"
[ "$(awk -F'\t' '{print NF}' <<<"$d")" = 6 ] || fail "the definition is not one part (a tab in it split it): $d"
grep -qF 'take off\tweight \\ slim' <<<"$d" || fail "the tab and the backslash in the definition are not escaped: $d"
t=$(run --claims cili "$scratch/cili/ili.ttl")
[ "$(grep -c '^claim	' <<<"$c")" = "$(grep -c '^\[' <<<"$t")" ] || fail "--claims-tsv and --claims say a different number of claims: $c / $t"
! grep -q '^\[' <<<"$c" || fail "--claims-tsv also wrote a claim as text: $c"

# VerbNet: a member's sense and its PropBank roleset are types of the highway, written LIST:KEY
mkdir -p "$scratch/vn"
cat >"$scratch/vn/heed-0.0.xml" <<'EOF'
<!DOCTYPE VNCLASS SYSTEM "vn_class-3.dtd">
<VNCLASS ID="heed-0.0">
 <MEMBERS>
  <MEMBER fn_mapping="None" grouping="" name="heed" verbnet_key="heed#1" wn="heed%2:41:00" features=""/>
  <MEMBER fn_mapping="Compliance" grouping="keep.04" name="keep_to" verbnet_key="keep_to#1" wn="" features=""/>
 </MEMBERS>
 <THEMROLES/>
 <FRAMES/>
 <SUBCLASSES/>
</VNCLASS>
EOF
v=$(run --claims-tsv verbnet "$scratch/vn/heed-0.0.xml") || fail "verbnet did not read: $v"
grep -Pq '^claim\t[0-9]+\t[0-9.e+-]+\theed\tili:i[0-9]+$' <<<"$v" || fail "heed's sense is not ili:KEY: $v"
grep -Pq '^claim\t[0-9]+\t[0-9.e+-]+\tkeep_to\tpbroleset:keep\.04$' <<<"$v" || fail "keep_to's roleset is not pbroleset:keep.04: $v"
! grep -P '^(claim|held)\t' <<<"$v" | grep -q '<[a-z]*>' || fail "a type is written as its list alone: $v"
echo "claims-tsv: ok"