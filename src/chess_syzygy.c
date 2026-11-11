/* Syzygy endgame tablebases (Ronald de Man's format, syzygy-tables.info), decoded for a recipe (input decode syzygy).
 *
 * A table, KQvK.rtbw or KQvK.rtbz, holds a verdict for every position of its material, White's or Black's to move, the
 * material either way round (KQvK is also KvKQ): its WDL file the win/draw/loss under the 50-move rule (a cursed win
 * is a win the 50-move rule draws, a blessed loss a loss it saves), its DTZ file the distance to zeroing, the plies to
 * the next capture or pawn move under best play. Neither is a list of positions: each is an index over the material's
 * placements, folded by the board's symmetries and compressed, its values right only where no capture decides them,
 * and the DTZ file holds one side to move. So the verdict of a position is a probe: the captures searched (the tables
 * of the material they leave, found beside the file), then the table. That is the format's own law, as Fathom
 * (Ronald de Man, basil00, Jon Dart; MIT) implements it: its index encoding and pair decompression are carried over
 * here; every move, check and position is chess_rules.c's (the one chess this Engine has).
 *
 * What the decoder writes is the table's content: a line for every legal position of the material, in the order the
 * table's pieces are placed, its FEN (chess_rules' lp_chess_fen_of, so a FEN read back is the same position) and the
 * verdict, tab-separated:
 *   .rtbw   FEN \t win | cursed win | draw | blessed loss | loss         every position
 *   .rtbz   FEN \t the distance to zeroing, signed for the side to move   every position that is not a draw (a drawn
 *                                                                        position has no zeroing to reach)
 * as Fathom's probe_wdl and probe_dtz give it: DTZ in plies, over 100 where the 50-move rule turns the result, and
 * possibly one more than the true distance where the table rounds. The recipe (recipes/syzygy) says what each part is:
 * the FEN the position (grammar fen), the verdict a claim of it. No castling rights are in any table's position; an en
 * passant square is, where a pawn of the side to move can take on it. The tables a probe reaches are read whole into
 * memory when the file is opened (a 3-to-5-piece table's captures reach the smaller tables of the same set). */
#include "engine.h"
#include "chess_rules.h"
#include <omp.h>
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
typedef struct {
    char name[16]; int num, symmetric, pawned, kk_enc; uint8_t pawns[2];
    uint8_t *bytes[2], *mem[2];                       /* each file's bytes (64-aligned, as the format's offsets assume) */
    Enc wdl[8], dtz[4];                               /* pieces: wdl[bside], dtz[0]; pawns: wdl[file + 4 bside], dtz[file] */
    const uint8_t *dtzMap; uint16_t dtzMapIdx[4][4]; uint8_t dtzFlags[4];
} Table;
typedef struct { Table *t; int n, cap; char dir[1024]; } Set;

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

/* ---- the tables of a set: a material's name (KQvKR), and its files read whole */
static void material_name(const int w[7], const int b[7], char *out){
    static const char L[] = " PNBRQK"; char *o = out;
    for (int p = KING; p >= PAWN; p--) for (int i = 0; i < w[p]; i++) *o++ = L[p];
    *o++ = 'v'; for (int p = KING; p >= PAWN; p--) for (int i = 0; i < b[p]; i++) *o++ = L[p];
    *o = 0;
}
static uint8_t *file_read(const char *path, uint8_t **mem, size_t *n){
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); rewind(f); if (l <= 0) { fclose(f); return NULL; }
    *mem = malloc((size_t)l + 64); uint8_t *b = (uint8_t *)(((uintptr_t)*mem + 63) & ~(uintptr_t)63);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(*mem); *mem = NULL; return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static int table_file(Table *t, const char *dir, int type){
    char path[1100]; snprintf(path, sizeof path, "%s/%s%s", dir, t->name, SUFFIX[type]); size_t n;
    uint8_t *data = file_read(path, &t->mem[type], &n); if (!data) return 0;
    if ((n & 63) != 16 || le32(data) != MAGIC[type]) { fprintf(stderr, "%s: no Syzygy %s table (or an incomplete one)\n", path, type ? "DTZ" : "WDL"); return -1; }
    t->bytes[type] = data;
    int split = type != TB_DTZ && (data[4] & 0x01), num = t->pawned ? 4 : 1; data += 5;
    Enc *ei = type == TB_WDL ? t->wdl : t->dtz; size_t tb_size[4][2];
    for (int f = 0; f < num; f++) {
        tb_size[f][0] = enc_init(&ei[f], t, data, 0, f);
        if (split) tb_size[f][1] = enc_init(&ei[num + f], t, data, 4, f);
        data += t->num + 1 + (t->pawned && t->pawns[1]); }
    data += (uintptr_t)data & 1;
    size_t size[4][2][3];
    for (int f = 0; f < num; f++) { uint8_t flags;
        if (!(ei[f].precomp = pairs_setup((const uint8_t **)&data, tb_size[f][0], size[f][0], &flags, type))) return -1;
        if (type == TB_DTZ) t->dtzFlags[f] = flags;
        if (split && !(ei[num + f].precomp = pairs_setup((const uint8_t **)&data, tb_size[f][1], size[f][1], &flags, type))) return -1; }
    if (type == TB_DTZ) {
        t->dtzMap = data;
        for (int f = 0; f < num; f++) if (t->dtzFlags[f] & 2) {
            if (!(t->dtzFlags[f] & 16)) for (int i = 0; i < 4; i++) { t->dtzMapIdx[f][i] = (uint16_t)(data + 1 - t->dtzMap); data += 1 + data[0]; }
            else { data += (uintptr_t)data & 1; for (int i = 0; i < 4; i++) { t->dtzMapIdx[f][i] = (uint16_t)((data + 2 - t->dtzMap) / 2); data += 2 + 2 * le16(data); } } }
        data += (uintptr_t)data & 1; }
    for (int f = 0; f < num; f++) { ei[f].precomp->indexTable = data; data += size[f][0][0]; if (split) { ei[num + f].precomp->indexTable = data; data += size[f][1][0]; } }
    for (int f = 0; f < num; f++) { ei[f].precomp->sizeTable = data; data += size[f][0][1]; if (split) { ei[num + f].precomp->sizeTable = data; data += size[f][1][1]; } }
    for (int f = 0; f < num; f++) {
        data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~(uintptr_t)0x3f); ei[f].precomp->data = data; data += size[f][0][2];
        if (split) { data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~(uintptr_t)0x3f); ei[num + f].precomp->data = data; data += size[f][1][2]; } }
    if (data > t->bytes[type] + n) { fprintf(stderr, "%s: the table's sections run past its end\n", path); return -1; }
    return 1;
}
static void pairs_free(Pairs *p){ if (p) { free(p->symLen); free(p->base); free(p); } }
static void table_free(Table *t){
    for (int i = 0; i < 8; i++) pairs_free(t->wdl[i].precomp);
    for (int i = 0; i < 4; i++) pairs_free(t->dtz[i].precomp);
    free(t->mem[0]); free(t->mem[1]);
}
/* The table of a material, as either side holds it: its index in the set, and whether the material is the table's
 * with the colours swapped. -1: the set has none (a material of the kings alone has none, and needs none). */
static int set_find(const Set *s, const int w[7], const int b[7], int *flip){
    char a[16], z[16]; material_name(w, b, a); material_name(b, w, z);
    for (int i = 0; i < s->n; i++) { if (!strcmp(s->t[i].name, a)) { *flip = 0; return i; } if (!strcmp(s->t[i].name, z)) { *flip = 1; return i; } }
    return -1;
}
/* A material's table loaded from the set's directory, under whichever of its two names the file has: 1 loaded (or
 * there already), 0 the directory has none, -1 its file is not a table. */
static int set_load(Set *s, const int w[7], const int b[7]){
    int flip; if (set_find(s, w, b, &flip) >= 0) return 1;
    for (int side = 0; side < 2; side++) {
        const int *x = side ? b : w, *y = side ? w : b; Table t; memset(&t, 0, sizeof t); material_name(x, y, t.name);
        char path[1100]; snprintf(path, sizeof path, "%s/%s%s", s->dir, t.name, SUFFIX[TB_WDL]); FILE *probe = fopen(path, "rb"); if (!probe) continue; fclose(probe);
        for (int p = PAWN; p <= KING; p++) t.num += x[p] + y[p];
        int same = 1; for (int p = PAWN; p <= KING; p++) if (x[p] != y[p]) same = 0; t.symmetric = same;
        t.pawned = x[PAWN] || y[PAWN];
        if (!t.pawned) { int singles = 0; for (int p = PAWN; p <= KING; p++) singles += (x[p] == 1) + (y[p] == 1); t.kk_enc = singles == 2; }
        else { t.pawns[0] = (uint8_t)x[PAWN]; t.pawns[1] = (uint8_t)y[PAWN]; if (y[PAWN] && (!x[PAWN] || x[PAWN] > y[PAWN])) { uint8_t q = t.pawns[0]; t.pawns[0] = t.pawns[1]; t.pawns[1] = q; } }
        for (int type = 0; type < 2; type++) { int got = table_file(&t, s->dir, type); if (got < 0 || (!got && type == TB_WDL)) { table_free(&t); return -1; } }
        if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->t = realloc(s->t, sizeof(Table) * (size_t)s->cap); }
        s->t[s->n++] = t; return 1; }
    return 0;
}
/* Every table a probe from this material can reach: the material after a capture of either side's, a promotion, or
 * both, down to the kings alone. A table the set lacks is said; probes that reach it fail. */
static int set_reach(Set *s, const int w[7], const int b[7], int depth){
    int n = 0; for (int p = PAWN; p < KING; p++) n += w[p] + b[p];
    if (!n || depth > 32) return 1;
    int got = set_load(s, w, b); if (got < 0) return 0;
    if (!got) { char name[16]; material_name(w, b, name); fprintf(stderr, "syzygy: %s: no table %s beside it; its positions' probes fail\n", s->dir, name); return 1; }
    for (int side = 0; side < 2; side++) {
        int x[7], y[7]; memcpy(x, side ? b : w, sizeof x); memcpy(y, side ? w : b, sizeof y);
        for (int p = PAWN; p < KING; p++) if (y[p]) { y[p]--; if (!(side ? set_reach(s, y, x, depth + 1) : set_reach(s, x, y, depth + 1))) return 0;     /* x takes a y piece */
            if (x[PAWN]) for (int q = KNIGHT; q <= QUEEN; q++) { x[PAWN]--; x[q]++; if (!(side ? set_reach(s, y, x, depth + 1) : set_reach(s, x, y, depth + 1))) return 0; x[q]--; x[PAWN]++; }
            y[p]++; }
        if (x[PAWN]) for (int q = KNIGHT; q <= QUEEN; q++) { x[PAWN]--; x[q]++; if (!(side ? set_reach(s, y, x, depth + 1) : set_reach(s, x, y, depth + 1))) return 0; x[q]--; x[PAWN]++; } }
    return 1;
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
static void material_of(const Bd *b, int w[7], int k[7]){
    memset(w, 0, sizeof(int) * 7); memset(k, 0, sizeof(int) * 7);
    for (int s = 0; s < 64; s++) { int8_t p = b->sq[s]; if (p > 0) w[p]++; else if (p < 0) k[-p]++; }
}
/* type TB_WDL: -2..2; TB_DTZ: the stored distance for the wdl s. success: 0 failed, -1 the DTZ table holds the other
 * side to move. */
static int probe_table(const Set *set, const Bd *b, int s, int *success, int type){
    int w[7], k[7]; material_of(b, w, k);
    int only_kings = 1; for (int p = PAWN; p < KING; p++) if (w[p] || k[p]) only_kings = 0;
    if (type == TB_WDL && only_kings) return 0;
    int flip, at = set_find(set, w, k, &flip); if (at < 0) { *success = 0; return 0; }
    const Table *t = &set->t[at]; if (type == TB_DTZ && !t->bytes[TB_DTZ]) { *success = 0; return 0; }
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

/* ---- the verdicts (Fathom's probe_ab, probe_wdl, probe_dtz), every move chess_rules' */
static int is_capture(const Bd *b, const Mv *m){ return !m->castle && (b->sq[m->to] || m->ep); }
static int probe_ab(const Set *set, const Bd *b, int alpha, int beta, int *success){
    Mv all[256]; int n = gen(b, all);
    for (int i = 0; i < n; i++) { if (!is_capture(b, &all[i])) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_ab(set, &t, -beta, -alpha, success);
        if (!*success) return 0;
        if (v > alpha) { if (v >= beta) return v; alpha = v; } }
    int v = probe_table(set, b, 0, success, TB_WDL);
    return alpha >= v ? alpha : v;
}
/* -2 loss, -1 blessed loss, 0 draw, 1 cursed win, 2 win, for the side to move. success 2: a capture (or an en passant
 * capture alone) is the best move, which probe_dtz needs. */
static int probe_wdl(const Set *set, const Bd *b, int *success){
    *success = 1; Mv all[256]; int n = gen(b, all), bestCap = -3, bestEp = -3;
    for (int i = 0; i < n; i++) { if (!is_capture(b, &all[i])) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_ab(set, &t, -2, -bestCap, success);
        if (!*success) return 0;
        if (v > bestCap) { if (v == 2) { *success = 2; return 2; } if (!all[i].ep) bestCap = v; else if (v > bestEp) bestEp = v; } }
    int v = probe_table(set, b, 0, success, TB_WDL); if (!*success) return 0;
    if (bestEp > bestCap) { if (bestEp > v) { *success = 2; return bestEp; } bestCap = bestEp; }
    if (bestCap >= v) { *success = 1 + (bestCap > 0); return bestCap; }
    if (bestEp > -3 && v == 0) {                                             /* stalemate but for an en passant capture */
        int other = 0; for (int i = 0; i < n; i++) if (!all[i].ep) { other = 1; break; }
        if (!other && !in_check(b, b->side)) { *success = 2; return bestEp; } }
    return v;
}
static const int WdlToDtz[5] = { -1, -101, 0, 101, 1 };
static int is_mate(const Bd *b){ Mv all[256]; return in_check(b, b->side) && !gen(b, all); }
static int probe_dtz(const Set *set, const Bd *b, int *success){
    int wdl = probe_wdl(set, b, success); if (!*success) return 0;
    if (!wdl) return 0;
    if (*success == 2) return WdlToDtz[wdl + 2];
    Mv all[256]; int n = gen(b, all);
    if (wdl > 0) for (int i = 0; i < n; i++) {                                /* a winning pawn move */
        if (abs(b->sq[all[i].from]) != PAWN || is_capture(b, &all[i])) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_wdl(set, &t, success); if (!*success) return 0;
        if (v == wdl) return WdlToDtz[wdl + 2]; }
    int dtz = probe_table(set, b, wdl, success, TB_DTZ);
    if (*success >= 0) return WdlToDtz[wdl + 2] + (wdl > 0 ? dtz : -dtz);
    *success = 1; int best = wdl > 0 ? INT32_MAX : WdlToDtz[wdl + 2];      /* the table holds the other side to move */
    for (int i = 0; i < n; i++) {
        if (is_capture(b, &all[i]) || abs(b->sq[all[i].from]) == PAWN) continue;
        Bd t = *b; make(&t, &all[i]); int v = -probe_dtz(set, &t, success); if (!*success) return 0;
        if (v == 1 && is_mate(&t)) best = 1;
        else if (wdl > 0) { if (v > 0 && v + 1 < best) best = v + 1; }
        else if (v - 1 < best) best = v - 1; }
    return best;
}

/* ---- the decoder: every legal position of the table's material, a line each */
static const char *const WDL_TEXT[5] = { "loss", "blessed loss", "draw", "cursed win", "win" };
typedef struct {
    Set set; int type, table, failed;
    int8_t piece[2][TB_PIECES]; int orientations, num;                     /* the pieces placed, as each colouring of the material */
    uint64_t chunk, chunks;                                                  /* a chunk: the squares of the first pieces (64^head) */
    int head;
    lp_buf out; size_t at;                                                   /* lines written and not yet read */
    uint64_t positions, written, unprobed;
} Decoder;

/* One chunk's lines: piece i on square sq[i]; same pieces in ascending squares (one placement of identical pieces),
 * a pawn on neither back rank, each side to move where the side not to move is not in check, then each en passant
 * square a pawn of the side to move can take on. */
typedef struct { char *b; size_t n, cap; uint64_t positions, written, unprobed; int failed; } Lines;
static void lines_put(Lines *l, const char *s, size_t n){ if (l->n + n > l->cap) { l->cap = (l->n + n) * 2 + 4096; l->b = realloc(l->b, l->cap); } memcpy(l->b + l->n, s, n); l->n += n; }
static void position_line(const Decoder *d, const Bd *b, Lines *l){
    int success; char line[160]; size_t n = lp_chess_fen_of(b, line); l->positions++;
    if (d->type == TB_WDL) { int v = probe_wdl(&d->set, b, &success); if (!success) { l->unprobed++; return; }
        n += (size_t)snprintf(line + n, sizeof line - n, "\t%s\n", WDL_TEXT[v + 2]); }
    else { int v = probe_dtz(&d->set, b, &success); if (!success) { l->unprobed++; return; } if (!v) return;
        n += (size_t)snprintf(line + n, sizeof line - n, "\t%d\n", v); }
    lines_put(l, line, n); l->written++;
}
static void place(const Decoder *d, const int8_t *pc, Bd *b, int i, Lines *l){
    if (i == d->num) {
        for (int side = 0; side < 2; side++) { b->side = (uint8_t)side; b->ep = -1;
            if (in_check(b, !side)) continue;
            position_line(d, b, l);
            /* the en passant squares: a pawn of the other side that has just moved two squares, the two behind it empty */
            int dir = side ? 1 : -1, row = side ? 3 : 4;                       /* White's pawn on rank 4 when Black moves, Black's on 5 */
            for (int f = 0; f < 8; f++) { int at = row * 8 + f, over = at - 8 * dir, from = at - 16 * dir;
                if (b->sq[at] != (side ? PAWN : -PAWN) || b->sq[over] || b->sq[from]) continue;
                Bd e = *b; e.ep = (int8_t)over; if (ep_square(&e) >= 0) position_line(d, &e, l); } }
        return; }
    for (int s = 0; s < 64; s++) {
        if (b->sq[s]) continue;
        if ((pc[i] == PAWN || pc[i] == -PAWN) && (s < 8 || s >= 56)) continue;
        if (i && pc[i] == pc[i - 1]) { int prev = -1; for (int q = 63; q >= 0; q--) if (b->sq[q] == pc[i]) { prev = q; break; } if (s < prev) continue; }
        b->sq[s] = pc[i]; place(d, pc, b, i + 1, l); b->sq[s] = 0; }
}
static void chunk_lines(const Decoder *d, uint64_t chunk, Lines *l){
    uint64_t per = 1; for (int i = 0; i < d->head; i++) per *= 64;
    int orient = (int)(chunk / per); uint64_t c = chunk % per; const int8_t *pc = d->piece[orient];
    Bd b; memset(&b, 0, sizeof b); b.ep = -1;
    for (int i = d->head - 1; i >= 0; i--) { int s = (int)(c % 64); c /= 64;
        if (b.sq[s]) return; if ((pc[i] == PAWN || pc[i] == -PAWN) && (s < 8 || s >= 56)) return; b.sq[s] = pc[i]; }
    int sq[TB_PIECES]; c = chunk % per; for (int j = d->head - 1; j >= 0; j--) { sq[j] = (int)(c % 64); c /= 64; }
    for (int i = 1; i < d->head; i++) if (pc[i] == pc[i - 1] && sq[i] < sq[i - 1]) return;
    place(d, pc, &b, d->head, l);
}
static int decoder_refill(Decoder *d){
    if (d->chunk >= d->chunks) return 0;
    int threads = omp_get_max_threads(); uint64_t batch = (uint64_t)threads * 64; if (d->num <= 3) batch = d->chunks;
    if (d->chunk + batch > d->chunks) batch = d->chunks - d->chunk;
    Lines *lines = calloc((size_t)batch, sizeof(Lines));
    #pragma omp parallel for schedule(dynamic, 1)
    for (int64_t i = 0; i < (int64_t)batch; i++) chunk_lines(d, d->chunk + (uint64_t)i, &lines[i]);
    d->out.n = 0; d->at = 0;
    for (uint64_t i = 0; i < batch; i++) { uint8_t *o = lp_buf_room(&d->out, lines[i].n); if (lines[i].n) memcpy(o, lines[i].b, lines[i].n);
        d->positions += lines[i].positions; d->written += lines[i].written; d->unprobed += lines[i].unprobed; free(lines[i].b); }
    free(lines); d->chunk += batch; return 1;
}

static void *syzygy_open(const char *path){
    static int ready; if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) { indices_init(); __atomic_store_n(&ready, 1, __ATOMIC_RELEASE); }
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\'); if (back > slash) slash = back;
    const char *base = slash ? slash + 1 : path, *dot = strrchr(base, '.');
    int type = dot && !strcmp(dot, ".rtbz") ? TB_DTZ : dot && !strcmp(dot, ".rtbw") ? TB_WDL : -1;
    if (type < 0 || (size_t)(dot - base) >= 16) { fprintf(stderr, "%s: a Syzygy table is NAME.rtbw or NAME.rtbz\n", path); return NULL; }
    int w[7] = { 0 }, k[7] = { 0 }, *x = w, n = 0; static const char L[] = " PNBRQK";
    for (const char *c = base; c < dot; c++) { if (*c == 'v') { if (x == k) break; x = k; continue; } const char *p = strchr(L + 1, *c); if (!p || !*c) { n = -1; break; } x[p - L]++; n++; }
    if (n < 3 || n > TB_PIECES || w[KING] != 1 || k[KING] != 1) { fprintf(stderr, "%s: %.*s names no material of two kings\n", path, (int)(dot - base), base); return NULL; }
    Decoder *d = calloc(1, sizeof *d); d->type = type; d->num = n;
    snprintf(d->set.dir, sizeof d->set.dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    if (!set_reach(&d->set, w, k, 0)) { fprintf(stderr, "%s: a table it reaches cannot be read\n", path); free(d); return NULL; }
    int flip; d->table = set_find(&d->set, w, k, &flip);
    if (d->table < 0 || flip || (type == TB_DTZ && !d->set.t[d->table].bytes[TB_DTZ])) { fprintf(stderr, "%s: not read as a table of its set\n", path); free(d); return NULL; }
    /* its pieces, each colouring: the material as named (White the first side), and with the colours swapped unless the
     * two sides hold the same */
    d->orientations = d->set.t[d->table].symmetric ? 1 : 2;
    for (int o = 0; o < d->orientations; o++) { int i = 0;
        for (int side = 0; side < 2; side++) { const int *m = side ? k : w; int sign = (side ^ o) ? -1 : 1;
            for (int p = KING; p >= PAWN; p--) for (int j = 0; j < m[p]; j++) d->piece[o][i++] = (int8_t)(sign * p); } }
    d->head = n < 3 ? n : 3; d->chunks = (uint64_t)d->orientations; for (int i = 0; i < d->head; i++) d->chunks *= 64;
    return d;
}
static size_t syzygy_read(void *context, uint8_t *out, size_t capacity){
    Decoder *d = context; if (d->failed) return 0;
    while (d->at == d->out.n) if (!decoder_refill(d)) {
        if (d->positions) { static const char *name[2] = { "WDL", "DTZ" };
            fprintf(stderr, "\n  syzygy %s %s: %llu positions, %llu lines, %llu not probed\n", d->set.t[d->table].name, name[d->type], (unsigned long long)d->positions, (unsigned long long)d->written, (unsigned long long)d->unprobed); d->positions = 0; }
        return 0; }
    size_t n = d->out.n - d->at; if (n > capacity) n = capacity;
    memcpy(out, d->out.b + d->at, n); d->at += n; return n;
}
static int syzygy_error(void *context){ Decoder *d = context; return d->failed || d->unprobed; }
static void syzygy_close(void *context){
    Decoder *d = context; for (int i = 0; i < d->set.n; i++) table_free(&d->set.t[i]); free(d->set.t); lp_buf_free(&d->out); free(d);
}
const InputDecoder syzygy_decoder = { "syzygy", syzygy_open, syzygy_read, syzygy_error, syzygy_close };
