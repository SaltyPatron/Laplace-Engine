#!/usr/bin/env python3
"""Cached key providers share one backend and preserve cold-read source identity."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('engine', type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    psql = os.environ.get('PSQL', 'psql')
    host, port, user = (os.environ.get(key, default) for key, default in
                       [('PGHOST', '/tmp'), ('PGPORT', '5432'), ('PGUSER', 'laplace')])
    databases = []

    def sql(db, query):
        result = subprocess.run([psql, '-XwAtq', '-h', host, '-p', port, '-U', user,
                                 '-d', db, '-v', 'ON_ERROR_STOP=1', '-c', query],
                                capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    try:
        with tempfile.TemporaryDirectory(prefix='cached-key-reader-', dir=os.environ.get('LAPLACE_WORK')) as scratch:
            root = Path(scratch)
            recipes = root / 'recipes'
            shutil.copytree(Path(__file__).resolve().parents[1] / 'recipes', recipes,
                            ignore=shutil.ignore_patterns('grammars'))
            source = recipes / 'cached-reader'
            source.mkdir()
            (source / 'source').write_text(
                'name cached-reader\nwitness Cached key readers\nclass AcademicCurated\n'
                'root $LAPLACE_DATA/readers\n', encoding='utf-8')
            (source / 'provider.recipe').write_text(
                'name cached-reader-provider\nmatch provider-*.tsv\ntier row by \\n\n'
                'names key text\ntier field by \\t\nthing row text\nkey row key\ncontent text\n', encoding='utf-8')
            (source / 'consumer.recipe').write_text(
                'name cached-reader-consumer\nmatch links.tsv\ntier row by \\n\n'
                'names subject object\ntier field by \\t\nthing row subject\n'
                'refer subject cached-reader-provider\nrefer object cached-reader-provider\n'
                'relation "links to" object\nattest row object\n', encoding='utf-8')
            deltas = []
            for count in (4, 12):
                data = root / f'data-{count}' / 'readers'
                data.mkdir(parents=True)
                for i in range(count):
                    (data / f'provider-{i}.tsv').write_text(f'{i}\tRecorded key {i}.\n', encoding='utf-8')
                environment = dict(os.environ, LAPLACE_DATA=str(data.parent),
                                   LAPLACE_RECIPES=str(recipes), LAPLACE_WORK=str(root / f'work-{count}'))
                environment.pop('LAPLACE_INGEST_ONE', None)

                def run(db, name):
                    connection = ' '.join(f"{key}='{value}'" for key, value in
                                          [('host', host), ('port', port), ('user', user), ('dbname', db)])
                    result = subprocess.run([str(engine), 'ingest', '-d', connection, '-j', '2', 'cached-reader'],
                                            env=environment, capture_output=True, text=True, encoding='utf-8', timeout=180)
                    report = result.stdout + result.stderr
                    log = Path(environment['LAPLACE_WORK']) / 'logs/ingest/cached-reader.log'
                    report += log.read_text(encoding='utf-8') if log.exists() else ''
                    (root / f'{name}-{count}.log').write_text(report, encoding='utf-8')
                    assert result.returncode == 0, report
                    return report

                def database(suffix):
                    name = 'laplace_key_reader_' + uuid.uuid4().hex[:12] + '_' + suffix
                    sql('postgres', f'CREATE DATABASE "{name}"')
                    databases.append(name)
                    sql(name, 'CREATE EXTENSION laplace CASCADE')
                    return name

                warm = database('warm')
                run(warm, 'initial')
                (data / 'links.tsv').write_text(''.join(f'{i}\t{(i + 1) % count}\n' for i in range(count)), encoding='utf-8')
                before = int(sql(warm, 'SELECT sessions FROM pg_stat_database WHERE datname=current_database()'))
                report = run(warm, 'replay')
                after = int(sql(warm, 'SELECT sessions FROM pg_stat_database WHERE datname=current_database()'))
                assert f'{count} files recorded already' in report, report
                restored = len(re.findall('compiled reference bindings from recorded metadata:', report))
                assert restored == count, (restored, count, report)
                deltas.append(after - before)
                cold = database('cold')
                run(cold, 'cold')
                witness = sql(cold, 'SELECT id::text FROM public.witness')
                assert re.fullmatch('[a-f0-9]{32}', witness), witness
                query = ("SELECT w.id::text,e.tier,e.hilbert,encode(ST_AsEWKB(e.coord),'hex') "
                         "FROM public.witness w JOIN public.entity e ON e.id=w.id "
                         f"WHERE w.id='{witness}'::blake3")
                expected = sql(cold, query)
                actual = sql(warm, query)
                assert expected and actual == expected, ('Warm and cold source references differ', actual, expected)
                print(f'cached-key-reader: {count} recorded providers, {restored} real restores, {deltas[-1]} sessions', flush=True)
            assert deltas[0] == deltas[1], ('Cached providers increased the session count', deltas)
            print('cached-key-reader: constant connection count and identical warm/cold canonical source references')
    finally:
        for db in databases:
            sql('postgres', f'DROP DATABASE IF EXISTS "{db}"')


if __name__ == '__main__':
    main()
