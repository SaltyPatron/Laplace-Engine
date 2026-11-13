"""Cached key providers: a reading that adds a consumer file finds every unchanged provider file recorded by its content
trunk, restores nothing from metadata, and gives the same source reference as a cold reading.

For 4 and then 12 provider files, a source of its own (one per count) is ingested with its providers (and an anchor
file whose row attests, so that this first release is a witness `laplace forget` takes out), then again with
a consumer file whose rows refer to the providers' keys. That release's trunk ID, tier, Hilbert key and coordinate are
kept; the source is forgotten and ingested cold, and must give the same trunk and the same reference.

Scoped: each source's trunks (Fixture.trunks) and the entity rows of those trunk IDs. The cold reading is the source
forgotten and read again in the one deployed database, not a second database. The earlier check's comparison of
backend session counts (pg_stat_database) between the two provider counts is dropped: the counter is the database's,
and the deployed service's own connections to the same database move it.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, sql  # noqa: E402

CHECK = 'cached-key-reader'


def reference(trunk):
    return sql("SELECT e.id::text||'|'||e.tier||'|'||e.hilbert||'|'||encode(ST_AsEWKB(e.coord),'hex')"
               ' FROM entity e WHERE e.id=:\'t\'::blake3', variables={'t': trunk})


def main():
    with Fixture(CHECK) as fx:
        sources = {}
        for count in (4, 12):
            name, root = fx.source(f'readers-{count}', 'class AcademicCurated', recipes={
                'provider.recipe': f'name {CHECK}-{count}-provider\nmatch provider-*.tsv\ntier row by \\n\n'
                                   'names key text\ntier field by \\t\nthing row text\nkey row key\ncontent text\n',
                'consumer.recipe': f'name {CHECK}-{count}-consumer\nmatch links.tsv\ntier row by \\n\n'
                                   'names subject object\ntier field by \\t\nthing row subject\n'
                                   f'refer subject {CHECK}-{count}-provider\nrefer object {CHECK}-{count}-provider\n'
                                   'relation "links to" object\nattest row object\n',
                # A witness is known when it has attestations, and the providers attest nothing: a file whose row
                # attests makes the providers' release a witness too, so `laplace forget` takes it out with the rest.
                'anchor.recipe': f'name {CHECK}-{count}-anchor\nmatch anchor.tsv\ntier row by \\n\nnames thing value\n'
                                 'tier field by \\t\nthing row thing\ncontent thing value\nrelation "has value" value\n'
                                 'attest row value\n'})
            (root / 'anchor.tsv').write_text(f'Laplace check cached key reader {count}\tanchor\n', encoding='utf-8', newline='\n')
            sources[count] = (name, root)
        fx.forget()
        for count, (name, root) in sources.items():
            for i in range(count):
                (root / f'provider-{i}.tsv').write_text(f'{i}\tLaplace check cached key {i}.\n', encoding='utf-8', newline='\n')

            def run():
                result = fx.run('ingest', '-j', '2', name)
                log = fx.work / 'logs/ingest' / f'{name}.log'
                return result.stdout + result.stderr + (log.read_text(encoding='utf-8', errors='replace') if log.exists() else '')

            run()
            providers = fx.trunks(name)
            assert len(providers) == 1, f'the providers\' reading did not record one witness: {providers}'
            (root / 'links.tsv').write_text(''.join(f'{i}\t{(i + 1) % count}\n' for i in range(count)),
                                            encoding='utf-8', newline='\n')
            report = run()
            # The unchanged providers (and the anchor) are read and found recorded by their content trunks; nothing is
            # restored from metadata.
            assert re.search(rf'which say nothing again: {count + 1}\b', report), report
            release = [t for t in fx.trunks(name) if t not in providers]
            assert len(release) == 1, f'the reading with the consumer is not one new release: {providers} {fx.trunks(name)}'
            warm = reference(release[0])
            assert warm, 'the warm release has no entity row'
            fx.forget(name)
            assert fx.trunks(name) == [], f'forget left a release of {name}'
            run()
            cold = fx.trunks(name)
            assert cold == release, f'warm and cold readings are different sources: {release} {cold}'
            assert reference(cold[0]) == warm, ('warm and cold source references differ', warm, reference(cold[0]))
            print(f'cached-key-reader: {count} recorded providers', flush=True)
    print('cached-key-reader: unchanged providers found recorded and identical warm/cold canonical source references')
    return 0


if __name__ == '__main__':
    sys.exit(main())
