"""Producer receipts (LAPLACE_INGEST_PROGRESS_PATH) describe the committed source and its trunk; an unchanged
reading changes nothing; the retained testimony measures what is stored; and a warm partial reading gives the same
source reference as a cold one.

Its own source reads Unicode's LinkEmail.txt, NamedSequencesProv.txt and ReadMe.txt with copies of the unicode
source's recipes (renamed, no tier-0 floor): two Link_Email rows of private-use codepoints, one of them twice, so no
other source attests its claims. Scoped: the frames' source and file IDs, the source's trunks (Fixture.trunks), the
claims its trunks attest and their standings; nothing else of a table is read.

What the shared database cannot show, and is not asserted: the complete frame's entity and physicality totals equal
to the tables' row counts (the database holds rows this run did not write: the totals are required positive, and every
ID a frame names is recorded); the measurement recovery itself (laplace_measure_attestations commits per witness, so it
cannot run inside a transaction that is rolled back, and a committed removal of attestation rows other than `laplace
forget` is not made): the stored rows are compared, read only, with the testimony the recovery measures from
(laplace_contained_series). The cold reading is the source forgotten and read again, not a second database.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, ids, sql  # noqa: E402

CHECK = 'ingest-progress'


def frames(path):
    return [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]


def reference(trunk):
    return sql("SELECT jsonb_build_array(e.id::text,e.tier,e.hilbert,encode(ST_AsEWKB(e.coord),'hex'))"
               ' FROM entity e WHERE e.id=:\'t\'::blake3', variables={'t': trunk})


def standings(trunks):
    return sql('SELECT jsonb_agg(to_jsonb(c) ORDER BY c.claim) FROM consensus c WHERE c.claim IN'
               f' (SELECT claim FROM attestation WHERE witness=ANY({ids(trunks)}))')


def main():
    with Fixture(CHECK) as fx:
        def recipe(file, name):
            text = (fx.recipes / 'unicode' / file).read_text(encoding='utf-8')
            return re.sub(r'^name .*$', f'name {CHECK}-{name}', text, count=1, flags=re.M)
        source, root = fx.source('ucd', 'class StandardsDerived', recipes={
            'link-email.recipe': recipe('link-email.recipe', 'link-email'),
            'named-sequences.recipe': recipe('named-sequences.recipe', 'named-sequences'),
            'readme.recipe': recipe('readme.recipe', 'readme')})
        (root / 'LinkEmail.txt').write_text('E000\nE001\nE000\n', encoding='utf-8', newline='\n')
        # Like the real NamedSequencesProv.txt: fully read, with no provisional rows.
        (root / 'NamedSequencesProv.txt').write_text('# Provisional named sequences\n'
                                                     '# No provisional entries are currently defined.\n',
                                                     encoding='utf-8', newline='\n')
        fx.forget()
        log = fx.work / 'logs/ingest' / f'{source}.log'

        def ingest(progress=None, **env):
            if progress:
                env['LAPLACE_INGEST_PROGRESS_PATH'] = str(progress)
            fx.run('ingest', '-j', '2', source, env=env)
            return log.read_text(encoding='utf-8', errors='replace')

        # Cross grammar-framed windows so the last row updates an earlier cumulative series rather than duplicating
        # or adding its total again.
        progress = fx.scratch / 'progress.jsonl'
        ingest(progress, LAPLACE_RECORD_WINDOW_BYTES='1')
        receipts = frames(progress)
        complete = [f for f in receipts if f['event'] == 'complete']
        assert len(complete) == 1, f'no single complete frame: {receipts}'
        last = complete[0]
        trunk = last.get('source_id', '')
        assert fx.trunks(source) == [trunk], f'the complete frame\'s source is not the recorded trunk: {trunk} {fx.trunks(source)}'
        assert last['entities'] > 0 and last['physicalities'] > 0, f'the run wrote no rows: {last}'
        assert last['files_total'] == 2 and last['files_done'] == 2, f'files: {last}'
        assert any(f['event'] == 'file-start' for f in receipts), 'no file-start frame'
        assert any(f['event'] == 'file-composed' and f.get('file', '').endswith('/NamedSequencesProv.txt') and 'file_id' not in f
                   for f in receipts), 'the comments-only file is not composed without a file ID'
        named = sorted({f['file_id'] for f in receipts if f['event'] == 'file-composed' and 'file_id' in f})
        assert named, 'no composed file frame names its file'
        stored = sql(f'SELECT count(DISTINCT id) FROM entity WHERE id=ANY({ids([trunk, *named])})')
        assert int(stored) == len(named) + 1, f'a file or source a frame names is not recorded: {stored} of {len(named) + 1}'
        assert sql('SELECT count(*) FROM attestation WHERE witness=:\'t\'::blake3', variables={'t': trunk}) != '0', \
            'the source the frame names attests nothing'
        assert last['attestations_measured'] is True and last['attestations'] == 2, \
            f'two distinct assertions became sources, repeated games, or unmeasured counters: {last}'
        assert sql('SELECT count(DISTINCT claim) FROM attestation WHERE witness=:\'t\'::blake3', variables={'t': trunk}) == '2', \
            'the source does not attest two distinct claims'
        series = sql('SELECT count(*) FROM laplace_contained_series(ARRAY[:\'t\'::blake3])'
                     ' WHERE claim=laplace_compose(ARRAY[laplace_cp_id(57344),laplace_id(\'Link_Email\')])'
                     ' AND games=2 AND score=2', variables={'t': trunk})
        assert series == '1', 'streaming windows lost or duplicated cumulative testimony'
        assert sum(1 for f in receipts if f['event'] == 'progress' and f.get('file', '').endswith('/LinkEmail.txt')) >= 3, \
            'the windows of LinkEmail.txt did not each report progress'

        # An unchanged reading: every file found recorded, nothing under the trunk or of its standings changed.
        before, standing_before = reference(trunk), standings([trunk])
        text = ingest()
        assert re.search(r'which say nothing again: [1-9]', text), text
        assert fx.trunks(source) == [trunk] and reference(trunk) == before, 'an unchanged precheck changed the source reference'
        assert standings([trunk]) == standing_before, 'an unchanged reading changed the standings'

        # The retained testimony measures what is stored: the games laplace_measure_attestations would write back.
        difference = sql("""
SELECT count(*) FROM (
 (SELECT claim::text, games::bigint FROM laplace_contained_series(ARRAY[:'t'::blake3]) WHERE games>0
  EXCEPT SELECT claim::text, games FROM attestation WHERE witness=:'t'::blake3)
 UNION ALL
 (SELECT claim::text, games FROM attestation WHERE witness=:'t'::blake3
  EXCEPT SELECT claim::text, games::bigint FROM laplace_contained_series(ARRAY[:'t'::blake3]) WHERE games>0)) d""",
                         variables={'t': trunk})
        assert difference == '0', f'stored attestations differ from the testimony that measures them: {difference} rows'

        # Reuse unread recorded files while adding a new one; the new release's reference must be a cold reading's.
        (root / 'ReadMe.txt').write_text('Laplace check ingest-progress fixture description\n', encoding='utf-8', newline='\n')
        text = ingest()
        assert re.search(r'which say nothing again: [1-9]', text), text
        releases = fx.trunks(source)
        new = [t for t in releases if t != trunk]
        assert trunk in releases and len(new) == 1, f'the partial reading did not add one release beside the first: {releases}'
        warm = reference(new[0])
        fx.forget(source)
        assert fx.trunks(source) == [], 'forget left a release of the source'
        ingest()
        assert fx.trunks(source) == new, f'the cold reading is another source: {new} {fx.trunks(source)}'
        assert reference(new[0]) == warm, 'the partial reading disagrees with the cold source ID/coordinates/tier/Hilbert'
    print('ingest-progress: committed receipts, unchanged reuse, measured testimony and warm/cold source reference parity')
    return 0


if __name__ == '__main__':
    sys.exit(main())
