#!/bin/bash
# `after X` orders a run and asks that X be in; it is no request to read X again. A source named whose X is recorded
# already (a witness whose trunk begins with X's declared name) passes X over without opening one of its files; one
# whose X is not recorded takes X in first, as a chain run does; and X named itself is ingested as asked.
# Ingesting openings after unicode spent 405 s re-reading Unicode to find every file of it recorded.
#   tests/after-recorded.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/after-recorded.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/after-recorded.XXXXXX")
db=laplace_after_$$
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
cleanup(){ "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"; rm -rf "$scratch"; }
trap cleanup EXIT
fail(){ echo "after-recorded: $*" >&2; exit 1; }
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data LAPLACE_RECIPES=$scratch/recipes
unset LAPLACE_INGEST_ONE
logs=$LAPLACE_WORK/logs/ingest
cp -r "$here/../recipes" "$LAPLACE_RECIPES"
# A: unicode, two tiny files and no tier-0 floor. B: a source of one file that comes after unicode.
sed -i '/^tier0$/d' "$LAPLACE_RECIPES/unicode/source"
mkdir -p "$LAPLACE_DATA/UCD/Public/UCD/latest" "$LAPLACE_DATA/after-b" "$LAPLACE_RECIPES/after-b"
printf '0041\n0042\n0041\n' > "$LAPLACE_DATA/UCD/Public/UCD/latest/LinkEmail.txt"
printf '# Provisional named sequences\n' > "$LAPLACE_DATA/UCD/Public/UCD/latest/NamedSequencesProv.txt"
printf '0043\n0044\n' > "$LAPLACE_DATA/after-b/LinkEmail.txt"
printf 'name after-b\nwitness After Recorded Test B\nclass StandardsDerived\nroot $LAPLACE_DATA/after-b\nafter unicode\n' > "$LAPLACE_RECIPES/after-b/source"
sed 's/^name unicode-link-email$/name after-b-link-email/' "$LAPLACE_RECIPES/unicode/link-email.recipe" > "$LAPLACE_RECIPES/after-b/link-email.recipe"
"$L" deploy -d "$conn" > "$scratch/deploy.log" 2>&1 || { cat "$scratch/deploy.log"; exit 1; }

# Nothing recorded: B named takes A in first.
"$L" ingest -d "$conn" -j 2 after-b > "$scratch/first.log" 2>&1 || { cat "$scratch/first.log"; cat "$logs"/*.log; exit 1; }
[ -s "$logs/unicode.log" ] || fail "unicode, not recorded, was not taken in before after-b: $(cat "$scratch/first.log")"
grep -Eq '^[0-9]+ +unicode +in ' "$scratch/first.log" || fail "unicode did not go in first: $(cat "$scratch/first.log")"
grep -Eq '^[0-9]+ +after-b +in ' "$scratch/first.log" || fail "after-b did not go in: $(cat "$scratch/first.log")"

# A recorded: B named again passes A over, unread. Its log is not written, so none of its files was opened.
rm -f "$logs/unicode.log" "$logs/after-b.log"
"$L" ingest -d "$conn" -j 2 after-b > "$scratch/second.log" 2>&1 || { cat "$scratch/second.log"; exit 1; }
[ ! -e "$logs/unicode.log" ] || fail "unicode, recorded, was ingested again: $(cat "$logs/unicode.log")"
grep -Eq '^[0-9]+ +unicode +recorded .*its files not read' "$scratch/second.log" || fail "unicode was not said recorded: $(cat "$scratch/second.log")"
grep -Eq '^[0-9]+ +after-b +in ' "$scratch/second.log" || fail "after-b did not go in after its recorded prerequisite: $(cat "$scratch/second.log")"
[ -s "$logs/after-b.log" ] || fail "after-b was not ingested"

# A named itself is ingested as asked, recorded or not.
"$L" ingest -d "$conn" -j 2 unicode after-b > "$scratch/third.log" 2>&1 || { cat "$scratch/third.log"; exit 1; }
[ -s "$logs/unicode.log" ] || fail "unicode, named, was not ingested: $(cat "$scratch/third.log")"
echo "after-recorded: a recorded prerequisite is passed over unread; an unrecorded one, or one named, is ingested"
