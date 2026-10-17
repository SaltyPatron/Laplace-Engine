#!/bin/bash
# What a recipe does not read is said, never passed over (Laplace-Engine#70): a named part the recipe neither disposes
# of nor names in any of its lines is left open, counted and named when the file ends, wherever it stands in the
# part: a member of a counted group (wndb.5's f_num and w_num) as much as a field. A layout's names are no reading of
# them; a part of a part the recipe reads (a KEY of KEY>VALUE inside a field that is content) is read with it; an
# omitted part is disposed of; each part is counted once.
#   tests/left-open.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/left-open.sh path/to/laplace}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/left-open.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch LAPLACE_RECIPES=$scratch/recipes
fail(){ echo "left-open: $*" >&2; exit 1; }
mkdir -p "$scratch/recipes/sample" "$scratch/data/sample"
printf 'name sample\nwitness the sample\nlineage sample\nclass AcademicCurated\nroot $LAPLACE_DATA/sample\n' > "$scratch/recipes/sample/source"
cat > "$scratch/recipes/sample/rows.recipe" <<'EOF'
name sample-rows
match rows.tsv
tier row by \n
names word n pairs dropped
counted n group first second
tier field by \t
part pairs by space is >
thing row word
content word first pairs
relation "the word's pairs" pairs
attest row pairs
omit n dropped
EOF
printf 'ice\t2\tcold\t1\twet\t2\ta>b c>d\tx\nfire\t1\thot\t3\te>f\tz\n' > "$scratch/data/sample/rows.tsv"
out=$(LAPLACE_DATA=$scratch/data "$L" ingest --claims -s sample "$scratch/data/sample/rows.tsv" 2>&1 | tr -d '\r') || fail "rows.tsv did not read: $out"
grep -q "3 named parts of .*rows.tsv have no disposition in the recipe: they are left open, not read: second$" <<<"$out" \
  || fail "second (a member of a counted group the layout alone names) is not said to be left open, each once: $out"
grep -q '^\[ice, \[c, d\]\]$' <<<"$out" || fail "the pairs are not read: $out"
echo "left-open: what no line of a recipe reads is said to be left open"
