/* Tree-sitter lexical start conditions and counted repetition for wndb(5).
 * The generated parser consumes every field and constructs the AST. Scanner
 * state is fixed size, serialized at token boundaries, and never allocated per
 * record. Count end tokens are zero width and cannot be emitted prematurely. */
#include "tree_sitter/parser.h"
#include <stdint.h>
#include <stdlib.h>

enum Token {
    DATA_START, INDEX_START, SENSE_START, EXC_START, FRAME_START, SENTIDX_START, HEADER_START,
    DATA_POS, WORD_COUNT, WORD_TOKEN, WORDS_END, POINTER_COUNT, POINTER_SYMBOL, POINTERS_END,
    FRAME_COUNT, FRAME_PLUS, FRAMES_END, NONVERB, INDEX_COUNT, INDEX_POINTER_COUNT,
    INDEX_POINTER_SYMBOL, INDEX_POINTERS_END, INDEX_SENSE_COUNT, INDEX_TAG_COUNT, INDEX_OFFSET, INDEX_END
};
enum Stage { READY, DATA_TYPE, WORDS_COUNT, WORDS, POINTERS_COUNT, POINTERS, FRAMES_COUNT,
             FRAMES, FINISHED, INDEX_WORDS_COUNT, INDEX_PTR_COUNT, INDEX_PTRS, INDEX_SENSES, INDEX_TAGS, INDEX_OFFSETS };
typedef struct { uint8_t stage, verb; uint32_t words, pointers, frames, total; } Scanner;

static bool space(int32_t c){ return c == ' ' || c == '\t'; }
static bool eol(int32_t c){ return !c || c == '\r' || c == '\n'; }
static void skip(TSLexer *l){ while (space(l->lookahead)) l->advance(l, true); }
static int digit(int32_t c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool number(TSLexer *l, unsigned base, unsigned width, uint32_t *value){
    skip(l); uint32_t v = 0; unsigned n = 0; int d;
    while ((d = digit(l->lookahead)) >= 0 && (unsigned)d < base) {
        if (v > (UINT32_MAX - (unsigned)d) / base) return false;
        v = v * base + (unsigned)d; n++; l->advance(l, false);
    }
    if (!n || (width && n != width) || (!space(l->lookahead) && !eol(l->lookahead))) return false;
    *value = v; l->mark_end(l); return true;
}
static bool token(TSLexer *l){
    skip(l); if (eol(l->lookahead)) return false;
    do { l->advance(l, false); } while (!space(l->lookahead) && !eol(l->lookahead));
    l->mark_end(l); return true;
}
typedef struct { uint32_t length; bool decimal, percent, comma_list, pos; } Prefix;
static Prefix prefix(TSLexer *l){
    Prefix p = {0, true, false, true, false};
    skip(l); int32_t first = l->lookahead; bool after_comma = false;
    while (!space(l->lookahead) && !eol(l->lookahead)) {
        int32_t c = l->lookahead;
        p.decimal &= c >= '0' && c <= '9'; p.percent |= c == '%';
        if (c == ',') { if (!p.length || after_comma) p.comma_list = false; after_comma = true; }
        else { p.comma_list &= c >= '0' && c <= '9'; after_comma = false; }
        p.length++; l->advance(l, false);
    }
    p.decimal &= p.length != 0; p.comma_list &= p.length != 0 && !after_comma;
    p.pos = p.length == 1 && (first == 'n' || first == 'v' || first == 'a' || first == 'r' || first == 's');
    return p;
}
/* A zero-width lexical start condition removes ambiguity between the unrelated
 * line formats. Only the first four whitespace-delimited tokens are inspected;
 * their bytes are subsequently consumed by the generated parser, never copied. */
static int classify(TSLexer *l){
    if ((l->get_column(l) != 0 && !l->is_at_included_range_start(l)) || eol(l->lookahead)) return -1;
    l->mark_end(l);
    if (space(l->lookahead)) return HEADER_START;
    Prefix a = prefix(l), b = prefix(l), c = prefix(l), d = prefix(l);
    if (a.decimal && a.length == 8 && b.decimal && b.length == 2 && c.pos) return DATA_START;
    if (a.percent && b.decimal && b.length == 8) return SENSE_START;
    if (a.percent && (!b.length || b.comma_list)) return SENTIDX_START;
    if (b.pos && c.decimal && d.decimal) return INDEX_START;
    if (a.decimal) return FRAME_START;
    return EXC_START;
}

void *tree_sitter_wndb_external_scanner_create(void){ return calloc(1, sizeof(Scanner)); }
void tree_sitter_wndb_external_scanner_destroy(void *payload){ free(payload); }
unsigned tree_sitter_wndb_external_scanner_serialize(void *payload, char *buffer){
    const Scanner *s = payload; buffer[0] = (char)s->stage; buffer[1] = (char)s->verb;
    uint32_t values[] = {s->words, s->pointers, s->frames, s->total};
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++) buffer[2 + 4*i + j] = (char)(values[i] >> (8*j));
    return 18;
}
void tree_sitter_wndb_external_scanner_deserialize(void *payload, const char *buffer, unsigned length){
    Scanner *s = payload; *s = (Scanner){0}; if (length != 18) return;
    s->stage = (uint8_t)buffer[0]; s->verb = (uint8_t)buffer[1];
    uint32_t *values[] = {&s->words, &s->pointers, &s->frames, &s->total};
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++) *values[i] |= (uint32_t)(uint8_t)buffer[2 + 4*i + j] << (8*j);
}
bool tree_sitter_wndb_external_scanner_scan(void *payload, TSLexer *l, const bool *valid){
    Scanner *s = payload; uint32_t n;
    if (valid[DATA_START] || valid[INDEX_START] || valid[SENSE_START] || valid[EXC_START] ||
        valid[FRAME_START] || valid[SENTIDX_START] || valid[HEADER_START]) {
        int kind = classify(l);
        if (kind >= 0 && valid[kind]) {
            *s = (Scanner){0};
            s->stage = kind == DATA_START ? DATA_TYPE : kind == INDEX_START ? INDEX_WORDS_COUNT : FINISHED;
            l->result_symbol = (TSSymbol)kind; return true;
        }
        return false;
    }
    if (valid[DATA_POS] && s->stage == DATA_TYPE) {
        skip(l); int32_t c = l->lookahead;
        if (c != 'n' && c != 'v' && c != 'a' && c != 'r' && c != 's') return false;
        l->advance(l, false); if (!space(l->lookahead)) return false; l->mark_end(l);
        s->verb = c == 'v'; s->stage = WORDS_COUNT; l->result_symbol = DATA_POS; return true;
    }
    if (valid[WORD_COUNT] && s->stage == WORDS_COUNT && number(l, 16, 2, &n) && n) {
        s->words = n; s->stage = WORDS; l->result_symbol = WORD_COUNT; return true;
    }
    if (valid[WORD_TOKEN] && s->stage == WORDS && s->words && token(l)) {
        s->words--; l->result_symbol = WORD_TOKEN; return true;
    }
    if (valid[WORDS_END] && s->stage == WORDS && !s->words) {
        s->stage = POINTERS_COUNT; l->result_symbol = WORDS_END; return true;
    }
    if (valid[POINTER_COUNT] && s->stage == POINTERS_COUNT && number(l, 10, 3, &n)) {
        s->pointers = n; s->stage = POINTERS; l->result_symbol = POINTER_COUNT; return true;
    }
    if (valid[POINTER_SYMBOL] && s->stage == POINTERS && s->pointers && token(l)) {
        s->pointers--; l->result_symbol = POINTER_SYMBOL; return true;
    }
    if (valid[POINTERS_END] && s->stage == POINTERS && !s->pointers) {
        s->stage = s->verb ? FRAMES_COUNT : FINISHED; l->result_symbol = POINTERS_END; return true;
    }
    if (valid[NONVERB] && s->stage == FINISHED && !s->verb) { l->result_symbol = NONVERB; return true; }
    if (valid[FRAME_COUNT] && s->stage == FRAMES_COUNT && number(l, 10, 2, &n)) {
        s->frames = n; s->stage = FRAMES; l->result_symbol = FRAME_COUNT; return true;
    }
    if (valid[FRAME_PLUS] && s->stage == FRAMES && s->frames) {
        skip(l); if (l->lookahead != '+') return false; l->advance(l, false); l->mark_end(l);
        s->frames--; l->result_symbol = FRAME_PLUS; return true;
    }
    if (valid[FRAMES_END] && s->stage == FRAMES && !s->frames) {
        s->stage = FINISHED; l->result_symbol = FRAMES_END; return true;
    }
    if (valid[INDEX_COUNT] && s->stage == INDEX_WORDS_COUNT && number(l, 10, 0, &n) && n) {
        s->words = s->total = n; s->stage = INDEX_PTR_COUNT; l->result_symbol = INDEX_COUNT; return true;
    }
    if (valid[INDEX_POINTER_COUNT] && s->stage == INDEX_PTR_COUNT && number(l, 10, 0, &n)) {
        s->pointers = n; s->stage = INDEX_PTRS; l->result_symbol = INDEX_POINTER_COUNT; return true;
    }
    if (valid[INDEX_POINTER_SYMBOL] && s->stage == INDEX_PTRS && s->pointers && token(l)) {
        s->pointers--; l->result_symbol = INDEX_POINTER_SYMBOL; return true;
    }
    if (valid[INDEX_POINTERS_END] && s->stage == INDEX_PTRS && !s->pointers) {
        s->stage = INDEX_SENSES; l->result_symbol = INDEX_POINTERS_END; return true;
    }
    if (valid[INDEX_SENSE_COUNT] && s->stage == INDEX_SENSES && number(l, 10, 0, &n) && n == s->total) {
        s->stage = INDEX_TAGS; l->result_symbol = INDEX_SENSE_COUNT; return true;
    }
    if (valid[INDEX_TAG_COUNT] && s->stage == INDEX_TAGS && number(l, 10, 0, &n) && n <= s->total) {
        s->stage = INDEX_OFFSETS; l->result_symbol = INDEX_TAG_COUNT; return true;
    }
    if (valid[INDEX_OFFSET] && s->stage == INDEX_OFFSETS && s->words && number(l, 10, 8, &n)) {
        s->words--; l->result_symbol = INDEX_OFFSET; return true;
    }
    if (valid[INDEX_END] && s->stage == INDEX_OFFSETS && !s->words) {
        s->stage = FINISHED; l->result_symbol = INDEX_END; return true;
    }
    return false;
}
