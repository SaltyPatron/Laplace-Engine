// UTS #51 keyboard/display palettes. Headers scope rows across blank lines;
// per-row annotations contain product fields, not disposable comments.
module.exports = grammar({
  name: 'emoji_palette',
  extras: $ => [],
  conflicts: $ => [[$.codepoints]],
  rules: {
    document: $ => repeat(choice($.palette, $.subgroup, $.entry, $.comment, $.blank_line)),
    palette: $ => prec.right(seq($.group_header,
      repeat(choice($.subgroup, $.entry, $.comment, $.blank_line)))),
    subgroup: $ => prec.right(seq($.subgroup_header,
      repeat(choice($.entry, $.comment, $.blank_line)))),
    group_header: $ => seq(token(prec(2, /#[ \t]*group:/)), $._space, $.group_name, $._newline),
    subgroup_header: $ => seq(token(prec(2, /#[ \t]*subgroup:/)), $._space, $.subgroup_name, $._newline),
    group_name: $ => /[^\r\n]+/,
    subgroup_name: $ => /[^\r\n]+/,
    entry: $ => seq($.codepoints, optional($._space), ';', optional($._space), $.status,
      $._space, '#', $._space, $.display, $._space, $.version, $._space, $.emoji_name,
      optional($._newline)),
    codepoints: $ => seq($.hex, repeat(seq($._space, $.hex))),
    hex: $ => /[0-9A-F]{4,6}/,
    status: $ => choice('component', 'fully-qualified', 'minimally-qualified', 'unqualified'),
    display: $ => /[^ \t\r\n]+/,
    version: $ => /E[0-9]+\.[0-9]+/,
    emoji_name: $ => /[^\r\n]+/,
    comment: $ => seq(token(prec(-1, /#[^\r\n]*/)), optional($._newline)),
    blank_line: $ => /[ \t]*\r?\n/,
    _space: $ => /[ \t]+/,
    _newline: $ => /\r?\n/
  }
});
