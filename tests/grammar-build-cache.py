#!/usr/bin/env python3
"""Compile a real grammar; unchanged inputs must never invoke generation or compilation."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='grammar-build-cache-') as scratch:
    work = Path(scratch)
    source = work / 'source'
    package = source / 'lp_words'
    shutil.copytree(root / 'recipes/formats/grammars/lp_words', package)
    output = work / 'libraries'
    environment = dict(os.environ, LAPLACE_BUILD=str(work / 'build'))
    candidates = [os.environ.get('LAPLACE_TREESITTER_CLI'), shutil.which('tree-sitter')]
    if os.environ.get('LAPLACE_BUILD'):
        candidates.append(str(Path(os.environ['LAPLACE_BUILD']) / 'deps/tree-sitter-cli/release/tree-sitter'))
    generator = next((p for p in candidates if p and Path(p).is_file()
                      and subprocess.run([p, '--version'], capture_output=True, timeout=5).returncode == 0), None)
    assert generator, 'the grammar build must provide its usable pinned Tree-sitter generator'
    environment['LAPLACE_TREESITTER_CLI'] = generator

    def build(expected):
        result = subprocess.run(['bash', str(root / 'tools/build_grammars.sh'), str(source), str(output)],
                                env=environment, text=True, capture_output=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        assert expected in result.stdout, result.stdout
        print(expected)

    build('Grammars: 1 built, 0 reused, 1 generated')
    binary = output / 'libtree-sitter-lp_words.so'
    before = binary.stat().st_mtime_ns
    build('Grammars: 0 built, 1 reused, 0 generated')
    assert binary.stat().st_mtime_ns == before, 'unchanged library was replaced'
    definition = package / 'grammar.js'
    os.utime(definition, None)
    build('Grammars: 0 built, 1 reused, 0 generated')
    assert binary.stat().st_mtime_ns == before, 'checkout timestamp invalidated the cache'
    definition.write_text(definition.read_text().replace(r'[^ \t\r\n]+', r'[A-Za-z]+'))
    build('Grammars: 1 built, 0 reused, 1 generated')
    binary.write_bytes(b'corrupt artifact')
    build('Grammars: 1 built, 0 reused, 0 generated')
    library = ctypes.CDLL(str(binary))
    symbol = library.tree_sitter_lp_words
    symbol.restype = ctypes.c_void_p
    assert symbol(), 'rebuilt library did not export a usable grammar'
print('PASS: actual grammar compilation, unchanged/timestamp reuse, definition invalidation and corrupt artifact recovery')
