// Physical text records expose blank-line boundaries as integer symbols.
// Paragraph and section layout instructions group these grammar records.
module.exports = grammar({
  name: 'lp_records',
  extras: $ => [],
  rules: {
    document: $ => seq(repeat(choice(seq($.line, choice('\r\n', '\n', '\r')), $.blank_line)), optional($.line)),
    line: $ => /[^\r\n]+/,
    blank_line: $ => choice('\r\n', '\n', '\r')
  }
});
