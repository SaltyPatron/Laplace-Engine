// Blank-line-separated records, with physical lines retained as fields.
module.exports = grammar({
  name: 'lp_paragraphs',
  extras: $ => [],
  rules: {
    document: $ => seq(optional($.separator), optional(seq($.record,
      repeat(seq($.separator, $.record)), optional($.separator)))),
    record: $ => seq($.line, repeat(seq($._newline, $.line)), optional($._newline)),
    line: $ => /[^\r\n]+/,
    separator: $ => /\r?\n([ \t]*\r?\n)+/,
    _newline: $ => choice('\r\n', '\n', '\r')
  }
});
