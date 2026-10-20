// CoNLL-U packaging: one native token per sentence, retaining its last
// physical newline for the worker's included-range CoNLL-U parser.
module.exports = grammar({
  name: 'lp_conllu_records',
  extras: $ => [],
  rules: {
    document: $ => seq(optional($.separator), optional(seq($.record,
      repeat(seq($.separator, $.record)), optional($.separator)))),
    record: $ => token(/([^\r\n]+\r?\n)+/),
    separator: $ => /(?:\r?\n)+/
  }
});
