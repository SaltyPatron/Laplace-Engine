#!/usr/bin/env python3
"""Materialize a pinned grammar derivation in the declared build tree."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def contained(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise ValueError(f'Grammar input escapes its root: {relative}')
    return path


def prepare(manifest, sources, build, definition_path=False):
    manifest = manifest.resolve()
    spec = json.loads(manifest.read_text(encoding='utf-8'))
    if not re.fullmatch(r'[A-Za-z0-9_-]+', spec['source']):
        raise ValueError('Invalid grammar source name')
    if not re.fullmatch(r'[a-f0-9]{40}', spec['commit']):
        raise ValueError('Invalid grammar source revision')
    source = contained(sources.resolve(), spec['source'])
    patch = contained(manifest.parent, spec['patch']) if spec.get('patch') else None
    fingerprint = hashlib.sha256(manifest.read_bytes())
    if patch:
        fingerprint.update(patch.read_bytes())
    key = fingerprint.hexdigest()[:16]
    destination = (build / (manifest.parent.name + '-' + key)).resolve()
    for protected in (sources.resolve(), manifest.parent):
        if destination == protected or destination.is_relative_to(protected):
            raise ValueError('Derived grammar must be built outside source trees')

    # Verify every preimage before writing anything. Generated parser sources
    # are not inputs: the normal Tree-sitter generator owns them in the build.
    inputs = []
    for entry in spec['files']:
        data = contained(source, entry['path']).read_bytes()
        digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if digest != entry['sha']:
            raise ValueError(f"Pinned grammar input differs: {entry['path']}")
        target = contained(destination, entry['path'])
        if any(target == previous for previous, _ in inputs):
            raise ValueError('Duplicate grammar input')
        inputs.append((target, data))
    destination.mkdir(parents=True, exist_ok=True)
    for target, data in inputs:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    # git apply is the same patch implementation on Windows and Linux. A
    # changed upstream preimage or an ambiguous patch is a build error.
    if patch:
        for options in (['--check'], []):
            subprocess.run(['git', 'apply', '--unidiff-zero', '--whitespace=error-all', *options, str(patch)],
                           cwd=destination, check=True, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
    definition = contained(destination, spec.get('grammar', 'grammar.js'))
    if not definition.is_file():
        raise ValueError('Prepared grammar definition is missing')
    return definition if definition_path else destination


if __name__ == '__main__':
    try:
        definition_path = len(sys.argv) == 5 and sys.argv[4] == '--definition'
        if len(sys.argv) != 4 and not definition_path:
            raise ValueError('usage: prepare_grammar.py manifest external_sources build_directory [--definition]')
        print(prepare(*(Path(value) for value in sys.argv[1:4]), definition_path=definition_path))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'Grammar derivation failed: {error}', file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr.decode(errors='replace'), file=sys.stderr)
        sys.exit(1)
