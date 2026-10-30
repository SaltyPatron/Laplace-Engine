#!/usr/bin/env bash
# A byte frame must leave the generic JSON recipe's claims unchanged, exclude
# the binary tail, and reject malformed input before any source admission.
set -euo pipefail
L=${1:?usage: tests/input-frame.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/input-frame.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_RECIPES=$scratch/recipes
mkdir -p "$scratch/recipes/formats"
cp "$here/../recipes/formats/json.format" "$scratch/recipes/formats/"
cat > "$scratch/recipes/sample.recipe" <<'EOF'
name sample
match *.bin
class AcademicCurated
format frame
tier record by \n
thing record head relation tail
content head relation tail
itself record
EOF
sed 's/\*.bin/*.json/; s/format frame/format json/; s/name sample/name plain/' \
    "$scratch/recipes/sample.recipe" > "$scratch/recipes/plain.recipe"
body='{"head":"dog","relation":"related to","tail":"animal"}'
printf '%s' "$body" > "$scratch/sample.json"
# There must be a resolvable format even when loading the unframed baseline.
printf 'input length-prefix u64le\ninclude json\n' > "$scratch/recipes/formats/frame.format"
"$L" ingest --claims -j 2 "$scratch/sample.json" > "$scratch/plain.log" 2> "$scratch/plain.err"
sed -n '/^\[/p' "$scratch/plain.log" > "$scratch/plain.claims"
grep -qx '\[dog, related to, animal\]' "$scratch/plain.claims"
for encoding in u8 u16le u16be u32le u32be u64le u64be; do
    case $encoding in u8) width=1;; u16*) width=2;; u32*) width=4;; u64*) width=8;; esac
    printf 'input length-prefix %s\ninclude json\n' "$encoding" > "$scratch/recipes/formats/frame.format"
    printf -v count '\\%03o' "${#body}"
    {
        if [[ $encoding == *be ]]; then for ((i=1; i<width; i++)); do printf '\0'; done; fi
        printf '%b' "$count"
        if [[ $encoding != *be ]]; then for ((i=1; i<width; i++)); do printf '\0'; done; fi
        printf '%s' "$body"
        printf '\377\000{"head":"TAIL","relation":"must not","tail":"appear"}'
    } > "$scratch/sample.bin"
    "$L" ingest --claims -j 2 "$scratch/sample.bin" > "$scratch/frame.log" 2> "$scratch/frame.err"
    sed -n '/^\[/p' "$scratch/frame.log" > "$scratch/frame.claims"
    cmp "$scratch/plain.claims" "$scratch/frame.claims"
    "$L" tree -n 5 "$scratch/sample.bin" > "$scratch/tree.log" 2> "$scratch/tree.err"
    grep -q "input section: offset $width, bytes ${#body}" "$scratch/tree.log"
done
# A large payload does not become a parser buffer or a source claim.
truncate -s 8G "$scratch/sample.bin"
"$L" ingest --claims -j 2 "$scratch/sample.bin" > "$scratch/sparse.log" 2> "$scratch/sparse.err"
sed -n '/^\[/p' "$scratch/sparse.log" > "$scratch/sparse.claims"
cmp "$scratch/plain.claims" "$scratch/sparse.claims"
for invalid in short length json; do
    case $invalid in
        short) printf '\0' > "$scratch/sample.bin";;
        length) printf '\377\377\377\377\377\377\377\377{}' > "$scratch/sample.bin";;
        json) printf '\0\0\0\0\0\0\0\003{xx' > "$scratch/sample.bin";;
    esac
    if "$L" ingest --no-load -j 2 "$scratch/sample.bin" > "$scratch/invalid.log" 2>&1; then
        cat "$scratch/invalid.log" >&2; echo "input-frame: accepted invalid $invalid" >&2; exit 1
    fi
done
for declaration in 'input length-prefix native' 'input length-prefix u64le extra' $'input length-prefix u64le\ninput length-prefix u64le'; do
    printf '%s\ninclude json\n' "$declaration" > "$scratch/recipes/formats/frame.format"
    if "$L" tree "$scratch/sample.bin" > "$scratch/invalid-recipe.log" 2>&1; then
        cat "$scratch/invalid-recipe.log" >&2; echo 'input-frame: accepted invalid framing declaration' >&2; exit 1
    fi
    grep -q 'input length-prefix' "$scratch/invalid-recipe.log"
done
echo 'input-frame: generic claims agree for seven encodings; binary tails and malformed frames excluded'
