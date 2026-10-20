/* Lexical XML tokens for the scope-event grammar. No tag stack, DOM or heap
 * allocations: scope matching belongs to the compiled Engine walker. */
#include "tree_sitter/parser.h"
#include <stdint.h>
#include <string.h>

enum { PI_TARGET, PI_CONTENT, COMMENT, CHAR_DATA, CDATA, XML_MODEL, XML_STYLESHEET };

static void advance(TSLexer *lexer){ lexer->advance(lexer, false); }
static bool xml_char(int32_t c){
    return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff) ||
        (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
static bool name_start(int32_t c){
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xf6) || (c >= 0xf8 && c <= 0x2ff) ||
        (c >= 0x370 && c <= 0x37d) || (c >= 0x37f && c <= 0x1fff) ||
        (c >= 0x200c && c <= 0x200d) || (c >= 0x2070 && c <= 0x218f) ||
        (c >= 0x2c00 && c <= 0x2fef) || (c >= 0x3001 && c <= 0xd7ff) ||
        (c >= 0xf900 && c <= 0xfdcf) || (c >= 0xfdf0 && c <= 0xfffd) || (c >= 0x10000 && c <= 0xeffff);
}
static bool name_char(int32_t c){
    return name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xb7 ||
        (c >= 0x300 && c <= 0x36f) || (c >= 0x203f && c <= 0x2040);
}
static bool pi_target(TSLexer *lexer, const bool *valid){
    if (!name_start(lexer->lookahead)) return false;
    char ascii[16]; uint32_t length = 0; bool plain = true;
    do {
        int32_t c = lexer->lookahead;
        if (length < sizeof ascii) ascii[length] = (char)c;
        plain &= c < 128; length++; advance(lexer);
    } while (!lexer->eof(lexer) && name_char(lexer->lookahead));
    lexer->mark_end(lexer);
    if (plain && length == 3 && (ascii[0] | 32) == 'x' && (ascii[1] | 32) == 'm' && (ascii[2] | 32) == 'l') return false;
    if (plain && length == 9 && !memcmp(ascii, "xml-model", 9) && valid[XML_MODEL]) lexer->result_symbol = XML_MODEL;
    else if (plain && length == 14 && !memcmp(ascii, "xml-stylesheet", 14) && valid[XML_STYLESHEET]) lexer->result_symbol = XML_STYLESHEET;
    else { if (!valid[PI_TARGET]) return false; lexer->result_symbol = PI_TARGET; }
    return true;
}
static bool pi_content(TSLexer *lexer){
    bool any = false;
    while (!lexer->eof(lexer)) {
        if (!xml_char(lexer->lookahead)) return false;
        if (lexer->lookahead == '?') {
            lexer->mark_end(lexer); advance(lexer);
            if (lexer->lookahead == '>') { lexer->result_symbol = PI_CONTENT; return any; }
            lexer->mark_end(lexer); any = true;
        } else { advance(lexer); lexer->mark_end(lexer); any = true; }
    }
    return false;
}
static bool comment(TSLexer *lexer){
    static const char begin[] = "<!--";
    for (unsigned i = 0; i < sizeof begin - 1; i++) {
        if (lexer->eof(lexer) || lexer->lookahead != begin[i]) return false;
        advance(lexer);
    }
    while (!lexer->eof(lexer)) {
        if (!xml_char(lexer->lookahead)) return false;
        if (lexer->lookahead == '-') {
            advance(lexer);
            if (lexer->lookahead == '-') {
                advance(lexer); if (lexer->lookahead != '>') return false;
                advance(lexer); lexer->mark_end(lexer); lexer->result_symbol = COMMENT; return true;
            }
        } else advance(lexer);
    }
    return false;
}
static bool text(TSLexer *lexer, bool cdata){
    bool any = false;
    while (!lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        if (!xml_char(c)) return false;
        if (!cdata && (c == '<' || c == '&')) break;
        if (c == ']') {
            /* Keep at most one bracket in this token. The grammar joins
             * adjacent CData tokens; no lexer lookahead can swallow ]]>. */
            if (any) break;
            advance(lexer); lexer->mark_end(lexer);
            if (lexer->lookahead == ']') { advance(lexer); if (lexer->lookahead == '>') return false; }
            any = true; break;
        }
        advance(lexer); lexer->mark_end(lexer); any = true;
    }
    lexer->result_symbol = cdata ? CDATA : CHAR_DATA; return any;
}
bool tree_sitter_lp_xml_scopes_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid){
    (void)payload;
    if (valid[PI_TARGET] && valid[PI_CONTENT] && valid[COMMENT] && valid[CHAR_DATA] && valid[CDATA]) return false;
    if (valid[PI_TARGET] || valid[XML_MODEL] || valid[XML_STYLESHEET]) {
        if (name_start(lexer->lookahead)) return pi_target(lexer, valid);
    }
    if (valid[PI_CONTENT]) return pi_content(lexer);
    if (valid[COMMENT] && lexer->lookahead == '<') return comment(lexer);
    if (valid[CDATA]) return text(lexer, true);
    if (valid[CHAR_DATA]) return text(lexer, false);
    return false;
}
void *tree_sitter_lp_xml_scopes_external_scanner_create(void){ return NULL; }
void tree_sitter_lp_xml_scopes_external_scanner_destroy(void *payload){ (void)payload; }
unsigned tree_sitter_lp_xml_scopes_external_scanner_serialize(void *payload, char *buffer){ (void)payload; (void)buffer; return 0; }
void tree_sitter_lp_xml_scopes_external_scanner_deserialize(void *payload, const char *buffer, unsigned length){ (void)payload; (void)buffer; (void)length; }
