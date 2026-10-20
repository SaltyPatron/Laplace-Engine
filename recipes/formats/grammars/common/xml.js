/**
 * @file Reusable constructs
 * @author ObserverOfTime
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

/**
 * @param {"'" | ''} q
 * @see {@link https://www.w3.org/TR/xml/#NT-PubidChar}
 */
const pubid_char = (q) =>
  new RegExp(`[ \\r\\na-zA-Z0-9\\-${q}()+,./:=?;!*#@$_%]+`);

/**
 * @param {GrammarSymbols<any>} $
 * @param {'"' | "'"} q
 */
const att_value = ($, q) =>
  seq(
    q,
    field(
      'content',
      repeat(choice(
        new RegExp(`[^<&${q}]`),
        $._Reference
      ))
    ),
    q
  );

/**
 * @param {GrammarSymbols<any>} $
 * @param {'"' | "'"} q
 */
const entity_value = ($, q) =>
  seq(
    q,
    field(
      'content',
      repeat(choice(
        new RegExp(`[^<%&${q}]`),
        $.PEReference,
        $._Reference
      ))
    ),
    q
  );


/** @param {RuleOrLiteral[]} choices */
const str = (...choices) =>
  choice(
    seq("'", ...choices, "'"),
    seq('"', ...choices, '"')
  );

/**
 * @param {GrammarSymbols<any>} $
 * @param {RuleOrLiteral[]} choices
 */
const ref = ($, ...choices) =>
  choice(...choices, $.PEReference);

/** @param {RuleOrLiteral[]} rules */
const rseq = (...rules) => repeat(seq(...rules));

/** @param {RuleOrLiteral[]} rules */
const rseq1 = (...rules) => repeat1(seq(...rules));

const O = optional;

// XML 1.0 Fifth Edition, productions [4] and [4a]. These ranges are
// independent of the host locale and include supplementary-plane names.
const NAME_START = ':A-Z_a-z\\u00C0-\\u00D6\\u00D8-\\u00F6\\u00F8-\\u02FF' +
  '\\u0370-\\u037D\\u037F-\\u1FFF\\u200C-\\u200D\\u2070-\\u218F' +
  '\\u2C00-\\u2FEF\\u3001-\\uD7FF\\uF900-\\uFDCF\\uFDF0-\\uFFFD\\u{10000}-\\u{EFFFF}';
const NAME_CHAR = NAME_START + '\\-.0-9\\u00B7\\u0300-\\u036F\\u203F-\\u2040';
const NAME = new RegExp('[' + NAME_START + '][' + NAME_CHAR + ']*', 'u');
const NMTOKEN = new RegExp('[' + NAME_CHAR + ']+', 'u');

/** @type {Record<string, ($: GrammarSymbols<any>) => RuleOrLiteral>} */
const rules = {
    _markupdecl: $ => choice(
      $.elementdecl,
      $.AttlistDecl,
      $._EntityDecl,
      $.NotationDecl,
      $.PI,
      $.Comment
    ),

    _DeclSep: $ => choice($.PEReference, $._S),

    elementdecl: $ => seq(
      '<!',
      'ELEMENT',
      $._S,
      ref($, $.Name),
      $._S,
      $.contentspec,
      O($._S),
      '>'
    ),

    contentspec: $ => choice(
      'EMPTY',
      'ANY',
      $.Mixed,
      $.children,
      $.PEReference
    ),

    Mixed: $ => choice(
      seq(
        '(',
        O($._S),
        ref($, '#PCDATA'),
        rseq(
          O($._S),
          '|',
          O($._S),
          ref($, $.Name),
        ),
        O($._S),
        rseq(
          $.PEReference,
          O($._S)
        ),
        ')',
        '*'
      ),
      prec(-1, seq(
        '(',
        O($._S),
        ref($, '#PCDATA'),
        O($._S),
        rseq(
          $.PEReference,
          O($._S)
        ),
        ')'
      ))
    ),

    children: $ => prec(1, seq(
      $._choice,
      O(choice('?', '*', '+'))
    )),

    _cp: $ => prec.left(seq(
      ref($, $.Name, $._choice),
      O(choice('?', '*', '+'))
    )),

    _choice: $ => seq(
      '(',
      O($._S),
      $._cp,
      rseq(
        O($._S),
        choice('|', ','),
        O($._S),
        $._cp
      ),
      rseq(
        O($._S),
        $.PEReference,
      ),
      O($._S),
      ')'
    ),

    AttlistDecl: $ => seq(
      '<!',
      'ATTLIST',
      $._S,
      ref($, $.Name),
      repeat(choice(
        $.AttDef,
        seq($._S, $.PEReference)
      )),
      O($._S),
      '>'
    ),

    AttDef: $ => prec.right(seq(
      $._S,
      ref($, $.Name),
      $._S,
      $._AttType,
      O(seq($._S, $.DefaultDecl))
    )),

    _AttType: $ => choice(
      $.StringType,
      $.TokenizedType,
      $._EnumeratedType,
      $.PEReference
    ),

    StringType: _ => 'CDATA',

    TokenizedType: _ => token(choice(
      'ID',
      'IDREF',
      'IDREFS',
      'ENTITY',
      'ENTITIES',
      'NMTOKEN',
      'NMTOKENS',
    )),

    _EnumeratedType: $ => choice(
      $.NotationType,
      $.Enumeration
    ),

    NotationType: $ => seq(
      'NOTATION',
      $._S,
      '(',
      O($._S),
      ref($, $.Name),
      rseq(
        O($._S),
        '|',
        O($._S)
      ),
      ref($, $.Name),
      O($._S),
      ')'
    ),

    Enumeration: $ => seq(
      '(',
      O($._S),
      $.Nmtoken,
      rseq(
        O($._S),
        '|',
        O($._S),
        $.Nmtoken
      ),
      O($._S),
      ')'
    ),

    DefaultDecl: $ => choice(
      '#REQUIRED',
      '#IMPLIED',
      seq(
        O(seq('#FIXED', $._S)),
        $.AttValue
      ),
      $.PEReference
    ),

    _EntityDecl: $ => choice(
      $.GEDecl,
      $.PEDecl
    ),

    GEDecl: $ => seq(
      '<!',
      'ENTITY',
      $._S,
      ref($, $.Name),
      $._S,
      choice(
        $.EntityValue,
        seq(
          $.ExternalID,
          O($.NDataDecl)
        )
      ),
      O($._S),
      '>'
    ),

    PEDecl: $ => seq(
      '<!',
      'ENTITY',
      $._S,
      '%',
      $._S,
      $.Name,
      $._S,
      choice(
        $.EntityValue,
        $.ExternalID
      ),
      O($._S),
      '>'
    ),

    EntityValue: $ => choice(
      entity_value($, '"'),
      entity_value($, "'")
    ),

    NDataDecl: $ => seq($._S, 'NDATA', $._S, ref($, $.Name)),

    NotationDecl: $ => seq(
      '<!',
      'NOTATION',
      $._S,
      ref($, $.Name),
      $._S,
      choice($.ExternalID, $.PublicID),
      O($._S),
      '>'
    ),

    PEReference: $ => seq('%', $.Name, ';'),

    _S: _ => /[ \t\r\n]+/,

    Name: _ => NAME,

    Nmtoken: _ => NMTOKEN,

    _Reference: $ => choice($.EntityRef, $.CharRef),

    EntityRef: $ => seq('&', $.Name, ';'),

    CharRef: _ => choice(
      seq('&#', /[0-9]+/, ';'),
      seq('&#x', /[0-9a-fA-F]+/, ';')
    ),

    AttValue: $ => choice(
      att_value($, '"'),
      att_value($, "'")
    ),

    ExternalID: $ => choice(
      seq('SYSTEM', $._S, $.SystemLiteral),
      seq('PUBLIC', $._S, $.PubidLiteral, $._S, $.SystemLiteral)
    ),

    PublicID: $ => prec.right(
      seq(ref($, 'PUBLIC'), $._S, $.PubidLiteral)
    ),

    SystemLiteral: $ => choice(
      seq('"', optional(alias(/[^"]+/, $.URI)), '"'),
      seq("'", optional(alias(/[^']+/, $.URI)), "'")
    ),

    PubidLiteral: _ => choice(
      seq('"', optional(pubid_char("'")), '"'),
      seq("'", optional(pubid_char('')), "'")
    ),

    _VersionInfo: $ => seq(
      $._S,
      'version',
      $._Eq,
      str($.VersionNum)
    ),

    VersionNum: _ => /1\.[0-9]+/,

    _EncodingDecl: $ => seq(
      $._S,
      'encoding',
      $._Eq,
      str($.EncName)
    ),

    EncName: _ => /[A-Za-z][A-Za-z0-9._\-]*/,

    PI: $ => seq(
      '<?',
      $.PITarget,
      O(seq($._S, $._pi_content)),
      '?>'
    ),

    _Eq: $ => seq(O($._S), '=', O($._S))
}

// XML/DTD rules from tree-sitter-xml 5000ae8f22d11fbe93939b05c1e37cf21117162d.
// MIT license: xml.LICENSE. CommonJS module for Engine-owned grammars.
module.exports = { pubid_char, att_value, entity_value, str, ref, rseq, rseq1, rules };
