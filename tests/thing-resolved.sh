#!/bin/bash
# PredicateMatrix keeps one complete n-ary correspondence per row, in source
# order. Language, predicate, roles, lexicalizations and frequency remain in
# that joint context. Empty cells contribute nothing. Missing pointer keys
# are diagnosed; identifier text remains canonical content without a slot.
#   tests/thing-resolved.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/thing-resolved.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/thing-resolved.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "thing-resolved: $*" >&2; exit 1; }
out=$(LAPLACE_DATA=$scratch "$L" ingest --claims -s predicate-matrix "$here/thing-resolved/PredicateMatrix.v1.3.txt" 2>&1 | tr -d '\r') || fail "ingest failed: $out"
count(){ grep -c -- "$1" <<<"$out" || true; }
[ "$(count '^\[eng,')" = 3 ] || fail "the three source rows did not remain three joint claims: $out"
grep -qxF '[nosuch.98, [pbroleset, cross-resource mapping, fnframe], No_such_frame]' <<<"$out" || fail "the source crosswalk was not retained: $out"
grep -qxF '[eng, v, first.1.default, arg0, test, Agent, Agent, nosuch.99, 0, 7]' <<<"$out" || fail "row first lost its joint correspondence or stated relation count: $out"
grep -qxF '[eng, v, second.1.default, arg0, nosuch-0.0, test, Theme, Agent, 1]' <<<"$out" || fail "row second lost its own class identifier or context: $out"
grep -qxF '[eng, v, third.1.default, arg0, test, Patient, No_such_frame, [No_such_frame, Agent], nosuch.98, 2, 9]' <<<"$out" || fail "row third lost its frame-qualified role, context or relation count: $out"
# The missing pointer is counted below as one absent key. It must not be
# counted again as an undisposed semantic field; NULL cells owe nothing.
grep -q '== unresolved obligations: 0 ' <<<"$out" || fail "empty cells or the diagnosed pointer were counted again: $out"
! grep -q 'left out of the path: 7_VN_SUBCLASS\|left out of the path: 16_PB\|left out of the path: 13_FN' <<<"$out" || fail "an empty part or a node is owed: $out"
# the nodes no list holds: counted once each as said without a slot, though a type line and a maps line both read them
grep -Eq 'predicate-matrix +pbroleset +16_PB_ROLESET +2$' <<<"$out" || fail "the two rolesets not counted once each: $out"
grep -Eq 'predicate-matrix +fnframe +13_FN_FRAME +1$' <<<"$out" || fail "the frame not counted once: $out"
grep -Eq 'predicate-matrix +vnclass +5_VN_CLASS +1$' <<<"$out" || fail "the class identifier without a cache slot is not counted once: $out"
echo "thing-resolved: ok"
