#!/usr/bin/env python3
"""The real parity EXIT trap drops scratch databases and exposes deletion failures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    psql = shutil.which(os.environ.get('PSQL', 'psql'))
    if not psql:
        raise RuntimeError('PSQL must select the acceptance PostgreSQL client')
    environment = dict(os.environ, REAL_PSQL=psql)
    def sql(statement):
        return subprocess.check_output([psql, '-XqAt', '-v', 'ON_ERROR_STOP=1', '-d', 'postgres',
                                        '-c', statement], env=environment, text=True).strip()

    with tempfile.TemporaryDirectory(prefix='parity-cleanup-', dir=os.environ.get('TMPDIR')) as scratch:
        root = Path(scratch)
        (root / 'mono').mkdir()
        (root / 'mono/Laplace.Cli.dll').touch()  # Admission only; no managed program runs in this failure test.
        engine = root / 'engine'
        engine.write_text('''#!/usr/bin/env python3
import json, os, re, subprocess, sys
database = re.search(r'dbname=(laplace_parity_[a-z0-9_]+)', sys.argv[-1]).group(1)
databases = [database, database.removesuffix('_engine') + '_mono']
open(os.environ['DATABASE_RECORD'], 'w').write(json.dumps(databases))
for name in databases:
    subprocess.run([os.environ['REAL_PSQL'], '-XqAt', '-v', 'ON_ERROR_STOP=1', '-d', 'postgres',
                    '-c', 'CREATE DATABASE "' + name + '"'], check=True)
sys.exit(19)  # A controlled deployment failure, before any ingestion or corpus reads.
''')
        engine.chmod(0o700)
        wrapper = root / 'psql'
        wrapper.write_text('''#!/usr/bin/env python3
import os, sys
if os.environ.get('REFUSE_MONO_DROP') == '1' and any('DROP DATABASE' in arg and '_mono"' in arg for arg in sys.argv):
    print('controlled PostgreSQL deletion refusal', file=sys.stderr)
    sys.exit(23)
os.execv(os.environ['REAL_PSQL'], [os.environ['REAL_PSQL'], *sys.argv[1:]])
''')
        wrapper.chmod(0o700)
        record = root / 'databases.json'
        environment.update(PSQL=str(wrapper), LAPLACE_ENGINE_BIN=str(engine), TMPDIR=str(root),
                           LAPLACE_MONO_CLI_DIR=str(root / 'mono'), LAPLACE_DATA=str(root),
                           DATABASE_RECORD=str(record))
        harness = root / 'parity.sh'
        # read_text normalizes a Windows checkout's CRLF for the Linux shell.
        harness.write_text((Path(__file__).resolve().parents[1] / 'tools/parity/parity.sh').read_text())
        for refused, keep in ((False, False), (True, False), (False, True)):
            names = []
            try:
                environment['REFUSE_MONO_DROP'] = '1' if refused else '0'
                command = ['bash', str(harness), '--out', str(root / 'output')]
                if keep:
                    command.append('--keep')
                result = subprocess.run([*command, 'fixture=fixture'], env=environment, capture_output=True, text=True)
                assert record.is_file(), result.stdout + result.stderr
                names = json.loads(record.read_text())
                assert result.returncode == 19, result.stdout + result.stderr
                remaining = [name for name in names if sql("SELECT 1 FROM pg_database WHERE datname='" + name + "'") == '1']
                assert remaining == (names if keep else names[1:] if refused else []), (remaining, result.stderr)
                if refused:
                    assert 'controlled PostgreSQL deletion refusal' in result.stderr
                    assert 'database cleanup failed: ' + names[1] in result.stderr
                if keep:
                    assert '--keep' in result.stdout
            finally:
                for name in names:
                    sql('DROP DATABASE IF EXISTS "' + name + '" WITH (FORCE)')
    print('PASS parity cleanup: real scratch databases removed; refusal visible; original failure and explicit retention preserved')


if __name__ == '__main__':
    main()
