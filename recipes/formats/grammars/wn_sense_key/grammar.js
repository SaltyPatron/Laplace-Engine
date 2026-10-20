// WordNet sensekey(5): satellite adjectives carry head_word and head_id.
module.exports = grammar({
  name: 'wn_sense_key',
  extras: $ => [],
  rules: {
    document: $ => $.sense_key,
    sense_key: $ => seq(
      field('lemma', $.lemma), '%',
      choice(
        seq(field('ss_type', alias(/[1-4]/, $.ss_type)), ':',
            field('lex_filenum', $.lex_filenum), ':', field('lex_id', $.hex_id), '::'),
        seq(field('ss_type', alias('5', $.ss_type)), ':',
            field('lex_filenum', $.lex_filenum), ':', field('lex_id', $.hex_id), ':',
            field('head_word', $.head_word), ':', field('head_id', $.hex_id))
      )
    ),
    lemma: $ => /[^%\s]+/,
    lex_filenum: $ => /[0-9]{2}/,
    hex_id: $ => /[0-9a-fA-F]{2}/,
    head_word: $ => /[^:\s]+/
  }
});
