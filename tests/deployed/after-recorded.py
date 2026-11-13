"""`after X` orders a run and asks that X be in; it is no request to read X again. A source named whose X is recorded
already (a witness whose trunk begins with X's declared name) passes X over without opening one of its files; one whose
X is not recorded takes X in first, as a chain run does; and X named itself is ingested as asked.

Two sources of its own: A, which reads LinkEmail.txt (unicode's link-email recipe, renamed) and so attests, and B, of
one such file, which says `after A`. What is asserted is the program's report and its per-source logs; the database is
read only to find A's trunk. A real source is never the prerequisite.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture  # noqa: E402

CHECK = 'after-recorded'


def main():
    with Fixture(CHECK) as fx:
        link = (fx.recipes / 'unicode/link-email.recipe').read_text(encoding='utf-8')

        def recipe(part):
            return re.sub(r'^name .*$', f'name {CHECK}-{part}-link-email', link, count=1, flags=re.M)
        a, a_root = fx.source('a', 'class StandardsDerived', recipes={'link-email.recipe': recipe('a')})
        (a_root / 'LinkEmail.txt').write_text('E000\nE001\nE000\n', encoding='utf-8', newline='\n')
        b, b_root = fx.source('b', f'class StandardsDerived\nafter {a}', recipes={'link-email.recipe': recipe('b')})
        (b_root / 'LinkEmail.txt').write_text('E002\nE003\n', encoding='utf-8', newline='\n')
        fx.forget()
        logs = fx.work / 'logs/ingest'

        def line(output, source, status):
            return re.search(rf'^[0-9]+ +{re.escape(source)} +{status}', output, re.M)

        # Nothing recorded: B named takes A in first.
        first = fx.run('ingest', '-j', '2', b).stdout
        assert (logs / f'{a}.log').is_file() and (logs / f'{a}.log').stat().st_size, \
            f'{a}, not recorded, was not taken in before {b}: {first}'
        assert line(first, a, 'in '), f'{a} did not go in first: {first}'
        assert line(first, b, 'in '), f'{b} did not go in: {first}'
        assert fx.trunks(a), f'{a} went in and recorded no witness'

        # A recorded: B named again passes A over, unread. Its log is not written, so none of its files was opened.
        (logs / f'{a}.log').unlink(missing_ok=True)
        (logs / f'{b}.log').unlink(missing_ok=True)
        second = fx.run('ingest', '-j', '2', b).stdout
        assert not (logs / f'{a}.log').exists(), f'{a}, recorded, was ingested again: {(logs / f"{a}.log").read_text()}'
        assert line(second, a, r'recorded .*its files not read'), f'{a} was not said recorded: {second}'
        assert line(second, b, 'in '), f'{b} did not go in after its recorded prerequisite: {second}'
        assert (logs / f'{b}.log').is_file() and (logs / f'{b}.log').stat().st_size, f'{b} was not ingested'

        # A named itself is ingested as asked, recorded or not.
        fx.run('ingest', '-j', '2', a, b)
        assert (logs / f'{a}.log').is_file() and (logs / f'{a}.log').stat().st_size, f'{a}, named, was not ingested'
    print('after-recorded: a recorded prerequisite is passed over unread; an unrecorded one, or one named, is ingested')
    return 0


if __name__ == '__main__':
    sys.exit(main())
