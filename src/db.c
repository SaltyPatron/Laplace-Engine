/* Writing to PostgreSQL: trunk-to-leaf deduplication, binary COPY straight into each leaf partition on its own
 * connection, and the semantics (witnesses, the ledger, standings). SQL only fetches and writes. */
#include "engine.h"
#include <libpq-fe.h>
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

/* A binary uuid[] parameter. */
static size_t uuid_array(uint8_t *out, const lp_id *ids, uint32_t n){
    uint32_t hdr[5] = { htonl(1), htonl(0), htonl(2950), htonl(n), htonl(1) }; memcpy(out, hdr, 20); uint8_t *q = out + 20;
    for (uint32_t i = 0; i < n; i++) { uint32_t l = htonl(16); memcpy(q, &l, 4); memcpy(q + 4, ids[i].b, 16); q += 20; }
    return (size_t)(q - out);
}
/* Which of these IDs the database already records, at any tier: chunks queried on every connection at once. */
static uint8_t *recorded(PGconn **pg, int npg, const lp_id *ids, uint64_t n){
    uint8_t *hit = calloc(n ? n : 1, 1); const uint64_t CH = 100000;
    #pragma omp parallel for num_threads(npg) schedule(dynamic)
    for (uint64_t i0 = 0; i0 < n; i0 += CH) {
        uint32_t k = (uint32_t)(n - i0 < CH ? n - i0 : CH); uint8_t *ab = malloc(20 + 20 * (size_t)k);
        size_t len = uuid_array(ab, ids + i0, k); PGconn *c = pg[omp_get_thread_num()];
        const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
        PGresult *r = PQexecParams(c, "SELECT u.i FROM unnest($1::uuid[]) WITH ORDINALITY AS u(id, i) WHERE EXISTS (SELECT 1 FROM entity e WHERE e.id = u.id)", 1, NULL, v, l, f, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { fprintf(stderr, "dedup: %s", PQerrorMessage(c)); exit(1); }
        for (int j = 0; j < PQntuples(r); j++) hit[i0 + (uint64_t)atoll(PQgetvalue(r, j, 0)) - 1] = 1;
        PQclear(r); free(ab);
    }
    return hit;
}

/* ---- partitions: every tier up to 15 split 16 ways by the ID's first hex digit; deeper tiers in the default, split alike */
static int part_of(const lp_id *id, uint8_t tier){ return (tier < 16 ? tier : 16) * 16 + (id->b[0] >> 4); }
static void part_name(int p, const char *table, char *out, size_t cap){
    int t = p / 16, k = p % 16;
    if (t == 16) snprintf(out, cap, "%s_tx_%x", table, k); else snprintf(out, cap, "%s_t%d_%x", table, t, k);
}
#define NPART (17 * 16)


typedef struct { uint32_t shard; uint32_t idx; } NRef;
static NRef *bucket[NPART]; static uint64_t nbucket[NPART];
static void write_node_rows(PGconn *pg, int p, uint64_t *rows_e, uint64_t *rows_p, int atoms_needed){
    char tn[64]; Copy c = { 0 }; char sql[160]; uint8_t geo[64 * 1024];
    lp_id *ids = NULL; uint32_t *runs = NULL; size_t idc = 0;
    part_name(p, "entity", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY %s (id, tier, coord, hilbert) FROM STDIN (FORMAT binary)", tn);
    copy_begin(&c, pg, sql);
    if (atoms_needed && p / 16 == 0)
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            if ((T0[cp].id.b[0] >> 4) != p % 16) continue;
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
    part_name(p, "physicality", tn, sizeof tn); snprintf(sql, sizeof sql, "COPY %s (entity, tier, hilbert, path) FROM STDIN (FORMAT binary)", tn);
    copy_begin(&c, pg, sql);
    if (atoms_needed && p / 16 == 0)
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            if ((T0[cp].id.b[0] >> 4) != p % 16) continue;
            uint32_t one = 1; size_t gl = lp_ewkb_runs(&T0[cp].id, &one, 1, geo, sizeof geo);
            c16(&c, 4); cfield(&c, T0[cp].id.b, 16); cf_i16(&c, 0); cf_i64(&c, hsigned(T0[cp].hilbert)); cfield(&c, geo, (uint32_t)gl); c.rows++;
        }
    for (uint64_t b = 0; b < nbucket[p]; b++) {
        int s = (int)bucket[p][b].shard; Node *x = &shard[s].node[bucket[p][b].idx];
        if (x->nv > idc) { idc = x->nv * 2; ids = xrealloc(ids, idc * sizeof(lp_id)); runs = xrealloc(runs, idc * 4); }
        for (uint32_t v = 0; v < x->nv; v++) { ids[v] = shard[s].vtx[x->voff + v].id; runs[v] = shard[s].vtx[x->voff + v].run; }
        size_t gl = lp_ewkb_runs(ids, runs, x->nv, NULL, 0); uint8_t *gp = gl > sizeof geo ? malloc(gl) : geo;
        lp_ewkb_runs(ids, runs, x->nv, gp, gl);
        lp_coord co; memcpy(co.m, x->m, 32);
        c16(&c, 4); cfield(&c, x->id.b, 16); cf_i16(&c, x->tier); cf_i64(&c, hsigned(lp_hilbert4(&co))); cfield(&c, gp, (uint32_t)gl); c.rows++;
        if (gp != geo) free(gp);
    }
    copy_end(&c); *rows_p = c.rows;
    free(c.b); free(ids); free(runs);
}

/* ---- standings: a map from claim ID to its slot */
typedef struct { lp_id id; lp_rating r; uint32_t matches; uint8_t had; } Standing;
static Standing *stand; static uint32_t *smap; static uint64_t scap, sn;
static uint64_t skey(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 4, 8); return k; }
static Standing *stand_get(const lp_id *id, int add){
    uint64_t k = skey(id) & (scap - 1);
    while (smap[k]) { Standing *s = &stand[smap[k] - 1]; if (!memcmp(&s->id, id, 16)) return s; k = (k + 1) & (scap - 1); }
    if (!add) return NULL;
    stand[sn] = (Standing){ *id, { 1500.0, 350.0, 0.06 }, 0, 0 }; smap[k] = (uint32_t)++sn; return &stand[sn - 1];
}

int load(const char *conninfo, int npg, File *files, int nfiles, LoadStats *st){
    PGconn **pg = malloc(sizeof(PGconn *) * npg);
    for (int i = 0; i < npg; i++) {
        pg[i] = PQconnectdb(conninfo); if (PQstatus(pg[i]) != CONNECTION_OK) { fprintf(stderr, "%s", PQerrorMessage(pg[i])); return 1; }
        PQclear(PQexec(pg[i], "SET synchronous_commit = off"));
    }
    PGresult *r = PQexec(pg[0], "SELECT count(*) FROM entity WHERE tier = 0");
    int atoms_needed = !(PQresultStatus(r) == PGRES_TUPLES_OK && atoll(PQgetvalue(r, 0, 0)) == (long long)LP_NCP); PQclear(r);

    /* ---- trunk to leaf: a recorded node means its whole subtree is recorded, so nothing below it is checked */
    double t = now();
    uint64_t cap = 1 << 20, nf = 0; lp_id *front = malloc(cap * sizeof(lp_id));
    #define FPUSH(x) do { if (nf == cap) { cap *= 2; front = xrealloc(front, cap * sizeof(lp_id)); } front[nf++] = (x); } while (0)
    for (int fi = 0; fi < nfiles; fi++) {
        if (files[fi].known || files[fi].skipped) continue;
        Node *x = table_find(&files[fi].trunk.id); if (x && !x->keep) { x->keep = 3; FPUSH(x->id); }
        for (uint64_t i = 0; i < files[fi].ev.n; i++) { Node *c = table_find(&files[fi].ev.e[i].claim); if (c && !c->keep) { c->keep = 3; FPUSH(c->id); } }
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
    st->t_dedup = now() - t;

    /* ---- every leaf partition on its own connection; new nodes bucketed by partition once */
    t = now();
    { uint64_t cnt[NPART] = { 0 };
      for (int s = 0; s < NSHARD; s++) for (uint64_t i = 0; i < shard[s].n; i++) if (shard[s].node[i].keep == 1) cnt[part_of(&shard[s].node[i].id, shard[s].node[i].tier)]++;
      for (int p = 0; p < NPART; p++) { bucket[p] = malloc(sizeof(NRef) * (cnt[p] + 1)); nbucket[p] = 0; }
      for (int s = 0; s < NSHARD; s++) for (uint64_t i = 0; i < shard[s].n; i++) if (shard[s].node[i].keep == 1) {
          int p = part_of(&shard[s].node[i].id, shard[s].node[i].tier); bucket[p][nbucket[p]++] = (NRef){ (uint32_t)s, (uint32_t)i }; } } uint64_t re[NPART] = { 0 }, rp[NPART] = { 0 };
    #pragma omp parallel for num_threads(npg) schedule(dynamic)
    for (int p = 0; p < NPART; p++) {
        if (!nbucket[p] && !(atoms_needed && p / 16 == 0)) continue;             /* nothing new for this partition */
        write_node_rows(pg[omp_get_thread_num()], p, &re[p], &rp[p], atoms_needed);
    }
    for (int p = 0; p < NPART; p++) { st->ent_rows += re[p]; st->phy_rows += rp[p]; }
    st->t_copy = now() - t;

    /* ---- semantics: witnesses, the ledger, and standings played in reading order */
    t = now(); uint64_t nev = 0; for (int fi = 0; fi < nfiles; fi++) nev += files[fi].ev.n;
    if (nev) {
        scap = 1; while (scap < nev * 2) scap <<= 1; smap = calloc(scap, 4); stand = malloc(sizeof(Standing) * (nev + 1)); sn = 0;
        for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++) stand_get(&files[fi].ev.e[i].claim, 1);
        /* claims already recorded start from their recorded standing */
        lp_id *old = malloc(sizeof(lp_id) * (sn + 1)); uint64_t nold = 0;
        for (uint64_t i = 0; i < sn; i++) { Node *x = table_find(&stand[i].id); if (!x || x->keep != 1) old[nold++] = stand[i].id; }
        const uint64_t CH = 100000;
        #pragma omp parallel for num_threads(npg) schedule(dynamic)
        for (uint64_t i0 = 0; i0 < nold; i0 += CH) {
            uint32_t k = (uint32_t)(nold - i0 < CH ? nold - i0 : CH); uint8_t *ab = malloc(20 + 20 * (size_t)k);
            size_t len = uuid_array(ab, old + i0, k); PGconn *c = pg[omp_get_thread_num()];
            const char *v[1] = { (const char *)ab }; int l[1] = { (int)len }, f[1] = { 1 };
            PGresult *q = PQexecParams(c, "SELECT claim, rating, deviation, volatility, matches FROM standing WHERE claim = ANY($1::uuid[])", 1, NULL, v, l, f, 1);
            if (PQresultStatus(q) != PGRES_TUPLES_OK) { fprintf(stderr, "standing: %s", PQerrorMessage(c)); exit(1); }
            #pragma omp critical
            for (int j = 0; j < PQntuples(q); j++) {
                lp_id id; memcpy(id.b, PQgetvalue(q, j, 0), 16); Standing *s = stand_get(&id, 0); if (!s) continue;
                double d[3]; for (int z = 0; z < 3; z++) { uint64_t u = 0; const uint8_t *b = (const uint8_t *)PQgetvalue(q, j, 1 + z); for (int y = 0; y < 8; y++) u = u << 8 | b[y]; memcpy(&d[z], &u, 8); }
                const uint8_t *mb = (const uint8_t *)PQgetvalue(q, j, 4);
                s->r = (lp_rating){ d[0], d[1], d[2] }; s->matches = (uint32_t)mb[0] << 24 | mb[1] << 16 | mb[2] << 8 | mb[3]; s->had = 1;
            }
            PQclear(q); free(ab);
        }
        free(old);
        for (int fi = 0; fi < nfiles; fi++) {                                    /* first in, first out */
            const Recipe *rc = files[fi].recipe;
            for (uint64_t i = 0; i < files[fi].ev.n; i++) {
                Standing *s = stand_get(&files[fi].ev.e[i].claim, 0);
                lp_attest(&s->r, rc->trust, files[fi].ev.e[i].score, 1500.0, 0.5, 0.0); s->matches++;
            }
        }
        Copy c = { 0 };
        copy_begin(&c, pg[0], "COPY witness (id, lineage, trust) FROM STDIN (FORMAT binary)");
        for (int fi = 0; fi < nfiles; fi++) if (files[fi].ev.n) { c16(&c, 3); cfield(&c, files[fi].witness.id.b, 16); c32(&c, 0xFFFFFFFFu); cf_f64(&c, files[fi].recipe->trust); }
        copy_end(&c);
        #pragma omp parallel for num_threads(npg) schedule(static, 1)
        for (int w = 0; w < npg; w++) {                                          /* the ledger, split across connections */
            Copy lc = { 0 }; uint64_t k = 0;
            copy_begin(&lc, pg[w], "COPY attestation (claim, witness, score) FROM STDIN (FORMAT binary)");
            for (int fi = 0; fi < nfiles; fi++) for (uint64_t i = 0; i < files[fi].ev.n; i++, k++) {
                if (k % (uint64_t)npg != (uint64_t)w) continue;
                c16(&lc, 3); cfield(&lc, files[fi].ev.e[i].claim.b, 16); cfield(&lc, files[fi].witness.id.b, 16); cf_f32(&lc, files[fi].ev.e[i].score); lc.rows++;
            }
            copy_end(&lc);
            #pragma omp atomic
            st->led += lc.rows;
            free(lc.b);
        }
        copy_begin(&c, pg[0], "COPY standing (claim, rating, deviation, volatility, matches) FROM STDIN (FORMAT binary)");
        for (uint64_t i = 0; i < sn; i++) {
            Standing *s = &stand[i]; if (s->had) continue;
            c16(&c, 5); cfield(&c, s->id.b, 16); cf_f64(&c, s->r.rating); cf_f64(&c, s->r.deviation); cf_f64(&c, s->r.volatility); cf_i32(&c, (int32_t)s->matches); st->std_new++;
        }
        copy_end(&c); free(c.b);
        for (uint64_t i0 = 0; i0 < sn; ) {                                       /* recorded standings: set-based updates */
            static const uint32_t oid[5] = { 2950, 701, 701, 701, 23 }; static const int w[5] = { 16, 8, 8, 8, 4 };
            uint64_t idx[100000]; uint32_t n = 0;
            for (; i0 < sn && n < 100000; i0++) if (stand[i0].had) idx[n++] = i0;
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
            PGresult *u = PQexecParams(pg[0], "UPDATE standing s SET rating = u.r, deviation = u.d, volatility = u.v, matches = u.m "
                "FROM unnest($1::uuid[], $2::float8[], $3::float8[], $4::float8[], $5::int[]) AS u(c, r, d, v, m) WHERE s.claim = u.c", 5, NULL, v, alen, fm, 0);
            if (PQresultStatus(u) != PGRES_COMMAND_OK) { fprintf(stderr, "standing update: %s", PQerrorMessage(pg[0])); return 1; }
            PQclear(u); for (int f = 0; f < 5; f++) free(arr[f]); st->std_upd += n;
        }
    }
    st->t_sem = now() - t;

    Copy c = { 0 };
    copy_begin(&c, pg[0], "COPY source (trunk, origin, format, bytes, content) FROM STDIN (FORMAT binary)");
    for (int fi = 0; fi < nfiles; fi++) {
        if (files[fi].known || files[fi].skipped) continue;
        const char *fmt = files[fi].recipe ? files[fi].recipe->name : "text";
        c16(&c, 5); cfield(&c, files[fi].trunk.id.b, 16); cfield(&c, files[fi].path, (uint32_t)strlen(files[fi].path));
        cfield(&c, fmt, (uint32_t)strlen(fmt)); cf_i64(&c, (int64_t)files[fi].bytes); cfield(&c, files[fi].sha, 32);
    }
    copy_end(&c); free(c.b);
    for (int i = 0; i < npg; i++) PQfinish(pg[i]);
    return 0;
}
