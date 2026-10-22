// A move-text fragment has no game header or termination marker. Reuse the
// PGN grammar's SAN, variation and annotation rules without inventing a result.
module.exports = grammar(require('../pgn/grammar.js'), {
  name: 'pgn_moves',
  rules: { document: $ => repeat($._element) }
});
