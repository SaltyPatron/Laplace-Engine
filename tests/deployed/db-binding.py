"""The production libpq adapter and the installed binding, then ingestion, an indexed read and removal of a fixture.

pg_plan_lifecycle (tests/pg-plan-lifecycle.c, built in the deployed Engine's build tree) runs against the deployed
database: connection-local prepared plans, the SQL buffer a plan came from, reset, the caller's planner policy, read
snapshots exported and imported, the installed extension and PostGIS namespaces bound on two connections and across
a reset, and the exact-ID reads (coordinates, tiers, paths, holders, standings) of a probe composition it inserts and
rolls back. The catalog readback below shows every C function of the extension bound to one module, the binding
`laplace deploy` restores (laplace_restore_function_bindings; the deployment runs it).

The fixture: a three-row source of words no other source holds is ingested by its name (a source's files read with -s make no trunk), recomposed byte for
byte, ingested again with nothing under its trunk changed, read back by `hop`, and forgotten by its trunk ID, after
which nothing of that trunk is recorded. Everything asserted is under its own trunk.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, connection, fingerprint, sql  # noqa: E402

CHECK = 'db-binding'


def adapter():
    binary = Path(os.environ.get('LAPLACE_CHECK_BIN', '')) / 'pg_plan_lifecycle'
    if not binary.is_file():
        raise SystemExit(f'db-binding: no check program {binary} (LAPLACE_CHECK_BIN; CMake target pg_plan_lifecycle)')
    result = subprocess.run([str(binary), connection()], text=True, capture_output=True, timeout=300)
    if result.returncode:
        raise SystemExit(f'db-binding: pg_plan_lifecycle exited {result.returncode}\n{result.stdout}{result.stderr}')
    print(result.stdout.strip())


def bindings():
    """Every C function the extension owns is bound to the one module laplace_isa() is bound to."""
    rows = sql(r"""
SELECT count(DISTINCT p.probin), count(*) FILTER (WHERE p.probin IS DISTINCT FROM
         (SELECT probin FROM pg_catalog.pg_proc WHERE oid=(SELECT pg_catalog.format('%I.laplace_isa()', n.nspname)
            FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON n.oid=e.extnamespace
           WHERE e.extname='laplace')::pg_catalog.regprocedure))
  FROM pg_catalog.pg_proc p
  JOIN pg_catalog.pg_depend d ON d.classid='pg_catalog.pg_proc'::pg_catalog.regclass AND d.objid=p.oid AND d.deptype='e'
  JOIN pg_catalog.pg_extension e ON e.oid=d.refobjid AND e.extname='laplace'
 WHERE p.prolang=(SELECT oid FROM pg_catalog.pg_language WHERE lanname='c')""")
    modules, elsewhere = rows.split('|')
    assert modules == '1' and elsewhere == '0', f'the extension\'s C functions are bound to {modules} modules ({elsewhere} not to laplace_isa\'s)'


def main():
    adapter()
    bindings()
    words = ['-'.join(('binding', 'check', w)) for w in ('dog', 'warm', 'cat', 'cold')]
    with Fixture(CHECK) as fx:
        name, root = fx.source('reading', 'class AcademicCurated', recipes={
            'reading.recipe': f'name {CHECK}-reading\nmatch *.tsv\ntier row by \\n\nnames thing value\ntier field by \\t\n'
                              'thing row thing\ncontent thing value\nrelation "has value" value\nattest row value\n'})
        dog, warm, cat, cold = words
        (root / 'facts.tsv').write_text(f'{dog}\t{warm}\n{cat}\t{cold}\n{dog}\t{warm}\n', encoding='utf-8', newline='\n')
        fx.forget()
        report = fx.run('ingest', '-j', '2', name).stdout
        assert 'mismatched 0' in report, report
        witness = re.search(r'its ID, the witness\s+([a-f0-9]{32})', report)
        assert witness, report
        trunk = witness[1]
        assert fx.trunks(name) == [trunk], f'the source trunk said is not the one recorded: {trunk} {fx.trunks(name)}'
        counts = sql('SELECT count(*) FROM attestation WHERE witness=:\'t\'::blake3;'
                     ' SELECT count(*) FROM consensus WHERE claim IN (SELECT claim FROM attestation WHERE witness=:\'t\'::blake3);'
                     ' SELECT count(*) FROM physicality WHERE entity=:\'t\'::blake3', variables={'t': trunk})
        assert all(int(v) > 0 for v in counts.splitlines()), f'attestations, standings, trunk paths: {counts}'
        recorded = fingerprint([trunk])
        fx.run('ingest', '-j', '2', name)
        assert fx.trunks(name) == [trunk] and fingerprint([trunk]) == recorded, 'an unchanged reading changed its trunk'
        hop = fx.run('hop', dog).stdout
        assert re.search(r'attested: [1-9]', hop), f'the indexed read did not find the claim the fixture attests:\n{hop}'
        fx.run('forget', '--trunk', trunk)
        left = sql('SELECT count(*) FROM attestation WHERE witness=:\'t\'::blake3;'
                   ' SELECT count(*) FROM physicality WHERE entity=:\'t\'::blake3', variables={'t': trunk})
        assert left.splitlines() == ['0', '0'], f'forget left attestations or trunk paths: {left}'
        assert fx.trunks(name) == [], f'forget left the source trunk: {fx.trunks(name)}'
    print('db-binding: the libpq adapter, the installed binding, ingestion, an indexed read and removal of its own source')
    return 0


if __name__ == '__main__':
    sys.exit(main())
