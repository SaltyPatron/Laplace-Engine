"""`laplace status` fails loudly when the server cannot read its tier 0 or highway, and passes when it can.

laplace.tier0 and laplace.highway are the extension's PGC_SUSET settings (Laplace-postgres laplace_pg.c,
cache_setting): a superuser session, or one granted SET on them, may set them for itself at connection start. Each
status here runs its own sessions with PGOPTIONS naming a file that does not exist, so only those sessions see it; the
database's own settings are never altered. The earlier private checks did the same with ALTER DATABASE on a database of
their own, which the shared database does not allow.

It proves: status refuses an unreadable server tier 0 with the server's message and no stale comparison; status refuses
an unreadable server highway; status of the deployment as it is reports both artifacts the same as this Engine's and
no negative row count from planner statistics not yet gathered. It also proves the argument-level refusal of
`deploy --prepare --configure-server-cache` (no database is reached: the program refuses before it connects, and the
connection string it is given names no server).
"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, connection, program, psql, sql  # noqa: E402


def main():
    may_set = sql("SELECT CASE WHEN (SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname=current_user) THEN true"
                  " ELSE pg_catalog.has_parameter_privilege('laplace.tier0','SET')"
                  " AND pg_catalog.has_parameter_privilege('laplace.highway','SET') END")
    if may_set != 't':
        raise SystemExit('server-cache: the runner role may not set laplace.tier0/laplace.highway for its own session '
                         '(PGC_SUSET); this check needs a superuser or GRANT SET ON PARAMETER')
    with Fixture('server-cache') as fx:
        missing = fx.scratch / 'missing'
        assert ' ' not in str(missing), f'the scratch path holds a space PGOPTIONS cannot carry: {missing}'
        for setting, message in (('laplace.tier0', 'cannot map tier 0'), ('laplace.highway', 'cannot map the highway')):
            options = f'-c {setting}={missing.as_posix()}/{setting}.bin'
            seen = subprocess.run([psql(), connection(), '-XwAt', '-c', f"SELECT current_setting('{setting}')"],
                                  env=dict(os.environ, PGOPTIONS=options), text=True, capture_output=True, timeout=60)
            assert seen.returncode == 0 and seen.stdout.strip().endswith(f'{setting}.bin'), \
                f'the session did not take {setting} from PGOPTIONS: {seen.stdout}{seen.stderr}'
            failed = fx.run('status', check=False, env={'PGOPTIONS': options})
            assert failed.returncode != 0, f'status accepted an unreadable server {setting}:\n{failed.stdout}'
            assert message in failed.stderr, f'status did not give the server\'s refusal of {setting}:\n{failed.stdout}{failed.stderr}'
            assert 'older than' not in failed.stdout, failed.stdout
        checked = fx.run('status', check=False)
        # by section: tier 0 and highway each the same as this engine's, nothing DIFFERS (status itself compares the
        # chess perf-cache when the deployment has one, so the count of matching lines depends on what is deployed)
        sections, current = {}, None
        for line in checked.stdout.splitlines():
            if line and not line[0].isspace():
                current = line.split('  ')[0].strip(); sections[current] = []
            elif current is not None:
                sections[current].append(line.strip())
        assert checked.returncode == 0 and 'DIFFERS' not in checked.stdout and all(
            "the same as this engine's" in sections.get(name, ()) for name in ('tier 0', 'highway')), \
            f'status of the deployment does not find both artifacts the same as this engine\'s:\n{checked.stdout}{checked.stderr}'
        assert not re.search(r'entities and paths\s+-\d+', checked.stdout), \
            f'unknown planner statistics became a negative row count:\n{checked.stdout}'

    # The argument-level refusal: never run through Fixture.run (it refuses deploy outright); the connection string
    # names a port nothing listens on, so even a program that did not refuse would reach no database.
    with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as scratch:
        unused = 'host=127.0.0.1 port=1 dbname=unused connect_timeout=1'
        refused = subprocess.run([str(program()), 'deploy', '--prepare', '--configure-server-cache', '-d', unused],
                                 env=dict(os.environ, LAPLACE_CONNINFO=unused, LAPLACE_WORK=scratch),
                                 text=True, capture_output=True, timeout=60)
        assert refused.returncode == 2 and 'cannot configure them' in refused.stderr, \
            f'deploy --prepare accepted a server-cache mutation: {refused.returncode}\n{refused.stdout}{refused.stderr}'
    print('server-cache: status refuses an unreadable server tier 0 and highway, passes the deployment as it is, '
          'and --prepare refuses to configure server caches')
    return 0


if __name__ == '__main__':
    sys.exit(main())
