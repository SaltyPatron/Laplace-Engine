#!/bin/bash
# The runtime entries, against a database: a turn said elsewhere (laplace said) and what a witness named at run time
# states (laplace witness) are recorded by the one writer: content converging on what is recorded, the witness a trunk,
# an attestation a claim, witness and lineage at the witness's class, the standings folded in the database.
#   said      a turn is the user's [session, turn] at its ordinal (UserPromptContent) and the responder's
#             [response, prompt] (ResponseContent); said again it adds nothing; the next turn takes the next place.
#   witness   a statement's claims are attested under [the witness's name, the statement]; stated again it adds nothing;
#             its subject is the entity the session's response already is; another witness is another row of the same
#             claim and moves its standing; a malformed statement leaves no attestation and exits 1.
# A disposable database (lp_witness_*) is deployed and dropped after.
#   tests/runtime-witness.sh path/to/laplace        (PGHOST, PGUSER as the runner's; PSQL, DROPDB: the client programs)
set -euo pipefail
L=${1:?usage: tests/runtime-witness.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/witness.XXXXXX")
if command -v cygpath >/dev/null 2>&1; then scratch=$(cygpath -m "$scratch"); here=$(cygpath -m "$here"); fi
db=lp_witness_$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')
conn="host=${PGHOST:-/tmp} port=${PGPORT:-5432} user=${PGUSER:-laplace} dbname=$db"
dropdb=${DROPDB:-$(dirname "${PSQL:-$(command -v psql)}")/dropdb}
cleanup(){
  "$dropdb" -h "${PGHOST:-/tmp}" -p "${PGPORT:-5432}" -U "${PGUSER:-laplace}" --if-exists "$db" >/dev/null 2>&1 || true
  local target base; target=$(cd "$scratch" && pwd -P); base=$(cd "${TMPDIR:-/tmp}" && pwd -P)
  case "$target" in "$base"/witness.*) rm -rf -- "$scratch";; *) return 1;; esac
}
trap cleanup EXIT
fail(){ echo "runtime-witness: $*" >&2; exit 1; }
export LAPLACE_WORK=$scratch/work LAPLACE_RECIPES=${LAPLACE_RECIPES_FROM:-$here/../recipes}
export LAPLACE_FIRMWARE=${LAPLACE_FIRMWARE:-$here/../firmware/program.firmware}
sql(){ "${PSQL:-psql}" -XwAt -v ON_ERROR_STOP=1 -d "$conn" -c "$1"; }
state(){ sql "SELECT (SELECT count(*) FROM public.entity)||' '||(SELECT count(*) FROM public.physicality)||' '||(SELECT count(*) FROM public.attestation)||' '||(SELECT count(*) FROM public.consensus)||' '||(SELECT md5(string_agg(claim::text||rating||deviation||volatility||matches, ',' ORDER BY claim::text)) FROM public.consensus)"; }
attestations(){ sql "SELECT count(*) FROM public.attestation"; }
id_of(){ "$L" text "$1" | sed -n 's/^entity  *\([0-9a-f]\{32\}\) .*/\1/p'; }

"$L" deploy -d "$conn" >"$scratch/deploy.log" 2>&1 || fail "deploy: $(tail -5 "$scratch/deploy.log")"

# ---- a turn said elsewhere
question='What is the capital of France?'; answer='Paris is the capital of France'
said(){ "$L" said -d "$conn" --as carol --session geography "$@"; }
out=$(said "$question" "$answer" 2>&1) || fail "said: $out"
grep -q "WITNESS    turn [0-9a-f]\{32\}, at 1 in the session" <<<"$out" || fail "the first turn is not at 1: $out"
answer_id=$(id_of "$answer"); [[ "$answer_id" =~ ^[0-9a-f]{32}$ ]] || fail "no ID for the answer"
[[ "$(sql "SELECT count(*) FROM public.entity WHERE id::text='$answer_id'")" == 1 ]] || fail "the response is not recorded as the content it is"
[[ "$(attestations)" == 2 ]] || fail "a turn is two attestations, the user's and the responder's: $(attestations)"
[[ "$(sql "SELECT string_agg(trust::text||':'||coalesce(\"position\"::text,'-'), ' ' ORDER BY trust DESC) FROM public.attestation")" == "0.3:1 0.2:-" ]] \
  || fail "the user's claim is not at UserPromptContent and place 1, the responder's at ResponseContent: $(sql 'SELECT trust,"position" FROM public.attestation')"
[[ "$(sql "SELECT count(DISTINCT witness) FROM public.attestation")" == 2 ]] || fail "the user and the responder are not two witnesses"
[[ "$(sql "SELECT count(*) FROM public.consensus c JOIN public.attestation a ON a.claim::text=c.claim::text WHERE c.rating>1500")" == 2 ]] || fail "the turn's claims have no standing folded in the database"
before=$(state)
out=$(said "$question" "$answer" 2>&1) || fail "said again: $out"
grep -q "is held by the session, at 1: nothing is witnessed again" <<<"$out" || fail "the same turn said again is not found held: $out"
[[ "$(state)" == "$before" ]] || fail "the same turn said again changed the record: $before -> $(state)"
out=$(said --by "a conventional model" "And of Germany?" "Berlin is the capital of Germany" 2>&1) || fail "said, second turn: $out"
grep -q "at 2 in the session" <<<"$out" || fail "the second turn is not at 2: $out"
[[ "$(attestations)" == 4 ]] || fail "two turns are four attestations: $(attestations)"
[[ "$(sql "SELECT count(DISTINCT witness) FROM public.attestation")" == 4 ]] || fail "each turn's two trunks are witnesses of their own"
out=$("$L" said -d "$conn" --as carol --session history "$question" "$answer" 2>&1) || fail "said, another session: $out"
grep -q "at 1 in the session" <<<"$out" || fail "the same exchange in another session is not that session's first turn: $out"

# ---- what a witness states
printf '%s\tis\ttrue\t1\nthe earth is flat\tis\ttrue\t0\ndef f(): pass\tcompiles\t\t0.5\n' "$answer" > "$scratch/feedback.statement"
witness(){ "$L" witness -d "$conn" -j 2 "$@"; }
base=$(attestations); entities=$(sql "SELECT count(*) FROM public.entity")
out=$(witness --as alice --class UserPromptContent "$scratch/feedback.statement" 2>&1) || fail "witness: $out"
alice=$(sed -n 's/^  its ID, the witness  *\([0-9a-f]\{32\}\)$/\1/p' <<<"$out"); [[ "$alice" =~ ^[0-9a-f]{32}$ ]] || fail "no witness trunk said: $out"
[[ "$(sql "SELECT count(*)||' '||count(DISTINCT lineage)||' '||min(trust)||' '||max(trust) FROM public.attestation WHERE witness::text='$alice'")" == "3 1 0.3 0.3" ]] \
  || fail "alice's three claims are not three attestations of her trunk, one lineage, at her class: $(sql "SELECT claim,lineage,trust FROM public.attestation WHERE witness::text='$alice'")"
[[ "$(sql "SELECT string_agg(score::text, ' ' ORDER BY score) FROM public.attestation WHERE witness::text='$alice'")" == "0 0.5 1" ]] || fail "the outcomes are not the statement's scores"
[[ "$(sql "SELECT count(*) FROM public.entity WHERE id::text='$answer_id'")" == 1 ]] || fail "the statement's subject did not converge on the recorded response"
[[ "$(sql "SELECT count(*) FROM public.physicality p WHERE p.entity::text IN (SELECT claim::text FROM public.attestation WHERE witness::text='$alice') AND '$answer_id' IN (SELECT v::text FROM unnest(public.laplace_vertex_ids(p.path)) v)")" == 1 ]] \
  || fail "no claim of alice's holds the recorded response as its subject"
standing(){ sql "SELECT c.rating FROM public.consensus c JOIN public.attestation a ON a.claim::text=c.claim::text WHERE a.witness::text='$alice' AND a.score=$1"; }
confirmed=$(standing 1); refuted=$(standing 0)
awk -v c="$confirmed" -v r="$refuted" 'BEGIN{exit !(c>1500 && r<1500)}' || fail "a confirmation did not raise and a refutation lower the standing: $confirmed $refuted"
sql "CREATE TABLE seen_entity AS SELECT id::text AS id FROM public.entity" >/dev/null
before=$(state)
cp "$scratch/feedback.statement" "$scratch/again.statement"
out=$(witness --as alice --class UserPromptContent "$scratch/again.statement" 2>&1) || fail "witness again: $out"
[[ "$(state)" == "$before" ]] || fail "the same statement stated again changed the record: $before -> $(state): $(sql "SELECT e.id::text||' tier '||e.tier||' '||coalesce(public.laplace_text(e.id),'') FROM public.entity e WHERE e.id::text NOT IN (SELECT id FROM seen_entity)") :: $(grep -i 'witness\|trunk\|recorded' <<<"$out")"
grep -q "^  its ID, the witness  *$alice\$" <<<"$out" || fail "the same statement is another witness: $out"

out=$(witness --as bob --class UserCuratedResource "$scratch/feedback.statement" 2>&1) || fail "witness, bob: $out"
bob=$(sed -n 's/^  its ID, the witness  *\([0-9a-f]\{32\}\)$/\1/p' <<<"$out"); [[ "$bob" =~ ^[0-9a-f]{32}$ && "$bob" != "$alice" ]] || fail "bob is not a witness of his own: $out"
[[ "$(sql "SELECT count(*)||' '||min(trust) FROM public.attestation WHERE witness::text='$bob'")" == "3 0.6" ]] || fail "bob's attestations are not his own rows at his class"
[[ "$(sql "SELECT count(DISTINCT a.lineage) FROM public.attestation a WHERE a.claim::text IN (SELECT claim::text FROM public.attestation WHERE witness::text='$alice')")" == 2 ]] || fail "alice and bob are not two lineages of the same claims"
[[ "$(sql "SELECT count(*) FROM public.consensus c WHERE c.claim::text IN (SELECT claim::text FROM public.attestation WHERE witness::text='$alice')")" == 3 ]] || fail "two witnesses of a claim are not one standing"
awk -v a="$(standing 1)" -v b="$confirmed" -v c="$(standing 0)" -v d="$refuted" 'BEGIN{exit !(a>b && c<d)}' || fail "a second witness did not move the standings: $(standing 1) $(standing 0)"
[[ "$(attestations)" == $((base + 6)) ]] || fail "two witnesses of three claims are six attestations: $(attestations) from $base"

# what the same witness states later is another trunk of the same lineage
printf 'water\tis\twet\n' > "$scratch/later.statement"
out=$(witness --as alice --class UserPromptContent "$scratch/later.statement" 2>&1) || fail "witness, later: $out"
later=$(sed -n 's/^  its ID, the witness  *\([0-9a-f]\{32\}\)$/\1/p' <<<"$out"); [[ "$later" =~ ^[0-9a-f]{32}$ && "$later" != "$alice" ]] || fail "a later statement is not a trunk of its own: $out"
[[ "$(sql "SELECT count(DISTINCT lineage) FROM public.attestation WHERE witness::text IN ('$alice','$later')")" == 1 ]] || fail "what one witness states at two times is not one lineage"

# ---- a malformed statement is refused whole
before=$(state)
printf 'ice\tis\tcold\t1\nno relation here\n' > "$scratch/malformed.statement"
code=0; out=$(witness --as mallory --class UserPromptContent "$scratch/malformed.statement" 2>&1) || code=$?
[[ $code -eq 1 ]] || fail "a malformed statement: exit $code, not 1: $out"
[[ "$(attestations)" == "$(sql "SELECT count(*) FROM public.attestation" )" && "$(cut -d' ' -f3- <<<"$(state)")" == "$(cut -d' ' -f3- <<<"$before")" ]] || fail "a refused statement left attestations or standings: $before -> $(state)"
code=0; out=$(said "" "nothing asked" 2>&1) || code=$?
[[ $code -eq 1 && "$(cut -d' ' -f3- <<<"$(state)")" == "$(cut -d' ' -f3- <<<"$before")" ]] || fail "an empty prompt: exit $code: $out"
echo "runtime-witness: a turn said elsewhere and a witness's statement are recorded once, converge on recorded content, attest per witness and lineage at the class's trust, and fold their standings in the database"
