"""Real COPY and publication, closed subtrees during an interruption, and exact replay, in the deployed database: a
serial ingest (one writer), a parallel one (eight writers) watched while it publishes, and one killed part way and
resumed, then ingested again unchanged, all record the same rows; no parent the check composed is ever visible
before a child it holds, during publication or after the kill.

Scope: one check source (check-publication-waves-ordered, the stock text recipe over one file of distinct words) is
ingested, fingerprinted and forgotten between the cases, so each case writes its whole closure again. A fingerprint
(check.fingerprint) and the closure observation cover only the IDs under the check's trunk, as the serial case
recorded them. The killed ingest's leftover statements are cancelled by its own application name, never anyone
else's. The original's GIN pending-list assertion (laplace_clean_pending_list over every physicality GIN index) is
not kept: the pending list is the whole index's, shared with every writer of the deployed database, and cleaning
it is index maintenance a check does not do there.
"""
import os
import re
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, closure, fingerprint, ids, sql  # noqa: E402

# Enough distinct words for a DAG of several tiers and parallel publication; few enough that forgetting the check's
# closure stays under the Engine's 4,096-candidate set-oriented sweep. Override with PUBLICATION_WAVES_WORDS.
WORDS = int(os.environ.get('PUBLICATION_WAVES_WORDS', '3000'))


def missing_children(parents):
    """The children of the given parents that are neither stored nor codepoints: what a reader would find absent."""
    if not parents:
        return set()
    rows = sql('SELECT DISTINCT v.id::text FROM laplace_paths(' + ids(parents) + ') p'
               ' CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) v(id)'
               ' WHERE laplace_codepoint(v.id) IS NULL'
               ' AND NOT EXISTS (SELECT 1 FROM entity e WHERE e.id=v.id)')
    return {r for r in rows.splitlines() if r}


def main():
    with Fixture('publication-waves') as fixture:
        name, root = fixture.source('ordered', 'reads text')
        (root / 'ordered.txt').write_text(' '.join(f'q{i:08x}' for i in range(WORDS)) + '\n',
                                          encoding='utf-8', newline='\n')
        application = f'laplace-check-publication-waves-{uuid.uuid4().hex[:12]}'
        # LAPLACE_INGEST_ONE: the source runs in the started process, its report in the case's log.
        base = {'LAPLACE_TRUNKS': '1', 'LAPLACE_WORKING_SET_BYTES': str(1 << 30), 'PGAPPNAME': application,
                'LAPLACE_INGEST_ONE': '1'}

        def start(writers, case):
            log = fixture.scratch / f'{case}.log'
            env = dict(base, LAPLACE_DB_POOL_SIZE=str(writers), LAPLACE_WORK=str(fixture.scratch / f'{case}-work'))
            return fixture.start('ingest', '-j', '8', name, log=log, env=env), log

        def finished(process, log, case):
            if process.wait(timeout=600) != 0:
                raise SystemExit(f'publication-waves: {case} ingest exited {process.returncode}\n'
                                 + log.read_text(errors='replace'))

        def trunk():
            found = fixture.trunks(name)
            if len(found) != 1:
                raise SystemExit(f'publication-waves: {len(found)} trunks recorded for {name}, not 1')
            return found

        reports = []
        fixture.forget()
        before = time.monotonic()
        process, log = start(1, 'serial')
        finished(process, log, 'serial')
        trunks = trunk()
        expected = fingerprint(trunks)
        expected_ids = closure(trunks)
        if len(expected_ids) <= WORDS:
            raise SystemExit(f'publication-waves: {len(expected_ids)} IDs under the trunk: the fixture is no multi-tier DAG')
        # The parents above the words: what could be published before a child it holds.
        parents = sql('SELECT id::text FROM entity WHERE id=ANY(' + ids(expected_ids) + ') AND tier>1').split()
        reports.append(f'serial {time.monotonic() - before:.3f} s, {len(expected_ids)} IDs')
        fixture.forget()
        left = sql('SELECT count(*) FROM entity WHERE id=ANY(' + ids(parents) + ')')
        if left != '0':
            raise SystemExit(f'publication-waves: forget left {left} of the check\'s parents stored: the next case would publish nothing')

        before = time.monotonic()
        process, log = start(8, 'parallel')
        observations = 0
        while process.poll() is None:
            if missing_children(parents):
                process.kill()
                raise SystemExit('publication-waves: a visible parent preceded its stored child')
            observations += 1
            time.sleep(0.02)
        finished(process, log, 'parallel')
        if fingerprint(trunk()) != expected:
            raise SystemExit('publication-waves: parallel publication changed canonical state')
        lanes = [int(x) for x in re.findall(r'up to (\d+) writers', log.read_text(errors='replace'))]
        if not lanes or max(lanes) <= 1:
            raise SystemExit('publication-waves: the fixture did not exercise parallel publication\n'
                             + log.read_text(errors='replace'))
        reports.append(f'parallel {time.monotonic() - before:.3f} s, {observations} observations, {max(lanes)} writers')
        fixture.forget()

        process, log = start(8, 'interrupted')
        deadline = time.monotonic() + 600
        while process.poll() is None and time.monotonic() < deadline:
            if sql('SELECT count(*) FROM laplace_paths(' + ids(expected_ids) + ')') != '0':
                process.kill()
                break
            time.sleep(0.02)
        process.wait(timeout=60)
        if process.returncode == 0:
            raise SystemExit('publication-waves: the fixture finished before it could be interrupted')
        # The killed client's backends may still be finishing a statement: cancel those of its own application name.
        sql("SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name=:'app' AND state='active'",
            variables={'app': application})
        for _ in range(200):
            if sql("SELECT count(*) FROM pg_stat_activity WHERE application_name=:'app'", variables={'app': application}) == '0':
                break
            time.sleep(0.05)
        else:
            raise SystemExit('publication-waves: the killed ingest\'s sessions did not end')
        if missing_children(parents):
            raise SystemExit('publication-waves: the interruption exposed an incomplete subtree')
        process, log = start(8, 'resumed')
        finished(process, log, 'resumed')
        if fingerprint(trunk()) != expected:
            raise SystemExit('publication-waves: the resumed ingest changed canonical state')
        process, log = start(8, 'replay')
        finished(process, log, 'replay')
        if fingerprint(trunk()) != expected:
            raise SystemExit('publication-waves: an unchanged replay changed canonical state')
        reports.append('interrupted, resumed and replayed: closed and identical')
    print('publication-waves: ' + '; '.join(reports))
    return 0


if __name__ == '__main__':
    sys.exit(main())
