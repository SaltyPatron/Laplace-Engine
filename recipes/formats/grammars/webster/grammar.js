// Webster's Unabridged Dictionary (1913), as Project Gutenberg sets it in plain text. An entry is a headword line
// written in capitals, then a signature line: the word as it is syllabified and accented (Ab"la*tive), its variants,
// its parts of speech (n., v. t., a.), a bracketed inflection, an etymology (Etym: [...]) and a domain label such as
// (Gram.). Senses, Defn: paragraphs, quotations, usage labels ([Obs.]), "See X" references and Syn. lists follow, in
// lines, until the next headword.
//
// The lexer takes the longest token that matches, and decides by precedence only between tokens of one length. A line
// written wholly in capitals is a headword because the headword token takes the line's CR, which no body token does,
// and so is the longest. The text tokens are written so as to stop before a "See ", "see " or "Syn.", where a
// reference or a list begins, since a regular expression cannot look ahead.
module.exports = grammar({
  name: 'webster',
  extras: $ => [],
  rules: {
    document: $ => seq(repeat(choice($.preface, $.blank)), repeat($.entry)),
    entry: $ => prec.right(seq(
      field('headword', $.headword), '\n',
      repeat($.blank),
      field('signature', $.signature),
      repeat(choice($.synonyms, $.body, $.blank)))),
    // Project Gutenberg's text has CR LF line ends; the token takes the CR, which a node's trim drops.
    headword: $ => /[A-Z][A-Z0-9 '\-;().,&]*\r/,
    signature: $ => choice(
      seq(repeat1($._signed), $._newline),
      seq(repeat($._signed), alias($.etymology_open, $.etymology))),
    _signed: $ => choice($.form, $.pos, $.domain, $.etymology, $.note, $._sep, $.other),
    // The syllabified spelling and its variants, parted by commas: each begins with a capital or a mark of accent.
    form: $ => token(prec(1, /[A-Z*`"'(#][^,\r\n]*/)),
    pos: $ => token(prec(1, /(v\. ?[ti]\.( ?& ?[ti]\.)?|v\.|p\. ?(p|pr|a)\.|vb\. ?n\.|imp\.|pl\.|prep\.|pron\.|conj\.|interj\.|adv\.|adj\.|a\.|n\.|pref(ix)?\.|prop\.)/)),
    _sep: $ => token(prec(1, /[, &]+/)),
    domain: $ => /\([A-Z][A-Za-zÀ-ɏ]*\.(?: [A-Z][A-Za-zÀ-ɏ]*\.)*\)/,
    // An etymology is bracketed and may run over lines, but never over a blank line: one the source leaves unclosed (a
    // quotation of Greek lost its closing bracket) ends at the blank line.
    // The line that holds an unclosed one ends with it.
    _etym_start: $ => seq(token(prec(2, 'Etym: [')),
      repeat(choice(alias($.etym_crossref, $.crossref), alias($.etym_see_bare, $.etymology_text), alias($.etym_newline, $.etymology_text), $.etymology_text, $.etymology_fragment))),
    etymology: $ => seq($._etym_start, ']'),
    etymology_open: $ => seq($._etym_start, $._etym_blank),
    _etym_blank: $ => token(prec(1, /\r?\n\r?\n/)),
    // A run of plain text stops before a "See " or "see "; a lone S or s that ends a line takes the CR after it (a node's
    // trim drops it), and one that ends the bracket is a fragment of its own.
    etymology_text: $ => /([^\]\r\nSs]|S[^e\]\r\n]|s[^e\]\r\n]|[Ss]e[^e\]\r\n]|[Ss]ee[^ \]\r\n]|[Ss]\r|[Ss]e\r|[Ss]ee\r|\r?\n[^\]\r\nSs]|\r?\n[Ss][^e\]\r\n]|\r?\n[Ss]e[^e\]\r\n]|\r?\n[Ss]ee[^ \]\r\n])+/,
    etymology_fragment: $ => /[Ss]e?/,
    note: $ => /\[[^\]\r\n]*\]/,
    other: $ => token(prec(-1, /[^\r\n]/)),
    body: $ => choice(
      seq(repeat1($._said), $._newline),
      seq(repeat($._said), alias($.etymology_open, $.etymology))),
    _said: $ => choice($.usage, $.domain, $.crossref, $.see_bare, $.etymology, $.text, $.note, $.other),
    usage: $ => /\[[A-Z][A-Za-z.\- ]*\.\]/,
    // A reference: "See " or "see ", then the entry it names, written as an entry's name is, with capitals.
    // "See " and the name of the entry it points at, written with capitals as an entry's name is: one token, which the
    // recipe parts into the two. Where no such name follows, "See " is text.
    crossref: $ => /[Ss]ee [A-Z][A-Za-z'\-]*( [A-Z][A-Za-z'\-]*)*/,
    etym_crossref: $ => /[Ss]ee [A-Z][A-Za-z'\-]*( [A-Z][A-Za-z'\-]*)*/,
    see_bare: $ => alias(/[Ss]ee /, $.text),
    etym_see_bare: $ => /[Ss]ee /,
    // A line break before a reference stands apart from it, so that the reference is exactly "See " and its target.
    etym_newline: $ => /\r?\n/,
    // Text does not begin with a semicolon or a full stop, which part and end a Syn. list.
    text: $ => /([^\r\n\[(Ss;.]|S[^ey\r\n\[(]|s[^e\r\n\[(]|[Ss]e[^e\r\n\[(]|[Ss]ee[^ \r\n\[(]|Sy[^n\r\n\[(]|Syn[^.\r\n\[(]|[Ss]\r|[Ss]e\r|[Ss]ee\r)([^\r\n\[(Ss]|S[^ey\r\n\[(]|s[^e\r\n\[(]|[Ss]e[^e\r\n\[(]|[Ss]ee[^ \r\n\[(]|Sy[^n\r\n\[(]|Syn[^.\r\n\[(]|[Ss]\r|[Ss]e\r|[Ss]ee\r)*/,
    // A Syn. list: the synonyms parted by semicolons, to the full stop that ends the list. What follows is discussion.
    synonyms: $ => seq($.syn_mark, $.synonym, repeat(seq($._semi, $.synonym)), optional('.'), optional($._newline)),
    syn_mark: $ => token(prec(3, /Syn\.(\r?\n)?[ ]*-+ ?/)),
    _semi: $ => token(prec(1, /;[ \t;]*(\r?\n)?/)),
    synonym: $ => /[^;.]+/,
    blank: $ => /[ \t]*\r?\n/,
    preface: $ => token(prec(-2, /[^\r\n]+\r?\n/)),
    _newline: $ => /\r?\n/
  }
});
