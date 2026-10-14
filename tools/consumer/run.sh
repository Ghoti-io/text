#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The packaging and consumer test for the XML, HTML, Encoding and markup work, from a fresh clone.
#
#   tools/consumer/run.sh PREFIX [--test] [--deps PKGCONFIGDIR]
#
# Clones this repository (a `git clone`), lays the working tree's own files over the clone (so that an
# uncommitted state is tested as it would be once committed; the clone has no third_party/), runs every
# tools/*/fetch.sh as suite/install.sh does (network), checks the pins independently (check-pins.sh),
# builds and installs into PREFIX (a prefix of its own, with an OWNER file the caller wrote), compiles
# each consumer_*.c out of tree against the installed headers through pkg-config, and runs it.  With
# --test it then runs `make test` in the clone against that prefix, as install.sh --test does.
# Dependencies (cutil, chron, unicode, regex) are found through PKG_CONFIG_PATH, which the caller
# sets, or --deps adds a directory.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX=$1; shift
TEST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --test) TEST=1 ;;
    --deps) shift; PKG_CONFIG_PATH="$1:${PKG_CONFIG_PATH:-}" ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
[ -f "$PREFIX/OWNER" ] || { echo "run.sh: $PREFIX/OWNER is missing: a private prefix says who holds it" >&2; exit 2; }
W=$(mktemp -d "${TMPDIR:-/tmp}/text-exit.XXXXXX")
trap 'rm -rf "$W"' EXIT
echo "== fresh clone into $W/text"
git clone --quiet "$here" "$W/text"
# the working tree's files over the clone (tracked and untracked-unignored; nothing from third_party or build)
( cd "$here" && git ls-files -z --cached --others --exclude-standard | tr '\0' '\n' | while read -r f; do [ -e "$f" ] && printf '%s\0' "$f"; done ) \
  | rsync -a --from0 --files-from=- "$here/" "$W/text/"
( cd "$here" && git ls-files --deleted -z | xargs -0 -r -I{} rm -f "$W/text/{}" )
cd "$W/text"
[ ! -d third_party ] || { echo "run.sh: the clone has third_party/: it is not fresh" >&2; exit 1; }
echo "== fetch scripts (as suite/install.sh runs them)"
for f in tools/*/fetch.sh; do
  echo "-- $f"
  sh "$f"
done
echo "== pins"
sh tools/consumer/check-pins.sh "$W/text"
echo "== build and install into $PREFIX"
make -j"${JOBS:-4}" PREFIX="$PREFIX" install > "$W/build.log" 2>&1 || { tail -30 "$W/build.log"; exit 1; }
tail -3 "$W/build.log"
echo "== consumers"
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:${PKG_CONFIG_PATH:-}"
pc=$(ls "$PREFIX"/share/pkgconfig/ghoti.io-text-*.pc | head -1)
pcname=$(basename "$pc" .pc)
echo "pkg-config module: $pcname"
cflags=$(pkg-config --cflags "$pcname"); libs=$(pkg-config --libs "$pcname")
mkdir -p "$W/consumers"
fails=0
for c in xml html encoding markup; do
  src="$W/text/tools/consumer/consumer_$c.c"
  # out of tree: compiled in a directory that is not the clone, with only the installed include path
  ( cd "$W/consumers" && cc -std=c17 -Wall -Wextra -Werror -pedantic $cflags -o "consumer_$c" "$src" $libs ) \
    || { echo "FAIL consumer_$c: did not compile or link"; fails=$((fails + 1)); continue; }
  # no rpath is relied on: the dependency prefixes are named for the loader
  libpath="$PREFIX/lib/ghoti.io"
  for d in $(echo "$PKG_CONFIG_PATH" | tr ':' ' '); do [ -n "$d" ] && libpath="$libpath:$(dirname "$(dirname "$d")")/lib/ghoti.io"; done
  out=$(cd "$W/consumers" && LD_LIBRARY_PATH="$libpath" ./consumer_$c) \
    || { echo "FAIL consumer_$c: exit $?"; fails=$((fails + 1)); continue; }
  echo "ok   consumer_$c:"; echo "$out" | sed 's/^/       /'
done
[ "$fails" -eq 0 ] || exit 1
if [ "$TEST" = 1 ]; then
  echo "== make test in the clone"
  make -j"${JOBS:-4}" PREFIX="$PREFIX" test > "$W/test.log" 2>&1 && echo "make test: ok" || { tail -60 "$W/test.log"; echo "make test FAILED"; exit 1; }
  grep -E "^(\[  PASSED|.*tests? passed)" "$W/test.log" | tail -3 || true
fi
echo "run.sh: ok"
