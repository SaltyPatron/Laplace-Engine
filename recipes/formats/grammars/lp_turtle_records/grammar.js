// Turtle packaging boundaries. Strings, IRIs, decimal points and escaped name
// punctuation are lexical atoms, so only a statement's unquoted dot ends it.
// Each worker validates the full RDF syntax with tree-sitter-turtle; this
// grammar retains one token per statement instead of a serial product AST.
const uchar = /\\u[0-9a-fA-F]{4}|\\U[0-9a-fA-F]{8}/;
const escape = choice(/\\[tbnrf\\"']/, uchar);
const iri = seq('<', repeat(choice(/[^<>"{}|^`\\\x00-\x20]/, uchar)), '>');
const quoted = quote => seq(quote, repeat(choice(
  quote === '"' ? /[^"\\\r\n]/ : /[^'\\\r\n]/, escape)), quote);
const long = quote => seq(quote.repeat(3), repeat(seq(
  optional(choice(quote, quote.repeat(2))),
  choice(quote === '"' ? /[^"\\]/ : /[^'\\]/, escape))), quote.repeat(3));
const space = /[ \t\r\n]+/;
const comment = /#[^\r\n]*/;
const namePart = choice(/[^\s<>"'#.\[\](){};,\\]+/, /\\[_~.\-!$&'()*+,;=\/?#@%]/, uchar);
const name = seq(namePart, repeat(choice(namePart, seq('.', namePart))));
const number = /[+-]?(?:[0-9]+\.[0-9]*(?:[eE][+-]?[0-9]+)|\.[0-9]+(?:[eE][+-]?[0-9]+)?|[0-9]+\.[0-9]+|[0-9]+[eE][+-]?[0-9]+)/;
const atom = choice(iri, long('"'), long("'"), quoted('"'), quoted("'"), number, name, /[;,\[\]()]/);

// Preserve the pinned upstream grammar for directives and TriG graphs. Only
// ordinary top-level triples use the compact token; graph triples retain the
// upstream syntax and are separately selected by the outer record mask.
module.exports = grammar(require('./product'), {
  name: 'lp_turtle_records',
  rules: {
    document: $ => repeat(choice($.record, $.directive, $.graph)),
    record: $ => token(seq(atom, repeat(choice(atom, space, comment)), '.'))
  }
});
