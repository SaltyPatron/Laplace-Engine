"""Run the production staging initializer against scoped roles in a disposable PG database."""
import os
from pathlib import Path
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
native = Path(os.environ['LAPLACE_NATIVE'])
pgbin = Path(os.environ['PG_CONFIG']).parent
identifier = 'staging_test_' + uuid.uuid4().hex[:12]
environment = dict(os.environ, PGHOST=os.environ.get('PGHOST', '/tmp'), PGUSER=os.environ.get('PGUSER', 'laplace'))


def sql(database, statement):
    result = subprocess.run([str(pgbin / 'psql'), '-XwAt', '-v', 'ON_ERROR_STOP=1', '-d', database, '-f', '-'],
                            input=statement, env=environment, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout.strip()


try:
    sql('postgres', f'CREATE DATABASE "{identifier}" TEMPLATE template0; CREATE ROLE "{identifier}";')
    sql(identifier, f'''CREATE TABLE entity(id int) PARTITION BY RANGE(id);
CREATE TABLE entity_00 PARTITION OF entity FOR VALUES FROM(0) TO(100);
CREATE TABLE consensus(claim int) PARTITION BY RANGE(claim);
CREATE TABLE consensus_0 PARTITION OF consensus FOR VALUES FROM(0) TO(100);
CREATE SCHEMA stage;
CREATE UNLOGGED TABLE stage.consensus_0(LIKE consensus_0);
CREATE UNIQUE INDEX consensus_0_claim ON stage.consensus_0(claim);
GRANT USAGE,CREATE ON SCHEMA stage TO "{identifier}";
GRANT SELECT,TRUNCATE ON ALL TABLES IN SCHEMA stage TO "{identifier}";
GRANT USAGE ON SCHEMA public TO "{identifier}";
GRANT SELECT ON ALL TABLES IN SCHEMA public TO "{identifier}";''')
    with tempfile.TemporaryDirectory() as temporary:
        executable = Path(temporary) / 'staging-role'
        lib = pgbin.parent / 'lib'
        subprocess.run(['cc', '-O1', '-ffunction-sections', '-fdata-sections', '-fopenmp',
            '-I' + str(native / 'include'), '-I' + str(native / 'src'), '-I' + str(pgbin.parent / 'include'),
            str(ROOT / 'tests/staging-role.c'), '-Wl,--gc-sections', '-L' + str(lib), '-Wl,-rpath,' + str(lib),
            '-lpq', '-o', str(executable)], check=True)
        env = dict(environment, LAPLACE_TEST_CONNINFO='dbname=' + identifier, LAPLACE_TEST_ROLE=identifier)
        subprocess.run([str(executable)], env=env, check=True)
        subprocess.run([str(executable)], env=env, check=True)
        assert sql(identifier, "SELECT count(*) FROM pg_tables WHERE schemaname='stage';") == '2'
    print('Production stage_open passed: existing administrator-owned schema/index, missing partition staging table, scoped non-superuser and repeated invocation.')
finally:
    sql('postgres', f'DROP DATABASE IF EXISTS "{identifier}" WITH (FORCE); DROP ROLE IF EXISTS "{identifier}";')
