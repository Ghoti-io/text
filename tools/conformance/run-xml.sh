#!/bin/sh
# Score the XML parser against the W3C XML Conformance Test Suite.
#
#   tools/conformance/run-xml.sh
#
# The suite is third_party/xml/xmlconf, put there by tools/xml/fetch.sh from the
# archive pinned in tools/xml/XMLCONF_SHA256. It is not fetched here: a pin is
# the point, and a score about whatever the network returned today is not a
# measurement of this library. If the data is absent this fails and says how to
# get it; it never skips, because a run that scored nothing and exited 0 would
# say the parser conforms having read none of the suite.
#
# XMLCONF_MIN_SCORED sets a floor on how many cases were actually scored, so
# that a truncated corpus cannot pass by being small.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${XMLCONF_SUITE:-$root/third_party/xml/xmlconf}

if [ ! -f "$suite/xmlconf.xml" ]; then
	echo "$suite/xmlconf.xml is not here." >&2
	echo "Fetch the pinned suite:  tools/xml/fetch.sh" >&2
	exit 1
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/xmlconf-runner
cc -O1 -o "$runner" "$root/tools/conformance/xmlconf_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/xmlconf_suite.py" "$suite" "$runner" \
	"$root/tools/conformance/XMLCONF_EXCEPTIONS"
