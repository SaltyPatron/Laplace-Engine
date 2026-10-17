#!/bin/bash
# A thing named by several parts together, resolved: the path of those of them that resolve (Laplace-Engine#32, BRIEF: a
# mapping row is a path over the resolved highway nodes). A part the source leaves empty is in no path and is owed
# nothing. A key written as the lists write it (type ... node: a PropBank roleset, a FrameNet frame, a VerbNet class as
# the vnclass list keys it) is that node though no list holds it (BRIEF: a highway node is content): content in the
# path, its key whole, counted once. A pointer no list holds (an offset CILI does not number) is left out of the path
# and is an unresolved obligation. Either way the row is still one thing and what is said of it is still said: no row
# is lost. Read through the PredicateMatrix recipe on a fixture whose keys are none any version of the resources writes
# (pb:nosuch.99, vn:nosuch-0.0, mcr:ili-30-99999999-v).
#   tests/thing-resolved.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/thing-resolved.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/thing-resolved.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch
fail(){ echo "thing-resolved: $*" >&2; exit 1; }
out=$(LAPLACE_DATA=$scratch "$L" ingest --claims -s predicate-matrix "$here/thing-resolved/PredicateMatrix.v1.3.txt" 2>&1 | tr -d '\r') || fail "ingest failed: $out"
count(){ grep -c -- "$1" <<<"$out" || true; }
claims=$(grep '^\[' <<<"$out" || true)
# every row is a thing, and what is said of it is said: three rows, three predicates
for p in first second third; do
  [ "$(count "the predicate, id:$p.1.default\]")" = 1 ] || fail "row $p: its predicate is not said of it (the row was lost): $out"; done
# the path holds what resolves, in the recipe's order, each key whole; neither a pointer no list holds nor an empty part is in it
grep -qF '[[nosuch.99, vn:Agent, fn:Agent, pb:0], the predicate, id:first.1.default]' <<<"$out" || fail "row first: the roleset no list holds is not its own node in the path: $out"
grep -qF '[[nosuch-0.0, vn:Theme, fn:Agent, pb:1], the predicate, id:second.1.default]' <<<"$out" || fail "row second: the class no list holds is not its own node in the path, or the offset no list holds is: $out"
grep -qF '[[No_such_frame, nosuch.98, vn:Patient, fn:Agent, pb:2], the predicate, id:third.1.default]' <<<"$out" || fail "row third: the frame and roleset no list holds are not their own nodes in the path: $out"
! grep -q '99999999' <<<"$claims" || fail "an offset no list holds is in a claim: $out"
# the key is never parted: the class is one part of the path, never its pieces (nosuch, -, 0.0)
! grep -Eq '(\[|, )nosuch(, |\])' <<<"$claims" || fail "a key was parted into pieces: $out"
# the pointer no list holds is the one obligation, once; the NULLs and the nodes owe nothing
grep -q '== unresolved obligations: 1 ' <<<"$out" || fail "not one unresolved obligation: $out"
grep -Eq 'left out of the path: 12_MCR_iliOffset +1$' <<<"$out" || fail "the offset not owed once: $out"
! grep -q 'left out of the path: 5_VN_CLASS\|left out of the path: 7_VN_SUBCLASS\|left out of the path: 16_PB\|left out of the path: 13_FN' <<<"$out" || fail "an empty part or a node is owed: $out"
# the nodes no list holds: counted once each as said without a slot, though a type line and a maps line both read them
grep -Eq 'predicate-matrix +pbroleset +16_PB_ROLESET +2$' <<<"$out" || fail "the two rolesets not counted once each: $out"
grep -Eq 'predicate-matrix +fnframe +13_FN_FRAME +1$' <<<"$out" || fail "the frame not counted once: $out"
grep -Eq 'predicate-matrix +vnclass +5_VN_CLASS +1$' <<<"$out" || fail "the class not counted once: $out"
grep -q 'predicate-matrix: 1 keys in .* name a type no list of the highway holds' <<<"$out" || fail "the offset is not counted once: $out"
echo "thing-resolved: ok"
