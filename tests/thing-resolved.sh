#!/bin/bash
# A thing named by several parts together, resolved: the path of those of them that resolve (Laplace-Engine#32, BRIEF: a
# mapping row is a path over the resolved highway nodes). A part the source leaves empty is in no path and is owed
# nothing; a key no list of the highway holds is left out of the path and is an unresolved obligation, so the row is
# still one thing and what is said of it is still said: no row is lost. A key is counted once for the part it is
# written in, however many of the recipe's lines read it. Read through the PredicateMatrix recipe, whose rows are such
# paths, on a fixture whose node keys are none any version of the resources writes (pb:nosuch.99).
#   tests/thing-resolved.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/thing-resolved.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/thing-resolved.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "thing-resolved: $*" >&2; exit 1; }
out=$(LAPLACE_DATA=$scratch "$L" ingest --claims -s predicate-matrix "$here/thing-resolved/PredicateMatrix.v1.3.txt" 2>&1 | tr -d '\r') || fail "ingest failed: $out"
count(){ grep -c -- "$1" <<<"$out" || true; }
# every row is a thing, and what is said of it is said: three rows, three predicates
for p in first second third; do
  [ "$(count "the predicate, id:$p.1.default\]")" = 1 ] || fail "row $p: its predicate is not said of it (the row was lost): $out"; done
# the path holds what resolves, in the recipe's order; neither an unresolved key nor an empty part is in it
grep -qF '[[vn:Agent, fn:Agent, pb:0], the predicate, id:first.1.default]' <<<"$out" || fail "row first: not the path of its resolved parts: $out"
grep -qF '[[vn:Theme, fn:Agent, pb:1], the predicate, id:second.1.default]' <<<"$out" || fail "row second: not the path of its resolved parts: $out"
! grep -E '^\[.*(nosuch|No_such_frame).*\]$' <<<"$out" | grep -q . || fail "an unresolved key is in a claim, taken for text: $out"
# each unresolved key an obligation, once for its part: two rolesets, a class, a frame; the NULLs owe nothing
grep -q '== unresolved obligations: 4 ' <<<"$out" || fail "not four unresolved obligations: $out"
grep -Eq 'left out of the path: 16_PB_ROLESET +2$' <<<"$out" || fail "the two rolesets not owed once each: $out"
grep -Eq 'left out of the path: 5_VN_CLASS +1$' <<<"$out" || fail "the class not owed once: $out"
grep -Eq 'left out of the path: 13_FN_FRAME +1$' <<<"$out" || fail "the frame not owed once: $out"
! grep -q 'left out of the path: 7_VN_SUBCLASS' <<<"$out" || fail "an empty part (vn:NULL) is owed: $out"
# the keys no list holds: counted once each, though a type line and a maps line both read them
grep -q 'predicate-matrix: 4 keys in .* name a type no list of the highway holds' <<<"$out" || fail "the unknown keys are not counted once each: $out"
echo "thing-resolved: ok"