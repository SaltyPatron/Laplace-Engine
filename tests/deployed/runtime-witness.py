"""The runtime entries: a turn said elsewhere (`laplace said`) and what a witness named at run time states (`laplace
witness`) are recorded by the one writer: content converging on what is recorded, the witness a trunk, an attestation a
claim, witness and lineage at the witness's class, the standings folded in the database.

  said      a turn is the user's [session, turn] at its ordinal (UserPromptContent) and the responder's [response,
            prompt] (ResponseContent); said again it adds nothing; the next turn takes the next place.
  witness   a statement's claims are attested under [the witness's name, the statement]; stated again it adds nothing;
            its subject is the entity the session's response already is; another witness is another row of the same
            claim and moves its standing; a malformed statement leaves no attestation and exits 1.

Scoped: every user (--as), responder (--by) and stating witness is a witness of this check (Fixture.witness, forgotten
with it), and every query is constrained by their trunks, the claims those trunks attest, or an ID the check composed.
The prompts, responses and statements say this check's name, so no other witness attests their claims.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import Fixture, fingerprint, ids, sql  # noqa: E402

CHECK = 'runtime-witness'
MARK = 'Laplace check runtime-witness'
TRUNK = re.compile(r'^  its ID, the witness\s+([0-9a-f]{32})$', re.M)


def main():
    question = f'{MARK}: what is the capital of France?'
    answer = f'{MARK}: Paris is the capital of France'
    with Fixture(CHECK) as fx:
        carol, responder, model = fx.witness('carol'), fx.witness('responder'), fx.witness('conventional model')
        alice, bob, mallory = fx.witness('alice'), fx.witness('bob'), fx.witness('mallory')
        fx.forget()

        def trunks(*witnesses):
            return sorted({t for w in witnesses for t in fx.witness_trunks(w)})

        def rows(statement, *witnesses):
            return sql(statement.replace('TRUNKS', ids(trunks(*witnesses))))

        def state():
            """What is recorded under every witness of the check, and the standings of the claims they attest."""
            return fingerprint(trunks(*fx.witnesses))

        def said(*arguments, check=True):
            return fx.run('said', '--as', carol, '--by', responder, *arguments, check=check)

        # ---- a turn said elsewhere
        receipt = fx.scratch / 'said.jsonl'
        out = said('--session', 'geography', '--receipt', str(receipt), question, answer).stdout
        assert re.search(r'WITNESS    turn [0-9a-f]{32}, at 1 in the session', out), f'the first turn is not at 1: {out}'
        events = [json.loads(line) for line in receipt.read_text(encoding='utf-8').splitlines()]
        assert [e['event'] for e in events] == ['resolve', 'witness'] and events[1]['witnessed'] is True \
            and events[1]['ordinal'] == 1 and events[1]['response'] == events[0]['response'] \
            and re.search(rf"WITNESS    turn {events[1]['turn']}, at 1", out), \
            f'the receipt does not say the turn the trace says: {events} {out}'
        text = fx.run('text', answer).stdout
        answer_id = re.search(r'^entity +([0-9a-f]{32}) ', text, re.M)
        assert answer_id, f'no ID for the answer: {text}'
        answer_id = answer_id[1]
        assert sql(f"SELECT count(*) FROM entity WHERE id='{answer_id}'::blake3") == '1', \
            'the response is not recorded as the content it is'
        assert len(trunks(carol)) == 1 and len(trunks(responder)) == 1, \
            f'the user and the responder are not two witnesses: {trunks(carol)} {trunks(responder)}'
        assert rows('SELECT count(*) FROM attestation WHERE witness=ANY(TRUNKS)', carol, responder) == '2', \
            'a turn is two attestations, the user\'s and the responder\'s'
        places = rows("SELECT string_agg(trust::text||':'||coalesce(\"position\"::text,'-'), ' ' ORDER BY trust DESC)"
                      ' FROM attestation WHERE witness=ANY(TRUNKS)', carol, responder)
        assert places == '0.3:1 0.2:-', \
            f'the user\'s claim is not at UserPromptContent and place 1, the responder\'s at ResponseContent: {places}'
        assert rows('SELECT count(*) FROM consensus c WHERE c.rating>1500 AND c.claim IN'
                    ' (SELECT claim FROM attestation WHERE witness=ANY(TRUNKS))', carol, responder) == '2', \
            'the turn\'s claims have no standing folded in the database'
        before = state()
        out = said('--session', 'geography', question, answer).stdout
        assert 'is held by the session, at 1: nothing is witnessed again' in out, f'the same turn said again is not found held: {out}'
        assert state() == before, 'the same turn said again changed the record'
        out = fx.run('said', '--as', carol, '--by', model, '--session', 'geography',
                     f'{MARK}: and of Germany?', f'{MARK}: Berlin is the capital of Germany').stdout
        assert 'at 2 in the session' in out, f'the second turn is not at 2: {out}'
        assert rows('SELECT count(*) FROM attestation WHERE witness=ANY(TRUNKS)', carol, responder, model) == '4', \
            'two turns are four attestations'
        assert len(trunks(carol, responder, model)) == 4, 'each turn\'s two trunks are not witnesses of their own'
        out = said('--session', 'history', question, answer).stdout
        assert 'at 1 in the session' in out, f'the same exchange in another session is not that session\'s first turn: {out}'

        # ---- what a witness states
        subject = '_'.join(('laplace', 'check', 'runtime', 'witness'))
        statement = fx.scratch / 'feedback.statement'
        statement.write_text(f'{answer}\tis\ttrue\t1\n{MARK}: the earth is flat\tis\ttrue\t0\n'
                             f'def {subject}(): pass\tcompiles\t\t0.5\n', encoding='utf-8', newline='\n')

        def witness(name, cls, path, check=True):
            return fx.run('witness', '-j', '2', '--as', name, '--class', cls, path, check=check)

        out = witness(alice, 'UserPromptContent', statement).stdout
        alice_trunk = TRUNK.search(out)
        assert alice_trunk, f'no witness trunk said: {out}'
        alice_trunk = alice_trunk[1]
        assert trunks(alice) == [alice_trunk], f'alice\'s trunk is not the one recorded: {alice_trunk} {trunks(alice)}'
        mine = f"'{alice_trunk}'::blake3"
        shape = sql(f'SELECT count(*)||\' \'||count(DISTINCT lineage)||\' \'||min(trust)||\' \'||max(trust)'
                    f' FROM attestation WHERE witness={mine}')
        assert shape == '3 1 0.3 0.3', f'alice\'s three claims are not three attestations of her trunk, one lineage, at her class: {shape}'
        scores = sql(f"SELECT string_agg(score::text, ' ' ORDER BY score) FROM attestation WHERE witness={mine}")
        assert scores == '0 0.5 1', f'the outcomes are not the statement\'s scores: {scores}'
        assert sql(f"SELECT count(*) FROM entity WHERE id='{answer_id}'::blake3") == '1', \
            'the statement\'s subject did not converge on the recorded response'
        assert sql(f"SELECT count(*) FROM physicality p WHERE p.entity IN (SELECT claim FROM attestation WHERE witness={mine})"
                   f" AND '{answer_id}'::blake3 IN (SELECT v FROM unnest(laplace_vertex_ids(p.path)) v)") == '1', \
            'no claim of alice\'s holds the recorded response as its subject'

        def standing(witness_id, score):
            return float(sql(f'SELECT c.rating FROM consensus c JOIN attestation a ON a.claim=c.claim'
                             f" WHERE a.witness='{witness_id}'::blake3 AND a.score={score}"))
        confirmed, refuted = standing(alice_trunk, 1), standing(alice_trunk, 0)
        assert confirmed > 1500 > refuted, f'a confirmation did not raise and a refutation lower the standing: {confirmed} {refuted}'
        before = state()
        again = fx.scratch / 'again.statement'
        again.write_bytes(statement.read_bytes())
        out = witness(alice, 'UserPromptContent', again).stdout
        assert state() == before, 'the same statement stated again changed the record'
        assert TRUNK.search(out) and TRUNK.search(out)[1] == alice_trunk, f'the same statement is another witness: {out}'

        out = witness(bob, 'UserCuratedResource', statement).stdout
        bob_trunk = TRUNK.search(out)
        assert bob_trunk and bob_trunk[1] != alice_trunk, f'bob is not a witness of his own: {out}'
        bob_trunk = bob_trunk[1]
        assert sql(f"SELECT count(*)||' '||min(trust) FROM attestation WHERE witness='{bob_trunk}'::blake3") == '3 0.6', \
            'bob\'s attestations are not his own rows at his class'
        assert sql(f'SELECT count(DISTINCT a.lineage) FROM attestation a WHERE a.claim IN'
                   f' (SELECT claim FROM attestation WHERE witness={mine})') == '2', \
            'alice and bob are not two lineages of the same claims'
        assert sql(f'SELECT count(*) FROM consensus c WHERE c.claim IN (SELECT claim FROM attestation WHERE witness={mine})') == '3', \
            'two witnesses of a claim are not one standing'
        assert standing(alice_trunk, 1) > confirmed and standing(alice_trunk, 0) < refuted, \
            f'a second witness did not move the standings: {standing(alice_trunk, 1)} {standing(alice_trunk, 0)}'
        assert rows('SELECT count(*) FROM attestation WHERE witness=ANY(TRUNKS)', alice, bob) == '6', \
            'two witnesses of three claims are not six attestations'

        # what the same witness states later is another trunk of the same lineage
        later_statement = fx.scratch / 'later.statement'
        later_statement.write_text(f'{MARK}: water\tis\twet\n', encoding='utf-8', newline='\n')
        out = witness(alice, 'UserPromptContent', later_statement).stdout
        later = TRUNK.search(out)
        assert later and later[1] != alice_trunk, f'a later statement is not a trunk of its own: {out}'
        assert sql(f"SELECT count(DISTINCT lineage) FROM attestation WHERE witness IN ({mine},'{later[1]}'::blake3)") == '1', \
            'what one witness states at two times is not one lineage'

        # ---- a malformed statement is refused whole
        before = state()
        malformed = fx.scratch / 'malformed.statement'
        malformed.write_text(f'{MARK}: ice\tis\tcold\t1\nno relation here\n', encoding='utf-8', newline='\n')
        refused = witness(mallory, 'UserPromptContent', malformed, check=False)
        assert refused.returncode == 1, f'a malformed statement: exit {refused.returncode}, not 1: {refused.stdout}{refused.stderr}'
        assert trunks(mallory) == [] and state() == before, 'a refused statement left attestations or standings'
        empty = said('--session', 'geography', '', f'{MARK}: nothing asked', check=False)
        assert empty.returncode == 1 and state() == before, f'an empty prompt: exit {empty.returncode}: {empty.stdout}{empty.stderr}'
    print('runtime-witness: a turn said elsewhere and a witness\'s statement are recorded once, converge on recorded '
          'content, attest per witness and lineage at the class\'s trust, and fold their standings in the database')
    return 0


if __name__ == '__main__':
    sys.exit(main())
