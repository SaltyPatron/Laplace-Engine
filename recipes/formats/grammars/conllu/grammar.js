// CoNLL-U's ten tab-separated columns, sentence boundaries and comment records.
module.exports = grammar({
  name: 'conllu',
  extras: $ => [],
  rules: {
    document: $ => seq(repeat($._newline), optional(seq(
      $.sentence, repeat(seq(repeat1($._newline), $.sentence)), repeat($._newline)))),
    sentence: $ => repeat1(choice($.comment, $.word_line)),
    comment: $ => seq('#', optional(field('key', $.note_key)),
      optional(seq('=', field('value', $.note_value))), $._newline),
    note_key: $ => /[^=\r\n]+/,
    note_value: $ => token(/[^\r\n]*/),
    word_line: $ => seq(
      field('ID', $.id), '\t', field('FORM', $.field), '\t',
      field('LEMMA', $.field), '\t', field('UPOS', $.field), '\t',
      field('XPOS', $.field), '\t', field('FEATS', $.field), '\t',
      field('HEAD', $.field), '\t', field('DEPREL', $.field), '\t',
      field('DEPS', $.field), '\t', field('MISC', $.field), $._newline),
    id: $ => /[0-9]+([.-][0-9]+)?/,
    field: $ => /[^\t\r\n]+/,
    _newline: $ => choice('\n', '\r\n')
  }
});
