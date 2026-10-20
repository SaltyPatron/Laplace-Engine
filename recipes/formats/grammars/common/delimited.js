// Container syntax shared by the compiled tier layouts. Whitespace is data;
// only the selected separator and unquoted record endings split fields.
module.exports = function delimited(name, separator, quoted, escaped, padded = false, remark = '') {
  const inClass = c => c.replace(/[\\\]\^-]/g, '\\$&');
  const excluded = inClass(separator + remark) + '\\r\\n' + (escaped ? '\\\\' : '');
  const ordinary = new RegExp('[^' + excluded + (quoted ? '"' : '') + ']');
  const initial = new RegExp('[^' + excluded + (quoted ? '"' : '') + ']');
  const escape = seq('\\', optional(/[\s\S]/));
  const first = escaped ? choice(escape, initial) : initial;
  const rest = escaped ? choice(escape, ordinary) : ordinary;
  const raw = seq(first, repeat(rest));
  const quotedCharacter = escaped ? choice(escape, '""', /[^"\\]/) : choice('""', /[^"]/);
  const quotes = seq('"', repeat(quotedCharacter), '"');
  // Padding must never consume the container's separator (notably TSV tabs).
  const padding = separator === '\t' ? /[ ]+/ : /[ \t]+/;
  const wrapped = padded ? seq(optional(padding), quotes, optional(padding)) : quotes;
  return grammar({
    name,
    extras: $ => [],
    externals: $ => [$._empty_field, $._separator, $._record_end],
    rules: {
      document: $ => seq(repeat($.row), optional(alias($._last_row, $.row))),
      row: $ => seq($._fields, $._record_end),
      _last_row: $ => $._fields,
      _fields: $ => remark ? seq($.field, repeat(seq($._separator, $.field)), optional($.remark)) : seq($.field, repeat(seq($._separator, $.field))),
      field: $ => choice(token(quoted ? choice(prec(1, wrapped), raw) : raw), $._empty_field),
      ...(remark ? {
        remark: $ => seq(remark, optional(field('value', $.remark_text))),
        remark_text: $ => token(/[^\r\n]+/)
      } : {})
    }
  });
};
