#include "literal_sql.h"
#include <string.h>

int sql_literal_bounds(const uint8_t *text, uint32_t length, SqlLiteral *out){
    *out = (SqlLiteral){.end=length};
    if (!length) return 1;
    if (text[0] == '$') {
        const uint8_t *close = memchr(text + 1, '$', length - 1);
        if (!close) return 0;
        uint32_t tag = (uint32_t)(close - text + 1);
        if (tag > length / 2 || memcmp(text, text + length - tag, tag)) return 0;
        out->begin = tag; out->end = length - tag; return 1;
    }
    uint32_t begin = 0;
    if (length > 2 && (text[0] == 'e' || text[0] == 'E' || text[0] == 'n' || text[0] == 'N') && text[1] == '\'') {
        begin = 1; out->escaped = text[0] == 'e' || text[0] == 'E';
    }
    uint8_t quote = text[begin];
    if (quote != '\'' && quote != '"') return 1;
    if (length < begin + 2 || text[length - 1] != quote) return 0;
    out->begin = begin + 1; out->end = length - 1; out->quote = quote; return 1;
}
