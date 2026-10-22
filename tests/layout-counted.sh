#!/bin/bash
# A layout's counted groups (Laplace-Engine#70): a part that is a count, in any radix, of the groups that follow it,
# each group the next parts under the member names the layout gives, the names after the count going on after the last
# group; and a remark kept as a named text (wndb.5's gloss after |). Read by `laplace structure`, which prints the tree
# its compiled Tree-sitter grammar projects from a file.
#   tests/layout-counted.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/layout-counted.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
fail(){ echo "layout-counted: $*" >&2; exit 1; }
out=$("$L" structure "$here/layout/wndb.layout" "$here/layout/data.sample" -n 1000 | tr -d '\r')
row(){ awk -v n="-- $1" '$0==n{on=1;next} /^-- /{on=0} on' <<<"$out"; }
count(){ grep -c -- "$1" <<<"$2" || true; }

[ "$(grep -c '^-- ' <<<"$out")" = 3 ] || fail "the license line (a comment) is a row, or a row is missing: $out"
r0=$(row 0); r1=$(row 1); r2=$(row 2)
# radix 16, one word; radix 10, three pointers, each its four members in order; no f_cnt written: none read
[ "$(count '^  group word$' "$r0")" = 1 ] || fail "row 0: not one word group: $r0"
[ "$(count '^  group pointer$' "$r0")" = 3 ] || fail "row 0: not three pointer groups: $r0"
[ "$(grep -A4 -m1 '^  group pointer$' <<<"$r0" | sed -n '2,5p' | awk '{print $2}' | tr '\n' ' ')" = "pointer_symbol offset pos source_target " ] || fail "row 0: a pointer's members out of order: $r0"
grep -q '^  value p_cnt  "003"$' <<<"$r0" || fail "row 0: the name after the counted words is not p_cnt: $r0"
! grep -q 'f_cnt' <<<"$r0" || fail "row 0: an f_cnt the line does not write: $r0"
grep -q '^  value gloss  "that which is perceived or known"$' <<<"$r0" || fail "row 0: the kept remark is not the gloss, trimmed: $r0"
! grep -q '^  text  ""$' <<<"$r0" || fail "row 0: the space before the remark made an empty part: $r0"
# radix 16: 0a is ten words; the names go on after them (p_cnt, then the frames f_cnt counts)
[ "$(count '^  group word$' "$r1")" = 10 ] || fail "row 1: 0a in radix 16 is not ten words: $r1"
grep -q '^    value word  "w10"$' <<<"$r1" || fail "row 1: the tenth word is not read as a word: $r1"
[ "$(count '^  group pointer$' "$r1")" = 1 ] || fail "row 1: not one pointer: $r1"
[ "$(count '^  group frame$' "$r1")" = 2 ] || fail "row 1: not two frames: $r1"
grep -q '^    value w_num  "01"$' <<<"$r1" || fail "row 1: the second frame's w_num: $r1"
# A valid non-verb record still enforces its hexadecimal word count and zero pointers.
[ "$(count '^  group word$' "$r2")" = 1 ] || fail "row 2: not one adjective word: $r2"
[ "$(count '^  group pointer$' "$r2")" = 0 ] || fail "row 2: unexpected pointers: $r2"
grep -q '^  value p_cnt  "000"$' <<<"$r2" || fail "row 2: the pointer count is misplaced: $r2"

# The compiled grammar rejects malformed counts and truncated counted groups.
invalid=$(mktemp "${TMPDIR:-/tmp}/layout-record.XXXXXX")
for change in 's/a 01 alone/a zz alone/' 's/n 01 entity/n 02 entity/' 's/003 ~/004 ~/'; do
  sed "$change" "$here/layout/data.sample" > "$invalid"
  set +e; o=$("$L" structure "$here/layout/wndb.layout" "$invalid" 2>&1); c=$?; set -e
  [ "$c" = 2 ] && grep -q 'does not parse completely' <<<"$o" || { rm -f "$invalid"; fail "malformed counted record accepted (exit $c): $o"; }
done
rm -f "$invalid"

# a radix out of 2..36, or a group with no members, is refused
bad=$(mktemp "${TMPDIR:-/tmp}/layout-counted.XXXXXX"); trap 'rm -f "$bad"' EXIT
printf 'tier row by \\n\nnames n\ncounted n radix 37 g a\ntier field by space\n' >"$bad"
set +e; o=$("$L" structure "$bad" "$here/layout/data.sample" 2>&1); c=$?; set -e
[ "$c" = 2 ] && grep -q 'radix 2 to 36' <<<"$o" || fail "radix 37 not refused (exit $c): $o"
printf 'tier row by \\n\nnames n\ncounted n g\ntier field by space\n' >"$bad"
set +e; o=$("$L" structure "$bad" "$here/layout/data.sample" 2>&1); c=$?; set -e
[ "$c" = 2 ] || fail "a counted group with no members not refused (exit $c): $o"
echo "layout-counted: ok"
