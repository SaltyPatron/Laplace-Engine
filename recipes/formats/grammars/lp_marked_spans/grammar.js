// Ordered text with non-nested annotated spans. Markers are packaging;
// hashes are comments only at the beginning of a physical line.
module.exports = grammar({
  name: 'lp_marked_spans',
  extras: $ => [],
  rules: {
    document: $ => repeat(choice($.record, $.comment, $.blank_line)),
    record: $ => prec.right(seq(choice($._first_text, $.span),
      repeat(choice($.text, $.span)), optional($._newline))),
    span: $ => seq($.open, $.span_text, $.close),
    _first_text: $ => alias(/[^#⸠⸡\r\n][^⸠⸡\r\n]*/, $.text),
    text: $ => token(prec(2, /[^⸠⸡\r\n]+/)),
    span_text: $ => /[^⸠⸡\r\n]+/,
    open: $ => '⸠',
    close: $ => '⸡',
    comment: $ => token(prec(1, /#[^\r\n]*/)),
    blank_line: $ => $._newline,
    _newline: $ => choice('\r\n', '\n', '\r')
  }
});
