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
    fw.k = 2.0; fw.lambda = 0.05; fw.fan = op == FW_SEARCH ? 512 : 4096; fw.hops = 8; fw.top_within = 0; fw.fact = 2.0; fw.order_witness = 1; fw.shape = FW_FRECHET;
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
        else if (!strcmp(tok, "top")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "always")) fw.top_within = 0; else if (v && !strcmp(v, "within")) fw.top_within = NUMBER();
            else { fprintf(stderr, "%s:%d: top always | within N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "order")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "witness")) fw.order_witness = 1; else if (v && !strcmp(v, "standing")) fw.order_witness = 0;
            else { fprintf(stderr, "%s:%d: order witness | standing\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "refuse")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "predicate")) names(&fw, fw.refuse_predicate, &fw.nrefuse_predicate, FW_NAMES, fw.path, line);
            else if (v && !strcmp(v, "witness")) names(&fw, fw.refuse_witness, &fw.nrefuse_witness, FW_NAMES, fw.path, line);
            else { fprintf(stderr, "%s:%d: refuse predicate NAME... | witness NAME...\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "shape")) { v = strtok(NULL, " \t\r\n");
            if (v && !strcmp(v, "frechet")) fw.shape = FW_FRECHET; else if (v && !strcmp(v, "dtw")) fw.shape = FW_DTW;
            else if (v && !strcmp(v, "outliers")) { fw.shape = FW_OUTLIERS; fw.shape_n = NUMBER(); }
            else if (v && !strcmp(v, "edr")) { fw.shape = FW_EDR; fw.shape_n = NUMBER(); }
            else { fprintf(stderr, "%s:%d: shape frechet | outliers N | dtw | edr N\n", fw.path, line); exit(2); } }
        else if (!strcmp(tok, "take") && in == FW_PULL) { v = strtok(NULL, " \t\r\n"); if (fw.ntake >= FW_TAKES) { fprintf(stderr, "%s:%d: more steps than a pull takes (%d)\n", fw.path, line, FW_TAKES); exit(2); }
            int what = v && !strcmp(v, "fact") ? FW_TAKE_FACT : v && !strcmp(v, "segment") ? FW_TAKE_SEGMENT : v && !strcmp(v, "attestations") ? FW_TAKE_ATTESTATIONS : v && !strcmp(v, "constituents") ? FW_TAKE_CONSTITUENTS : -1;
            if (what < 0) { fprintf(stderr, "%s:%d: take fact | segment | attestations N | constituents N\n", fw.path, line); exit(2); }
            fw.take[fw.ntake].what = what; fw.take[fw.ntake].n = what == FW_TAKE_ATTESTATIONS || what == FW_TAKE_CONSTITUENTS ? (int)NUMBER() : 1; fw.ntake++; }
        else { fprintf(stderr, "%s:%d: %s is not a decision a firmware makes\n", fw.path, line, tok); exit(2); }
        #undef NUMBER
    }
    fclose(f);
    return fw;
}
void firmware_say(const Firmware *fw, int op){
    printf("firmware   %s   for %s: k %g, lambda %g, fan %d, hops %d, top %s", fw->path, OPS[op], fw->k, fw->lambda, fw->fan, fw->hops, fw->top_within > 0 ? "within" : "always");
    if (fw->top_within > 0) printf(" %g", fw->top_within);
    if (fw->fact <= 1.0) printf(", a fact at trust %g", fw->fact);
    if (fw->nrefuse_predicate) { printf(", refuses"); for (int i = 0; i < fw->nrefuse_predicate; i++) printf(" %s", fw->refuse_predicate[i]); }
    if (fw->nrefuse_witness) { printf(", refuses what is witnessed by"); for (int i = 0; i < fw->nrefuse_witness; i++) printf(" %s", fw->refuse_witness[i]); }
    printf("\n");
}
