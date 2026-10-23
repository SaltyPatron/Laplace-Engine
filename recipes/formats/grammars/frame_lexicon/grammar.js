// Frame-scoped lexical alignments shared by WordFrameNet and eXtendedWFN.
// Blank lines are packaging inside a section; only the next header ends it.
module.exports = grammar({
  name: 'frame_lexicon',
  extras: $ => [],
  conflicts: $ => [[$.lemma]],
  rules: {
    document: $ => repeat(choice($.frame, $.blank_line)),
    frame: $ => prec.right(seq($.header, repeat(choice($.entry, $.blank_line)))),
    header: $ => seq('Frame:', $._space, field('name', $.frame_name), $._newline),
    frame_name: $ => /[^ \t\r\n]+/,
    entry: $ => prec.right(seq(
      field('lemma', $.lemma),
      choice(
        seq($._space, field('pos', $.pos), $._space, field('reference', $.reference)),
        seq('|', field('pos', $.pos), $._space, field('reference', $.reference),
          $._space, field('annotation', $.source_annotation))
      ),
      optional(seq($._space, field('definition', $.definition))),
      optional($._newline)
    )),
    lemma: $ => seq($.word, repeat(seq($._space, $.word))),
    word: $ => choice($.pos, /[^ \t\r\n|]+/),
    pos: $ => choice('n', 'v', 'a', 's', 'r'),
    // A complete offset/category ends the lexical head. Prefer that branch
    // over continuing a multiword lemma through the following definition.
    reference: $ => prec.dynamic(1, seq(field('offset', $.offset), '-', field('category', $.category))),
    offset: $ => token(prec(2, /[0-9]{8}/)),
    category: $ => /[nvasr]/,
    source_annotation: $ => /[0-9]+/,
    definition: $ => /[^\r\n]+/,
    blank_line: $ => /[ \t]*\r?\n/,
    _space: $ => /[ \t]+/,
    _newline: $ => /\r?\n/
  }
});
