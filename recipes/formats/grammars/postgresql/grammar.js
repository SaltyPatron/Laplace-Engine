// PostgreSQL extension sources are psql scripts. Extend the pinned SQL grammar
// with psql packaging and the two-string C linkage form of CREATE FUNCTION.
import sql from './upstream/sql.js';

export default grammar(sql, {
  name: 'postgresql',
  conflicts: ($, original) => original.concat([[$._check_constraint]]),
  rules: {
    program: ($, original) => repeat(choice(
      $.psql_command,
      seq(choice($.transaction, $.statement, $.block, $.do_statement), ';')
    )),
    do_statement: $ => seq($.keyword_do, optional($.function_language),
      field('body', alias($._literal_string, $.literal)), optional($.function_language)),
    invocation: ($, original) => choice(original, prec(1, seq(
      $.object_reference, '(', $._expression,
      optional(seq($.keyword_from, $._expression)),
      $.keyword_for, $._expression, ')'
    ))),
    alter_column: ($, original) => choice(original, seq(
      $.keyword_alter, optional($.keyword_column), field('name', $.identifier),
      optional(seq($.keyword_set, $.keyword_data)), $.keyword_type,
      field('type', $._type), $.keyword_using, field('conversion', $._expression)
    )),
    _check_constraint: $ => prec.right(seq(
      optional(seq($.keyword_constraint, $.identifier)),
      $.keyword_check, '(', $._expression, ')'
    )),
    psql_command: $ => token(seq('\\', /[^\r\n]*/)),
    function_body: ($, original) => choice(original, seq(
      $.keyword_as,
      field('library', alias($._single_quote_string, $.literal)),
      ',',
      field('entrypoint', alias($._single_quote_string, $.literal))
    ))
  }
});
