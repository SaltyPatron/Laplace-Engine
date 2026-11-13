"""A source that is a working tree says `supersede` (as workspace-code does): each whole reading of it is what it says
now, and the readings before it are forgotten once it is the witness, as `laplace forget` takes a witness out.

Its own source, a copy of workspace-code's python recipe (and the plain python recipe) under a check name: a file
changed since keeps none of its former content that only the earlier reading held; a file removed since goes; a file
both readings hold stays; an unchanged reading changes nothing. A 2,800-function file crosses a complete Native
registration window with real identifier claims, and the standings of the claims its trunk attests are counted.

Scoped: the source's trunks are found by its own witness record (Fixture.trunks); the standings counted are those of the
claims its trunk attests; what is held is read by `hop` of identifiers put together at run time, so this file, read as
code by workspace-code, holds none of them, and only this source can hold them.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, sql  # noqa: E402

CHECK = 'supersede'


def identifier(*words):
    return '_'.join(('laplace', 'check', 'supersede', *words))


def main():
    alpha, kept, gone, beta = (identifier(*w) for w in (('alpha', 'version', 'one'), ('kept', 'unchanged'),
                                                        ('removed', 'since'), ('beta', 'version', 'two')))
    with Fixture(CHECK) as fx:
        def recipe(path, name, match):
            text = (fx.recipes / path).read_text(encoding='utf-8')
            text = re.sub(r'^name .*$', f'name {name}', text, count=1, flags=re.M)
            return re.sub(r'^match .*$', f'match {match}', text, count=1, flags=re.M)
        source, tree = fx.source('tree', 'supersede', recipes={
            'python.recipe': recipe('workspace-code/python.recipe', f'{CHECK}-python', '*.pyi'),
            'raw.recipe': recipe('python.recipe', f'{CHECK}-raw', '*.py')})
        fx.forget()

        def held(text):
            output = fx.run('hop', text).stdout
            return 'nothing recorded' not in output

        def ingest():
            result = fx.run('ingest', '-j', '2', source, timeout=1800)
            log = fx.work / 'logs/ingest' / f'{source}.log'
            return result.stdout + (log.read_text(encoding='utf-8', errors='replace') if log.exists() else '')

        def standings(trunk):
            return int(sql('SELECT count(*) FROM consensus c WHERE c.claim IN'
                           ' (SELECT a.claim FROM attestation a WHERE a.witness=:\'t\'::blake3)', variables={'t': trunk}))

        (tree / 'a.py').write_text(f'def {alpha}():\n    return 1\n', encoding='utf-8', newline='\n')
        (tree / 'b.py').write_text(f'def {kept}():\n    return 0\n', encoding='utf-8', newline='\n')
        (tree / 'gone.py').write_text(f'def {gone}():\n    return 9\n', encoding='utf-8', newline='\n')
        (tree / 'wide.pyi').write_text(''.join(f'def shared_symbol_{i:04d}():\n    return 0\n' for i in range(2800)),
                                       encoding='utf-8', newline='\n')
        ingest()
        assert held(alpha) and held(gone), 'the first reading is not held'
        first = fx.trunks(source)
        assert len(first) == 1 and re.fullmatch('[0-9a-f]{32}', first[0]), f'the first reading did not record one witness: {first}'
        claims = standings(first[0])
        assert claims >= 8200, f'the wide source lost claims across registration windows: {claims}'

        (tree / 'a.py').write_text(f'def {beta}():\n    return 2\n', encoding='utf-8', newline='\n')
        (tree / 'gone.py').unlink()
        ingest()
        second = fx.trunks(source)
        assert len(second) == 1 and second != first, f'the changed reading did not replace the earlier witness: {first} -> {second}'
        assert held(beta), 'the new content is not held'
        assert held(kept), 'a file both readings hold was taken out'
        claims = standings(second[0])
        assert claims >= 8200, f'supersession lost the wide source\'s standings: {claims}'
        assert not held(alpha), 'a changed file\'s former content is still held'
        assert not held(gone), 'a removed file is still held'

        ingest()
        assert fx.trunks(source) == second, f'an unchanged reading changed the witness: {second} -> {fx.trunks(source)}'
        assert held(beta) and held(kept), 'an unchanged reading took out what it holds'
    print('supersede: a changed file\'s former content and a removed file go with the earlier reading; '
          'a shared file and an unchanged reading stay')
    return 0


if __name__ == '__main__':
    sys.exit(main())
