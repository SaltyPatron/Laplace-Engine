#!/bin/bash
# A source's files may name directories, walked through the ignore files the source names (Laplace-Engine#61): a
# workspace's repositories are read as their authors keep them, and what their .gitignore files leave out (builds,
# dependencies, local secrets) is not read. Patterns follow git: a trailing / names directories, a leading / anchors to
# the ignore file's directory, ! takes a file back, a nested ignore file adds to its parent's, hidden entries are kept out.
#   tests/workspace-code.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/workspace-code.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/workspace-code.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_SRC=$scratch/src
fail(){ echo "workspace-code: $*" >&2; exit 1; }

w=$LAPLACE_SRC/Laplace; mkdir -p "$w"/{build,sub,.hidden,docs/build} "$LAPLACE_SRC/Laplace-MCP"
printf 'build/\n*.log\n!keep.log\n/top.txt\n' > "$w/.gitignore"; printf 'secret*\n' > "$w/sub/.gitignore"
for f in a.c build/x.c sub/a.log keep.log top.txt sub/top.txt .hidden/x.c sub/secret.py sub/b.py docs/build/y.md docs/z.md; do
  printf 'int x;\n' > "$w/$f"
done
printf 'x = 1\n' > "$w/sub/b.py"
printf '# readme\n' > "$LAPLACE_SRC/Laplace-MCP/README.md"

out=$(LAPLACE_TRUNKS=1 LAPLACE_INGEST_ONE=1 "$L" ingest --no-load workspace-code 2>&1) || fail "the ingest failed: $out"
got=$(grep -o "$LAPLACE_SRC/[^ ]*" <<<"$out" | sed "s|$LAPLACE_SRC/||" | LC_ALL=C sort | tr '\n' ' ')
want="Laplace-MCP/README.md Laplace/a.c Laplace/docs/z.md Laplace/keep.log Laplace/sub/b.py Laplace/sub/top.txt "
[ "$got" = "$want" ] || fail "read [$got], not [$want]"
grep -q 'mismatched 0' <<<"$out" || fail "a file did not recompose: $out"
echo "workspace-code: directories walked through their ignore files; ${want// /, }read"
