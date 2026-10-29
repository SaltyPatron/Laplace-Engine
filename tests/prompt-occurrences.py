#!/usr/bin/env python3
"""Forward operands follow canonical word boundaries, independently of DAG height."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

engine = str(Path(sys.argv[1]).resolve(strict=True))
environment = dict(os.environ, LAPLACE_CONNINFO="host=127.0.0.1 port=1 dbname=unavailable connect_timeout=1")


def run(*args):
    return subprocess.run([engine, *args], env=environment, capture_output=True,
                          text=True, encoding="utf-8", timeout=30)


def identity(text):
    result = run("text", text)
    assert result.returncode == 0, result.stderr
    return re.search(r"(?m)^entity\s+([a-f0-9]{32})", result.stdout)[1]


cases = [
    ("What is a cat?", ["What", " ", "is", " ", "a", " ", "cat", "?"]),
    ("cat", ["cat"]),
    ("a a", ["a", " ", "a"]),
    ("cafe\u0301 猫", ["cafe\u0301", " ", "猫"]),
    ("One.\n\nTwo!", ["One", ".", "\n", "\n", "Two", "!"]),
]
ids = {part: identity(part) for _, parts in cases for part in parts}
for text, parts in cases:
    result = run("text", "--segments", text)
    assert result.returncode == 0, result.stderr
    observed = json.loads(result.stdout)
    assert observed["entity"] == identity(text), (text, observed)
    assert observed["segments"] == [ids[part] for part in parts], (text, observed)

# This previously silently truncated to 256, then reached the database. Detect
# the real occurrence count and reject it before any database access or writes.
result = run("turn", "--read", " ".join(["word"] * 130))
assert result.returncode == 2 and "259 occurrences" in result.stderr, result.stdout + result.stderr
assert "connection" not in result.stderr.lower(), result.stderr
print("prompt occurrences: Native boundaries, repeated identities, Unicode, paragraphs and explicit window overflow")
