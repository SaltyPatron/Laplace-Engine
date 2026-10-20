// Whitespace-separated records. Padding separates fields and is retained in
// the packaging tree; unlike delimited cells, repeated spaces are not columns.
module.exports = grammar({
  name: 'lp_words',
  extras: $ => [],
  rules: {
    document: $ => seq(repeat(choice(seq($.row, choice('\r\n', '\n', '\r')), $.blank_line)), optional($.row)),
    row: $ => seq(optional($.spacing), $.field, repeat(seq($.spacing, $.field)), optional($.spacing)),
    spacing: $ => /[ \t]+/,
    field: $ => /[^ \t\r\n]+/,
    blank_line: $ => /[ \t]*(\r\n|\n|\r)/
  }
});
