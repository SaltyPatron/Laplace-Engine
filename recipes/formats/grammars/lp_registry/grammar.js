// RFC-style key/value records separated by a %% line. Continuations belong
// to their preceding value in the native AST, never to a neighboring field.
module.exports = grammar({
  name: 'lp_registry',
  extras: $ => [],
  conflicts: $ => [[$.record], [$.value]],
  rules: {
    document: $ => seq($.record, repeat(seq($._separator, $.record)), optional($._newline)),
    record: $ => seq($.pair, repeat(seq($._newline, $.pair))),
    pair: $ => seq(field('key', $.key), ':', field('value', $.value)),
    key: $ => /[A-Za-z][A-Za-z0-9-]*/,
    value: $ => seq($.text, repeat(seq($._newline, $.continuation))),
    text: $ => token(/[^\r\n]*/),
    continuation: $ => /[ \t][^\r\n]*/,
    _separator: $ => /\r?\n%%\r?\n/,
    _newline: $ => choice('\r\n', '\n')
  }
});
