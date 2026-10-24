#!/usr/bin/env bash
# Compile prepared external parsers and Engine-owned grammar definitions.
# Generated sources stay in the declared build tree; libraries are replaced by rename.
set -euo pipefail
SRC=${1:-${LAPLACE_GRAMMAR_SRC:?Declare grammar sources}}
OUT=${2:-${LAPLACE_GRAMMARS:?Declare grammar output}}
SRC=$(cd "$SRC" && pwd)
EXTERNAL_SOURCES=${LAPLACE_GRAMMAR_SRC:-}
if [[ -z "$EXTERNAL_SOURCES" && -n "${LAPLACE_EXTERNAL:-}" ]]; then
  EXTERNAL_SOURCES="$LAPLACE_EXTERNAL/tree-sitter-grammars"
fi
mkdir -p "$OUT"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) win=1; ext=dll; CC=${CC:-icx-cc} ;;
  *) win=; ext=so; CC=${CC:-icx} ;;
esac

compile() {
  local parser=$1 scanner_root=$2 common=$3 name binary fresh
  name=$(sed -n 's/.*TSLanguage \*tree_sitter_\([A-Za-z0-9_]*\)(void).*/\1/p' "$parser" | head -1)
  [[ -n "$name" ]] || { echo "No exported language in $parser" >&2; return 1; }
  binary="$OUT/libtree-sitter-$name.$ext"
  fresh="$binary.$$.tmp"
  local -a sources=("$parser") flags=(-O2 -shared -w "-I$(dirname "$parser")" "-I$scanner_root" "-I$common")
  for scanner in scanner.c scanner.cc scanner.cpp; do
    [[ ! -f "$scanner_root/$scanner" ]] || sources+=("$scanner_root/$scanner")
  done
  if [[ -n "$win" ]]; then
    flags+=(-fms-runtime-lib=dll "-Wl,/NODEFAULTLIB:libcmt,/EXPORT:tree_sitter_$name")
  else
    flags+=(-fPIC)
  fi
  "$CC" "${flags[@]}" -o "$fresh" "${sources[@]}" || { rm -f "$fresh"; return 1; }
  mv -f "$fresh" "$binary"
  echo "Built grammar $name"
}

generator=
generate() {
  local definition=$1 package generated
  package=$(dirname "$definition")
  [[ ! -f "$package/src/parser.c" ]] || return 0
  if [[ -z "$generator" ]]; then
    generator=${LAPLACE_TREESITTER_CLI:-$(command -v tree-sitter || true)}
    if [[ -z "$generator" ]] || ! "$generator" --version >/dev/null 2>&1; then
      runtime=${LAPLACE_TREESITTER:-${LAPLACE_DEPSRC:?Declare pinned dependency sources}/tree-sitter}
      target="${LAPLACE_BUILD:?Declare the build root}/deps/tree-sitter-cli"
      cargo build --locked --release --package tree-sitter-cli --manifest-path "$runtime/Cargo.toml" --target-dir "$target"
      generator="$target/release/tree-sitter"
    fi
  fi
  generated="${LAPLACE_BUILD:?Declare the build root}/deps/grammars/$(basename "$package")/src"
  mkdir -p "$generated"
  "$generator" generate "$definition" --output "$generated" --abi 15 --js-runtime native
  compile "$generated/parser.c" "$package/src" "$(dirname "$package")/common"
}
while IFS= read -r -d '' definition; do
  generate "$definition"
done < <(find "$SRC" -name grammar.js -not -path '*/node_modules/*' -print0)

while IFS= read -r -d '' manifest; do
  package=$(python3 "$(dirname "$0")/prepare_grammar.py" "$manifest" \
    "${EXTERNAL_SOURCES:?Declare pinned external grammar sources}" \
    "${LAPLACE_BUILD:?Declare the build root}/deps/grammars/derived")
  generate "$package/grammar.js"
done < <(find "$SRC" -name derivation.json -not -path '*/node_modules/*' -print0)

while IFS= read -r -d '' parser; do
  package=$(dirname "$(dirname "$parser")")
  compile "$parser" "$(dirname "$parser")" "$(dirname "$package")/common"
done < <(find "$SRC" -path '*/src/parser.c' -not -path '*/node_modules/*' -print0)
