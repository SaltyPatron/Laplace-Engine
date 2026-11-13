"""Unicode property ranges keep every codepoint identity, surrogates included: range endpoints keep their canonical
identity and order, a surrogate is never replaced by its hex spelling, a surrogate block boundary is published as an
address (a claim the source attests), an index entry keeps its non-scalar address, and a declared codepoint sequence
keeps a surrogate's bytes.

Scope: one check source (check-unicode-range-addresses-ucd), the stock unicode recipes copied under the check's
witness (without the real source's `tier0` line), over a fixture UCD root. The deployed database's own unicode source
holds the same content-addressed rows, so every assertion requires the row under the check's trunk (check.closure)
or the claim in laplace_contained_series under it, never merely somewhere in a table. The original's PL/pgSQL loop over
the ranges is one set-oriented query.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, closure, ids, sql  # noqa: E402

UCD_XML = '''<ucd xmlns="http://www.unicode.org/ns/2003/ucd/1.0">
  <blocks>
    <block first-cp="0000" last-cp="007F" name="Basic Latin"/>
    <block first-cp="D800" last-cp="DB7F" name="High Surrogates"/>
    <block first-cp="DB80" last-cp="DBFF" name="High Private Use Surrogates"/>
    <block first-cp="DC00" last-cp="DFFF" name="Low Surrogates"/>
  </blocks>
</ucd>
'''


def main():
    with Fixture('unicode-range-addresses') as fixture:
        latest = fixture.data / 'UCD/Public/UCD/latest'
        name, root = fixture.source('ucd', 'class StandardsDerived', copy_from='unicode', root=latest)
        (root / 'idna').mkdir(parents=True, exist_ok=True)
        # The mixed range is the actual Idna2008.txt row. Both-address and singleton cases exercise the same declared
        # range field; U+0000 remains a real scalar.
        (root / 'idna/Idna2008.txt').write_text(
            '0000..002C ; DISALLOWED\nD800..FA0D ; DISALLOWED\nD800..DFFF ; DISALLOWED\nD800 ; DISALLOWED\n',
            encoding='utf-8', newline='\n')
        (root / 'ucd.all.flat.xml').write_text(UCD_XML, encoding='utf-8', newline='\n')
        (root / 'Index.txt').write_text('High Surrogates\tD800\nLATIN CAPITAL LETTER A\t0041\n', encoding='utf-8', newline='\n')
        fixture.forget()
        fixture.run('ingest', '-j', '2', name)
        trunks = fixture.trunks(name)
        if len(trunks) != 1:
            raise SystemExit(f'unicode-range-addresses: {len(trunks)} trunks recorded for {name}, not 1; publication failed')
        under = ids(closure(trunks))
        found = sql(f"""
SELECT 'lost|'||count(*) FROM (VALUES (0,44),(55296,64013),(55296,57343)) r(a,b)
 WHERE NOT EXISTS (SELECT FROM physicality p
   WHERE p.entity=laplace_compose(ARRAY[laplace_cp_id(r.a),laplace_cp_id(r.b)]) AND p.entity=ANY({under})
     AND ST_NPoints(p.path)=2
     AND (laplace_vertex_ids(ST_PointN(p.path,1)))[1]=laplace_cp_id(r.a)
     AND (laplace_vertex_ids(ST_PointN(p.path,2)))[1]=laplace_cp_id(r.b));
SELECT 'hex|'||(laplace_id('D800')=ANY({under}));
SELECT 'boundary|'||EXISTS (SELECT FROM laplace_contained_series({ids(trunks)})
 WHERE claim=laplace_compose(ARRAY[laplace_id('High Surrogates'),laplace_id('Start Code'),laplace_cp_id(55296)]));
SELECT 'index|'||EXISTS (SELECT FROM physicality
 WHERE entity=laplace_compose(ARRAY[laplace_id('High Surrogates'),laplace_cp_id(55296)]) AND entity=ANY({under}));
""")
        seen = dict(line.split('|', 1) for line in found.splitlines() if '|' in line)
        if seen.get('lost') != '0':
            raise SystemExit(f'unicode-range-addresses: {seen.get("lost")} range endpoints lost their canonical identity or order')
        if seen.get('hex') != 'f':
            raise SystemExit('unicode-range-addresses: a surrogate codepoint was replaced by its hex spelling')
        if seen.get('boundary') != 't':
            raise SystemExit('unicode-range-addresses: the surrogate block boundary was omitted instead of published as an address')
        if seen.get('index') != 't':
            raise SystemExit('unicode-range-addresses: the index entry lost its non-scalar address')

        # A declared codepoint sequence preserves a surrogate through reading.
        surrogate = fixture.scratch / 'surrogate'
        surrogate.mkdir()
        (surrogate / 'NamedSequences.txt').write_text('SURROGATE CODEPOINT; D800\n', encoding='utf-8', newline='\n')
        result = fixture.run('ingest', '--claims-tsv', '-s', name, surrogate / 'NamedSequences.txt', check=False)
        if bytes.fromhex('eda080') not in (result.stdout.encode('utf-8', 'surrogateescape') + result.stderr.encode('utf-8', 'surrogateescape')):
            raise SystemExit('unicode-range-addresses: surrogate codepoint bytes were lost')
    print('unicode-range-addresses: exact endpoints, surrogate identities and the published witness passed under the check\'s trunk')
    return 0


if __name__ == '__main__':
    sys.exit(main())
