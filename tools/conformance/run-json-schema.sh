#!/bin/sh
# Score this library's schema engine against JSON-Schema-Test-Suite.
#
#   tools/conformance/run-json-schema.sh
#
# The suite is cloned into build/json-schema-test-suite on first use and
# checked out at the commit named in tools/conformance/JSON_SCHEMA_COMMIT.
# The commit is pinned rather than tracking the default branch for the reason
# run-json.sh does not need to care about: this score is a percentage, and a
# percentage that moves because somebody else added test cases is not a
# measurement of anything here. `regex` pins the same commit, so the two
# libraries' numbers are about the same corpus.
#
# JSS_MIN sets a floor the required-suite rate must meet; JSS_REPORT names a
# file to write the wrong answers to; JSS_DRAFT picks the draft directory.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
commit=$(cat "$root/tools/conformance/JSON_SCHEMA_COMMIT")
suite=${JSS_SUITE:-$root/build/json-schema-test-suite}
draft=${JSS_DRAFT:-draft2020-12}

# Almost no schema in the suite carries a `$schema`: the draft it is written
# against is the directory it sits in.  An implementation is expected to be
# told, and one that is not reads every file as its own default - so draft7's
# `$ref`-with-siblings and `additionalItems` cases were being scored against
# 2020-12's rules, which is a measurement of the harness rather than of the
# engine.
case "$draft" in
draft2020-12) JSS_DIALECT="https://json-schema.org/draft/2020-12/schema" ;;
draft2019-09) JSS_DIALECT="https://json-schema.org/draft/2019-09/schema" ;;
draft7)       JSS_DIALECT="http://json-schema.org/draft-07/schema#" ;;
draft6)       JSS_DIALECT="http://json-schema.org/draft-06/schema#" ;;
draft4)       JSS_DIALECT="http://json-schema.org/draft-04/schema#" ;;
*)            JSS_DIALECT="" ;;
esac
export JSS_DIALECT

if [ ! -d "$suite/.git" ]; then
	echo "fetching JSON-Schema-Test-Suite into $suite"
	git clone https://github.com/json-schema-org/JSON-Schema-Test-Suite.git "$suite"
fi
if [ "$(git -C "$suite" rev-parse HEAD)" != "$commit" ]; then
	git -C "$suite" fetch --quiet origin "$commit" 2>/dev/null || git -C "$suite" fetch --quiet
	git -C "$suite" checkout --quiet "$commit"
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }

[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
pc="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
cflags=$(PKG_CONFIG_PATH="$pc" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$pc" pkg-config --libs $DEP_PCS)

# `pattern` and `patternProperties` are regular expressions, and this library
# has no engine - the caller supplies one. ghoti.io-regex is the one written
# for it, so the score is measured with it when it is installed. Without it
# those groups are refused like any other unenforceable keyword, and the
# report says so rather than quietly scoring a smaller corpus.
if PKG_CONFIG_PATH="$pc" pkg-config --exists ghoti.io-regex-0; then
	cflags="$cflags -DGTEXT_CONFORMANCE_HAVE_REGEX $(PKG_CONFIG_PATH="$pc" pkg-config --cflags ghoti.io-regex-0)"
	libs="$libs $(PKG_CONFIG_PATH="$pc" pkg-config --libs ghoti.io-regex-0)"
	echo "regular expressions: ghoti.io-regex"
	# The floor is only meaningful with the provider: without one, `pattern`
	# and `patternProperties` are refused and the score legitimately drops,
	# and a gate that fired on that would be reporting a missing package as a
	# regression.
	#
	# It is the whole required suite. A floor set just under the current score
	# leaves room to lose an assertion without anybody noticing, and there is
	# no longer a required assertion this engine is entitled to miss.
	: "${JSS_MIN:=100}"
	export JSS_MIN
else
	echo "regular expressions: none installed - pattern groups will not run"
fi

conformance_library
runner=$root/build/json-schema-runner
cc -O1 -o "$runner" "$root/tools/conformance/json_schema_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

JSS_COMMIT="$commit" JSS_REMOTES="$suite/remotes" exec python3 "$root/tools/conformance/json_schema_suite.py" \
	"$suite" "$runner" --draft "$draft"
