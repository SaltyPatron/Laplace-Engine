"""Ingest, replay and read through the candidate CLI under a scoped PostgreSQL role."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('engine', type=Path, help='the candidate Engine binary built by this run')
engine = parser.parse_args().engine.resolve(strict=True)
pgbin = Path(os.environ['PG_CONFIG']).parent
identifier = 'staging_test_' + uuid.uuid4().hex[:12]
environment = dict(os.environ, PGHOST=os.environ.get('PGHOST', '/tmp'), PGUSER=os.environ.get('PGUSER', 'laplace'))


def sql(database, statement):
    result = subprocess.run([str(pgbin / 'psql'), '-XwAt', '-v', 'ON_ERROR_STOP=1', '-d', database, '-f', '-'],
                            input=statement, env=environment, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout.strip()


created = False
try:
    sql('postgres', f'CREATE DATABASE "{identifier}" TEMPLATE template0; CREATE ROLE "{identifier}";')
    created = True
    deployment = subprocess.run([str(engine), 'deploy', '-d', 'dbname=' + identifier],
                                env=environment, text=True, capture_output=True, timeout=180)
    print(deployment.stdout, end='')
    print(deployment.stderr, end='')
    deployment.check_returncode()
    sql(identifier, f'''SELECT laplace_ensure_stage();
CREATE INDEX staging_test_guard ON stage.attestation(claim);
GRANT USAGE,CREATE ON SCHEMA stage TO "{identifier}";
GRANT SELECT ON ALL TABLES IN SCHEMA stage TO "{identifier}";
GRANT USAGE ON SCHEMA public TO "{identifier}";
GRANT SELECT,INSERT,UPDATE,DELETE ON ALL TABLES IN SCHEMA public TO "{identifier}";
GRANT USAGE,SELECT ON ALL SEQUENCES IN SCHEMA public TO "{identifier}";''')
    scoped = f"dbname={identifier} options='-c role={identifier}'"
    assert sql(scoped, "SELECT current_user || '|' || CASE WHEN rolsuper THEN 'superuser' ELSE 'scoped' END FROM pg_roles WHERE rolname=current_user;") == identifier + '|scoped'
    guard = sql(identifier, "SELECT oid::text || '|' || relowner FROM pg_class WHERE oid='stage.staging_test_guard'::regclass;")

    with tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        recipes = scratch / 'recipes'
        shutil.copytree(ROOT / 'recipes', recipes)
        source = recipes / 'scoped-loader'
        source.mkdir()
        data = scratch / 'data'
        data.mkdir()
        (source / 'source').write_text(
            'name scoped-loader\nwitness Scoped Loader Integration\nclass UserCuratedResource\n'
            f'root {data}\nreads python\n', encoding='utf-8')
        (data / 'probe.py').write_text('def scoped_ingestion_probe():\n    return "scoped ingestion"\n', encoding='utf-8')
        env = dict(environment, LAPLACE_RECIPES=str(recipes), LAPLACE_WORK=str(scratch / 'work'),
                   OMP_NUM_THREADS='2', LAPLACE_DB_POOL_SIZE='2', LAPLACE_TIMES='1')

        def run(*arguments):
            result = subprocess.run([str(engine), *arguments], env=env, text=True,
                                    capture_output=True, timeout=180)
            print(result.stdout, end='')
            print(result.stderr, end='')
            result.check_returncode()
            return result.stdout + result.stderr

        counts_sql = "SELECT (SELECT count(*) FROM public.entity) || '|' || (SELECT count(*) FROM public.physicality) || '|' || (SELECT count(*) FROM public.witness);"
        run('ingest', '-d', scoped, '-j', '2', 'scoped-loader')
        counts = sql(identifier, counts_sql)
        assert all(int(count) > 0 for count in counts.split('|')), counts
        owner = 'dbname=' + identifier
        assert 'projection audit: 0 entity coordinates, 0 path Hilbert keys' in run('projection', '-d', owner)
        assert 'projection applied: 0 entity coordinates, 0 path Hilbert keys' in run('projection', '-d', owner, '--apply')
        assert sql(identifier, counts_sql) == counts, 'Projection maintenance changed structural/witness counts'
        run('ingest', '-d', scoped, '-j', '2', 'scoped-loader')
        assert sql(identifier, counts_sql) == counts, 'Replay changed the recorded structural/witness closure'
        assert sql(identifier, "SELECT oid::text || '|' || relowner FROM pg_class WHERE oid='stage.staging_test_guard'::regclass;") == guard
        assert sql(identifier, "SELECT count(*) FROM pg_tables WHERE schemaname='stage' AND tablename LIKE 'lp_%';") == '0', 'Loader left private staging tables behind'
        output = run('hop', '-d', scoped, 'scoped_ingestion_probe')
        assert 'observed: held by 1 path' in output, 'Scoped Native read did not recover the recorded code holder'
    print('Candidate Engine passed scoped non-superuser ingestion, replay deduplication, administrator-owned staging preservation, cleanup and connection-owned Native database readback.')
finally:
    if created:
        sql('postgres', f'DROP DATABASE IF EXISTS "{identifier}" WITH (FORCE); DROP ROLE IF EXISTS "{identifier}";')
