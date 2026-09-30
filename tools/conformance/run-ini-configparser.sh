#!/bin/sh
# Score the INI reader against `configparser` over this machine's .cfg and .ini files.
#
#   tools/conformance/run-ini-configparser.sh
#
# **The only INI corpus gate whose reference is installed.** `configparser` is in the
# standard library of the python3 that runs the scorer, so unlike the Desktop Entry
# corpus (a validator but no agreeing value reader) and the systemd corpus (neither,
# because this machine has no systemd) this one can be asked all three questions:
# whether we accept the same files, whether every value agrees, and whether the
# rewrite is byte for byte.
#
# The corpus is selected by extension and then **partitioned by the reference**: a
# `.cfg` is not necessarily one of these documents at all - most `lit.cfg` files are
# Python scripts - and the ones the reference refuses are the only real refusals this
# format's corpus offers. They stay in the denominator.
#
# INI_CONFIGPARSER_DIRS overrides the directories searched.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

dirs=${INI_CONFIGPARSER_DIRS:-/etc /usr/share /usr/lib}
present=""
for d in $dirs; do
	[ -d "$d" ] && present="$present $d"
done
if [ -z "$present" ]; then
	echo "none of these directories exist: $dirs" >&2
	echo "set INI_CONFIGPARSER_DIRS to a directory of .cfg or .ini files" >&2
	exit 1
fi

mkdir -p "$root/build"
runner=$root/build/ini-configparser-runner
cc -O1 -o "$runner" "$root/tools/conformance/configparser_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/configparser_suite.py" "$runner" $present
