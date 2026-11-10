// The Standard Electrical Dictionary (T. O'Conor Sloane, 1892), Project Gutenberg eBook #36776. Two empty lines part
// the entries. An entry is a term on a line of its own ("Abscissa.", "Absolute. adj."), then its definition in
// paragraphs, with references to other terms ("(See Co-ordinates)", "see \"Zero, Absolute\"") and, at the foot, the
// synonyms the book lists ("Synonyms--Actual Energy--Energy of Motion.").
//
// The lexer takes the longest token that matches, so a run of text is written to stop before a "See " or "see ", and
// not to begin with the hyphens or the full stop that part and end a list of synonyms.
module.exports = grammar({
  name: 'sloane',
  extras: $ => [],
  rules: {
    document: $ => seq(repeat($.front), $.start, repeat($.entry), $.end, repeat($.back)),
    front: $ => token(prec(-1, /[^\r\n]*\r?\n/)),
    start: $ => token(prec(1, /\*\*\* START[^\r\n]*(\r?\n)+/)),
    end: $ => token(prec(1, /(\r?\n)*\*\*\* END[^\r\n]*\r?\n/)),
    back: $ => /[^\r\n]*\r?\n/,
    entry: $ => seq($.term, optional($.qualifier), repeat(choice($._unit, $._nl, $._para)), $._break),
    // The term is what the line writes before its first full stop; the rest of the line (its stop, "adj.") qualifies it.
    term: $ => /[^\r\n.]+/,
    qualifier: $ => /\.[^\r\n]*/,
    _unit: $ => choice($.synonyms, $.crossref, $.see_bare, $.text, $.other),
    crossref: $ => /[Ss]ee [A-Z]([^)\r\n."\-,]|,[^\-\r\n)"]|-[^\-)\r\n."])*/,
    // "See " where no term in capitals follows (a quoted title, a figure) is text.
    see_bare: $ => alias(/[Ss]ee /, $.text),
    synonyms: $ => seq($.syn_mark, $.synonym, repeat(seq($._dashes, $.synonym)), optional($._stop)),
    syn_mark: $ => token(prec(2, /(\r?\n)?Synonyms?(\.?--|\. ?)/)),
    synonym: $ => /[^-.\r\n]([^-\r\n.]|-[^-\r\n.])*/,
    _dashes: $ => '--',
    _stop: $ => '.',
    text: $ => /([^\r\n\-.Ss]|S[^e\r\n]|s[^e\r\n]|[Ss]e[^e\r\n]|[Ss]ee[^ \r\n])([^\r\n\-Ss]|[Ss][^e\r\n]|[Ss]e[^e\r\n]|[Ss]ee[^ \r\n])*/,
    other: $ => token(prec(-1, /[^\r\n]/)),
    _nl: $ => /\r?\n/,
    _para: $ => /\r?\n\r?\n/,
    _break: $ => /\r?\n\r?\n(\r?\n)+/
  }
});
