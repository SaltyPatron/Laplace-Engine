// ConceptNet concept IRIs; the optional qualifier retains its complete
// source notation. URL endpoints remain packaging for ExternalURL records.
module.exports = grammar({
  name: 'concept_uri',
  extras: $ => [],
  rules: {
    document: $ => choice($.concept, $.external),
    concept: $ => seq('/c/', $.lexicalization,
      optional(seq('/', $.pos, optional(seq('/', $.qualifier))))),
    lexicalization: $ => seq($.language, '/', $.lemma),
    language: $ => /[a-zA-Z][a-zA-Z0-9_-]*/,
    lemma: $ => /[^\/\t\r\n]+/,
    pos: $ => /[nvasr]/,
    qualifier: $ => /[^\t\r\n]+/,
    external: $ => /https?:\/\/[^\t\r\n]+/
  }
});
