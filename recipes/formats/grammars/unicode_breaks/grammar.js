// UCD auxiliary break tests: each marker precedes a hexadecimal scalar;
// the final marker describes the boundary after the last scalar.
module.exports = grammar({
  name: 'unicode_breaks',
  extras: $ => [/[ \t]+/, $.comment],
  rules: {
    document: $ => repeat(choice($.record, $._newline)),
    record: $ => seq(repeat1($.token), field('after', $.boundary), $._newline),
    token: $ => seq(field('before', $.boundary), field('codepoint', $.hex)),
    boundary: $ => choice('÷', '×'),
    hex: $ => /[0-9A-Fa-f]{4,6}/,
    comment: $ => /#[^\r\n]*/,
    _newline: $ => /\r?\n/
  }
});
