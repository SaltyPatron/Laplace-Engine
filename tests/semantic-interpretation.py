#!/usr/bin/env python3
"""Source notation, open identifiers, scoped argument slots and real readback."""
import argparse
import csv
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--database', action='store_true')
    parser.add_argument('--recipes', type=Path,
                        help='Verify the selected installed recipe resources instead of checkout resources')
    args = parser.parse_args()
    engine = args.engine.resolve()
    installed_recipes = args.recipes.resolve() if args.recipes else Path(__file__).resolve().parents[1] / 'recipes'
    with tempfile.TemporaryDirectory(prefix='semantic-interpretation-', dir=os.environ.get('TMPDIR')) as scratch:
        root = Path(scratch)
        recipes = root / 'recipes'
        # Runtime recipes refer to already compiled grammars. Grammar source
        # trees belong to preparation/build and are not per-check payloads.
        shutil.copytree(installed_recipes, recipes, ignore=shutil.ignore_patterns('grammars'))
        shutil.copytree(installed_recipes.parent / 'firmware', root / 'firmware')
        source = recipes / 'semantic-alignment'
        source.mkdir()
        (source / 'source').write_text(
            'name semantic-alignment\nwitness Source vocabulary alignment\nclass AcademicCurated\n'
            'root $LAPLACE_DATA/alignment\n', encoding='utf-8')
        data = root / 'data/alignment'
        data.mkdir(parents=True)
        for spelling, vocabulary, pos in [('letters', 'wordnet-pos', 'n'),
                                           ('names', 'english-pos', 'noun'),
                                           ('canonical', None, 'NOUN')]:
            (source / f'{spelling}.recipe').write_text(
                f'name align-{spelling}\nmatch {spelling}.tsv\n'
                'tier row by \\n\nnames word pos pointer from to\ntier field by \\t\n'
                'thing row word\ncontent word from to\n'
                'type pos upos' + (f' vocabulary {vocabulary}' if vocabulary else '') + '\n'
                'type pointer pbroleset\nvalue upos pos\nvalue pbroleset pointer\n'
                'attest row pos pointer\nmaps row from pbroleset to to vnclass\n', encoding='utf-8')
            (data / f'{spelling}.tsv').write_text(
                f'duck\t{pos}\tlaplace-unlisted.987654321\tduck.03\tnovel-987654321.2\n', encoding='utf-8')
        with (data / 'letters.tsv').open('a', encoding='utf-8') as file:
            file.write('duck\tv\tlaplace-unlisted.987654321\tduck.03\tnovel-987654321.2\n')
        propbank = root / 'data/propbank'
        propbank.mkdir()
        (recipes / 'propbank/source').write_text(
            'name propbank\nwitness Scoped predicate arguments\nclass AcademicCurated\n'
            'root $LAPLACE_DATA/propbank\n', encoding='utf-8')
        (propbank / 'duck.xml').write_text('''<frameset><predicate lemma="duck">
<roleset id="duck.01" name="avoid"><aliases><alias pos="v">duck</alias></aliases>
<roles><role n="0" f="PAG" descr="avoider"><rolelinks><rolelink resource="FrameNet" class="leave-51.2">Mover</rolelink></rolelinks></role></roles>
<example name="first"><text>PersonX ducks.</text><propbank><arg type="ARG0">PersonX</arg></propbank></example></roleset>
<roleset id="duck.02" name="bird"><aliases><alias pos="n">duck</alias></aliases>
<roles><role n="0" f="PPT" descr="bird"/></roles>
<example name="second"><text>PersonX ducks.</text><propbank><arg type="ARG0">PersonX</arg><arg type="ARG1">PersonY</arg></propbank></example></roleset>
</predicate></frameset>''', encoding='utf-8')
        env = dict(os.environ, LAPLACE_RECIPES=str(recipes), LAPLACE_DATA=str(root / 'data'),
                   LAPLACE_WORK=str(root / 'work'), LAPLACE_INGEST_ONE='1')

        def run(*arguments, expected=0, extra=None):
            result = subprocess.run([str(engine), *map(str, arguments)], env=dict(env, **(extra or {})),
                                    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
            output = result.stdout + result.stderr
            assert result.returncode == expected, (arguments, result.returncode, output)
            return output

        # Real CLDR excerpts cover every field class which rejected the ISO639
        # foundation rung. Check the claims as well as the omission counter:
        # declaring a source key must not suppress other subjects' type fields.
        cldr = Path(__file__).resolve().parent / 'fixtures/cldr/supplementalData.xml'
        expected_cldr = [
            ('Hanb', 'compound'), ('Hanb', 'Equivalent', 'Hani Bopo'),
            ('Aran', 'visual'), ('Aran', 'Equivalent', 'Arab'),
            ('Hans', 'subset'), ('Hans', 'Equivalent', 'Hani'),
            ('AC', 'GDP figures', '50800000'),
            ('AC', 'literacy percent for the territory', '99'),
            ('AC', 'country’s population', '940'),
            ('[AC, en]', 'language population percent', '99'),
            ('[AC, en]', 'references and standards', 'R1020'),
            ('[gregorian, 0]', 'invariant strings for identifying the eras', 'bce'),
            ('[gregorian, 0]', 'space-separated identifiers, each also unique within the calendar', 'bc'),
            ('R1020', 'references and standards', '[missing]'),
        ]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'iso-639', cldr)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected_cldr), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        print('PASS CLDR populations, forward reference keys, script variants and unused era ends across worker counts')

        exceptions = Path(__file__).resolve().parent / 'fixtures/wordnet-exceptions'
        expected_exceptions = [
            ('aardwolves', 'NOUN'), ('[aardwolves, NOUN]', 'base form', 'aardwolf'),
            ('axes', 'NOUN'), ('[axes, NOUN]', 'base form', 'ax'), ('[axes, NOUN]', 'base form', 'axis'),
            ('abetted', 'VERB'), ('[abetted, VERB]', 'base form', 'abet'),
            ('acer', 'ADJ'), ('[acer, ADJ]', 'base form', 'acer'),
            ('best', 'ADV'), ('[best, ADV]', 'base form', 'well'),
        ]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'princeton-wordnet',
                       *(exceptions / name for name in ('noun.exc', 'verb.exc', 'adj.exc', 'adv.exc')))
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected_exceptions), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        print('PASS all four WordNet exception categories, multiple base forms and identity forms across worker counts')

        arguments = Path(__file__).resolve().parent / 'fixtures/verbatlas-arguments'
        expected_arguments = [
            ('i46360', 'shadow argument'),
            ('[i46360, shadow argument]', 'Theme', 'bn:00070772n'),
            ('[i46360, shadow argument]', 'Instrument', 'bn:00000902n'),
            ('[i46360, shadow argument]', 'Result', 'bn:00098546a'),
            ('[i46360, shadow argument]', 'Source', 'bn:00021015n'),
            ('i46360', 'implicit argument'),
            ('[i46360, implicit argument]', 'Theme', 'bn:00035378n'),
            ('[i46360, implicit argument]', 'Theme', 'bn:00035596n'),
            ('va:1001f', 'name of a VerbAtlas frame', 'AUXILIARY'),
            ('va:1001f', 'frame definition', 'Supporting frame'),
            ('va:1001f', 'small definition of the frame', 'Auxiliary.'),
            ('va:1001f', 'identifier vocabulary', 'vaframe'),
            ('[vaframe, va:1001f]', 'identifies', 'va:1001f'),
            ('va:0001f', 'name of a VerbAtlas frame', 'TOLERATE'),
            ('va:0001f', 'frame definition', 'An agent tolerates a theme'),
            ('va:0001f', 'the synset that roughly represents the frame', 'i46360'),
            ('va:0001f', 'small definition of the frame', 'Tolerate.'),
            ('i46360', 'Put up with something'),
            ('va:0001f', 'identifier vocabulary', 'vaframe'),
            ('[vaframe, va:0001f]', 'identifies', 'va:0001f'),
            *[('i46360', 'identifier vocabulary', 'ili')] * 3,
        ]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'verbatlas',
                       *(arguments / name for name in ('VA_bn2shadow.tsv', 'VA_bn2implicit.tsv', 'VA_frame_info.tsv')))
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected_arguments), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        # An extended layout must be rejected rather than silently dropping the
        # fifth argument, which the old three-column recipe did for pairs 2..4.
        overflow = root / 'VA_bn2shadow.tsv'
        overflow.write_text((arguments / overflow.name).read_text(encoding='utf-8').rstrip() +
                            '\tAgent\tbn:00011769n\n', encoding='utf-8')
        rejected = run('ingest', '--claims-tsv', '-s', 'verbatlas', overflow, expected=1)
        assert 'semantic_omissions=2' in rejected, rejected
        print('PASS distinct VerbAtlas argument categories, all role/filler pairs, absent prototypes and overflow rejection')

        annotations = Path(__file__).resolve().parent / 'fixtures/sghatecheck'
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'sghatecheck', annotations)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            # Equal judgments from the individual and both supplied aggregates
            # remain three distinct qualified observations, beside the gold.
            assert sorted(claims) == sorted([('A sample test case.', 'hateful')] +
                                            [('A sample test case.', 'non-hateful')] * 3), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        print('PASS individual and aggregate SGHateCheck judgments retain distinct source qualifiers and group metadata')

        multiword = Path(__file__).resolve().parent / 'fixtures/propbank-multiword'
        multiword_readings = {}
        slot = '[[[consider.01, take_into_account], A B], A]'
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'propbank', multiword)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            for claim in [('consider.01', 'm'), ('m', 'identifier vocabulary', 'propbank-pos'),
                          ('[consider.01, 0]', 'argument alias', 'considerer'),
                          ('[consider.01, 0]', 'NOUN'), (slot, 'part of speech', 'VB'),
                          (slot, 'argument number', '0'), (slot, 'lexical content', 'take'),
                          ('[consider.01, take_into_account]', 'source representation', 'Source expression.'),
                          ('[consider.01, take_into_account]', 'target representation', 'Target expression.')]:
                assert claim in claims, (workers, claim, claims)
            assert len(claims) == 24 and 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
            multiword_readings[workers] = sorted(claims)
        assert multiword_readings[1] == multiword_readings[4], 'Worker count changed multiword slot identities'
        print('PASS PropBank source categories, scoped argument aliases, multiword slots and representations')

        bias_frames = Path(__file__).resolve().parent / 'fixtures/social-bias-frames'
        expected_bias_frames = [('Sample text.', relation, value) for relation, value in [
            ('demographic group targeted', 'group, one'), ('demographic group targeted', 'group two'),
            ('high-level category of the demographic group(s) targeted', 'category'),
            ('implied statement', 'A quoted "word".'), ('implied statement', 'Ω text'),
            ('group vs. individual target', '0.5'), ('was the intent behind the statement to offend', '1'),
            ('is the post a sexual or lewd reference', '0'), ('could the post be offensive to anyone', '0.5'),
            ('source of the post', 'fixture')]]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'social-bias-frames', bias_frames)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected_bias_frames), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        print('PASS JSON-in-CSV lists decode once, retaining commas, quotes, Unicode and every array member')

        chemistry = Path(__file__).resolve().parent / 'fixtures/social-chemistry'
        expected_selections = [
            ('First rule.', 'Who is the RoT most likely targeting', 'friend'),
            ('First rule.', 'who is most likely to do the action or its opposite', 'narrator'),
            ('First rule.', 'Who is the RoT most likely targeting', 'friend'),
            ('First rule.', 'who is most likely to do the action or its opposite', 'narrator'),
            ('Second rule.', 'Who is the RoT most likely targeting', 'colleague'),
            ('Second rule.', 'who is most likely to do the action or its opposite', 'no one listed'),
            ('Third rule.', 'Who is the RoT most likely targeting', 'no one listed'),
        ]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'social-chemistry-101', chemistry)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            selections = [claim for claim in claims if len(claim) == 3 and claim[1] in {x[1] for x in expected_selections}]
            assert sorted(selections) == sorted(expected_selections), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        invalid_selections = root / 'social-chem-101.v1.0.tsv'
        invalid_selections.write_text('rot\tcharacters\trot-char-targeting\n' +
            ''.join(f'Invalid {i}\tnarrator|friend\t{value}\n' for i, value in
                    enumerate(('char-2', 'char--1', 'char-4294967296'))), encoding='utf-8')
        rejected = run('ingest', '--claims-tsv', '-s', 'social-chemistry-101', invalid_selections, expected=1)
        assert 'semantic_omissions=3' in rejected, rejected
        print('PASS row-local character pointers, explicit no-one answers, absent answers and invalid-index rejection')

        readings = {}
        for workers in (1, 4):
            text = run('ingest', '--claims', '-j', workers, 'semantic-alignment')
            for claim in ('[duck, NOUN]', '[duck, VERB]', '[duck, laplace-unlisted.987654321]',
                          '[duck.03, [pbroleset, cross-resource mapping, vnclass], novel-987654321.2]',
                          '[[wordnet-pos, n], denotes, NOUN]', '[[english-pos, noun], denotes, NOUN]'):
                assert claim in text, (claim, text)
            readings[workers] = sorted(line for line in text.splitlines() if line.startswith('['))
        assert readings[1] == readings[4], 'Worker count changed semantic evidence'
        text = run('ingest', '--claims', 'propbank')
        for sense, description in [('duck.01', 'avoider'), ('duck.02', 'bird')]:
            assert f'[[{sense}, 0], mneumonic, {description}]' in text, text
            assert f'[PersonX ducks., [{sense}, 0], PersonX]' in text, text
            assert f'[[{sense}, ARG0], refers to, [{sense}, 0]]' in text, text
        assert '[duck.01, [pbroleset, cross-resource mapping, fnframe], leave-51.2]' in text, text
        assert '[duck.01, [pbroleset, cross-resource mapping, vnclass], leave-51.2]' not in text, text
        assert '[PersonX ducks., [duck.02, 1], PersonY]' in text, text
        assert '[[duck.02, 1], mneumonic,' not in text, 'A reference invented a role definition'
        # WN-LMF's proposed-ILI marker is absent only in that field: the
        # ordinary English word "in" remains a lexical entry and a sense.
        wordnet = root / 'english-wordnet-proposed.xml'
        wordnet.write_text('''<LexicalResource><Lexicon id="test" label="Test" language="eng">
<LexicalEntry id="entry"><Lemma writtenForm="in" partOfSpeech="n"/><Sense id="sense" synset="wnlmf-new"/></LexicalEntry>
<Synset id="wnlmf-new" ili="in" partOfSpeech="n"><Definition>Unnumbered source concept.</Definition></Synset>
</Lexicon></LexicalResource>''', encoding='utf-8')
        proposed = run('ingest', '--claims', '-s', 'open-english-wordnet', wordnet)
        assert '[in, eng, wnlmf-new]' in proposed and '[wnlmf-new, in]' not in proposed, proposed
        # Acquisition can retain the download beside its extracted directory.
        # Read one release representation, with archive-only hosts supported.
        release = root / 'data/OpenEnglishWordNet-2099-test'
        extracted = release / 'english-wordnet-2099-test.xml'
        extracted.mkdir(parents=True)
        plain = extracted / extracted.name
        plain.write_bytes(wordnet.read_bytes())
        with gzip.open(release / (extracted.name + '.gz'), 'wb') as archive:
            archive.write(wordnet.read_bytes())
        representations = []
        for extracted_present in (True, False):
            if not extracted_present:
                plain.unlink()
                extracted.rmdir()
            text = run('ingest', '--claims-tsv', 'open-english-wordnet')
            assert 'decomposition: 1 files,' in text and 'mismatched 0' in text, text
            representations.append(sorted(line.split('\t')[3:] for line in text.splitlines() if line.startswith('claim\t')))
        assert representations[0] == representations[1], 'Extracted and compressed copies changed the source claims'
        print('PASS one Open English WordNet representation, with extracted preference and compressed fallback')
        constructors = recipes / 'key-constructors'
        constructors.mkdir()
        (constructors / 'source').write_text('name key-constructors\nwitness Declared key constructors\n'
            'class AcademicCurated\nroot $LAPLACE_DATA/constructors\n', encoding='utf-8')
        (constructors / 'constructor.recipe').write_text('name key-constructors\nmatch *.xml\nformat xml\n'
            'thing Entry id\nkey Entry id\ntype Entry.id language node\nthing Pointer name\ncontent name\n'
            'refer target Entry\nrelation "resolves to" target\nattest Pointer target\n', encoding='utf-8')
        absent = root / 'absent.xml'
        present = root / 'present.xml'
        absent.write_text('<Root><Pointer name="reference" target="en"/></Root>', encoding='utf-8')
        present.write_text('<Root><Entry id="en"/><Pointer name="reference" target="en"/></Root>', encoding='utf-8')
        def resolutions(path):
            return [line.split('\t')[3:] for line in run('ingest', '--claims-tsv', '-s', 'key-constructors', path).splitlines()
                    if line.startswith('claim\t') and '\tresolves to\t' in line]
        absent_resolution = resolutions(absent)
        assert absent_resolution == resolutions(present), 'Reference and declaration used different identifier interpretations'
        assert absent_resolution[0][-1] in ('eng', 'language:eng'), absent_resolution
        backlog = recipes / 'forward-backlog'
        backlog.mkdir()
        (backlog / 'source').write_text('name forward-backlog\nwitness Deferred canonical references\n'
            'class AcademicCurated\nroot $LAPLACE_DATA/backlog\n', encoding='utf-8')
        (backlog / 'reading.recipe').write_text('name forward-backlog\nmatch *.xml\nformat xml\n'
            'key Entry id\nthing Entry resolved target required\nthing Entry text\nthing Use text\n'
            'content text\nrefer target Entry\nrelation "points to" target\nattest Use target\n', encoding='utf-8')
        deferred = root / 'backlog.xml'
        expected = [(f'use{i:04}', 'points to', 'canonical') for i in range(1100)]
        deferred.write_text('<Root>' + ''.join(f'<Use text="use{i:04}" target="shared"/>' for i in range(1100)) +
            '<Entry id="shared" target="terminal"/><Entry id="shared" text="later duplicate"/>'
            '<Entry id="terminal" text="canonical"/></Root>', encoding='utf-8')
        for workers, window in ((1, 97), (4, 97), (1, 65536), (4, 65536)):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'forward-backlog', deferred,
                       extra={'LAPLACE_RECORD_WINDOW_BYTES': str(window)})
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected), (workers, window, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        deferred.write_text('<Root><Use text="unresolved" target="absent"/></Root>', encoding='utf-8')
        rejected = run('ingest', '--claims-tsv', '-s', 'forward-backlog', deferred, expected=1)
        assert 'semantic_omissions=1' in rejected, rejected
        print('PASS long forward-key backlog, chained identities, first-declaration ownership and missing-key rejection across windows and workers')
        manifest = Path(os.environ.get('LAPLACE_MANIFEST', Path(__file__).resolve().parents[2] / 'Laplace-Native/manifest'))
        with (manifest / 'banks.tsv').open(encoding='utf-8') as file:
            closed = {row['list'] for row in csv.DictReader((line for line in file if not line.startswith('#')), delimiter='\t')}
        with (manifest / 'slots/lists.tsv').open(encoding='utf-8') as file:
            open_lists = [row['list'] for row in csv.DictReader(file, delimiter='\t') if row['list'] not in closed]
        all_open = recipes / 'open-identifiers'
        all_open.mkdir()
        (all_open / 'source').write_text('name open-identifiers\nwitness Open vocabulary references\n'
                                          'class AcademicCurated\nroot $LAPLACE_DATA/open-identifiers\n', encoding='utf-8')
        reference_lists = [*open_lists, 'source-defined-vocabulary']
        fields = ['ref_' + name for name in reference_lists]
        (all_open / 'references.recipe').write_text('name open-references\nmatch references.tsv\n'
            'tier row by \\n\nnames word ' + ' '.join(fields) + '\ntier field by \\t\nthing row word\ncontent word\n' +
            ''.join(f'type {field} {name}' + (' node' if name == 'source-defined-vocabulary' else '') +
                    f'\nvalue {name} {field}\nattest row {field}\n'
                    for name, field in zip(reference_lists, fields)), encoding='utf-8')
        references = root / 'data/open-identifiers'
        references.mkdir()
        (references / 'references.tsv').write_text('references\t' + '\t'.join('unlisted-' + name + '.987654321'
                                                                             for name in reference_lists) + '\n', encoding='utf-8')
        (all_open / 'redundant.recipe').write_text('name redundant-declarations\nmatch redundant.tsv\n'
            'tier row by \\n\nnames identifier\ntier field by \\t\nthing row identifier\n'
            'type identifier pbroleset node\ntypes pbroleset "PropBank roleset" row\n', encoding='utf-8')
        (references / 'redundant.tsv').write_text('single-unlisted.01\n', encoding='utf-8')
        text = run('ingest', '--claims', 'open-identifiers')
        for name in reference_lists:
            assert f'[unlisted-{name}.987654321, identifier vocabulary, {name}]' in text, (name, text)
        print(f'PASS all {len(open_lists)} declared open identifier vocabularies and a source-defined vocabulary retain uncached references and their families')
        print('PASS shared categories, original notation, open IDs, crosswalks, scoped argument definitions and usages')

        # An unfamiliar closed category is an omission; a source pointer which
        # is simply absent from the cache above is not. Inspection reports both
        # through the real CLI, without an alternative permissive load mode.
        (data / 'canonical.tsv').write_text('bad\tNOT_A_POS\tnew.03\tnew.03\tnew-99\n', encoding='utf-8')
        rejected = run('ingest', '--claims', 'semantic-alignment', expected=1)
        assert 'semantic_omissions=1' in rejected and 'unresolved obligations: 1 ' in rejected, rejected
        (data / 'canonical.tsv').write_text('duck\tNOUN\tlaplace-unlisted.987654321\tduck.03\tnovel-987654321.2\n', encoding='utf-8')
        print('PASS closed vocabulary failures are counted and return failure')
        if not args.database:
            return

        psql = os.environ.get('PSQL', 'psql')
        host, port, user = (os.environ.get(name, default) for name, default in
                            [('PGHOST', '/tmp'), ('PGPORT', '5432'), ('PGUSER', 'laplace')])
        database = 'laplace_semantics_' + uuid.uuid4().hex[:12]
        created = False

        def sql(query, db=database):
            result = subprocess.run([psql, '-XwAtq', '-h', host, '-p', port, '-U', user, '-d', db,
                                     '-v', 'ON_ERROR_STOP=1', '-c', query], capture_output=True, text=True,
                                    encoding='utf-8', timeout=180)
            assert result.returncode == 0, result.stderr
            return result.stdout.strip()

        connection = ' '.join(f"{name}='{value}'" for name, value in
                              [('host', host), ('port', port), ('user', user), ('dbname', database)])
        try:
            sql(f'CREATE DATABASE "{database}"', 'postgres')
            created = True
            sql('CREATE EXTENSION laplace CASCADE')
            run('ingest', '-d', connection, '-j', 4, 'semantic-alignment')
            run('ingest', '-d', connection, '-j', 4, 'propbank')
            before = sql('SELECT count(*) FROM public.witness')
            assert before == '2', before
            run('ingest', '-d', connection, 'open-identifiers')
            # Two declarations describe one reference in one source record.
            # Replaying actual containment must still recover one observation.
            games = sql("""SELECT sum(games) FROM public.laplace_contained_series(
                (SELECT array_agg(id) FROM public.witness))
                WHERE claim=public.laplace_compose(ARRAY[public.laplace_text_id('single-unlisted.01'),
                    public.laplace_text_id('identifier vocabulary'),public.laplace_text_id('pbroleset')])""")
            assert games == '1', ('Duplicate declarations manufactured observations', games)
            run('ingest', '-d', connection, '-j', 4, '-s', 'social-chemistry-101', chemistry)
            repeated = sql("""SELECT sum(games)::text || ':' || min(position)::text
                FROM public.laplace_contained_series(ARRAY[public.laplace_text_id('Social-Chem-101 Dataset')])
                WHERE claim=public.laplace_compose(ARRAY[public.laplace_text_id('First rule.'),
                    public.laplace_text_id('Who is the RoT most likely targeting'),public.laplace_text_id('friend')])""")
            assert repeated == '2:1', ('Qualified observations or their source positions changed', repeated)
            # Use the product consumer: these are committed, ranked claims read
            # from PostgreSQL, not the preview or a test's reconstruction.
            duck = run('hop', '-d', connection, 'duck')
            assert 'NOUN' in duck and 'VERB' in duck and 'laplace-unlisted.987654321' in duck, duck
            mapping = run('hop', '-d', connection, 'duck.03')
            assert 'cross-resource mapping' in mapping and 'novel-987654321.2' in mapping, mapping
            reference = run('hop', '-d', connection, 'laplace-unlisted.987654321')
            assert 'identifier vocabulary' in reference and 'pbroleset' in reference, reference
            usage = run('hop', '-d', connection, 'PersonX ducks.')
            assert 'duck.01' in usage and 'duck.02' in usage and 'PersonX' in usage, usage
            assert 'PersonY' in usage, usage
            # Vocabulary edits invalidate the OS/file precheck even when all
            # corpus bytes, timestamps and the .recipe itself are unchanged.
            vocabulary = recipes / 'vocabularies/wordnet-pos.tsv'
            vocabulary.write_text(vocabulary.read_text(encoding='utf-8').replace('n NOUN', 'n ADV'), encoding='utf-8')
            rereading = root / 'rereading.jsonl'
            run('ingest', '-d', connection, 'semantic-alignment',
                extra={'LAPLACE_INGEST_PROGRESS_PATH': str(rereading)})
            events = [json.loads(line) for line in rereading.read_text(encoding='utf-8').splitlines()]
            assert any(e['event'] == 'file-composed' and e['file'].endswith('letters.tsv') for e in events), events
            assert 'ADV' in run('hop', '-d', connection, 'duck')
            before = sql('SELECT count(*) FROM public.witness')
            progress = root / 'failed.jsonl'
            (data / 'canonical.tsv').write_text('bad\tNOT_A_POS\tnew.03\tnew.03\tnew-99\n', encoding='utf-8')
            rejected = run('ingest', '-d', connection, 'semantic-alignment', expected=1,
                           extra={'LAPLACE_INGEST_PROGRESS_PATH': str(progress)})
            assert 'semantic_omissions=1' in rejected, rejected
            events = [json.loads(line) for line in progress.read_text(encoding='utf-8').splitlines()]
            assert events[-1]['event'] == 'failed' and not any(e['event'] == 'complete' for e in events), events
            assert sql('SELECT count(*) FROM public.witness') == before
            print('PASS committed CLI readback and failed-source publication/progress boundary')
        finally:
            if created:
                sql(f'DROP DATABASE "{database}" WITH (FORCE)', 'postgres')


if __name__ == '__main__':
    main()
