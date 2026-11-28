#!/usr/bin/env bash
# Transport windows must preserve the compiled XML scope stream and its claims.
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/record-framing.sh path/to/laplace}
here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/record-framing.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_DATA=$scratch/data
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
file=$scratch/ucd.all.flat.xml
# Quotes, references, comments, scopes and UTF-8 all cross the small windows.
{
  printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<ucd xmlns="http://www.unicode.org/ns/2003/ucd/1.0">' '<!-- packaging -->' '<blocks>'
  for ((i=0; i<80; i++)); do
    printf '<block first-cp="%04X" last-cp="%04X" name="Block %s &amp; Ω"/>\n' "$((i*16))" "$((i*16+15))" "$i"
  done
  printf '%s\n' '</blocks>' '<repertoire>' \
    '<char cp="3400" kSemanticVariant="U+4E18"/>' '</repertoire>' '</ucd>'
} > "$file"
read_claims(){
  LAPLACE_RECORD_WINDOW_BYTES=$1 "$L" ingest --claims -j 2 -s unicode "$file" > "$scratch/$1.log" 2> "$scratch/$1.err"
  sed -n '/^\[/p' "$scratch/$1.log" | LC_ALL=C sort > "$scratch/$1.claims"
  grep -q 'unresolved obligations: 0 ' "$scratch/$1.log"
}
read_claims 65536
[[ -s $scratch/65536.claims ]]
grep -q 'semantic variant' "$scratch/65536.claims"
for window in 97 257 4096; do
  read_claims "$window"
  cmp "$scratch/65536.claims" "$scratch/$window.claims"
done
# Recovery locates boundaries only. Invalid lexical content and mismatched
# scope names must still fail the strict parser / compiled scope validation.
cp "$file" "$scratch/valid.xml"
for change in 's/last-cp="000F"/last-cp="000F/' 's#</blocks>#</wrong>#' 's#</ucd>##'; do
  sed "$change" "$scratch/valid.xml" > "$file"
  if LAPLACE_RECORD_WINDOW_BYTES=97 "$L" ingest --no-load -j 2 -s unicode "$file" > "$scratch/invalid.log" 2>&1; then
    cat "$scratch/invalid.log"
    echo 'record-framing: malformed XML was accepted' >&2
    exit 1
  fi
done
# Parallel XML scope bridging (>256 KB single-root document whose closing tag
# straddles the 262144-byte partial bridge span, e.g. FrameNet lu10032.xml).
big=$scratch/lu_bridge_boundary.xml
{
  printf '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
  printf '<?xml-stylesheet type="text/xsl" href="lexUnit.xsl"?>\n'
  printf '<lexUnit POS="V" name="rehearse.v" ID="10032" frame="Practice" frameID="962">\n'
  printf '  <definition>practise for performance</definition>\n'
  printf '  <lexeme POS="V" name="rehearse"/>\n'
  # Pad to ~262256 bytes so start_byte + 262144 lands inside the closing </lexUnit> tag.
  pad=$(printf '%*s' 120 '' | tr ' ' 'x')
  for ((i=0; i<1930; i++)); do
    printf '  <!-- %04d %s -->\n' "$i" "$pad"
  done
  printf '</lexUnit>'
} > "$big"
"$L" ingest --no-load -j 4 -s framenet "$big" > "$scratch/big.log" 2>&1
grep -q 'mismatched 0' "$scratch/big.log"
echo 'record-framing: window-independent claims, bridge-boundary XML and strict malformed-input rejection passed'
