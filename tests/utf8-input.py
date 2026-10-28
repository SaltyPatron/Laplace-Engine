#!/usr/bin/env python3
"""CLI identities agree with raw UTF-8 input; Unicode paths open byte-exact content."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

engine, reference = (str(Path(value).resolve(strict=True)) for value in sys.argv[1:3])
for content in ("Å", "Ж", "東", "😀", "e\u0301", "☕\ufe0f", "𐐀"):
    expected = subprocess.check_output([reference, "--id-stdin"], input=content.encode("utf-8")).decode("ascii").strip()
    result = subprocess.run([engine, "text", content], capture_output=True, text=True, encoding="utf-8")
    assert result.returncode == 0, result.stderr
    found = re.search(r"(?m)^entity\s+([a-f0-9]{32})", result.stdout)
    assert found and found[1] == expected, (content, expected, result.stdout)

with tempfile.TemporaryDirectory(prefix="utf8-input-", dir=os.environ.get("LAPLACE_WORK")) as scratch:
    path = Path(scratch) / "native_Ж_東_😀.txt"
    path.write_bytes("A Ж 東 😀 e\u0301.\n".encode("utf-8"))
    environment = dict(os.environ, LAPLACE_CONNINFO="host=127.0.0.1 port=1 dbname=unavailable connect_timeout=1")
    result = subprocess.run([engine, "ingest", "--no-load", "-j", "2", str(path)], env=environment,
                            capture_output=True, text=True, encoding="utf-8", timeout=60)
    assert result.returncode == 0 and "mismatched 0" in result.stdout, result.stderr + result.stdout
print("UTF-8 input: scalar and grapheme identities agree with raw bytes; Unicode path roundtrip passed")
