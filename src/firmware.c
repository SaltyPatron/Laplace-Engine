/* The personality firmware: a pull's decisions, read from a file and never from the records. The knowledge is the
 * records; how they are navigated and how an answer is reached is the firmware, one for each human being
 * (Semantics: Personality firmware). What the file can say is described in firmware/program.firmware.
 *
 *   $LAPLACE_FIRMWARE, or --firmware FILE on a command: the firmware a pull runs under */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *OPS[FW_OPS] = { "hop", "search", "translate", "follows", "pull" };

const char *firmware_path(void){
    const char *e = getenv("LAPLACE_FIRMWARE"); if (e && *e) return e;
    static char p[4200]; snprintf(p, sizeof p, "%s/../firmware/program.firmware", laplace_recipes()); return p;
}
static void names(Firmware *fw, char (*into)[96], int *n, int cap, const char *path, int line){
    char *tok; int any = 0;
    while ((tok = strtok(NULL, " \t\r\n"))) { if (tok[0] == '#') break; if (*n >= cap) { fprintf(stderr, "%s:%d: more names than a firmware holds (%d)\n", path, line, cap); exit(2); } snprintf(into[(*n)++], 96, "%s", tok); any = 1; }
    if (!any) { fprintf(stderr, "%s:%d: nothing is named\n", path, line); exit(2); }
    (void)fw;
}
/* Reads the firmware for one operation: the decisions that hold everywhere, then the operation's own. */
Firmware firmware_for(const char *path, int op){
    Firmware fw; memset(&fw, 0, sizeof fw);
    fw.k = 2.0; fw.lambda = 0.05; fw.fan = op == FW_SEARCH ? 512 : 4096; fw.hops = 8; fw.emit = 32; fw.top_within = 0; fw.fact = 2.0; fw.order_witness = 1; fw.shape = FW_FRECHET;
    fw.walks = 64; fw.steps = 4; fw.walk_fan = 512; fw.nearest = 512; fw.restart = 0.25; fw.sure = 2.0; fw.lift = 2.0; fw.tie = FW_TIE_FIRST;
    { static const int E[] = { FW_E_GROUNDS, FW_E_CONTINUITY, FW_E_AGREE, FW_E_HEADS, FW_E_COOCCUR, FW_E_WALKS, FW_E_SHAPE, FW_E_CONFIDENCE, FW_E_SHARED }; memcpy(fw.elect, E, sizeof E); fw.nelect = FW_E_KEYS; }
    snprintf(fw.path, sizeof fw.path, "%s", path && *path ? path : firmware_path());
    FILE *f = fopen(fw.path, "r"); if (!f) { perror(fw.path); fprintf(stderr, "LAPLACE_FIRMWARE names the firmware a pull runs under\n"); exit(1); }
    char buf[1024]; int line = 0, in = -1;                                  /* in: the operation whose instruction set is being read */
    while (fgets(buf, sizeof buf, f)) {
        line++; char *c = buf; while (*c == ' ' || *c == '\t') c++;
        if (*c == '#' || *c == '\n' || *c == '\r' || !*c) continue;
        char *tok = strtok(c, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "for")) { tok = strtok(NULL, " \t\r\n"); in = -2;
            for (int i = 0; tok && i < FW_OPS; i++) if (!strcmp(tok, OPS[i])) in = i;
            if (in == -2) { fprintf(stderr, "%s:%d: %s is not an operation of the forward pass (hop, search, translate, follows, pull)\n", fw.path, line, tok ? tok : ""); exit(2); }
            continue; }
        if (in >= 0 && in != op) continue;                                 /* another operation's instruction set */
        char *v = NULL;
        #define NUMBER() ({ v = strtok(NULL, " \t\r\n"); char *e_; if (!v) { fprintf(stderr, "%s:%d: %s N\n", fw.path, line, tok); exit(2); } double d_ = strtod(v, &e_); if (*e_) { fprintf(stderr, "%s:%d: %s is not a number\n", fw.path, line, v); exit(2); } d_; })
        if (!strcmp(tok, "k")) fw.k = NUMBER();
        else if (!strcmp(tok, "lambda")) fw.lambda = NUMBER();
        else if (!strcmp(tok, "fan")) fw.fan = (int)NUMBER();
        else if (!strcmp(tok, "hops")) fw.hops = (int)NUMBER();
        else if (!strcmp(tok, "fact")) fw.fact = NUMBER();
        else if (!strcmp(tok, "emit")) fw.emit = (int)NUMBER();
        else if (!strcmp(tok, "enough")) { fw.enough = NUMBER(); if (fw.enough < 0 || fw.enough > 1) { fprintf(stderr, "%s:%d: enough is between 0 and 1\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "top")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "always")) fw.top_within = 0; else if (v && !strcmp(v, "within")) fw.top_within = NUMBER();
            else { fprintf(stderr, "%s:%d: top always | within N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "order")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "witness")) fw.order_witness = 1; else if (v && !strcmp(v, "standing")) fw.order_witness = 0;
            else { fprintf(stderr, "%s:%d: order witness | standing\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "only")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "predicate")) names(&fw, fw.only_predicate, &fw.nonly_predicate, FW_NAMES, fw.path, line);
            else { fprintf(stderr, "%s:%d: only predicate NAME...\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "refuse")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "predicate")) names(&fw, fw.refuse_predicate, &fw.nrefuse_predicate, FW_NAMES, fw.path, line);
            else if (v && !strcmp(v, "witness")) names(&fw, fw.refuse_witness, &fw.nrefuse_witness, FW_NAMES, fw.path, line);
            else { fprintf(stderr, "%s:%d: refuse predicate NAME... | witness NAME...\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "weigh")) { double w = NUMBER(); char *nm; int any = 0;                 /* weigh N KIND...: strands of these kinds pull N as hard */
            if (w < 0 || w > 1) { fprintf(stderr, "%s:%d: a weight is between 0 and 1\n", fw.path, line); exit(2); }
            while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') { if (fw.nweigh >= FW_WEIGHS) { fprintf(stderr, "%s:%d: more weights than a firmware holds (%d)\n", fw.path, line, FW_WEIGHS); exit(2); }
                snprintf(fw.weigh_name[fw.nweigh], 96, "%s", nm); fw.weigh[fw.nweigh++] = w; any = 1; }
            if (!any) { fprintf(stderr, "%s:%d: weigh N KIND...\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "role")) { v = strtok(NULL, " \t\r\n");                               /* role by KIND | role N VALUE...: how hard a word of that kind pulls */
            if (v && !strcmp(v, "by")) { v = strtok(NULL, " \t\r\n"); if (!v) { fprintf(stderr, "%s:%d: role by KIND\n", fw.path, line); exit(2); } snprintf(fw.role_by, sizeof fw.role_by, "%s", v); }
            else { char *e_; double w = v ? strtod(v, &e_) : -1; char *nm; if (!v || *e_ || w < 0 || w > 1) { fprintf(stderr, "%s:%d: role by KIND | role N VALUE... (N between 0 and 1)\n", fw.path, line); exit(2); }
                while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') { if (fw.nrole >= FW_WEIGHS) { fprintf(stderr, "%s:%d: more roles than a firmware holds\n", fw.path, line); exit(2); }
                    snprintf(fw.role_name[fw.nrole], 96, "%s", nm); fw.role[fw.nrole++] = w; } } }
        else if (!strcmp(tok, "walks")) { fw.walks = (int)NUMBER(); if (fw.walks < 0) { fprintf(stderr, "%s:%d: walks N (0: none)\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "nearest")) { fw.nearest = (int)NUMBER(); if (fw.nearest < 0) { fprintf(stderr, "%s:%d: nearest N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "walkfan")) { fw.walk_fan = (int)NUMBER(); if (fw.walk_fan < 1) { fprintf(stderr, "%s:%d: walkfan N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "steps")) { fw.steps = (int)NUMBER(); if (fw.steps < 0) { fprintf(stderr, "%s:%d: steps N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "restart")) { fw.restart = NUMBER(); if (fw.restart < 0 || fw.restart > 1) { fprintf(stderr, "%s:%d: restart is between 0 and 1\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "sure")) { fw.sure = NUMBER(); if (fw.sure < 0) { fprintf(stderr, "%s:%d: sure N (standard errors)\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "lift")) { fw.lift = NUMBER(); if (fw.lift < 1) { fprintf(stderr, "%s:%d: lift is at least 1\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "tie")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "first")) fw.tie = FW_TIE_FIRST; else if (v && !strcmp(v, "draw")) fw.tie = FW_TIE_DRAW; else if (v && !strcmp(v, "ask")) fw.tie = FW_TIE_ASK;
            else { fprintf(stderr, "%s:%d: tie first | draw | ask\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "seed")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "observation")) fw.seed_session = 0; else if (v && !strcmp(v, "session")) fw.seed_session = 1;
            else { fprintf(stderr, "%s:%d: seed observation | session\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "elect")) { static const char *K[FW_E_KEYS] = { "grounds", "continuity", "agree", "cooccur", "walks", "shape", "confidence", "shared", "heads" }; char *nm; fw.nelect = 0;
            while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') { int k = -1; for (int i = 0; i < FW_E_KEYS; i++) if (!strcmp(nm, K[i])) k = i;
                if (k < 0 || fw.nelect >= FW_E_KEYS) { fprintf(stderr, "%s:%d: elect KEY... (grounds continuity agree heads cooccur walks shape confidence shared)\n", fw.path, line); exit(2); } fw.elect[fw.nelect++] = k; }
            if (!fw.nelect) { fprintf(stderr, "%s:%d: elect KEY...\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "shape")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "frechet")) fw.shape = FW_FRECHET; else if (v && !strcmp(v, "dtw")) fw.shape = FW_DTW;
            else if (v && !strcmp(v, "outliers")) { fw.shape = FW_OUTLIERS; fw.shape_n = NUMBER(); }
            else if (v && !strcmp(v, "edr")) { fw.shape = FW_EDR; fw.shape_n = NUMBER(); }
            else { fprintf(stderr, "%s:%d: shape frechet | outliers N | dtw | edr N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "take") && in == FW_PULL) { v = strtok(NULL, " \t\r\n"); if (fw.ntake >= FW_TAKES) { fprintf(stderr, "%s:%d: more steps than a pull takes (%d)\n", fw.path, line, FW_TAKES); exit(2); }
            int what = v && !strcmp(v, "chain") ? FW_TAKE_CHAIN : v && !strcmp(v, "fact") ? FW_TAKE_FACT : v && !strcmp(v, "segment") ? FW_TAKE_SEGMENT : v && !strcmp(v, "attestations") ? FW_TAKE_ATTESTATIONS : v && !strcmp(v, "constituents") ? FW_TAKE_CONSTITUENTS : -1;
            if (what < 0) { fprintf(stderr, "%s:%d: take fact | segment | attestations N | constituents N | chain N RELATION...\n", fw.path, line); exit(2); }
            fw.take[fw.ntake].what = what; fw.take[fw.ntake].n = what == FW_TAKE_ATTESTATIONS || what == FW_TAKE_CONSTITUENTS || what == FW_TAKE_CHAIN ? (int)NUMBER() : 1; fw.ntake++;
            if (what == FW_TAKE_CHAIN) { char *nm; fw.nalt = 1; memset(fw.nchain, 0, sizeof fw.nchain);                 /* RELATION... | RELATION...: the first chain that reaches its end */
                while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') { if (!strcmp(nm, "|")) { if (fw.nalt < FW_ALTS) fw.nalt++; continue; } int a_ = fw.nalt - 1; if (fw.nchain[a_] < FW_CHAIN) snprintf(fw.chain[a_][fw.nchain[a_]++], 96, "%s", nm); }
                if (!fw.nchain[0]) { fprintf(stderr, "%s:%d: take chain N RELATION...\n", fw.path, line); exit(2); } } }
        else if (!strcmp(tok, "up") && in == FW_TRANSLATE) { char *nm; fw.nup = 0;               /* up RELATION...: from a word to its concept, in order */
            while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') { if (fw.nup >= FW_CHAIN) { fprintf(stderr, "%s:%d: more steps than a chain holds (%d)\n", fw.path, line, FW_CHAIN); exit(2); } snprintf(fw.up[fw.nup++], 96, "%s", nm); }
            if (!fw.nup) { fprintf(stderr, "%s:%d: up RELATION...\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "language") && (in == FW_TRANSLATE || in == FW_PULL)) { char *a_ = strtok(NULL, " \t\r\n"), *b_ = strtok(NULL, " \t\r\n");
            if (!a_ || !b_) { fprintf(stderr, "%s:%d: language HELD-BY SAYS\n", fw.path, line); exit(2); } snprintf(fw.language[0], 96, "%s", a_); snprintf(fw.language[1], 96, "%s", b_); }
        else if (!strcmp(tok, "gloss") && in == FW_TRANSLATE) { v = strtok(NULL, " \t\r\n"); if (!v) { fprintf(stderr, "%s:%d: gloss RELATION\n", fw.path, line); exit(2); } snprintf(fw.gloss, sizeof fw.gloss, "%s", v); }
        else { fprintf(stderr, "%s:%d: %s is not a decision a firmware makes\n", fw.path, line, tok); exit(2); }
        #undef NUMBER
    }
    fclose(f);
    refuse_named(&fw); return fw;
}
/* Every name the firmware says, as the entity it names: the text's trunk, decomposed as any text is. A pass computes
 * them once, not again for every stage, step or strand that uses them. */
const Firmware *firmware_ids(Firmware *fw){
    if (fw->id.ready) return fw;
    lp_text *c = lp_text_new(lp_tier0_map(NULL)); if (!c) { fprintf(stderr, "cannot open ICU's break iterators\n"); exit(1); }
    #define NAMED(name) (entity_named(c, (name), NULL, 0, NULL).id)
    for (int i = 0; i < fw->nrefuse_predicate; i++) fw->id.refuse[i] = NAMED(fw->refuse_predicate[i]);
    for (int i = 0; i < fw->nonly_predicate; i++) fw->id.only[i] = NAMED(fw->only_predicate[i]);
    for (int i = 0; i < fw->nrefuse_witness; i++) fw->id.refuse_witness[i] = NAMED(fw->refuse_witness[i]);
    for (int i = 0; i < fw->nweigh; i++) fw->id.weigh[i] = NAMED(fw->weigh_name[i]);
    if (fw->role_by[0]) fw->id.role_by = NAMED(fw->role_by);
    for (int i = 0; i < fw->nrole; i++) fw->id.role[i] = NAMED(fw->role_name[i]);
    for (int a = 0; a < fw->nalt; a++) for (int i = 0; i < fw->nchain[a]; i++) fw->id.chain[a][i] = NAMED(fw->chain[a][i]);
    for (int i = 0; i < fw->nup; i++) fw->id.up[i] = NAMED(fw->up[i]);
    for (int i = 0; i < 2; i++) if (fw->language[i][0]) fw->id.language[i] = NAMED(fw->language[i]);
    if (fw->gloss[0]) fw->id.gloss = NAMED(fw->gloss);
    #undef NAMED
    lp_text_free(c); fw->id.ready = 1;
    return fw;
}
void firmware_say(const Firmware *fw, int op){
    printf("firmware   %s   for %s: k %g, lambda %g, fan %d, hops %d, top %s", fw->path, OPS[op], fw->k, fw->lambda, fw->fan, fw->hops, fw->top_within > 0 ? "within" : "always");
    if (fw->top_within > 0) printf(" %g", fw->top_within);
    if (fw->fact <= 1.0) printf(", a fact at trust %g", fw->fact);
    if (fw->nrefuse_predicate) { printf(", refuses"); for (int i = 0; i < fw->nrefuse_predicate; i++) printf(" %s", fw->refuse_predicate[i]); }
    if (fw->nweigh) printf(", weighs %d kinds of strand", fw->nweigh);
    if (op == FW_PULL) { static const char *T[] = { "first", "draw", "ask" }; printf(", walks %d of %d steps (fan %d), restart %g, sure %g, lift %g, a tie: %s, seed: %s", fw->walks, fw->steps, fw->walk_fan, fw->restart, fw->sure, fw->lift, T[fw->tie], fw->seed_session ? "session" : "observation"); }
    if (fw->nonly_predicate) { printf(", only the heads"); for (int i = 0; i < fw->nonly_predicate; i++) printf(" %s", fw->only_predicate[i]); }
    if (fw->nrefuse_witness) { printf(", refuses what is witnessed by"); for (int i = 0; i < fw->nrefuse_witness; i++) printf(" %s", fw->refuse_witness[i]); }
    printf("\n");
}
