"""The client presence query (db_recorded over laplace_present_ids) on one frontier of the whole Native floor, the
check's own stored IDs each sent twice, and random 128-bit values: every stored ID and its duplicate is found, no
random value is, and every floor answer agrees with the stored rows.

Scope: one check source (check-presence-frontier-input, one sentence read as text) is ingested; the IDs sent as
stored are those under its trunk (check.closure). The original required the whole floor absent, true of a freshly
deployed private database; the deployed database may hold codepoint rows written by its unicode source, and
db_recorded promises nothing for tier 0 beyond what is stored, so the check program (presence_frontier, from
LAPLACE_CHECK_BIN) compares each floor answer with an independent indexed probe of public.entity by the floor's IDs.
"""
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import Fixture, closure, connection, ids, sql  # noqa: E402


def main():
    binary = Path(os.environ.get('LAPLACE_CHECK_BIN', '')) / 'presence_frontier'
    if not binary.is_file():
        raise SystemExit('LAPLACE_CHECK_BIN must hold the presence_frontier check program')
    tier0 = os.environ.get('LAPLACE_TIER0') or sys.exit('LAPLACE_TIER0 must name the installed tier-0 table')
    with Fixture('presence-frontier') as fixture:
        name, root = fixture.source('input', 'reads text')
        (root / 'input.txt').write_text('Presence qualification with committed canonical higher references.\n',
                                        encoding='utf-8', newline='\n')
        fixture.forget()
        fixture.run('ingest', '-j', '2', name)
        trunks = fixture.trunks(name)
        if len(trunks) != 1:
            raise SystemExit(f'presence-frontier: {len(trunks)} trunks recorded for {name}, not 1')
        stored = sql('SELECT id::text FROM entity WHERE id=ANY(' + ids(closure(trunks)) + ') AND tier>0 ORDER BY 1').split()
        if not stored:
            raise SystemExit('presence-frontier: nothing stored under the check\'s trunk')
        listed = fixture.scratch / 'stored.ids'
        listed.write_text(''.join(i + '\n' for i in stored), encoding='ascii', newline='\n')
        result = subprocess.run([str(binary), connection(), tier0, str(listed)], capture_output=True, text=True, timeout=900)
        if result.returncode:
            raise SystemExit('presence-frontier failed\n' + result.stdout + result.stderr)
        print(result.stdout.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
