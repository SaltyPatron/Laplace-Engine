#!/usr/bin/env python3
"""Compare what two pipelines claim of one source by the claims' text (Laplace#1879, #68).

    claims.py A.tsv B.tsv [--names A,B] [--predicates MAP.tsv] [--strip-lists LIST,...] [--report FILE.md]
                          [--json FILE] [--sample N]

The engine and the monorepo give the same content different IDs, so their claims are compared as text: subject,
predicate, object. Each side is one file, in either form:

  the engine's   laplace ingest --claims-tsv:  claim<TAB>position<TAB>score<TAB>part<TAB>part...  (other lines,
                 held tuples and what the run says, are passed over); a claim of three or more parts is subject,
                 predicate and the rest joined by ", "; of two, subject and object with no predicate
  canonical      subject<TAB>predicate<TAB>object[<TAB>count]  (claims-export.sh mono)

Parts keep the escapes both writers use (\\t, \\n, \\\\); they are compared after Unicode NFC and with runs of
white space as one space. --strip-lists drops a highway list's name from a part (ili:i35545 -> i35545) where the
other side writes the key alone. --predicates maps side A's predicates onto B's: A_PREDICATE<TAB>B_PREDICATE a line
(# comments), applied before comparing.

What is reported: the distinct claims of each side, those both make, those only one makes, per predicate; and the
crosswalk: a subject and object both sides relate, under different predicates, counted by predicate pair, which is
what a --predicates map is written from. The exit is 0; the report is the result.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import unicodedata
from collections import Counter
from pathlib import Path

WS = re.compile(r"\s+")
LISTS = ("ili", "pbroleset", "vnclass", "vnrole", "fnframe", "fnfe", "fnlu", "vaframe", "lexfile", "upos", "deprel",
         "wnframe", "wnsentence")


def norm(s: str) -> str:
    return WS.sub(" ", unicodedata.normalize("NFC", s)).strip()


def strip_list(s: str, lists: tuple[str, ...]) -> str:
    head, sep, key = s.partition(":")
    return key if sep and head in lists and key else s


def read(path: Path, lists: tuple[str, ...], pmap: dict[str, str] | None) -> Counter:
    out: Counter = Counter()
    with path.open(encoding="utf-8", errors="replace", newline="") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line:
                continue
            cols = line.split("\t")
            if cols[0] == "claim" and len(cols) >= 4:
                parts = cols[3:]
                if len(parts) >= 3:
                    s, p, o = parts[0], parts[1], ", ".join(parts[2:])
                elif len(parts) == 2:
                    s, p, o = parts[0], "", parts[1]
                else:
                    s, p, o = parts[0], "", ""
                n = 1
            elif cols[0] in ("held",) or line.startswith((" ", "==", "--", "[", "laplace ")):
                continue
            elif len(cols) in (3, 4):
                s, p, o = cols[0], cols[1], cols[2]
                n = int(cols[3]) if len(cols) == 4 and cols[3].isdigit() else 1
            else:
                continue
            s, p, o = (norm(strip_list(x, lists)) for x in (s, p, o))
            if pmap is not None:
                p = pmap.get(p, p)
            out[(s, p, o)] += n
    return out


def read_map(path: Path | None) -> dict[str, str] | None:
    if path is None:
        return None
    m = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        a, _, b = line.partition("\t")
        m[norm(a)] = norm(b)
    return m


def compare(a: Counter, b: Counter, sample: int) -> dict:
    ka, kb = set(a), set(b)
    both, only_a, only_b = ka & kb, ka - kb, kb - ka
    per: dict[str, dict[str, int]] = {}
    for side, keys in (("a", ka), ("b", kb), ("both", both)):
        for (_, p, _) in keys:
            per.setdefault(p, {"a": 0, "b": 0, "both": 0})[side] += 1
    so_a: dict[tuple[str, str], set[str]] = {}
    for (s, p, o) in only_a:
        so_a.setdefault((s, o), set()).add(p)
    cross: Counter = Counter()
    for (s, p, o) in only_b:
        for pa in so_a.get((s, o), ()):
            cross[(pa, p)] += 1
    srt = lambda ks: sorted(ks)[:sample]
    return {
        "claims": {"a": len(ka), "b": len(kb), "both": len(both), "only_a": len(only_a), "only_b": len(only_b)},
        "match_of_a": round(len(both) / len(ka), 6) if ka else None,
        "match_of_b": round(len(both) / len(kb), 6) if kb else None,
        "predicates": dict(sorted(per.items())),
        "crosswalk": [{"a": pa, "b": pb, "pairs": n} for (pa, pb), n in cross.most_common()],
        "sample": {"only_a": [list(k) for k in srt(only_a)], "only_b": [list(k) for k in srt(only_b)],
                   "both": [list(k) for k in srt(both)]},
    }


def report(r: dict, names: tuple[str, str]) -> str:
    A, B = names
    c = r["claims"]
    pct = lambda x: "n/a" if x is None else f"{100 * x:.1f}%"
    out = [f"# Claim-level parity: {A} vs {B}", "",
           f"| | {A} | {B} |", "|---|---:|---:|",
           f"| distinct claims | {c['a']:,} | {c['b']:,} |",
           f"| made by both | {c['both']:,} ({pct(r['match_of_a'])}) | {c['both']:,} ({pct(r['match_of_b'])}) |",
           f"| made only here | {c['only_a']:,} | {c['only_b']:,} |", "",
           "## By predicate", "", f"| predicate | {A} | {B} | both |", "|---|---:|---:|---:|"]
    for p, v in r["predicates"].items():
        out.append(f"| {p or '(none)'} | {v['a']:,} | {v['b']:,} | {v['both']:,} |")
    out += ["", f"## Crosswalk: same subject and object, different predicate ({A} -> {B})", ""]
    if r["crosswalk"]:
        out += [f"| {A} predicate | {B} predicate | pairs |", "|---|---|---:|"]
        out += [f"| {x['a'] or '(none)'} | {x['b'] or '(none)'} | {x['pairs']:,} |" for x in r["crosswalk"][:40]]
    else:
        out.append("none")
    for k, title in (("only_a", f"only {A}"), ("only_b", f"only {B}"), ("both", "both")):
        out += ["", f"## Sample: {title}", ""] + [f"- `{s}` | `{p}` | `{o}`" for s, p, o in r["sample"][k]]
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--names", default="engine,mono")
    ap.add_argument("--predicates", type=Path)
    ap.add_argument("--strip-lists", nargs="?", const=",".join(LISTS), default="")
    ap.add_argument("--report", type=Path)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--sample", type=int, default=10)
    o = ap.parse_args(argv)
    names = tuple(o.names.split(",", 1)) if "," in o.names else (o.names, "b")
    lists = tuple(x for x in o.strip_lists.split(",") if x)
    a = read(o.a, lists, read_map(o.predicates))
    b = read(o.b, lists, None)
    r = compare(a, b, o.sample)
    r["names"] = list(names)
    text = report(r, names)
    if o.report:
        o.report.write_text(text, encoding="utf-8")
    if o.json:
        o.json.write_text(json.dumps(r, indent=1, ensure_ascii=False), encoding="utf-8")
    c = r["claims"]
    print(f"{names[0]} {c['a']:,} claims, {names[1]} {c['b']:,}; both {c['both']:,}; only {names[0]} {c['only_a']:,}, "
          f"only {names[1]} {c['only_b']:,}; {len(r['crosswalk'])} predicate pairs in the crosswalk")
    return 0


if __name__ == "__main__":
    sys.exit(main())