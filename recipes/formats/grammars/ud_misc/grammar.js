// CoNLL-U MISC is a pipe-separated list of treebank-specific annotations.
// Unlike FEATS, its entries do not have to be attribute/value assignments.
module.exports = grammar({
  name: 'ud_misc',
  extras: $ => [],
  rules: {
    document: $ => seq($._annotation, repeat(seq('|', $._annotation))),
    _annotation: $ => choice($.pair, $.empty_pair, $.flag),
    pair: $ => seq(field('key', $.name), '=', field('value', $.value)),
    empty_pair: $ => seq($.name, '='),
    name: $ => /[^=|\t\r\n]+/,
    value: $ => /[^|\t\r\n]+/,
    flag: $ => /[^=|\t\r\n]+/
  }
});
