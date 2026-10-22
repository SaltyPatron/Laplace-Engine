#!/bin/bash
# The WordNet 3.0 database files as their recipes read them (Laplace-Engine#70): a synset is the concept CILI keys by
# its offset and type, a key template naming the part beside it (\0-{ss_type}); a pointer is the relation WordNet
# writes between two concepts; an index line's satellite offsets, written a, resolve by the template after it
# (as \0-{pos} as \0-s). No offset and no sense key is content. Read with the highway the environment names.
#   tests/wordnet-recipes.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/wordnet-recipes.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/wordnet-recipes.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "wordnet-recipes: $*" >&2; exit 1; }
claims(){ LAPLACE_DATA=$scratch "$L" ingest --claims -s princeton-wordnet "$here/wordnet/$1" 2>&1 | tr -d '\r'; }

d=$(claims data.noun) || fail "data.noun did not read: $d"
grep -q ', words in the synset, \[entity, eng, <ili>\]\]$' <<<"$d" || fail "entity is not the English sense of its synset: $d"
grep -q ', words in the synset, \[abstract entity, eng, <ili>\]\]$' <<<"$d" || fail "abstract_entity is not the English sense written with its space: $d"
grep -q '^\[<ili>, @, <ili>\]$' <<<"$d" || fail "physical_entity's hypernym is not a concept: $d"
[ "$(grep -c ', ~, ' <<<"$d")" -ge 10 ] || fail "the hyponym pointers are not relations between concepts: $d"
grep -q 'name a type no list of the highway holds' <<<"$d" && grep -q '99999999' <<<"$d" || fail "an offset CILI does not key is not said: $d"
! grep -q ', words in the synset, \[nothing, eng, <ili>\]\]$' <<<"$d" || fail "an unknown synset emitted an English sense: $d"
! grep -Eq '[0-9]{8}-[nvasr]|%[1-5]:' <<<"$(grep '^\[' <<<"$d")" || fail "an offset or a sense key is content: $d"
# The fixture lacks the lexical target word. Its missing scoped binding must
# be diagnosed alongside the unknown synset, rather than becoming a claim.
grep -q '2 keys in ' <<<"$d" && grep -q '\[target 01\]' <<<"$d" || fail "the unresolved lexical pointer was not diagnosed: $d"

i=$(claims index.adj) || fail "index.adj did not read: $i"
[ "$(grep -c '^\[able, synset containing lemma, ' <<<"$i")" = 4 ] || fail "able's four synsets are not its concepts: $i"
grep -q '^\[dissilient, synset containing lemma, ' <<<"$i" || fail "a satellite written a does not resolve as s: $i"
grep -q '^\[able, type of pointer that lemma has in all synsets containing it, !\]$' <<<"$i" || fail "able's pointer types: $i"
grep -q '^\[able, a\]$' <<<"$i" || fail "able's syntactic category: $i"
# The verb frames and sentence templates are highway lists (wnframe, wnsentence) whose slots the Native manifest freezes.
# Build the self-contained highway from frozen identifiers with no raw corpus
# root. Frame and sentence keys must resolve through its embedded index.
native=${LAPLACE_MANIFEST:-$here/../../Laplace-Native/manifest}
[ -f "$native/banks.tsv" ] || fail "no Native manifest at $native (set LAPLACE_MANIFEST)"
cp -R "$native" "$scratch/manifest"; mkdir -p "$scratch/nothing"
LAPLACE_MANIFEST=$scratch/manifest LAPLACE_DATA=$scratch/nothing LAPLACE_MODELS=$scratch/nothing \
  "$L" highway -o "$scratch/highway.bin" >"$scratch/highway.log" 2>&1 || fail "the fixture highway did not build: $(cat "$scratch/highway.log")"
for extension in keys layout nodes; do
  [ ! -e "$scratch/highway.bin.$extension" ] || fail "highway requires an external $extension sidecar"
done
LAPLACE_HIGHWAY=$scratch/highway.bin "$L" highway --fingerprint | grep -Eq '^fingerprint [0-9a-f]{64}$' || fail "the embedded highway index did not validate"
fx(){ LAPLACE_HIGHWAY=$scratch/highway.bin LAPLACE_DATA=$scratch/nothing "$L" ingest --claims -s princeton-wordnet "$here/wordnet/$1" 2>&1 | tr -d '\r'; }

v=$(fx data.verb) || fail "data.verb did not read: $v"
[ "$(grep -c '^\[<ili>, generic verb sentence frames for words in the synset, <wnframe>\]$' <<<"$v")" = 2 ] || fail "the synset frames (w_num 00) are not relations to wnframe: $v"
grep -q '^\[\[melt off, eng, <ili>\], generic verb sentence frames for words in the synset, <wnframe>\]$' <<<"$v" || fail "frame 8 is not bound to the selected English sense: $v"
grep -q 'name a type no list of the highway holds' <<<"$v" && grep -q '\[target 01\]' <<<"$v" || fail "missing lexical target bindings were not diagnosed: $v"
vt=$(LAPLACE_HIGHWAY=$scratch/highway.bin LAPLACE_DATA=$scratch/nothing "$L" ingest --claims-tsv -s princeton-wordnet "$here/wordnet/data.verb" 2>&1 | tr -d '\r') || fail "data.verb TSV did not read: $vt"
grep -Pq '\twnframe:8$' <<<"$vt" || fail "frame 8 did not resolve from the embedded index: $vt"
! grep -q 'left open' <<<"$v" || fail "data.verb leaves a part open: $v"

x=$(fx sentidx.vrb) || fail "sentidx.vrb did not read: $x"
[ "$(grep -c '^\[<ili>, example sentences illustrating the use of the verb sense, <wnsentence>\]$' <<<"$x")" = 2 ] || fail "abash's templates 126 and 127 are not relations to wnsentence: $x"
grep -q 'unresolved obligations: 0 ' <<<"$x" || fail "sentidx.vrb leaves obligations: $x"
xt=$(LAPLACE_HIGHWAY=$scratch/highway.bin LAPLACE_DATA=$scratch/nothing "$L" ingest --claims-tsv -s princeton-wordnet "$here/wordnet/sentidx.vrb" 2>&1 | tr -d '\r') || fail "sentidx.vrb TSV did not read: $xt"
for key in 126 127; do
  grep -Pq "\twnsentence:$key$" <<<"$xt" || fail "sentence $key did not resolve from the embedded index: $xt"
done
! grep -Eq '%[1-5]:' <<<"$(grep '^\[' <<<"$x")" || fail "a sense key is content: $x"

for f in frames.vrb sents.vrb; do
  r=$(fx $f) || fail "$f did not read: $r"
  ! grep -Eq 'left open|no list of the highway holds' <<<"$r" || fail "$f: $r"
done
echo "wordnet-recipes: ok"
