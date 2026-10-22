// WordNet's documented single-letter syntactic categories. The enclosing
// field retains the original letter while recipes bind its universal class.
module.exports = grammar({
  name: 'wordnet_category',
  extras: $ => [],
  rules: {
    document: $ => $.category,
    category: $ => /[nvasr]/
  }
});
