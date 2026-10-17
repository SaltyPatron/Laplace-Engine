#!/bin/bash
# translate goes through the lexicalization [lemma, language, ILI] (Laplace-Engine#32): a word up to its concepts in the
# language asked, and back down to the words of another. Its way is read from the WN-LMF recipe's own declarations, so a
# recipe that renames them fails translate at once instead of leaving it empty. A small Open English WordNet and an Open
# Multilingual Wordnet French lexicon, as their files write them, go into a disposable database (lp_translate_*),
# dropped after.
#   tests/translate.sh path/to/laplace        (PGHOST, PGUSER as the runner's; LAPLACE_TIER0, LAPLACE_HIGHWAY: the
#                                             perf-caches, the highway holding CILI's interlingual index)
set -euo pipefail
L=${1:?usage: tests/translate.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/translate.XXXXXX")
db=lp_translate_$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
dropdb=${DROPDB:-$(dirname "${PSQL:-$(command -v psql)}")/dropdb}
trap '"$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db" >/dev/null 2>&1 || true; rm -rf "$scratch"' EXIT
fail(){ echo "translate: $*" >&2; exit 1; }
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
en=$LAPLACE_DATA/OpenEnglishWordNet-test; fr=$LAPLACE_DATA/OMW-2.0/omw-2.0/omw-fr; mkdir -p "$en" "$fr"
# Open English WordNet 2025+ (english-wordnet-2025-plus.xml), the entries and synsets of dog's first two senses
cat > "$en/english-wordnet-test.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE LexicalResource SYSTEM "http://globalwordnet.github.io/schemas/WN-LMF-1.3.dtd">
<LexicalResource xmlns:dc="https://globalwordnet.github.io/schemas/dc/">
  <Lexicon id="oewn"
           label="Open English Wordnet"
           language="en"
           email="english-wordnet@googlegroups.com"
           license="https://creativecommons.org/licenses/by/4.0"
           version="2025+"
           url="https://github.com/globalwordnet/english-wordnet">
    <LexicalEntry id="oewn-dog-n">
      <Lemma writtenForm="dog" partOfSpeech="n">
        <Pronunciation variety="GB">dɒɡ</Pronunciation>
      </Lemma>
      <Sense id="oewn-dog__1.05.00.." synset="oewn-02086723-n"/>
      <Sense id="oewn-dog__1.18.00.." synset="oewn-10042764-n"/>
    </LexicalEntry>
    <LexicalEntry id="oewn-domestic_dog-n">
      <Lemma writtenForm="domestic dog" partOfSpeech="n"/>
      <Sense id="oewn-domestic_dog__1.05.00.." synset="oewn-02086723-n"/>
    </LexicalEntry>
    <LexicalEntry id="oewn-Canis_familiaris-n">
      <Lemma writtenForm="Canis familiaris" partOfSpeech="n"/>
      <Sense id="oewn-canis_familiaris__1.05.00.." synset="oewn-02086723-n"/>
    </LexicalEntry>
    <LexicalEntry id="oewn-unnumbered-n">
      <Lemma writtenForm="unnumbered concept" partOfSpeech="n"/>
      <Sense id="oewn-unnumbered__1.05.00.." synset="oewn-99999999-n"/>
    </LexicalEntry>
    <Synset id="oewn-02086723-n" ili="i46360" members="oewn-dog-n oewn-domestic_dog-n oewn-Canis_familiaris-n" partOfSpeech="n" lexfile="noun.animal">
      <Definition>a member of the genus Canis (probably descended from the common wolf) that has been domesticated by man since prehistoric times; occurs in many breeds</Definition>
    </Synset>
    <Synset id="oewn-10042764-n" ili="i89566" members="oewn-dog-n" partOfSpeech="n" lexfile="noun.person">
      <Definition>informal term for a man</Definition>
    </Synset>
    <Synset id="oewn-99999999-n" ili="in" members="oewn-unnumbered-n" partOfSpeech="n" lexfile="noun.animal">
      <Definition>a concept proposed to CILI and not yet numbered</Definition>
    </Synset>
  </Lexicon>
</LexicalResource>
XML
# OMW 2.0's French (omw-fr.xml, WOLF): chien's first two senses, as the file writes them (members are sense ids)
cat > "$fr/omw-fr.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE LexicalResource SYSTEM "https://globalwordnet.github.io/schemas/WN-LMF-1.4.dtd">
<LexicalResource xmlns:dc="https://globalwordnet.github.io/schemas/dc/">
  <Lexicon id="omw-fr"
           label="WOLF (Wordnet Libre du Français)"
           language="fr"
           email="bond@ieee.org"
           license="http://www.cecill.info/licenses/Licence_CeCILL-C_V1-en.html"
           version="2.0"
           url="http://pauillac.inria.fr/~sagot/index.html#wolf">
    <LexicalEntry id="omw-fr-chien-n">
      <Lemma writtenForm="chien" partOfSpeech="n" />
      <Sense id="omw-fr-chien-02084071-n" synset="omw-fr-02084071-n" />
      <Sense id="omw-fr-chien-02084732-n" synset="omw-fr-02084732-n" />
    </LexicalEntry>
    <LexicalEntry id="omw-fr-canis_familiaris-n">
      <Lemma writtenForm="canis familiaris" partOfSpeech="n" />
      <Sense id="omw-fr-canis_familiaris-02084071-n" synset="omw-fr-02084071-n" />
    </LexicalEntry>
    <LexicalEntry id="omw-fr-toutou-n">
      <Lemma writtenForm="toutou" partOfSpeech="n" />
      <Sense id="omw-fr-toutou-02084732-n" synset="omw-fr-02084732-n" />
    </LexicalEntry>
    <Synset id="omw-fr-02084071-n" ili="i46360" partOfSpeech="n" members="omw-fr-chien-02084071-n omw-fr-canis_familiaris-02084071-n" />
    <Synset id="omw-fr-02084732-n" ili="i46361" partOfSpeech="n" members="omw-fr-toutou-02084732-n omw-fr-chien-02084732-n" />
  </Lexicon>
</LexicalResource>
XML

"$L" deploy -d "$conn" >"$scratch/deploy.log" 2>&1 || fail "deploy: $(tail -5 "$scratch/deploy.log")"
for s in open-english-wordnet open-multilingual-wordnet; do
  LAPLACE_INGEST_ONE=1 "$L" ingest -d "$conn" -j 2 "$s" >"$scratch/$s.run" 2>&1 || fail "ingest $s: $(tail -20 "$scratch/$s.run")"
done
fwf=$here/../firmware/program.firmware
tl(){ "$L" translate -d "$conn" --firmware "$fwf" "$@" 2>&1 || true; }
line(){ grep -E "^ +$1 " <<<"$2" || true; }      # a language's line of an answer
# a concept's lines, found by its definition (the concept itself is shown as its text, the ILI, where CILI is read)
block(){ awk -v d="$1" '/ ± / {on = index($0, d) > 0} on' <<<"$2"; }
concepts(){ grep -c ' ± ' <<<"$1" || true; }
dog='a member of the genus Canis'; man='informal term for a man'

out=$(tl dog en en)
[[ $(concepts "$out") == 2 ]] || fail "dog en en: not dog's two concepts (i46360, i89566): $out"
l=$(line en "$(block "$dog" "$out")")
for w in 'dog' 'domestic dog' 'Canis familiaris'; do grep -q "$w" <<<"$l" || fail "dog en en: i46360 has no English $w: $out"; done
l=$(line en "$(block "$man" "$out")"); [[ "$l" == *dog* && "$l" != *domestic* ]] || fail "dog en en: i89566 is not dog alone: $out"
echo "translate: dog en en: i46360 is dog, domestic dog, Canis familiaris, shown with its definition; i89566 is dog"

out=$(tl dog en fr)
l=$(line fr "$(block "$dog" "$out")")
for w in 'chien' 'canis familiaris'; do grep -q "$w" <<<"$l" || fail "dog en fr: i46360 has no French $w: $out"; done
[[ "$l" != *toutou* ]] || fail "dog en fr: toutou (i46361) is given for i46360: $out"
grep -q 'nothing attested' <<<"$(line fr "$(block "$man" "$out")")" || fail "dog en fr: i89566 has French words, and the fixture gives none: $out"
echo "translate: dog en fr: i46360 is chien, canis familiaris; i89566 has nothing in French"

out=$(tl chien fr en)
[[ $(concepts "$out") == 2 ]] || fail "chien fr en: not chien's two concepts (i46360, i46361): $out"
l=$(line en "$(block "$dog" "$out")"); grep -q 'domestic dog' <<<"$l" || fail "chien fr en: i46360 has no domestic dog: $out"
out=$(tl dog fr en); grep -q 'no lexicalization of it is attested in a language written fr' <<<"$out" || fail "dog fr en: dog is no French word: $out"
out=$(tl 'unnumbered concept' en en); grep -q 'no lexicalization of it' <<<"$out" || fail "a sense whose synset has no ILI was given a stand-in concept: $out"
"$L" hop -d "$conn" 'unnumbered concept' 2>&1 | grep -q 'nothing recorded' && fail "the sense whose synset has no ILI was not read"
echo "translate: chien fr en goes back to English; a sense of a synset CILI has not numbered is the word with its synset"

# drift: a recipe that no longer declares what the firmware names fails translate, loudly
r=$scratch/recipes; cp -r "$LAPLACE_RECIPES" "$r"
sed -i 's/^relation "lexical sense" Sense$/relation "lexical sense" Senses/' "$r/wn-lmf.recipe"
rc=0; err=$(LAPLACE_RECIPES=$r "$L" translate -d "$conn" --firmware "$fwf" dog en fr 2>&1) || rc=$?
[[ $rc == 2 ]] && grep -q 'declares no relation of Sense' <<<"$err" || fail "a recipe that declares no relation of Sense did not fail translate (rc $rc): $err"
echo "translate: a recipe that renames what translate reads fails it at once (rc 2)"