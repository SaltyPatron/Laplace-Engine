#!/bin/bash
# Unicode property ranges retain non-scalar addresses as content, not atoms.
set -euo pipefail
L=${1:?usage: tests/unicode-range-addresses.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:?}/unicode-range.XXXXXX")
db=laplace_unicode_range_$$
psql=${PSQL:-psql}
dropdb=$(dirname "$(command -v "$psql")")/dropdb
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
cleanup(){ "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db"; rm -r "$scratch"; }
trap cleanup EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data LAPLACE_RECIPES=$scratch/recipes
cp -r "$here/../recipes" "$LAPLACE_RECIPES"
sed -i '/^tier0$/d' "$LAPLACE_RECIPES/unicode/source"
root=$LAPLACE_DATA/UCD/Public/UCD/latest/idna
mkdir -p "$root"
# The mixed range is the actual Idna2008.txt row. Both-address and singleton
# cases exercise the same declared range field; U+0000 remains a real scalar.
printf '%s\n' '0000..002C ; DISALLOWED' 'D800..FA0D ; DISALLOWED' \
    'D800..DFFF ; DISALLOWED' 'D800 ; DISALLOWED' > "$root/Idna2008.txt"
cat > "$LAPLACE_DATA/UCD/Public/UCD/latest/ucd.all.flat.xml" <<'XML'
<ucd xmlns="http://www.unicode.org/ns/2003/ucd/1.0">
  <blocks>
    <block first-cp="0000" last-cp="007F" name="Basic Latin"/>
    <block first-cp="D800" last-cp="DB7F" name="High Surrogates"/>
    <block first-cp="DB80" last-cp="DBFF" name="High Private Use Surrogates"/>
    <block first-cp="DC00" last-cp="DFFF" name="Low Surrogates"/>
  </blocks>
</ucd>
XML
printf 'High Surrogates\tD800\nLATIN CAPITAL LETTER A\t0041\n' > "$LAPLACE_DATA/UCD/Public/UCD/latest/Index.txt"
"$L" deploy -d "$conn" > "$scratch/deploy.log" 2>&1 || { cat "$scratch/deploy.log"; exit 1; }
"$L" ingest -d "$conn" -j 2 unicode > "$scratch/ingest.log" 2>&1 || {
    cat "$scratch/ingest.log"; cat "$LAPLACE_WORK/logs/ingest/unicode.log"; exit 1;
}
"$psql" -Xw -v ON_ERROR_STOP=1 -d "$conn" <<'SQL'
DO $$
DECLARE expected blake3[];
BEGIN
    FOR expected IN SELECT parts FROM (VALUES
        (ARRAY[laplace_cp_id(0),laplace_cp_id(44)]),
        (ARRAY[laplace_id('D800'),laplace_cp_id(64013)]),
        (ARRAY[laplace_id('D800'),laplace_id('DFFF')])) AS ranges(parts)
    LOOP
        IF NOT EXISTS (SELECT FROM public.physicality
            WHERE entity=laplace_compose(expected) AND ST_NPoints(path)=2
              AND (laplace_vertex_ids(ST_PointN(path,1)))[1]=expected[1]
              AND (laplace_vertex_ids(ST_PointN(path,2)))[1]=expected[2]) THEN
            RAISE EXCEPTION 'range endpoints lost their canonical identity or order: %',expected;
        END IF;
    END LOOP;
    IF NOT EXISTS (SELECT FROM public.entity WHERE id=laplace_id('D800'))
       OR EXISTS (SELECT FROM public.entity WHERE id=laplace_cp_id(55296))
       OR NOT EXISTS (SELECT FROM public.witness) THEN
        RAISE EXCEPTION 'non-scalar address became an atom, or source publication failed';
    END IF;
    IF NOT EXISTS (SELECT FROM public.consensus WHERE claim=laplace_compose(ARRAY[
        laplace_id('High Surrogates'),laplace_id('Start Code'),laplace_id('D800')])) THEN
        RAISE EXCEPTION 'the surrogate block boundary was omitted instead of published as an address';
    END IF;
    IF NOT EXISTS (SELECT FROM public.physicality WHERE entity=laplace_compose(ARRAY[
        laplace_id('High Surrogates'),laplace_id('D800')])) THEN
        RAISE EXCEPTION 'the index entry lost its non-scalar address';
    END IF;
END $$;
SQL
# A sequence explicitly declared as scalar codepoints still rejects a surrogate.
mkdir -p "$scratch/invalid"
printf 'BAD SURROGATE; D800\n' > "$scratch/invalid/NamedSequences.txt"
if "$L" ingest --no-load -s unicode "$scratch/invalid/NamedSequences.txt" > "$scratch/invalid.log" 2>&1; then
    echo 'a surrogate was accepted as scalar sequence content' >&2; exit 1
fi
grep -q 'invalid scalar content' "$scratch/invalid.log"
echo 'unicode-range-addresses: exact range endpoints, scalar floor and published witness passed'
