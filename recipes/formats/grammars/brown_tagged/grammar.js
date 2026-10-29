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
    token: $ => seq(field('word', $.word), token.immediate('/'),
      field('tag', alias(token.immediate(/[^\s/]+/), $.segment))),
    // Slashes can lead or trail a word as well as occur inside it: //in,
    // /l//nn and AAb//nn spell '/', '/l/' and 'AAb/' respectively.
    word: $ => seq(choice($.segment, '/'),
      repeat(choice(token.immediate('/'), token.immediate(/[^\s/]+/)))),
    segment: $ => /[^\s/]+/,
    _newline: $ => /\r?\n/
  }
});
