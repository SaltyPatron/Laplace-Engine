// Enhanced UD dependencies. A relation may itself contain colons (obl:agent).
module.exports = grammar({
  name: 'deps',
  extras: $ => [],
  rules: {
    document: $ => choice('_', seq($.edge, repeat(seq('|', $.edge)))),
    edge: $ => seq(field('key', $.head), ':', field('value', $.relation)),
    head: $ => /[0-9]+(\.[0-9]+)?/,
    relation: $ => /[^|\t\r\n]+/
  }
});
