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
# Derived providers declare their source dependency once. Use the normal
# Operations lock/provisioner in every build plane, including clean CI hosts.
required=$(python3 -c 'import json,pathlib,sys; print("\n".join(sorted({json.loads(p.read_text())["source"] for p in pathlib.Path(sys.argv[1]).rglob("derivation.json")})))' "$SRC")
if [[ -n "$required" ]]; then
  operations=${LAPLACE_OPERATIONS:-$(dirname "$(dirname "$0")")/../Laplace-Operations}
  [[ -f "$operations/grammars.sh" ]] || { echo 'Pinned derivations require the Operations grammar provisioner' >&2; exit 1; }
  mapfile -t required_sources <<< "$required"
  bash "$operations/grammars.sh" fetch "${EXTERNAL_SOURCES:?Declare pinned external grammar sources}" "${required_sources[@]}"
fi
mkdir -p "$OUT"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) win=1; ext=dll; CC=${CC:-icx-cc} ;;
  *) win=; ext=so; CC=${CC:-icx} ;;
esac
compiler_identity=$("$CC" --version)
machine=$(uname -sm)
built=0 reused=0 generated_count=0
# The Engine's own definitions (recipes/formats/grammars, derived and patched, what the recipes are written against)
# own their language names: compiling any other source tree into the same output leaves those libraries alone.
OWNER=$(cd "$(dirname "$0")/../recipes/formats/grammars" 2>/dev/null && pwd || true)
declare -A owned=()
if [[ -n "$OWNER" && "$SRC" != "$OWNER" ]]; then
  for owned_dir in "$OWNER"/*/; do owned[$(basename "$owned_dir")]=1; done
fi

# Contents survive fresh checkouts and source paths differ between build planes.
# Include every local header/scanner, not checkout timestamps or absolute paths.
inputs_key() {
  python3 - "$@" <<'PY'
import hashlib, pathlib, sys
h = hashlib.sha256(sys.argv[1].encode())
for index, value in enumerate(sys.argv[2:]):
    root = pathlib.Path(value)
    h.update(f'root:{index}\0'.encode())
    if not root.exists():
        h.update(b'missing\0')
        continue
    files = sorted(p for p in root.rglob('*') if p.is_file()
                   and not {'.git', 'node_modules'}.intersection(p.relative_to(root).parts)) if root.is_dir() else [root]
    for path in files:
        name = path.relative_to(root).as_posix() if root.is_dir() else path.name
        data = path.read_bytes()
        h.update(name.encode() + b'\0' + str(len(data)).encode() + b'\0' + data)
print(h.hexdigest())
PY
}

compile() {
  local parser=$1 scanner_root=$2 common=$3 name binary fresh
  # tree_sitter_x(void) or tree_sitter_x() (awk, mermaid and gstlaunch ship the latter)
  name=$(sed -n 's/.*TSLanguage \*tree_sitter_\([A-Za-z0-9_]*\)(\(void\)\{0,1\}).*/\1/p' "$parser" | head -1)
  [[ -n "$name" ]] || { echo "No exported language in $parser" >&2; return 1; }
  if [[ -n "${prebuilt:-}" && -n "${owned[$name]:-}" ]]; then echo "Kept grammar $name: defined in $OWNER"; return 0; fi
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
  local key stamp="$binary.inputs" digest flag
  local -a semantic_flags=()
  for flag in "${flags[@]}"; do [[ "$flag" == -I* ]] || semantic_flags+=("$flag"); done
  key=$(inputs_key "$compiler_identity|$machine|${semantic_flags[*]}" "$(dirname "$parser")" "$scanner_root" "$common")
  if [[ -s "$binary" && -f "$stamp" ]]; then
    digest=$(sha256sum "$binary"); digest=${digest%% *}
    if [[ $(cat "$stamp") == "$key $digest" ]]; then
      reused=$((reused + 1))
      echo "Reused grammar $name"
      return 0
    fi
  fi
  "$CC" "${flags[@]}" -o "$fresh" "${sources[@]}" || { rm -f "$fresh"; return 1; }
  digest=$(sha256sum "$fresh"); digest=${digest%% *}
  mv -f "$fresh" "$binary"
  printf '%s %s\n' "$key" "$digest" > "$stamp.$$.tmp"
  mv -f "$stamp.$$.tmp" "$stamp"
  built=$((built + 1))
  echo "Built grammar $name"
}

generator= generator_identity=
generate() {
  local definition=$1 package generated key=${2:-} input stamp output
  package=$(dirname "$definition")
  [[ $(basename "$definition") != grammar.json ]] || package=$(dirname "$package")
  [[ ! -f "$package/src/parser.c" ]] || return 0
  # A workspace root (tree-sitter-jinja) holds its grammar packages in subdirectories, each with its own grammar.js
  # and tree-sitter.json; the root's grammar.js is no package of its own, and the CLI cannot generate it.
  if [[ $(basename "$definition") == grammar.js && ! -f "$package/tree-sitter.json" ]] &&
     compgen -G "$package/*/grammar.js" >/dev/null; then return 0; fi
  if [[ -z "$generator" ]]; then
    generator=${LAPLACE_TREESITTER_CLI:-$(command -v tree-sitter || true)}
    if [[ -z "$generator" ]] || ! "$generator" --version >/dev/null 2>&1; then
      runtime=${LAPLACE_TREESITTER:-${LAPLACE_DEPSRC:?Declare pinned dependency sources}/tree-sitter}
      target="${LAPLACE_BUILD:?Declare the build root}/deps/tree-sitter-cli"
      cargo build --locked --release --package tree-sitter-cli --manifest-path "$runtime/Cargo.toml" --target-dir "$target"
      generator="$target/release/tree-sitter"
    fi
    generator_identity=$("$generator" --version)
  fi
  generated="${LAPLACE_BUILD:?Declare the build root}/deps/grammars/${key:-$(basename "$package")}/src"
  mkdir -p "$generated"
  stamp="$(dirname "$generated")/.generation-inputs"
  input=$(inputs_key "$generator_identity|abi15|native" "$package" "$(dirname "$package")/common")
  output=$(inputs_key generated "$generated")
  if [[ ! -s "$generated/parser.c" || ! -f "$stamp" || $(cat "$stamp") != "$input $output" ]]; then
    # The embedded QuickJS runtime first; a grammar.js it cannot load (tree-sitter-perl's v-flag class-set regexes)
    # is evaluated by Node where the host has it. The generated parser depends only on the evaluated grammar.
    if ! "$generator" generate "$definition" --output "$generated" --abi 15 --js-runtime native; then
      command -v node >/dev/null || { echo "Cannot evaluate $definition: QuickJS failed and node is not installed" >&2; return 1; }
      echo "Evaluating $definition with node"
      "$generator" generate "$definition" --output "$generated" --abi 15 --js-runtime node
    fi
    output=$(inputs_key generated "$generated")
    printf '%s %s\n' "$input" "$output" > "$stamp.$$.tmp"
    mv -f "$stamp.$$.tmp" "$stamp"
    generated_count=$((generated_count + 1))
  fi
  compile "$generated/parser.c" "$package/src" "$(dirname "$package")/common"
}
while IFS= read -r -d '' definition; do
  generate "$definition"
done < <(find "$SRC" -name grammar.js -not -path '*/node_modules/*' -print0)

while IFS= read -r -d '' manifest; do
  definition=$(python3 "$(dirname "$0")/prepare_grammar.py" "$manifest" \
    "${EXTERNAL_SOURCES:?Declare pinned external grammar sources}" \
    "${LAPLACE_BUILD:?Declare the build root}/deps/grammars/derived" --definition)
  generate "$definition" "$(basename "$(dirname "$manifest")")"
done < <(find "$SRC" -name derivation.json -not -path '*/node_modules/*' -print0)

# Prebuilt parsers of another tree (vendored gmr-postgres, c_sharp, typescript, ...) never replace a language the Engine
# defines; a definition generated here (a copy of the Engine's own in a test tree) still builds.
prebuilt=1
while IFS= read -r -d '' parser; do
  package=$(dirname "$(dirname "$parser")")
  compile "$parser" "$(dirname "$parser")" "$(dirname "$package")/common"
done < <(find "$SRC" -path '*/src/parser.c' -not -path '*/node_modules/*' -print0)
echo "Grammars: $built built, $reused reused, $generated_count generated"
