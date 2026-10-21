// UD's core relation and ordered subtype identifiers. The original field
// remains available as its canonical full identifier; subtype inference reads
// this grammar's base rather than stripping source bytes in C.
module.exports = grammar({
  name: 'ud_relation',
  extras: $ => [],
  rules: {
    document: $ => choice('_', seq($.base, repeat(seq(':', $.subtype)))),
    base: $ => /[a-z]+/,
    subtype: $ => /[^:|\t\r\n]+/
  }
});
