#!/usr/bin/env python3
"""Compare what two pipelines recorded for one source (Laplace#1885 step 1).

    diff.py A_DIR B_DIR [--names A,B] [--report FILE.md] [--json FILE] [--accept FILE] [--require-equal]

Each side's directory holds export.sh's canonical TSVs, either directly (the whole database) or as before/ and after/
(the database before and after the source went in: the source's own contribution is after minus before). A side
may also hold forgotten/ (after the source was forgotten again); its round trip is then checked against before/.

What is compared: the entity ID sets (and how many at each tier), the claim ID sets, and the testimony (claim,
witness, games). Witness IDs differ between pipelines by construction, so testimony is compared by claim: the games
each claim was said, summed over its witnesses.

Differences are findings. Each is a class (entities-only-a, claims-only-b, games-differ, tier-differ, forget-a, ...).
--accept names accepted classes, one a line: CLASS<TAB>why. --require-equal exits 1 when a class not accepted has
any members; without it the exit is 0 and the report is the result.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

HEX = re.compile(r"[^0-9a-f]")
SAMPLE = 10


def norm(value: str) -> str:
    return HEX.sub("", value.strip().lower())


def read_rows(path: Path) -> list[list[str]]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t") for line in f if line.strip()]


class Snapshot:
    def __init__(self, directory: Path):
        self.tier: dict[str, int] = {}
        for row in read_rows(directory / "entities.tsv"):
            self.tier[norm(row[0])] = int(row[1]) if len(row) > 1 and row[1].strip().lstrip("-").isdigit() else -1
        self.claims = {norm(r[0]) for r in read_rows(directory / "claims.tsv")}
        self.games: Counter[str] = Counter()
        self.witnesses: set[str] = set()
        for row in read_rows(directory / "testimony.tsv"):
            claim, witness = norm(row[0]), norm(row[1]) if len(row) > 1 else ""
            self.witnesses.add(witness)
            self.games[claim] += float(row[2]) if len(row) > 2 and row[2].strip() else 1

    def minus(self, other: "Snapshot") -> "Snapshot":
        out = Snapshot.__new__(Snapshot)
        out.tier = {k: v for k, v in self.tier.items() if k not in other.tier}
        out.claims = self.claims - other.claims
        out.games = Counter({k: v - other.games.get(k, 0) for k, v in self.games.items() if v != other.games.get(k, 0)})
        out.witnesses = self.witnesses - other.witnesses
        return out


def side(directory: Path) -> tuple[Snapshot, dict | None]:
    before, after = directory / "before", directory / "after"
    if not after.is_dir():
        return Snapshot(directory), None
    b, a = Snapshot(before), Snapshot(after)
    roundtrip = None
    if (directory / "forgotten").is_dir():
        f = Snapshot(directory / "forgotten")
        roundtrip = {
            "entities_left": len(set(f.tier) - set(b.tier)),
            "entities_lost": len(set(b.tier) - set(f.tier)),
            "claims_left": len(f.claims - b.claims),
            "claims_lost": len(b.claims - f.claims),
        }
    return a.minus(b), roundtrip


def sample(items) -> list[str]:
    return sorted(items)[:SAMPLE]


def compare(a: Snapshot, b: Snapshot, names: tuple[str, str]) -> dict:
    ea, eb = set(a.tier), set(b.tier)
    shared = ea & eb
    tier_differ = sorted(k for k in shared if a.tier[k] != b.tier[k])
    ca, cb = a.claims, b.claims
    claims_shared = ca & cb
    said = set(a.games) & set(b.games)
    games_differ = sorted(k for k in said if abs(a.games[k] - b.games[k]) > 1e-9)
    tiers = sorted(set(a.tier.values()) | set(b.tier.values()))
    return {
        "names": list(names),
        "counts": {
            names[0]: {"entities": len(ea), "claims": len(ca), "claims_said": len(a.games),
                       "games": sum(a.games.values()), "witnesses": len(a.witnesses)},
            names[1]: {"entities": len(eb), "claims": len(cb), "claims_said": len(b.games),
                       "games": sum(b.games.values()), "witnesses": len(b.witnesses)},
        },
        "by_tier": {str(t): {names[0]: sum(1 for v in a.tier.values() if v == t),
                             names[1]: sum(1 for v in b.tier.values() if v == t),
                             "shared": sum(1 for k in shared if a.tier[k] == t)} for t in tiers},
        "classes": {
            "entities-only-a": {"count": len(ea - eb), "sample": sample(ea - eb)},
            "entities-only-b": {"count": len(eb - ea), "sample": sample(eb - ea)},
            "tier-differ": {"count": len(tier_differ), "sample": tier_differ[:SAMPLE]},
            "claims-only-a": {"count": len(ca - cb), "sample": sample(ca - cb)},
            "claims-only-b": {"count": len(cb - ca), "sample": sample(cb - ca)},
            "games-differ": {"count": len(games_differ), "sample": games_differ[:SAMPLE]},
        },
        "shared": {"entities": len(shared), "claims": len(claims_shared), "claims_said_by_both": len(said)},
    }


def report_md(result: dict, accepted: dict[str, str]) -> str:
    a, b = result["names"]
    lines = [f"## Parity: {a} (A) against {b} (B)", ""]
    if result.get("scope"):
        lines += [result["scope"], ""]
    lines += ["| | " + a + " | " + b + " |", "|---|---:|---:|"]
    for key in ("entities", "claims", "claims_said", "games", "witnesses"):
        lines.append(f"| {key} | {result['counts'][a][key]:,} | {result['counts'][b][key]:,} |")
    s = result["shared"]
    lines += ["", f"Shared: {s['entities']:,} entities, {s['claims']:,} claims, {s['claims_said_by_both']:,} claims said on both sides.", ""]
    lines += ["| tier | " + a + " | " + b + " | shared |", "|---:|---:|---:|---:|"]
    for t, row in result["by_tier"].items():
        lines.append(f"| {t} | {row[a]:,} | {row[b]:,} | {row['shared']:,} |")
    lines += ["", "| difference | count | accepted | sample |", "|---|---:|---|---|"]
    for cls, v in result["classes"].items():
        why = accepted.get(cls, "")
        lines.append(f"| {cls} | {v['count']:,} | {why} | {' '.join(v['sample'][:3])} |")
    for side_name in ("roundtrip_a", "roundtrip_b"):
        rt = result.get(side_name)
        if rt:
            who = a if side_name.endswith("a") else b
            lines.append(f"\nForget round trip ({who}): {rt['entities_left']} entities and {rt['claims_left']} claims left "
                         f"behind, {rt['entities_lost']} entities and {rt['claims_lost']} claims lost.")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("a", type=Path); p.add_argument("b", type=Path)
    p.add_argument("--names", default="A,B"); p.add_argument("--report", type=Path); p.add_argument("--json", type=Path)
    p.add_argument("--accept", type=Path); p.add_argument("--require-equal", action="store_true")
    p.add_argument("--scope", default="")
    args = p.parse_args(argv)
    names = tuple(args.names.split(",", 1)) if "," in args.names else ("A", "B")
    accepted: dict[str, str] = {}
    if args.accept:
        for row in read_rows(args.accept):
            if row and not row[0].startswith("#"):
                accepted[row[0].strip()] = row[1].strip() if len(row) > 1 else "accepted"
    (sa, rta), (sb, rtb) = side(args.a), side(args.b)
    result = compare(sa, sb, names)
    result["scope"] = args.scope
    for key, rt in (("roundtrip_a", rta), ("roundtrip_b", rtb)):
        if rt is not None:
            result[key] = rt
            for what, n in rt.items():
                result["classes"][f"forget-{key[-1]}-{what.replace('_', '-')}"] = {"count": n, "sample": []}
    unaccepted = {c: v["count"] for c, v in result["classes"].items() if v["count"] and c not in accepted}
    result["unaccepted"] = unaccepted
    md = report_md(result, accepted)
    if args.report:
        args.report.write_text(md, encoding="utf-8")
    if args.json:
        args.json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    sys.stdout.write(md)
    if unaccepted:
        print("differences not accepted: " + ", ".join(f"{c} {n:,}" for c, n in unaccepted.items()))
    return 1 if args.require_equal and unaccepted else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
