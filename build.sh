#!/bin/sh
# Build Laplace: the extension and the engine, each with Laplace-Native built as part of it.
#   ./build.sh            build both
#   ./build.sh install    also install the extension into PostgreSQL's directories
set -e
. "$(dirname "$0")/laplace.env"
for r in Laplace-postgres Laplace-Engine; do
  cmake -S "$LAPLACE_SRC/$r" -B "$LAPLACE_BUILD/$r/icx-release" -G Ninja -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build "$LAPLACE_BUILD/$r/icx-release"
done
# Installed by rename, never by overwriting in place: a backend that has the old library mapped keeps the old inode.
[ "$1" = install ] && { stage=$(mktemp -d); DESTDIR=$stage cmake --install "$LAPLACE_BUILD/Laplace-postgres/icx-release" >/dev/null
  (cd "$stage" && find . -type f) | while read -r f; do mv -f "$stage/$f" "/${f#./}"; done; rm -rf "$stage"; }
"$LAPLACE_BUILD/Laplace-Engine/icx-release/laplace" 2>&1 | tail -5
