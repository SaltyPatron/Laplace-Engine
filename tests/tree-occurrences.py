#!/usr/bin/env python3
"""References distinguish occurrences, independently of packaging keys or workers."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


engine = Path(sys.argv[1]).resolve()
recipes = Path(__file__).resolve().parents[1] / "recipes"


def records(path, source, root, workers, extra_env=None):
    env = dict(os.environ, LAPLACE_WORK=str(root / "work"))
    env.update(extra_env or {})
    result = subprocess.run(
        [str(engine), "ingest", "--claims-tsv", "-j", str(workers),
         "-s", source, str(path)],
        env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    rows = [line.split("\t", 4) for line in result.stdout.splitlines()
            if line.startswith("record\t")]
    assert rows, result.stdout + result.stderr
    return [row[3] for row in rows]


with tempfile.TemporaryDirectory(prefix="laplace-tree-occurrences-") as scratch:
    root = Path(scratch)
    words = ["see", "a", "a", "b", "b"]
    cases = {"ordered": [0, 1, 1, 2, 3], "crossed": [0, 1, 1, 3, 2]}
    identities = {}
    for name, heads in cases.items():
        path = root / (name + ".conllu")
        lines = ["# text = " + " ".join(words)]
        for index, (word, head) in enumerate(zip(words, heads), 1):
            lines.append("\t".join([str(index), word, word,
                "VERB" if index == 1 else "NOUN", "_", "_", str(head),
                "root" if index == 1 else "dep", "_", "_"]))
        path.write_text("\n".join(lines) + "\n\n", encoding="utf-8")
        env = {"LAPLACE_RECIPES": str(recipes)}
        single = records(path, "universal-dependencies", root, 1, env)
        assert records(path, "universal-dependencies", root, 4, env) == single
        identities[name] = single
    assert identities["ordered"] != identities["crossed"], (
        "Distinct occurrence-reference graphs collapsed to one record: "
        + str(identities))

    # Real Lichess opening rows: the PGN capture is packaging, not a second
    # copy of the moves in the opening's semantic subject. A longer opening
    # must retain every move, not collapse to its first move.
    opening = root / 'a.tsv'
    opening.write_text('eco\tname\tpgn\nA00\tAmar Opening\t1. Nh3\n'
                       'A00\tAmar Opening: Paris Gambit\t1. Nh3 d5 2. g3 e5 3. f4\n', encoding='utf-8')
    expected = [
        ['Nh3', 'opening name', 'Amar Opening'],
        ['Nh3', 'Encyclopaedia of Chess Openings code', 'A00'],
        ['[Nh3, d5, g3, e5, f4]', 'opening name', 'Amar Opening: Paris Gambit'],
        ['[Nh3, d5, g3, e5, f4]', 'Encyclopaedia of Chess Openings code', 'A00'],
    ]
    for workers in (1, 4):
        result = subprocess.run([str(engine), 'ingest', '--claims-tsv', '-j', str(workers),
                                 '-r', str(recipes), '-s', 'openings', str(opening)],
                                env=dict(os.environ, LAPLACE_RECIPES=str(recipes)),
                                capture_output=True, text=True, encoding='utf-8', errors='replace')
        assert result.returncode == 0, result.stdout + result.stderr
        claims = [line.split('\t')[3:] for line in result.stdout.splitlines() if line.startswith('claim\t')]
        assert claims == expected, claims

    # The same tree directive also serves an ordinary XML source. Its
    # arbitrary integer keys locate rows; changing those keys cannot change
    # the retained graph. These are not CoNLL-U token numbers.
    estate = root / "recipes"
    source = estate / "occurrences"
    source.mkdir(parents=True)
    (estate / "formats").mkdir()
    shutil.copy2(recipes / "file.recipe", estate / "file.recipe")
    shutil.copy2(recipes / "formats/xml.format", estate / "formats/xml.format")
    (source / "source").write_text(
        "name occurrences\nwitness occurrence graph\nclass AcademicCurated\n"
        "root $LAPLACE_DATA/occurrences\n", encoding="utf-8")
    (source / "rows.recipe").write_text(r"""name occurrence-rows
match *.xml
format xml
thing record of node
thing node FORM
content FORM DEPREL
key node ID within record
when HEAD is 0
omit HEAD
always
refer HEAD node
layers record node FORM DEPREL
tree record node HEAD DEPREL
""", encoding="utf-8")
    env = {"LAPLACE_RECIPES": str(estate), "LAPLACE_DATA": str(root)}

    def graph(name, parents, forms, keys=None):
        keys = keys or list(range(1, len(forms) + 1))
        path = root / (name + ".xml")
        nodes = []
        for i, (form, parent) in enumerate(zip(forms, parents)):
            head = str(keys[parent - 1]) if parent else "0"
            relation = "dep" if parent else "root"
            nodes.append(f'<node ID="{keys[i]}" FORM="{form}" HEAD="{head}" DEPREL="{relation}"/>')
        path.write_text("<record>" + "".join(nodes) + "</record>", encoding="utf-8")
        return records(path, "occurrences", root, 4, env)

    ordinary = graph("ordinary", cases["ordered"], words)
    assert graph("renumbered", cases["ordered"], words, [11, 23, 41, 67, 89]) == ordinary
    assert graph("changed-edges", cases["crossed"], words) != ordinary

    # Removing the former fixed child/root limits must retain the dependency
    # distinctions beyond those boundaries, too. All content and tag layers
    # are identical between each pair; only a late parent edge changes.
    forms = ["see"] + ["a"] * 300
    parents = [0] + [1] * 300
    wide = graph("wide", parents, forms)
    parents[-1] = len(forms) - 1
    assert graph("wide-late-edge", parents, forms) != wide
    parents = [0] * 70
    forest = graph("forest", parents, ["a"] * 70)
    parents[-1] = len(parents) - 1
    assert graph("forest-late-edge", parents, ["a"] * 70) != forest

print("tree-occurrences: distinct graphs, source-key independence, workers, wide trees and forests")
