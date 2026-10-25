#!/bin/bash
# A source's files may name directories, walked through the ignore files the source names (Laplace-Engine#61): a
# workspace's repositories are read as their authors keep them, and what their .gitignore files leave out (builds,
# dependencies, local secrets) is not read. Patterns follow git: a trailing / names directories, a leading / anchors to
# the ignore file's directory, ! takes a file back, a nested ignore file adds to its parent's, hidden entries are kept out.
#   tests/workspace-code.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/workspace-code.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/workspace-code.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch/work LAPLACE_SRC=$scratch/src
if command -v cygpath >/dev/null 2>&1; then
  export LAPLACE_SRC=$(cygpath -am "$LAPLACE_SRC")
fi
fail(){ echo "workspace-code: $*" >&2; exit 1; }

w=$LAPLACE_SRC/Laplace; mkdir -p "$w"/{build,sub,.hidden,docs/build} "$LAPLACE_SRC/Laplace-MCP"
printf 'build/\n*.log\n!keep.log\n/top.txt\n' > "$w/.gitignore"; printf 'secret*\n' > "$w/sub/.gitignore"
for f in a.c build/x.c sub/a.log keep.log top.txt sub/top.txt .hidden/x.c sub/secret.py sub/b.py docs/build/y.md docs/z.md; do
  printf 'int x;\n' > "$w/$f"
done
printf 'x = 1\n' > "$w/sub/b.py"
printf '# readme\n' > "$LAPLACE_SRC/Laplace-MCP/README.md"

out=$(LAPLACE_TRUNKS=1 LAPLACE_INGEST_ONE=1 "$L" ingest --no-load workspace-code 2>&1 | tr -d '\r') || fail "the ingest failed: $out"
got=$(grep -o "$LAPLACE_SRC/[^ ]*" <<<"$out" | sed "s|$LAPLACE_SRC/||" | LC_ALL=C sort | tr '\n' ' ')
want="Laplace-MCP/README.md Laplace/a.c Laplace/docs/z.md Laplace/keep.log Laplace/sub/b.py Laplace/sub/top.txt "
[ "$got" = "$want" ] || fail "read [$got], not [$want]"
grep -q 'mismatched 0' <<<"$out" || fail "a file did not recompose: $out"
echo "workspace-code: directories walked through their ignore files; ${want// /, }read"

# The shared code grammar records typed declaration subjects and ternary
# semantic claims. Neither a declaration's kind nor its parameters may be lost.
printf 'int doubled(int value){ int local = external(value); return local * 2; }\n' > "$w/a.c"
printf 'int tripled(int value){ return value * 3; }\n' > "$w/a.cpp"
printf 'class Sample { public int Twice(int value){ return value * 2; } }\n' > "$w/a.cs"
printf 'def squared(value):\n    return value * value\n' > "$w/a.py"
printf 'function increment(value: number): number { return value + 1; }\n' > "$w/a.ts"
printf 'CREATE TABLE widgets (id integer);\nSELECT widgets FROM widgets;\n' > "$w/a.sql"
for spec in 'c:function_definition:doubled' 'cpp:function_definition:tripled' 'cs:method_declaration:Twice' \
            'py:function_definition:squared' 'ts:function_declaration:increment' 'sql:create_table:widgets'; do
  IFS=: read -r ext kind name <<< "$spec"
  out=$("$L" ingest --claims-tsv -j 1 -s workspace-code "$w/a.$ext" 2> "$scratch/$ext.log" | tr -d '\r') || fail "$ext claims: $out $(cat "$scratch/$ext.log")"
  grep -Fq "[$kind, " <<< "$out" || fail "$ext lost its typed declaration: $out"
  grep -Fq $'\tdeclared identifier\t'"$name" <<< "$out" || fail "$ext lost its declared identifier: $out"
  grep -Fq $'\treferenced identifier\t' <<< "$out" || fail "$ext did not descend into references: $out"
  grep -q 'mismatched 0' <<< "$out" || fail "$ext did not recompose: $out"
  grep -q 'unresolved obligations: 0 ' <<< "$out" || fail "$ext left unresolved fields: $out"
  if [ "$ext" = c ]; then
    grep -Fq '[parameter_declaration, int value]' <<< "$out" || fail "C parameter declaration: $out"
    grep -Fq $'\tparameter signature\t(int value)' <<< "$out" || fail "C parameter signature: $out"
    grep -Fq $'\treferenced identifier\texternal' <<< "$out" || fail "C call target: $out"
    ! grep -Fq $'\tdeclared identifier\tlocal' <<< "$out" || fail "C local variable renamed the containing function: $out"
    [ "$(grep -c $'\timplemented by\t' <<< "$out")" = 1 ] || fail "C body was attested more than once: $out"
  fi
done
echo 'workspace-code: typed declarations and semantic identifiers retained across C, C++, C#, Python, TypeScript and SQL'

# Raw C packaging includes declaration macros and conditional fragments which
# only become complete C after preprocessing. Preserve both branches and every
# comment/continuation byte while still rejecting malformed ordinary C.
cat > "$w/macros.c" <<'C'
#define DECLARE_SCALAR(name, value) \
    /* the comment belongs to this macro */ \
    int name(void) { return value; }
DECLARE_SCALAR(first, 1) DECLARE_SCALAR(second, 2)
PG_MODULE_MAGIC_EXT(.name = "workspace", .version = "1");
static const float coefficients[] = {
#define SCALE(x) 1.0f/x
  SCALE(3), SCALE(7)
#undef SCALE
};
#if __has_include(<stdint.h>)
#include <stdint.h>
#endif
#if defined(ADDRESS_SANITIZER)
__attribute__((no_sanitize("signed-integer-overflow")))
#endif
LP_API int Loop(void) {
    int n = 0;
    ADVANCE_MAP('x', 1, 'y', 2,);
    STBI__IDCT_1D(n, n, n, n, n, n, n, n)
    TABLE_EACH(node) n++;
    TABLE_EACH(node) { n += node->live; }
    switch (n) {
      case 0: break;
#ifdef EPERM
      case EPERM: return 1;
#endif
      default: break;
    }
    return n;
}
static
#ifdef STBI_THREAD_LOCAL
STBI_THREAD_LOCAL
#endif
const char *thread_message;
int Fallback(int value) {
#if defined(ACCELERATED)
    if (value) { return value; } else
#endif
    { return 0; }
}
int Conditional(int value) {
#if defined(POSITIVE)
    if (value > 0)
#else
    if (value < 0)
#endif
    { return value; }
    return 0;
}
int Assembly(int value) {
    int result;
    __asm__ __volatile__ (
#if defined(WIDE)
        "wide" : [out]"=r"(result) : [in]"r"(value)
#else
        "narrow" : [out]"=r"(result) : [in]"r"(value)
#endif
    );
    return result;
}
int AssemblyCommon(int value) {
    int result;
    __asm__ __volatile__ (
#if defined(WIDE)
        "wide"
#else
        "narrow"
#endif
        : "=r"(result) : "r"(value)
    );
    return result;
}
int AssemblyMS(void) {
    int result;
    __asm {
      mov eax, 1
      cpuid
      mov result, edx
    }
    if (result) __asm int 3;
    return result;
}
static int CALLBACK Callback(void *context) { return context != 0; }
int Varargs(va_list values) {
    return (int)va_arg(values, long long) + va_arg(values, const char *)[0];
}
C
for ending in lf crlf; do
  if [ "$ending" = crlf ]; then awk '{printf "%s\r\n", $0}' "$w/macros.c" > "$w/macros-crlf.c"; input=$w/macros-crlf.c; else input=$w/macros.c; fi
  out=$("$L" ingest --claims-tsv -j 4 -s workspace-code "$input" 2> "$scratch/macros-$ending.log" | tr -d '\r') || fail "C preprocessor $ending: $(cat "$scratch/macros-$ending.log")"
  for name in Loop Fallback Conditional Assembly AssemblyCommon AssemblyMS Callback Varargs; do
    grep -Fq $'\tdeclared identifier\t'"$name" <<< "$out" || fail "C preprocessor lost $name"
  done
  grep -Fq $'\treferenced identifier\teax' <<< "$out" || fail 'MS assembly operands were not inspected'
  grep -q 'mismatched 0' <<< "$out" || fail 'C preprocessor packaging changed'
  grep -q 'unresolved obligations: 0 ' <<< "$out" || fail 'C preprocessor fields unresolved'
done
printf 'int Broken(int x) { if (x) return 1; else }\n' > "$w/broken.c"
if "$L" ingest --no-load -s workspace-code "$w/broken.c" > "$scratch/broken-c.log" 2>&1; then fail 'ordinary dangling C else was accepted'; fi
printf 'int Broken(void) { return 1 }\n' > "$w/broken.c"
if "$L" tree -g c_laplace -n 1000 "$w/broken.c" > "$scratch/broken-tree.log" 2>&1; then fail 'tree inspection reported recovered C as valid'; fi
grep -q '(MISSING ;)' "$scratch/broken-tree.log" || fail 'tree inspection hid missing punctuation'
echo 'workspace-code: C macro declarations, conditional fragments and assembly retained with LF and CRLF; malformed C rejected'

cat > "$w/defaults.cpp" <<'CPP'
#include <vector>
#include <array>
int Braced(std::vector<int> values = {}, std::array<long, 2> pair = {1, 2}) {
    return values.empty() ? (int)pair[0] : values[0];
}
CPP
out=$("$L" ingest --claims-tsv -j 4 -s workspace-code "$w/defaults.cpp" 2> "$scratch/defaults.log" | tr -d '\r') || fail "C++ braced defaults: $(cat "$scratch/defaults.log")"
grep -Fq $'\tdeclared identifier\tBraced' <<< "$out" || fail 'C++ braced defaults lost the declaration'
grep -Fq 'values = {}' <<< "$out" || fail 'C++ empty braced default was lost'
grep -Fq 'pair = {1, 2}' <<< "$out" || fail 'C++ populated braced default was lost'
grep -q 'mismatched 0' <<< "$out" || fail 'C++ braced default packaging changed'
echo 'workspace-code: C++ braced default arguments retained through the shared C packaging profile'

# One declaration may exceed the working window. Framing must keep its grammar
# boundary and the following declaration, with the same products at either size.
{
  printf '// generated declaration\nstatic const unsigned table[] = {'
  for ((i=0;i<512;i++)); do printf '%d,' "$i"; done
  printf '};\nint Following(void) { return table[1]; }\n'
} > "$w/window.c"
for window in 97 8388608; do
  out=$(LAPLACE_RECORD_WINDOW_BYTES=$window "$L" ingest --claims-tsv -j 4 -s workspace-code "$w/window.c" 2> "$scratch/window-$window.log" | tr -d '\r') || fail "C record window $window: $(cat "$scratch/window-$window.log")"
  grep -Fq $'\tdeclared identifier\tFollowing' <<< "$out" || fail 'C record window lost the following function'
  grep -q 'mismatched 0' <<< "$out" || fail 'C record window changed packaging'
  grep '^claim' <<< "$out" | LC_ALL=C sort > "$scratch/window-$window.claims"
done
cmp "$scratch/window-97.claims" "$scratch/window-8388608.claims" || fail 'C claims changed with record window size'
echo 'workspace-code: oversized declarations retain exact packaging and identical claims across record windows'

# The workspace uses import types inside generic result objects and arrays.
cat > "$w/imports.ts" <<'TS'
export function ImportedTypes() {
  return apiGet<{ rows: import('./types').SourceRosterRow[]; value: import('./types').Box<number> }>('/rows');
}
TS
out=$("$L" ingest --claims-tsv -j 4 -s workspace-code "$w/imports.ts" 2> "$scratch/imports.log" | tr -d '\r') || fail "TypeScript import types: $(cat "$scratch/imports.log")"
grep -Fq $'\tdeclared identifier\tImportedTypes' <<< "$out" || fail 'TypeScript import types lost the declaration'
grep -Fq "import('./types').SourceRosterRow[]" <<< "$out" || fail 'TypeScript array import type was lost'
grep -q 'mismatched 0' <<< "$out" || fail 'TypeScript import type packaging changed'
printf "type Broken = import('./types').;\n" > "$w/broken.ts"
if "$L" ingest --no-load -s workspace-code "$w/broken.ts" > "$scratch/broken-ts.log" 2>&1; then fail 'malformed TypeScript import type was accepted'; fi
echo 'workspace-code: TypeScript imported array/generic types retained; malformed import type rejected'

# Valid unsafe code in the installed workspace uses a cast or pointer arithmetic
# as the operand of indirection. Neither form may fall back to unparsed text.
cat > "$w/unsafe.cs" <<'CS'
class PointerProducts {
    public static unsafe int Read(byte[] bytes) {
        fixed (byte* p = bytes) return *(int*)p;
    }
    public static unsafe void Write(int* p, int value) {
        *(p + 1) = value;
        *p = value;
        ++*p;
    }
    public static unsafe int Next(int* p) { return *p + 1; }
    public static unsafe int Contextual(int[] values) {
        int required, async, partial, field;
        fixed (int* p = values) required = ReadValue(p, 1);
        async = required;
        partial = async;
        field = partial;
        int[] selected = [field, partial];
        return selected[0];
    }
    public required int Count { get; init; }
    public async System.Threading.Tasks.Task Load() { await System.Threading.Tasks.Task.Yield(); }
}
CS
for workers in 1 4; do
  out=$("$L" ingest --claims-tsv -j "$workers" -s workspace-code "$w/unsafe.cs" 2> "$scratch/unsafe-$workers.log" | tr -d '\r') || fail "C# unsafe products: $out $(cat "$scratch/unsafe-$workers.log")"
  for name in Read Write Next Contextual Load; do
    grep -Fq $'\tdeclared identifier\t'"$name" <<< "$out" || fail "C# lost $name: $out"
  done
  grep -Fq 'return *(int*)p' <<< "$out" || fail "C# cast indirection was lost: $out"
  grep -Fq '*(p + 1) = value' <<< "$out" || fail "C# arithmetic indirection was lost: $out"
  for name in required async partial field; do
    grep -Fq $'\treferenced identifier\t'"$name" <<< "$out" || fail "C# contextual identifier $name was lost: $out"
  done
  grep -q 'unresolved obligations: 0 ' <<< "$out" || fail "C# unsafe fields unresolved: $out"
  grep '^claim' <<< "$out" | LC_ALL=C sort > "$scratch/unsafe-$workers.claims"
done
cmp "$scratch/unsafe-1.claims" "$scratch/unsafe-4.claims" || fail 'C# unsafe claims changed with worker count'
printf 'class Broken { public unsafe int Read( { return *; } }\n' > "$w/broken.cs"
if "$L" ingest --no-load -s workspace-code "$w/broken.cs" > "$scratch/broken.log" 2>&1; then
  fail 'malformed C# was accepted'
fi
grep -q 'does not parse completely' "$scratch/broken.log" || fail 'malformed C# failed for an unrelated reason'
echo 'workspace-code: unsafe cast/arithmetic indirection retained, worker parity held, malformed input rejected'

# SQL bodies are code selected by the declaration's LANGUAGE, wherever that
# clause appears. The outer parser retains psql packaging; child grammars must
# inspect PL/pgSQL statements and SQL expressions, including escaped literals.
cat > "$w/functions.sql" <<'SQL'
\echo workspace code
CREATE FUNCTION before_body(value integer) RETURNS integer
LANGUAGE sql STABLE SET search_path = public AS $sql$
SELECT dependency(value);
$sql$;
CREATE FUNCTION after_body(value integer) RETURNS integer
AS 'SELECT dependency(value) + 1' LANGUAGE SQL;
CREATE FUNCTION escaped_body(value integer) RETURNS integer
AS E'SELECT \x64ependency(value) + \u0032' LANGUAGE sql;
CREATE FUNCTION procedural_body(value integer) RETURNS integer
LANGUAGE plpgsql SET search_path = public AS $body$
DECLARE result integer; other integer; call text;
BEGIN
  SELECT dependency(value) INTO result;
  SELECT dependency(value), value INTO STRICT result, other FROM widgets;
  INSERT INTO widgets(id) VALUES (value) RETURNING id INTO result;
  call := 'dependency';
  IF call LIKE 'dep\_%' ESCAPE '\' THEN
    RAISE NOTICE 'first line '
      'second line';
  END IF;
  IF result IS NULL THEN RETURN 0; END IF;
  RETURN result + dependency(value);
END
$body$;
DO $body$ BEGIN PERFORM dependency(1); END $body$;
CREATE FUNCTION external_body(value integer) RETURNS integer
AS 'native_library', 'entry_point' LANGUAGE c;
SQL
for workers in 1 4; do
  out=$("$L" ingest --claims-tsv -j "$workers" -s workspace-code "$w/functions.sql" 2> "$scratch/sql-$workers.log" | tr -d '\r') || fail "SQL bodies: $out $(cat "$scratch/sql-$workers.log")"
  for name in before_body after_body escaped_body procedural_body external_body; do
    grep -Fq $'\tdeclared identifier\t'"$name" <<< "$out" || fail "SQL lost $name"
  done
  grep -Fq $'\treferenced identifier\tdependency' <<< "$out" || fail 'SQL body call target was not inspected'
  grep -Fq $'\treferenced identifier\tresult' <<< "$out" || fail 'PL/pgSQL body expressions were not inspected'
  grep -Fq $'\timplemented by\tSELECT dependency(value) + 2' <<< "$out" || fail 'SQL escaped body was not decoded'
  grep -Fq $'\timplemented by\tentry_point' <<< "$out" || fail 'Native entry point was not retained as content'
  grep -q 'mismatched 0' <<< "$out" || fail 'SQL packaging did not recompose exactly'
  grep -q 'unresolved obligations: 0 ' <<< "$out" || fail 'SQL left unresolved fields'
  grep '^claim' <<< "$out" | LC_ALL=C sort > "$scratch/sql-$workers.claims"
done
cmp "$scratch/sql-1.claims" "$scratch/sql-4.claims" || fail 'SQL claims changed with worker count'
for body in 'SELECT FROM' 'BEGIN RETURN +; END' ''; do
  if [[ "$body" = SELECT* ]]; then language=sql; else language=plpgsql; fi
  printf 'CREATE FUNCTION broken() RETURNS integer LANGUAGE %s AS $$%s$$;\n' "$language" "$body" > "$w/broken.sql"
  if "$L" ingest --no-load -s workspace-code "$w/broken.sql" > "$scratch/broken-sql.log" 2>&1; then
    fail "malformed $language body was accepted"
  fi
  grep -q 'does not parse completely' "$scratch/broken-sql.log" || fail 'malformed SQL failed for an unrelated reason'
done
echo 'workspace-code: SQL/PLpgSQL bodies inspected, literals decoded, worker parity held, malformed and empty bodies rejected'

# The outer psql/template grammar admits the function annotations declared in
# Laplace's sqldefines.h.in, without changing plain SQL body admission.
for attribute in LAPLACE_IMMUTABLE_STRICT LAPLACE_IMMUTABLE LAPLACE_STABLE_STRICT LAPLACE_VOLATILE_STRICT; do
  printf 'CREATE FUNCTION templated(value integer) RETURNS integer LANGUAGE sql %s AS $$SELECT value$$;\n' "$attribute" > "$w/template.sql"
  out=$("$L" ingest --claims-tsv -j 1 -s workspace-code "$w/template.sql" 2> "$scratch/template.log" | tr -d '\r') || fail "SQL template $attribute: $(cat "$scratch/template.log")"
  grep -Fq $'\tdeclared identifier\ttemplated' <<< "$out" || fail "SQL template lost its declaration: $attribute"
  grep -q 'mismatched 0' <<< "$out" || fail "SQL template packaging changed: $attribute"
done
echo 'workspace-code: declared SQL template annotations retained and their SQL bodies inspected'
