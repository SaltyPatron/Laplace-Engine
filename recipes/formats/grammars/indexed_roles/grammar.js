// A list of record-local references qualified by a construction/semantic role.
module.exports = grammar({
  name: 'indexed_roles',
  extras: $ => [],
  rules: {
    document: $ => seq($.binding, repeat(seq(',', $.binding))),
    binding: $ => seq($.target, ':', $.role),
    // Some treebanks name the construction anchor; others use a row ID.
    // The recipe resolves only the latter and preserves literal anchors.
    target: $ => /[^:,|\t\r\n]+/,
    role: $ => /[^,|\t\r\n]+/
  }
});
