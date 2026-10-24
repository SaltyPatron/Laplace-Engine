// UAX #38 reference lists: scalar targets with dictionary/usage qualifiers.
// The outer grammar owns escaping; this parser reads its included field range.
module.exports = grammar({
  name: 'scalar_references',
  extras: $ => [],
  rules: {
    document: $ => seq(optional($._space), $.reference,
      repeat(seq($._space, $.reference)), optional($._space)),
    reference: $ => seq('U+', field('target', $.scalar),
      optional(seq('<', $.qualifier, repeat(seq(',', $.qualifier))))),
    scalar: $ => /[0-9A-F]{4,6}/,
    qualifier: $ => seq(field('source', $.source),
      optional(seq(':', field('flags', $.flags)))),
    source: $ => /[ks][A-Za-z0-9_]+/,
    flags: $ => /[TBZFJ]+/,
    _space: $ => /[ \t\r\n]+/
  }
});
