"""Ingestion, replay and a Native read through the deployed program under the deployed scoped database identity.

The deployment grants its runtime a role that is no superuser and owns nothing (Laplace-Operations
installation/database.py, runtime_grants): CONNECT and TEMPORARY on the database, USAGE and CREATE on schema stage,
and the rows of the extension's tables. A check cannot make a role of its own on the shared cluster (a role created in a
transaction it rolls back is invisible to the ingest's own connections), so it ingests as that granted role: the
runner's session takes it with `-c role=...` (PGOPTIONS), as the earlier private check took its own.

It proves, scoped to its own source: the ingest succeeds as the scoped role and records its trunk with attestations;
a second ingest of the unchanged source changes nothing under that trunk; the loader leaves no per-run staging table
behind and replaces or re-owns no lasting stage relation; and the code it read is held, read back by `hop` as the
scoped role. The fixture's code identifier is put together at run time, so this file, read as code by
workspace-code, does not hold it.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, connection, fingerprint, psql, sql  # noqa: E402

CHECK = 'scoped-role'


def scoped_role():
    """The deployed scoped ingestion role: not a superuser, may log in, may create in stage and insert entities."""
    rows = sql("""
SELECT r.rolname FROM pg_catalog.pg_roles r
 WHERE NOT r.rolsuper AND r.rolcanlogin AND pg_catalog.to_regnamespace('stage') IS NOT NULL
   AND pg_catalog.has_schema_privilege(r.oid, 'stage', 'CREATE')
   AND pg_catalog.has_table_privilege(r.oid, (SELECT pg_catalog.format('%I.entity', n.nspname)
         FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON n.oid=e.extnamespace
        WHERE e.extname='laplace'), 'INSERT')
 ORDER BY r.rolname""")
    roles = [r for r in rows.splitlines() if r]
    if not roles:
        raise SystemExit('scoped-role: the deployment granted no scoped ingestion role '
                         '(Laplace-Operations installation/database.py runtime_grants)')
    return roles[0]


def as_role(role, statement):
    """One psql statement in a session that has taken the role, as the Engine's sessions do below."""
    result = subprocess.run([psql(), connection(), '-XwAt', '-v', 'ON_ERROR_STOP=1', '-c', statement],
                            env=dict(os.environ, PGOPTIONS=f'-c role={role}'), text=True, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result.stdout.strip()


def stage_relations():
    """The relations of schema stage: lasting ones with their OID and owner, and the per-run lp_* tables by name."""
    rows = sql(r"""SELECT c.relname, c.oid::text, c.relowner::text FROM pg_catalog.pg_class c
 JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='stage' ORDER BY 1""")
    lasting, per_run = {}, set()
    for line in rows.splitlines():
        name, oid, owner = line.split('|')
        if name.startswith('lp_'):
            per_run.add(name)
        else:
            lasting[name] = (oid, owner)
    return lasting, per_run


def main():
    role = scoped_role()
    found = as_role(role, "SELECT current_user || '|' || CASE WHEN rolsuper THEN 'superuser' ELSE 'scoped' END"
                          " FROM pg_catalog.pg_roles WHERE rolname=current_user")
    assert found == role + '|scoped', f'the session did not take the scoped role: {found}'
    identifier = '_'.join(('laplace', 'check', 'scoped', 'reader', 'probe'))
    with Fixture(CHECK) as fx:
        name, root = fx.source('loader', 'reads python', recipes={
            # A witness is known when it has attestations, and python content attests nothing: a table whose rows
            # attest gives the source a witness.
            'claims.recipe': f'name {CHECK}-claims\nmatch *.tsv\ntier row by \\n\nnames thing value\ntier field by \\t\n'
                             'thing row thing\ncontent thing value\nrelation "has value" value\nattest row value\n'})
        (root / 'claims.tsv').write_text('scoped dog\twarm\nscoped cat\tcold\n', encoding='utf-8', newline='\n')
        (root / 'probe.py').write_text(f'def {identifier}():\n    return "scoped ingestion"\n', encoding='utf-8', newline='\n')
        fx.forget()
        scoped = {'PGOPTIONS': f'-c role={role}', 'OMP_NUM_THREADS': '2', 'LAPLACE_DB_POOL_SIZE': '2', 'LAPLACE_TIMES': '1'}
        lasting, per_run = stage_relations()

        fx.run('ingest', '-j', '2', name, env=scoped)
        trunks = fx.trunks(name)
        assert len(trunks) == 1, f'the scoped ingest did not record one trunk of its source: {trunks}'
        attested = sql('SELECT count(*) FROM attestation WHERE witness=:\'t\'::blake3', variables={'t': trunks[0]})
        assert int(attested) > 0, 'the scoped ingest recorded no attestation under its trunk'
        recorded = fingerprint(trunks)

        fx.run('ingest', '-j', '2', name, env=scoped)
        assert fx.trunks(name) == trunks, f'replay changed the source trunk: {trunks} -> {fx.trunks(name)}'
        assert fingerprint(trunks) == recorded, 'replay changed what is recorded under the source trunk'

        lasting_after, per_run_after = stage_relations()
        assert lasting_after == lasting, f'the scoped loader replaced or re-owned a stage relation: {lasting} -> {lasting_after}'
        left = per_run_after - per_run
        assert not left, f'the scoped loader left private staging tables behind: {sorted(left)}'

        output = fx.run('hop', identifier, env={'PGOPTIONS': f'-c role={role}'}).stdout
        assert 'observed: held by 1 path' in output, f'the scoped Native read did not recover the recorded code holder:\n{output}'
    print(f'scoped-role: ingestion, replay, staging ownership and Native readback as {role}, scoped to its own source')
    return 0


if __name__ == '__main__':
    sys.exit(main())
