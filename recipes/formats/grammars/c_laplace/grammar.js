// Export annotations are defined by the workspace headers, not C type names.
// Keep their source bytes while parsing the declarations they annotate.
module.exports = grammar(require('./upstream/c.js'), {
  name: 'c_laplace',
  rules: {
    type_qualifier: ($, original) => choice(original, 'LP_API', 'LAPLACE_CHESS_API'),
    _top_level_item: ($, original) => choice(original, $.c_linkage_guard),
    _block_item: ($, original) => choice(original, $.c_linkage_guard),
    // These conditional braces balance after preprocessing, rather than in
    // the raw file. Treat the complete guard as packaging, not an unmatched
    // C block. Declarations between the two guards remain normal AST nodes.
    c_linkage_guard: $ => token(prec(2,
      /#[ \t]*ifdef[ \t]+__cplusplus[ \t]*\r?\n[ \t]*(?:extern[ \t]+"C"[ \t]*\{|})[ \t]*\r?\n[ \t]*#[ \t]*endif/
    ))
  }
});
