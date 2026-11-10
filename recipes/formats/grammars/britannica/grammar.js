// Encyclopaedia Britannica, 11th Edition (1911), in Project Gutenberg's slices. An article begins at a paragraph whose
// first words are the article's headword in capitals ("CATABOLISM, or KATABOLISM (Gr. ...", "CATAMARCA (_San Fernando
// de Catamarca_), capital ..."); its further paragraphs follow, with references to other articles written as "see
// CATACOMB" or "See BIBLIOGRAPHY AND BIBLIOLOGY".
//
// The lexer takes the longest token that matches. A paragraph that opens an article begins with two capitals; one that
// does not begins otherwise, and so the two openings never compete. A run of text stops before a reference.
module.exports = grammar({
  name: 'britannica',
  extras: $ => [],
  conflicts: $ => [[$.article]],
  rules: {
    document: $ => seq(repeat($.front), $.start, optional($._item), repeat(seq($._para, $._item)), $.end, repeat($.back)),
    _item: $ => choice($.article, $.paragraph),
    front: $ => token(prec(-1, /[^\r\n]*\r?\n/)),
    start: $ => token(prec(1, /\*\*\* START[^\r\n]*(\r?\n)+/)),
    end: $ => token(prec(1, /(\r?\n)*\*\*\* END[^\r\n]*\r?\n/)),
    back: $ => /[^\r\n]*\r?\n/,
    article: $ => seq($.headword, repeat($._unit), repeat(seq($._para, $.paragraph))),
    // A paragraph that opens no article.
    paragraph: $ => seq($.lead, repeat($._unit)),
    // The headword: capitals, with the commas and spaces of a name ("CATARGIU, LASCAR"), to the comma or bracket that opens the text.
    headword: $ => /[A-Z][A-Z'\-]+( [A-Z][A-Z'\-]+)*(, [A-Z][A-Z'\-]+( [A-Z][A-Z'\-]+)*)*/,
    lead: $ => /([^A-Z\r\n]|[A-Z][^A-Z\r\n])([^\r\nSs]|S[^e\r\n]|s[^e\r\n]|[Ss]e[^e\r\n]|[Ss]ee[^ \r\n])*/,
    _unit: $ => choice($.crossref, $.see_bare, $.text, $.other, $._nl),
    crossref: $ => /[Ss]ee [A-Z][A-Z'\-]+( [A-Z][A-Z'\-]+)*/,
    see_bare: $ => alias(/[Ss]ee /, $.text),
    text: $ => /([^\r\nSs]|S[^e\r\n]|s[^e\r\n]|[Ss]e[^e\r\n]|[Ss]ee[^ \r\n])+/,
    other: $ => token(prec(-1, /[^\r\n]/)),
    _nl: $ => /\r?\n/,
    _para: $ => /\r?\n(\r?\n)+/
  }
});
