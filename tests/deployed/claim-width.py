"""One complete wide claim read back from the deployed database through the production CLI: an 80-operand claim found
by hop with its middle or last operand given (a match late in the claim, the true last operand), its standing shown
as consensus holds it, held selecting it by all 80 parts; and translate following the true endpoint of a 5,003-operand
lexicalization relation, past the former 4,096-operand buffer.

Scope: the claims are composed by a real ingest of the check's own source (check-claim-width-wide, a tuple a line,
`itself row`), so every row is written by the Engine and taken back out by `laplace forget`. Assertions read only what
the check composed: the claim text in a report, consensus by the claim's own ID, held's rows under the check's trunk,
and translate's output line for the check's lemmas. Other claims the deployed database holds may stand beside them in a
report; none is counted.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, literal, sql  # noqa: E402

PARTS = [chr(1024 + i) for i in range(80)]
EXPECTED = '[' + ', '.join(PARTS) + ']'
WIDE = 'name {name}\nmatch *.wide\ntier row by \\n\nnames op...\ntier field by \\t\nthing row op of\ncontent op\nitself row\n'
# The lexicalization as translate reads it from a recipe: a sense is [lemma, language, concept], and "R" the relation
# a word has to one. It matches no file: the claims above are the senses' claims.
NAVIGATION = 'name {name}\nformat xml\nthing Sense lemma language concept\nrelation "R" Sense\n'


def main():
    with Fixture('claim-width') as fixture:
        navigation = f'{fixture.name("navigation")[0]}'
        name, root = fixture.source('wide', recipes={
            'wide.recipe': WIDE.format(name=fixture.name('wide')[0] + '-rows'),
            'navigation.recipe': NAVIGATION.format(name=navigation)})
        rows = ['\t'.join(PARTS),
                # [A, R, AEC]: A's sense in language E, concept C
                'A\tR\tAEC',
                # [B, R, 5000 operands, BFC]: B's sense in language F, concept C, the sense the 5,003rd operand
                '\t'.join(['B', 'R', *(chr(4096 + i) for i in range(5000)), 'BFC'])]
        (root / 'claims.wide').write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
        fixture.forget()
        fixture.run('ingest', name)
        trunks = fixture.trunks(name)
        if len(trunks) != 1:
            raise SystemExit(f'claim-width: {name} has {len(trunks)} trunks recorded, not 1')

        claim = sql("SELECT laplace_text_id(:'text')::text", variables={'text': ''.join(PARTS)})
        matches = sql('SELECT matches FROM consensus WHERE claim=:\'claim\'::blake3', variables={'claim': claim})
        if not matches:
            raise SystemExit(f'claim-width: the 80-operand claim {claim} has no standing')
        line = re.compile(r'^\s*\S+\s+\S+\s+\S+\s+(\d+)\s+\S*\s+' + re.escape(EXPECTED) + r'\s*$', re.M)
        for selectors in [(PARTS[0], '?', PARTS[-1]), (PARTS[0], PARTS[65], PARTS[-1]), (PARTS[0], PARTS[65], '?')]:
            report = fixture.run('hop', *selectors, env={'OMP_NUM_THREADS': '2'}).stdout
            found = line.search(report)
            if not found or found[1] != matches:
                raise SystemExit(f'claim-width: hop {" ".join(selectors)} does not show the claim with its {matches} matches\n{report}')
        report = fixture.run('hop', PARTS[0], '?', PARTS[11], env={'OMP_NUM_THREADS': '2'}).stdout
        if EXPECTED in report:
            raise SystemExit(f'claim-width: a claim whose last operand is not {PARTS[11]} matched it\n{report}')
        held = fixture.run('held', *PARTS, env={'OMP_NUM_THREADS': '2'}).stdout
        if '1 claims' not in held or not re.search(rf'^\s+{claim}\s+witness\s+{trunks[0]}\s', held, re.M):
            raise SystemExit(f'claim-width: held does not select the claim under the check\'s trunk\n{held}')
        print('claim-width: all 80 operands, the stored match count, late middle match, true last operand and complete positional selection passed')

        firmware = fixture.scratch / 'navigation.firmware'
        firmware.write_text(f'for translate\nlexicalization {navigation} Sense language concept\nfan 12\n',
                            encoding='utf-8', newline='\n')
        result = fixture.run('translate', '--firmware', firmware, 'A', 'E', 'F', env={'OMP_NUM_THREADS': '2'})
        if not re.search(r'^\s+F\s+B\s*$', result.stdout, re.M):
            raise SystemExit(f'claim-width: translate did not reach B through the 5003-operand relation\n{result.stdout}{result.stderr}')
        print('claim-navigation: translation follows the true endpoint of a 5003-operand relation through the shared reader')
    return 0


if __name__ == '__main__':
    sys.exit(main())
