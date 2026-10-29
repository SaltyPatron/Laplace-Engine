#!/usr/bin/env python3
"""Observe real committed windows and atomic, concurrent source publication."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('engine', type=Path)
    engine = parser.parse_args().engine.resolve()
    psql = os.environ.get('PSQL', 'psql')
    host, port, user = (os.environ.get(name, default) for name, default in
                        [('PGHOST', '/tmp'), ('PGPORT', '5432'), ('PGUSER', 'laplace')])
    db = 'laplace_windows_' + uuid.uuid4().hex[:12]
    arguments = [psql, '-XwAtq', '-h', host, '-p', port, '-U', user, '-v', 'ON_ERROR_STOP=1']
    processes = []
    created = False

    def stop_processes():
        for process in reversed(processes):
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)

    def sql(query, database=db):
        result = subprocess.run([*arguments, '-d', database, '-c', query], capture_output=True,
                                text=True, encoding='utf-8', timeout=180)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    try:
        sql('CREATE DATABASE "' + db + '"', 'postgres')
        created = True
        sql('CREATE EXTENSION laplace CASCADE')
        with contextlib.ExitStack() as resources:
            root = Path(resources.enter_context(tempfile.TemporaryDirectory(
                prefix='observation-windows-', dir=os.environ.get('LAPLACE_WORK'))))
            resources.callback(stop_processes)
            recipes = root / 'recipes'
            shutil.copytree(Path(__file__).resolve().parents[1] / 'recipes', recipes)
            definitions = recipes / 'window-publication'
            definitions.mkdir()
            (definitions / 'source').write_text(
                'name window-publication\nwitness Native window publication\nclass AcademicCurated\n'
                'root $LAPLACE_DATA/windows\n', encoding='utf-8', newline='\n')
            (definitions / 'reading.recipe').write_text(
                'name window-reading\nmatch *.tsv\ntier row by \\n\nnames thing value\n'
                'tier field by \\t\nthing row thing\ncontent thing value\n'
                'relation "has value" value\nattest row value\n', encoding='utf-8', newline='\n')
            data = root / 'data/windows'
            data.mkdir(parents=True)
            for i in range(8):
                (data / f'{i}.tsv').write_text('dog\twarm\ncat\tcold\ndog\twarm\n', encoding='utf-8', newline='\n')
            progress = root / 'progress.jsonl'
            env = dict(os.environ, LAPLACE_DATA=str(root / 'data'), LAPLACE_RECIPES=str(recipes),
                       LAPLACE_WORK=str(root / 'work'), LAPLACE_INGEST_ONE='1',
                       LAPLACE_RECORD_WINDOW_BYTES='1', LAPLACE_WORKING_SET_BYTES='1',
                       LAPLACE_INGEST_PROGRESS_PATH=str(progress))
            conn = ' '.join(f"{name}='{value.replace(chr(39), chr(92)+chr(39))}'" for name, value in
                            [('host', host), ('port', port), ('user', user), ('dbname', db)])
            command = [str(engine), 'ingest', '-r', str(recipes), '-d', conn, '-j', '2',
                       '-s', 'window-publication', str(data)]

            # A real second connection holds only this fixture's publication
            # barrier. Parsing and all observation windows must finish before
            # the witness insert waits here.
            gate = subprocess.Popen([*arguments, '-d', db], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, encoding='utf-8')
            processes.append(gate)
            gate.stdin.write("SELECT pg_advisory_lock(18579,1); SELECT 'held';\n")
            gate.stdin.flush()
            while gate.stdout.readline().strip() != 'held':
                if gate.poll() is not None:
                    raise RuntimeError('publication barrier connection exited')
            sql("""
              CREATE FUNCTION public.window_barrier() RETURNS trigger LANGUAGE plpgsql AS $$
              BEGIN PERFORM pg_advisory_xact_lock(18579,1); RETURN NEW; END $$;
              CREATE TRIGGER window_barrier BEFORE INSERT ON public.witness FOR EACH ROW
                WHEN (NEW.trust=0.85) EXECUTE FUNCTION public.window_barrier();
              ALTER FUNCTION public.laplace_contained_series(public.blake3[]) RENAME TO window_reference_series;
              CREATE FUNCTION public.laplace_contained_series(trunks public.blake3[],
                OUT trunk public.blake3,OUT claim public.blake3,OUT games double precision,
                OUT tokens double precision,OUT score double precision,OUT "position" integer,
                OUT dependence_root public.blake3,OUT dependence_trust double precision)
                RETURNS SETOF record LANGUAGE plpgsql AS $$
              BEGIN RAISE EXCEPTION 'new source was walked downward again'; END $$;
            """)
            with (root / 'initial.log').open('w', encoding='utf-8') as log:
                process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                processes.append(process)
                deadline = time.monotonic() + 120
                while sql("SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a ON a.pid=l.pid "
                              "WHERE a.datname=current_database() AND l.locktype='advisory' "
                              "AND l.classid=18579 AND l.objid=1 AND NOT l.granted") == '0':
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError((root / 'initial.log').read_text(encoding='utf-8'))
                    time.sleep(0.1)
                pending = sql("SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
                              "WHERE n.nspname='stage' AND c.relkind='r' AND c.relname LIKE 'lp_pending_%'")
                assert re.fullmatch(r'lp_pending_[0-9]+', pending), pending
                assert int(sql(f'SELECT count(*) FROM stage."{pending}"')) > 0, 'window observations were not committed'
                assert sql('SELECT count(*) FROM public.witness') == '0', 'witness escaped atomic publication'
                assert sql('SELECT count(*) FROM public.consensus') == '0', 'unregistered source published standings'
                assert sql('SELECT bool_and(pg_try_advisory_xact_lock(1279348803,h)) FROM generate_series(0,15) h') == 't', \
                    'streaming retained public consensus bucket locks'
                frames = [json.loads(line) for line in progress.read_text(encoding='utf-8').splitlines()]
                file_id = next(frame['file_id'] for frame in frames if frame.get('event') == 'file-composed' and 'file_id' in frame)
                assert re.fullmatch(r'[0-9a-f]{32}', file_id)
                # A concurrent independent witness must be included by final
                # Native publication, rather than overwritten by an old snapshot.
                sql(f"INSERT INTO public.witness(id,trust) VALUES ('{file_id}'::public.blake3,0.4)")
                gate.communicate("SELECT pg_advisory_unlock(18579,1);\n\\q\n", timeout=30)
                if process.wait(timeout=180):
                    raise RuntimeError((root / 'initial.log').read_text(encoding='utf-8'))
            sql('DROP FUNCTION public.laplace_contained_series(public.blake3[]); '
                'ALTER FUNCTION public.window_reference_series(public.blake3[]) RENAME TO laplace_contained_series')

            def exact_standings():
                sql('CREATE UNLOGGED TABLE stage.window_empty (LIKE stage.attestation INCLUDING DEFAULTS); '
                    'CREATE UNLOGGED TABLE stage.window_claims AS SELECT DISTINCT claim FROM '
                    'public.laplace_contained_series(ARRAY(SELECT id FROM public.witness))')
                assert int(sql('SELECT count(*) FROM stage.window_claims')) > 0, 'fixture emitted no testimony'
                assert sql("""SELECT NOT EXISTS (
                  (SELECT claim,rating,deviation,volatility,matches FROM public.consensus
                   EXCEPT ALL SELECT * FROM public.laplace_stage_snapshot('stage.window_empty','stage.window_claims'))
                  UNION ALL
                  (SELECT * FROM public.laplace_stage_snapshot('stage.window_empty','stage.window_claims')
                   EXCEPT ALL SELECT claim,rating,deviation,volatility,matches FROM public.consensus))""") == 't', \
                    'publication differs from complete registered Native evidence'
                sql('DROP TABLE stage.window_empty,stage.window_claims')

            exact_standings()
            (data / 'additional.tsv').write_text('dog\tcold\ncat\twarm\n', encoding='utf-8', newline='\n')
            result = subprocess.run(command, env=env, capture_output=True, text=True, encoding='utf-8', timeout=180)
            assert result.returncode == 0, result.stdout + result.stderr
            exact_standings()
            print(json.dumps({'committed_private_windows': True, 'consensus_locks_while_streaming': 0,
                              'new_source_downward_rewalk': False, 'concurrent_witness': 'included',
                              'publication_and_partial_replay': 'exact Native standings'}))
    finally:
        stop_processes()
        if created:
            sql('DROP DATABASE "' + db + '" WITH (FORCE)', 'postgres')


if __name__ == '__main__':
    main()
