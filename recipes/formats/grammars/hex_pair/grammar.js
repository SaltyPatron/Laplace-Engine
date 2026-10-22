// Two adjacent byte indices, parsed in-place inside an outer record field.
module.exports = grammar({
  name: 'hex_pair',
  extras: $ => [],
  rules: {
    document: $ => seq($.source, $.target),
    source: $ => /[0-9a-fA-F]{2}/,
    target: $ => /[0-9a-fA-F]{2}/
  }
});
