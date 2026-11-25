/* The personality firmware: a pull's decisions, read from a file and never from the records. The knowledge is the
 * records; how they are navigated and how an answer is reached is the firmware, one for each human being
 * (Semantics: Personality firmware). What the file can say is described in firmware/program.firmware.
 *
 *   $LAPLACE_FIRMWARE, or --firmware FILE on a command: the firmware a pull runs under */
#include "query.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *OPS[FW_OPS] = { "hop", "search", "translate", "follows", "pull" };

const char *firmware_path(void){
    const char *e = getenv("LAPLACE_FIRMWARE"); if (e && *e) return e;
    static char p[4200]; snprintf(p, sizeof p, "%s/../firmware/program.firmware", laplace_recipes()); return p;
}

static int fw_fail(char *err, size_t err_cap, int code, const char *fmt, ...){
    if (err && err_cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_cap, fmt, ap);
        va_end(ap);
    }
    return code;
}

static int names_chk(char (*into)[96], int *n, int cap, const char *path, int line, char *err, size_t err_cap){
    char *tok; int any = 0;
    while ((tok = strtok(NULL, " \t\r\n"))) {
        if (tok[0] == '#') break;
        if (*n >= cap) return fw_fail(err, err_cap, 2, "%s:%d: more names than a firmware holds (%d)\n", path, line, cap);
        if (tok[0] == '"') {
            char nm[96]; size_t l = (size_t)snprintf(nm, sizeof nm, "%s", tok + 1);
            while ((!l || nm[l - 1] != '"') && (tok = strtok(NULL, " \t\r\n")))
                l += (size_t)snprintf(nm + l, l < sizeof nm ? sizeof nm - l : 0, " %s", tok);
            if (l && l < sizeof nm && nm[l - 1] == '"') nm[--l] = 0;
            snprintf(into[(*n)++], 96, "%s", nm); any = 1; continue;
        }
        snprintf(into[(*n)++], 96, "%s", tok); any = 1;
    }
    if (!any) return fw_fail(err, err_cap, 2, "%s:%d: nothing is named\n", path, line);
    return 0;
}

static int parse_number(const char *path, int line, const char *tok, double *out, char *err, size_t err_cap){
    char *v = strtok(NULL, " \t\r\n");
    if (!v) return fw_fail(err, err_cap, 2, "%s:%d: %s N\n", path, line, tok);
    char *e_ = NULL;
    double d_ = strtod(v, &e_);
    if (*e_) return fw_fail(err, err_cap, 2, "%s:%d: %s is not a number\n", path, line, v);
    *out = d_;
    return 0;
}

static int parse_toggle(const char *path, int line, const char *tok, int *out, char *err, size_t err_cap){
    char *v = strtok(NULL, " \t\r\n");
    if (!v) return fw_fail(err, err_cap, 2, "%s:%d: %s on | off\n", path, line, tok);
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes")) { *out = 1; return 0; }
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no")) { *out = 0; return 0; }
    return fw_fail(err, err_cap, 2, "%s:%d: %s on | off\n", path, line, tok);
}

static int firmware_parse(const char *path, int op, Firmware *out, char *err, size_t err_cap){
    Firmware fw; memset(&fw, 0, sizeof fw);
    fw.k = 2.0; fw.lambda = 0.05; fw.fan = op == FW_SEARCH ? 512 : 4096; fw.hops = 8; fw.emit = 32;
    fw.top_within = 0; fw.fact = 2.0; fw.order_witness = 1; fw.shape = FW_FRECHET;
    fw.walks = 64; fw.steps = 4; fw.walk_fan = 512; fw.nearest = 512; fw.rounds = 2; fw.breadth = 64;
    fw.restart = 0.25; fw.sure = 2.0; fw.lift = 2.0; fw.tie = FW_TIE_FIRST;
    fw.ooda_rounds = 0; fw.salience_floor = 0.0; fw.multi_tier = 1; fw.combine_clauses = 1;
    {
        static const int E[] = { FW_E_GROUNDS, FW_E_CONTINUITY, FW_E_AGREE, FW_E_HEADS, FW_E_COOCCUR, FW_E_WALKS, FW_E_SHAPE, FW_E_CONFIDENCE, FW_E_SHARED };
        memcpy(fw.elect, E, sizeof E); fw.nelect = FW_E_KEYS;
    }
    snprintf(fw.path, sizeof fw.path, "%s", path && *path ? path : firmware_path());
    FILE *f = fopen(fw.path, "r");
    if (!f && path && *path && !strchr(path, '/') && !strchr(path, '\\')) {
        char base[4200];
        snprintf(base, sizeof base, "%s", firmware_path());
        char *slash = strrchr(base, '/'), *bslash = strrchr(base, '\\');
        if (bslash > slash) slash = bslash;
        if (slash) {
            *slash = 0;
            size_t plen = strlen(path);
            int has_ext = plen > 9 && strcmp(path + plen - 9, ".firmware") == 0;
            char alt[4300];
            snprintf(alt, sizeof alt, "%s/%s%s", base, path, has_ext ? "" : ".firmware");
            f = fopen(alt, "r");
            if (f) snprintf(fw.path, sizeof fw.path, "%s", alt);
        }
    }
    if (!f) {
        char alt[4200];
        snprintf(alt, sizeof alt, "%s/program.firmware", fw.path);
        f = fopen(alt, "r");
        if (f) snprintf(fw.path, sizeof fw.path, "%s", alt);
    }
    if (!f) return fw_fail(err, err_cap, 1, "%s: cannot open firmware (LAPLACE_FIRMWARE names the firmware a pull runs under)\n", fw.path);

    char buf[1024]; int line = 0, in = -1, rc = 0;
    while (!rc && fgets(buf, sizeof buf, f)) {
        line++; char *c = buf; while (*c == ' ' || *c == '\t') c++;
        if (*c == '#' || *c == '\n' || *c == '\r' || !*c) continue;
        char *tok = strtok(c, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "for")) {
            tok = strtok(NULL, " \t\r\n"); in = -2;
            for (int i = 0; tok && i < FW_OPS; i++) if (!strcmp(tok, OPS[i])) in = i;
            if (in == -2) rc = fw_fail(err, err_cap, 2, "%s:%d: %s is not an operation of the forward pass (hop, search, translate, follows, pull)\n", fw.path, line, tok ? tok : "");
            continue;
        }
        if (in >= 0 && in != op) continue;
        char *v = NULL; double nv = 0.0;
        #define NUM(dst) do { rc = parse_number(fw.path, line, tok, &nv, err, err_cap); if (rc) goto done; (dst) = nv; } while (0)
        if (!strcmp(tok, "k")) NUM(fw.k);
        else if (!strcmp(tok, "lambda")) NUM(fw.lambda);
        else if (!strcmp(tok, "fan")) { NUM(nv); fw.fan = (int)nv; }
        else if (!strcmp(tok, "hops")) { NUM(nv); fw.hops = (int)nv; }
        else if (!strcmp(tok, "fact")) NUM(fw.fact);
        else if (!strcmp(tok, "emit")) { NUM(nv); fw.emit = (int)nv; }
        else if (!strcmp(tok, "enough")) { NUM(fw.enough); if (fw.enough < 0 || fw.enough > 1) rc = fw_fail(err, err_cap, 2, "%s:%d: enough is between 0 and 1\n", fw.path, line); }
        else if (!strcmp(tok, "ooda")) { NUM(nv); if (nv < 0) rc = fw_fail(err, err_cap, 2, "%s:%d: ooda N (0 or positive)\n", fw.path, line); else fw.ooda_rounds = (int)nv; }
        else if (!strcmp(tok, "salience")) { NUM(fw.salience_floor); if (fw.salience_floor < 0) rc = fw_fail(err, err_cap, 2, "%s:%d: salience N (>= 0)\n", fw.path, line); }
        else if (!strcmp(tok, "multitier")) rc = parse_toggle(fw.path, line, tok, &fw.multi_tier, err, err_cap);
        else if (!strcmp(tok, "combine")) rc = parse_toggle(fw.path, line, tok, &fw.combine_clauses, err, err_cap);
        else if (!strcmp(tok, "top")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "always")) fw.top_within = 0;
            else if (v && !strcmp(v, "within")) NUM(fw.top_within);
            else rc = fw_fail(err, err_cap, 2, "%s:%d: top always | within N\n", fw.path, line);
        } else if (!strcmp(tok, "order")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "witness")) fw.order_witness = 1;
            else if (v && !strcmp(v, "standing")) fw.order_witness = 0;
            else rc = fw_fail(err, err_cap, 2, "%s:%d: order witness | standing\n", fw.path, line);
        } else if (!strcmp(tok, "prevalence")) {
            char *rest = strtok(NULL, "\r\n"); while (rest && (*rest == ' ' || *rest == '\t')) rest++;
            if (!rest || !*rest) rc = fw_fail(err, err_cap, 2, "%s:%d: prevalence RELATION\n", fw.path, line);
            else {
                snprintf(fw.prevalence, sizeof fw.prevalence, "%s", rest);
                fw.prevalence_pair = !strcmp(rest, "pair");
                size_t L = strlen(fw.prevalence);
                while (L && (fw.prevalence[L - 1] == ' ' || fw.prevalence[L - 1] == '\t')) fw.prevalence[--L] = 0;
            }
        } else if (!strcmp(tok, "only")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "predicate")) rc = names_chk(fw.only_predicate, &fw.nonly_predicate, FW_NAMES, fw.path, line, err, err_cap);
            else rc = fw_fail(err, err_cap, 2, "%s:%d: only predicate NAME...\n", fw.path, line);
        } else if (!strcmp(tok, "refuse")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "predicate")) rc = names_chk(fw.refuse_predicate, &fw.nrefuse_predicate, FW_NAMES, fw.path, line, err, err_cap);
            else if (v && !strcmp(v, "witness")) rc = names_chk(fw.refuse_witness, &fw.nrefuse_witness, FW_NAMES, fw.path, line, err, err_cap);
            else rc = fw_fail(err, err_cap, 2, "%s:%d: refuse predicate NAME... | witness NAME...\n", fw.path, line);
        } else if (!strcmp(tok, "negate")) rc = names_chk(fw.negate, &fw.nnegate, FW_NAMES, fw.path, line, err, err_cap);
        else if (!strcmp(tok, "negate_relation")) rc = names_chk(fw.negate_relation, &fw.nnegate_relation, FW_NAMES, fw.path, line, err, err_cap);
        else if (!strcmp(tok, "scope_stop")) rc = names_chk(fw.scope_stop, &fw.nscope_stop, FW_NAMES, fw.path, line, err, err_cap);
        else if (!strcmp(tok, "weigh")) {
            double w; NUM(w); char *nm; int any = 0;
            if (w < 0 || w > 1) { rc = fw_fail(err, err_cap, 2, "%s:%d: a weight is between 0 and 1\n", fw.path, line); goto done; }
            while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') {
                if (fw.nweigh >= FW_WEIGHS) { rc = fw_fail(err, err_cap, 2, "%s:%d: more weights than a firmware holds (%d)\n", fw.path, line, FW_WEIGHS); goto done; }
                snprintf(fw.weigh_name[fw.nweigh], 96, "%s", nm); fw.weigh[fw.nweigh++] = w; any = 1;
            }
            if (!any) rc = fw_fail(err, err_cap, 2, "%s:%d: weigh N KIND...\n", fw.path, line);
        } else if (!strcmp(tok, "role")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "by")) {
                v = strtok(NULL, " \t\r\n");
                if (!v) rc = fw_fail(err, err_cap, 2, "%s:%d: role by KIND\n", fw.path, line);
                else snprintf(fw.role_by, sizeof fw.role_by, "%s", v);
            } else {
                char *e_ = NULL; double w = v ? strtod(v, &e_) : -1; char *nm;
                if (!v || *e_ || w < 0 || w > 1) rc = fw_fail(err, err_cap, 2, "%s:%d: role by KIND | role N VALUE... (N between 0 and 1)\n", fw.path, line);
                else while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') {
                    if (fw.nrole >= FW_WEIGHS) { rc = fw_fail(err, err_cap, 2, "%s:%d: more roles than a firmware holds\n", fw.path, line); goto done; }
                    snprintf(fw.role_name[fw.nrole], 96, "%s", nm); fw.role[fw.nrole++] = w;
                }
            }
        } else if (!strcmp(tok, "walks")) { NUM(nv); if (nv < 0) rc = fw_fail(err, err_cap, 2, "%s:%d: walks N (0: none)\n", fw.path, line); else fw.walks = (int)nv; }
        else if (!strcmp(tok, "rounds")) { NUM(nv); if (nv < 1) rc = fw_fail(err, err_cap, 2, "%s:%d: rounds N (at least 1)\n", fw.path, line); else fw.rounds = (int)nv; }
        else if (!strcmp(tok, "breadth")) { NUM(nv); if (nv < 1) rc = fw_fail(err, err_cap, 2, "%s:%d: breadth N\n", fw.path, line); else fw.breadth = (int)nv; }
        else if (!strcmp(tok, "nearest")) { NUM(nv); if (nv < 0) rc = fw_fail(err, err_cap, 2, "%s:%d: nearest N\n", fw.path, line); else fw.nearest = (int)nv; }
        else if (!strcmp(tok, "walkfan")) { NUM(nv); if (nv < 1) rc = fw_fail(err, err_cap, 2, "%s:%d: walkfan N\n", fw.path, line); else fw.walk_fan = (int)nv; }
        else if (!strcmp(tok, "steps")) { NUM(nv); if (nv < 0) rc = fw_fail(err, err_cap, 2, "%s:%d: steps N\n", fw.path, line); else fw.steps = (int)nv; }
        else if (!strcmp(tok, "restart")) { NUM(fw.restart); if (fw.restart < 0 || fw.restart > 1) rc = fw_fail(err, err_cap, 2, "%s:%d: restart is between 0 and 1\n", fw.path, line); }
        else if (!strcmp(tok, "sure")) { NUM(fw.sure); if (fw.sure < 0) rc = fw_fail(err, err_cap, 2, "%s:%d: sure N (standard errors)\n", fw.path, line); }
        else if (!strcmp(tok, "lift")) { NUM(fw.lift); if (fw.lift < 1) rc = fw_fail(err, err_cap, 2, "%s:%d: lift is at least 1\n", fw.path, line); }
        else if (!strcmp(tok, "tie")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "first")) fw.tie = FW_TIE_FIRST;
            else if (v && !strcmp(v, "draw")) fw.tie = FW_TIE_DRAW;
            else if (v && !strcmp(v, "ask")) fw.tie = FW_TIE_ASK;
            else rc = fw_fail(err, err_cap, 2, "%s:%d: tie first | draw | ask\n", fw.path, line);
        } else if (!strcmp(tok, "seed")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "observation")) fw.seed_session = 0;
            else if (v && !strcmp(v, "session")) fw.seed_session = 1;
            else rc = fw_fail(err, err_cap, 2, "%s:%d: seed observation | session\n", fw.path, line);
        } else if (!strcmp(tok, "elect")) {
            static const char *K[FW_E_KEYS] = { "grounds", "continuity", "agree", "cooccur", "walks", "shape", "confidence", "shared", "heads" };
            char *nm; fw.nelect = 0;
            while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') {
                int k = -1; for (int i = 0; i < FW_E_KEYS; i++) if (!strcmp(nm, K[i])) k = i;
                if (k < 0 || fw.nelect >= FW_E_KEYS) { rc = fw_fail(err, err_cap, 2, "%s:%d: elect KEY... (grounds continuity agree heads cooccur walks shape confidence shared)\n", fw.path, line); goto done; }
                fw.elect[fw.nelect++] = k;
            }
            if (!fw.nelect) rc = fw_fail(err, err_cap, 2, "%s:%d: elect KEY...\n", fw.path, line);
        } else if (!strcmp(tok, "shape")) {
            v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "frechet")) fw.shape = FW_FRECHET;
            else if (v && !strcmp(v, "dtw")) fw.shape = FW_DTW;
            else if (v && !strcmp(v, "outliers")) { fw.shape = FW_OUTLIERS; NUM(fw.shape_n); }
            else if (v && !strcmp(v, "edr")) { fw.shape = FW_EDR; NUM(fw.shape_n); }
            else rc = fw_fail(err, err_cap, 2, "%s:%d: shape frechet | outliers N | dtw | edr N\n", fw.path, line);
        } else if (!strcmp(tok, "take") && in == FW_PULL) {
            v = strtok(NULL, " \t\r\n");
            if (fw.ntake >= FW_TAKES) { rc = fw_fail(err, err_cap, 2, "%s:%d: more steps than a pull takes (%d)\n", fw.path, line, FW_TAKES); goto done; }
            int what = v && !strcmp(v, "chain") ? FW_TAKE_CHAIN : v && !strcmp(v, "fact") ? FW_TAKE_FACT : v && !strcmp(v, "segment") ? FW_TAKE_SEGMENT : v && !strcmp(v, "attestations") ? FW_TAKE_ATTESTATIONS : v && !strcmp(v, "constituents") ? FW_TAKE_CONSTITUENTS : -1;
            if (what < 0) { rc = fw_fail(err, err_cap, 2, "%s:%d: take fact | segment | attestations N | constituents N | chain N RELATION...\n", fw.path, line); goto done; }
            fw.take[fw.ntake].what = what;
            if (what == FW_TAKE_ATTESTATIONS || what == FW_TAKE_CONSTITUENTS || what == FW_TAKE_CHAIN) { NUM(nv); fw.take[fw.ntake].n = (int)nv; }
            else fw.take[fw.ntake].n = 1;
            fw.ntake++;
            if (what == FW_TAKE_CHAIN) {
                char *nm; fw.nalt = 1; memset(fw.nchain, 0, sizeof fw.nchain);
                while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') {
                    if (!strcmp(nm, "|")) { if (fw.nalt < FW_ALTS) fw.nalt++; continue; }
                    int a_ = fw.nalt - 1;
                    if (fw.nchain[a_] < FW_CHAIN) snprintf(fw.chain[a_][fw.nchain[a_]++], 96, "%s", nm);
                }
                if (!fw.nchain[0]) rc = fw_fail(err, err_cap, 2, "%s:%d: take chain N RELATION...\n", fw.path, line);
            }
        } else if (!strcmp(tok, "lexicalization") && (in == FW_TRANSLATE || in == FW_PULL)) {
            int k_ = 0; char *nm;
            while (k_ < 4 && (nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') snprintf(fw.lexical[k_++], 96, "%s", nm);
            if (k_ != 4) rc = fw_fail(err, err_cap, 2, "%s:%d: lexicalization RECIPE THING LANGUAGE CONCEPT\n", fw.path, line);
        } else if (!strcmp(tok, "gloss") && (in == FW_TRANSLATE || in == FW_PULL)) {
            char *a_ = strtok(NULL, " \t\r\n"), *b_ = strtok(NULL, " \t\r\n");
            if (!a_ || !b_ || a_[0] == '#' || b_[0] == '#') rc = fw_fail(err, err_cap, 2, "%s:%d: gloss RECIPE NAME\n", fw.path, line);
            else { snprintf(fw.gloss[0], 96, "%s", a_); snprintf(fw.gloss[1], 96, "%s", b_); }
        } else if (!strcmp(tok, "usages") && (in == FW_TRANSLATE || in == FW_PULL)) {
            char *a_ = strtok(NULL, " \t\r\n"), *b_ = strtok(NULL, " \t\r\n");
            if (!a_ || !b_ || a_[0] == '#' || b_[0] == '#') rc = fw_fail(err, err_cap, 2, "%s:%d: usages RECIPE NAME\n", fw.path, line);
            else { snprintf(fw.usages[0], 96, "%s", a_); snprintf(fw.usages[1], 96, "%s", b_); }
        } else if (!strcmp(tok, "equivalent") && in == FW_TRANSLATE) {
            char *nm; int k_ = 0;
            while (k_ < 5 && (nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') snprintf(fw.equiv[k_++], 96, "%s", nm);
            if (k_ < 2) rc = fw_fail(err, err_cap, 2, "%s:%d: equivalent RECIPE NAME...\n", fw.path, line);
            else fw.nequiv = k_ - 1;
        } else if ((!strcmp(tok, "up") || !strcmp(tok, "language")) && in == FW_TRANSLATE) {
            rc = fw_fail(err, err_cap, 2, "%s:%d: %s: translate reads its way from the recipe's own declarations: lexicalization RECIPE THING LANGUAGE CONCEPT\n", fw.path, line, tok);
        } else if (!strcmp(tok, "up") && in == FW_PULL) {
            char *nm; fw.nup = 0;
            while ((nm = strtok(NULL, " \t\r\n")) && nm[0] != '#') {
                if (fw.nup >= FW_CHAIN) { rc = fw_fail(err, err_cap, 2, "%s:%d: more steps than a chain holds (%d)\n", fw.path, line, FW_CHAIN); goto done; }
                snprintf(fw.up[fw.nup++], 96, "%s", nm);
            }
            if (!fw.nup) rc = fw_fail(err, err_cap, 2, "%s:%d: up RELATION...\n", fw.path, line);
        } else if (!strcmp(tok, "language") && in == FW_PULL) {
            char *a_ = strtok(NULL, " \t\r\n"), *b_ = strtok(NULL, " \t\r\n");
            if (!a_ || !b_) rc = fw_fail(err, err_cap, 2, "%s:%d: language HELD-BY SAYS\n", fw.path, line);
            else { snprintf(fw.language[0], 96, "%s", a_); snprintf(fw.language[1], 96, "%s", b_); }
        } else {
            rc = fw_fail(err, err_cap, 2, "%s:%d: %s is not a decision a firmware makes\n", fw.path, line, tok);
        }
        #undef NUM
    }
done:
    fclose(f);
    if (rc) return rc;
    *out = fw;
    return 0;
}

Firmware firmware_for(const char *path, int op){
    Firmware fw; char err[1024] = "";
    int rc = firmware_parse(path, op, &fw, err, sizeof err);
    if (rc) { fputs(err, stderr); exit(rc); }
    refuse_named(&fw);
    return fw;
}

static int firmware_name_chk(lp_text *c, const Firmware *fw, const char *name, lp_id *out, char *err, size_t err_cap){
    const char *text = name;
    if (name[0] == '@') {
        const char *slash = strchr(name + 1, '/');
        if (!slash || slash == name + 1 || !slash[1])
            return fw_fail(err, err_cap, 2, "%s: invalid relation reference %s; use @RECIPE/FIELD\n", fw->path, name);
        Recipe *r; int nr = recipes_load(laplace_recipes(), &r); text = NULL;
        for (int i = 0; i < nr; i++) {
            if (!r[i].broken && strlen(r[i].name) == (size_t)(slash - name - 1)
                && !memcmp(r[i].name, name + 1, (size_t)(slash - name - 1))) {
                text = say_relation(&r[i], slash + 1);
                break;
            }
        }
        if (!text)
            return fw_fail(err, err_cap, 2, "%s: %s does not name a loaded recipe's declared relation\n", fw->path, name);
    }
    *out = entity_named(c, text, NULL, 0, NULL).id;
    return 0;
}

int firmware_ids_checked(Firmware *fw, char *err, size_t err_cap){
    if (fw->id.ready) return 0;
    lp_text *c = lp_text_new(lp_tier0_map(NULL));
    if (!c) return fw_fail(err, err_cap, 1, "cannot open ICU's break iterators\n");
    #define NAMED(name) (entity_named(c, (name), NULL, 0, NULL).id)
    int rc = 0;
    for (int i = 0; i < fw->nrefuse_predicate; i++) fw->id.refuse[i] = NAMED(fw->refuse_predicate[i]);
    for (int i = 0; i < fw->nonly_predicate; i++) fw->id.only[i] = NAMED(fw->only_predicate[i]);
    for (int i = 0; i < fw->nrefuse_witness; i++) fw->id.refuse_witness[i] = NAMED(fw->refuse_witness[i]);
    for (int i = 0; i < fw->nnegate; i++) fw->id.negate[i] = NAMED(fw->negate[i]);
    for (int i = 0; !rc && i < fw->nnegate_relation; i++) rc = firmware_name_chk(c, fw, fw->negate_relation[i], &fw->id.negate_relation[i], err, err_cap);
    for (int i = 0; i < fw->nscope_stop; i++) fw->id.scope_stop[i] = NAMED(fw->scope_stop[i]);
    for (int i = 0; i < fw->nweigh; i++) fw->id.weigh[i] = NAMED(fw->weigh_name[i]);
    if (fw->role_by[0]) fw->id.role_by = NAMED(fw->role_by);
    for (int i = 0; i < fw->nrole; i++) fw->id.role[i] = NAMED(fw->role_name[i]);
    for (int a = 0; !rc && a < fw->nalt; a++)
        for (int i = 0; !rc && i < fw->nchain[a]; i++)
            rc = firmware_name_chk(c, fw, fw->chain[a][i], &fw->id.chain[a][i], err, err_cap);
    for (int i = 0; !rc && i < fw->nup; i++)
        rc = firmware_name_chk(c, fw, fw->up[i], &fw->id.up[i], err, err_cap);
    for (int i = 0; i < 2; i++) if (fw->language[i][0]) fw->id.language[i] = NAMED(fw->language[i]);
    if (fw->prevalence[0] && !fw->prevalence_pair) fw->id.prevalence = NAMED(fw->prevalence);
    #undef NAMED
    if (!rc) {
        size_t sz = 0; const void *bytes = lp_map_file(fw->path, 0, &sz);
        if (!bytes) rc = fw_fail(err, err_cap, 1, "%s: cannot map firmware bytes\n", fw->path);
        else {
            fw->id.image_id = lp_text_decompose(c, (const uint8_t *)bytes, sz, NULL, NULL).id;
            lp_unmap_file(bytes, sz);
        }
    }
    lp_text_free(c);
    if (!rc) fw->id.ready = 1;
    return rc;
}

const Firmware *firmware_ids(Firmware *fw){
    char err[1024] = "";
    int rc = firmware_ids_checked(fw, err, sizeof err);
    if (rc) { fputs(err, stderr); exit(rc); }
    return fw;
}

int firmware_load_checked(const char *path, int op, Firmware *out, char *err, size_t err_cap){
    if (!out) return 2;
    int rc = firmware_parse(path, op, out, err, err_cap);
    if (rc) return rc;
    rc = firmware_ids_checked(out, err, err_cap);
    if (rc) return rc;
    return 0;
}

static void binding_json(const char *name, const lp_id *id){
    char hex[33]; lp_id_hex(id, hex); fputs("{\"name\":", stdout); lp_fput_json(stdout, name);
    printf(",\"entity\":\"%s\"}", hex);
}

int cmd_firmware(int argc, char **argv){
    const char *path = NULL, *operation = "pull";
    int a = opts(argc, argv, (const Opt[]){ { .flag="--firmware", .kind='s', .at=&path },
        { .flag="--operation", .kind='s', .at=&operation }, { 0 } });
    int op = 0; while (op < FW_OPS && strcmp(OPS[op], operation)) op++;
    if (a != argc || op == FW_OPS) { fprintf(stderr, "usage: laplace firmware [--firmware FILE] [--operation hop|search|translate|follows|pull]\n"); return 2; }
    Firmware fw = firmware_for(path, op); tier0_open(NULL); firmware_ids(&fw);
    char fid[33]; lp_id_hex(&fw.id.image_id, fid);
    printf("{\"firmware_id\":\"%s\",\"operation\":", fid); lp_fput_json(stdout, operation); fputs(",\"up\":[", stdout);
    for (int i=0; i<fw.nup; i++) { if (i) putchar(','); binding_json(fw.up[i], &fw.id.up[i]); }
    fputs("],\"chains\":[", stdout);
    for (int a_=0; a_<fw.nalt; a_++) { if (a_) putchar(','); putchar('[');
        for (int i=0; i<fw.nchain[a_]; i++) { if (i) putchar(','); binding_json(fw.chain[a_][i], &fw.id.chain[a_][i]); }
        putchar(']'); }
    puts("]}"); return 0;
}

void firmware_say(const Firmware *fw, int op){
    printf("firmware   %s   for %s: k %g, lambda %g, fan %d, hops %d, top %s", fw->path, OPS[op], fw->k, fw->lambda, fw->fan, fw->hops, fw->top_within > 0 ? "within" : "always");
    if (fw->top_within > 0) printf(" %g", fw->top_within);
    if (fw->fact <= 1.0) printf(", a fact at trust %g", fw->fact);
    if (fw->nrefuse_predicate) { printf(", refuses"); for (int i = 0; i < fw->nrefuse_predicate; i++) printf(" %s", fw->refuse_predicate[i]); }
    if (fw->nweigh) printf(", weighs %d kinds of strand", fw->nweigh);
    if (op == FW_PULL) {
        static const char *T[] = { "first", "draw", "ask" };
        printf(", rounds %d of breadth %d, walks %d of %d steps (fan %d), restart %g, sure %g, lift %g, a tie: %s, seed: %s, ooda %d, multitier %d, combine %d",
               fw->rounds, fw->breadth, fw->walks, fw->steps, fw->walk_fan, fw->restart, fw->sure, fw->lift, T[fw->tie],
               fw->seed_session ? "session" : "observation", fw->ooda_rounds, fw->multi_tier, fw->combine_clauses);
    }
    if (fw->nonly_predicate) { printf(", only the heads"); for (int i = 0; i < fw->nonly_predicate; i++) printf(" %s", fw->only_predicate[i]); }
    if (fw->nrefuse_witness) { printf(", refuses what is witnessed by"); for (int i = 0; i < fw->nrefuse_witness; i++) printf(" %s", fw->refuse_witness[i]); }
    printf("\n");
}
