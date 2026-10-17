#!/usr/bin/env bash
# Whether a change can move what an ingest records, and so whether the parity harness runs on it: the changed paths,
# one a line, on stdin; prints true or false. A line "*" is a change that could not be listed, and runs it. What can
# move an ingest is the Engine's source and build (src/, CMakeLists.txt, CMakePresets.json), what it reads (recipes/,
# firmware/), its tools (tools/: the harness, the grammars) and this workflow. Tests, documentation and the rest do not.
set -euo pipefail
if grep -Eq '^([*]$|src/|recipes/|firmware/|tools/|CMakeLists[.]txt$|CMakePresets[.]json$|[.]github/workflows/laplace[.]yml$)'; then
  echo true
else
  echo false
fi
