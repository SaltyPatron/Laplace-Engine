"""Rejected Native readings fail again on replay and publish no witness; after a real correction the reading is
published, and an unchanged replay says nothing again and leaves the testimony exactly as it was.

Scope: two check sources, check-failed-file-replay-pgn (a PGN game with a missing move, then corrected) and
check-failed-file-replay-category (a part-of-speech tag no tagset lists, then NOUN). Where the original counted
DISTINCT witness over the whole attestation table, the check counts the trunks recorded for its own source
(check.Fixture.trunks: none while rejected, one once corrected), and reads laplace_contained_series under that trunk
alone. Each source is ingested alone (LAPLACE_INGEST_ONE=1, as the original did; neither names `after`), so the
rejection's diagnostic and exit status are the ingest's own.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, ids, sql  # noqa: E402

CASES = [
    ('pgn', 'twic-invalid.pgn', 'grammar pgn\nrecords played\n',
     '[Event "Missing move"]\n[Result "1-0"]\n\n1. e4 -- 2. Nf3 1-0\n',
     '[Event "Corrected moves"]\n[Result "1-0"]\n\n1. e4 e5 2. Nf3 1-0\n', 'incomplete_parts=1'),
    ('category', 'words.tsv',
     'tier row by \\n\nnames word pos\ntier field by \\t\nthing row word\n'
     'content word\ntype pos upos\nvalue upos pos\nattest row pos\n',
     'duck\tNOT_A_POS\n', 'duck\tNOUN\n', 'semantic_omissions=1'),
]


def main():
    with Fixture('failed-file-replay') as fixture:
        sources = []
        for part, filename, body, rejected, corrected, diagnostic in CASES:
            name, root = fixture.source(part, 'class StructuredCorpus', recipes={
                'content.recipe': f'name {fixture.name(part)[0]}-content\nmatch {filename}\n' + body})
            sources.append((name, root / filename, rejected, corrected, diagnostic))
        fixture.forget()

        def ingest(name, expected):
            result = fixture.run('ingest', '-j', '4', name, check=False, timeout=300, env={'LAPLACE_INGEST_ONE': '1'})
            output = result.stdout + result.stderr
            if result.returncode != expected:
                raise SystemExit(f'failed-file-replay: ingest {name} exited {result.returncode}, not {expected}\n{output}')
            return output

        for name, path, rejected, corrected, diagnostic in sources:
            path.write_text(rejected, encoding='utf-8', newline='\n')
            for attempt in range(2):
                output = ingest(name, 1)
                if diagnostic not in output:
                    raise SystemExit(f'failed-file-replay: {name} attempt {attempt}: no {diagnostic}\n{output}')
                if fixture.trunks(name):
                    raise SystemExit(f'failed-file-replay: {name}: a rejected replay published its source')
            path.write_text(corrected, encoding='utf-8', newline='\n')
            ingest(name, 0)
            trunks = fixture.trunks(name)
            if len(trunks) != 1:
                raise SystemExit(f'failed-file-replay: {name}: {len(trunks)} trunks recorded after correction, not 1')
            query = ('SELECT jsonb_agg(to_jsonb(s) ORDER BY claim,dependence_root)'
                     ' FROM laplace_contained_series(' + ids(trunks) + ') s')
            before = sql(query)
            if before in ('', 'null'):
                raise SystemExit(f'failed-file-replay: {name}: the corrected reading published no testimony')
            replay = ingest(name, 0)
            if not re.search(r'which say nothing again: 1\b', replay) or sql(query) != before:
                raise SystemExit(f'failed-file-replay: {name}: the accepted file lost its exact cached replay\n{replay}')
    print('failed-file-replay: rejected replays failed twice without a witness; corrected readings published and replayed exactly (2 cases)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
