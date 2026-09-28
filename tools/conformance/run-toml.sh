#!/bin/sh
# Score this library's TOML parser against toml-test.
#
#   tools/conformance/run-toml.sh
#
# The suite is cloned into build/toml-test on first use and checked out at the
# commit named in tools/conformance/TOML_SUITE_COMMIT.  TOML_MIN sets a floor
# the score must meet; TOML_REPORT names a file to write the failing cases to;
# TOML_SUITE_VERSION picks which of the suite's two manifests to score against
# and defaults to 1.0.0, which is what this parser implements.  Mirrors
# run-json.sh.
#
# Two of the modes read this library's output with `tomllib`, which is reached
# through tools/oracle/ like every other reference here, so this script needs
# whatever GHOTI_ORACLE_* the Makefile is passing. GHOTI_ORACLE_MODE=host uses
# this machine's own interpreter and says so.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${TOML_SUITE:-$root/build/toml-test}
commit=$(cat "$root/tools/conformance/TOML_SUITE_COMMIT")

if [ ! -d "$suite/tests/valid" ]; then
	echo "fetching toml-test into $suite"
	git clone https://github.com/toml-lang/toml-test.git "$suite"
fi
# Pinned, for the reason run-json.sh gives: a percentage that moves because
# somebody upstream added a case is not a measurement of this library.
if [ "$(git -C "$suite" rev-parse HEAD)" != "$commit" ]; then
	git -C "$suite" fetch --quiet origin "$commit" 2>/dev/null || git -C "$suite" fetch --quiet
	git -C "$suite" checkout --quiet "$commit"
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
runner=$suite/../toml-runner
cc -O1 -o "$runner" "$root/tools/conformance/toml_test_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

exec python3 "$root/tools/conformance/toml_test_suite.py" "$suite" "$runner"
