#!/usr/bin/env python3
"""Source notation, open identifiers, scoped argument slots and real readback."""
import argparse
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
    args = parser.parse_args()
    engine = args.engine.resolve()
    installed_recipes = Path(__file__).resolve().parents[1] / 'recipes'
    with tempfile.TemporaryDirectory(prefix='semantic-interpretation-', dir=os.environ.get('TMPDIR')) as scratch:
        root = Path(scratch)
        recipes = root / 'recipes'
        shutil.copytree(installed_recipes, recipes)
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
<roles><role n="0" f="PAG" descr="avoider"/></roles>
<example name="first"><text>PersonX ducks.</text><propbank><arg type="ARG0">PersonX</arg></propbank></example></roleset>
<roleset id="duck.02" name="bird"><aliases><alias pos="n">duck</alias></aliases>
<roles><role n="0" f="PPT" descr="bird"/></roles>
<example name="second"><text>PersonX ducks.</text><propbank><arg type="ARG0">PersonX</arg></propbank></example></roleset>
</predicate></frameset>''', encoding='utf-8')
        env = dict(os.environ, LAPLACE_RECIPES=str(recipes), LAPLACE_DATA=str(root / 'data'),
                   LAPLACE_WORK=str(root / 'work'), LAPLACE_INGEST_ONE='1')

        def run(*arguments, expected=0, extra=None):
            result = subprocess.run([str(engine), *map(str, arguments)], env=dict(env, **(extra or {})),
                                    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
            output = result.stdout + result.stderr
            assert result.returncode == expected, (arguments, result.returncode, output)
            return output

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
            # Use the product consumer: these are committed, ranked claims read
            # from PostgreSQL, not the preview or a test's reconstruction.
            duck = run('hop', '-d', connection, 'duck')
            assert 'NOUN' in duck and 'VERB' in duck and 'laplace-unlisted.987654321' in duck, duck
            mapping = run('hop', '-d', connection, 'duck.03')
            assert 'cross-resource mapping' in mapping and 'novel-987654321.2' in mapping, mapping
            usage = run('hop', '-d', connection, 'PersonX ducks.')
            assert 'duck.01' in usage and 'duck.02' in usage and 'PersonX' in usage, usage
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
