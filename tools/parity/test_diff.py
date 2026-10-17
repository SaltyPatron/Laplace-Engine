#!/usr/bin/env python3
"""diff.py on fixtures: no database. Run by the Engine's CI checks."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

DIFF = Path(__file__).resolve().parent / "diff.py"


def write(d: Path, entities=(), claims=(), testimony=()):
    d.mkdir(parents=True, exist_ok=True)
    (d / "entities.tsv").write_text("".join(f"{e}\t{t}\n" for e, t in entities))
    (d / "claims.tsv").write_text("".join(f"{c}\n" for c in claims))
    (d / "testimony.tsv").write_text("".join(f"{c}\t{w}\t{g}\n" for c, w, g in testimony))


class Diff(unittest.TestCase):
    def run_diff(self, a, b, *extra):
        out = Path(self.tmp.name) / "summary.json"
        r = subprocess.run([sys.executable, str(DIFF), str(a), str(b), "--json", str(out), *extra],
                           capture_output=True, text=True)
        return r, json.loads(out.read_text())

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_equal_sides_have_no_differences(self):
        for s in ("a", "b"):
            write(self.root / s, [("AA", 0), ("bb", 1)], ["cc"], [("cc", "w" + s, 2)])
        r, j = self.run_diff(self.root / "a", self.root / "b", "--require-equal")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(j["unaccepted"], {})
        self.assertEqual(j["shared"]["entities"], 2)

    def test_contribution_is_after_less_before_and_differences_are_classes(self):
        write(self.root / "a/before", [("01", 0)])
        write(self.root / "a/after", [("01", 0), ("02", 1), ("03", 2)], ["c1", "c2"], [("c1", "x", 1), ("c2", "x", 3)])
        write(self.root / "b/before", [("01", 0)])
        write(self.root / "b/after", [("01", 0), ("02", 2), ("04", 1)], ["c1"], [("c1", "y", 1), ("c1", "z", 1)])
        r, j = self.run_diff(self.root / "a", self.root / "b")
        self.assertEqual(r.returncode, 0)
        c = j["classes"]
        self.assertEqual((c["entities-only-a"]["count"], c["entities-only-b"]["count"]), (1, 1))
        self.assertEqual(c["tier-differ"]["count"], 1)
        self.assertEqual((c["claims-only-a"]["count"], c["claims-only-b"]["count"]), (1, 0))
        self.assertEqual(c["games-differ"]["count"], 1)      # c1: 1 against 2
        r, _ = self.run_diff(self.root / "a", self.root / "b", "--require-equal")
        self.assertEqual(r.returncode, 1)

    def test_accepted_classes_pass_and_forget_round_trip_is_checked(self):
        write(self.root / "a/before", [("01", 0)])
        write(self.root / "a/after", [("01", 0), ("02", 1)])
        write(self.root / "a/forgotten", [("01", 0), ("02", 1)])     # forget left 02 behind
        write(self.root / "b/before", [("01", 0)])
        write(self.root / "b/after", [("01", 0), ("02", 1)])
        accept = self.root / "accept.tsv"
        accept.write_text("# class\twhy\n")
        r, j = self.run_diff(self.root / "a", self.root / "b", "--accept", str(accept), "--require-equal")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(j["unaccepted"], {"forget-a-entities-left": 1})
        accept.write_text("forget-a-entities-left\tfixture\n")
        r, _ = self.run_diff(self.root / "a", self.root / "b", "--accept", str(accept), "--require-equal")
        self.assertEqual(r.returncode, 0, r.stdout)


if __name__ == "__main__":
    unittest.main()
