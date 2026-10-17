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
grep -q ', words in the synset, entity\]$' <<<"$d" || fail "entity is not a word of its synset: $d"
grep -q ', words in the synset, abstract entity\]$' <<<"$d" || fail "abstract_entity is not written with its space: $d"
grep -q ', @, that which is perceived or known' <<<"$d" || fail "physical_entity's hypernym is not entity's concept: $d"
[ "$(grep -c ', ~, ' <<<"$d")" -ge 10 ] || fail "the hyponym pointers are not relations between concepts: $d"
grep -q 'name a type no list of the highway holds' <<<"$d" && grep -q '99999999' <<<"$d" || fail "an offset CILI does not key is not said: $d"
! grep -q ', words in the synset, nothing\]$' <<<"$d" || fail "a synset no concept is says something: $d"
! grep -Eq '[0-9]{8}-[nvasr]|%[1-5]:' <<<"$(grep '^\[' <<<"$d")" || fail "an offset or a sense key is content: $d"
# A lexical pointer (source/target not 0000) names a word of the target synset, which no list keys yet: an unresolved
# obligation, said, not passed over.
grep -q 'unresolved obligations: 1 ' <<<"$d" && grep -Eq 'attest target +1$' <<<"$d" || fail "data.noun's lexical pointer is not an unresolved obligation: $d"

i=$(claims index.adj) || fail "index.adj did not read: $i"
[ "$(grep -c '^\[able, synset containing lemma, ' <<<"$i")" = 4 ] || fail "able's four synsets are not its concepts: $i"
grep -q '^\[dissilient, synset containing lemma, ' <<<"$i" || fail "a satellite written a does not resolve as s: $i"
grep -q '^\[able, type of pointer that lemma has in all synsets containing it, !\]$' <<<"$i" || fail "able's pointer types: $i"
grep -q '^\[able, a\]$' <<<"$i" || fail "able's syntactic category: $i"
# The verb frames and sentence templates are highway lists (wnframe, wnsentence) whose slots the Native manifest freezes.
# A highway is built here from a copy of that manifest and a fixture data root (CILI's concepts for the fixture synsets,
# frames.vrb, sents.vrb, one index.sense line), every other list from its slots, so the lists are read before any
# installed highway carries them.
native=${LAPLACE_MANIFEST:-$here/../../Laplace-Native/manifest}
[ -f "$native/banks.tsv" ] || fail "no Native manifest at $native (set LAPLACE_MANIFEST)"
cp -R "$native" "$scratch/manifest"; mkdir -p "$scratch/nothing"
LAPLACE_MANIFEST=$scratch/manifest LAPLACE_DATA=$here/wordnet-highway LAPLACE_MODELS=$scratch/nothing \
  "$L" highway -o "$scratch/highway.bin" >"$scratch/highway.log" 2>&1 || fail "the fixture highway did not build: $(cat "$scratch/highway.log")"
keys=$(tr -d '\r' <"$scratch/highway.bin.keys")
grep -Pq '^wnframe\t8\t' <<<"$keys" || fail "frame 8 is not a key of wnframe"
grep -Pq '^wnsentence\t126\t' <<<"$keys" || fail "sentence 126 is not a key of wnsentence"
fx(){ LAPLACE_HIGHWAY=$scratch/highway.bin LAPLACE_DATA=$scratch/nothing "$L" ingest --claims -s princeton-wordnet "$here/wordnet/$1" 2>&1 | tr -d '\r'; }

v=$(fx data.verb) || fail "data.verb did not read: $v"
[ "$(grep -c '^\[<ili>, generic verb sentence frames for words in the synset, <wnframe>\]$' <<<"$v")" = 2 ] || fail "the synset frames (w_num 00) are not relations to wnframe: $v"
grep -q 'unresolved obligations: 3 ' <<<"$v" || fail "data.verb's obligations: $v"
grep -Eq 'attest f_num +1$' <<<"$v" || fail "a frame of one word is not an unresolved obligation: $v"
grep -Eq 'attest target +2$' <<<"$v" || fail "the lexical pointers are not unresolved obligations: $v"
! grep -q 'left open' <<<"$v" || fail "data.verb leaves a part open: $v"

x=$(fx sentidx.vrb) || fail "sentidx.vrb did not read: $x"
[ "$(grep -c '^\[<ili>, example sentences illustrating the use of the verb sense, <wnsentence>\]$' <<<"$x")" = 2 ] || fail "abash's templates 126 and 127 are not relations to wnsentence: $x"
grep -q 'unresolved obligations: 0 ' <<<"$x" || fail "sentidx.vrb leaves obligations: $x"
! grep -Eq '%[1-5]:' <<<"$(grep '^\[' <<<"$x")" || fail "a sense key is content: $x"

for f in frames.vrb sents.vrb; do
  r=$(fx $f) || fail "$f did not read: $r"
  ! grep -Eq 'left open|no list of the highway holds' <<<"$r" || fail "$f: $r"
done
echo "wordnet-recipes: ok"