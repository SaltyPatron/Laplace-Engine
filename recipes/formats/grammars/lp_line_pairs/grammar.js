// Two-line conformance records. Comments and blank lines may occur between
// the two fields; a hash inside either field remains content.
module.exports = grammar({
  name: 'lp_line_pairs',
  extras: $ => [],
  rules: {
    document: $ => repeat(choice($.record, $.comment, $.blank_line)),
    record: $ => seq($.source, $._newline,
      repeat(choice($.comment, $.blank_line)), $.result),
    source: $ => /[^#\r\n][^\r\n]*/,
    result: $ => /[^#\r\n][^\r\n]*/,
    comment: $ => /#[^\r\n]*/,
    blank_line: $ => $._newline,
    _newline: $ => choice('\r\n', '\n', '\r')
  }
});
