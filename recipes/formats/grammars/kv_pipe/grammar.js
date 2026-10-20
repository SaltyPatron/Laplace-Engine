// CoNLL-U FEATS/MISC: one field's syntax, injected over its original byte range.
module.exports = grammar({
  name: 'kv_pipe',
  extras: $ => [],
  rules: {
    document: $ => choice('_', seq($.pair, repeat(seq('|', $.pair)))),
    pair: $ => seq(field('key', $.name), '=', field('value', $.value)),
    name: $ => /[^=|\t\r\n]+/,
    value: $ => /[^|\t\r\n]+/
  }
});
