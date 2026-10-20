// PGN packaging. SAN legality belongs to the chess position engine; this
// grammar identifies games, tags, move text, annotations and nested RAVs.
module.exports = grammar({
  name: 'pgn',
  extras: $ => [/\s/, /\uFEFF/, $.comment, $.line_comment, $.escape_line],
  rules: {
    document: $ => repeat($.game),
    game: $ => seq(repeat($.tag_pair), repeat($._element), $.result),
    tag_pair: $ => seq('[', field('name', $.tag_name), field('value', $.tag_string), ']'),
    tag_name: $ => /[A-Za-z][A-Za-z0-9_]*/,
    tag_string: $ => token(seq('"', repeat(choice(/[^"\\\r\n]/, /\\[^\r\n]/)), '"')),
    _element: $ => choice($.move_number, $.san, $.nag, $.annotation, $.variation, $.en_passant),
    move_number: $ => token(seq(/[0-9]+/, choice('...', '.'))),
    san: $ => token(choice(
      /(?:O-O(?:-O)?|0-0(?:-0)?)[+#]?[!?]{0,2}/,
      /[KQRBN]?[a-h]?[1-8]?x?[a-h][1-8](?:=[QRBN])?[+#]?[!?]{0,2}/,
      '--'
    )),
    nag: $ => /\$[0-9]+/,
    annotation: $ => /[!?]{1,2}/,
    en_passant: $ => 'e.p.',
    variation: $ => seq('(', repeat($._element), ')'),
    result: $ => choice('1-0', '0-1', '1/2-1/2', '*'),
    comment: $ => token(seq('{', /[^}]*/, '}')),
    line_comment: $ => /;[^\r\n]*/,
    escape_line: $ => /%[^\r\n]*/
  }
});
