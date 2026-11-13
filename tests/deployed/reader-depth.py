"""The production text/parts reader over rows in the deployed database: a 4,096-tier chain, a 100,000-long run,
role-annotated vertices, 32 batched display prefixes, duplicates, missing IDs and virtual scalars; and a corrupt
cycle rejected explicitly ("reader: cyclic composition").

Scope: the check program (reader_depth, from LAPLACE_CHECK_BIN) composes every ID, coordinate and path with Native,
writes the rows into public.entity/physicality inside one transaction on its own connection, reads them back with the
production reader through that connection, and rolls the transaction back. The cycle run corrupts one of those
uncommitted rows and the reader exits on it, so the server aborts the transaction: nothing of either run is ever
committed or visible to another session, and no whole table is read.
"""
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check  # noqa: E402


def main():
    binary = Path(os.environ.get('LAPLACE_CHECK_BIN', '')) / 'reader_depth'
    if not binary.is_file():
        raise SystemExit('LAPLACE_CHECK_BIN must hold the reader_depth check program')
    tier0 = os.environ.get('LAPLACE_TIER0') or sys.exit('LAPLACE_TIER0 must name the installed tier-0 table')
    command = [str(binary), check.connection(), tier0]
    result = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if result.returncode:
        raise SystemExit('reader_depth failed\n' + result.stdout + result.stderr)
    result = subprocess.run([*command, '--cycle'], capture_output=True, text=True, timeout=300)
    if result.returncode == 0 or 'reader: cyclic composition' not in result.stderr:
        raise SystemExit('a corrupt cycle was not rejected explicitly\n' + result.stdout + result.stderr)
    print('reader: 4096 tiers, runs, roles, batched frontiers and a rejected cycle over uncommitted rows of the deployed database')
    return 0


if __name__ == '__main__':
    sys.exit(main())
