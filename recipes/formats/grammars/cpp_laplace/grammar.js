// C++ adds its language rules to the same C packaging profile. Braced default
// arguments are initializer lists, not compound literals with a missing type.
const C = require('../c_laplace/grammar.js');
module.exports = grammar(require('./upstream/cpp.js'), {
  name: 'cpp_laplace',
  conflicts: ($, original) => original.concat(C.workspaceConflicts($), [
    [$._declarator, $.type_specifier, $.expression, $.macro_declaration],
    [$._block_item, $._initializer_directive],
    [$.comma_expression, $._initializer_value]
  ]),
  rules: {
    optional_parameter_declaration: $ => seq(
      $._declaration_specifiers,
      field('declarator', optional(choice($._declarator, $.abstract_reference_declarator))),
      '=', field('default_value', choice($.expression, $.initializer_list))
    )
  }
});
