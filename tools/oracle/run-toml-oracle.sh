#!/bin/sh
# Compare this library's TOML reader against a pinned `tomllib`, over generated
# documents.
#
#   tools/oracle/run-toml-oracle.sh
#
# The library's half of the comparison is the toml-test runner, because it
# already emits toml-test tagged JSON - the one encoding that tells an integer
# from a float and a date-time from a string. Building a second driver that said
# the same thing differently would be a second thing to keep right.
#
# The reference's half runs inside the pinned image; see
# tools/oracle/containers/IMAGES for which and why.
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

exec python3 "$root/tools/oracle/oracle_run.py" tomllib -- \
	python3 "$root/tools/oracle/toml_diff.py" "$runner"
