#!/usr/bin/env python3
"""claims.py on fixtures: no database, no engine. Run by the Engine's CI checks."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CLAIMS = Path(__file__).resolve().parent / "claims.py"

ENGINE = "\n".join([
    "laplace ingest   32 threads   170 recipes",
    "claim\t0\t1\tabide_by\tili:i34411",
    "claim\t0\t1\ti21790\tskos:definition\tkeep from exhaling or expelling",
    "claim\t3\t0.5\tDogs\tnsubj\tbark",
    "claim\t0\t1\tx\tsays\tone\ttwo",
    "held\t0\t1\tthing\tsaid",
    "claim\t0\t1\ttab\tin\ta\\tb",
    "== unresolved obligations: 0 parts a claim would name",
    "   190 compositions, 119 reused; 4 claims said by records",
    "",
])
MONO = "\n".join([
    "abide_by\t\ti34411\t2",
    "i21790\tDEFINITION\tkeep  from exhaling or expelling\t1",
    "Dogs\tnsubj\tbark\t1",
    "x\tsays\tone, two\t1",
    "tab\tin\ta\\tb\t1",
    "only\tmono\tclaim\t1",
    "",
])


class Claims(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "e.tsv").write_text(ENGINE, encoding="utf-8")
        (self.root / "m.tsv").write_text(MONO, encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def run_claims(self, *extra):
        out = self.root / "r.json"
        r = subprocess.run([sys.executable, str(CLAIMS), str(self.root / "e.tsv"), str(self.root / "m.tsv"),
                            "--json", str(out), "--report", str(self.root / "r.md"), *extra], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(out.read_text(encoding="utf-8"))

    def test_engine_lines_only_claims(self):
        r = self.run_claims()
        self.assertEqual(r["claims"]["a"], 5, r)  # held tuples and the run's lines are not claims
        self.assertEqual(r["claims"]["b"], 6)

    def test_text_match_and_list_names(self):
        r = self.run_claims()
        both = {tuple(x) for x in r["sample"]["both"]}
        self.assertIn(("Dogs", "nsubj", "bark"), both)
        self.assertIn(("x", "says", "one, two"), both)        # parts after the predicate joined by ", "
        self.assertIn(("tab", "in", "a\\tb"), both)           # escapes compared as written
        self.assertNotIn(("abide_by", "", "i34411"), both)    # ili:i34411 is not i34411 unless asked
        r = self.run_claims("--strip-lists")
        self.assertIn(["abide_by", "", "i34411"], r["sample"]["both"])
        self.assertEqual(r["claims"]["both"], 4)

    def test_white_space_and_crosswalk(self):
        r = self.run_claims()
        # "keep  from" is "keep from"; the predicates differ, so the pair is in the crosswalk, not a match
        self.assertIn({"a": "skos:definition", "b": "DEFINITION", "pairs": 1}, r["crosswalk"])
        m = self.root / "map.tsv"
        m.write_text("# engine -> mono\nskos:definition\tDEFINITION\n", encoding="utf-8")
        r = self.run_claims("--strip-lists", "--predicates", str(m))
        self.assertEqual(r["claims"]["both"], 5)
        self.assertEqual(r["claims"]["only_b"], 1)
        self.assertEqual(r["sample"]["only_b"], [["only", "mono", "claim"]])
        self.assertEqual(r["crosswalk"], [])

    def test_report(self):
        self.run_claims("--names", "engine,mono")
        text = (self.root / "r.md").read_text(encoding="utf-8")
        self.assertIn("| distinct claims | 5 | 6 |", text)
        self.assertIn("## Crosswalk", text)


if __name__ == "__main__":
    unittest.main()