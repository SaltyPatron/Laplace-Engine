/* Syzygy endgame tablebases (Ronald de Man's format, syzygy-tables.info) as a read: chess_tablebase.h.
 *
 * A table, KQvK.rtbw or KQvK.rtbz, holds a verdict for every position of its material, White's or Black's to move, the
 * material either way round (KQvK is also KvKQ): its WDL file the win/draw/loss under the 50-move rule, its DTZ file
 * the distance to zeroing, the plies to the next capture or pawn move under best play. Neither is a list of positions:
 * each is an index over the material's placements, folded by the board's symmetries and compressed, its values right
 * only where no capture decides them, and the DTZ file holds one side to move. So the verdict of a position is a probe:
 * the captures searched (the tables of the material they leave), then the table. That is the format's own law, as
 * Fathom (Ronald de Man, basil00, Jon Dart; MIT) implements it: its index encoding, pair decompression, probe_wdl,
 * probe_dtz and root ranking are carried over here; every move, check and position is chess_rules.c's (the one chess
 * this Engine has). A table is mapped when a probe first needs it and unmapped when the cache is full and it is the
 * least recently used: nothing is read whole, and no table is loaded because another could reach it. */
#include "chess_tablebase.h"
#include "os.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TB_WDL, TB_DTZ };
#define TB_PIECES 7
#define TB_MAX_SYMS 4096
static const char *const SUFFIX[2] = { ".rtbw", ".rtbz" };
static const uint32_t MAGIC[2] = { 0x5d23e871, 0xa50c66d7 };

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p){ return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; }
static uint64_t be64(const uint8_t *p){ return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* ---- the index: placements of the pieces, folded by symmetry (Fathom's tables and encode) */
static const int8_t OffDiag[64] = {
  0,-1,-1,-1,-1,-1,-1,-1, 1, 0,-1,-1,-1,-1,-1,-1, 1, 1, 0,-1,-1,-1,-1,-1, 1, 1, 1, 0,-1,-1,-1,-1,
  1, 1, 1, 1, 0,-1,-1,-1, 1, 1, 1, 1, 1, 0,-1,-1, 1, 1, 1, 1, 1, 1, 0,-1, 1, 1, 1, 1, 1, 1, 1, 0 };
static const uint8_t Triangle[64] = {
  6, 0, 1, 2, 2, 1, 0, 6, 0, 7, 3, 4, 4, 3, 7, 0, 1, 3, 8, 5, 5, 8, 3, 1, 2, 4, 5, 9, 9, 5, 4, 2,
  2, 4, 5, 9, 9, 5, 4, 2, 1, 3, 8, 5, 5, 8, 3, 1, 0, 7, 3, 4, 4, 3, 7, 0, 6, 0, 1, 2, 2, 1, 0, 6 };
static const uint8_t FlipDiag[64] = {
  0, 8,16,24,32,40,48,56, 1, 9,17,25,33,41,49,57, 2,10,18,26,34,42,50,58, 3,11,19,27,35,43,51,59,
  4,12,20,28,36,44,52,60, 5,13,21,29,37,45,53,61, 6,14,22,30,38,46,54,62, 7,15,23,31,39,47,55,63 };
static const uint8_t Lower[64] = {
 28, 0, 1, 2, 3, 4, 5, 6, 0,29, 7, 8, 9,10,11,12, 1, 7,30,13,14,15,16,17, 2, 8,13,31,18,19,20,21,
  3, 9,14,18,32,22,23,24, 4,10,15,19,22,33,25,26, 5,11,16,20,23,25,34,27, 6,12,17,21,24,26,27,35 };
static const uint8_t Diag[64] = {
  0, 0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0, 0, 9, 0, 0, 0, 2, 0, 0,10, 0, 0, 0, 0, 0, 3,11, 0, 0, 0,
  0, 0, 0,12, 4, 0, 0, 0, 0, 0,13, 0, 0, 5, 0, 0, 0,14, 0, 0, 0, 0, 6, 0,15, 0, 0, 0, 0, 0, 0, 7 };
static const uint8_t Flap[64] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 6,12,18,18,12, 6, 0, 1, 7,13,19,19,13, 7, 1, 2, 8,14,20,20,14, 8, 2,
  3, 9,15,21,21,15, 9, 3, 4,10,16,22,22,16,10, 4, 5,11,17,23,23,17,11, 5, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t PawnTwist[64] = {
  0, 0, 0, 0, 0, 0, 0, 0,47,35,23,11,10,22,34,46,45,33,21, 9, 8,20,32,44,43,31,19, 7, 6,18,30,42,
 41,29,17, 5, 4,16,28,40,39,27,15, 3, 2,14,26,38,37,25,13, 1, 0,12,24,36, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int16_t KKIdx[10][64] = {
  { -1, -1, -1,  0,  1,  2,  3,  4, -1, -1, -1,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 },
  { 58, -1, -1, -1, 59, 60, 61, 62, 63, -1, -1, -1, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115 },
  {116,117, -1, -1, -1,118,119,120,121,122, -1, -1, -1,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,
   142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173 },
  {174, -1, -1, -1,175,176,177,178,179, -1, -1, -1,180,181,182,183,184, -1, -1, -1,185,186,187,188,189,190,191,192,193,194,195,196,
   197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228 },
  {229,230, -1, -1, -1,231,232,233,234,235, -1, -1, -1,236,237,238,239,240, -1, -1, -1,241,242,243,244,245,246,247,248,249,250,251,
   252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271,272,273,274,275,276,277,278,279,280,281,282,283 },
  {284,285,286,287,288,289,290,291,292,293, -1, -1, -1,294,295,296,297,298, -1, -1, -1,299,300,301,302,303, -1, -1, -1,304,305,306,
   307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338 },
  { -1, -1,339,340,341,342,343,344, -1, -1,345,346,347,348,349,350, -1, -1,441,351,352,353,354,355, -1, -1, -1,442,356,357,358,359,
    -1, -1, -1, -1,443,360,361,362, -1, -1, -1, -1, -1,444,363,364, -1, -1, -1, -1, -1, -1,445,365, -1, -1, -1, -1, -1, -1, -1,446 },
  { -1, -1, -1,366,367,368,369,370, -1, -1, -1,371,372,373,374,375, -1, -1, -1,376,377,378,379,380, -1, -1, -1,447,381,382,383,384,
    -1, -1, -1, -1,448,385,386,387, -1, -1, -1, -1, -1,449,388,389, -1, -1, -1, -1, -1, -1,450,390, -1, -1, -1, -1, -1, -1, -1,451 },
  {452,391,392,393,394,395,396,397, -1, -1, -1, -1,398,399,400,401, -1, -1, -1, -1,402,403,404,405, -1, -1, -1, -1,406,407,408,409,
    -1, -1, -1, -1,453,410,411,412, -1, -1, -1, -1, -1,454,413,414, -1, -1, -1, -1, -1, -1,455,415, -1, -1, -1, -1, -1, -1, -1,456 },
  {457,416,417,418,419,420,421,422, -1,458,423,424,425,426,427,428, -1, -1, -1, -1, -1,429,430,431, -1, -1, -1, -1, -1,432,433,434,
    -1, -1, -1, -1, -1,435,436,437, -1, -1, -1, -1, -1,459,438,439, -1, -1, -1, -1, -1, -1,460,440, -1, -1, -1, -1, -1, -1, -1,461 } };
static const uint8_t FileToFile[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };
static const int WdlToMap[5] = { 1, 3, 0, 2, 0 };
static const uint8_t PAFlags[5] = { 8, 0, 0, 0, 4 };
static size_t Binomial[7][64], PawnIdx[6][24], PawnFactorFile[6][4];
static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER; static int indices_ready;      /* pthread_once is not on every platform os.h covers */

static void indices_init(void){
    for (int i = 0; i < 7; i++) for (int j = 0; j < 64; j++) { size_t f = 1, l = 1; for (int k = 0; k < i; k++) { f *= (size_t)(j - k); l *= (size_t)(k + 1); } Binomial[i][j] = f / l; }
    for (int i = 0; i < 6; i++) { size_t s = 0;
        for (int j = 0; j < 24; j++) { PawnIdx[i][j] = s; s += Binomial[i][PawnTwist[(1 + (j % 6)) * 8 + (j / 6)]]; if ((j + 1) % 6 == 0) { PawnFactorFile[i][j / 6] = s; s = 0; } } }
}

typedef struct {
    const uint8_t *indexTable, *sizeTable, *data, *offset, *symPat;
    uint8_t *symLen; uint64_t *base;
    uint8_t blockSize, idxBits, minLen, constValue[2];
} Pairs;
typedef struct { Pairs *precomp; size_t factor[TB_PIECES]; uint8_t pieces[TB_PIECES], norm[TB_PIECES]; } Enc;
/* A material's table: its files, mapped while state is 1. key: its material as named (White's side first). */
typedef struct {
    uint64_t key; char name[16], path[2][1100]; int num, symmetric, pawned, kk_enc, has_dtz; uint8_t pawns[2];
    const uint8_t *map[2]; size_t size[2];
    Enc wdl[8], dtz[4];                               /* pieces: wdl[bside], dtz[0]; pawns: wdl[file + 4 bside], dtz[file] */
    const uint8_t *dtzMap; uint16_t dtzMapIdx[4][4]; uint8_t dtzFlags[4];
    int state, uses; uint64_t last;                   /* state 0 not mapped, 1 mapped, -1 not a table */
} Table;
struct LpTablebase { Table **t; int n, cap, max_open, open, peak; uint64_t tick, maps, unmaps; pthread_mutex_t mu; };

static size_t subfactor(size_t k, size_t n){ size_t f = n, l = 1; for (size_t i = 1; i < k; i++) { f *= n - i; l *= i + 1; } return f / l; }
static size_t enc_init(Enc *ei, const Table *t, const uint8_t *tb, int shift, int file){
    int more = t->pawned && t->pawns[1] > 0;
    for (int i = 0; i < t->num; i++) { ei->pieces[i] = (uint8_t)((tb[i + 1 + more] >> shift) & 0x0f); ei->norm[i] = 0; }
    int order = (tb[0] >> shift) & 0x0f, order2 = more ? (tb[1] >> shift) & 0x0f : 0x0f;
    int k = ei->norm[0] = (uint8_t)(t->pawned ? t->pawns[0] : t->kk_enc ? 2 : 3);
    if (more) { ei->norm[k] = t->pawns[1]; k += ei->norm[k]; }
    for (int i = k; i < t->num; i += ei->norm[i]) for (int j = i; j < t->num && ei->pieces[j] == ei->pieces[i]; j++) ei->norm[i]++;
    int n = 64 - k; size_t f = 1;
    for (int i = 0; k < t->num || i == order || i == order2; i++) {
        if (i == order) { ei->factor[0] = f; f *= t->pawned ? PawnFactorFile[ei->norm[0] - 1][file] : t->kk_enc ? 462 : 31332; }
        else if (i == order2) { ei->factor[ei->norm[0]] = f; f *= subfactor(ei->norm[ei->norm[0]], (size_t)(48 - ei->norm[0])); }
        else { ei->factor[k] = f; f *= subfactor(ei->norm[k], (size_t)n); n -= ei->norm[k]; k += ei->norm[k]; } }
    return f;
}
static void sym_len(Pairs *d, uint32_t s, char *seen){
    const uint8_t *w = d->symPat + 3 * s; uint32_t s2 = (uint32_t)(w[2] << 4) | (w[1] >> 4);
    if (s2 == 0x0fff) d->symLen[s] = 0;
    else { uint32_t s1 = (uint32_t)((w[1] & 0xf) << 8) | w[0]; if (!seen[s1]) sym_len(d, s1, seen); if (!seen[s2]) sym_len(d, s2, seen); d->symLen[s] = (uint8_t)(d->symLen[s1] + d->symLen[s2] + 1); }
    seen[s] = 1;
}
/* A table's pairs header: its sizes (index table, size table, data) in size[3]. NULL: it is malformed. */
static Pairs *pairs_setup(const uint8_t **at, size_t tb_size, size_t size[3], uint8_t *flags, int type){
    const uint8_t *data = *at; Pairs *d = calloc(1, sizeof *d); *flags = data[0];
    if (data[0] & 0x80) { d->constValue[0] = type == TB_WDL ? data[1] : 0; *at = data + 2; size[0] = size[1] = size[2] = 0; return d; }
    d->blockSize = data[1]; d->idxBits = data[2]; uint32_t real = le32(data + 4), blocks = real + data[3];
    int maxLen = data[8]; d->minLen = data[9]; int h = maxLen - d->minLen + 1; uint32_t nsyms = le16(data + 10 + 2 * h);
    if (h <= 0 || nsyms >= TB_MAX_SYMS) { free(d); return NULL; }
    d->offset = data + 10; d->symPat = data + 12 + 2 * h; d->symLen = calloc(nsyms ? nsyms : 1, 1); d->base = calloc((size_t)h, sizeof(uint64_t));
    *at = data + 12 + 2 * h + 3 * nsyms + (nsyms & 1);
    size_t indices = (tb_size + ((size_t)1 << d->idxBits) - 1) >> d->idxBits;
    size[0] = 6 * indices; size[1] = 2 * (size_t)blocks; size[2] = (size_t)real << d->blockSize;
    char seen[TB_MAX_SYMS]; memset(seen, 0, nsyms);
    for (uint32_t s = 0; s < nsyms; s++) if (!seen[s]) sym_len(d, s, seen);
    d->base[h - 1] = 0;
    for (int i = h - 2; i >= 0; i--) d->base[i] = (d->base[i + 1] + le16(d->offset + 2 * i) - le16(d->offset + 2 * (i + 1))) / 2;
    for (int i = 0; i < h; i++) d->base[i] <<= 64 - (d->minLen + i);
    return d;
}
static const uint8_t *pairs_value(const Pairs *d, size_t idx){
    if (!d->idxBits) return d->constValue;
    uint32_t main = (uint32_t)(idx >> d->idxBits); int lit = (int)(idx & (((size_t)1 << d->idxBits) - 1)) - (1 << (d->idxBits - 1));
    uint32_t block = le32(d->indexTable + 6 * main); lit += le16(d->indexTable + 6 * main + 4);
    if (lit < 0) while (lit < 0) lit += le16(d->sizeTable + 2 * --block) + 1;
    else while (lit > le16(d->sizeTable + 2 * block)) lit -= le16(d->sizeTable + 2 * block++) + 1;
    const uint8_t *ptr = d->data + ((size_t)block << d->blockSize);
    int m = d->minLen; uint64_t code = be64(ptr); ptr += 8; uint32_t bits = 0, sym;
    for (;;) {
        int l = m; while (code < d->base[l - m]) l++;
        sym = le16(d->offset + 2 * (l - m)) + (uint32_t)((code - d->base[l - m]) >> (64 - l));
        if (lit < (int)d->symLen[sym] + 1) break;
        lit -= (int)d->symLen[sym] + 1; code <<= l; bits += (uint32_t)l;
        if (bits >= 32) { bits -= 32; code |= (uint64_t)be32(ptr) << bits; ptr += 4; } }
    while (d->symLen[sym]) { const uint8_t *w = d->symPat + 3 * sym; uint32_t s1 = (uint32_t)((w[1] & 0xf) << 8) | w[0];
        if (lit < (int)d->symLen[s1] + 1) sym = s1; else { lit -= (int)d->symLen[s1] + 1; sym = (uint32_t)(w[2] << 4) | (w[1] >> 4); } }
    return d->symPat + 3 * sym;
}

/* ---- the tables: a material's name (KQvKR) and key, its files mapped when a probe needs them */
static uint64_t material_key(const int w[7], const int b[7]){
    uint64_t k = 0; for (int p = PAWN; p < KING; p++) k = k << 4 | (uint64_t)(w[p] & 15);
    for (int p = PAWN; p < KING; p++) k = k << 4 | (uint64_t)(b[p] & 15);
    return k;
}
static int file_type(const uint8_t *data, size_t n, Table *t, int type){
    const char *path = t->path[type];
    if ((n & 63) != 16 || le32(data) != MAGIC[type]) { fprintf(stderr, "%s: no Syzygy %s table (or an incomplete one)\n", path, type ? "DTZ" : "WDL"); return 0; }
    int split = type != TB_DTZ && (data[4] & 0x01), num = t->pawned ? 4 : 1; const uint8_t *at = data + 5;
    Enc *ei = type == TB_WDL ? t->wdl : t->dtz; size_t tb_size[4][2];
    for (int f = 0; f < num; f++) {
        tb_size[f][0] = enc_init(&ei[f], t, at, 0, f);
        if (split) tb_size[f][1] = enc_init(&ei[num + f], t, at, 4, f);
        at += t->num + 1 + (t->pawned && t->pawns[1]); }
    at += (uintptr_t)at & 1;
    size_t size[4][2][3];
    for (int f = 0; f < num; f++) { uint8_t flags;
        if (!(ei[f].precomp = pairs_setup(&at, tb_size[f][0], size[f][0], &flags, type))) return 0;
        if (type == TB_DTZ) t->dtzFlags[f] = flags;
        if (split && !(ei[num + f].precomp = pairs_setup(&at, tb_size[f][1], size[f][1], &flags, type))) return 0; }
    if (type == TB_DTZ) {
        t->dtzMap = at;
        for (int f = 0; f < num; f++) if (t->dtzFlags[f] & 2) {
            if (!(t->dtzFlags[f] & 16)) for (int i = 0; i < 4; i++) { t->dtzMapIdx[f][i] = (uint16_t)(at + 1 - t->dtzMap); at += 1 + at[0]; }
            else { at += (uintptr_t)at & 1; for (int i = 0; i < 4; i++) { t->dtzMapIdx[f][i] = (uint16_t)((at + 2 - t->dtzMap) / 2); at += 2 + 2 * le16(at); } } }
        at += (uintptr_t)at & 1; }
    for (int f = 0; f < num; f++) { ei[f].precomp->indexTable = at; at += size[f][0][0]; if (split) { ei[num + f].precomp->indexTable = at; at += size[f][1][0]; } }
    for (int f = 0; f < num; f++) { ei[f].precomp->sizeTable = at; at += size[f][0][1]; if (split) { ei[num + f].precomp->sizeTable = at; at += size[f][1][1]; } }
    for (int f = 0; f < num; f++) {
        at = (const uint8_t *)(((uintptr_t)at + 0x3f) & ~(uintptr_t)0x3f); ei[f].precomp->data = at; at += size[f][0][2];
        if (split) { at = (const uint8_t *)(((uintptr_t)at + 0x3f) & ~(uintptr_t)0x3f); ei[num + f].precomp->data = at; at += size[f][1][2]; } }
    if (at > data + n) { fprintf(stderr, "%s: the table's sections run past its end\n", path); return 0; }
    return 1;
}
static void pairs_free(Pairs *p){ if (p) { free(p->symLen); free(p->base); free(p); } }
static void table_unmap(LpTablebase *tb, Table *t){
    for (int i = 0; i < 8; i++) { pairs_free(t->wdl[i].precomp); t->wdl[i].precomp = NULL; }
    for (int i = 0; i < 4; i++) { pairs_free(t->dtz[i].precomp); t->dtz[i].precomp = NULL; }
    for (int k = 0; k < 2; k++) if (t->map[k]) { os_unmap(t->map[k], t->size[k]); t->map[k] = NULL; }
    if (t->state == 1) { tb->open--; tb->unmaps++; }
    t->state = 0; t->dtzMap = NULL;
}
/* Its files mapped and their headers read (under tb->mu). 1 mapped; 0 not a table. */
static int table_map(LpTablebase *tb, Table *t){
    for (int k = 0; k < 2; k++) {
        if (k == TB_DTZ && !t->has_dtz) continue;
        t->map[k] = os_map_read(t->path[k], &t->size[k]);
        if (!t->map[k] || ((uintptr_t)t->map[k] & 63) || !file_type(t->map[k], t->size[k], t, k)) { if (!t->map[k]) fprintf(stderr, "%s: cannot be mapped\n", t->path[k]); table_unmap(tb, t); return 0; } }
    t->state = 1; tb->open++; tb->maps++; if (tb->open > tb->peak) tb->peak = tb->open; return 1;
}
/* The least recently used tables no probe holds, unmapped while more than room are mapped. */
static void cache_bound(LpTablebase *tb, int room){
    while (tb->open > room) {
        Table *lru = NULL;
        for (int i = 0; i < tb->n; i++) { Table *t = tb->t[i]; if (t->state == 1 && !t->uses && (!lru || t->last < lru->last)) lru = t; }
        if (!lru) return;                                                     /* every one is in use: bounded again on release */
        table_unmap(tb, lru); }
}
/* The table of a material as either side holds it, mapped and held for the probe: flip 1 where the material is the
 * table's with the colours swapped. NULL: the tables have none (or it cannot be read). */
static Table *table_acquire(LpTablebase *tb, const int w[7], const int b[7], int *flip){
    uint64_t a = material_key(w, b), z = material_key(b, w); Table *t = NULL;
    pthread_mutex_lock(&tb->mu);
    for (int i = 0; i < tb->n && !t; i++) { if (tb->t[i]->key == a) { t = tb->t[i]; *flip = 0; } else if (tb->t[i]->key == z) { t = tb->t[i]; *flip = 1; } }
    if (t && t->state == 0) { cache_bound(tb, tb->max_open - 1); if (!table_map(tb, t)) t->state = -1; }     /* room made first */
    if (t && t->state != 1) t = NULL;
    if (t) { t->uses++; t->last = ++tb->tick; }
    pthread_mutex_unlock(&tb->mu); return t;
}
static void table_release(LpTablebase *tb, Table *t){
    pthread_mutex_lock(&tb->mu); t->uses--; if (!t->uses) cache_bound(tb, tb->max_open); pthread_mutex_unlock(&tb->mu);
}
/* A table file found under the root: its material from its name (two kings, at most TB_PIECES pieces). */
static void table_add(LpTablebase *tb, const char *wdl_path){
    const char *slash = strrchr(wdl_path, '/'), *back = strrchr(wdl_path, '\\'); if (back > slash) slash = back;
    const char *base = slash ? slash + 1 : wdl_path, *dot = strrchr(base, '.');
    if (!dot || strcmp(dot, SUFFIX[TB_WDL]) || (size_t)(dot - base) >= 16) return;
    int w[7] = { 0 }, k[7] = { 0 }, *x = w, n = 0; static const char L[] = " PNBRQK";
    for (const char *c = base; c < dot; c++) { if (*c == 'v') { if (x == k) { n = -1; break; } x = k; continue; } const char *p = strchr(L + 1, *c); if (!p || !*c) { n = -1; break; } x[p - L]++; n++; }
    if (n < 3 || n > TB_PIECES || w[KING] != 1 || k[KING] != 1) return;
    uint64_t key = material_key(w, k); for (int i = 0; i < tb->n; i++) if (tb->t[i]->key == key) return;     /* the same material in two directories: the first */
    Table *t = calloc(1, sizeof *t); t->key = key; snprintf(t->name, sizeof t->name, "%.*s", (int)(dot - base), base); t->num = n;
    snprintf(t->path[TB_WDL], sizeof t->path[0], "%s", wdl_path); snprintf(t->path[TB_DTZ], sizeof t->path[1], "%.*s%s", (int)(dot - wdl_path), wdl_path, SUFFIX[TB_DTZ]);
    FILE *dtz = fopen(t->path[TB_DTZ], "rb"); if (dtz) { t->has_dtz = 1; fclose(dtz); }
    int same = 1; for (int p = PAWN; p <= KING; p++) if (w[p] != k[p]) same = 0; t->symmetric = same;
    t->pawned = w[PAWN] || k[PAWN];
    if (!t->pawned) { int singles = 0; for (int p = PAWN; p <= KING; p++) singles += (w[p] == 1) + (k[p] == 1); t->kk_enc = singles == 2; }
    else { t->pawns[0] = (uint8_t)w[PAWN]; t->pawns[1] = (uint8_t)k[PAWN]; if (k[PAWN] && (!w[PAWN] || w[PAWN] > k[PAWN])) { uint8_t q = t->pawns[0]; t->pawns[0] = t->pawns[1]; t->pawns[1] = q; } }
    if (tb->n == tb->cap) { tb->cap = tb->cap ? tb->cap * 2 : 256; tb->t = realloc(tb->t, sizeof(Table *) * (size_t)tb->cap); }
    tb->t[tb->n++] = t;
}
const char *lp_tablebase_root(char *out, size_t cap){
    const char *e = getenv("LAPLACE_SYZYGY"); if (e && *e) { snprintf(out, cap, "%s", e); return out; }
    const char *d = getenv("LAPLACE_DATA"); snprintf(out, cap, "%s/Games/Chess/syzygy", d && *d ? d : "."); return out;
}
LpTablebase *lp_tablebase_open(const char *root, int max_open){
    pthread_mutex_lock(&once_mu); if (!indices_ready) { indices_init(); indices_ready = 1; } pthread_mutex_unlock(&once_mu);
    char r[1024]; if (!root) root = lp_tablebase_root(r, sizeof r);
    LpTablebase *tb = calloc(1, sizeof *tb); tb->max_open = max_open > 0 ? max_open : 64; pthread_mutex_init(&tb->mu, NULL);
    for (int depth = 0; depth < 2; depth++) {
        char pattern[1100]; snprintf(pattern, sizeof pattern, depth ? "%s/*/*.rtbw" : "%s/*.rtbw", root);
        glob_t g; if (!glob(pattern, 0, NULL, &g)) { for (size_t i = 0; i < g.gl_pathc; i++) table_add(tb, g.gl_pathv[i]); globfree(&g); } }
    if (!tb->n) { fprintf(stderr, "%s: no Syzygy table under it (NAME.rtbw, or DIR/NAME.rtbw)\n", root); lp_tablebase_close(tb); return NULL; }
    return tb;
}
void lp_tablebase_close(LpTablebase *tb){
    if (!tb) return;
    for (int i = 0; i < tb->n; i++) { table_unmap(tb, tb->t[i]); free(tb->t[i]); }
    free(tb->t); pthread_mutex_destroy(&tb->mu); free(tb);
}
static LpTablebase *shared; static int shared_tried; static pthread_mutex_t shared_mu = PTHREAD_MUTEX_INITIALIZER;
LpTablebase *lp_tablebase_shared(void){
    pthread_mutex_lock(&shared_mu); if (!shared_tried) { shared = lp_tablebase_open(NULL, 0); shared_tried = 1; } LpTablebase *tb = shared; pthread_mutex_unlock(&shared_mu); return tb;
}
void lp_tablebase_stats(LpTablebase *tb, LpTablebaseStats *s){
    pthread_mutex_lock(&tb->mu); s->tables = tb->n; s->pieces = 0; for (int i = 0; i < tb->n; i++) if (tb->t[i]->num > s->pieces) s->pieces = tb->t[i]->num; s->mapped = tb->open; s->peak = tb->peak; s->max_open = tb->max_open; s->maps = tb->maps; s->unmaps = tb->unmaps; pthread_mutex_unlock(&tb->mu);
}

/* ---- a probe: the table's value of a position (Fathom's probe_table), over chess_rules' board */
static size_t encode(int *p, const Enc *ei, const Table *t, int pawned){
    int n = t->num, k; size_t idx;
    #define SWAP(a, b) do { int q_ = a; a = b; b = q_; } while (0)
    if (p[0] & 0x04) for (int i = 0; i < n; i++) p[i] ^= 0x07;
    if (!pawned) {
        if (p[0] & 0x20) for (int i = 0; i < n; i++) p[i] ^= 0x38;
        for (int i = 0; i < n; i++) if (OffDiag[p[i]]) { if (OffDiag[p[i]] > 0 && i < (t->kk_enc ? 2 : 3)) for (int j = 0; j < n; j++) p[j] = FlipDiag[p[j]]; break; }
        if (t->kk_enc) { idx = (size_t)KKIdx[Triangle[p[0]]][p[1]]; k = 2; }
        else { int s1 = p[1] > p[0], s2 = (p[2] > p[0]) + (p[2] > p[1]);
            if (OffDiag[p[0]]) idx = (size_t)(Triangle[p[0]] * 63 * 62 + (p[1] - s1) * 62 + (p[2] - s2));
            else if (OffDiag[p[1]]) idx = (size_t)(6 * 63 * 62 + Diag[p[0]] * 28 * 62 + Lower[p[1]] * 62 + p[2] - s2);
            else if (OffDiag[p[2]]) idx = (size_t)(6 * 63 * 62 + 4 * 28 * 62 + Diag[p[0]] * 7 * 28 + (Diag[p[1]] - s1) * 28 + Lower[p[2]]);
            else idx = (size_t)(6 * 63 * 62 + 4 * 28 * 62 + 4 * 7 * 28 + Diag[p[0]] * 7 * 6 + (Diag[p[1]] - s1) * 6 + (Diag[p[2]] - s2));
            k = 3; }
        idx *= ei->factor[0];
    } else {
        for (int i = 1; i < t->pawns[0]; i++) for (int j = i + 1; j < t->pawns[0]; j++) if (PawnTwist[p[i]] < PawnTwist[p[j]]) SWAP(p[i], p[j]);
        k = t->pawns[0]; idx = PawnIdx[k - 1][Flap[p[0]]];
        for (int i = 1; i < k; i++) idx += Binomial[k - i][PawnTwist[p[i]]];
        idx *= ei->factor[0];
        if (t->pawns[1]) { int e = k + t->pawns[1];
            for (int i = k; i < e; i++) for (int j = i + 1; j < e; j++) if (p[i] > p[j]) SWAP(p[i], p[j]);
            size_t s = 0; for (int i = k; i < e; i++) { int sq = p[i], skips = 0; for (int j = 0; j < k; j++) skips += sq > p[j]; s += Binomial[i - k + 1][sq - skips - 8]; }
            idx += s * ei->factor[k]; k = e; } }
    while (k < n) { int e = k + ei->norm[k];
        for (int i = k; i < e; i++) for (int j = i + 1; j < e; j++) if (p[i] > p[j]) SWAP(p[i], p[j]);
        size_t s = 0; for (int i = k; i < e; i++) { int sq = p[i], skips = 0; for (int j = 0; j < k; j++) skips += sq > p[j]; s += Binomial[i - k + 1][sq - skips]; }
        idx += s * ei->factor[k]; k = e; }
    #undef SWAP
    return idx;
}
/* The squares of the table's pieces from i on of piece code pc[i] (1-6 White's pawn to king, 9-14 Black's), the
 * colours swapped where flip, each square XOR mirror; pieces of one code are consecutive. */
static int fill(const Bd *b, const uint8_t *pc, int flip, int mirror, int *p, int i){
    int white = !(pc[i] >> 3); if (flip) white = !white; int8_t want = (int8_t)((pc[i] & 7) * (white ? 1 : -1));
    for (int s = 0; s < 64; s++) if (b->sq[s] == want) p[i++] = s ^ mirror;
    return i;
}
static int material_of(const Bd *b, int w[7], int k[7]){
    memset(w, 0, sizeof(int) * 7); memset(k, 0, sizeof(int) * 7); int n = 0;
    for (int s = 0; s < 64; s++) { int8_t p = b->sq[s]; if (p > 0) w[p]++; else if (p < 0) k[-p]++; n += p != 0; }
    return n;
}
static int table_value(const Table *t, const Bd *b, int flip, int s, int *success, int type){
    if (type == TB_DTZ && !t->map[TB_DTZ]) { *success = 0; return 0; }
    int bside; if (!t->symmetric) bside = (b->side == 0) == flip; else { flip = b->side != 0; bside = 0; }
    int p[TB_PIECES]; size_t idx; int f = 0; uint8_t flags = 0; const Enc *ei;
    if (!t->pawned) {
        if (type == TB_DTZ) { flags = t->dtzFlags[0]; if ((flags & 1) != bside && !t->symmetric) { *success = -1; return 0; } ei = &t->dtz[0]; }
        else ei = &t->wdl[bside];
        for (int i = 0; i < t->num;) i = fill(b, ei->pieces, flip, 0, p, i);
        idx = encode(p, ei, t, 0);
    } else {
        const Enc *first = type == TB_WDL ? t->wdl : t->dtz;
        int i = fill(b, first->pieces, flip, flip ? 0x38 : 0, p, 0);
        for (int j = 1; j < t->pawns[0]; j++) if (Flap[p[0]] > Flap[p[j]]) { int q = p[0]; p[0] = p[j]; p[j] = q; }
        f = FileToFile[p[0] & 7];
        if (type == TB_DTZ) { flags = t->dtzFlags[f]; if ((flags & 1) != bside && !t->symmetric) { *success = -1; return 0; } }
        ei = type == TB_WDL ? &t->wdl[f + 4 * bside] : &t->dtz[f];
        while (i < t->num) i = fill(b, ei->pieces, flip, flip ? 0x38 : 0, p, i);
        idx = encode(p, ei, t, 1);
    }
    if (!ei->precomp) { *success = 0; return 0; }
    const uint8_t *v8 = pairs_value(ei->precomp, idx);
    if (type == TB_WDL) return (int)v8[0] - 2;
    int v = v8[0] + ((v8[1] & 0x0f) << 8);
    if (flags & 2) { int m = WdlToMap[s + 2];
        if (!(flags & 16)) v = t->dtzMap[t->dtzMapIdx[f][m] + v];
        else v = le16(t->dtzMap + 2 * ((size_t)t->dtzMapIdx[f][m] + (size_t)v)); }
    if (!(flags & PAFlags[s + 2]) || (s & 1)) v *= 2;
    return v;
}
/* type TB_WDL: -2..2; TB_DTZ: the stored distance for the wdl s. success: 0 failed, -1 the DTZ table holds the other
 * side to move. A position with castling rights is in no table. */
static int probe_table(LpTablebase *tb, const Bd *b, int s, int *success, int type){
    int w[7], k[7]; if (material_of(b, w, k) > TB_PIECES || b->rights[0] || b->rights[1]) { *success = 0; return 0; }
    int only_kings = 1; for (int p = PAWN; p < KING; p++) if (w[p] || k[p]) only_kings = 0;
    if (type == TB_WDL && only_kings) return 0;
    int flip; Table *t = table_acquire(tb, w, k, &flip); if (!t) { *success = 0; return 0; }
    int v = table_value(t, b, flip, s, success, type);
    table_release(tb, t); return v;
}

/* ---- the verdicts (Fathom's probe_ab, probe_wdl, probe_dtz), every move chess_rules' */
static int is_capture(const Bd *b, const Mv *m){ return !m->castle && (b->sq[m->to] || m->ep); }
static int is_zeroing(const Bd *b, const Mv *m){ return is_capture(b, m) || (!m->castle && abs(b->sq[m->from]) == PAWN); }
static int probe_ab(LpTablebase *tb, const Bd *b, int alpha, int beta, int *success){
    Mv all[256]; int n = gen(b, all);
    for (int i = 0; i < n; i++) { if (!is_capture(b, &all[i])) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_ab(tb, &t, -beta, -alpha, success);
        if (!*success) return 0;
        if (v > alpha) { if (v >= beta) return v; alpha = v; } }
    int v = probe_table(tb, b, 0, success, TB_WDL);
    return alpha >= v ? alpha : v;
}
/* -2 loss, -1 blessed loss, 0 draw, 1 cursed win, 2 win, for the side to move. success 2: a capture (or an en passant
 * capture alone) is the best move, which probe_dtz needs. */
static int probe_wdl(LpTablebase *tb, const Bd *b, int *success){
    *success = 1; Mv all[256]; int n = gen(b, all), bestCap = -3, bestEp = -3;
    for (int i = 0; i < n; i++) { if (!is_capture(b, &all[i])) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_ab(tb, &t, -2, -bestCap, success);
        if (!*success) return 0;
        if (v > bestCap) { if (v == 2) { *success = 2; return 2; } if (!all[i].ep) bestCap = v; else if (v > bestEp) bestEp = v; } }
    int v = probe_table(tb, b, 0, success, TB_WDL); if (!*success) return 0;
    if (bestEp > bestCap) { if (bestEp > v) { *success = 2; return bestEp; } bestCap = bestEp; }
    if (bestCap >= v) { *success = 1 + (bestCap > 0); return bestCap; }
    if (bestEp > -3 && v == 0) {                                             /* stalemate but for an en passant capture */
        int other = 0; for (int i = 0; i < n; i++) if (!all[i].ep) { other = 1; break; }
        if (!other && !in_check(b, b->side)) { *success = 2; return bestEp; } }
    return v;
}
static const int WdlToDtz[5] = { -1, -101, 0, 101, 1 };
static int is_mate(const Bd *b){ Mv all[256]; return in_check(b, b->side) && !gen(b, all); }
static int probe_dtz(LpTablebase *tb, const Bd *b, int *success){
    int wdl = probe_wdl(tb, b, success); if (!*success) return 0;
    if (!wdl) return 0;
    if (*success == 2) return WdlToDtz[wdl + 2];
    Mv all[256]; int n = gen(b, all);
    if (wdl > 0) for (int i = 0; i < n; i++) {                                /* a winning pawn move */
        if (abs(b->sq[all[i].from]) != PAWN || is_capture(b, &all[i])) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_wdl(tb, &t, success); if (!*success) return 0;
        if (v == wdl) return WdlToDtz[wdl + 2]; }
    int dtz = probe_table(tb, b, wdl, success, TB_DTZ);
    if (*success >= 0) return WdlToDtz[wdl + 2] + (wdl > 0 ? dtz : -dtz);
    *success = 1; int best = wdl > 0 ? INT32_MAX : WdlToDtz[wdl + 2];      /* the table holds the other side to move */
    for (int i = 0; i < n; i++) {
        if (is_capture(b, &all[i]) || abs(b->sq[all[i].from]) == PAWN) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_dtz(tb, &t, success); if (!*success) return 0;
        if (v == 1 && is_mate(&t)) best = 1;
        else if (wdl > 0) { if (v > 0 && v + 1 < best) best = v + 1; }
        else if (v - 1 < best) best = v - 1; }
    return best;
}
int lp_tablebase_wdl(LpTablebase *tb, const Bd *b, int *wdl){ int success; int v = probe_wdl(tb, b, &success); if (!success) return 0; *wdl = v; return 1; }
int lp_tablebase_dtz(LpTablebase *tb, const Bd *b, int *wdl, int *dtz){
    int success; int w = probe_wdl(tb, b, &success); if (!success) return 0;
    int d = probe_dtz(tb, b, &success); if (!success) return 0;
    *wdl = w; *dtz = d; return 1;
}

/* ---- the moves ranked, and the line (Fathom's root probe: the dtz of each move counted from the root) */
static int ranked_order(const LpTbRanked *x, int xi, const LpTbRanked *y, int yi){
    if (x->wdl != y->wdl) return y->wdl - x->wdl;                             /* the mover's result: a win first */
    if (x->wdl && x->value != y->value) return x->value < y->value ? -1 : 1; /* a win's soonest zeroing; a loss's latest (most negative) */
    int ax = abs(x->dtz), ay = abs(y->dtz);                                   /* then the position that zeroes again soonest (a win), latest (a loss) */
    if (x->wdl > 0 && ax != ay) return ax < ay ? -1 : 1;
    if (x->wdl < 0 && ax != ay) return ax > ay ? -1 : 1;
    return xi - yi;
}
int lp_tablebase_rank(LpTablebase *tb, const Bd *b, LpTbRanked *out, int cap){
    Mv all[256]; int n = gen(b, all); if (n > cap) n = cap; int success;
    for (int i = 0; i < n; i++) { LpTbRanked *r = &out[i]; memset(r, 0, sizeof *r); r->move = all[i];
        Bd t = *b; make(&t, &all[i]); r->zeroing = (uint8_t)is_zeroing(b, &all[i]); r->mate = (uint8_t)is_mate(&t);
        int w = probe_wdl(tb, &t, &success); if (!success) return -1;
        int d = probe_dtz(tb, &t, &success); if (!success) return -1;
        r->wdl = -w; r->dtz = -d;
        int v;
        if (r->zeroing) v = WdlToDtz[r->wdl + 2];
        else { v = -d; if (v > 0) v++; else if (v < 0) v--; }
        if (v == 2 && r->mate) v = 1;
        r->value = v; }
    for (int i = 1; i < n; i++) { LpTbRanked x = out[i]; int j = i;            /* stable: equal moves keep gen's order */
        while (j && ranked_order(&out[j - 1], 0, &x, 1) > 0) { out[j] = out[j - 1]; j--; }
        out[j] = x; }
    return n;
}
int lp_tablebase_line(LpTablebase *tb, const Bd *b, Mv *moves, int cap, char result[8]){
    int wdl; if (!lp_tablebase_wdl(tb, b, &wdl)) return -1;
    if (!wdl) return 0;
    snprintf(result, 8, "%s", wdl == 2 ? (b->side ? "0-1" : "1-0") : wdl == -2 ? (b->side ? "1-0" : "0-1") : "1/2-1/2");
    Bd cur = *b; int n = 0; LpTbRanked r[256];
    for (;;) {
        int w[7], k[7], pieces = material_of(&cur, w, k); if (pieces <= 2) break;            /* the kings alone: drawn */
        int nm = lp_tablebase_rank(tb, &cur, r, 256); if (nm < 0) return -1;
        if (!nm) break;                                                                       /* mate (or stalemate) */
        if (!r[0].wdl) break;                                                                 /* a draw by the tables: the line ends */
        if (n == cap) return -1;
        moves[n++] = r[0].move; make(&cur, &r[0].move); }
    return n;
}
size_t lp_tablebase_pgn(const Bd *b, const Mv *moves, int plies, const char *result, char *out){
    char *o = out; o += sprintf(o, "[FEN \""); o += lp_chess_fen_of(b, o); o += sprintf(o, "\"]\n\n");
    Bd cur = *b; int full = 1;
    for (int i = 0; i < plies; i++) {
        Mv all[256]; int na = gen(&cur, all); char san[16]; san_of(&cur, &moves[i], all, na, 1, san);
        if (!cur.side) o += sprintf(o, "%s%d. ", i ? " " : "", full); else if (!i) o += sprintf(o, "%d... ", full); else *o++ = ' ';
        o += sprintf(o, "%s", san);
        if (cur.side) full++; make(&cur, &moves[i]); }
    o += sprintf(o, "%s%s\n\n", plies ? " " : "", result);
    return (size_t)(o - out);
}

/* ---- for readers: a FEN */
static void uci_of(const Mv *m, char *out){
    int to = m->castle ? ((m->to & 7) > (m->from & 7) ? (m->from & ~7) + 6 : (m->from & ~7) + 2) : m->to;     /* a castling: the king's own two steps */
    char *o = out; *o++ = (char)('a' + (m->from & 7)); *o++ = (char)('1' + (m->from >> 3)); *o++ = (char)('a' + (to & 7)); *o++ = (char)('1' + (to >> 3));
    if (m->promo) *o++ = ".pnbrqk"[m->promo]; *o = 0;
}
/* A move from b: its SAN, UCI, its ID, the position it makes and the [P, M, P'] claim's ID, as chess.c composes them. */
static void move_ids(const Bd *b, const lp_id *P, const Mv *m, const Mv *all, int na, char san[16], char uci[8], lp_id *mid, lp_id *pos, lp_id *claim){
    san_of(b, m, all, na, 1, san); uci_of(m, uci); id_move(b, m, mid);
    Bd t = *b; make(&t, m); id_position(&t, pos);
    lp_id three[3] = { *P, *mid, *pos }; lp_id_compose(three, 3, claim);
}
static int fen_board(const char *fen, size_t n, Bd *b){ return fen && board_fen(b, fen, n, 0) && king_sq(b, 0) >= 0 && king_sq(b, 1) >= 0; }
int lp_tablebase_probe_fen(LpTablebase *tb, const char *fen, size_t n, LpTbProbe *out){
    Bd b; memset(out, 0, sizeof *out); if (!fen_board(fen, n, &b)) return -1;
    id_position(&b, &out->position);
    Mv all[256]; int na = gen(&b, all); if (na > LP_TB_MOVES) na = LP_TB_MOVES;
    LpTbRanked r[256]; int nr = tb ? lp_tablebase_rank(tb, &b, r, 256) : -1, covered = nr >= 0 && lp_tablebase_dtz(tb, &b, &out->wdl, &out->dtz);
    out->covered = covered; out->nmoves = na;
    for (int i = 0; i < na; i++) { LpTbMove *m = &out->move[i]; const Mv *mv = covered ? &r[i].move : &all[i];
        move_ids(&b, &out->position, mv, all, na, m->san, m->uci, &m->move, &m->position, &m->claim);
        if (covered) { m->wdl = r[i].wdl; m->dtz = r[i].dtz; m->value = r[i].value; m->zeroing = r[i].zeroing; m->mate = r[i].mate; } }
    return covered;
}
int lp_tablebase_line_fen(LpTablebase *tb, const char *fen, size_t n, LpTbPly *out, int cap, lp_id *line, char result[8]){
    Bd b; if (!tb || !fen_board(fen, n, &b)) return -1;
    Mv *mv = malloc(sizeof(Mv) * (size_t)(cap > 0 ? cap : 1)); int plies = lp_tablebase_line(tb, &b, mv, cap, result);
    if (plies < 0) { free(mv); return -1; }
    lp_id *traj = malloc(sizeof(lp_id) * (size_t)(2 * plies + 1)); size_t nt = 0; id_position(&b, &traj[nt++]);
    Bd cur = b;
    for (int i = 0; i < plies; i++) { Mv all[256]; int na = gen(&cur, all); LpTbPly *p = &out[i];
        move_ids(&cur, &traj[nt - 1], &mv[i], all, na, p->san, p->uci, &p->move, &p->position, &p->claim);
        traj[nt++] = p->move; traj[nt++] = p->position; make(&cur, &mv[i]); }
    if (line) { if (nt == 1) *line = traj[0]; else lp_id_compose(traj, nt, line); }
    if (!plies) result[0] = 0;
    free(traj); free(mv); return plies;
}
