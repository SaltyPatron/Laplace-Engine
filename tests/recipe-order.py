#!/usr/bin/env python3
"""Actual recipe dependencies schedule providers before consumers, including cycles."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("engine", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    with tempfile.TemporaryDirectory(prefix="recipe-order-", dir=os.environ.get("LAPLACE_WORK")) as scratch:
        root = Path(scratch)
        recipes = root / "recipes"
        shutil.copytree(Path(__file__).resolve().parents[1] / "recipes", recipes,
                        ignore=shutil.ignore_patterns('grammars'))
        (recipes / "order").write_text("schedule\n", encoding="utf-8", newline="\n")
        definitions = recipes / "schedule"
        definitions.mkdir()
        (definitions / "source").write_text(
            "name schedule\nwitness Native recipe scheduling\nclass AcademicCurated\nroot $LAPLACE_DATA/schedule\n",
            encoding="utf-8", newline="\n")
        data = root / "data/schedule"
        data.mkdir(parents=True)
        count = 40

        def recipe(name, file, fields, references):
            lines = [f"name {name}", f"match {file}", r"tier row by \n", "names identifier word " + " ".join(fields),
                     r"tier field by \t", "thing row word", "content word", "key row identifier"]
            for field, target in references:
                lines += [f"refer {field} {target}", f'relation "depends on" {field}', f"attest row {field}"]
            (definitions / (name + ".recipe")).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

        for level in range(count):
            file = f"chain-{count - 1 - level:03}.tsv"
            references = [("parent", f"schedule-{level - 1:03}")] if level else []
            recipe(f"schedule-{level:03}", file, ["parent"] if level else [], references)
            (data / file).write_text(f"id\tword{level:03}" + ("\tid" if level else "") + "\n",
                                     encoding="utf-8", newline="\n")
        recipe("cycle-a", "a-cycle.tsv", ["partner", "base"],
               [("partner", "cycle-b"), ("base", "schedule-010")])
        recipe("cycle-b", "b-cycle.tsv", ["partner", "base"],
               [("partner", "cycle-a"), ("base", "schedule-005")])
        (data / "a-cycle.tsv").write_text("id\tcycleA\tid\tid\n", encoding="utf-8", newline="\n")
        (data / "b-cycle.tsv").write_text("id\tcycleB\tid\tid\n", encoding="utf-8", newline="\n")
        progress = root / "progress.jsonl"
        env = os.environ.copy()
        env.update(LAPLACE_DATA=str(root / "data"), LAPLACE_RECIPES=str(recipes), LAPLACE_WORK=str(root),
                   LAPLACE_INGEST_PROGRESS_PATH=str(progress))
        for options in ([], ['--no-load'], ['--plan'], ['--claims']):
            for inputs in (['misspelled-source'], ['schedule', str(root / 'missing file.tsv')]):
                invalid = subprocess.run([str(engine), 'ingest', '-r', str(recipes), '-d',
                                          'host=127.0.0.1 port=1 connect_timeout=1', *options, *inputs],
                                         env=env, capture_output=True, text=True, encoding='utf-8', timeout=10)
                assert invalid.returncode == 2, invalid.stdout + invalid.stderr
                assert 'neither a declared source nor an accessible file or directory' in invalid.stderr, invalid.stderr
                assert not progress.exists(), 'Invalid input began ingestion before the whole selection was checked'
        result = subprocess.run([str(engine), "ingest", "-r", str(recipes), "-j", "2", "--no-load", "--claims",
                                 "-s", "schedule", str(data)], env=env, capture_output=True,
                                text=True, encoding="utf-8", timeout=60)
        report = result.stdout + result.stderr
        assert result.returncode == 0, report
        started = [Path(frame["file"]).name for frame in map(json.loads, progress.read_text(encoding="utf-8").splitlines())
                   if frame["event"] == "file-start"]
        chain = [file for file in started if file.startswith("chain-")]
        assert chain == [f"chain-{n:03}.tsv" for n in reversed(range(count))], started
        assert started.index("chain-029.tsv") < started.index("a-cycle.tsv") < started.index("b-cycle.tsv"), started
        assert started.index("b-cycle.tsv") < started.index("chain-027.tsv"), started
        expected = {f"[word{level:03}, depends on, word{level - 1:03}]" for level in range(1, count)}
        expected |= {"[cycleA, depends on, cycleB]", "[cycleA, depends on, word010]",
                     "[cycleB, depends on, cycleA]", "[cycleB, depends on, word005]"}
        observed = {line.strip() for line in result.stdout.splitlines() if line.startswith("[")}
        assert observed == expected, {"missing": sorted(expected - observed), "extra": sorted(observed - expected), "report": report}
        print(json.dumps({"dependency_chain": count, "cyclic_group": 2, "external_provider_order": "preserved",
                          "declaration_order": "preserved", "resolved_claims": len(observed), "database_access": False}))


if __name__ == "__main__":
    main()
