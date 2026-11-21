#ifndef LAPLACE_RECIPE_PAIR_H
#define LAPLACE_RECIPE_PAIR_H

/* Parameterized parallel bitext pair matching, ISO 639-3 normalization, and
 * pair-local key-domain scoping (shared across ingest, say, and recipe loading). */
#include "engine.h"
#include "laplace/os.h"
#include "laplace/highway.h"
#include <blake3.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { PAIR_ROLE_NONE = 0, PAIR_ROLE_LEAD = 1, PAIR_ROLE_PARTNER = 2 };

typedef struct {
    uint8_t active;
    uint8_t role;
    char pair[64];
    char lang[32];
    char partner[32];
    char iso3[8];
} PairBinding;

static inline int lp_recipe_iso639_3(const char *raw, char out[8]){
    if (!raw || !*raw) return 0;
    char low[32], norm[32];
    size_t len = strlen(raw);
    if (!len || len >= sizeof low) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)raw[i];
        low[i] = (char)tolower(c);
        norm[i] = low[i] == '-' ? '_' : low[i];
    }
    low[len] = 0;
    norm[len] = 0;

    static const struct { const char *code, *iso3; } OVERRIDES[] = {
        {"zh_cn", "cmn"}, {"zh_hans", "cmn"}, {"zh_sg", "cmn"},
        {"zh_tw", "zho"}, {"zh_hant", "zho"}, {"zh_hk", "zho"},
        {"pt_br", "por"}, {"pt_pt", "por"},
        {"ze_en", "eng"}, {"ze_zh", "zho"}
    };
    for (size_t i = 0; i < sizeof OVERRIDES / sizeof OVERRIDES[0]; i++) {
        if (!strcmp(norm, OVERRIDES[i].code)) {
            snprintf(out, 8, "%s", OVERRIDES[i].iso3);
            return 1;
        }
    }

    if (HW) {
        const lp_tier0_record *rec = lp_highway_resolve(HW, "language", low);
        if (rec) {
            const char *const *parts = NULL;
            size_t np = lp_highway_identifier_parts_for_id(HW, &rec->id, &parts);
            if (np == 1 && parts && parts[0] && strlen(parts[0]) == 3) {
                memcpy(out, parts[0], 4);
                return 1;
            }
        }
    }

    static const struct { const char *iso1, *iso3; } FALLBACK[] = {
        {"aa", "aar"}, {"ab", "abk"}, {"ae", "ave"}, {"af", "afr"}, {"ak", "aka"}, {"am", "amh"},
        {"an", "arg"}, {"ar", "ara"}, {"as", "asm"}, {"av", "ava"}, {"ay", "aym"}, {"az", "aze"},
        {"ba", "bak"}, {"be", "bel"}, {"bg", "bul"}, {"bh", "bih"}, {"bi", "bis"}, {"bm", "bam"},
        {"bn", "ben"}, {"bo", "bod"}, {"br", "bre"}, {"bs", "bos"}, {"ca", "cat"}, {"ce", "che"},
        {"ch", "cha"}, {"co", "cos"}, {"cr", "cre"}, {"cs", "ces"}, {"cu", "chu"}, {"cv", "chv"},
        {"cy", "cym"}, {"da", "dan"}, {"de", "deu"}, {"dv", "div"}, {"dz", "dzo"}, {"ee", "ewe"},
        {"el", "ell"}, {"en", "eng"}, {"eo", "epo"}, {"es", "spa"}, {"et", "est"}, {"eu", "eus"},
        {"fa", "fas"}, {"ff", "ful"}, {"fi", "fin"}, {"fj", "fij"}, {"fo", "fao"}, {"fr", "fra"},
        {"fy", "fry"}, {"ga", "gle"}, {"gd", "gla"}, {"gl", "glg"}, {"gn", "grn"}, {"gu", "guj"},
        {"gv", "glv"}, {"ha", "hau"}, {"he", "heb"}, {"hi", "hin"}, {"ho", "hmo"}, {"hr", "hrv"},
        {"ht", "hat"}, {"hu", "hun"}, {"hy", "hye"}, {"hz", "her"}, {"ia", "ina"}, {"id", "ind"},
        {"ie", "ile"}, {"ig", "ibo"}, {"ii", "iii"}, {"ik", "ipk"}, {"in", "ind"}, {"io", "ido"},
        {"is", "isl"}, {"it", "ita"}, {"iu", "iku"}, {"iw", "heb"}, {"ja", "jpn"}, {"ji", "yid"},
        {"jv", "jav"}, {"jw", "jav"}, {"ka", "kat"}, {"kg", "kon"}, {"ki", "kik"}, {"kj", "kua"},
        {"kk", "kaz"}, {"kl", "kal"}, {"km", "khm"}, {"kn", "kan"}, {"ko", "kor"}, {"kr", "kau"},
        {"ks", "kas"}, {"ku", "kur"}, {"kv", "kom"}, {"kw", "cor"}, {"ky", "kir"}, {"la", "lat"},
        {"lb", "ltz"}, {"lg", "lug"}, {"li", "lim"}, {"ln", "lin"}, {"lo", "lao"}, {"lt", "lit"},
        {"lu", "lub"}, {"lv", "lav"}, {"mg", "mlg"}, {"mh", "mah"}, {"mi", "mri"}, {"mk", "mkd"},
        {"ml", "mal"}, {"mn", "mon"}, {"mo", "ron"}, {"mr", "mar"}, {"ms", "msa"}, {"mt", "mlt"},
        {"my", "mya"}, {"na", "nau"}, {"nb", "nob"}, {"nd", "nde"}, {"ne", "nep"}, {"ng", "ndo"},
        {"nl", "nld"}, {"nn", "nno"}, {"no", "nor"}, {"nr", "nbl"}, {"nv", "nav"}, {"ny", "nya"},
        {"oc", "oci"}, {"oj", "oji"}, {"om", "orm"}, {"or", "ori"}, {"os", "oss"}, {"pa", "pan"},
        {"pi", "pli"}, {"pl", "pol"}, {"ps", "pus"}, {"pt", "por"}, {"qu", "que"}, {"rm", "roh"},
        {"rn", "run"}, {"ro", "ron"}, {"ru", "rus"}, {"rw", "kin"}, {"sa", "san"}, {"sc", "srd"},
        {"sd", "snd"}, {"se", "sme"}, {"sg", "sag"}, {"sh", "hbs"}, {"si", "sin"}, {"sk", "slk"},
        {"sl", "slv"}, {"sm", "smo"}, {"sn", "sna"}, {"so", "som"}, {"sq", "sqi"}, {"sr", "srp"},
        {"ss", "ssw"}, {"st", "sot"}, {"su", "sun"}, {"sv", "swe"}, {"sw", "swa"}, {"ta", "tam"},
        {"te", "tel"}, {"tg", "tgk"}, {"th", "tha"}, {"ti", "tir"}, {"tk", "tuk"}, {"tl", "tgl"},
        {"tn", "tsn"}, {"to", "ton"}, {"tr", "tur"}, {"ts", "tso"}, {"tt", "tat"}, {"tw", "twi"},
        {"ty", "tah"}, {"ug", "uig"}, {"uk", "ukr"}, {"ur", "urd"}, {"uz", "uzb"}, {"ve", "ven"},
        {"vi", "vie"}, {"vo", "vol"}, {"wa", "wln"}, {"wo", "wol"}, {"xh", "xho"}, {"yi", "yid"},
        {"yo", "yor"}, {"za", "zha"}, {"zh", "zho"}, {"zu", "zul"}
    };
    for (size_t i = 0; i < sizeof FALLBACK / sizeof FALLBACK[0]; i++) {
        if (!strcmp(low, FALLBACK[i].iso1)) {
            snprintf(out, 8, "%s", FALLBACK[i].iso3);
            return 1;
        }
    }
    if (len == 3 && isalpha((unsigned char)low[0]) && isalpha((unsigned char)low[1]) && isalpha((unsigned char)low[2])) {
        memcpy(out, low, 4);
        return 1;
    }
    return 0;
}

static inline int lp_recipe_pair_bind(const char *pattern, const char *path, PairBinding *out){
    if (!pattern || !*pattern || !path || !*path) return 0;
    const char *tpl = strstr(pattern, "{l1}-{l2}.{lang}");
    if (!tpl || tpl[16]) return 0;
    size_t prefix_len = (size_t)(tpl - pattern);

    const char *b0 = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b0 = p + 1;
    char base[1024];
    snprintf(base, sizeof base, "%s", b0);
    size_t bl = strlen(base);
    if (bl > 3 && !strcmp(base + bl - 3, ".gz")) base[bl -= 3] = 0;
    if (bl <= prefix_len || memcmp(base, pattern, prefix_len)) return 0;

    const char *rest = base + prefix_len;
    const char *dot = strrchr(rest, '.');
    if (!dot || dot == rest || !dot[1]) return 0;
    size_t pair_len = (size_t)(dot - rest);
    const char *side = dot + 1;
    size_t side_len = strlen(side);
    if (!pair_len || pair_len >= 64 || !side_len || side_len >= 32) return 0;

    for (size_t i = 0; i < pair_len; i++) {
        unsigned char c = (unsigned char)rest[i];
        if (!isalnum(c) && c != '-' && c != '_') return 0;
    }
    for (size_t i = 0; i < side_len; i++) {
        unsigned char c = (unsigned char)side[i];
        if (!isalnum(c) && c != '-' && c != '_') return 0;
    }

    const char *partner_ptr = NULL;
    size_t partner_len = 0;
    uint8_t role = PAIR_ROLE_NONE;
    if (pair_len > side_len + 1 && rest[pair_len - side_len - 1] == '-' &&
        !strncasecmp(rest + pair_len - side_len, side, side_len)) {
        role = PAIR_ROLE_PARTNER;
        partner_ptr = rest;
        partner_len = pair_len - side_len - 1;
    } else if (pair_len > side_len + 1 && rest[side_len] == '-' &&
               !strncasecmp(rest, side, side_len)) {
        role = PAIR_ROLE_LEAD;
        partner_ptr = rest + side_len + 1;
        partner_len = pair_len - side_len - 1;
    } else {
        return 0;
    }
    if (!partner_len || partner_len >= 32) return 0;
    if (side_len == partner_len && !strncasecmp(side, partner_ptr, side_len)) return 0;

    char l_side[32], l_partner[32], iso_side[8], iso_partner[8];
    for (size_t i = 0; i < side_len; i++) l_side[i] = (char)tolower((unsigned char)side[i]);
    l_side[side_len] = 0;
    for (size_t i = 0; i < partner_len; i++) l_partner[i] = (char)tolower((unsigned char)partner_ptr[i]);
    l_partner[partner_len] = 0;
    if (!lp_recipe_iso639_3(l_side, iso_side) || !lp_recipe_iso639_3(l_partner, iso_partner)) return 0;

    if (out) {
        memset(out, 0, sizeof *out);
        out->active = 1;
        out->role = role;
        for (size_t i = 0; i < pair_len; i++) out->pair[i] = (char)tolower((unsigned char)rest[i]);
        out->pair[pair_len] = 0;
        snprintf(out->lang, sizeof out->lang, "%s", l_side);
        snprintf(out->partner, sizeof out->partner, "%s", l_partner);
        snprintf(out->iso3, sizeof out->iso3, "%s", iso_side);
    }
    return 1;
}

static inline const char *lp_pair_role_name(uint8_t role){
    return role == PAIR_ROLE_LEAD ? "lead" : role == PAIR_ROLE_PARTNER ? "partner" : "";
}

static inline int lp_pair_is_token(const char *name){
    return name && (!strcmp(name, "{role}") || !strcmp(name, "{lang}") ||
                    !strcmp(name, "{partner}") || !strcmp(name, "{pair}") ||
                    !strcmp(name, "{iso3}"));
}

static inline const char *lp_pair_token_value(const PairBinding *pair, const char *name){
    if (!pair || !pair->active || !name) return "";
    if (!strcmp(name, "{role}")) return lp_pair_role_name(pair->role);
    if (!strcmp(name, "{lang}")) return pair->lang;
    if (!strcmp(name, "{partner}")) return pair->partner;
    if (!strcmp(name, "{pair}")) return pair->pair;
    if (!strcmp(name, "{iso3}")) return pair->iso3;
    return "";
}

static inline int lp_pair_expand(const char *src, const PairBinding *pair, char *out, size_t cap, int *had_token){
    if (had_token) *had_token = 0;
    if (!src || !strchr(src, '{')) return 0;
    size_t u = 0;
    int saw = 0;
    for (const char *p = src; *p; ) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (end && (size_t)(end - p + 1) < 32) {
                char tok[32];
                size_t tl = (size_t)(end - p + 1);
                memcpy(tok, p, tl);
                tok[tl] = 0;
                if (lp_pair_is_token(tok)) {
                    saw = 1;
                    const char *val = lp_pair_token_value(pair, tok);
                    if (!val[0]) { if (had_token) *had_token = 1; return 0; }
                    size_t vl = strlen(val);
                    if (u + vl >= cap) { if (had_token) *had_token = 1; return 0; }
                    memcpy(out + u, val, vl);
                    u += vl;
                    p = end + 1;
                    continue;
                }
            }
        }
        if (u + 1 >= cap) return 0;
        out[u++] = *p++;
    }
    if (had_token) *had_token = saw;
    if (!saw) return 0;
    out[u] = 0;
    return 1;
}

static inline lp_id lp_pair_effective_domain(lp_id base, const PairBinding *pair, int scoped){
    if (!scoped || !pair || !pair->active || !pair->pair[0]) return base;
    blake3_hasher hash;
    lp_id out;
    blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, base.b, sizeof base.b);
    blake3_hasher_update(&hash, pair->pair, strlen(pair->pair));
    blake3_hasher_finalize(&hash, out.b, sizeof out.b);
    return out;
}

int lp_say_pair_match(const Recipe *recipe, const char *path);
size_t lp_say_file_dependency(const Recipe *recipe, const char *path);

#endif
