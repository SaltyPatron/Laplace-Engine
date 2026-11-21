#include "codecs.h"
#include "laplace/os.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- Native .git repository / packfile / loose-object Merkle DAG grammar
 * Reads loose zlib-compressed git objects AND packfile v2 (.pack + .idx) with OFS_DELTA / REF_DELTA
 * reconstruction, lowering commits, trees, blobs, and refs into content-addressed Laplace Merkle entities. */

static uint32_t g_be32(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t g_be64(const uint8_t *p){
    return ((uint64_t)g_be32(p) << 32) | (uint64_t)g_be32(p + 4);
}

typedef struct { int mode; } GitRecipe;

static int git_says(Recipe *r, const char *path, char *tok){
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(GitRecipe));
    if (!strcmp(tok, "records")) {
        char *v = strtok(NULL, " \t\r\n");
        if (!v || (strcmp(v, "commits") && strcmp(v, "objects") && strcmp(v, "git"))) {
            fprintf(stderr, "%s: records commits | objects | git\n", path);
            return -1;
        }
        ((GitRecipe *)r->native_recipe)->mode = 1;
        return 1;
    }
    fprintf(stderr, "%s: \"%s\" is not something a git recipe says\n", path, tok);
    return -1;
}

static int git_done(Recipe *r, const char *path){
    (void)path;
    if (!r->native_recipe) r->native_recipe = calloc(1, sizeof(GitRecipe));
    r->curated = 1;
    return 1;
}

static int hex_to_sha1(const char *hex, uint8_t out[20]){
    for (int i = 0; i < 20; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[2 * i + j];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void sha1_to_hex(const uint8_t sha[20], char out[41]){
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[2 * i] = H[(sha[i] >> 4) & 0xF];
        out[2 * i + 1] = H[sha[i] & 0xF];
    }
    out[40] = 0;
}

enum { GIT_COMMIT = 1, GIT_TREE = 2, GIT_BLOB = 3, GIT_TAG = 4, GIT_OFS_DELTA = 6, GIT_REF_DELTA = 7 };

typedef struct {
    const uint8_t *pack;
    size_t         pack_len;
    const uint8_t *idx;
    size_t         idx_len;
    uint32_t       nobj;
} PackFile;

typedef struct {
    char     git_dir[4096];
    PackFile *packs;
    size_t   npacks;
    lp_strmap *cache; /* 40-char sha1 hex -> Ref index in cached_refs */
    Ref      *refs;
    size_t   nrefs, crefs;
} GitRepo;

static uint8_t *slurp_file(const char *path, size_t *len){
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int64_t sz = os_fsize(f);
    if (sz < 0 || sz > (128LL << 20)) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz ? (size_t)sz : 1u);
    if (!b || (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz)) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return b;
}

static uint8_t *inflate_exact(const uint8_t *src, size_t max_in, size_t expected_out,
                              size_t *in_consumed, size_t *out_len){
    z_stream zs = { 0 };
    if (inflateInit(&zs) != Z_OK) return NULL;
    size_t cap = expected_out ? expected_out : 4096u;
    uint8_t *dst = malloc(cap ? cap : 1u);
    zs.next_in = (Bytef *)(uintptr_t)src;
    zs.avail_in = (uInt)(max_in > (size_t)0xFFFFFFFFu ? 0xFFFFFFFFu : max_in);
    for (;;) {
        if (zs.total_out >= cap) { cap *= 2u; dst = xrealloc(dst, cap); }
        zs.next_out = dst + zs.total_out;
        zs.avail_out = (uInt)(cap - zs.total_out);
        int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { inflateEnd(&zs); free(dst); return NULL; }
    }
    if (in_consumed) *in_consumed = zs.total_in;
    *out_len = zs.total_out;
    inflateEnd(&zs);
    return dst;
}

static int apply_git_delta(const uint8_t *base, size_t blen,
                           const uint8_t *delta, size_t dlen,
                           uint8_t **out_buf, size_t *out_len){
    size_t pos = 0;
    uint64_t src_sz = 0, tgt_sz = 0;
    for (int sh = 0; pos < dlen && sh < 64; sh += 7) {
        uint8_t b = delta[pos++];
        src_sz |= (uint64_t)(b & 0x7Fu) << sh;
        if (!(b & 0x80u)) break;
    }
    for (int sh = 0; pos < dlen && sh < 64; sh += 7) {
        uint8_t b = delta[pos++];
        tgt_sz |= (uint64_t)(b & 0x7Fu) << sh;
        if (!(b & 0x80u)) break;
    }
    if (src_sz != blen || tgt_sz > (128u << 20)) return 0;
    uint8_t *dst = malloc(tgt_sz ? (size_t)tgt_sz : 1u);
    size_t dp = 0;
    while (pos < dlen) {
        uint8_t cmd = delta[pos++];
        if (cmd & 0x80u) {
            uint32_t off = 0, csz = 0;
            if (cmd & 0x01u && pos < dlen) off |= (uint32_t)delta[pos++];
            if (cmd & 0x02u && pos < dlen) off |= (uint32_t)delta[pos++] << 8;
            if (cmd & 0x04u && pos < dlen) off |= (uint32_t)delta[pos++] << 16;
            if (cmd & 0x08u && pos < dlen) off |= (uint32_t)delta[pos++] << 24;
            if (cmd & 0x10u && pos < dlen) csz |= (uint32_t)delta[pos++];
            if (cmd & 0x20u && pos < dlen) csz |= (uint32_t)delta[pos++] << 8;
            if (cmd & 0x40u && pos < dlen) csz |= (uint32_t)delta[pos++] << 16;
            if (!csz) csz = 0x10000u;
            if ((size_t)off + csz > blen || dp + csz > tgt_sz) { free(dst); return 0; }
            memcpy(dst + dp, base + off, csz);
            dp += csz;
        } else if (cmd) {
            if (cmd > dlen - pos || dp + cmd > tgt_sz) { free(dst); return 0; }
            memcpy(dst + dp, delta + pos, cmd);
            pos += cmd;
            dp += cmd;
        } else {
            free(dst); return 0;
        }
    }
    if (dp != tgt_sz) { free(dst); return 0; }
    *out_buf = dst;
    *out_len = (size_t)tgt_sz;
    return 1;
}

static int find_in_pack_idx(const PackFile *pf, const uint8_t sha1[20], uint64_t *offset_out){
    if (pf->idx_len < 8 + 256 * 4 || memcmp(pf->idx, "\xfftOc", 4) != 0 || g_be32(pf->idx + 4) != 2u) return 0;
    uint8_t fb = sha1[0];
    uint32_t lo = fb ? g_be32(pf->idx + 8 + ((size_t)fb - 1u) * 4u) : 0u;
    uint32_t hi = g_be32(pf->idx + 8 + (size_t)fb * 4u);
    const uint8_t *names = pf->idx + 8 + 256u * 4u;
    if (lo > hi || hi > pf->nobj) return 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        int cmp = memcmp(names + (size_t)mid * 20u, sha1, 20u);
        if (cmp == 0) {
            const uint8_t *offs = names + (size_t)pf->nobj * 20u + (size_t)pf->nobj * 4u;
            uint32_t off32 = g_be32(offs + (size_t)mid * 4u);
            if (!(off32 & 0x80000000u)) {
                *offset_out = off32;
            } else {
                const uint8_t *off64 = offs + (size_t)pf->nobj * 4u + (size_t)(off32 & 0x7FFFFFFFu) * 8u;
                *offset_out = g_be64(off64);
            }
            return 1;
        }
        if (cmp < 0) lo = mid + 1u; else hi = mid;
    }
    return 0;
}

static int read_object(GitRepo *repo, const uint8_t sha1[20], int *type_out, uint8_t **data_out, size_t *len_out, int depth);

static int read_pack_at(GitRepo *repo, const PackFile *pf, uint64_t off,
                        int *type_out, uint8_t **data_out, size_t *len_out, int depth){
    if (depth > 32 || off >= pf->pack_len) return 0;
    size_t pos = (size_t)off;
    uint8_t c = pf->pack[pos++];
    int typ = (c >> 4) & 7;
    uint64_t sz = c & 0x0Fu;
    int sh = 4;
    while (c & 0x80u) {
        if (pos >= pf->pack_len || sh >= 64) return 0;
        c = pf->pack[pos++];
        sz |= (uint64_t)(c & 0x7Fu) << sh;
        sh += 7;
    }
    if (typ == GIT_OFS_DELTA) {
        if (pos >= pf->pack_len) return 0;
        uint8_t b = pf->pack[pos++];
        uint64_t back = b & 0x7Fu;
        while (b & 0x80u) {
            if (pos >= pf->pack_len) return 0;
            b = pf->pack[pos++];
            back = ((back + 1u) << 7) | (b & 0x7Fu);
        }
        if (back > off) return 0;
        uint8_t *base = NULL, *delta = NULL; size_t blen = 0, dlen = 0; int btype = 0;
        if (!read_pack_at(repo, pf, off - back, &btype, &base, &blen, depth + 1)) return 0;
        delta = inflate_exact(pf->pack + pos, pf->pack_len - pos, (size_t)sz, NULL, &dlen);
        if (!delta) { free(base); return 0; }
        int ok = apply_git_delta(base, blen, delta, dlen, data_out, len_out);
        *type_out = btype;
        free(base); free(delta);
        return ok;
    }
    if (typ == GIT_REF_DELTA) {
        if (pos + 20 > pf->pack_len) return 0;
        const uint8_t *base_sha = pf->pack + pos;
        pos += 20;
        uint8_t *base = NULL, *delta = NULL; size_t blen = 0, dlen = 0; int btype = 0;
        if (!read_object(repo, base_sha, &btype, &base, &blen, depth + 1)) return 0;
        delta = inflate_exact(pf->pack + pos, pf->pack_len - pos, (size_t)sz, NULL, &dlen);
        if (!delta) { free(base); return 0; }
        int ok = apply_git_delta(base, blen, delta, dlen, data_out, len_out);
        *type_out = btype;
        free(base); free(delta);
        return ok;
    }
    *type_out = typ;
    *data_out = inflate_exact(pf->pack + pos, pf->pack_len - pos, (size_t)sz, NULL, len_out);
    return *data_out != NULL;
}

static int read_object(GitRepo *repo, const uint8_t sha1[20], int *type_out, uint8_t **data_out, size_t *len_out, int depth){
    if (depth > 32) return 0;
    char hex[41]; sha1_to_hex(sha1, hex);
    char lpath[4200];
    snprintf(lpath, sizeof lpath, "%s/objects/%.2s/%s", repo->git_dir, hex, hex + 2);
    size_t zlen = 0;
    uint8_t *zbuf = slurp_file(lpath, &zlen);
    if (zbuf) {
        size_t ulen = 0;
        uint8_t *raw = inflate_exact(zbuf, zlen, 4096, NULL, &ulen);
        free(zbuf);
        if (!raw) return 0;
        const uint8_t *nul = memchr(raw, 0, ulen);
        if (!nul) { free(raw); return 0; }
        int typ = 0;
        if (!strncmp((const char *)raw, "commit ", 7)) typ = GIT_COMMIT;
        else if (!strncmp((const char *)raw, "tree ", 5)) typ = GIT_TREE;
        else if (!strncmp((const char *)raw, "blob ", 5)) typ = GIT_BLOB;
        else if (!strncmp((const char *)raw, "tag ", 4)) typ = GIT_TAG;
        else { free(raw); return 0; }
        size_t hdr = (size_t)(nul - raw) + 1u;
        size_t body = ulen - hdr;
        uint8_t *out = malloc(body ? body : 1u);
        if (body) memcpy(out, raw + hdr, body);
        free(raw);
        *type_out = typ;
        *data_out = out;
        *len_out = body;
        return 1;
    }
    for (size_t i = 0; i < repo->npacks; i++) {
        uint64_t off = 0;
        if (find_in_pack_idx(&repo->packs[i], sha1, &off)) {
            return read_pack_at(repo, &repo->packs[i], off, type_out, data_out, len_out, depth + 1);
        }
    }
    return 0;
}

static int valid_utf8_text(const uint8_t *p, size_t n){
    if (!n || n > 262144u || memchr(p, 0, n)) return 0;
    lp_id id;
    return lp_text_id(CTX[0], (const char *)p, n, &id) ? 1 : 0;
}

static Ref git_compose_object(GitRepo *repo, File *f, const uint8_t sha1[20], int depth){
    char hex[41]; sha1_to_hex(sha1, hex);
    int64_t at = lp_strmap_find(repo->cache, hex, 40);
    if (at >= 0) return repo->refs[*(size_t *)lp_strmap_at(repo->cache, (size_t)at)];
    if (depth > 64) return text_ref(CTX[0], (const uint8_t *)hex, 40);

    int typ = 0; uint8_t *raw = NULL; size_t len = 0;
    Ref res = text_ref(CTX[0], (const uint8_t *)hex, 40);
    if (!read_object(repo, sha1, &typ, &raw, &len, 0)) return res;

    if (typ == GIT_BLOB) {
        res = len && valid_utf8_text(raw, len)
            ? text_ref(CTX[0], raw, len)
            : text_ref(CTX[0], (const uint8_t *)hex, 40);
    } else if (typ == GIT_TREE) {
        lp_vec(Ref) entries = { 0 };
        size_t pos = 0;
        while (pos < len) {
            const uint8_t *sp = memchr(raw + pos, ' ', len - pos);
            if (!sp) break;
            const uint8_t *nul = memchr(sp + 1, 0, len - (size_t)(sp + 1 - raw));
            if (!nul || (size_t)(nul + 1 - raw) + 20u > len) break;
            size_t ml = (size_t)(sp - (raw + pos));
            size_t nl = (size_t)(nul - (sp + 1));
            const uint8_t *csha = nul + 1;
            Ref target = git_compose_object(repo, f, csha, depth + 1);
            Ref item[3] = {
                text_ref(CTX[0], raw + pos, ml),
                text_ref(CTX[0], sp + 1, nl),
                target
            };
            lp_push(&entries, compose(item, 3, ref_above(item, 3)));
            pos = (size_t)(csha + 20u - raw);
        }
        res = entries.n ? compose(entries.v, (uint32_t)entries.n, ref_above(entries.v, entries.n))
                        : text_ref(CTX[0], (const uint8_t *)hex, 40);
        lp_vec_free(&entries);
    } else if (typ == GIT_COMMIT) {
        Ref tree_ref = { 0 }; int has_tree = 0;
        lp_vec(Ref) parents = { 0 };
        const char *author = NULL; size_t author_len = 0;
        const char *msg = NULL; size_t msg_len = 0;
        size_t pos = 0;
        while (pos < len) {
            const uint8_t *nl = memchr(raw + pos, '\n', len - pos);
            size_t ll = nl ? (size_t)(nl - (raw + pos)) : (len - pos);
            if (ll == 0) {
                if (nl) { msg = (const char *)(nl + 1); msg_len = len - (size_t)(nl + 1 - raw); }
                break;
            }
            const char *line = (const char *)(raw + pos);
            if (ll >= 45 && !strncmp(line, "tree ", 5)) {
                uint8_t ts[20];
                if (hex_to_sha1(line + 5, ts)) { tree_ref = git_compose_object(repo, f, ts, depth + 1); has_tree = 1; }
            } else if (ll >= 47 && !strncmp(line, "parent ", 7)) {
                uint8_t ps[20];
                if (hex_to_sha1(line + 7, ps)) lp_push(&parents, git_compose_object(repo, f, ps, depth + 1));
            } else if (ll > 7 && !strncmp(line, "author ", 7)) {
                author = line + 7; author_len = ll - 7;
            }
            pos = nl ? (size_t)(nl + 1 - raw) : len;
        }
        lp_vec(Ref) parts = { 0 };
        lp_push(&parts, text_ref(CTX[0], (const uint8_t *)hex, 40));
        if (has_tree) lp_push(&parts, tree_ref);
        if (author && author_len) lp_push(&parts, text_ref(CTX[0], (const uint8_t *)author, author_len));
        if (msg && msg_len) lp_push(&parts, text_ref(CTX[0], (const uint8_t *)msg, msg_len));
        res = compose(parts.v, (uint32_t)parts.n, ref_above(parts.v, parts.n));

        lp_vec(Ref) gclaims = { 0 };
        if (has_tree) {
            Ref p3[3] = { res, text_ref(CTX[0], (const uint8_t *)"tree", 4), tree_ref };
            lp_push(&gclaims, compose(p3, 3, ref_above(p3, 3)));
        }
        for (size_t pi = 0; pi < parents.n; pi++) {
            Ref p3[3] = { res, text_ref(CTX[0], (const uint8_t *)"parent", 6), parents.v[pi] };
            lp_push(&gclaims, compose(p3, 3, ref_above(p3, 3)));
        }
        if (gclaims.n > 0) lp_codec_emit_claim_record(f, &res, gclaims.v, gclaims.n);
        lp_vec_free(&gclaims);
        f->records++;
        lp_vec_free(&parts);
        lp_vec_free(&parents);
    }
    free(raw);

    bool fresh = false;
    size_t *slot = lp_strmap_get(repo->cache, hex, 40, &fresh);
    if (fresh) {
        if (repo->nrefs == repo->crefs) {
            repo->crefs = repo->crefs ? repo->crefs * 2u : 128u;
            repo->refs = xrealloc(repo->refs, sizeof *repo->refs * repo->crefs);
        }
        *slot = repo->nrefs;
        repo->refs[repo->nrefs++] = res;
    }
    return res;
}

static void git_open_packs(GitRepo *repo){
    char pdir[4200];
    snprintf(pdir, sizeof pdir, "%s/objects/pack", repo->git_dir);
    os_list ls = { 0 };
    if (os_list_dir(pdir, &ls) != 0) return;
    for (size_t i = 0; i < ls.n; i++) {
        size_t l = strlen(ls.item[i]);
        if (l < 5 || strcmp(ls.item[i] + l - 4, ".idx")) continue;
        char idxp[4300], packp[4300];
        snprintf(idxp, sizeof idxp, "%s/%s", pdir, ls.item[i]);
        snprintf(packp, sizeof packp, "%s/%.*s.pack", pdir, (int)(l - 4), ls.item[i]);
        size_t ilen = 0, plen = 0;
        uint8_t *ib = slurp_file(idxp, &ilen), *pb = slurp_file(packp, &plen);
        if (ib && pb && ilen >= 8 + 256u * 4u && plen >= 12 && !memcmp(pb, "PACK", 4)) {
            repo->packs = xrealloc(repo->packs, sizeof *repo->packs * (repo->npacks + 1u));
            repo->packs[repo->npacks++] = (PackFile){
                .pack = pb, .pack_len = plen,
                .idx = ib, .idx_len = ilen,
                .nobj = g_be32(ib + 8 + 255u * 4u)
            };
        } else {
            free(ib); free(pb);
        }
    }
    os_list_free(&ls);
}

static void git_repo_free(GitRepo *repo){
    for (size_t i = 0; i < repo->npacks; i++) {
        free((void *)repo->packs[i].pack);
        free((void *)repo->packs[i].idx);
    }
    free(repo->packs);
    if (repo->cache) lp_strmap_free(repo->cache);
    free(repo->refs);
}

static void resolve_git_ref(GitRepo *repo, const char *ref_text, size_t n, uint8_t sha_out[20], int *ok){
    *ok = 0;
    char buf[512];
    size_t l = n < sizeof buf - 1 ? n : sizeof buf - 1;
    memcpy(buf, ref_text, l); buf[l] = 0;
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == ' ')) buf[--l] = 0;
    if (!strncmp(buf, "ref: ", 5)) {
        char rp[4200];
        snprintf(rp, sizeof rp, "%s/%s", repo->git_dir, buf + 5);
        size_t rl = 0;
        uint8_t *rb = slurp_file(rp, &rl);
        if (rb && rl >= 40) { *ok = hex_to_sha1((const char *)rb, sha_out); free(rb); return; }
        free(rb);
        snprintf(rp, sizeof rp, "%s/packed-refs", repo->git_dir);
        rb = slurp_file(rp, &rl);
        if (rb) {
            const char *p = (const char *)rb, *end = p + rl;
            while (p < end) {
                const char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
                if (len > 41 && p[40] == ' ' && !strncmp(p + 41, buf + 5, strlen(buf + 5))) {
                    *ok = hex_to_sha1(p, sha_out);
                    break;
                }
                p = nl ? nl + 1 : end;
            }
            free(rb);
        }
        return;
    }
    if (l >= 40) *ok = hex_to_sha1(buf, sha_out);
}

static void git_read(const Recipe *r, File *f, const uint8_t *src, size_t n){
    (void)r;
    GitRepo repo = { 0 };
    repo.cache = lp_strmap_sized(sizeof(size_t));

    /* Resolve enclosing .git directory from f->path */
    snprintf(repo.git_dir, sizeof repo.git_dir, "%s", f->path);
    os_slashes(repo.git_dir);
    char *dotgit = strstr(repo.git_dir, "/.git");
    if (dotgit) dotgit[5] = 0;
    else {
        char *sl = strrchr(repo.git_dir, '/');
        if (sl) *sl = 0; else snprintf(repo.git_dir, sizeof repo.git_dir, ".");
    }
    git_open_packs(&repo);

    /* Case 1: direct loose zlib object file */
    size_t ulen = 0;
    uint8_t *loose = inflate_exact(src, n, 4096, NULL, &ulen);
    if (loose) {
        const uint8_t *nul = memchr(loose, 0, ulen);
        if (nul && (!strncmp((const char *)loose, "commit ", 7)
                 || !strncmp((const char *)loose, "tree ", 5)
                 || !strncmp((const char *)loose, "blob ", 5))) {
            size_t hdr = (size_t)(nul - loose) + 1u;
            Ref root = text_ref(CTX[0], loose + hdr, ulen - hdr);
            f->trunk = root;
            f->laid = 0;
            f->records += 1;
            free(loose);
            git_repo_free(&repo);
            return;
        }
        free(loose);
    }

    /* Case 2: .git/HEAD or ref file */
    uint8_t head_sha[20]; int has_head = 0;
    resolve_git_ref(&repo, (const char *)src, n, head_sha, &has_head);
    if (!has_head) {
        git_repo_free(&repo);
        f->trunk = text_ref(CTX[0], src, n);
        f->laid = 0;
        return;
    }

    Ref head_commit = git_compose_object(&repo, f, head_sha, 0);
    f->trunk = head_commit;
    f->laid = 0;
    git_repo_free(&repo);
}

const NativeGrammar git_grammar = {
    "git", git_says, git_done, git_read, NULL, NULL, NULL
};
