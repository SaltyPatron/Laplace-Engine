#!/usr/bin/env python3
"""Rejected Native readings must fail again, then recover after real correction."""
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
    args = parser.parse_args()
    psql = os.environ.get('PSQL', 'psql')
    host, port, user = (os.environ.get(k, v) for k, v in
                        [('PGHOST', '/tmp'), ('PGPORT', '5432'), ('PGUSER', 'laplace')])
    database = 'failed_file_' + uuid.uuid4().hex[:12]

    def sql(query, db=database):
        result = subprocess.run([psql, '-XwAtq', '-h', host, '-p', port, '-U', user, '-d', db,
                                 '-v', 'ON_ERROR_STOP=1', '-c', query], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    with tempfile.TemporaryDirectory(prefix='failed-file-replay-') as scratch:
        root = Path(scratch)
        recipes = root / 'recipes'
        shutil.copytree(Path(__file__).resolve().parents[1] / 'recipes', recipes,
                        ignore=shutil.ignore_patterns('grammars'))
        cases = [
            ('rejected-pgn', 'twic-invalid.pgn', 'grammar pgn\nrecords played\n',
             '[Event "Missing move"]\n[Result "1-0"]\n\n1. e4 -- 2. Nf3 1-0\n',
             '[Event "Corrected moves"]\n[Result "1-0"]\n\n1. e4 e5 2. Nf3 1-0\n', 'incomplete_parts=1'),
            ('rejected-category', 'words.tsv',
             'tier row by \\n\nnames word pos\ntier field by \\t\nthing row word\n'
             'content word\ntype pos upos\nvalue upos pos\nattest row pos\n',
             'duck\tNOT_A_POS\n', 'duck\tNOUN\n', 'semantic_omissions=1'),
        ]
        env = dict(os.environ, LAPLACE_RECIPES=str(recipes), LAPLACE_DATA=str(root / 'data'),
                   LAPLACE_WORK=str(root / 'work'), LAPLACE_INGEST_ONE='1')
        conn = f"host='{host}' port='{port}' user='{user}' dbname='{database}'"

        def ingest(source, expected):
            result = subprocess.run([str(args.engine.resolve()), 'ingest', '-d', conn, '-j', '4', source],
                                     env=env, capture_output=True, text=True, encoding='utf-8', timeout=180)
            output = result.stdout + result.stderr
            assert result.returncode == expected, (source, expected, result.returncode, output)
            return output

        sql(f'CREATE DATABASE "{database}"', 'postgres')
        try:
            sql('CREATE EXTENSION laplace CASCADE')
            for ordinal, (name, filename, body, rejected, corrected, diagnostic) in enumerate(cases):
                definition = recipes / name
                definition.mkdir()
                (definition / 'source').write_text(
                    f'name {name}\nwitness {name}\nclass StructuredCorpus\nroot $LAPLACE_DATA/{name}\n', encoding='utf-8')
                (definition / 'content.recipe').write_text(f'name {name}-content\nmatch {filename}\n' + body, encoding='utf-8')
                data = root / 'data' / name
                data.mkdir(parents=True)
                path = data / filename
                path.write_text(rejected, encoding='utf-8')
                for attempt in range(2):
                    output = ingest(name, 1)
                    assert diagnostic in output, (name, attempt, output)
                    assert sql('SELECT count(*) FROM public.witness') == str(ordinal), 'Rejected replay published a source'
                path.write_text(corrected, encoding='utf-8')
                ingest(name, 0)
                assert sql('SELECT count(*) FROM public.witness') == str(ordinal + 1)
                query = "SELECT jsonb_agg(to_jsonb(s) ORDER BY claim,dependence_root) FROM public.laplace_contained_series(ARRAY(SELECT id FROM public.witness)) s"
                before = sql(query)
                assert before != 'null', 'Corrected reading published no testimony'
                replay = ingest(name, 0)
                assert '1 files recorded already' in replay and sql(query) == before, 'Accepted file lost exact cached replay'
            print(json.dumps({'rejected_replays': 'failed twice without witnesses',
                              'corrected_readings': 'published and replayed exactly', 'cases': len(cases)}))
        finally:
            sql(f'DROP DATABASE "{database}" WITH (FORCE)', 'postgres')


if __name__ == '__main__':
    main()
