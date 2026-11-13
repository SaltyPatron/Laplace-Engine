#!/usr/bin/env bash
# Whether a change to main is deployed: whether it can move what the deployed system does or what its database checks
# check. The changed paths, one a line, on stdin; prints true or false. A line "*" is a change that could not be
# listed, and deploys. The Engine's source and build (src/, cmake/, CMakeLists.txt, CMakePresets.json), what it reads
# (recipes/, firmware/), its tools (tools/: the grammars), its database checks (tests/deployed/, run by
# Laplace-Operations against the deployed database) and this workflow deploy. Other tests and documentation do not.
set -euo pipefail
if grep -Eq '^([*]$|src/|cmake/|recipes/|firmware/|tools/|tests/deployed/|CMakeLists[.]txt$|CMakePresets[.]json$|[.]github/workflows/laplace[.]yml$)'; then
  echo true
else
  echo false
fi
