#!/usr/bin/env bash
# Retain the established CI entry point; the real fixture is portable.
set -euo pipefail
exec python3 "$(dirname "$0")/cached-key-reader.py" "${1:?usage: tests/cached-key-reader.sh path/to/laplace}"
