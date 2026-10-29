#!/usr/bin/env python3
"""The actual query firmware follows producer declarations, including renamed ones."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

engine = str(Path(sys.argv[1]).resolve())
root = Path(__file__).resolve().parents[1]
env = dict(os.environ, LAPLACE_CONNINFO="host=127.0.0.1 port=1 connect_timeout=1")


def run(*args, expected=0):
    result = subprocess.run([engine, *args], env=env, text=True, encoding="utf-8", capture_output=True, timeout=30)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout


def entity(text):
    return json.loads(run("text", "--segments", text))["entity"]


with tempfile.TemporaryDirectory(prefix="firmware-relations-", dir=env.get("LAPLACE_WORK")) as scratch:
    recipes = Path(scratch) / "recipes"
    shutil.copytree(root / "recipes", recipes)
    env["LAPLACE_RECIPES"] = str(recipes)
    fw = Path(scratch) / "query.firmware"
    fw.write_text("for pull\nup @wn-lmf/Sense\ntake chain 1 @wn-lmf/Sense @wn-lmf/Definition | @conllu/LEMMA\n", encoding="utf-8")

    def policy():
        return json.loads(run("firmware", "--firmware", str(fw)))

    result = policy()
    assert result["up"][0]["entity"] == entity("lexical sense"), result
    assert result["chains"][0][1]["entity"] == entity("definition of the synset"), result
    assert result["chains"][1][0]["entity"] == entity("Lemma or stem of word form"), result
    assert result["chains"][0][0] == result["up"][0], result

    producer = recipes / "wn-lmf.recipe"
    before = producer.read_text(encoding="utf-8")
    assert 'relation "lexical sense" Sense' in before
    producer.write_text(before.replace('relation "lexical sense" Sense', 'relation "lexical sense renamed by producer" Sense'), encoding="utf-8")
    result = policy()
    assert result["up"][0]["entity"] == entity("lexical sense renamed by producer"), result
    assert result["chains"][0][0] == result["up"][0], result

    fw.write_text("for pull\nup literal\ntake chain 1 literal\n", encoding="utf-8")
    assert policy()["up"][0]["entity"] == entity("literal")
    for invalid in ("@wn-lmf/missing", "@missing/Sense", "@wn-lmf", "@/Sense", "@wn-lmf/"):
        fw.write_text(f"for pull\nup {invalid}\n", encoding="utf-8")
        run("firmware", "--firmware", str(fw), expected=2)
        run("turn", "--read", "--firmware", str(fw), "hello", expected=2)
        run("pull", "--firmware", str(fw), "hello", expected=2)

env["LAPLACE_RECIPES"] = str(root / "recipes")
default = json.loads(run("firmware", "--firmware", str(root / "firmware/program.firmware")))
assert default["up"][0]["name"] == "@wn-lmf/Sense", default
print("firmware relations: producer wording, chained references, literals and fail-before-connect checks passed")
