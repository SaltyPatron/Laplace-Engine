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
# Open English WordNet 2025+ (english-wordnet-2025-plus.xml), the entries and synsets of dog's first two senses, and
# game's entries as the file writes them: its adjective entry (lame; plucky) before its noun entry (contest, single
# play, pastime), so the witness's own order puts game's least used senses first
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
    <LexicalEntry id="oewn-game-a">
      <Lemma writtenForm="game" partOfSpeech="a"/>
      <Sense id="oewn-game__5.00.00.unfit.01" synset="oewn-01022380-s"/>
      <Sense id="oewn-game__5.00.00.brave.00" synset="oewn-00264860-s"/>
    </LexicalEntry>
    <LexicalEntry id="oewn-game-n">
      <Lemma writtenForm="game" partOfSpeech="n"/>
      <Sense id="oewn-game__1.04.00.." synset="oewn-00456623-n"/>
      <Sense id="oewn-game__1.04.03.." synset="oewn-00457223-n"/>
      <Sense id="oewn-game__1.04.01.." synset="oewn-00431591-n"/>
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
    <Synset id="oewn-01022380-s" ili="i5573" members="oewn-game-a" partOfSpeech="s" lexfile="adj.all">
      <Definition>disabled in the feet or legs</Definition>
    </Synset>
    <Synset id="oewn-00264860-s" ili="i1478" members="oewn-game-a" partOfSpeech="s" lexfile="adj.all">
      <Definition>willing to face danger</Definition>
    </Synset>
    <Synset id="oewn-00456623-n" ili="i37810" members="oewn-game-n" partOfSpeech="n" lexfile="noun.act">
      <Definition>a contest with rules to determine a winner</Definition>
    </Synset>
    <Synset id="oewn-00457223-n" ili="i37811" members="oewn-game-n" partOfSpeech="n" lexfile="noun.act">
      <Definition>a single play of a sport or other contest</Definition>
    </Synset>
    <Synset id="oewn-00431591-n" ili="i37661" members="oewn-game-n" partOfSpeech="n" lexfile="noun.act">
      <Definition>an amusement or pastime</Definition>
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

# ISO 639-3's code table (iso-639-3.tab, SIL), English's and French's rows as it writes them: eng is en in 639-1, fra
# is fr in 639-1 and fre in 639-2/B, so a language asked by any of its codes is the same language
mkdir -p "$LAPLACE_DATA/ISO639"
printf 'Id\tPart2b\tPart2t\tPart1\tScope\tLanguage_Type\tRef_Name\tComment\neng\teng\teng\ten\tI\tL\tEnglish\t\nfra\tfre\tfra\tfr\tI\tL\tFrench\t\n' >"$LAPLACE_DATA/ISO639/iso-639-3.tab"
# WordNet 3.0's cntlist (dict/cntlist), dog's and game's lines as it writes them: "tag_cnt sense_key sense_number". Each
# sense key is the lexicalization [lemma, en, ILI] its lemma and concept make (recipes/princeton-wordnet/cntlist.recipe),
# its concept through the highway (index.sense's offset, CILI's ILI); the count orders translate's senses
wn=$LAPLACE_DATA/Wordnet/WordNet-3.0/dict; mkdir -p "$wn"
printf '42 dog%%1:05:00:: 1\n38 game%%1:04:00:: 1\n26 game%%1:04:03:: 2\n14 game%%1:04:01:: 3\n' >"$wn/cntlist"
"$L" deploy -d "$conn" >"$scratch/deploy.log" 2>&1 || fail "deploy: $(tail -5 "$scratch/deploy.log")"
for s in iso-639 open-english-wordnet open-multilingual-wordnet princeton-wordnet; do
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

# en and eng, fr, fra and fre are one language's codes, joined by the links ISO 639-3's table states (Sources, tier 1)
for q in 'dog eng fra' 'dog en fre' 'dog eng fr'; do
  o=$(tl $q); [[ $(concepts "$o") == $(concepts "$out") ]] || fail "$q: not the concepts of dog en fr: $o"
  [[ "$(line "${q##* }" "$(block "$dog" "$o")" | grep -o 'chien\|canis familiaris\|toutou' | sort)" == "$(line fr "$(block "$dog" "$out")" | grep -o 'chien\|canis familiaris\|toutou' | sort)" ]] || fail "$q: i46360's French words are not dog en fr's: $o"
done
o=$(tl chien fra eng); l=$(line eng "$(block "$dog" "$o")"); grep -q 'domestic dog' <<<"$l" || fail "chien fra eng: i46360 has no domestic dog: $o"
echo "translate: dog eng fra is dog en fr: a language is asked by any of the codes ISO 639-3 states are its"

out=$(tl chien fr en)
[[ $(concepts "$out") == 2 ]] || fail "chien fr en: not chien's two concepts (i46360, i46361): $out"
l=$(line en "$(block "$dog" "$out")"); grep -q 'domestic dog' <<<"$l" || fail "chien fr en: i46360 has no domestic dog: $out"
out=$(tl dog fr en); grep -q 'no lexicalization of it is attested in a language written fr' <<<"$out" || fail "dog fr en: dog is no French word: $out"
out=$(tl 'unnumbered concept' en en); grep -q 'no lexicalization of it' <<<"$out" || fail "a sense whose synset has no ILI was given a stand-in concept: $out"
"$L" hop -d "$conn" 'unnumbered concept' 2>&1 | grep -q 'nothing recorded' && fail "the sense whose synset has no ILI was not read"
echo "translate: chien fr en goes back to English; a sense of a synset CILI has not numbered is the word with its synset"

# by observed usages (Laplace-Wiki Sequence/Web.md 15.6: "ordered by observed usages, then the witness's sense order,
# then standing"): game's senses as cntlist counts them, 38, 26, 14, before the adjective senses the witness lists first
# and no concordance tags
out=$(tl game en en)
order=$(grep ' ± ' <<<"$out")      # a concept is shown as its text, here its definition (the comment on block, above)
[[ $(sed -n 1p <<<"$order") == *'a contest with rules to determine a winner'*'observed 38 times' ]] || fail "game en en: the first concept is not i37810 (a contest with rules), observed 38 times: $out"
[[ $(sed -n 2p <<<"$order") == *'a single play of a sport'*'observed 26 times' && $(sed -n 3p <<<"$order") == *'an amusement or pastime'*'observed 14 times' ]] || fail "game en en: i37811 (26) and i37661 (14) do not follow: $out"
[[ $(sed -n 4p <<<"$order") == *'disabled in the feet or legs' ]] || fail "game en en: the unobserved senses do not come after the observed ones, in the witness's order (i5573 first): $out"
# game's three noun synsets each list [game], so they are one synset (recipes/wn-lmf.recipe): its definitions are not
# shown as any one concept's
[[ $(sed -n 2p <<<"$(block 'a contest with rules' "$out")") != *'a single play'* ]] || fail "game en en: i37810 is shown with i37811's definition: $out"
out=$(tl dog en en); [[ $(grep ' ± ' <<<"$out" | head -1) == *'observed 42 times' ]] && grep -q 'a member of the genus Canis' <<<"$(block 'observed 42' "$out")" || fail "dog en en: i46360 is not first, observed 42 times: $out"
echo "translate: game en en orders game's senses by cntlist's counts (i37810 38, i37811 26, i37661 14), then the unobserved"

# drift: a recipe that no longer declares what the firmware names fails translate, loudly
r=$scratch/recipes; cp -r "$LAPLACE_RECIPES" "$r"
sed -i 's/^relation "lexical sense" Sense$/relation "lexical sense" Senses/' "$r/wn-lmf.recipe"
rc=0; err=$(LAPLACE_RECIPES=$r "$L" translate -d "$conn" --firmware "$fwf" dog en fr 2>&1) || rc=$?
[[ $rc == 2 ]] && grep -q 'declares no relation of Sense' <<<"$err" || fail "a recipe that declares no relation of Sense did not fail translate (rc $rc): $err"
echo "translate: a recipe that renames what translate reads fails it at once (rc 2)"
r2=$scratch/recipes2; cp -r "$LAPLACE_RECIPES" "$r2"
sed -i 's/^relation "number of times the sense is tagged" tag_cnt$/relation "number of times the sense is tagged" tag_count/' "$r2/princeton-wordnet/cntlist.recipe"
rc=0; err=$(LAPLACE_RECIPES=$r2 "$L" translate -d "$conn" --firmware "$fwf" game en en 2>&1) || rc=$?
[[ $rc == 2 ]] && grep -q 'usages: the recipe wordnet-cntlist declares no relation' <<<"$err" || fail "a cntlist recipe that declares no relation of tag_cnt did not fail translate (rc $rc): $err"
echo "translate: a usage recipe that renames what translate reads fails it at once (rc 2)"
