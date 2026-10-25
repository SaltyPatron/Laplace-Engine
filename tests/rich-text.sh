#!/usr/bin/env bash
# Exercise the production recipe/compiler/Native path, preserving source bytes
# while requiring canonical content to cross inline formatting boundaries.
set -euo pipefail
L=${1:?usage: tests/rich-text.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/rich-text.XXXXXX")
cleanup(){
  local target base; target=$(cd "$scratch" && pwd -P); base=$(cd "${TMPDIR:-/tmp}" && pwd -P)
  case "$target" in "$base"/rich-text.*) rm -rf -- "$target";; *) return 1;; esac
}
trap cleanup EXIT
mkdir "$scratch/inputs"
export LAPLACE_WORK=$scratch/work
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
export LAPLACE_TRUNKS=1
printf 'mississippi' > "$scratch/inputs/word.txt"
printf 'mississippi' > "$scratch/inputs/identifier.js"
printf '<b>mississippi</b>' > "$scratch/inputs/bold.html"
printf 'mi<b>ss</b>issippi' > "$scratch/inputs/split.html"
printf 'mis&#115;issippi' > "$scratch/inputs/decimal.html"
printf 'mis&#X73;issippi' > "$scratch/inputs/hex.html"
printf 'mi<span title="different packaging">ss</span>issippi' > "$scratch/inputs/attribute.html"
printf 'mi<!-- comment -->ssissippi<script>not content</script><style>not content</style>' > "$scratch/inputs/hidden.html"
printf '≂̸ € �' > "$scratch/inputs/references.txt"
printf '&NotEqualTilde; &#128; &#0;' > "$scratch/inputs/references.html"
printf 'alpha\nbeta' > "$scratch/inputs/paragraphs.txt"
printf '<p>alpha</p><p>beta</p>' > "$scratch/inputs/paragraphs.html"
printf 'alpha<br/>beta' > "$scratch/inputs/break.html"
"$L" ingest --entities --no-load -j 2 -d 'host=127.0.0.1 port=1 connect_timeout=1' "$scratch/inputs" > "$scratch/output.log" 2>&1 || { cat "$scratch/output.log"; exit 1; }
trunk(){ sed -n "s/.*trunk \([0-9a-f]\{32\}\).*\/$1.*/\1/p" "$scratch/output.log" | tail -n 1; }
word=$(trunk word.txt)
[[ ${#word} == 32 ]] || { cat "$scratch/output.log"; exit 1; }
[[ $(trunk identifier.js) == "$word" ]] || { echo 'rich-text: code identifier changed shared content identity'; cat "$scratch/output.log"; exit 1; }
for file in bold split decimal hex attribute hidden; do
  [[ $(trunk "$file.html") == "$word" ]] || { echo "rich-text: $file did not converge on $word"; cat "$scratch/output.log"; exit 1; }
done
for pair in references paragraphs; do
  [[ $(trunk "$pair.html") == "$(trunk "$pair.txt")" ]] || { echo "rich-text: $pair differs"; cat "$scratch/output.log"; exit 1; }
done
[[ $(trunk break.html) == "$(trunk paragraphs.txt)" ]] || { echo 'rich-text: explicit line break differs'; cat "$scratch/output.log"; exit 1; }
grep -q 'mismatched 0' "$scratch/output.log"
echo 'rich-text: inline boundaries, attributes, references, excluded content, paragraph boundaries and exact packaging reconstruction passed'
