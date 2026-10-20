// Container syntax shared by the compiled tier layouts. Whitespace is data;
// only the selected separator and unquoted record endings split fields.
module.exports = function delimited(name, separator, quoted, escaped, padded = false, remark = '') {
  const inClass = c => c.replace(/[\\\]\^-]/g, '\\$&');
  const excluded = inClass(separator + remark) + '\\r\\n' + (escaped ? '\\\\' : '');
  const ordinary = new RegExp('[^' + excluded + ']');
  const initial = new RegExp('[^' + excluded + (quoted ? '"' : '') + ']');
  const escape = seq('\\', optional(/[\s\S]/));
  const first = escaped ? choice(escape, initial) : initial;
  const rest = escaped ? choice(escape, ordinary) : ordinary;
  const raw = optional(seq(first, repeat(rest)));
  const quotedCharacter = escaped ? choice(escape, '""', /[^"\\]/) : choice('""', /[^"]/);
  const quotes = seq('"', repeat(quotedCharacter), '"');
  const wrapped = padded ? seq(optional(/[ \t]+/), quotes, optional(/[ \t]+/)) : quotes;
  return grammar({
    name,
    extras: $ => [],
    rules: {
      document: $ => seq(repeat(seq($.row, choice('\r\n', '\n', '\r'))), optional($.row)),
      row: $ => remark ? seq($.field, repeat(seq(separator, $.field)), optional($.remark)) : seq($.field, repeat(seq(separator, $.field))),
      field: $ => token(quoted ? choice(prec(1, wrapped), raw) : raw),
      ...(remark ? {
        remark: $ => seq(remark, field('value', $.remark_text)),
        remark_text: $ => token(/[^\r\n]*/)
      } : {})
    }
  });
};
