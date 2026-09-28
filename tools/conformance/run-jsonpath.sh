#!/bin/sh
# Score this library against the JSONPath Compliance Test Suite (RFC 9535).
#
#   tools/conformance/run-jsonpath.sh
#
# The suite is cloned into build/jsonpath-cts on first use and checked out at
# the commit named in tools/conformance/JSONPATH_SUITE_COMMIT, for the reason
# the other four runners pin theirs: a score that moves because somebody
# upstream added a case is not a measurement of this library.
#
# JPC_MIN sets a floor the pass rate must meet, over the cases that were
# attempted. The count that is *not* attempted - a query refused as
# unsupported - is printed beside it, because a percentage over a subset
# means nothing without it.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${JPC_SUITE:-$root/build/jsonpath-cts}
commit=$(cat "$root/tools/conformance/JSONPATH_SUITE_COMMIT")

if [ ! -f "$suite/cts.json" ]; then
	echo "fetching the JSONPath compliance test suite into $suite"
	git clone https://github.com/jsonpath-standard/jsonpath-compliance-test-suite.git "$suite"
fi
if [ "$(git -C "$suite" rev-parse HEAD)" != "$commit" ]; then
	git -C "$suite" fetch --quiet origin "$commit" 2>/dev/null || git -C "$suite" fetch --quiet
	git -C "$suite" checkout --quiet "$commit"
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }

[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
runner=$suite/../jpc-runner
cc -O1 -o "$runner" "$root/tools/conformance/jsonpath_cts.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/jsonpath_cts.py" "$suite" "$runner"
