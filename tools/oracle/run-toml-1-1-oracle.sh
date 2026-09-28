#!/bin/sh
# Compare this library's TOML v1.1.0 reading against toml++'s, over the
# relaxations both implement.
#
#   tools/oracle/run-toml-1-1-oracle.sh
#
# Mirrors run-toml-oracle.sh: the library's half is the toml-test runner, which
# already emits the one encoding that tells an integer from a float and a
# date-time from a string, and the reference's half runs inside its pinned image.
# tools/oracle/toml_1_1_diff.py is where the overlap between toml++'s unreleased
# set and v1.1.0's is written down.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
runner=$root/build/toml-oracle-runner
cc -O1 -o "$runner" "$root/tools/conformance/toml_test_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/oracle/oracle_run.py" tomlpp -- \
	python3 "$root/tools/oracle/toml_1_1_diff.py" "$runner"
