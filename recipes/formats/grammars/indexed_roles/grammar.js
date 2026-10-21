// A list of record-local references qualified by a construction/semantic role.
module.exports = grammar({
  name: 'indexed_roles',
  extras: $ => [],
  rules: {
    document: $ => seq($.binding, repeat(seq(',', $.binding))),
    binding: $ => seq($.target, ':', $.role),
    target: $ => /[0-9]+(\.[0-9]+)?/,
    role: $ => /[^,|\t\r\n]+/
  }
});
