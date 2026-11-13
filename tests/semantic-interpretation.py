#!/usr/bin/env python3
"""Source notation, open identifiers and scoped argument slots, read without a database.

Their committed readback runs against the deployed database in tests/deployed/semantic-interpretation.py."""
import argparse
import csv
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('engine', type=Path)
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
        for spelling, how, vocabulary, pos in [('letters', 'crosswalk', 'wordnet-pos', 'n'),
                                                ('names', 'vocabulary', 'english-pos', 'noun'),
                                                ('canonical', None, None, 'NOUN')]:
            (source / f'{spelling}.recipe').write_text(
                f'name align-{spelling}\nmatch {spelling}.tsv\n'
                'tier row by \\n\nnames word pos pointer from to\ntier field by \\t\n'
                'thing row word\ncontent word from to\n'
                'type pos upos' + (f' {how} {vocabulary}' if vocabulary else '') + '\n'
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

        dependencies=root/'omw-dependencies.xml'
        dependencies.write_text('<LexicalResource>' + ''.join(
            f'<Lexicon id="wordnet-{version}" label="Wordnet {version}" language="en" email="owner@example.test" '
            f'license="wordnet" version="1.0"><Requires ref="omw-en" version="{version}"/>'
            f'<LexicalEntry id="word-{version}"><Lemma writtenForm="example" partOfSpeech="n"/></LexicalEntry></Lexicon>'
            for version in ('2.0','3.0')) + '</LexicalResource>',encoding='utf8')
        for workers in (1,4):
            text=run('ingest','--claims-tsv','-j',workers,'-s','open-multilingual-wordnet',dependencies)
            claims=[tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims)==sorted([
                ('Wordnet 2.0','requires wordnet','[omw-en, 2.0]'),
                ('Wordnet 3.0','requires wordnet','[omw-en, 3.0]'),
                *[('example','NOUN'),('[wordnet-pos, n]','denotes','NOUN'),
                  ('language:eng','identifier vocabulary','language')]*2]),(workers,claims,text)
        print('PASS WN-LMF dependencies retain both the required resource and its version')

        tatoeba = root / 'data/tatoeba'
        tatoeba.mkdir()
        tatoeba_source = recipes / 'tatoeba/source'
        tatoeba_source.write_text('name tatoeba\nwitness Tatoeba\nclass UserCuratedResource\n'
            'root $LAPLACE_DATA/tatoeba\nprefer sentences_detailed.csv over sentences.csv\n'
            'prefer sentences.csv over sentences_CC0.csv\n', encoding='utf-8')
        plain_sentences = '1\teng\tPreferred cat.\n2\teng\tFallback dog.\n'
        rich_sentences = ''.join(line+'\t\\N\t2026-01-01\t2026-01-01\n'
            for line in plain_sentences.splitlines()) + '3\teng\tOnly detailed.\t\\N\t2026-01-01\t2026-01-01\n'
        (tatoeba / 'links.csv').write_text('1\t2\n', encoding='utf-8')
        for available in ('both', 'detailed', 'plain', 'cc0', 'all', 'detailed_cc0', 'plain_cc0', 'directory'):
            plain = tatoeba / 'sentences.csv'
            detailed = tatoeba / 'sentences_detailed.csv'
            cc0 = tatoeba / 'sentences_CC0.csv'
            if plain.exists(): plain.unlink()
            if cc0.exists(): cc0.unlink()
            if detailed.is_dir(): detailed.rmdir()
            elif detailed.exists(): detailed.unlink()
            if available not in ('detailed', 'detailed_cc0', 'cc0'): plain.write_text(plain_sentences, encoding='utf-8')
            if available in ('both', 'detailed', 'all', 'detailed_cc0'): detailed.write_text(rich_sentences, encoding='utf-8')
            elif available == 'directory': detailed.mkdir()
            if available in ('cc0', 'all', 'detailed_cc0', 'plain_cc0'): cc0.write_text(''.join(line+'\t2026-01-01\n' for line in plain_sentences.splitlines()), encoding='utf-8')
            readings = []
            for workers in (1, 4):
                text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'tatoeba', tatoeba)
                claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
                languages = [claim for claim in claims if len(claim)==3 and claim[1]=='ISO 639-3 language code']
                expected_languages = [(word, 'ISO 639-3 language code', 'eng') for word in
                    (['Preferred cat.', 'Fallback dog.', 'Only detailed.'] if available in ('both','detailed','all','detailed_cc0')
                     else ['Preferred cat.', 'Fallback dog.'])]
                assert sorted(languages)==sorted(expected_languages), (available, claims, text)
                assert claims.count(('Fallback dog.', 'is the translation of', 'Preferred cat.'))==1, claims
                assert 'unresolved obligations: 0 ' in text, text
                readings.append(claims)
            assert readings[0]==readings[1], (available, readings)
        preferred_source = tatoeba_source.read_text(encoding='utf-8')
        tatoeba_source.write_text(preferred_source+'prefer sentences.csv over sentences_detailed.csv\n', encoding='utf-8')
        rejected = run('ingest', '--catalog', expected=2)
        assert 'cyclic source file preference' in rejected, rejected
        tatoeba_source.write_text(preferred_source, encoding='utf-8')
        print('PASS preferred sentence export avoids duplicate readings; links resolve with either provider and directories do not hide the fallback')
        (tatoeba / 'links.csv').write_text('1\t2\n1\t999\n999\t2\n999\t998\n', encoding='utf-8')
        for workers in (1,4):
            progress = root / f'tatoeba-domain-{workers}.jsonl'
            text = run('ingest','--claims-tsv','-j',workers,'-s','tatoeba',tatoeba,
                extra={'LAPLACE_INGEST_PROGRESS_PATH':str(progress),'LAPLACE_RECORD_WINDOW_BYTES':'1'})
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert claims.count(('Fallback dog.','is the translation of','Preferred cat.'))==1, claims
            assert 'declared reference-domain exclusions: 3 records' in text, text
            assert 'unresolved obligations: 0 ' in text, text
            frames = [json.loads(line) for line in progress.read_text(encoding='utf-8').splitlines()]
            links_done = [frame for frame in frames if frame['event']=='file-composed' and frame['file'].endswith('links.csv')]
            assert len(links_done)==1 and links_done[0]['records']==4, ('Framed rows read, including exclusions, were not reported',frames)
        link_recipe = recipes / 'tatoeba/links.recipe'
        admitted_recipe = link_recipe.read_text(encoding='utf-8')
        link_recipe.write_text('\n'.join(line for line in admitted_recipe.splitlines()
            if not line.startswith('admit references '))+'\n',encoding='utf-8')
        strict = run('ingest','--claims-tsv','-s','tatoeba',tatoeba,expected=1)
        assert 'unresolved identifier references' in strict, strict
        link_recipe.write_text(admitted_recipe,encoding='utf-8')
        (tatoeba / 'links.csv').write_text('1\t2\n',encoding='utf-8')
        print('PASS explicit exported reference domains count exclusions and preserve strict references elsewhere')

        dictionary = root / 'data/Wiktionary'
        dictionary.mkdir()
        dictionary_file = dictionary / 'kaikki.org-dictionary-fixture.jsonl'
        dictionary_entries = [
            {'lang_code':'en','word':'animal','pos':'noun',
             'hyponyms':[{'word':'domestic','tags':['domestic'],'raw_tags':['regional label'],
                          'qualifier':'chiefly rural'}], 'instances':[{'word':'Spot'}]},
            {'lang_code':'en','word':'move','pos':'verb','troponyms':[{'word':'run'}]}]
        dictionary_text = ''.join(json.dumps(entry)+'\n' for entry in dictionary_entries)
        dictionary_file.write_text(dictionary_text,encoding='utf-8')
        readings=[]
        for workers in (1,4):
            text=run('ingest','--claims-tsv','-j',workers,'-s','wiktionary-kaikki',dictionary)
            claims=[tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            for expected in [('[en, animal, noun]','more specific term','domestic'),
                             ('[en, animal, noun]','has instance','Spot'),
                             ('[en, move, verb]','particular manner of this action','run'),
                             ('domestic','qualifier of this form or sense','domestic'),
                             ('domestic','unparsed qualifier','regional label'),
                             ('domestic','unparsed qualifier','chiefly rural')]:
                assert claims.count(expected)==1,(expected,claims,text)
            assert 'unresolved obligations: 0 ' in text,text
            readings.append(claims)
        assert readings[0]==readings[1],readings
        dictionary_entries[0]['head_nr']=1
        dictionary_file.write_text(''.join(json.dumps(entry)+'\n' for entry in dictionary_entries),encoding='utf-8')
        rejected=run('ingest','--claims-tsv','-s','wiktionary-kaikki',dictionary,expected=1)
        assert 'attest head_nr' in rejected,rejected
        dictionary_file.write_text(dictionary_text,encoding='utf-8')
        print('PASS dictionary linkage direction and original qualifiers, including category homographs; unresolved head pointers remain explicit')


        sentence = root / 'spacing.conllu'
        conllu = ('# text = {text}\n# text_en = Translated.\n'
            '1\tsee\tsee\tVERB\t_\t_\t0\troot\t_\tCxnElt=1:Agent\n'
            '2\ta\ta\tNOUN\t_\t_\t1\tobj\t_\tSpaceAfter=No|CxnElt=named:Patient\n'
            '3\t.\t.\tPUNCT\t_\t_\t1\tpunct\t_\t_\n\n')
        for spacing in ('see a.', 'see  a.', 'see\u00a0a.'):
            sentence.write_text(conllu.format(text=spacing), encoding='utf-8')
            readings = []
            for workers in (1, 4):
                text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'universal-dependencies', sentence)
                claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
                assert (spacing, 'the fluent translation of the sentence to English', 'Translated.') in claims, claims
                assert ('see', '[see, Agent]') in claims and ('a', '[named, Patient]') in claims, claims
                assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
                readings.append(claims)
            assert readings[0] == readings[1], readings
        sentence.write_text(conllu.format(text='see b.'), encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'universal-dependencies', sentence, expected=2)
        assert sentence.as_posix() in rejected and 'at byte' in rejected and 'declared text differs' in rejected, rejected
        for annotation in ('__', 'GTtags=', '_|literal-marker'):
            sentence.write_text(conllu.format(text='see a.').replace('CxnElt=1:Agent', annotation), encoding='utf-8')
            rejected = run('ingest', '--no-load', '-s', 'universal-dependencies', sentence, expected=1)
            assert 'semantic_omissions=' in rejected and 'does not parse completely' not in rejected, rejected
        sentence.write_text(conllu.format(text='see a.').replace('VERB\t_\t_', 'VERB\t_\tBareFlag'), encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'universal-dependencies', sentence, expected=2)
        assert 'injected field FEATS' in rejected, rejected
        print('PASS original sentence spacing, numeric and literal construction anchors, MISC markers and strict FEATS')

        homograph = root / 'category-homograph.conllu'
        homograph.write_text('# text = go flat\n'
            '1\tgo\tgo\tVERB\t_\t_\t0\troot\t_\t_\n'
            '2\tflat\tflat\tX\t_\t_\t1\tflat:name\t_\t_\n\n', encoding='utf-8')
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'universal-dependencies', homograph)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert ('flat', 'flat:name', 'go') in claims, claims
            assert ('flat', 'flat') not in claims and 'unresolved obligations: 0 ' in text, text
        self_source = recipes / 'self-values'
        self_source.mkdir()
        (self_source / 'source').write_text('name self-values\nwitness Self values\n'
            'class AcademicCurated\nroot $LAPLACE_DATA/self-values\n', encoding='utf-8')
        self_data = root / 'data/self-values'
        self_data.mkdir()
        for name, category in [('untyped', '-'), ('unknown', 'deprel')]:
            (self_source / f'{name}.recipe').write_text(f'name self-{name}\nmatch {name}.tsv\n'
                'tier row by \\n\nnames word category\ntier field by \\t\n'
                f'thing row word\ncontent word category\nvalue {category} category\nattest row category\n', encoding='utf-8')
            path = self_data / f'{name}.tsv'
            path.write_text('laplace-unlisted-self\tlaplace-unlisted-self\n', encoding='utf-8')
            text = run('ingest', '--no-load', '-s', 'self-values', path, expected=1)
            assert 'value equal to its thing' in text, text
        print('PASS known category homographs retain edges without repeated-run claims; unknown self-values remain blocked')

        annotations = root / 'treebank-annotations.conllu'
        annotation_text = ('# text = go home\n'
            '1\tgo\tgo\tVERB\t_\t_\t0\troot\t_\tFunctor=0:PRED|LUWPOS=verb|LUWBILabel=B|BunsetuPositionType=SEM_HEAD\n'
            '2\thome\thome\tNOUN\t_\t_\t1\tobl\t_\tFunctor=1:DIR3,2:LOC|CxnElt=1:Agent,named:Patient\n\n')
        annotations.write_text(annotation_text, encoding='utf-8')
        expected_annotations = [('go', 'The tectogrammatical functor and its parent', 'PRED'),
            ('home', 'The tectogrammatical functor and its parent', '[goDIR3, homeLOC]'),
            ('go', 'Long Unit Word part-of-speech label', 'verb'),
            ('go', 'Long Unit Word boundary label', 'B'),
            ('go', 'Bunsetsu position type', 'SEM_HEAD')]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'universal-dependencies', annotations)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            for claim in expected_annotations:
                assert claim in claims, (claim, claims)
            assert ('home', '[goAgent, namedPatient]') in claims, claims
            assert 'unresolved obligations: 0 ' in text, text
        annotations.write_text(annotation_text.replace('1:DIR3', '99:DIR3'), encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'universal-dependencies', annotations, expected=1)
        assert 'unresolved identifier references' in rejected, rejected
        print('PASS documented treebank annotations retain independent layers, root functors and resolved t-tree parents; dangling parents fail')

        pages = root / 'ud-pages'
        pages.mkdir()
        for feature,definition,value in [('Poss','possessive','it is possessive'),('Abbr','abbreviation','it is abbreviated')]:
            (pages / (feature+'.md')).write_text(
                f"---\ntitle: '{feature}'\nshortdef: '{definition}'\n---\n"
                f'### <a name="Yes">`Yes`</a>: {value}\n', encoding='utf-8')
        expected_pages=[('Poss','short definition','possessive'),('[Poss, Yes]','called','it is possessive'),
                        ('Abbr','short definition','abbreviation'),('[Abbr, Yes]','called','it is abbreviated')]
        for workers in (1,4):
            text=run('ingest','--claims-tsv','-j',workers,'-s','universal-dependencies-documentation',pages)
            claims=[tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims)==sorted(expected_pages),(workers,claims,text)
        missing=pages/'Red.md'
        missing.write_text('### <a name="Mo">`Mo`</a>: Monosylabic\n',encoding='utf-8')
        text=run('ingest','--claims-tsv','-s','universal-dependencies-documentation',missing)
        assert [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]==[
            ('[Red, Mo]','called','Monosylabic')],text
        print('PASS documented page relations retain feature-local values and the declared headerless filename subject')

        frame=root/'framenet-pattern.xml'
        meanings={'Target':'Target','WSL':'word status','Other':'special syntactic phenomena','Sent':'sentence annotation',
                  **dict.fromkeys(['Noun','Verb','Adj','Adv','Prep','Scon','Art'],'part-of-speech-specific annotation'),
                  **dict.fromkeys(['CEE','CE','CstrPT','GovX'],'construction annotation layer'),
                  'BNC':'part of speech','PENN':'part of speech','FE':'frame element','GF':'grammatical function',
                  'PT':'phrase type','NER':'Named Entity Recognition'}
        layers=''.join(f'<layer name="{name}" rank="1"><label start="{5 if name=="Target" else 0}" '
                       f'end="{7 if name=="Target" else 3}" name="{name if name=="Target" else "Marker"}"/></layer>'
                       for name in meanings)
        frame.write_text('<lexUnit frame="Practice" frameID="962" name="run-through.n" ID="99999999" POS="N">'
            '<lexeme name="run-through" POS="N"/><valences><FEGroupRealization>'
            '<pattern><valenceUnit FE="Agent" PT="NP" GF="Ext"/><valenceUnit FE="Action" PT="DNI" GF=""/></pattern>'
            '<pattern><valenceUnit FE="Agent" PT="NP" GF="Obj"/><valenceUnit FE="Action" PT="DNI" GF=""/></pattern>'
            '</FEGroupRealization></valences><subCorpus><sentence><text>They ran.</text>'
            '<annotationSet luID="99999999">'+layers+'</annotationSet></sentence></subCorpus></lexUnit>', encoding='utf-8')
        lu='[Practice, run-through.n]'
        expected_frame=[
            (lu,'run-through','N','_','_'),(lu,'N'),(lu,'consist of','run-through'),
            ('['+lu+', valence pattern]','AgentAction','NPDNI','Ext_'),
            ('['+lu+', valence pattern]','AgentAction','NPDNI','Obj_'),
            (lu,'example sentence','They ran.'),(lu,'identifier vocabulary','fnlu'),
            ('[fnlu, 99999999]','identifies',lu),('99999999','[fnlu, cross-resource mapping, fnframe]','962'),
            ('Practice','identifier vocabulary','fnframe'),('99999999','identifier vocabulary','fnlu')]
        expected_frame += [('They ran.',meaning,
            '[[99999999, 5, 7], ['+name+', 1, ['+('5, 7, Target' if name=='Target' else '0, 3, Marker')+']]]')
            for name,meaning in meanings.items()]
        for workers in (1,4):
            text=run('ingest','--claims-tsv','-j',workers,'-s','framenet',frame)
            claims=[tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims)==sorted(expected_frame),(workers,claims,text)
            assert 'mismatched 0' in text,text
        print('PASS both FrameNet alignments, complete valence patterns, absent GF slots and all release annotation layers')

        brown=root/'ca01'
        brown.write_text('\n\nCount/nn //in minute/nn ./.\nUse/vb and/or/cc !/.\n'
                         '/l//nn AAb//nn /r//nn ///nn Fulton/np-tl\n\n',encoding='utf-8')
        readings=[]
        for workers in (1,4):
            text=run('ingest','--claims-tsv','-j',workers,'-s','brown',brown)
            claims=[tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            # The tags reach UD's parts of speech through the highway's brown tagset and its crosswalk edges; the
            # tag as the corpus writes it is the notation that denotes each.
            assert len(claims)==18,(claims,text)                  # a claim a token and a denotes a tag: no sentence-wide layer claim
            assert ('/','ADP') in claims and ('and/or','CCONJ') in claims,claims
            assert ('!','PUNCT') in claims and ('Fulton','NOUN') in claims,claims
            assert all((word,'NOUN') in claims for word in ('/l/', 'AAb/', '/r/', '//')),claims
            assert ('[brown, np-tl]','denotes','NOUN') in claims and ('[brown, cc]','denotes','CCONJ') in claims,claims
            assert 'mismatched 0' in text,text
            readings.append(sorted(claims))
        assert readings[0]==readings[1],readings
        unknown=root/'cb01'
        unknown.write_text('\n\nCount/nn minute/zz\n\n',encoding='utf-8')
        text=run('ingest','--claims-tsv','-j',1,'-s','brown',unknown,expected=1)
        assert '[tag zz]' in text and 'semantic_omissions=1' in text,text
        print('PASS Brown leading, embedded, trailing and repeated slashes retain the final word/tag separator; tags cross to UD by the highway')

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
            ('source of the post', 'fixture')]]      # the aggregated ratings (their mean, a fraction of annotators) are the record's, not claims
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'social-bias-frames', bias_frames)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected_bias_frames), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        print('PASS JSON-in-CSV lists decode once, retaining commas, quotes, Unicode and every array member')

        checklist = root / 'train-mhs-choices.tsv'
        checklist.write_text('text\tannotator_id\ttarget_race_asian\ttarget_religion_buddhist\t'
            'annotator_race_asian\tannotator_gender_prefer_not_to_say\tannotator_age\n'
            'Comment.\tworker-A\ttrue\tfalse\tfalse\ttrue\t24\n'
            'Comment.\tworker-B\tfalse\ttrue\ttrue\tfalse\t25\n', encoding='utf-8')
        expected_checklist = [
            ('Comment.', 'directed at or about individuals: Asian', answer) for answer in ('true', 'false')]
        expected_checklist += [
            ('Comment.', 'directed at or about individuals: Buddhist', answer) for answer in ('true', 'false')]
        for worker, age, race, refused in [('worker-A', '24', 'false', 'true'),
                                          ('worker-B', '25', 'true', 'false')]:
            expected_checklist += [(worker, 'self-reported age', age),
                (worker, 'self-reported race choice selected: Asian', race),
                (worker, 'self-reported gender choice selected: prefers not to state gender', refused)]
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'measuring-hate-speech', checklist)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            assert sorted(claims) == sorted(expected_checklist), (workers, claims, text)
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        unknown = root / 'train-mhs-unknown.tsv'
        unknown.write_text('text\tannotator_id\ttarget_undocumented\nComment.\tworker-A\ttrue\n', encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'measuring-hate-speech', unknown, expected=1)
        assert 'semantic_omissions=1' in rejected, rejected
        print('PASS target checklists retain false answers and annotator demographics; unknown statistics remain blocked')

        chess = Path(__file__).resolve().parent / 'chess/kinds.pgn'
        chess_readings = []
        for workers in (1, 4):
            for window in (97, 4097, 65536):
                text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'chess-com', chess,
                           extra={'LAPLACE_RECORD_WINDOW_BYTES': str(window)})
                claims = sorted(line for line in text.splitlines() if line.startswith('claim\t'))
                assert len(claims) == 37, (workers, window, text)      # the moves alone: a record is never the subject of a claim
                chess_readings.append(claims)
        assert all(reading == chess_readings[0] for reading in chess_readings), 'PGN read windows changed claims'
        incomplete = root / 'missing-result.pgn'
        incomplete.write_text('[Event "Incomplete"]\n[Result "1-0"]\n\n1. e4 e5\n', encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'chess-com', incomplete, expected=2,
                       extra={'LAPLACE_RECORD_WINDOW_BYTES': '97'})
        assert 'invalid PGN packaging' in rejected, rejected
        variant = root / 'drop-variant.pgn'
        variant.write_text('[Event "Drop syntax"]\n[Variant "Crazyhouse"]\n[Result "*"]\n\n'
                           '1. e4 e5 2. N@f3 B@g2 *\n', encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'chess-com', variant, expected=1,
                       extra={'LAPLACE_RECORD_WINDOW_BYTES': '29'})
        assert 'variant Crazyhouse is in no rules' in rejected and '1 kept as text' in rejected, rejected
        assert 'invalid PGN packaging' not in rejected, rejected
        print('PASS PGN grammar boundaries retain every game; unsupported drop variants parse and remain explicit')

        missing_move = root / 'twic-missing-move.pgn'
        missing_move.write_text('[Event "Missing move fixture"]\n[Result "1-0"]\n\n1. e4 -- 2. Nf3 1-0\n', encoding='utf-8')
        rejected = run('ingest', '--no-load', '-s', 'twic', missing_move, expected=1)
        assert 'incomplete_parts=1' in rejected and '-- is no legal move' in rejected, rejected
        twic_subset = root / 'data/twic-validated'
        twic_subset.mkdir()
        shutil.copy2(chess, twic_subset / 'twic-fixture.pgn')
        shutil.copy2(missing_move, twic_subset / 'quarantined-game-fixture-1.txt')
        (twic_subset / 'subset-receipt.json').write_text('{"excluded_games":1}\n', encoding='utf-8')
        (twic_subset / 'grammar-validated-exclusions.json').write_text('{"game_ordinal":1}\n', encoding='utf-8')
        subset_source = recipes / 'twic-validated/source'
        subset_source.write_text('\n'.join(
            'root ' + twic_subset.as_posix() if line.startswith('root ') else line
            for line in subset_source.read_text(encoding='utf-8').splitlines()) + '\n', encoding='utf-8')
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers, '-s', 'twic-validated', twic_subset)
            claims = [line for line in text.splitlines() if line.startswith('claim\t')]
            assert len(claims) == 37 and all('Missing move fixture' not in claim for claim in claims), text
            assert 'mismatched 0' in text and '1 kept as text' not in text, text
        print('PASS validated TWIC source preserves quarantined bytes without inventing legal transitions')

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

        validated_chemistry = root / 'data/Safety/SocialChemistry101/validated-subset'
        validated_chemistry.mkdir(parents=True)
        shutil.copyfile(chemistry / 'social-chem-101.v1.0.tsv',
                        validated_chemistry / 'social-chem-101.v1.0.tsv')
        shutil.copyfile(invalid_selections, validated_chemistry / 'quarantined-records.tsv')
        (validated_chemistry / 'subset-receipt.json').write_text(
            json.dumps({'quarantined_rows': 3, 'invalid_references': 3}), encoding='utf-8')
        (validated_chemistry / 'invalid-character-references.csv').write_text(
            'row,column,value\n2,rot-char-targeting,char-2\n', encoding='utf-8')
        for workers in (1, 4):
            text = run('ingest', '--claims-tsv', '-j', workers,
                       '-s', 'social-chemistry-101-validated', validated_chemistry)
            claims = [tuple(line.split('\t')[3:]) for line in text.splitlines() if line.startswith('claim\t')]
            selections = [claim for claim in claims if len(claim) == 3 and claim[1] in {x[1] for x in expected_selections}]
            assert sorted(selections) == sorted(expected_selections), (workers, claims, text)
            assert not any(claim[0].startswith('Invalid ') for claim in claims), claims
            assert 'unresolved obligations: 0 ' in text and 'mismatched 0' in text, text
        print('PASS named validated subset retains quarantined bytes as provenance without interpreting invalid judgments')

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


if __name__ == '__main__':
    main()
