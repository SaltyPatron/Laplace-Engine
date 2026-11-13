"""translate goes through the lexicalization [lemma, language, ILI] (Laplace-Engine#32), read back from the deployed
database: a word up to its concepts in the language asked and down to the words of another; a language asked by any of
the codes ISO 639-3 states are its; a sense whose synset CILI has not numbered keeps its source's synset identity; a
word's senses ordered by cntlist's observed usages before the unobserved ones; a recipe that renames what the firmware
reads fails translate at once (rc 2); typed UD value claims drive role salience (turn --read); and pull follows the
producer-declared sense, synset reference and definition.

Scope: four check sources hold the original's fixtures, each a copy of the stock recipes under the check's witness:
iso (ISO 639-3's table, iso-639's iso-639-3 recipe alone, without its own `witness SIL Global` line), oewn
(open-english-wordnet), omw (open-multilingual-wordnet's omw recipe alone; omw-en declares the real lineage WordNet)
and cntlist (princeton-wordnet), plus ud (universal-dependencies) for the roles. translate, hop, pull and turn read
the whole database's evidence: in the deployed database the real lexicons may add concepts, words and observations
to every answer. So the check asserts that the fixture's rows are in each answer, not that the answer is only them:
- "dog en en" and "chien fr en" have at least the fixture's concepts (the original: exactly 2), each with the
  fixture's words;
- the negative assertions that depend on what no other lexicon says are dropped: toutou not given for i46360, and
  i89566 with nothing in French (the original's "dog en fr" checks);
- the cntlist order keeps i37810, i37811, i37661 first, in that order, with non-increasing observed counts and i5573
  after them, rather than the exact counts 38, 26, 14 (another witness of the same usages adds to them) and i5573
  fourth (the real lexicon's other observed senses may stand between);
- "dog en en" keeps i46360 first with an observed count, not exactly 42.
Answers asked only of the fixture's own lemma ("unnumbered concept") stay exact.
"""
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import ROOT, Fixture  # noqa: E402

OEWN = '''<?xml version="1.0" encoding="UTF-8"?>
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
'''
OMW_FR = '''<?xml version="1.0" encoding="UTF-8"?>
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
'''
# ISO 639-3's code table, English's and French's rows as it writes them: eng is en in 639-1, fra is fr in 639-1 and fre
# in 639-2/B, so a language asked by any of its codes is the same language.
ISO = ('Id\tPart2b\tPart2t\tPart1\tScope\tLanguage_Type\tRef_Name\tComment\n'
       'eng\teng\teng\ten\tI\tL\tEnglish\t\nfra\tfre\tfra\tfr\tI\tL\tFrench\t\n')
# WordNet 3.0's cntlist, dog's and game's lines as it writes them: "tag_cnt sense_key sense_number".
CNTLIST = '42 dog%1:05:00:: 1\n38 game%1:04:00:: 1\n26 game%1:04:03:: 2\n14 game%1:04:01:: 3\n'
CONLLU = ('# text = game is played\n'
          '1\tgame\tgame\tNOUN\t_\t_\t3\tnsubj\t_\t_\n'
          '2\tis\tbe\tAUX\t_\t_\t3\taux\t_\t_\n'
          '3\tplayed\tplay\tVERB\t_\t_\t0\troot\t_\t_\n\n')
ROLE_FIRMWARE = 'role by UPOS\nrole 1 NOUN\nrole 0 AUX\nfor pull\nwalks 0\nnearest 0\nemit 0\n'
DOG = 'a member of the genus Canis'
MAN = 'informal term for a man'


def fail(message, out=''):
    raise SystemExit(f'translate: {message}\n{out}')


def line(language, text):
    """A language's lines of an answer."""
    return '\n'.join(l for l in text.splitlines() if re.match(rf'^ +{re.escape(language)} ', l))


def block(marker, text):
    """The paragraphs of an answer that hold marker (a concept's lines, found by its definition or its ILI)."""
    return '\n\n'.join(p for p in re.split(r'\n\s*\n', text) if marker in p)


def headers(text):
    return [l for l in text.splitlines() if ' ± ' in l]


def observed(header):
    found = re.search(r'observed (\d+) times', header)
    return int(found[1]) if found else None


def main():
    firmware = ROOT / 'firmware/program.firmware'
    with Fixture('translate') as fixture:
        iso_recipe = '\n'.join(l for l in (fixture.recipes / 'iso-639/iso-639-3.recipe').read_text(encoding='utf-8').splitlines()
                               if not l.startswith('witness ')) + '\n'
        iso, iso_root = fixture.source('iso', 'class StandardsDerived', recipes={'iso-639-3.recipe': iso_recipe})
        (iso_root / 'iso-639-3.tab').write_text(ISO, encoding='utf-8', newline='\n')
        oewn, oewn_root = fixture.source('oewn', 'class AcademicCurated', copy_from='open-english-wordnet')
        (oewn_root / 'english-wordnet-test.xml').write_text(OEWN, encoding='utf-8', newline='\n')
        omw, omw_root = fixture.source('omw', 'class AcademicCurated', recipes={
            'omw.recipe': (fixture.recipes / 'open-multilingual-wordnet/omw.recipe').read_text(encoding='utf-8')})
        (omw_root / 'omw-fr.xml').write_text(OMW_FR, encoding='utf-8', newline='\n')
        cntlist, cntlist_root = fixture.source('cntlist', 'class AcademicCurated', copy_from='princeton-wordnet')
        (cntlist_root / 'cntlist').write_text(CNTLIST, encoding='utf-8', newline='\n')
        ud, ud_root = fixture.source('ud', 'class AcademicCurated', copy_from='universal-dependencies')
        fixture.forget()
        for name in (iso, oewn, omw, cntlist):
            fixture.run('ingest', '-j', '2', name, env={'LAPLACE_INGEST_ONE': '1'})
            if len(fixture.trunks(name)) != 1:
                fail(f'{name} published no single trunk')

        def tl(*arguments, recipes=None):
            env = {'LAPLACE_RECIPES': str(recipes)} if recipes else None
            result = fixture.run('translate', '--firmware', firmware, *arguments, check=False, env=env)
            return result.stdout + result.stderr, result.returncode

        out, _ = tl('dog', 'en', 'en')
        l = line('en', block(DOG, out))
        if len(headers(out)) < 2 or not all(w in l for w in ('dog', 'domestic dog', 'Canis familiaris')):
            fail('dog en en: i46360 is not dog, domestic dog and Canis familiaris among dog\'s concepts', out)
        l = line('en', block(MAN, out))
        if 'dog' not in l or 'domestic' in l:
            fail('dog en en: i89566 is not dog alone', out)
        print('translate: dog en en: i46360 is dog, domestic dog, Canis familiaris, shown with its definition; i89566 is dog')

        en_fr, _ = tl('dog', 'en', 'fr')
        l = line('fr', block(DOG, en_fr))
        if not all(w in l for w in ('chien', 'canis familiaris')):
            fail('dog en fr: i46360 has no French chien and canis familiaris', en_fr)
        print('translate: dog en fr: i46360 is chien, canis familiaris')

        french = lambda text, language: sorted(re.findall(r'chien|canis familiaris|toutou', line(language, block(DOG, text))))
        for query in (('dog', 'eng', 'fra'), ('dog', 'en', 'fre'), ('dog', 'eng', 'fr')):
            o, _ = tl(*query)
            if len(headers(o)) != len(headers(en_fr)) or french(o, query[-1]) != french(en_fr, 'fr'):
                fail(f'{" ".join(query)}: not the concepts and French words of dog en fr', o)
        o, _ = tl('chien', 'fra', 'eng')
        if 'domestic dog' not in line('eng', block(DOG, o)):
            fail('chien fra eng: i46360 has no domestic dog', o)
        print('translate: dog eng fra is dog en fr: a language is asked by any of the codes ISO 639-3 states are its')

        out, _ = tl('chien', 'fr', 'en')
        if len(headers(out)) < 2 or 'domestic dog' not in line('en', block(DOG, out)):
            fail('chien fr en: not chien\'s concepts, i46360 with domestic dog', out)
        out, _ = tl('dog', 'fr', 'en')
        if 'no lexicalization of it is attested in a language written fr' not in out:
            fail('dog fr en: dog is no French word', out)
        out, _ = tl('unnumbered concept', 'en', 'en')
        found = headers(out)
        if len(found) != 1 or 'oewn-99999999-n' not in found[0] or 'unnumbered concept' not in line('en', out):
            fail('a sense whose synset has no ILI did not retain its source\'s synset identity', out)
        if 'nothing recorded' in fixture.run('hop', 'unnumbered concept', check=False).stdout:
            fail('the sense whose synset has no ILI was not read')
        print('translate: chien fr en goes back to English; a sense of a synset CILI has not numbered is the word with its synset')

        # By observed usages (Sequence/Web.md 15.6: "ordered by observed usages, then the witness's sense order, then
        # standing"): game's senses as cntlist counts them, before the adjective senses the witness lists first.
        out, _ = tl('game', 'en', 'en')
        order = headers(out)
        first = [next((i for i, h in enumerate(order) if ili in h), None) for ili in ('i37810', 'i37811', 'i37661', 'i5573')]
        counts = [observed(order[i]) if i is not None else None for i in first[:3]]
        if first[:3] != [0, 1, 2] or None in counts or counts != sorted(counts, reverse=True):
            fail('game en en: i37810, i37811 and i37661 are not first, by their observed counts', out)
        if first[3] is None or first[3] < 3:
            fail('game en en: the unobserved adjective sense i5573 does not come after the observed ones', out)
        definition = block('i37810', out)
        if 'a contest with rules' not in definition or 'a single play' in definition:
            fail('game en en: i37810 is shown with the wrong definition', out)
        out, _ = tl('dog', 'en', 'en')
        found = headers(out)
        if not found or observed(found[0]) is None or DOG not in block(found[0], out):
            fail('dog en en: i46360 is not first, with its observed count', out)
        print('translate: game en en orders game\'s senses by cntlist\'s counts (i37810, i37811, i37661), then the unobserved')

        # Drift: a recipe that no longer declares what the firmware names fails translate, loudly.
        renamed = fixture.scratch / 'recipes-renamed'
        shutil.copytree(fixture.recipes, renamed)
        path = renamed / 'wn-lmf.recipe'
        path.write_text(re.sub(r'^relation "lexical sense" Sense$', 'relation "lexical sense" Senses',
                               path.read_text(encoding='utf-8'), flags=re.M), encoding='utf-8', newline='\n')
        err, rc = tl('dog', 'en', 'fr', recipes=renamed)
        if rc != 2 or 'declares no relation of Sense' not in err:
            fail(f'a recipe that declares no relation of Sense did not fail translate (rc {rc})', err)
        renamed = fixture.scratch / 'recipes-renamed-games'
        shutil.copytree(fixture.recipes, renamed)
        path = renamed / 'princeton-wordnet/cntlist.recipe'
        path.write_text(re.sub(r'^games row tag_cnt$', 'games row tag_count', path.read_text(encoding='utf-8'), flags=re.M),
                        encoding='utf-8', newline='\n')
        err, rc = tl('game', 'en', 'en', recipes=renamed)
        if rc != 2 or 'usages: the recipe wordnet-cntlist declares no games' not in err:
            fail(f'a cntlist recipe that declares no games of tag_cnt did not fail translate (rc {rc})', err)
        print('translate: a recipe that renames what translate reads fails it at once (rc 2)')

        # Salience reads the same typed value claims the general UD recipe writes. A zero-weight AUX differs from a
        # missing role (which defaults to weight 1).
        (ud_root / 'roles.conllu').write_text(CONLLU, encoding='utf-8', newline='\n')
        fixture.run('ingest', '-j', '2', ud, env={'LAPLACE_INGEST_ONE': '1'})
        roles = fixture.scratch / 'roles.firmware'
        roles.write_text(ROLE_FIRMWARE, encoding='utf-8', newline='\n')
        read = fixture.run('turn', '--read', '--firmware', roles, 'game is', check=False)
        if read.returncode or not re.search(r'game\(1\.00\).*is~\(0\.00\)', read.stdout + read.stderr):
            fail('typed UPOS salience was not read', read.stdout + read.stderr)
        print('query roles: Native UD value claims drive NOUN/AUX salience without changing standings')
        result = fixture.run('pull', '--firmware', firmware, '--seed', '0', 'dog', check=False)
        answers = [l for l in result.stdout.splitlines() if l.startswith('answer ')]
        if result.returncode or not any(DOG in a for a in answers):
            fail('pull: the shared recipe chain did not read the stored definition', result.stdout + result.stderr)
        print('query navigation: pull follows the producer-declared sense, synset reference and definition')
    return 0


if __name__ == '__main__':
    sys.exit(main())
