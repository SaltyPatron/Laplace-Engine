"""Semantic interpretation read back from the deployed database (the database half of tests/semantic-interpretation.py,
whose source-reading half runs without a database in the checks step): original sentence spacing and structured
treebank bindings survive publication; mirrored sentence exports and duplicate declarations manufacture no
observations; a dictionary homograph qualifier is one observation; repeated qualified observations keep their count and
first position; a validated subset publishes under its own trunk with its declared lineage and replays its testimony
exactly, as does a validated chess subset; the committed claims read back through hop and held; a vocabulary edit
rereads the file it changes; and a rejected reading publishes no witness and reports failure, never completion.

Scope: every source is the check's own (check-semantic-interpretation-<part>): the stock recipes of
universal-dependencies, tatoeba, wiktionary-kaikki, social-chemistry-101, social-chemistry-101-validated, twic-validated
and propbank copied under the check's witnesses, and the original's ad hoc sources (alignment, open identifiers). Where
the original declared a real lineage ("Social-Chem-101 Dataset", "The Week in Chess", "Wiktionary"), the copy declares the
check's own witness, so no fixture claim plays in a real source's matchup. Every series is read under the check's own
trunks, never every witness of the attestation table. hop reports read the whole database's evidence: their selectors
are texts only the check writes (laplace-unlisted.987654321, duck.03, PersonX ducks., see  a., Preferred cat.), and
they are asserted to contain the check's rows, not to be only those. Where the original asked hop of "duck" (which the
deployed database's lexicons answer at length), the check asks held for the claim and requires its row under the
check's trunk.
"""
import json
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import ROOT, Fixture, ids, sql  # noqa: E402

TESTS = ROOT / 'tests'
CONLLU = ('# text = {text}\n# text_en = Translated.\n'
          '1\tsee\tsee\tVERB\t_\t_\t0\troot\t_\tCxnElt=1:Agent\n'
          '2\ta\ta\tNOUN\t_\t_\t1\tobj\t_\tSpaceAfter=No|CxnElt=named:Patient\n'
          '3\t.\t.\tPUNCT\t_\t_\t1\tpunct\t_\t_\n\n')
ANNOTATIONS = ('# text = go home\n'
               '1\tgo\tgo\tVERB\t_\t_\t0\troot\t_\tFunctor=0:PRED|LUWPOS=verb|LUWBILabel=B|BunsetuPositionType=SEM_HEAD\n'
               '2\thome\thome\tNOUN\t_\t_\t1\tobl\t_\tFunctor=1:DIR3,2:LOC|CxnElt=1:Agent,named:Patient\n\n')
DUCK = '''<frameset><predicate lemma="duck">
<roleset id="duck.01" name="avoid"><aliases><alias pos="v">duck</alias></aliases>
<roles><role n="0" f="PAG" descr="avoider"><rolelinks><rolelink resource="FrameNet" class="leave-51.2">Mover</rolelink></rolelinks></role></roles>
<example name="first"><text>PersonX ducks.</text><propbank><arg type="ARG0">PersonX</arg></propbank></example></roleset>
<roleset id="duck.02" name="bird"><aliases><alias pos="n">duck</alias></aliases>
<roles><role n="0" f="PPT" descr="bird"/></roles>
<example name="second"><text>PersonX ducks.</text><propbank><arg type="ARG0">PersonX</arg><arg type="ARG1">PersonY</arg></propbank></example></roleset>
</predicate></frameset>'''
PLAIN_SENTENCES = '1\teng\tPreferred cat.\n2\teng\tFallback dog.\n'
RICH_SENTENCES = ''.join(line + '\t\\N\t2026-01-01\t2026-01-01\n' for line in PLAIN_SENTENCES.splitlines())
DICTIONARY = [
    {'lang_code': 'en', 'word': 'animal', 'pos': 'noun',
     'hyponyms': [{'word': 'domestic', 'tags': ['domestic'], 'raw_tags': ['regional label'], 'qualifier': 'chiefly rural'}],
     'instances': [{'word': 'Spot'}]},
    {'lang_code': 'en', 'word': 'move', 'pos': 'verb', 'troponyms': [{'word': 'run'}]}]
MISSING_MOVE = '[Event "Missing move fixture"]\n[Result "1-0"]\n\n1. e4 -- 2. Nf3 1-0\n'
INVALID_SELECTIONS = ('rot\tcharacters\trot-char-targeting\n' +
                      ''.join(f'Invalid {i}\tnarrator|friend\t{value}\n'
                              for i, value in enumerate(('char-2', 'char--1', 'char-4294967296'))))


def compose(*parts):
    """SQL for the claim composed of these texts."""
    return 'laplace_compose(ARRAY[' + ','.join(f"laplace_text_id('{p}')" for p in parts) + '])'


def games(trunks, claim, what='sum(games)'):
    return sql(f'SELECT {what} FROM laplace_contained_series({ids(trunks)}) WHERE claim={claim}')


def series(trunks):
    return sql(f'SELECT jsonb_agg(to_jsonb(s) ORDER BY claim,dependence_root) FROM laplace_contained_series({ids(trunks)}) s')


def lineage_text(trunk):
    return sql(f"SELECT laplace_text(lineage) FROM (SELECT lineage FROM attestation WHERE witness='{trunk}'::blake3"
               ' ORDER BY lineage NULLS FIRST LIMIT 1) a')


def events(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]


def fail(message, *context):
    raise SystemExit('semantic-interpretation: ' + message + ''.join('\n' + str(c) for c in context))


def main():
    with Fixture('semantic-interpretation') as fixture:
        one = {'LAPLACE_INGEST_ONE': '1'}

        def ingest(name, expected=0, extra=None):
            result = fixture.run('ingest', '-j', '4', name, check=False, timeout=600, env=dict(one, **(extra or {})))
            output = result.stdout + result.stderr
            if result.returncode != expected:
                fail(f'ingest {name} exited {result.returncode}, not {expected}', output)
            return output

        def single(name):
            found = fixture.trunks(name)
            if len(found) != 1:
                fail(f'{len(found)} trunks recorded for {name}, not 1')
            return found

        def hop(*selectors):
            return fixture.run('hop', '-n', '1000', *selectors).stdout

        def held_by(trunks, *parts):
            out = fixture.run('held', *parts).stdout
            return any(re.search(rf'^\s+[0-9a-f]{{32}}\s+witness\s+{t}\s', out, re.M) for t in trunks)

        # Source vocabulary alignment: one word's part of speech written three ways, an open roleset pointer, a mapping.
        alignment_recipes = {}
        for spelling, how, vocabulary in [('letters', 'crosswalk', 'wordnet-pos'), ('names', 'vocabulary', 'english-pos'),
                                          ('canonical', None, None)]:
            alignment_recipes[f'{spelling}.recipe'] = (
                f'name {fixture.name("alignment")[0]}-{spelling}\nmatch {spelling}.tsv\n'
                'tier row by \\n\nnames word pos pointer from to\ntier field by \\t\n'
                'thing row word\ncontent word from to\n'
                'type pos upos' + (f' {how} {vocabulary}' if vocabulary else '') + '\n'
                'type pointer pbroleset\nvalue upos pos\nvalue pbroleset pointer\n'
                'attest row pos pointer\nmaps row from pbroleset to to vnclass\n')
        alignment, alignment_root = fixture.source('alignment', 'class AcademicCurated', recipes=alignment_recipes)
        for spelling, pos in [('letters', 'n'), ('names', 'noun'), ('canonical', 'NOUN')]:
            (alignment_root / f'{spelling}.tsv').write_text(
                f'duck\t{pos}\tlaplace-unlisted.987654321\tduck.03\tnovel-987654321.2\n', encoding='utf-8', newline='\n')
        with (alignment_root / 'letters.tsv').open('a', encoding='utf-8', newline='\n') as file:
            file.write('duck\tv\tlaplace-unlisted.987654321\tduck.03\tnovel-987654321.2\n')

        propbank, propbank_root = fixture.source('propbank', 'class AcademicCurated', copy_from='propbank')
        (propbank_root / 'duck.xml').write_text(DUCK, encoding='utf-8', newline='\n')

        ud, ud_root = fixture.source('ud', 'class AcademicCurated', copy_from='universal-dependencies')
        (ud_root / 'spacing.conllu').write_text(CONLLU.format(text='see  a.'), encoding='utf-8', newline='\n')
        (ud_root / 'annotations.conllu').write_text(ANNOTATIONS, encoding='utf-8', newline='\n')

        tatoeba, tatoeba_root = fixture.source('tatoeba', 'class UserCuratedResource\n'
                                               'prefer sentences_detailed.csv over sentences.csv\n'
                                               'prefer sentences.csv over sentences_CC0.csv', copy_from='tatoeba')
        (tatoeba_root / 'links.csv').write_text('1\t2\n', encoding='utf-8', newline='\n')
        (tatoeba_root / 'sentences.csv').write_text(PLAIN_SENTENCES, encoding='utf-8', newline='\n')
        (tatoeba_root / 'sentences_detailed.csv').write_text(RICH_SENTENCES, encoding='utf-8', newline='\n')

        dictionary, dictionary_root = fixture.source('wiktionary', 'class UserCuratedResource', copy_from='wiktionary-kaikki')
        (dictionary_root / 'kaikki.org-dictionary-fixture.jsonl').write_text(
            ''.join(json.dumps(entry) + '\n' for entry in DICTIONARY), encoding='utf-8', newline='\n')

        # Two declarations of one reference in one source record.
        identifiers, identifiers_root = fixture.source('identifiers', 'class AcademicCurated', recipes={
            'redundant.recipe': f'name {fixture.name("identifiers")[0]}-redundant\nmatch redundant.tsv\n'
                                'tier row by \\n\nnames identifier\ntier field by \\t\nthing row identifier\n'
                                'type identifier pbroleset node\ntypes pbroleset "PropBank roleset" row\n'})
        (identifiers_root / 'redundant.tsv').write_text('single-unlisted.01\n', encoding='utf-8', newline='\n')

        chemistry, _ = fixture.source('chemistry', 'class AcademicCurated', copy_from='social-chemistry-101',
                                      root=fixture.data / 'chemistry')
        shutil.copyfile(TESTS / 'fixtures/social-chemistry/social-chem-101.v1.0.tsv',
                        fixture.data / 'chemistry/social-chem-101.v1.0.tsv')
        chemistry_witness = fixture.witness_of(chemistry)
        validated, validated_root = fixture.source('validated', f'class AcademicCurated\nlineage {chemistry_witness}',
                                                   copy_from='social-chemistry-101-validated')
        shutil.copyfile(TESTS / 'fixtures/social-chemistry/social-chem-101.v1.0.tsv', validated_root / 'social-chem-101.v1.0.tsv')
        (validated_root / 'quarantined-records.tsv').write_text(INVALID_SELECTIONS, encoding='utf-8', newline='\n')
        (validated_root / 'subset-receipt.json').write_text(
            json.dumps({'quarantined_rows': 3, 'invalid_references': 3}), encoding='utf-8', newline='\n')
        (validated_root / 'invalid-character-references.csv').write_text(
            'row,column,value\n2,rot-char-targeting,char-2\n', encoding='utf-8', newline='\n')

        chess_lineage = fixture.name('chess-publisher')[1]
        chess, chess_root = fixture.source('chess', f'class StructuredCorpus\nlineage {chess_lineage}', copy_from='twic-validated')
        shutil.copy2(TESTS / 'chess/kinds.pgn', chess_root / 'twic-fixture.pgn')
        (chess_root / 'quarantined-game-fixture-1.txt').write_text(MISSING_MOVE, encoding='utf-8', newline='\n')
        (chess_root / 'subset-receipt.json').write_text('{"excluded_games":1}\n', encoding='utf-8', newline='\n')
        (chess_root / 'grammar-validated-exclusions.json').write_text('{"game_ordinal":1}\n', encoding='utf-8', newline='\n')

        fixture.forget()

        ingest(alignment)
        ingest(propbank)
        alignment_trunks, propbank_trunks = single(alignment), single(propbank)

        publication = ingest(ud)
        ud_trunks = single(ud)
        if games(ud_trunks, compose('see  a.', 'the fluent translation of the sentence to English', 'Translated.')) != '1':
            fail('original sentence spacing did not survive database publication', publication)
        for relation, pairs in [('The tectogrammatical functor and its parent', [('go', 'DIR3'), ('home', 'LOC')]),
                                (None, [('go', 'Agent'), ('named', 'Patient')])]:
            operands = "laplace_text_id('home'),"
            if relation:
                operands += f"laplace_text_id('{relation}'),"
            operands += 'laplace_compose(ARRAY[' + ','.join(compose(*pair) for pair in pairs) + '])'
            if games(ud_trunks, f'laplace_compose(ARRAY[{operands}])') != '1':
                fail(f'structured bindings or parent association changed in PostgreSQL ({relation})', publication)

        ingest(tatoeba)
        if games(single(tatoeba), compose('Preferred cat.', 'ISO 639-3 language code', 'eng')) != '1':
            fail('mirrored sentence exports manufactured observations')
        if 'Fallback dog.' not in hop('Preferred cat.'):
            fail('hop does not read the translation link back')

        ingest(dictionary)
        if games(single(dictionary), compose('domestic', 'qualifier of this form or sense', 'domestic')) != '1':
            fail('the dictionary homograph qualifier was lost or duplicated')
        usage = hop('see  a.')
        # The CLI shortens long display labels; the whole relation and the spacing were read in SQL above.
        if 'the fluent translation of the sentence to' not in usage or 'Translated.' not in usage:
            fail('hop does not read the treebank translation back', usage)

        ingest(identifiers)
        if games(single(identifiers), compose('single-unlisted.01', 'identifier vocabulary', 'pbroleset')) != '1':
            fail('duplicate declarations manufactured observations')

        progress = fixture.scratch / 'chemistry-progress.jsonl'
        ingest(chemistry, extra={'LAPLACE_INGEST_PROGRESS_PATH': str(progress)})
        completed = [e for e in events(progress) if e['event'] == 'complete']
        chemistry_trunks = single(chemistry)
        if len(completed) != 1 or completed[0]['source_id'] != chemistry_trunks[0]:
            fail('the complete source did not report its published trunk once', completed, chemistry_trunks)
        repeated = games(chemistry_trunks, compose('First rule.', 'Who is the RoT most likely targeting', 'friend'),
                         "sum(games)::text || ':' || min(position)::text")
        if repeated != '2:1':
            fail(f'qualified observations or their source positions changed: {repeated}')

        subset_progress = fixture.scratch / 'validated-progress.jsonl'
        ingest(validated, extra={'LAPLACE_INGEST_PROGRESS_PATH': str(subset_progress)})
        subset = single(validated)
        if subset[0] == chemistry_trunks[0]:
            fail('a derived subset impersonated the complete publisher source')
        if lineage_text(subset[0]) != chemistry_witness:
            fail(f'the subset does not declare its lineage: {lineage_text(subset[0])!r}')
        before = series(subset)
        ingest(validated, extra={'LAPLACE_INGEST_PROGRESS_PATH': str(subset_progress)})
        if series(subset) != before:
            fail('the subset replay changed its published testimony')

        ingest(chess)
        chess_trunks = single(chess)
        # Lineage is carried on attestation rows; games are records under the trunk, so a chess source that attests
        # nothing has no row to carry it. Where it attests, every row carries its declared lineage.
        attested = sql(f"SELECT count(*) FROM attestation WHERE witness='{chess_trunks[0]}'::blake3")
        if attested != '0' and lineage_text(chess_trunks[0]) != chess_lineage:
            fail(f'the validated chess subset does not declare its lineage: {lineage_text(chess_trunks[0])!r}')
        before = series(chess_trunks)
        if before in ('', 'null'):
            fail('validated games published no Native testimony')
        ingest(chess)
        if series(chess_trunks) != before:
            fail('the validated chess replay changed its testimony')

        # The product consumers: committed, ranked claims read from PostgreSQL.
        for parts in [('duck', 'NOUN'), ('duck', 'VERB'), ('duck', 'laplace-unlisted.987654321')]:
            if not held_by(alignment_trunks, *parts):
                fail(f'held does not find [{", ".join(parts)}] under the check\'s alignment trunk')
        mapping = hop('duck.03')
        if 'cross-resource mapping' not in mapping or 'novel-987654321.2' not in mapping:
            fail('hop does not read the roleset mapping back', mapping)
        reference = hop('laplace-unlisted.987654321')
        if 'identifier vocabulary' not in reference or 'pbroleset' not in reference:
            fail('hop does not read the open identifier back', reference)
        usage = hop('PersonX ducks.')
        if not all(x in usage for x in ('duck.01', 'duck.02', 'PersonX', 'PersonY')):
            fail('hop does not read the scoped argument usages back', usage)

        # A vocabulary edit invalidates the OS/file precheck even when the corpus bytes, timestamps and recipe are unchanged.
        vocabulary = fixture.recipes / 'vocabularies/english-pos.tsv'
        vocabulary.write_text(vocabulary.read_text(encoding='utf-8').replace('noun NOUN', 'noun ADV'), encoding='utf-8')
        rereading = fixture.scratch / 'rereading.jsonl'
        ingest(alignment, extra={'LAPLACE_INGEST_PROGRESS_PATH': str(rereading)})
        if not any(e['event'] == 'file-composed' and e['file'].endswith('names.tsv') for e in events(rereading)):
            fail('the vocabulary edit did not reread the file it changes')
        alignment_trunks = fixture.trunks(alignment)
        if not held_by(alignment_trunks, 'duck', 'ADV'):
            fail('held does not find [duck, ADV] under the check\'s alignment trunk after the vocabulary edit')

        recorded = fixture.trunks(alignment)
        failed = fixture.scratch / 'failed.jsonl'
        (alignment_root / 'canonical.tsv').write_text('bad\tNOT_A_POS\tnew.03\tnew.03\tnew-99\n', encoding='utf-8', newline='\n')
        rejected = ingest(alignment, expected=1, extra={'LAPLACE_INGEST_PROGRESS_PATH': str(failed)})
        if 'semantic_omissions=1' not in rejected:
            fail('the rejected reading does not count its omission', rejected)
        frames = events(failed)
        if frames[-1]['event'] != 'failed' or any(e['event'] == 'complete' for e in frames):
            fail('the rejected reading reported completion', frames)
        if fixture.trunks(alignment) != recorded:
            fail('the rejected reading published a witness')
    print('PASS committed CLI readback and failed-source publication/progress boundary (deployed database)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
