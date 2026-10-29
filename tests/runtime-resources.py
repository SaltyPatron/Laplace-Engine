#!/usr/bin/env python3
"""A package update removes obsolete rules without deleting unrelated files."""
from pathlib import Path
import subprocess
import tempfile


with tempfile.TemporaryDirectory(prefix='recipe-resources-') as scratch:
    root = Path(scratch)
    source, target = root / 'source', root / 'installed'
    expected = ['verbatlas/shadow.recipe', 'verbatlas/implicit.recipe', 'verbatlas/source',
                'formats/xml.format', 'vocabularies/wordnet-pos.tsv', 'order']
    obsolete = ['verbatlas/shadow-implicit.recipe', 'retired/source', 'formats/retired.format',
                'vocabularies/retired.tsv', 'retired/LICENSE.LICENSE']
    for directory, files in [(source, expected), (target, expected + obsolete + ['operator-note.txt'])]:
        for file in files:
            path = directory / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(file, encoding='utf8')
    command = ['cmake', '-DRECIPE_SOURCE='+str(source), '-DRECIPE_TARGET='+str(target),
               '-P', str(Path(__file__).resolve().parents[1] / 'cmake/PruneRecipeResources.cmake')]
    subprocess.run(command, check=True)
    assert all((target / file).exists() for file in expected + ['operator-note.txt'])
    assert not any((target / file).exists() for file in obsolete)
    subprocess.run(command, check=True)
    assert (target / 'operator-note.txt').read_text(encoding='utf8') == 'operator-note.txt'
    print('recipe resources: obsolete declarations removed; current resources and unrelated files retained; replay unchanged')
