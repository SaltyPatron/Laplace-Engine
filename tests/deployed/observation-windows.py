"""Committed observation windows and atomic source publication, observed in the deployed database: while a source
streams with one-byte windows, a second connection sees its committed window observations and, in the same snapshot,
no trunk of the source, no standing of its claims and no consensus bucket lock held; a second witness of the same
claims committed while the first source streams is included by its final publication, not overwritten; and the
published standings, after the first ingest and after a partial replay that adds a file, are exactly what Native
computes from the complete registered evidence (laplace_stage_snapshot).

Scope: two check sources, check-observation-windows-streamed (200 files, class AcademicCurated) and
check-observation-windows-concurrent (one file of the same claims). The words are the check's own, so its claims are
IDs nothing else attests. Windows are found by the stage load's own registration (the table comment names the owning
backend's pid) among the backends of the ingest's own application name. The standings comparison runs on stage tables
created inside a transaction that is rolled back.

What the original did with DDL on the deployed extension is not kept: it held publication on an advisory lock through a
trigger on public.attestation, inserted a fake concurrent attestation row, and replaced laplace_contained_series to
prove that a new source is not walked downward again. Here publication is not held: the source is made long enough
(200 files of one-byte windows) for the observation and the concurrent witness to fall inside its streaming, and the
check fails if they do not. The concurrent witness is a real ingest. The "no downward re-walk" assertion is dropped: it
can only be seen by replacing an extension function, and function call counters (pg_stat_user_functions) are shared
with every session of the database.
"""
import string
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, ids, sql  # noqa: E402

RECIPE = ('name {name}\nmatch *.tsv\ntier row by \\n\nnames thing value\n'
          'tier field by \\t\nthing row thing\ncontent thing value\n'
          'relation "has value" value\nattest row value\n')
FILES = 200


def letters(i):
    """A number written in letters, so a word stays one word."""
    out = ''
    while True:
        out = string.ascii_lowercase[i % 26] + out
        i //= 26
        if not i:
            return out


def rows(i):
    return [('windowdog', 'windowwarm'), ('windowcat', 'windowcold'), ('windowdog', 'windowwarm'),
            ('windowthing' + letters(i), 'windowwarm')]


def write(path, pairs):
    path.write_text(''.join(f'{a}\t{b}\n' for a, b in pairs), encoding='utf-8', newline='\n')


def claim_ids(pairs):
    """The claims [thing, has value, value] as the check composes them."""
    values = ','.join(f"laplace_compose(ARRAY[laplace_text_id('{a}'),laplace_text_id('has value'),laplace_text_id('{b}')])"
                      for a, b in sorted(set(pairs)))
    return sorted(r for r in sql(f'SELECT DISTINCT c::text FROM unnest(ARRAY[{values}]) c').splitlines() if r)


PENDING = """
 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
 JOIN pg_description d ON d.objoid=c.oid AND d.classoid='pg_class'::regclass AND d.objsubid=0
 WHERE n.nspname='stage' AND c.relkind='r' AND c.relname LIKE 'lp\\_pending\\_%'
   AND d.description ~ '^laplace:stage-load:1:[0-9]+:[0-9]+$'
   AND split_part(d.description,':',4)::integer IN (SELECT pid FROM pg_stat_activity WHERE application_name=:'app')"""


def observe(application, witness, claims):
    """One snapshot of the streaming source: rows in its committed windows, its trunks, its claims' standings, and
    whether its own sessions hold no consensus bucket lock. None when its windows are not there (yet, or any more)."""
    try:
        out = sql(f"""
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT format('SELECT %L||count(*) FROM stage.%I','rows|',c.relname) {PENDING} \\gexec
SELECT 'trunks|'||count(DISTINCT p.entity) FROM laplace_witness_trust() w JOIN physicality p ON p.entity=w.witness
 WHERE laplace_path_starts(p.path, laplace_text_id(:'witness'));
SELECT 'consensus|'||count(*) FROM consensus WHERE claim=ANY({ids(claims)});
-- The streaming ingest's own sessions (its application name) hold no consensus bucket lock; other work on the shared
-- database may fold its own claims meanwhile, which says nothing of this ingest.
SELECT 'locks|'||NOT EXISTS (SELECT FROM pg_locks l JOIN pg_stat_activity s ON s.pid=l.pid
 WHERE l.locktype='advisory' AND l.classid=1279348803::oid AND l.objsubid=2 AND l.objid BETWEEN 0 AND 15
   AND s.application_name=:'app');
COMMIT;
""", variables={'app': application, 'witness': witness})
    except RuntimeError:
        return None                       # its windows went between the catalog read and the count
    seen = dict(line.split('|', 1) for line in out.splitlines() if '|' in line)
    return seen if 'rows' in seen else None


def exact_standings(trunks):
    """Consensus of the check's claims equals Native's calculation from the complete registered evidence. The stage
    tables it needs are made in a transaction that is rolled back."""
    table = 'check_window_' + uuid.uuid4().hex[:12]
    out = sql(f"""
BEGIN;
CREATE UNLOGGED TABLE stage.{table}_empty (LIKE stage.attestation INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.{table}_claims AS SELECT DISTINCT claim FROM laplace_contained_series({ids(trunks)});
SELECT 'claims|'||count(*) FROM stage.{table}_claims;
SELECT 'exact|'||NOT EXISTS (
  (SELECT claim,rating,deviation,volatility,matches FROM consensus
    WHERE claim IN (SELECT claim FROM stage.{table}_claims)
   EXCEPT ALL SELECT * FROM laplace_stage_snapshot('stage.{table}_empty','stage.{table}_claims'))
  UNION ALL
  (SELECT * FROM laplace_stage_snapshot('stage.{table}_empty','stage.{table}_claims')
   EXCEPT ALL SELECT claim,rating,deviation,volatility,matches FROM consensus
    WHERE claim IN (SELECT claim FROM stage.{table}_claims)));
ROLLBACK;
""")
    seen = dict(line.split('|', 1) for line in out.splitlines() if '|' in line)
    if int(seen.get('claims', '0')) == 0:
        raise SystemExit('observation-windows: the fixture emitted no testimony')
    if seen.get('exact') != 't':
        raise SystemExit('observation-windows: publication differs from the complete registered Native evidence')


def main():
    with Fixture('observation-windows') as fixture:
        streamed, streamed_root = fixture.source('streamed', 'class AcademicCurated', recipes={
            'reading.recipe': RECIPE.format(name=fixture.name('streamed')[0] + '-reading')})
        concurrent, concurrent_root = fixture.source('concurrent', recipes={
            'reading.recipe': RECIPE.format(name=fixture.name('concurrent')[0] + '-reading')})
        pairs = []
        for i in range(FILES):
            write(streamed_root / f'{letters(i)}.tsv', rows(i))
            pairs += rows(i)
        write(concurrent_root / 'concurrent.tsv', rows(0))
        additional = [('windowdog', 'windowcold'), ('windowcat', 'windowwarm')]
        claims = claim_ids(pairs + additional)
        fixture.forget()

        application = f'laplace-check-observation-windows-{uuid.uuid4().hex[:12]}'
        env = {'LAPLACE_INGEST_ONE': '1', 'LAPLACE_RECORD_WINDOW_BYTES': '1', 'LAPLACE_WORKING_SET_BYTES': '1',
               'PGAPPNAME': application}
        log = fixture.scratch / 'streamed.log'
        process = fixture.start('ingest', '-j', '2', streamed, log=log, env=env)
        observed = None
        deadline = time.monotonic() + 300
        try:
            while process.poll() is None and time.monotonic() < deadline:
                seen = observe(application, fixture.witness_of(streamed), claims)
                if seen and int(seen['rows']) > 0 and seen['trunks'] == '0':
                    observed = seen
                    break
                time.sleep(0.02)
            if not observed:
                raise SystemExit('observation-windows: no committed window was seen before the source published'
                                 ' (the fixture streamed too fast to observe)\n' + log.read_text(errors='replace'))
            if observed['consensus'] != '0':
                raise SystemExit('observation-windows: an unregistered source published standings')
            if observed['locks'] != 't':
                raise SystemExit('observation-windows: streaming retained public consensus bucket locks')
            # A second, independent witness of the same claims, committed while the first source streams.
            fixture.run('ingest', '-j', '2', concurrent, env={'LAPLACE_INGEST_ONE': '1'})
            if process.poll() is not None or fixture.trunks(streamed):
                raise SystemExit('observation-windows: the streamed source published before the concurrent witness'
                                 ' was committed (the fixture streamed too fast)')
            if process.wait(timeout=600):
                raise SystemExit('observation-windows: the streamed ingest failed\n' + log.read_text(errors='replace'))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=60)
        trunks = fixture.trunks(streamed) + fixture.trunks(concurrent)
        if len(trunks) != 2:
            raise SystemExit(f'observation-windows: {len(trunks)} trunks recorded for the two sources, not 2')
        found = sorted(r for r in sql(f'SELECT DISTINCT claim::text FROM laplace_contained_series({ids(trunks)})').split() if r)
        if not set(found) <= set(claims):
            raise SystemExit('observation-windows: the sources attest claims the check did not compose:'
                             f' {sorted(set(found) - set(claims))[:4]}')
        both = sql(f"SELECT count(DISTINCT witness) FROM attestation WHERE claim=ANY({ids(found)}) AND witness=ANY({ids(trunks)})")
        if both != '2':
            raise SystemExit(f'observation-windows: {both} of the two witnesses stand under the claims after publication')
        exact_standings(trunks)
        write(streamed_root / 'additional.tsv', additional)
        fixture.run('ingest', '-j', '2', streamed, env=env)
        exact_standings(fixture.trunks(streamed) + fixture.trunks(concurrent))
    print('observation-windows: committed private windows, no consensus locks while streaming, a concurrent witness'
          ' included, publication and partial replay exact Native standings')
    return 0


if __name__ == '__main__':
    sys.exit(main())
