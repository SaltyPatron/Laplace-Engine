// XML packaging events. The compiled scope walker matches opening/closing
// names and document placement across byte windows; Tree-sitter owns lexical
// boundaries, attributes, declarations, references and injected DTD syntax.
// XML/DTD productions derive from tree-sitter-xml (MIT), see ../common/xml.LICENSE.
const c = require('../common/xml');
const O = optional;

module.exports = grammar({
  name: 'lp_xml_scopes',
  externals: $ => [$.PITarget, $._pi_content, $.Comment, $.CharData, $.CData,
    'xml-model', 'xml-stylesheet'],
  extras: _ => [],
  supertypes: $ => [$._markupdecl, $._AttType, $._EnumeratedType, $._EntityDecl, $._Reference],
  conflicts: $ => [[$.AttlistDecl, $.AttDef]],
  word: $ => $.Name,
  rules: {
    document: $ => seq(O($.XMLDecl), repeat($._misc), O($.doctypedecl),
      repeat(choice($.STag, $.ETag, $.EmptyElemTag, $.CharData, $._Reference,
        $.CDSect, $.PI, $.StyleSheetPI, $.XmlModelPI, $.Comment))),
    _misc: $ => prec(1, choice($.CharData, $.PI, $.StyleSheetPI, $.XmlModelPI, $.Comment)),
    STag: $ => seq('<', $.Name, c.rseq($._S, $.Attribute), O($._S), '>'),
    ETag: $ => seq('</', $.Name, O($._S), '>'),
    EmptyElemTag: $ => seq('<', $.Name, c.rseq($._S, $.Attribute), O($._S), '/>'),
    Attribute: $ => seq($.Name, $._Eq, $.AttValue),
    XMLDecl: $ => seq('<?', 'xml', $._VersionInfo, O($._EncodingDecl), O($._SDDecl), O($._S), '?>'),
    _SDDecl: $ => seq($._S, 'standalone', $._Eq, c.str(choice('yes', 'no'))),
    doctypedecl: $ => seq('<!', 'DOCTYPE', $._S, $.Name, O(seq($._S, $.ExternalID)), O($._S),
      O(seq('[', O(choice($._intSubset, $._S)), ']', O($._S))), '>'),
    _intSubset: $ => c.rseq1(O($._S), $._markupdecl, $._DeclSep),
    CDSect: $ => seq($.CDStart, repeat($.CData), ']]>'),
    CDStart: _ => '<![CDATA[',
    StyleSheetPI: $ => seq('<?', 'xml-stylesheet', c.rseq($._S, $.PseudoAtt), O($._S), '?>'),
    XmlModelPI: $ => seq('<?', 'xml-model', c.rseq($._S, $.PseudoAtt), O($._S), '?>'),
    PseudoAtt: $ => seq($.Name, $._Eq, $.PseudoAttValue),
    PseudoAttValue: $ => choice(c.att_value($, '"'), c.att_value($, "'")),
    ...c.rules,
    PI: $ => seq('<?', $.PITarget, O(seq($._S, O($._pi_content))), '?>')
  }
});
