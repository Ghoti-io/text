#!/bin/sh
# Score the INI reader over this machine's systemd unit files.
#
#   tools/conformance/run-ini-systemd.sh
#
# Acceptance and preservation only, and the reason is worth stating: **this machine
# has no systemd**. The unit files are shipped by other packages, PID 1 is `init`,
# and there is no `systemd-analyze` here to say whether a file is valid - so unlike
# the Desktop Entry corpus, which comes with a validator, this one cannot be asked
# whether accepting a file is right.
#
# The gate prints **which constructs the corpus does not contain**, zeros included,
# because those zeros are the finding: a `;` comment, CRLF, a BOM and every escape
# appear in no file here, so a clean run says nothing at all about them.
# check-ini-systemd-oracle is what does.
#
# INI_SYSTEMD_DIRS overrides the directories searched.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

dirs=${INI_SYSTEMD_DIRS:-/usr/lib/systemd /etc/systemd /lib/systemd}
present=""
for d in $dirs; do
	[ -d "$d" ] && present="$present $d"
done
if [ -z "$present" ]; then
	echo "none of these directories exist: $dirs" >&2
	echo "set INI_SYSTEMD_DIRS to a directory of unit files" >&2
	exit 1
fi

mkdir -p "$root/build"
runner=$root/build/ini-systemd-runner
cc -O1 -o "$runner" "$root/tools/conformance/systemd_unit_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/systemd_unit_suite.py" "$runner" $present
