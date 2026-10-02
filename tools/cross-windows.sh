#!/bin/sh
#
# Compile every source in this library for Windows with mingw-w64, and check on
# the artifact that the Windows arm of the headers was actually taken.
#
# Until this existed the library had never been built off Linux: five CI jobs,
# all ubuntu-latest. The portability surface is small - four preprocessor
# conditionals, all of them the export macros in include/ghoti.io/text/macros.h
# - but **the `#if` arms you do not take are never parsed**, so "small" and
# "checked" are different claims. One of those arms decides what every exported
# symbol in the library is declared as.
#
# What this is evidence about: the compiler's opinion of the code, for an LLP64
# target where `long` is 4 bytes and `size_t` is 8. It is not a Windows test
# run. The Makefile finds Windows with `findstring MINGW64_NT` on `uname -s`,
# true only under MSYS2, so a cross build from Linux is a *third* target beside
# linux and the native MSYS2 one. Real Windows stays the authority; see
# notes/suite/CONTAINERS.md section 3.
#
# Compile-only, deliberately. Linking would need cutil, chron, unicode and
# regex cross-built too, and the question here is whether this library's own
# sources are portable, which a compile answers and a link would only delay.
#
# Run from the library root, with podman (or docker) and the image from
# notes/suite/CONTAINERS.md:
#
#   podman build -t ghoti-cross-mingw64:deb13 -f <that Containerfile> .
#   tools/cross-windows.sh
#
# Exits 0 if every source compiles and the dllexport arm is confirmed present,
# 1 if anything fails, and 77 if the container or image is missing - which the
# caller is expected to report as a skip rather than a pass.
#
# Copyright 2026 by Corey Pennycuff

set -u

IMAGE=${GTEXT_MINGW_IMAGE:-localhost/ghoti-cross-mingw64:deb13}
WORKSPACE=${GTEXT_WORKSPACE:-../..}

ENGINE=""
for candidate in podman docker; do
  if command -v "$candidate" > /dev/null 2>&1; then
    ENGINE=$candidate
    break
  fi
done
if [ -z "$ENGINE" ]; then
  echo "cross-windows: no podman or docker; not run" >&2
  exit 77
fi
if ! $ENGINE image exists "$IMAGE" > /dev/null 2>&1 \
    && ! $ENGINE image inspect "$IMAGE" > /dev/null 2>&1; then
  echo "cross-windows: $IMAGE is not built; not run" >&2
  echo "  see notes/suite/CONTAINERS.md for the Containerfile" >&2
  exit 77
fi

# The generated header lives under build/, so a tree that has never been built
# natively has nothing to include. Say which, rather than failing on a missing
# <ghoti.io/text/libver_gen.h> fifty times.
if [ ! -d build ]; then
  echo "cross-windows: run a native build first (build/ holds the generated" >&2
  echo "  headers this needs on the include path)" >&2
  exit 1
fi

WS=$(cd "$WORKSPACE" && pwd)
HERE=$(pwd)
REL=$(printf '%s\n' "$HERE" | sed "s#^$WS/##")
if [ "$REL" = "$HERE" ]; then
  echo "cross-windows: $HERE is not inside $WS; set GTEXT_WORKSPACE" >&2
  exit 1
fi

# The script the container runs. Kept here as a heredoc rather than as a second
# tracked file, because the two halves have to agree about paths and a reader
# comparing them across files is a reader who will miss a difference.
SCRIPT=$(cat <<'INNER'
set -u
cd "/work/$REL" || exit 1
CC=x86_64-w64-mingw32-gcc
OBJDUMP=x86_64-w64-mingw32-objdump

INC="-I include/ -I build/linux/release/generated/"
for d in cutil chron unicode regex; do
  INC="$INC -I /work/.local/include/ghoti.io/$d-0"
done

# This library's own flags, less the two that are ELF-only: -fvisibility has no
# meaning for PE, and -fPIC is the default and warned about. Everything that
# could reject code - the warning set and -Werror - is kept, because a cross
# compile with the warnings off would answer a weaker question than the native
# build does.
FLAGS="-pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function"
FLAGS="$FLAGS -std=c17 -O2 -g -DGTEXT_BUILD"

ok=0
bad=0
failed=""
for f in $(find src -name '*.c' | sort); do
  if $CC $FLAGS $INC -c "$f" -o /tmp/x.obj 2> /tmp/err.txt; then
    ok=$((ok + 1))
  else
    bad=$((bad + 1))
    failed="$failed $f"
    printf '\n=== %s ===\n' "$f"
    head -20 /tmp/err.txt
  fi
done

printf '\nmingw-w64: %d sources compiled, %d failed\n' "$ok" "$bad"
if [ -n "$failed" ]; then
  echo "failed:"
  for f in $failed; do echo "  $f"; done
  exit 1
fi

# **Checked on the artifact, not on the exit status.** A clean compile says the
# code is acceptable to the compiler; it does not say which arm of
# `#if defined(_WIN32)` produced it. __declspec(dllexport) writes a .drectve
# section holding one -export: directive per symbol, and an ELF object has no
# such section - so its presence is what distinguishes the Windows arm from the
# __attribute__((visibility)) one having been taken by accident.
$CC $FLAGS $INC -c src/json/json_writer.c -o /tmp/probe.obj || exit 1
if ! $OBJDUMP -h /tmp/probe.obj | grep -q '\.drectve'; then
  echo "NO .drectve section: GTEXT_API did not expand to __declspec(dllexport)," >&2
  echo "  so the Windows arm of macros.h was not the one compiled." >&2
  exit 1
fi
exports=$($OBJDUMP -s -j .drectve /tmp/probe.obj \
  | tr -c '[:print:]\n' '\n' | grep -c 'export:')
echo ".drectve present, carrying $exports export directive lines:"
$OBJDUMP -s -j .drectve /tmp/probe.obj | sed -n '4,6p'

# LLP64, from a binary built for the target. `long` is 4 bytes where it is 8 on
# Linux, which is the one ABI difference most likely to break portable C, and
# the warning set above is what would have caught a conversion that assumed
# otherwise.
printf '#include <stdio.h>\n#include <stddef.h>\nint main(void){printf("long=%%d size_t=%%d ptr=%%d zu=%%zu\\n",(int)sizeof(long),(int)sizeof(size_t),(int)sizeof(void*),(size_t)1234567890123ULL);return 0;}\n' > /tmp/sz.c
if [ -n "${GTEXT_WINE_OUT:-}" ]; then
  $CC -O2 /tmp/sz.c -o "$GTEXT_WINE_OUT/llp64.exe" \
    && echo "built llp64.exe for wine on the host"
fi
INNER
)

OUT=""
WINE_MOUNT=""
if command -v wine > /dev/null 2>&1; then
  OUT=$(mktemp -d)
  WINE_MOUNT="-v $OUT:/out:z"
fi

# shellcheck disable=SC2086
$ENGINE run --rm -v "$WS:/work:ro,z" $WINE_MOUNT \
  -e REL="$REL" -e GTEXT_WINE_OUT="${OUT:+/out}" \
  "$IMAGE" sh -c "$SCRIPT"
status=$?

if [ $status -eq 0 ] && [ -n "$OUT" ] && [ -f "$OUT/llp64.exe" ]; then
  echo "--- the target's own answer, under wine on this host ---"
  WINEDEBUG=-all wine "$OUT/llp64.exe" 2>/dev/null
  echo "--- this host, natively, for contrast ---"
  cc -O2 -x c - -o "$OUT/native" <<'NATIVE' 2>/dev/null \
    && "$OUT/native"
#include <stdio.h>
#include <stddef.h>
int main(void){printf("long=%d size_t=%d ptr=%d zu=%zu\n",
  (int)sizeof(long),(int)sizeof(size_t),(int)sizeof(void*),
  (size_t)1234567890123ULL); return 0;}
NATIVE
  echo "(long differs: 4 on the Windows target, 8 here. That is LLP64, and it"
  echo " is the difference the warning set above is standing guard over.)"
fi

[ -n "$OUT" ] && rm -rf "$OUT"
exit $status
