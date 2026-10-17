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
grep -q 'unresolved obligations: 0 ' <<<"$d" || fail "data.noun leaves obligations: $d"

i=$(claims index.adj) || fail "index.adj did not read: $i"
[ "$(grep -c '^\[able, synset containing lemma, ' <<<"$i")" = 4 ] || fail "able's four synsets are not its concepts: $i"
grep -q '^\[dissilient, synset containing lemma, ' <<<"$i" || fail "a satellite written a does not resolve as s: $i"
grep -q '^\[able, type of pointer that lemma has in all synsets containing it, !\]$' <<<"$i" || fail "able's pointer types: $i"
grep -q '^\[able, a\]$' <<<"$i" || fail "able's syntactic category: $i"
echo "wordnet-recipes: ok"