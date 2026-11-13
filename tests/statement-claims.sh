#!/bin/bash
# What a witness named at run time states (laplace witness), read without a database: a .statement is a claim a line
# (recipes/statement.recipe: subject, relation, object and a score from 0 to 1, between tabs), its row strict: a line
# that attests nothing or writes a score that is no number on its scale is refused and the run exits 1. A statement is
# its content alone: the same lines under another file name are the same trunk. `laplace describe` lists both runtime
# entries, witness and said, and the receipt a turn writes; said and turn refuse an empty or missing text, and a receipt
# that cannot be written, before they open a database.
#   tests/statement-claims.sh path/to/laplace
set -euo pipefail
set -E; trap 'echo "FAILED: ${BASH_SOURCE[0]}:$LINENO: $BASH_COMMAND" >&2' ERR
L=${1:?usage: tests/statement-claims.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/statement.XXXXXX")
if command -v cygpath >/dev/null 2>&1; then scratch=$(cygpath -m "$scratch"); here=$(cygpath -m "$here"); fi
cleanup(){ local target base; target=$(cd "$scratch" && pwd -P); base=$(cd "${TMPDIR:-/tmp}" && pwd -P)
  case "$target" in "$base"/statement.*) rm -rf -- "$scratch";; *) return 1;; esac; }
trap cleanup EXIT
fail(){ echo "statement-claims: $*" >&2; exit 1; }
export LAPLACE_RECIPES=${LAPLACE_RECIPES_FROM:-$here/../recipes}
witness(){ "$L" witness --as "Statement Check" --class UserPromptContent "$@"; }

printf 'Paris is the capital of France\tis\ttrue\t1\nthe earth is flat\tis\ttrue\t0\ndef f(): pass\tcompiles\t\t0.5\nwater\tis\twet\n' > "$scratch/a.statement"
out=$(witness --claims "$scratch/a.statement" 2>&1) || fail "a well-formed statement was refused: $out"
grep -qxF '[Paris is the capital of France, is, true]' <<<"$out" || fail "the confirmed claim is not said: $out"
grep -qxF '[the earth is flat, is, true]   score 0' <<<"$out" || fail "the refuted claim does not carry its score: $out"
grep -qxF '[def f(): pass, compiles]   score 0.5' <<<"$out" || fail "a pair with a score is not said: $out"
grep -qxF '[water, is, wet]' <<<"$out" || fail "a claim with no score is not a win: $out"
grep -q '4 claims said by records' <<<"$out" || fail "the statement does not say four claims: $out"

# a statement is its content: no file name or path is in its trunk
cp "$scratch/a.statement" "$scratch/renamed.statement"
trunk(){ LAPLACE_TRUNKS=1 witness --no-load "$1" 2>&1 | sed -n 's/^  trunk \([0-9a-f]\{32\}\) .*/\1/p'; }
first=$(trunk "$scratch/a.statement"); second=$(trunk "$scratch/renamed.statement")
[[ "$first" =~ ^[0-9a-f]{32}$ && "$first" == "$second" ]] || fail "the same lines under another name are another trunk: $first $second"

refused(){ local name=$1 why=$2 out code=0; printf "$3" > "$scratch/$name.statement"
  out=$(witness --claims "$scratch/$name.statement" 2>&1) || code=$?
  [[ $code -eq 1 ]] || fail "$name: exit $code, not 1: $out"
  grep -q "refused: part [0-9]* $why" <<<"$out" || fail "$name: not refused for the reason ($why): $out"
  grep -q 'ingest rejected: .* refused_parts=[1-9]' <<<"$out" || fail "$name: the run was not rejected: $out"; }
refused subject-only 'attests nothing' 'just a subject\n'
refused no-relation 'attests nothing' 'a\t\tb\t1\n'
refused blank-line 'attests nothing' 'a\tis\tb\t1\n\nc\tis\td\t1\n'
refused word-score 'writes a score that is no number on its scale' 'a\tis\tb\tyes\n'
refused score-beyond 'writes a score that is no number on its scale' 'a\tis\tb\t7\n'
refused score-trailing 'writes a score that is no number on its scale' 'a\tis\tb\t1x\n'

code=0; out=$(witness --class Nope --claims "$scratch/a.statement" 2>&1) || code=$?
[[ $code -eq 2 ]] && grep -q 'not a trust class the registry declares' <<<"$out" || fail "an undeclared class: exit $code: $out"
code=0; out=$("$L" witness --as x --claims "$scratch/a.statement" 2>&1) || code=$?
[[ $code -eq 2 ]] || fail "a witness without a class: exit $code: $out"
code=0; out=$(witness --claims "$scratch/absent.statement" 2>&1) || code=$?
[[ $code -eq 2 ]] || fail "a file that is not there: exit $code: $out"

catalog=$("$L" describe)
for op in witness said; do grep -q "\"name\":\"$op\"" <<<"$catalog" || fail "describe does not list $op"; done
grep -q -- '--receipt' <<<"$catalog" || fail "describe does not list the turn's receipt"
code=0; out=$("$L" turn --files "$scratch/absent.txt" 2>&1) || code=$?
[[ $code -eq 1 ]] && grep -q "the prompt's file" <<<"$out" || fail "a turn's prompt file that is not there: exit $code: $out"
code=0; out=$("$L" said --as u --session s --receipt "$scratch/no/such/dir/r.jsonl" "a prompt" "a response" 2>&1) || code=$?
[[ $code -eq 1 ]] && grep -q 'the receipt' <<<"$out" || fail "a receipt that cannot be written: exit $code: $out"
code=0; out=$("$L" said --as u --session s "" "a response" 2>&1) || code=$?
[[ $code -eq 1 ]] && grep -q 'the prompt is empty' <<<"$out" || fail "an empty prompt: exit $code: $out"
code=0; out=$("$L" said --as u --session s --files "$scratch/absent.txt" "$scratch/a.statement" 2>&1) || code=$?
[[ $code -eq 1 ]] && grep -q "the prompt's file" <<<"$out" || fail "a prompt file that is not there: exit $code: $out"
printf 'not \xff text' > "$scratch/bytes.txt"
code=0; out=$("$L" said --as u --session s --files "$scratch/a.statement" "$scratch/bytes.txt" 2>&1) || code=$?
[[ $code -eq 1 ]] && grep -q 'the response is not UTF-8 text' <<<"$out" || fail "a response that is not UTF-8: exit $code: $out"
code=0; out=$("$L" said --as u --session s "only a prompt" 2>&1) || code=$?
[[ $code -eq 2 ]] || fail "a prompt with no response: exit $code: $out"
echo "statement-claims: a statement says its claims with their scores, is its content alone, and is refused whole when a line is malformed; said refuses what is no turn"
