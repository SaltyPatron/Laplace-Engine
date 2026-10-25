// Export annotations are defined by the workspace headers, not C type names.
// Keep their source bytes while parsing the declarations they annotate.
const upstream = require('./upstream/c.js');
const workspaceConflicts = $ => [
    [$.type_specifier, $.expression, $.macro_type_specifier, $.macro_declaration],
    [$.else_clause, $.preproc_trailing_else],
    [$.declaration, $._declaration_modifiers],
    [$.expression, $.macro_declaration],
    [$.argument_list, $.macro_type_arguments],
    ...['preproc_if', 'preproc_ifdef', 'preproc_else', 'preproc_elif', 'preproc_elifdef'].map(name =>
      [$[name], $[name + '_in_function']])
];
module.exports = grammar(upstream, {
  name: 'c_laplace',
  conflicts: ($, original) => original.concat(workspaceConflicts($)),
  rules: {
    preproc_include: $ => seq('#include', field('path', choice(
      $.string_literal, $.system_lib_string, $.identifier,
      alias($.preproc_call_expression, $.call_expression)
    )), token.immediate(/[ \t]*\r?\n/)),
    storage_class_specifier: ($, original) => choice(...original.members, '_Thread_local'),
    type_specifier: ($, original) => choice(...original.members, $.atomic_type_specifier),
    atomic_type_specifier: $ => seq('_Atomic', '(', $.type_descriptor, ')'),
    _expression_not_binary: ($, original) => choice(...original.members, $.va_arg_expression),
    va_arg_expression: $ => seq(choice('va_arg', '__builtin_va_arg'), '(',
      field('list', $.expression), ',', field('type', $.type_descriptor), ')'),
    type_qualifier: ($, original) => choice(...original.members,
      'LP_API', 'LAPLACE_CHESS_API', 'TS_PUBLIC', 'LAPLACE_TIER_TLS', 'PROPERTY_TLS',
      'LAPLACE_XML_ERROR_CONST', 'pg_noreturn',
      'DRFLAC_API', 'DRFLAC_DEPRECATED', 'DRFLAC_INLINE', 'DRFLAC_NO_THREAD_SANITIZE',
      'DRMP3_API', 'DRMP3_INLINE', 'DRWAV_API', 'DRWAV_INLINE', 'DRWAV_PRIVATE',
      'STBIDEF', 'STBI_EXTERN', 'STBI_THREAD_LOCAL', 'stbi_inline'
    ),
    _top_level_item: ($, original) => choice(...original.members, $.c_linkage_guard, $.macro_declaration,
      $.conditional_function_definition, $.conditional_init_declaration),
    _block_item: ($, original) => choice(...original.members, $.c_linkage_guard,
      $.conditional_function_definition, $.conditional_init_declaration),
    _non_case_statement: ($, original) => choice(...original.members, $.table_iteration_statement, $.macro_statement,
      $.ms_asm_block, $.ms_asm_inline),
    ms_call_modifier: ($, original) => choice(...original.members, 'STBV_CDECL', 'CALLBACK', 'WINAPI', 'APIENTRY', 'NTAPI'),
    // This macro expands a declaration/assignment sequence including its
    // final semicolon; its invocation itself has no source terminator.
    macro_statement: $ => seq(choice(
      'STBI__IDCT_1D', 'DRMP3_V0', 'DRMP3_V1', 'DRMP3_V2', 'DRMP3_S0', 'DRMP3_S1', 'DRMP3_S2'
    ), $.argument_list),
    // TABLE_EACH is the workspace's declared for/if loop macro. Its body
    // remains a C statement, including an unbraced increment or assignment.
    table_iteration_statement: $ => seq(
      choice(seq('TABLE_EACH', '(', field('name', $.identifier), ')'),
        seq(choice('STBI__CASE', 'foreach', 'forboth', 'forthree', 'for_each_cell', 'for_each_from'), $.argument_list)),
      field('body', $.statement)
    ),
    ms_asm_block: $ => seq(choice('__asm', '_asm'), '{', repeat($.ms_asm_instruction), '}'),
    ms_asm_inline: $ => seq(choice('__asm', '_asm'), field('opcode', $._ms_asm_opcode),
      optional(field('operands', $.ms_asm_operands)), ';'),
    _ms_asm_opcode: $ => choice($.identifier, alias('int', $.identifier)),
    ms_asm_instruction: $ => seq(
      field('opcode', $._ms_asm_opcode), optional(field('operands', $.ms_asm_operands)),
      token(prec(1, /\r?\n/))
    ),
    ms_asm_operands: $ => seq($.expression, repeat(seq(',', $.expression))),
    declaration: ($, original) => choice(original, seq(
      repeat1($.storage_class_specifier),
      choice(alias($.preproc_if_in_modifier, $.preproc_if), alias($.preproc_ifdef_in_modifier, $.preproc_ifdef)),
      repeat($.type_qualifier), field('type', $.type_specifier), repeat($.type_qualifier),
      upstream.commaSep1(field('declarator', choice($._declaration_declarator, $.init_declarator))), ';'
    )),
    ...preprocOne('_in_modifier', $ => repeat1(choice(
      $.storage_class_specifier, $.type_qualifier, $.attribute_specifier
    )), -1),
    conditional_function_definition: $ => seq(
      choice(alias($.preproc_if_in_function, $.preproc_if), alias($.preproc_ifdef_in_function, $.preproc_ifdef)),
      field('body', alias($.conditional_function_body, $.compound_statement))
    ),
    ...preprocOne('_in_function', $ => seq(repeat(choice(
      $.declaration, $.type_definition, $.preproc_def, $.preproc_function_def, $.preproc_include
    )), $.function_header)),
    function_header: $ => seq($._declaration_specifiers, optional($.ms_call_modifier),
      field('declarator', $._declarator), '{'),
    conditional_function_body: $ => seq(repeat($._block_item), '}'),
    conditional_init_declaration: $ => seq($._declaration_specifiers,
      field('declarator', $._declarator), '=',
      choice(alias($.preproc_if_in_init, $.preproc_if), alias($.preproc_ifdef_in_init, $.preproc_ifdef))
    ),
    ...preprocOne('_in_init', $ => seq($.expression, ';')),
    // A declaration-producing macro supplies its own declaration terminator.
    // Its raw invocation is packaging; no semicolon is invented. PostgreSQL's
    // module macro also passes designated initializer fields directly.
    macro_declaration: $ => seq(
      field('name', $.identifier),
      field('arguments', choice($.argument_list, $.macro_initializer_arguments, $.macro_type_arguments))
    ),
    macro_type_arguments: $ => prec(-1, seq('(', upstream.commaSep1(choice(
      $.expression, $.type_descriptor, $.macro_type_tuple
    )), ')')),
    macro_type_tuple: $ => seq('(', upstream.commaSep1($.type_descriptor), ')'),
    macro_initializer_arguments: $ => seq(
      '(', $.initializer_pair, repeat(seq(',', $.initializer_pair)), optional(','), ')'
    ),
    // Unlike a function call, a variadic macro may end with an empty argument
    // (Tree-sitter's ADVANCE_MAP is one such macro in the workspace headers).
    argument_list: $ => seq('(', optional(seq(
      choice($.expression, $.compound_statement),
      repeat(seq(',', choice($.expression, $.compound_statement))), optional(',')
    )), ')'),
    initializer_list: $ => choice(seq('{', repeat($._initializer_directive), optional(seq(
      $._initializer_value,
      repeat(seq(',', repeat($._initializer_directive), $._initializer_value)), optional(','),
      repeat($._initializer_directive)
    )), '}'), seq('{', repeat($._initializer_directive), repeat1($.macro_declaration),
      repeat($._initializer_directive), '}')),
    _initializer_directive: $ => choice($.preproc_def, $.preproc_function_def, $.preproc_call),
    _initializer_value: $ => choice($.initializer_pair, $.expression, $.initializer_list),
    // Conditional branches are preprocessor fragments: a branch may finish
    // with `else` whose statement follows #endif. Restrict that fragment to
    // preprocessor branches; an ordinary dangling else remains malformed.
    ...upstream.preprocIf('', $ => choice(
      $._block_item, $.macro_declaration, $.preproc_trailing_else, $.preproc_if_head, $.preproc_attribute
    )),
    preproc_attribute: $ => prec(-1, $.attribute_specifier),
    preproc_if_head: $ => prec(-1, seq('if', field('condition', $.parenthesized_expression))),
    preproc_trailing_else: $ => seq(
      'if', field('condition', $.parenthesized_expression),
      field('consequence', $.statement), 'else'
    ),
    preproc_argument_list: $ => seq('(', upstream.commaSep(choice(
      $._preproc_expression, $.system_lib_string
    )), ')'),
    // Comments are removed before macro/directive interpretation. Keep them
    // inside the raw argument token so a comment on a continued line does not
    // terminate a function macro or Watcom pragma halfway through its body.
    preproc_arg: _ => token(prec(-1, repeat1(choice(
      /[^\\\r\n/"']+/,
      /\\(?:\r?\n|[^\r\n])/,
      /\/\*[^*]*\*+([^/*][^*]*\*+)*\//,
      /\/\/(?:[^\\\r\n]|\\(?:\r?\n|[^\r\n]))*/,
      /"(?:[^"\\\r\n]|\\(?:\r?\n|[^\r\n]))*"/,
      /'(?:[^'\\\r\n]|\\(?:\r?\n|[^\r\n]))*'/,
      '/', "'"
    )))),
    gnu_asm_expression: ($, original) => choice(original, seq(
      choice('asm', '__asm__', '__asm'), repeat($.gnu_asm_qualifier), '(',
      choice(alias($.preproc_if_in_asm, $.preproc_if), alias($.preproc_ifdef_in_asm, $.preproc_ifdef)),
      optional($._asm_operands), ')'
    )),
    ...preprocOne('_in_asm', $ => $.asm_body),
    asm_body: $ => seq(
      field('assembly_code', $._string), optional($._asm_operands)
    ),
    _asm_operands: $ => seq(
        field('output_operands', $.gnu_asm_output_operand_list), optional(seq(
          field('input_operands', $.gnu_asm_input_operand_list), optional(seq(
            field('clobbers', $.gnu_asm_clobber_list), optional(field('goto_labels', $.gnu_asm_goto_list))
          ))
        ))
    ),
    // These conditional braces balance after preprocessing, rather than in
    // the raw file. Treat the complete guard as packaging, not an unmatched
    // C block. Declarations between the two guards remain normal AST nodes.
    c_linkage_guard: $ => token(prec(2,
      /#[ \t]*ifdef[ \t]+__cplusplus[ \t]*\r?\n[ \t]*(?:extern[ \t]+"C"[ \t]*\{|})[ \t]*\r?\n[ \t]*#[ \t]*endif/
    ))
  }
});

// The C++ profile inherits the same raw C/preprocessor packaging rules.
module.exports.workspaceConflicts = workspaceConflicts;
module.exports.PREC = upstream.PREC;
module.exports.commaSep = upstream.commaSep;
module.exports.commaSep1 = upstream.commaSep1;
module.exports.preprocIf = upstream.preprocIf;

// Unlike whole-file branches, typed fragments contain one nonempty unit.
// This prevents an empty qualifier branch from competing with an unrelated
// #ifdef around case statements or other complete C constructs.
function preprocOne(suffix, content, precedence = 0) {
  const directive = upstream.preprocessor;
  const alternatives = $ => choice(
    alias($['preproc_else' + suffix], $.preproc_else),
    alias($['preproc_elif' + suffix], $.preproc_elif),
    alias($['preproc_elifdef' + suffix], $.preproc_elifdef)
  );
  return {
    ['preproc_if' + suffix]: $ => prec(precedence, seq(
      directive('if'), field('condition', $._preproc_expression), '\n', content($),
      optional(alternatives($)), directive('endif')
    )),
    ['preproc_ifdef' + suffix]: $ => prec(precedence, seq(
      choice(directive('ifdef'), directive('ifndef')), field('name', $.identifier), content($),
      optional(alternatives($)), directive('endif')
    )),
    ['preproc_else' + suffix]: $ => prec(precedence, seq(directive('else'), content($))),
    ['preproc_elif' + suffix]: $ => prec(precedence, seq(
      directive('elif'), field('condition', $._preproc_expression), '\n', content($), optional(alternatives($))
    )),
    ['preproc_elifdef' + suffix]: $ => prec(precedence, seq(
      choice(directive('elifdef'), directive('elifndef')), field('name', $.identifier), content($), optional(alternatives($))
    ))
  };
}
