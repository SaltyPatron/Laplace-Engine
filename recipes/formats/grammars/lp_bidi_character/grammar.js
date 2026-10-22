// UAX #9 BidiCharacterTest: one input/direction and one aligned resolution.
// The grammar bounds records and groups the three output fields together.
module.exports = grammar({
  name: 'lp_bidi_character',
  extras: $ => [/[ \t]/],
  rules: {
    document: $ => seq(repeat(choice(seq($.record, $._newline), $.comment, $._newline)), optional($.record)),
    record: $ => seq($.input, ';', $.direction, ';', $.resolution, optional(';'), optional($.trailing_comment)),
    input: $ => /[0-9A-Fa-f]{4,6}([ \t]+[0-9A-Fa-f]{4,6})*/,
    direction: $ => /[012]/,
    resolution: $ => seq($.paragraph_level, ';', $.levels, ';', optional($.visual_order)),
    paragraph_level: $ => /[0-9]+/,
    levels: $ => seq(field('level', $.level), repeat(seq($._spaces, field('level', $.level))), optional($._spaces)),
    level: $ => /x|[0-9]+/,
    visual_order: $ => seq(field('index', $.index), repeat(seq($._spaces, field('index', $.index))), optional($._spaces)),
    index: $ => /[0-9]+/,
    comment: $ => /#[^\r\n]*/,
    trailing_comment: $ => /#[^\r\n]*/,
    _spaces: $ => /[ \t]+/,
    _newline: $ => choice('\r\n', '\n', '\r')
  }
});
