/* laplace flags: the flags that go with tier 0, generated natively from the Unicode data.
 *   laplace flags [-u UCD_ROOT] [-o tier0.flags]
 * The standard lists its own properties (PropertyAliases.txt) and, for each enumerated property, its own values
 * (PropertyValueAliases.txt). Those lists are the enums: a property is a field, a value is its place in the standard's
 * list. Every codepoint gets one 256-bit record: a bit for each binary property in the order the standard lists them,
 * then a field for each enumerated property, as wide as its list needs. The values come from the character database in
 * XML, which covers the whole codespace. The layout is written beside the records, and the records have a fingerprint,
 * so an install knows which flags it has. */
#define _GNU_SOURCE
#include "engine.h"
#include "blake3.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[48], say[64]; } Value;
typedef struct { char name[32], say[64]; int binary; Value *v; int nv, bit, width; uint64_t seen, unknown, *count; } Prop;
static Prop *prop; static int nprop;

static char *trim(char *s){ while (*s == ' ' || *s == '\t') s++; size_t l = strlen(s); while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = 0; return s; }
static FILE *open_in(const char *root, const char *rel){ char p[4096]; snprintf(p, sizeof p, "%s/%s", root, rel); FILE *f = fopen(p, "r"); if (!f) { perror(p); exit(1); } return f; }
static Prop *prop_named(const char *n, size_t l){ for (int i = 0; i < nprop; i++) if (strlen(prop[i].name) == l && !memcmp(prop[i].name, n, l)) return &prop[i]; return NULL; }

/* The standard's own rule for matching a value's name (UAX #44, LM3): case, spaces, underscores and hyphens do not count. */
static int same(const char *a, const char *b, size_t bl){
    size_t i = 0, j = 0, al = strlen(a);
    for (;;) {
        while (i < al && (a[i] == '_' || a[i] == '-' || a[i] == ' ')) i++;
        while (j < bl && (b[j] == '_' || b[j] == '-' || b[j] == ' ')) j++;
        if (i == al || j == bl) return i == al && j == bl;
        if ((a[i] | 32) != (b[j] | 32)) return 0;
        i++; j++;
    }
}
static void set_bits(uint8_t *rec, int bit, int width, uint32_t v){ for (int i = 0; i < width; i++) if (v >> i & 1) rec[(bit + i) >> 3] |= (uint8_t)(1u << ((bit + i) & 7)); }

int cmd_flags(int argc, char **argv){
    char dflt[4096], out_dflt[4096]; snprintf(dflt, sizeof dflt, "%s/Public/UCD/latest", laplace_ucd());
    snprintf(out_dflt, sizeof out_dflt, "%s", lp_tier0_path()); { char *d = strrchr(out_dflt, '.'); if (d) *d = 0; strcat(out_dflt, ".flags"); }
    const char *root = dflt, *outp = out_dflt;
    if (opts(argc, argv, (const Opt[]){ { "-u", 's', &root }, { "-o", 's', &outp }, { NULL } }) < argc) { fprintf(stderr, "usage: laplace flags [-u UCD_ROOT] [-o tier0.flags]\n"); return 2; }
    setlocale(LC_NUMERIC, "en_US.UTF-8"); double T = now(); char line[1 << 16];

    /* the properties the standard lists, binary and enumerated, in its order */
    FILE *f = open_in(root, "ucd/PropertyAliases.txt"); int kind = -1;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') { if (strstr(line, "Properties") && !strchr(line, '=')) kind = strstr(line, "Binary") ? 1 : (strstr(line, "Enumerated") || strstr(line, "Catalog")) ? 0 : -1; continue; }
        char *h = strchr(line, '#'); if (h) *h = 0; if (!*trim(line) || kind < 0) continue;
        char *a = trim(strtok(line, ";")), *b = strtok(NULL, ";"); if (!b) continue; b = trim(b);
        prop = xrealloc(prop, sizeof(Prop) * (size_t)(nprop + 1)); Prop *p = &prop[nprop++]; memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", a); snprintf(p->say, sizeof p->say, "%s", b); p->binary = kind;
    }
    fclose(f);
    /* each enumerated property's values, in the standard's order */
    f = open_in(root, "ucd/PropertyValueAliases.txt");
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0; if (!*trim(line)) continue;
        char *fld[5]; int nf = 0; for (char *t = strtok(line, ";"); t && nf < 5; t = strtok(NULL, ";")) fld[nf++] = trim(t);
        if (nf < 3) continue; Prop *p = prop_named(fld[0], strlen(fld[0])); if (!p || p->binary) continue;
        int ccc = !strcmp(fld[0], "ccc");                                    /* ccc is written as its number */
        p->v = xrealloc(p->v, sizeof(Value) * (size_t)(p->nv + 1)); Value *v = &p->v[p->nv++];
        snprintf(v->name, sizeof v->name, "%s", fld[1]); snprintf(v->say, sizeof v->say, "%s", ccc && nf > 3 ? fld[3] : fld[2]);
    }
    fclose(f);
    int bit = 0, nbin = 0, nenum = 0;
    for (int i = 0; i < nprop; i++) if (prop[i].binary) { prop[i].bit = bit++; prop[i].width = 1; nbin++; }
    for (int i = 0; i < nprop; i++) if (!prop[i].binary) {
        int w = 1; while ((1 << w) < prop[i].nv) w++;
        prop[i].bit = bit; prop[i].width = w; bit += w; nenum++; prop[i].count = calloc((size_t)prop[i].nv + 1, 8);
    }
    if (bit > 256) { fprintf(stderr, "the standard's properties need %d bits: more than a 256-bit record holds\n", bit); return 1; }
    printf("laplace flags   %s\n  %d binary properties, %d enumerated: %d of 256 bits   (%.2f s)\n", root, nbin, nenum, bit, now() - T);

    /* every codepoint's values, from the character database in XML */
    uint8_t *rec = calloc(LP_NCP, 32); uint8_t *have = calloc(LP_NCP, 1); uint64_t covered = 0, elements = 0;
    f = open_in(root, "ucdxml/ucd.all.flat.xml"); char *ln = NULL; size_t cap = 0; ssize_t got;
    while ((got = getline(&ln, &cap, f)) > 0) {
        char *e = ln; while (*e == ' ' || *e == '\t') e++;
        if (strncmp(e, "<char ", 6) && strncmp(e, "<reserved ", 10) && strncmp(e, "<noncharacter ", 14) && strncmp(e, "<surrogate ", 11)) continue;
        uint8_t r[32] = { 0 }; long first = -1, last = -1; elements++;
        for (char *a = strchr(e, ' '); a && *a; ) {
            while (*a == ' ') a++; char *eq = strchr(a, '='); if (!eq || eq[1] != '"') break;
            char *v = eq + 2, *q = strchr(v, '"'); if (!q) break;
            size_t nl = (size_t)(eq - a), vl = (size_t)(q - v);
            if (nl == 2 && !memcmp(a, "cp", 2)) first = last = strtol(v, NULL, 16);
            else if (nl == 8 && !memcmp(a, "first-cp", 8)) first = strtol(v, NULL, 16);
            else if (nl == 7 && !memcmp(a, "last-cp", 7)) last = strtol(v, NULL, 16);
            else { Prop *p = prop_named(a, nl);
                if (p) { p->seen++;
                    if (p->binary) { if (vl == 1 && v[0] == 'Y') set_bits(r, p->bit, 1, 1); }
                    else { int k = 0; while (k < p->nv && !same(p->v[k].name, v, vl) && !same(p->v[k].say, v, vl)) k++;
                           if (k == p->nv) p->unknown++; else { set_bits(r, p->bit, p->width, (uint32_t)k); } } } }
            a = q + 1;
        }
        if (first < 0 || last < first || last >= (long)LP_NCP) continue;
        for (long cp = first; cp <= last; cp++) { memcpy(rec + 32 * cp, r, 32); covered += !have[cp]; have[cp] = 1; }
    }
    fclose(f); free(ln);
    printf("  %'llu elements, %'llu of %'u codepoints covered   (%.2f s)\n", (unsigned long long)elements, (unsigned long long)covered, LP_NCP, now() - T);
    for (uint32_t cp = 0; cp < LP_NCP; cp++) for (int i = 0; i < nprop; i++) if (!prop[i].binary) {
        uint32_t v = 0; for (int b = 0; b < prop[i].width; b++) v |= (uint32_t)(rec[32 * cp + ((prop[i].bit + b) >> 3)] >> ((prop[i].bit + b) & 7) & 1) << b;
        prop[i].count[v < (uint32_t)prop[i].nv ? v : (uint32_t)prop[i].nv]++;
    }

    /* the records, their layout, their fingerprint */
    FILE *o = fopen(outp, "wb"); if (!o || fwrite(rec, 32, LP_NCP, o) != LP_NCP) { perror(outp); return 1; } fclose(o);
    char lay[4096]; snprintf(lay, sizeof lay, "%s.layout", outp); o = fopen(lay, "w"); if (!o) { perror(lay); return 1; }
    fprintf(o, "# The flags that go with tier 0: one 256-bit record per codepoint, little-endian bit order.\n# bit\twidth\tproperty\tas it is said\tvalues, in the standard's order (a field holds a value's place in this list)\n");
    for (int i = 0; i < nprop; i++) {
        fprintf(o, "%d\t%d\t%s\t%s\t", prop[i].bit, prop[i].width, prop[i].name, prop[i].say);
        for (int k = 0; k < prop[i].nv; k++) fprintf(o, "%s%s=%s", k ? " " : "", prop[i].v[k].name, prop[i].v[k].say);
        fputc('\n', o);
    }
    fclose(o);
    uint8_t fp[32]; blake3_hasher hs; blake3_hasher_init(&hs); blake3_hasher_update(&hs, rec, (size_t)LP_NCP * 32); blake3_hasher_finalize(&hs, fp, 32);

    printf("\n  %-5s %-5s %-10s %-34s %s\n", "bit", "width", "property", "as it is said", "values");
    for (int i = 0; i < nprop; i++) if (!prop[i].binary) {
        int top = 0; for (int k = 1; k < prop[i].nv; k++) if (prop[i].count[k] > prop[i].count[top]) top = k;
        printf("  %-5d %-5d %-10s %-34s %3d   most: %s %'llu%s\n", prop[i].bit, prop[i].width, prop[i].name, prop[i].say, prop[i].nv,
               prop[i].nv ? prop[i].v[top].say : "", (unsigned long long)(prop[i].nv ? prop[i].count[top] : 0), prop[i].unknown ? "   (values the list does not hold were seen)" : "");
    }
    int unseen = 0; for (int i = 0; i < nprop; i++) unseen += !prop[i].seen;
    printf("  bits 0 to %d: the binary properties, in the standard's order\n", nbin - 1);
    if (unseen) { printf("  properties the XML gives no value for:"); for (int i = 0; i < nprop; i++) if (!prop[i].seen) printf(" %s", prop[i].name); printf("\n"); }
    printf("\n%s: %u records x 32 bytes\n%s\nfingerprint (BLAKE3-256 of the records) ", outp, LP_NCP, lay);
    for (int i = 0; i < 32; i++) printf("%02x", fp[i]);
    printf("\n== total %.2f s\n", now() - T);
    return 0;
}
