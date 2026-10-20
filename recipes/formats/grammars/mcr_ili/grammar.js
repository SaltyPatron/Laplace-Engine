// MCR's versioned ILI reference. The highway identity is its referenced offset,
// not this packaging prefix; the recipe binds the parsed fields.
module.exports = grammar({
  name: 'mcr_ili',
  extras: $ => [],
  rules: {
    document: $ => $.reference,
    reference: $ => seq(optional('mcr:'), 'ili-',
      field('version', $.version), '-', field('offset', $.offset), '-', field('pos', $.pos)),
    version: $ => /[0-9]{2}/,
    offset: $ => /[0-9]{8}/,
    pos: $ => choice('n', 'v', 'a', 's', 'r')
  }
});
