#!/bin/sh
# Laplace, from an empty server to loaded, indexed and measured content: the five steps of Deployment, in order.
# Everything it does can be done again: what is built is not rebuilt, what is recorded is passed over by its bytes,
# and a run that was cut off is taken up by running it again.
#
#   ./deploy.sh                                      the database laplace.env names
#   LAPLACE_CONNINFO="... dbname=NAME" ./deploy.sh   another database; it is made if the server does not have it
#
# What it needs first: PostgreSQL 18 with PostGIS running, a role that may create databases and extensions, and the
# data under $LAPLACE_DATA (Operations: Setup, Builds). What each step said is kept in $LAPLACE_WORK/logs/deploy.
set -e
here=$(cd "$(dirname "$0")" && pwd)
. "$here/laplace.env"
log=$LAPLACE_WORK/logs/deploy/$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$log"
step(){ name=$1; shift; echo; echo "=== $name   $(date -u +%H:%M:%S)"
        { "$@" 2>&1; echo $? > "$log/$name.exit"; } | grep --line-buffered -v "^OMP: " | tee "$log/$name.log"
        [ "$(cat "$log/$name.exit")" = 0 ] || { echo "=== $name did not finish; what it said is in $log/$name.log"; exit 1; }; }

step build      "$here/build.sh" install
[ -s "$LAPLACE_TIER0" ] || step tier0 laplace tier0
[ -s "${LAPLACE_TIER0%.bin}.flags" ] || step flags laplace flags
step deploy     laplace deploy
step ingest     laplace ingest
step status     laplace status
step bench      laplace bench
ss -ltn 2>/dev/null | grep -q "0.0.0.0:5432\|\*:5432" || echo "
The server listens on localhost only: sudo ./setup.sh declares the machine, this among it (the one step that needs root)."
echo; echo "=== done   $(date -u +%H:%M:%S)   $log"
