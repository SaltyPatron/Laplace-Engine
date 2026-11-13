"""Rich text read back from the deployed database: plain and HTML files of one word share one content ID, each of the
four files with content is a file trunk [metadata, content], no stored composition holds a child at its own tier or
above, the retained packaging reads back byte for byte, and an unchanged second ingest finds all five files recorded
by their trunks and changes nothing.

Scope: one check source (check-rich-text-db-inputs, `reads text html`, the stock recipes) over the five files the
original ingested as a bare directory. Every query is constrained by the IDs under the check's trunk
(check.closure): the file trunks, the altitude audit, the packaging readback and the fingerprint. The file trunk's
metadata vertex is found by laplace_path_has_roles (role 4, METADATA, selected by bit 4) rather than by decoding the
path vertex M field by hand.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, closure, fingerprint, ids, sql  # noqa: E402

FILES = {'plain.txt': 'mississippi', 'bold.html': '<b>mississippi</b>', 'split.html': 'mi<b>ss</b>issippi',
         'escaped.html': 'mis&#115;issippi', 'script-only.html': '<script>not visible</script>'}
EXPECTED = ['1', '4', '0', '<b>mississippi</b>', '<script>not visible</script>', 'mi<b>ss</b>issippi', 'mis&#115;issippi']


def main():
    with Fixture('rich-text-db') as fixture:
        name, root = fixture.source('inputs', 'reads text html')
        for file, body in FILES.items():
            (root / file).write_bytes(body.encode('utf-8'))
        fixture.forget()
        env = {'LAPLACE_TRUNKS': '1', 'LAPLACE_INGEST_ONE': '1'}   # in this process: its report on stdout
        fixture.run('ingest', '-j', '2', name, env=env)
        trunks = fixture.trunks(name)
        if len(trunks) != 1:
            raise SystemExit(f'rich-text-db: {len(trunks)} trunks recorded for {name}, not 1')
        under = ids(closure(trunks))
        result = sql(f"""
SELECT count(*) FROM entity WHERE id=laplace_text_id('mississippi') AND id=ANY({under});
SELECT count(*) FROM physicality p WHERE p.entity=ANY({under}) AND p.tier>0
 AND laplace_path_length(p.path)=2
 AND laplace_path_ends(p.path,laplace_text_id('mississippi'))
 AND laplace_path_has_roles(p.path,16::smallint);
SELECT count(*) FROM physicality p JOIN entity e ON e.id=p.entity
 CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) v(id) JOIN entity c ON c.id=v.id
 WHERE p.entity=ANY({under}) AND e.tier>0 AND c.tier>=e.tier;
SELECT string_agg(laplace_text(v.id),E'\\n' ORDER BY laplace_text(v.id) COLLATE "C")
 FROM physicality p CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) v(id)
 WHERE p.entity=ANY({under}) AND p.path @> ARRAY[laplace_text_id('packaging')] AND v.id<>laplace_text_id('packaging');
""")
        if result.splitlines() != EXPECTED:
            raise SystemExit('rich-text-db: unexpected readback\n' + result)
        before = fingerprint(trunks)
        again = fixture.run('ingest', '-j', '2', name, env=env).stdout
        if fingerprint(fixture.trunks(name)) != before:
            raise SystemExit('rich-text-db: an unchanged source changed stored canonical state')
        if not re.search(r'files whose trunk was already recorded, which say nothing again: 5$', again, re.M):
            raise SystemExit('rich-text-db: the unchanged files were not all found recorded by their trunks\n' + again)
    print('rich-text-db: one shared content ID, four file trunks, exact PostgreSQL packaging readback, zero altitude'
          ' violations and unchanged-source pruning passed under the check\'s trunk')
    return 0


if __name__ == '__main__':
    sys.exit(main())
