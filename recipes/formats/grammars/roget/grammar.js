// Roget's Thesaurus of English Words and Phrases (Project Gutenberg eBook #10681). The text is classed in Classes and
// Sections (capitals), with numbered subsections ("1. BEING, IN THE ABSTRACT") and, under them, numbered heads: "1.
// Existence -- N. existence, being, entity, ens [Lat.], ...". A head's synonyms are grouped by part of speech, each
// group begun by its label (N. V. Adj. Adv. Phr. Int.) and made of words and phrases parted by commas and semicolons,
// across lines, with brackets ([Lat.], [Science of existence]), references (&c adj., &c 494) and daggers.
//
// The lexer takes the longest token that matches. A word may not begin where a label does ("V. ", "Adj. "): a word
// begins with a lower-case letter, a space or mark, or a capital with three or more lower-case letters after it.
module.exports = grammar({
  name: 'roget',
  extras: $ => [],
  rules: {
    document: $ => seq(repeat($.front), $.start, repeat(choice($.blank, $.heading, $.subsection, $.head)), $.end, repeat($.back)),
    front: $ => token(prec(-1, /[^\r\n]*\r?\n/)),
    start: $ => token(prec(1, /\*\*\* START[^\r\n]*\r?\n/)),
    end: $ => token(prec(1, /\*\*\* END[^\r\n]*\r?\n/)),
    back: $ => /[^\r\n]*\r?\n/,
    blank: $ => /[ \t]*\r?\n/,
    // CLASS I, SECTION II., a class's or section's title, a subsection's group of heads ("Internal conditions"): a line
    // that begins with a letter or a mark, not a digit or a space.
    heading: $ => /[^0-9*\r\n \t][^\r\n]*\r?\n/,
    subsection: $ => seq($.number, $.title, choice($._break, $._nl)),
    head: $ => seq($.number, $.title, $._dash, repeat1($.group), $._break),
    number: $ => /[0-9]+[a-z]?\.?/,
    title: $ => /([^\u2014\r\n]|\r?\n[^\u2014\r\n])+/,
    _dash: $ => /(\r?\n)?\u2014( |\r?\n)?/,
    group: $ => seq(choice($.label, alias($.label_bare, $.label)), repeat(choice($.item, $.bracket, $.reference, $._dagger, $._stop, $._sep, $._nl, $.other))),
    // A word or phrase of a group, the thing the head holds.
    item: $ => $.word,
    label: $ => /(N|V|Adj|Adv|Phr|Int)\. /,
    label_bare: $ => /(N|V|Adj|Adv|Phr|Int)\./,
    word: $ => /([^A-Z.,;\r\n&\[\u2020]|[A-Z][a-z][a-z][a-z]+|[A-Z][a-z]?[a-z]?[^a-z.,;\r\n&\[\u2020])([^,;\r\n&\[\u2020]*[^.,;\r\n&\[\u2020])?/,
    // The full stop that ends a list is the thesaurus's punctuation, not part of the last word.
    _stop: $ => /\./,
    bracket: $ => /\[([^\]\r\n]|\r?\n[^\]\r\n])*\]/,
    reference: $ => /&c[^,;\r\n]*/,
    _dagger: $ => '\u2020',
    _sep: $ => /[,;]/,
    _nl: $ => /\r?\n/,
    _break: $ => /\r?\n\r?\n/,
    other: $ => token(prec(-1, /[^\r\n]/))
  }
});
