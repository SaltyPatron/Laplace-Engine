"""Real grammar admission with physical windows of every size: an ingest that checkpoints after every grammar window
(LAPLACE_WORKING_SET_BYTES=1) and one whose whole fixture fits one shared working window (1 GiB) record the same
canonical rows, geometry, masks, witnesses, standings and cumulative testimony; and small files share database applies
when memory permits.

Scope: one check source (check-stream-working-set-frames, PropBank's recipes copied under the check's witness, eight
small frame files) is ingested with the one-byte budget, fingerprinted, forgotten, ingested again with the 1 GiB budget
and fingerprinted again. A fingerprint (check.fingerprint) covers only what the check's trunk reaches: the entity and
physicality rows of every ID under it, its claims' attestation rows by its trunk, their consensus rows and the
testimony laplace_contained_series reads under the trunk. Where the original counted laplace_merge_stage calls in
pg_stat_statements (cluster-wide, shared with the running service), the check reads the ingest's own count of shared
working windows, each one database apply, from the run's source log.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, fingerprint  # noqa: E402

FRAME = '''<?xml version="1.0" encoding="UTF-8"?>
<frameset><predicate lemma="shared">
<roleset id="shared.01" name="shared description">
<aliases><alias pos="v">shared</alias></aliases>
<roles><role n="0" descr="actor {i}"/><role n="1" descr="theme"/></roles>
<example name="example {i}"><text>They shared the result.</text></example>
</roleset></predicate></frameset>
'''
WINDOWS = re.compile(r'streaming persistence: ([\d,]+) shared working windows')


def main():
    with Fixture('stream-working-set') as fixture:
        name, root = fixture.source('frames', copy_from='propbank')
        for i in range(8):
            (root / f'frame-{i}.xml').write_text(FRAME.format(i=i), encoding='utf-8', newline='\n')
        fixture.forget()
        prints, windows = {}, {}
        # One byte forces a checkpoint after every grammar window; one GiB admits this whole small fixture. These are
        # qualification settings, not defaults.
        for lane, budget in (('small', 1), ('shared', 1024 * 1024 * 1024)):
            fixture.run('ingest', '-j', '2', name,
                        env={'LAPLACE_WORKING_SET_BYTES': str(budget), 'LAPLACE_RECORD_WINDOW_BYTES': '97'})
            log = (fixture.work / 'logs' / 'ingest' / f'{name}.log').read_text(encoding='utf-8', errors='replace')
            found = WINDOWS.search(log)
            if not found:
                raise SystemExit(f'stream-working-set: {lane}: the source log says no shared working windows\n{log}')
            windows[lane] = int(found[1].replace(',', ''))
            trunks = fixture.trunks(name)
            if len(trunks) != 1:
                raise SystemExit(f'stream-working-set: {lane}: {len(trunks)} trunks recorded for {name}, not 1')
            prints[lane] = fingerprint(trunks)
            for table in ('entity', 'consensus', 'series'):
                if not re.search(rf'^{table}\|[1-9]\d*\|', prints[lane], re.M):
                    raise SystemExit(f'stream-working-set: {lane}: no {table} rows under the check\'s trunk\n{prints[lane]}')
            print(f'stream-working-set: {lane} budget={budget} bytes, {windows[lane]} shared working windows')
            if lane == 'small':
                fixture.forget()
        if prints['small'] != prints['shared']:
            raise SystemExit('stream-working-set: the two budgets recorded different rows\n'
                             f'small:\n{prints["small"]}\nshared:\n{prints["shared"]}')
        if windows['shared'] != 1 or windows['small'] <= windows['shared']:
            raise SystemExit(f'stream-working-set: windows small={windows["small"]} shared={windows["shared"]}:'
                             ' the small files did not share one database apply')
    print('stream-working-set: identical canonical rows, geometry, masks, witnesses, standings and cumulative testimony'
          ' under the check\'s trunk; small files shared database applies')
    return 0


if __name__ == '__main__':
    sys.exit(main())
