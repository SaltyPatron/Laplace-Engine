// Inner part syntax. The semantic grammar selects separator symbol IDs;
// other punctuation remains in the original part's span, without rescanning.
module.exports = grammar({
  name: 'lp_parts',
  extras: $ => [],
  rules: {
    document: $ => repeat(choice($.text, $.slash, $.pipe, $.comma, $.semicolon, $.colon,
      $.equals, $.percent, $.greater, $.hyphen, $.dot, $.star, $.spacing, $.tab, $.newline, $.carriage)),
    text: $ => /[^/|,;:=%>.*\- \t\r\n]+/,
    slash: $ => '/', pipe: $ => '|', comma: $ => ',', semicolon: $ => ';', colon: $ => ':',
    equals: $ => '=', percent: $ => '%', greater: $ => '>', hyphen: $ => '-', dot: $ => '.',
    star: $ => '*',
    spacing: $ => ' ',
    tab: $ => '\t',
    newline: $ => '\n',
    carriage: $ => '\r'
  }
});
