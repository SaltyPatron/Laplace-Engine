/* Writing to PostgreSQL: trunk-to-leaf deduplication, binary COPY straight into each leaf partition on its own
 * connection, and the semantics (witnesses, the ledger, the consensus). SQL only fetches and writes. */
#include "engine.h"
#include <arpa/inet.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- binary COPY */
typedef struct { PGconn *pg; uint8_t *b; size_t n, cap; uint64_t rows, bytes; } Copy;
static void cflush(Copy *c){ if (c->n && PQputCopyData(c->pg, (const char *)c->b, (int)c->n) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(c->pg)); exit(1); } c->bytes += c->n; c->n = 0; }
static void cput(Copy *c, const void *p, size_t n){ if (c->n + n > c->cap) cflush(c); if (n > c->cap) { c->cap = n * 2; c->b = xrealloc(c->b, c->cap); } memcpy(c->b + c->n, p, n); c->n += n; }
static void c16(Copy *c, uint16_t v){ uint8_t b[2] = { v >> 8, v }; cput(c, b, 2); }
static void c32(Copy *c, uint32_t v){ uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; cput(c, b, 4); }
static void c64(Copy *c, uint64_t v){ uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); cput(c, b, 8); }
static void cfield(Copy *c, const void *p, uint32_t n){ c32(c, n); cput(c, p, n); }
static void cf_i16(Copy *c, int16_t v){ c32(c, 2); c16(c, (uint16_t)v); }
static void cf_i32(Copy *c, int32_t v){ c32(c, 4); c32(c, (uint32_t)v); }
static void cf_i64(Copy *c, int64_t v){ c32(c, 8); c64(c, (uint64_t)v); }
static void cf_f64(Copy *c, double v){ uint64_t u; memcpy(&u, &v, 8); c32(c, 8); c64(c, u); }
static void cf_f32(Copy *c, float v){ uint32_t u; memcpy(&u, &v, 4); c32(c, 4); c32(c, u); }
static void copy_begin(Copy *c, PGconn *pg, const char *sql){
    c->pg = pg; if (!c->b) { c->cap = 1 << 22; c->b = malloc(c->cap); } c->n = 0; c->rows = 0;
    PGresult *r = PQexec(pg, sql); if (PQresultStatus(r) != PGRES_COPY_IN) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(pg)); exit(1); } PQclear(r);
    static const uint8_t hdr[19] = { 'P','G','C','O','P','Y','\n',0xFF,'\r','\n',0, 0,0,0,0, 0,0,0,0 }; cput(c, hdr, 19);
}
static void copy_end(Copy *c){
    c16(c, 0xFFFF); cflush(c);
    if (PQputCopyEnd(c->pg, NULL) != 1) { fprintf(stderr, "COPY end: %s", PQerrorMessage(c->pg)); exit(1); }
    PGresult *r; while ((r = PQgetResult(c->pg))) { if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "COPY: %s", PQerrorMessage(c->pg)); exit(1); } PQclear(r); }
}
static int64_t hsigned(uint64_t h){ return (int64_t)(h ^ 0x8000000000000000ull); }      /* bigint order = Hilbert order */

/* Which of these IDs the database already records, at any tier. An ID's first hex digit says which sixteenth of every
 * tier it could be in, so each lookup goes to those partitions by name, and only to the ones that hold anything:
 * never to the partitioned table, which would probe every partition of every tier for every ID. */
static char *probe_sql[16]; static int probe_planned;
static void probe_plan(PGconn *pg){
    PGresult *r = PQexec(pg, "SELECT c.relname FROM pg_class c WHERE c.relkind = 'r' AND c.relname ~ '^entity_t([0-9]+|x)(_[0-9a-f])?$' ORDER BY 1");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "partitions: %s", PQerrorMessage(pg)); exit(1); }
    size_t cap[16] = { 0 }, len[16] = { 0 }; for (int h = 0; h < 16; h++) { free(probe_sql[h]); probe_sql[h] = NULL; }
    for (int j = 0; j < PQntuples(r); j++) {
        const char *name = PQgetvalue(r, j, 0); char q[160]; snprintf(q, sizeof q, "SELECT 1 FROM %s LIMIT 1", name);
        PGresult *e = PQexec(pg, q); int holds = PQresultStatus(e) == PGRES_TUPLES_OK && PQntuples(e) > 0; PQclear(e);
        if (!holds) continue;
        size_t nl = strlen(name); int whole = !(nl > 2 && name[nl - 2] == '_');        /* a tier that is one partition holds IDs of every first digit */
        char hx = name[nl - 1]; int only = hx <= '9' ? hx - '0' : hx - 'a' + 10;
        for (int h = whole ? 0 : only; h < (whole ? 16 : only + 1); h++) {
            if (len[h] + 256 > cap[h]) { cap[h] = cap[h] * 2 + 4096; probe_sql[h] = xrealloc(probe_sql[h], cap[h]); }
            len[h] += (size_t)snprintf(probe_sql[h] + len[h], cap[h] - len[h], "%s EXISTS (SELECT 1 FROM %s e WHERE e.id = u.id)",
                                       len[h] ? " OR" : "SELECT u.i FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) WHERE", name);
        }
    }
    PQclear(r);
}
static uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, uint64_t n){
    uint8_t *hit = calloc(n ? n : 1, 1); const uint64_t CH = 50000;
    if (!probe_planned) { probe_plan(pg[0]); probe_planned = 1; }
    uint64_t cnt[17] = { 0 }; for (uint64_t i = 0; i < n; i++) cnt[(ids[i].b[0] >> 4) + 1]++;
    for (int h = 0; h < 16; h++) cnt[h + 1] += cnt[h];
    uint64_t *at = malloc(sizeof(uint64_t) * (n + 1)), fill[16]; memcpy(fill, cnt, sizeof fill);
    for (uint64_t i = 0; i < n; i++) at[fill[ids[i].b[0] >> 4]++] = i;                 /* the IDs, by their first hex digit */
    typedef struct { int h; uint64_t lo, n; } Job; Job *job = malloc(sizeof(Job) * (n / CH + 17)); uint64_t nj = 0;
    for (int h = 0; h < 16; h++) { if (!probe_sql[h]) continue; for (uint64_t lo = cnt[h]; lo < cnt[h + 1]; lo += CH) job[nj++] = (Job){ h, lo, cnt[h + 1] - lo < CH ? cnt[h + 1] - lo : CH }; }
    #pragma omp parallel for num_threads(npg) schedule(dynamic)
    for (uint64_t j = 0; j < nj; j++) {
        uint32_t k = (uint32_t)job[j].n; lp_id *part = malloc(sizeof(lp_id) * k); for (uint32_t i = 0; i < k; i++) part[i] = ids[at[job[j].lo + i]];
        uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t len = ids_param(ab, part, k); PGconn *c = pg[omp_get_thread_num()];
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
        PGresult *r = PQexecParams(c, probe_sql[job[j].h], 1, NULL, v, l, f, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "dedup: %s", PQerrorMessage(c)); exit(1); }
        for (int x = 0; x < PQntuples(r); x++) hit[at[job[j].lo + (uint64_t)atoll(PQgetvalue(r, x, 0)) - 1]] = 1;
        PQclear(r); free(ab); free(part);
    }
    free(at); free(job);
    return hit;
}

/* ---- partitions: a tier each, tiers deeper than 15 in the default; the largest tiers split again 16 ways by the
 * ID's first hex digit. Which tiers are split is the schema's to say: it is read from the database, never assumed. */
static uint8_t split[17];
static void parts_plan(PGconn *pg){
    PGresult *r = PQexec(pg, "SELECT c.relname FROM pg_class c WHERE c.relkind = 'r' AND c.relname ~ '^entity_t([0-9]+|x)_[0-9a-f]$'");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "partitions: %s", PQerrorMessage(pg)); exit(1); }
    memset(split, 0, sizeof split);
    for (int j = 0; j < PQntuples(r); j++) { const char *n = PQgetvalue(r, j, 0) + 8; int t = *n == 'x' ? 16 : atoi(n); if (t >= 0 && t <= 16) split[t] = 1; }
    PQclear(r);
}
static int part_of(const lp_id *id, uint8_t tier){ int t = tier < 16 ? tier : 16; return t * 16 + (split[t] ? id->b[0] >> 4 : 0); }
static void part_name(int p, const char *table, char *out, size_t cap){
    int t = p / 16, k = p % 16; char tier[8]; if (t == 16) snprintf(tier, sizeof tier, "x"); else snprintf(tier, sizeof tier, "%d", t);
    if (split[t]) snprintf(out, cap, "%s_t%s_%x", table, tier, k); else snprintf(out, cap, "%s_t%s", table, tier);
}
#define NPART (17 * 16)


typedef struct { uint32_t shard; uint32_t idx; } NRef;
static NRef *bucket[NPART]; static uint64_t nbucket[NPART];
static void write_node_rows(PGconn *pg, int p, uint64_t *rows_e, uint64_t *rows_p, int atoms_needed){
    char tn[64]; Copy c = { 0 }; char sql[160]; uint8_t geo[64 * 1024];
    lp_id *ids = NULL; uint64_t *runs = NULL; size_t idc = 0;
    part_name(p, "entity", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY %s (id, tier, coord, hilbert) FROM STDIN (FORMAT binary)", tn);
    copy_begin(&c, pg, sql);
    if (atoms_needed && p / 16 == 0)
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            if (part_of(&T0[cp].id, 0) != p) continue;
            double x[4]; for (int d = 0; d < 4; d++) x[d] = (double)T0[cp].m[d] / LP_FIXED_ONE;
            size_t gl = lp_ewkb_point4(x, geo, sizeof geo);
            c16(&c, 4); cfield(&c, T0[cp].id.b, 16); cf_i16(&c, 0); cfield(&c, geo, (uint32_t)gl); cf_i64(&c, hsigned(T0[cp].hilbert)); c.rows++;
        }
    for (uint64_t b = 0; b < nbucket[p]; b++) {
        int s = (int)bucket[p][b].shard; Node *x = &shard[s].node[bucket[p][b].idx];
        double xm[4]; for (int d = 0; d < 4; d++) xm[d] = (double)x->m[d] / LP_FIXED_ONE;
        lp_coord co; memcpy(co.m, x->m, 32); size_t gl = lp_ewkb_point4(xm, geo, sizeof geo);
        c16(&c, 4); cfield(&c, x->id.b, 16); cf_i16(&c, x->tier); cfield(&c, geo, (uint32_t)gl); cf_i64(&c, hsigned(lp_hilbert4(&co))); c.rows++;
    }
    copy_end(&c); *rows_e = c.rows;
    part_name(p, "physicality", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY %s (entity, tier, hilbert, path, mask) FROM STDIN (FORMAT binary)", tn);
    uint8_t mask[4 + 32]; { uint32_t bl = htonl(256); memcpy(mask, &bl, 4); }         /* bit varying, binary: its length in bits, then its bytes, first bit first */
    copy_begin(&c, pg, sql);
    if (atoms_needed && p / 16 == 0)
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            if (part_of(&T0[cp].id, 0) != p) continue;
            uint64_t one = 1; size_t gl = lp_ewkb_runs(&T0[cp].id, &one, 1, geo, sizeof geo); memset(mask + 4, 0, 32);
            c16(&c, 5); cfield(&c, T0[cp].id.b, 16); cf_i16(&c, 0); cf_i64(&c, hsigned(T0[cp].hilbert)); cfield(&c, geo, (uint32_t)gl); cfield(&c, mask, 36); c.rows++;
        }
    for (uint64_t b = 0; b < nbucket[p]; b++) {
        int s = (int)bucket[p][b].shard; Node *x = &shard[s].node[bucket[p][b].idx];
        if (x->nv > idc) { idc = x->nv * 2; ids = xrealloc(ids, idc * sizeof(lp_id)); runs = xrealloc(runs, idc * 8); }
        for (uint32_t v = 0; v < x->nv; v++) { ids[v] = shard[s].vtx[x->voff + v].id; runs[v] = shard[s].vtx[x->voff + v].m; }
        size_t gl = lp_ewkb_runs(ids, runs, x->nv, NULL, 0); uint8_t *gp = gl > sizeof geo ? malloc(gl) : geo;
        lp_ewkb_runs(ids, runs, x->nv, gp, gl);
        lp_coord co; memcpy(co.m, x->m, 32);
        /* the mask: what the row is, and the types it holds (each constituent that is a type of a mask field) */
        memset(mask + 4, 0, 32); for (int b = 0; b < 8; b++) if (x->kind & (1u << b)) mask[4 + (b >> 3)] |= (uint8_t)(0x80 >> (b & 7));
        if (HW) for (uint32_t v = 0; v < x->nv; v++) { int32_t b = lp_highway_mask_bit(HW, &ids[v]); if (b >= 0 && b < 256) mask[4 + (b >> 3)] |= (uint8_t)(0x80 >> (b & 7)); }
        c16(&c, 5); cfield(&c, x->id.b, 16); cf_i16(&c, x->tier); cf_i64(&c, hsigned(lp_hilbert4(&co))); cfield(&c, gp, (uint32_t)gl); cfield(&c, mask, 36); c.rows++;
        if (gp != geo) free(gp);
    }
    copy_end(&c); *rows_p = c.rows;
    free(c.b); free(ids); free(runs);
}

/* ---- standings: a map from claim ID to its slot */
typedef struct { lp_id id; lp_rating r; uint32_t matches; uint8_t had, entered; } Standing;
static Standing *stand; static uint32_t *smap; static uint64_t scap, sn;
static uint64_t skey(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 4, 8); return k; }
/* The stock default a claim enters at: Glicko-2's rating for the unrated, and the uncertainty of the witness that brings
 * it (the deviation its trust plays with), unless the recipe gives this kind of statement its own. */
static double entry_deviation(const Event *e, double trust){
    if (e->enter_deviation > 0) return e->enter_deviation;
    double t = trust < 0 ? -trust : trust; if (t == 0.0) return 350.0;
    double d = lp_trust_deviation(t); return d < 30.0 ? 30.0 : d;
}
static Standing *stand_get(const lp_id *id, const Event *add, double trust){
    uint64_t k = skey(id) & (scap - 1);
    while (smap[k]) { Standing *s = &stand[smap[k] - 1]; if (!memcmp(&s->id, id, 16)) return s; k = (k + 1) & (scap - 1); }
    if (!add) return NULL;
    stand[sn] = (Standing){ *id, { add->enter_rating, entry_deviation(add, trust), 0.06 }, 0, 0, 0 };  /* the stock default for its level of attestation */
    smap[k] = (uint32_t)++sn; return &stand[sn - 1];
}

int load_whole;
int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st){
    PGconn **pg = malloc(sizeof(PGconn *) * npg);
    for (int i = 0; i < npg; i++) {
        pg[i] = db_connect(conninfo);
        PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    }
    probe_planned = 0;                                                       /* a partition empty at the last load may hold something now */
    parts_plan(pg[0]);
    PGresult *r = PQexec(pg[0], "SELECT count(*) FROM entity WHERE tier = 0");
    int atoms_needed = !(PQresultStatus(r) == PGRES_TUPLES_OK && atoll(PQgetvalue(r, 0, 0)) == (long long)LP_NCP); PQclear(r);

    /* ---- trunk to leaf: a recorded node means its whole subtree is recorded, so nothing below it is checked */
    double t = now();
    uint64_t cap = 1 << 20, nf = 0; lp_id *front = malloc(cap * sizeof(lp_id));
    #define FPUSH(x) do { if (nf == cap) { cap *= 2; front = xrealloc(front, cap * sizeof(lp_id)); } front[nf++] = (x); } while (0)
    if (load_whole)                                                          /* every node is looked for: nothing is taken to be recorded because what holds it is */
        for (int s = 0; s < NSHARD; s++) for (uint64_t i = 0; i < shard[s].n; i++) if (!shard[s].node[i].keep) { shard[s].node[i].keep = 3; FPUSH(shard[s].node[i].id); }
    /* The files first, by their trunks: a file whose trunk is recorded is recorded, with everything under it and
     * everything it attested, and nothing of it is looked for, played or written again. */
    { lp_id *trunk = malloc(sizeof(lp_id) * (size_t)(nfiles + 1)); int *of = malloc(sizeof(int) * (size_t)(nfiles + 1)); uint64_t nt = 0;
      for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { of[nt] = fi; trunk[nt++] = files[fi].file.id; }
      if (nt) { uint8_t *hit = recorded(pg, npg, trunk, nt); st->checked += nt;
                for (uint64_t i = 0; i < nt; i++) if (hit[i]) { files[of[i]].known = 1; st->known++; st->found++; Node *x = table_find(&trunk[i]); if (x) x->keep = 2; }
                free(hit); }
      free(trunk); free(of); }
    for (int fi = 0; fi < nfiles; fi++) {
        if (files[fi].known || files[fi].skipped) continue;
        if (files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (x && !x->keep) { x->keep = 3; FPUSH(x->id); } }
        Node *x = table_find(&files[fi].trunk.id); if (x && !x->keep) { x->keep = 3; FPUSH(x->id); }
        if (files[fi].ev.n) { Node *w = table_find(&files[fi].witness.id); if (w && !w->keep) { w->keep = 3; FPUSH(w->id); } }
        if (files[fi].has_lineage) { Node *lin = table_find(&files[fi].lineage.id); if (lin && !lin->keep) { lin->keep = 3; FPUSH(lin->id); } }
        for (uint64_t i = 0; i < files[fi].ev.n; i++) { Node *c = table_find(&files[fi].ev.e[i].claim); if (c && !c->keep) { c->keep = 3; FPUSH(c->id); }     /* a record's claims are under it */
            if (files[fi].ev.e[i].kind == EV_RECORD) { Node *w = table_find(&files[fi].ev.e[i].witnessed); if (w && !w->keep) { w->keep = 3; FPUSH(w->id); } }
            if (files[fi].ev.e[i].own_witness) { Node *w = table_find(&files[fi].ev.e[i].witness); if (w && !w->keep) { w->keep = 3; FPUSH(w->id); } } }
    }
    while (nf) {
        st->rounds++; st->checked += nf;
        uint8_t *hit = recorded(pg, npg, front, nf);
        uint64_t nn = 0; lp_id *next = malloc((nf + 1) * sizeof(lp_id)); uint64_t ncap = nf + 1;
        for (uint64_t i = 0; i < nf; i++) {
            Node *x = table_find(&front[i]);
            if (hit[i]) { x->keep = 2; st->found++; continue; }
            x->keep = 1; st->new_nodes++;
            Shard *s = &shard[x->id.b[0]];
            for (uint32_t v = 0; v < x->nv; v++) {
                Node *ch = table_find(&s->vtx[x->voff + v].id);
                if (ch && !ch->keep) { ch->keep = 3; if (nn == ncap) { ncap *= 2; next = xrealloc(next, ncap * sizeof(lp_id)); } next[nn++] = ch->id; }
            }
        }
        free(hit); free(front); front = next; nf = nn; cap = ncap;
        fprintf(stderr, "\r  dedup round %llu: %llu new so far, %llu subtrees already recorded   ", (unsigned long long)st->rounds,
                (unsigned long long)st->new_nodes, (unsigned long long)st->found);
    }
    free(front); fputc('\n', stderr);
    /* a file's trunk is written last, after what the file attested: it is held back from the writing below */
    uint64_t nlast = 0;
    for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (x && x->keep == 1) { x->keep = 5; nlast++; } }
    st->t_dedup += now() - t;

    /* ---- every leaf partition on its own connection; new nodes bucketed by partition once */
    t = now();
    { uint64_t cnt[NPART] = { 0 };
      for (int s = 0; s < NSHARD; s++) for (uint64_t i = 0; i < shard[s].n; i++) if (shard[s].node[i].keep == 1) cnt[part_of(&shard[s].node[i].id, shard[s].node[i].tier)]++;
      for (int p = 0; p < NPART; p++) { bucket[p] = malloc(sizeof(NRef) * (cnt[p] + 1)); nbucket[p] = 0; }
      for (int s = 0; s < NSHARD; s++) for (uint64_t i = 0; i < shard[s].n; i++) if (shard[s].node[i].keep == 1) {
          int p = part_of(&shard[s].node[i].id, shard[s].node[i].tier); bucket[p][nbucket[p]++] = (NRef){ (uint32_t)s, (uint32_t)i }; } } uint64_t re[NPART] = { 0 }, rp[NPART] = { 0 };
    /* A tier at a time, from the lowest: what a composition is made of is always of a lower tier than it, so whatever
     * is recorded has everything under it recorded, even if the writing is cut off. Trunk-to-leaf deduplication
     * rests on that. */
    for (int tier = 0; tier <= 16; tier++) {
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (int p = tier * 16; p < tier * 16 + 16; p++) {
            if (!nbucket[p] && !(atoms_needed && p / 16 == 0)) continue;         /* nothing new for this partition */
            write_node_rows(pg[omp_get_thread_num()], p, &re[p], &rp[p], atoms_needed);
        }
    }
    for (int p = 0; p < NPART; p++) { st->ent_rows += re[p]; st->phy_rows += rp[p]; free(bucket[p]); bucket[p] = NULL; }
    st->t_copy += now() - t;

    /* ---- semantics: witnesses, the ledger, and standings played in reading order */
    t = now(); uint64_t nev = 0;
    for (int fi = 0; fi < nfiles; fi++) { if (files[fi].known) { free(files[fi].ev.e); memset(&files[fi].ev, 0, sizeof files[fi].ev); } nev += files[fi].ev.n; }
    /* What the files attested and the files' trunks are written as one: either all of it is recorded or none is. */
    { PGresult *b = PQexec(pg[0], "BEGIN"); if (PQresultStatus(b) != PGRES_COMMAND_OK) { fprintf(stderr, "begin: %s", PQerrorMessage(pg[0])); return 1; } PQclear(b); }
    if (nev) {
        scap = 1; while (scap < nev * 2) scap <<= 1; smap = calloc(scap, 4); stand = malloc(sizeof(Standing) * (nev + 1)); sn = 0;
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) if (files[fi].ev.e[i].kind != EV_RECORD) stand_get(&files[fi].ev.e[i].claim, &files[fi].ev.e[i], files[fi].trust);
        /* claims already recorded start from their recorded standing */
        /* the statistics are partitioned by the claim's first hex digit: each read goes to its one partition */
        lp_id *old = malloc(sizeof(lp_id) * (sn + 1)); uint64_t nold = 0, ocnt[17] = { 0 };
        for (uint64_t i = 0; i < sn; i++) { Node *x = table_find(&stand[i].id); if (!x || x->keep != 1) { old[nold++] = stand[i].id; ocnt[(stand[i].id.b[0] >> 4) + 1]++; } }
        for (int h = 0; h < 16; h++) ocnt[h + 1] += ocnt[h];
        { lp_id *by = malloc(sizeof(lp_id) * (nold + 1)); uint64_t fill[16]; memcpy(fill, ocnt, sizeof fill); for (uint64_t i = 0; i < nold; i++) by[fill[old[i].b[0] >> 4]++] = old[i]; free(old); old = by; }
        const uint64_t CH = 100000; typedef struct { int h; uint64_t lo, n; } OJob; OJob *oj = malloc(sizeof(OJob) * (nold / CH + 17)); uint64_t noj = 0;
        for (int h = 0; h < 16; h++) for (uint64_t lo = ocnt[h]; lo < ocnt[h + 1]; lo += CH) oj[noj++] = (OJob){ h, lo, ocnt[h + 1] - lo < CH ? ocnt[h + 1] - lo : CH };
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (uint64_t j = 0; j < noj; j++) {
            uint32_t k = (uint32_t)oj[j].n; uint8_t *ab = malloc(20 + 20 * (size_t)k);
            size_t len = ids_param(ab, old + oj[j].lo, k); PGconn *c = pg[omp_get_thread_num()]; char sql[160]; snprintf(sql, sizeof sql, "SELECT claim, rating, deviation, volatility, matches FROM consensus_%x WHERE claim = ANY($1::blake3[])", oj[j].h);
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
            PGresult *q = PQexecParams(c, sql, 1, NULL, v, l, f, 1);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "standing: %s", PQerrorMessage(c)); exit(1); }
            #pragma omp critical
            for (int j = 0; j < PQntuples(q); j++) {
                lp_id id; memcpy(id.b, PQgetvalue(q, j, 0), 16); Standing *s = stand_get(&id, NULL, 0); if (!s) continue;
                double d[3]; for (int z = 0; z < 3; z++) { uint64_t u = 0; const uint8_t *b = (const uint8_t *)PQgetvalue(q, j, 1 + z); for (int y = 0; y < 8; y++) u = u << 8 | b[y]; memcpy(&d[z], &u, 8); }
                const uint8_t *mb = (const uint8_t *)PQgetvalue(q, j, 4);
                s->r = (lp_rating){ d[0], d[1], d[2] }; s->matches = (uint32_t)mb[0] << 24 | mb[1] << 16 | mb[2] << 8 | mb[3]; s->had = 1;
            }
            PQclear(q); free(ab);
        }
        free(old); free(oj);
        /* What was witnessed plays once per lineage: a copy of it is a row in the ledger and nothing more. What this
         * lineage witnessed before is read from the ledger; what it witnesses in this run is kept here. */
        typedef struct { lp_id witnessed, lin; uint8_t used; } Seen;
        uint64_t nrec = 0; for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) nrec += files[fi].ev.e[i].kind != EV_MEMBER;
        lp_id *wold = malloc(sizeof(lp_id) * (nrec + 1)); uint64_t nwold = 0;
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (e->kind == EV_MEMBER) continue;
            Node *x = table_find(&e->witnessed); if (!x || x->keep != 1) wold[nwold++] = e->witnessed; }
        uint64_t pcap = 1024; while (pcap < nrec * 3) pcap <<= 1; Seen *seen = calloc(pcap, sizeof(Seen));
        #define SEEN_AT(w, l, found) do { uint64_t h_; memcpy(&h_, (w)->b, 8); uint64_t c_; memcpy(&c_, (l)->b + 8, 8); h_ ^= c_ * 0x9E3779B97F4A7C15ull; k_ = h_ & (pcap - 1); found = 0; \
            while (seen[k_].used) { if (!memcmp(seen[k_].witnessed.b, (w)->b, 16) && !memcmp(seen[k_].lin.b, (l)->b, 16)) { found = 1; break; } k_ = (k_ + 1) & (pcap - 1); } } while (0)
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (uint64_t i0 = 0; i0 < nwold; i0 += CH) {
            uint32_t k = (uint32_t)(nwold - i0 < CH ? nwold - i0 : CH); uint8_t *ab = malloc(20 + 20 * (size_t)k);
            size_t len = ids_param(ab, wold + i0, k); PGconn *c = pg[omp_get_thread_num()];
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
            PGresult *q = PQexecParams(c, "SELECT a.claim, w.id, w.lineage FROM attestation a JOIN witness w ON w.id = a.witness WHERE a.claim = ANY($1::blake3[])", 1, NULL, v, l, f, 1);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "lineage: %s", PQerrorMessage(c)); exit(1); }
            #pragma omp critical
            for (int j = 0; j < PQntuples(q); j++) {
                lp_id wit, wid, lin; memcpy(wit.b, PQgetvalue(q, j, 0), 16); memcpy(wid.b, PQgetvalue(q, j, 1), 16);
                if (PQgetisnull(q, j, 2)) lin = wid; else memcpy(lin.b, PQgetvalue(q, j, 2), 16);
                uint64_t k_; int found; SEEN_AT(&wit, &lin, found);
                if (!found) { seen[k_].used = 1; seen[k_].witnessed = wit; seen[k_].lin = lin; }
            }
            PQclear(q); free(ab);
        }
        free(wold);
        /* The matchups, first in, first out. A claim entering for the first time enters at its stock default: there is
         * nothing recorded for it to play. After that, every incoming record plays the recorded one: the witness at
         * the rating its record would enter at, with the deviation its trust gives, and the outcome it attests. */
        for (int fi = 0; fi < nfiles; fi++) {
            double trust = files[fi].trust; int copy = 0;
            const lp_id *flin = files[fi].has_lineage ? &files[fi].lineage.id : &files[fi].witness.id;
            for (uint64_t i = 0; i < files[fi].ev.n; i++) {
                const Event *e = &files[fi].ev.e[i]; const lp_id *lin = e->own_witness ? &e->witness : flin;
                if (e->kind != EV_MEMBER) {                                  /* what is witnessed: once per lineage */
                    uint64_t k_; SEEN_AT(&e->witnessed, lin, copy);
                    if (!copy) { seen[k_].used = 1; seen[k_].witnessed = e->witnessed; seen[k_].lin = *lin; }
                    if (e->kind == EV_RECORD) continue;
                }
                if (copy) continue;
                Standing *s = stand_get(&e->claim, NULL, 0);
                if (!s->had && !s->entered) { s->entered = 1; continue; }
                lp_attest(&s->r, trust, e->score, e->enter_rating, 0.5, 30.0); s->matches++;
            }
        }
        free(seen);
        Copy c = { 0 };
        /* witnesses: each once, and only those the database does not know yet */
        lp_id *wid = malloc(sizeof(lp_id) * (size_t)nfiles); int *wfile = malloc(sizeof(int) * (size_t)nfiles), nw = 0;
        for (int fi = 0; fi < nfiles; fi++) if (files[fi].ev.n) {
            int k = 0; while (k < nw && memcmp(&wid[k], &files[fi].witness.id, 16)) k++;
            if (k == nw) { wid[nw] = files[fi].witness.id; wfile[nw++] = fi; }
        }
        /* witnesses a source names statement by statement: each is its own lineage, and plays at the source's trust */
        lp_id *own = NULL; double *owntrust = NULL; uint64_t nown = 0, cown = 0, ocap = 1 << 16; uint32_t *oslot = calloc(ocap, 4);
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) { const Event *e = &files[fi].ev.e[i]; if (!e->own_witness) continue;
            if ((nown + 1) * 2 > ocap) { free(oslot); ocap *= 2; oslot = calloc(ocap, 4); for (uint64_t j = 0; j < nown; j++) { uint64_t h; memcpy(&h, own[j].b, 8); uint64_t k = h & (ocap - 1); while (oslot[k]) k = (k + 1) & (ocap - 1); oslot[k] = (uint32_t)j + 1; } }
            uint64_t h; memcpy(&h, e->witness.b, 8); uint64_t k = h & (ocap - 1); int seen_ = 0;
            while (oslot[k]) { if (!memcmp(&own[oslot[k] - 1], &e->witness, 16)) { seen_ = 1; break; } k = (k + 1) & (ocap - 1); }
            if (seen_) continue;
            if (nown == cown) { cown = cown ? cown * 2 : 4096; own = xrealloc(own, cown * sizeof(lp_id)); owntrust = xrealloc(owntrust, cown * 8); }
            own[nown] = e->witness; owntrust[nown] = files[fi].trust; oslot[k] = (uint32_t)++nown; }
        uint8_t *oknown = calloc(nown ? nown : 1, 1);
        for (uint64_t i0 = 0; i0 < nown; i0 += 50000) { uint32_t k = (uint32_t)(nown - i0 < 50000 ? nown - i0 : 50000); uint8_t *ab = malloc(20 + 20 * (size_t)k); size_t len = ids_param(ab, own + i0, k);
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
            PGresult *q = PQexecParams(pg[0], "SELECT u.i FROM unnest($1::blake3[]) WITH ORDINALITY AS u(id, i) JOIN witness w ON w.id = u.id", 1, NULL, v, l, f, 0);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg[0])); return 1; }
            for (int j = 0; j < PQntuples(q); j++) oknown[i0 + (uint64_t)atoll(PQgetvalue(q, j, 0)) - 1] = 1;
            PQclear(q); free(ab); }
        uint8_t *known = calloc((size_t)(nw ? nw : 1), 1);
        { uint8_t *ab = malloc(20 + 20 * (size_t)nw); size_t len = ids_param(ab, wid, (uint32_t)nw);
          const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
          PGresult *q = PQexecParams(pg[0], "SELECT id FROM witness WHERE id = ANY($1::blake3[])", 1, NULL, v, l, f, 1);
          if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "witnesses: %s", PQerrorMessage(pg[0])); return 1; }
          for (int j = 0; j < PQntuples(q); j++) for (int k = 0; k < nw; k++) if (!memcmp(wid[k].b, PQgetvalue(q, j, 0), 16)) known[k] = 1;
          PQclear(q); free(ab); }
        copy_begin(&c, pg[0], "COPY witness (id, lineage, trust) FROM STDIN (FORMAT binary)");
        for (int k = 0; k < nw; k++) if (!known[k]) {
            int fi = wfile[k];
            c16(&c, 3); cfield(&c, files[fi].witness.id.b, 16);
            if (files[fi].has_lineage) cfield(&c, files[fi].lineage.id.b, 16); else c32(&c, 0xFFFFFFFFu);
            cf_f64(&c, files[fi].trust);
        }
        for (uint64_t i = 0; i < nown; i++) if (!oknown[i]) { int dup = 0; for (int k = 0; k < nw && !dup; k++) dup = !memcmp(&wid[k], &own[i], 16); if (dup) continue;
            c16(&c, 3); cfield(&c, own[i].b, 16); c32(&c, 0xFFFFFFFFu); cf_f64(&c, owntrust[i]); }
        copy_end(&c); free(wid); free(wfile); free(known); free(own); free(owntrust); free(oslot); free(oknown);
        /* the ledger, in reading order (its order is the order of play), and the new standings: each row into the
         * partition its claim's first hex digit names, sixteen copies, no routing */
        for (int h = 0; h < 16; h++) { Copy lc = { 0 }; char sql[160]; snprintf(sql, sizeof sql, "COPY attestation_%x (claim, witness, score, position) FROM STDIN (FORMAT binary)", h);
          copy_begin(&lc, pg[0], sql);
          for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) {
              if (files[fi].ev.e[i].kind == EV_MEMBER || (files[fi].ev.e[i].witnessed.b[0] >> 4) != h) continue;                /* witnessed within its record: the record's row */
              c16(&lc, 4); cfield(&lc, files[fi].ev.e[i].witnessed.b, 16); cfield(&lc, files[fi].ev.e[i].own_witness ? files[fi].ev.e[i].witness.b : files[fi].witness.id.b, 16); cf_f32(&lc, files[fi].ev.e[i].score);
              if (files[fi].ev.e[i].position) cf_i32(&lc, (int32_t)files[fi].ev.e[i].position); else c32(&lc, 0xFFFFFFFFu);
              lc.rows++;
          }
          copy_end(&lc); st->led += lc.rows; free(lc.b); }
        for (int h = 0; h < 16; h++) { char sql[160]; snprintf(sql, sizeof sql, "COPY consensus_%x (claim, rating, deviation, volatility, matches) FROM STDIN (FORMAT binary)", h);
          copy_begin(&c, pg[0], sql);
          for (uint64_t i = 0; i < sn; i++) {
              Standing *s = &stand[i]; if (s->had || (s->id.b[0] >> 4) != h) continue;
              c16(&c, 5); cfield(&c, s->id.b, 16); cf_f64(&c, s->r.rating); cf_f64(&c, s->r.deviation); cf_f64(&c, s->r.volatility); cf_i32(&c, (int32_t)s->matches); st->std_new++;
          }
          copy_end(&c); free(c.b); c = (Copy){ 0 }; }
        for (int h = 0; h < 16; h++) for (uint64_t i0 = 0; i0 < sn; ) {           /* recorded standings: set-based updates, each into its claim's partition */
            const uint32_t oid[5] = { id_oid, 701, 701, 701, 23 }; static const int w[5] = { 16, 8, 8, 8, 4 };
            uint64_t idx[100000]; uint32_t n = 0;
            for (; i0 < sn && n < 100000; i0++) if (stand[i0].had && (stand[i0].id.b[0] >> 4) == h) idx[n++] = i0;
            if (!n) continue;
            uint8_t *arr[5]; int alen[5];
            for (int f = 0; f < 5; f++) {
                arr[f] = malloc(20 + (size_t)n * (4 + w[f])); uint8_t *q = arr[f] + 20;
                uint32_t hdr[5] = { htonl(1), htonl(0), htonl(oid[f]), htonl(n), htonl(1) }; memcpy(arr[f], hdr, 20);
                for (uint32_t j = 0; j < n; j++) {
                    Standing *s = &stand[idx[j]]; uint32_t l = htonl((uint32_t)w[f]); memcpy(q, &l, 4); q += 4;
                    if (f == 0) memcpy(q, s->id.b, 16);
                    else if (f < 4) { double d = f == 1 ? s->r.rating : f == 2 ? s->r.deviation : s->r.volatility; uint64_t u; memcpy(&u, &d, 8); for (int y = 0; y < 8; y++) q[y] = (uint8_t)(u >> (56 - 8 * y)); }
                    else { uint32_t m = htonl(s->matches); memcpy(q, &m, 4); }
                    q += w[f];
                }
                alen[f] = (int)(q - arr[f]);
            }
            const char *v[5] = { (char *)arr[0], (char *)arr[1], (char *)arr[2], (char *)arr[3], (char *)arr[4] }; int fm[5] = { 1, 1, 1, 1, 1 };
            char usql[300]; snprintf(usql, sizeof usql, "UPDATE consensus_%x s SET rating = u.r, deviation = u.d, volatility = u.v, matches = u.m "
                "FROM unnest($1::blake3[], $2::float8[], $3::float8[], $4::float8[], $5::int[]) AS u(c, r, d, v, m) WHERE s.claim = u.c", h);
            PGresult *u = PQexecParams(pg[0], usql, 5, NULL, v, alen, fm, 0);
            if (PQresultStatus(u) != PGRES_COMMAND_OK) { fprintf(stderr, "standing update: %s", PQerrorMessage(pg[0])); return 1; }
            PQclear(u); for (int f = 0; f < 5; f++) free(arr[f]); st->std_upd += n;
        }
    }
    if (nev) { free(stand); free(smap); stand = NULL; smap = NULL; }
    st->t_sem += now() - t;

    /* the files' trunks: last */
    if (nlast) {
        uint64_t cnt[NPART] = { 0 };
        for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (x && x->keep == 5) cnt[part_of(&x->id, x->tier)]++; }
        for (int p = 0; p < NPART; p++) { bucket[p] = malloc(sizeof(NRef) * (cnt[p] + 1)); nbucket[p] = 0; }
        for (int fi = 0; fi < nfiles; fi++) if (!files[fi].known && !files[fi].skipped && files[fi].has_file) { Node *x = table_find(&files[fi].file.id); if (!x || x->keep != 5) continue;
            int p = part_of(&x->id, x->tier), sh = x->id.b[0]; bucket[p][nbucket[p]++] = (NRef){ (uint32_t)sh, (uint32_t)(x - shard[sh].node) }; x->keep = 1; }
        for (int p = 0; p < NPART; p++) { uint64_t e = 0, ph = 0; if (nbucket[p]) { write_node_rows(pg[0], p, &e, &ph, 0); st->ent_rows += e; st->phy_rows += ph; } free(bucket[p]); bucket[p] = NULL; }
    }
    { PGresult *e = PQexec(pg[0], "COMMIT"); if (PQresultStatus(e) != PGRES_COMMAND_OK) { fprintf(stderr, "commit: %s", PQerrorMessage(pg[0])); return 1; } PQclear(e); }
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}
