#!/bin/sh
# wine needs three writable things and a read-only rootfs is how every other
# image in this directory runs, so all three are pointed into /tmp:
#
#   WINEPREFIX       - wine takes a lock inside it, so it cannot be the baked one
#   XDG_RUNTIME_DIR  - unset, wine prints "invalid or not set" to stderr for
#                      every call and the messages interleave with the driver's
#                      stdout
# TMPDIR is deliberately **not** set, and that is a measurement rather than a
# preference: exporting TMPDIR=/tmp makes this wine abort with
# "free(): invalid pointer" before the driver's first instruction runs. Left
# unset, GetTempPath resolves inside the prefix - C:\users\<user>\Temp - which is
# writable and is where the document belongs anyway.
#
# The prefix is copied from the one built at image time rather than bootstrapped
# here: wineboot on a cold prefix takes seconds, and this runs once per gate, not
# once per document.
set -e
export WINEPREFIX=${WINEPREFIX:-/tmp/wine-run}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg}
export WINEDEBUG=${WINEDEBUG:--all}
mkdir -p "$XDG_RUNTIME_DIR"
if [ ! -d "$WINEPREFIX/drive_c" ]; then
  cp -a /wine "$WINEPREFIX"
fi
# stderr is discarded on purpose. wine writes fixme and err lines for a 32-bit
# rundll32 this image does not install, and the driver's answers are on stdout;
# keeping both would reproduce the interleaving failure this repository has a
# standing note about. A driver failure still shows as a missing answer line,
# which the harness reports.
exec /usr/lib/wine/wine64 /usr/local/bin/win32-profile-driver.exe "$@" 2>/dev/null
