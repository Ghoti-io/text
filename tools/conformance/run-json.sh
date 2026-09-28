#!/bin/sh
# Score this library against JSONTestSuite.
#
#   tools/conformance/run-json.sh
#
# The suite is cloned into build/json-test-suite on first use and checked out at
# the commit named in tools/conformance/JSON_SUITE_COMMIT.  JTS_MIN sets a floor
# the score must meet; JTS_REPORT names a file to
# write the failing cases to.  Mirrors run.sh, which does the same for YAML.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${JTS_SUITE:-$root/build/json-test-suite}
commit=$(cat "$root/tools/conformance/JSON_SUITE_COMMIT")

if [ ! -d "$suite/test_parsing" ]; then
	echo "fetching JSONTestSuite into $suite"
	git clone https://github.com/nst/JSONTestSuite.git "$suite"
fi
# The corpus is pinned. A score is a percentage, and a percentage that moves
# because somebody upstream added or changed cases is not a measurement of
# this library - run-json-schema.sh has pinned its suite for that reason since
# it was written, and these three were still tracking whatever the default
# branch held on the day they ran.
if [ "$(git -C "$suite" rev-parse HEAD)" != "$commit" ]; then
	git -C "$suite" fetch --quiet origin "$commit" 2>/dev/null || git -C "$suite" fetch --quiet
	git -C "$suite" checkout --quiet "$commit"
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }

[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
runner=$suite/../jts-runner
# The same rpath the Makefile links with, so the runner finds cutil without
# the caller having to set LD_LIBRARY_PATH.
cc -O1 -o "$runner" "$root/tools/conformance/json_test_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/json_test_suite.py" "$suite" "$runner"
