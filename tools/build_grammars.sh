#!/bin/sh
# Compile every tree-sitter grammar under $1 into $2/libtree-sitter-<name>.so, exporting tree_sitter_<name>.
# A grammar directory holds src/parser.c (and optionally src/scanner.c), directly or one level down (xml/, tsv/, ...).
SRC=${1:-/vault/Data/TreeSitter}; OUT=${2:-/repos/build/grammars}; CC=${CC:-icx}; mkdir -p "$OUT"
find "$SRC" -path '*/src/parser.c' -not -path '*/node_modules/*' | while read -r p; do
  d=$(dirname "$(dirname "$p")")
  name=$(sed -n 's/.*TSLanguage \*tree_sitter_\([A-Za-z0-9_]*\)(void).*/\1/p' "$p" | head -1)
  [ -n "$name" ] || continue
  so="$OUT/libtree-sitter-$name.so"; [ "$so" -nt "$p" ] && continue
  sc=""; [ -f "$d/src/scanner.c" ] && sc="$d/src/scanner.c"
  echo "$name"
  $CC -O2 -fPIC -shared -w -I"$d/src" -I"$(dirname "$d")/common" -o "$so" "$p" $sc 2>/dev/null || { echo "  failed: $name" >&2; rm -f "$so"; }
done
