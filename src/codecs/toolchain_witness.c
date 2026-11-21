#include "codecs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Emit a ToolchainVerification (trust 0.90) claim carrying explicit WIN/LOSS outcome
 * bits (lp_outcome_of) anchored onto the target entity and optional constituent nodes so
 * GIN salience differential highlights causal build/test failures in microseconds. */

int lp_toolchain_witness(const LpToolchainReport *rep, lp_id *claim_out, LoadStats *stats){
    if (!rep || !rep->subject || !rep->witness_name || !*rep->witness_name) return 1;

    const lp_trust_class *tc = lp_trust_class_named("ToolchainVerification");
    double trust = tc ? tc->prior : 0.90;
    double score = rep->passed ? 1.0 : 0.0;
    const char *label = (rep->target_label && *rep->target_label) ? rep->target_label : "verify";
    const char *verdict = rep->passed ? "PASS" : "FAIL";

    Ref wref = text_ref(CTX[0], (const uint8_t *)rep->witness_name, strlen(rep->witness_name));
    Ref lref = text_ref(CTX[0], (const uint8_t *)label, strlen(label));
    Ref vref = text_ref(CTX[0], (const uint8_t *)verdict, strlen(verdict));

    Ref subj = *rep->subject;
    subj.said = subj.outcome = subj.position = subj.spare = 0;
    Ref cp[3] = { subj, lref, vref };
    Ref cl = said_claim(compose(cp, 3, ref_above(cp, 3)));
    cl.outcome = (uint8_t)lp_outcome_of(score, &cl.spare);
    cl.position = rep->position ? rep->position : 1u;

    File f;
    memset(&f, 0, sizeof f);
    f.path = label;
    f.witness = wref;
    f.lineage = wref;
    f.has_lineage = 1;
    f.trust = trust;

    Event main_ev = {
        .claim = cl.id, .witnessed = cl.id,
        .score = (float)score, .enter_rating = 1500.0f,
        .position = cl.position, .kind = EV_CLAIM, .run = 1
    };
    ev_push(&f.ev, &main_ev);

    lp_vec(Ref) parts = { 0 };
    lp_push(&parts, cl);
    if (rep->constituents && rep->nconstituents) {
        Ref crel = text_ref(CTX[0], (const uint8_t *)"constituent", 11);
        for (size_t i = 0; i < rep->nconstituents; i++) {
            Ref c = rep->constituents[i];
            c.said = c.outcome = c.position = c.spare = 0;
            Ref p3[3] = { subj, crel, c };
            Ref cc = said_claim(compose(p3, 3, ref_above(p3, 3)));
            cc.outcome = (uint8_t)lp_outcome_of(score, &cc.spare);
            cc.position = (uint32_t)(i + 2u);
            Event ce = {
                .claim = cc.id, .witnessed = cc.id,
                .score = (float)score, .enter_rating = 1500.0f,
                .position = cc.position, .kind = EV_CLAIM, .run = 1
            };
            ev_push(&f.ev, &ce);
            lp_push(&parts, cc);
        }
    }

    Ref body = parts.n == 1 ? parts.v[0] : compose(parts.v, (uint32_t)parts.n, ref_above(parts.v, parts.n));
    lp_vec_free(&parts);

    Ref tr[2] = { said_metadata(wref), body };
    Ref trunk = compose(tr, 2, ref_above(tr, 2));
    f.trunk = trunk;
    f.file = trunk;
    f.has_file = 1;

    if (claim_out) *claim_out = cl.id;

    if (!rep->conninfo || !*rep->conninfo) {
        free(f.ev.e);
        return 0;
    }

    Fold *fold = fold_new();
    fold_events(fold, &f, 1);
    LoadStats local = { 0 };
    LoadStats *ls = stats ? stats : &local;
    LoadWitness lw = { .id = trunk.id, .lineage = wref.id, .trust = trust, .has_lineage = 1 };
    int rc = load(rep->conninfo, 1, &f, 1, ls);
    fold_free(fold);
    free(f.ev.e);
    if (rc || load_register(rep->conninfo, 1, &lw, 1, &f, 1) || merge(rep->conninfo, 1, ls)) {
        return 1;
    }
    return 0;
}
