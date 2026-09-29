#!/bin/sh
# Score the INI reader against editorconfig-core-test's 34 parser assertions.
#
#   tools/conformance/run-ini-editorconfig.sh
#
# The suite is cloned into build/editorconfig-core-test on first use and checked
# out at the commit named in tools/conformance/EDITORCONFIG_SUITE_COMMIT. This is
# the **only normative INI conformance suite that exists** - specification 0.17.2
# says a conforming core "must pass the tests in the core-tests repository" - so
# unlike every other INI target here the number is a pass count rather than an
# agreement with a reference, and the floor is all of them.
#
# Only 34 of the suite's 202 assertions are about the grammar; the rest test a
# filepath glob matcher, file discovery and a command line. EC_MIN sets the floor,
# which defaults to every assertion found.
#
# **The control run is not optional and runs first.** The harness has to do the
# glob matching and the section merge itself, because there is no `editorconfig`
# binary here and because a glob matcher is not this library's job. So it is first
# run against a throwaway parser written in Python, which must score 34 of 34: if
# it does not, the harness is broken and the library's score would mean nothing.
# A gate whose harness has never been shown to work cannot fail for the right
# reason.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${EC_SUITE:-$root/build/editorconfig-core-test}
commit=$(cat "$root/tools/conformance/EDITORCONFIG_SUITE_COMMIT")

if [ ! -d "$suite/parser" ]; then
	echo "fetching editorconfig-core-test into $suite"
	git clone https://github.com/editorconfig/editorconfig-core-test.git "$suite"
fi
# Pinned, for the reason run-csv.sh gives: a conformance number against a moving
# suite is not a number. The suite's own last push was 2025-04-21.
if [ "$(git -C "$suite" rev-parse HEAD)" != "$commit" ]; then
	git -C "$suite" fetch --quiet origin "$commit" 2>/dev/null || git -C "$suite" fetch --quiet
	git -C "$suite" checkout --quiet "$commit"
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/ini-editorconfig-runner
cc -O1 -o "$runner" "$root/tools/conformance/editorconfig_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

echo "control: the harness's own glob matcher and merge, against a Python parser"
python3 "$root/tools/conformance/editorconfig_suite.py" "$suite" "$runner" \
	--control
echo "library:"
exec python3 "$root/tools/conformance/editorconfig_suite.py" "$suite" "$runner"
