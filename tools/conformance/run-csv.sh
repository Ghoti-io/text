#!/bin/sh
# Score this library against csv-spectrum.
#
#   tools/conformance/run-csv.sh
#
# The suite is cloned into build/csv-spectrum on first use and checked out at
# the commit named in tools/conformance/CSV_SUITE_COMMIT.  CSS_MIN sets a floor
# the pass rate must meet, CSS_MIN_CORPUS a floor on how much of the corpus was
# checked at all.  Mirrors run.sh and
# run-json.sh, which do the same for YAML and JSON.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${CSS_SUITE:-$root/build/csv-spectrum}
commit=$(cat "$root/tools/conformance/CSV_SUITE_COMMIT")

if [ ! -d "$suite/csvs" ]; then
	echo "fetching csv-spectrum into $suite"
	git clone https://github.com/maxogden/csv-spectrum.git "$suite"
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
runner=$suite/../css-runner
cc -O1 -o "$runner" "$root/tools/conformance/csv_spectrum.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/csv_spectrum.py" "$suite" "$runner"
