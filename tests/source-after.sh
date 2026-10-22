#!/bin/bash
# A source's "after" names what must be recorded before it, and an ingest of a source by its name takes in everything
# it comes after (ingest.c). BRIEF.md: "Nothing is order-dependent: every node is content-derived, so what two sources
# say links by entity collision whichever is ingested first, and no placeholder waits for another source"; Laplace-Wiki
# Storage/Identity.md: "the same content always has the same ID". A recipe refers only to its own source's keys, so a
# source comes after another only for what that one must say first, never for where it stands in recipes/order. ConceptNet named wiktionary, and Tatoeba
# wiktionary and conceptnet: an ingest of ConceptNet took in all of the Wiktionary first (about 1 TB at the recorded
# measures), for nothing ConceptNet reads. recipes/order still puts them in the same order.
#   tests/source-after.sh path/to/laplace
set -euo pipefail
L=${1:?usage: tests/source-after.sh path/to/laplace}; here=$(cd "$(dirname "$0")" && pwd)
export LAPLACE_RECIPES=${LAPLACE_RECIPES:-$here/../recipes}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/source-after.XXXXXX"); trap 'rm -rf "$scratch"' EXIT
export LAPLACE_WORK=$scratch LAPLACE_DATA=$scratch
fail(){ echo "source-after: $*" >&2; exit 1; }

# the sources a plan of SOURCE takes in, by name, one on a line: the rows of the plan's table
takes(){ set +e; local out; out=$(env -u LAPLACE_INGEST_ONE "$L" ingest --plan "$1" 2>&1); set -e
         awk '$1 ~ /^[0-9]+$/ && $2 != "sources" {print $2}' <<<"$out" | sort; }

got=$(takes conceptnet | tr '\n' ' '); want="conceptnet iso-639 unicode "
[ "$got" = "$want" ] || fail "an ingest of conceptnet takes in [$got], not [$want]"
got=$(takes tatoeba | tr '\n' ' '); want="iso-639 tatoeba unicode "
[ "$got" = "$want" ] || fail "an ingest of tatoeba takes in [$got], not [$want]"

# recipes/order is unchanged by it: an ingest of every source still has wiktionary before conceptnet before tatoeba
order=$(grep -n -x -E 'wiktionary|conceptnet|tatoeba' "$LAPLACE_RECIPES/order" | cut -d: -f2 | tr '\n' ' ')
[ "$order" = "wiktionary conceptnet tatoeba " ] || fail "recipes/order: [$order]"
echo "source-after: conceptnet and tatoeba take in only unicode and iso-639; recipes/order keeps their places"
