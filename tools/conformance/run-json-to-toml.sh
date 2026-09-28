#!/bin/sh
# Score gtext_json_to_toml() over JSONTestSuite's documents.
#
#   tools/conformance/run-json-to-toml.sh
#
# The corpus is the one run-json.sh clones and pins - build/json-test-suite at
# the commit in tools/conformance/JSON_SUITE_COMMIT - because the documents that
# reach the three places TOML is narrower than JSON are JSON documents, and this
# repository already has a pinned population of those.
#
# It exists because the `via json` mode of run-toml.sh converts only documents
# that came *from* TOML, so no `null`, no integer wider than int64_t, and no
# root that is not a table appears anywhere in it. Those were reached by unit
# tests and by nothing with a population behind it.
#
# JTT_REPORT names a file to write any disagreements to. There is no floor: the
# oracle names one right answer per case.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${JTS_SUITE:-$root/build/json-test-suite}
commit=$(cat "$root/tools/conformance/JSON_SUITE_COMMIT")

if [ ! -d "$suite/test_parsing" ]; then
	echo "fetching JSONTestSuite into $suite"
	git clone https://github.com/nst/JSONTestSuite.git "$suite"
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
runner=$suite/../json-to-toml-runner
cc -O1 -o "$runner" "$root/tools/conformance/json_to_toml_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

JTS_COMMIT="$commit" exec python3 \
	"$root/tools/conformance/json_to_toml_suite.py" "$suite" "$runner"
