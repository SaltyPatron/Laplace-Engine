// Brown's distributed tagged text: paragraphs separated by blank lines,
// sentences by newlines, and whitespace-separated word/tag pairs. The final
// slash is the separator, so and/or/CC retains and/or as its word.
module.exports = grammar({
  name: 'brown_tagged',
  extras: $ => [/[ \t]+/],
  conflicts: $ => [[$.word]],
  rules: {
    document: $ => repeat(choice($.paragraph, $._newline)),
    paragraph: $ => prec.right(repeat1($.sentence)),
    sentence: $ => prec.right(seq(repeat1($.token), optional($._newline))),
    token: $ => seq(field('word', $.word), '/', field('tag', $.segment)),
    word: $ => seq($.segment, repeat(seq('/', $.segment))),
    segment: $ => /[^\s/]+/,
    _newline: $ => /\r?\n/
  }
});
