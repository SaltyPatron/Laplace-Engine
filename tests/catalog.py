#!/usr/bin/env python3
"""Check the actual CLI inventory against every declared source and filename match."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

engine = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1] / "recipes"
env = dict(os.environ, LAPLACE_CONNINFO="host=invalid", LAPLACE_TIER0="/no-cache",
           LAPLACE_GRAMMARS="/no-grammars")
result = subprocess.run([str(engine), "ingest", "-r", str(root), "--catalog"],
                        env=env, capture_output=True, text=True, check=True)
catalog = json.loads(result.stdout)
sources = {s["name"]: s for s in catalog["sources"]}
assert set(sources) == {p.parent.name for p in root.glob("*/source")}
assert len(sources) == len(catalog["sources"])
accepted = set()
for path in root.glob("*/source"):
    source = sources[path.parent.name]
    aliases = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = re.split(r"(?:^|\s)#", line, maxsplit=1)[0].split()
        if fields and fields[0] == "aliases":
            aliases.extend(fields[1:])
    assert source["aliases"] == aliases, path
    for name in [source["name"], *aliases]:
        assert name not in accepted, name
        accepted.add(name)
    earlier = [s["name"] for s in catalog["sources"][:list(sources).index(source["name"])]]
    assert set(source["after"]) <= set(earlier), path
    recipes = {r["name"]: r for r in source["recipes"]}
    for recipe in path.parent.glob("*.recipe"):
        lines = [line.split() for line in recipe.read_text(encoding="utf-8").splitlines()]
        name = next(fields[1] for fields in lines if fields and fields[0] == "name")
        matches = [word for fields in lines if fields and fields[0] == "match" for word in fields[1:]]
        assert recipes[name]["match"] == matches, recipe
assert len(catalog["formats"]) == len(list(root.glob("*.recipe")))
print(f"CLI catalog: {len(sources)} sources, {len(accepted) - len(sources)} aliases, "
      f"{len(catalog['formats'])} file formats; complete matches; no database, cache or grammars required")
