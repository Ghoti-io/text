#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
# Score the HTML serializer against html5lib-tests' serializer vectors, and round-trip the tree vectors.
#
#   tools/conformance/run-html-serializer.sh [--controls | --control NAME | --sanitize]
#
# The vectors are third_party/html/html5lib-tests/serializer and the trees are the tree-construction vectors
# third_party/html/wpt/html/syntax/parsing/resources, both put there by tools/html/fetch.sh at the commits pinned in
# tools/html. They are not fetched here: a pin is the point. If the data is absent this fails and says how to get
# it; it never skips.
#
# The scorer has floors (HTML_SERIALIZER_MIN_VECTORS, _MIN_PASS, _MIN_STABLE), so that a truncated corpus cannot pass
# by being small whether or not this script is run through make.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${HTML_SERIALIZER_SUITE:-$root/third_party/html/html5lib-tests/serializer}
trees=${HTML_SERIALIZER_TREES:-$root/third_party/html/wpt/html/syntax/parsing/resources}

if [ ! -f "$suite/core.test" ]; then
	echo "$suite/core.test is not here." >&2
	echo "Fetch the pinned vectors:  tools/html/fetch.sh" >&2
	exit 1
fi
if [ ! -f "$trees/tests1.dat" ]; then
	echo "$trees/tests1.dat is not here." >&2
	echo "Fetch the pinned vectors:  tools/html/fetch.sh" >&2
	exit 1
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/html-serializer-runner
cc -O1 -D_GNU_SOURCE $CFLAGS -o "$runner" "$root/tools/conformance/html_serializer_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

suite_py=$root/tools/conformance/html_serializer_suite.py
exceptions=$root/tools/conformance/HTML_SERIALIZER_EXCEPTIONS
extra=$root/tools/conformance/html_tree_extra.dat
export HTML_SERIALIZER_MIN_VECTORS=${HTML_SERIALIZER_MIN_VECTORS:-248}
export HTML_SERIALIZER_MIN_PASS=${HTML_SERIALIZER_MIN_PASS:-241}
export HTML_SERIALIZER_MIN_STABLE=${HTML_SERIALIZER_MIN_STABLE:-3950}

# --sanitize: the same gate with the library and the runner built with AddressSanitizer and UndefinedBehaviorSanitizer,
# the library from its sources (the archive is not instrumented).
if [ "$1" = "--sanitize" ]; then
	san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
	objdir=$(mktemp -d)
	trap 'rm -rf "$objdir"' EXIT
	i=0
	for src in $(find "$root/src" -name '*.c'); do
		i=$((i + 1))
		echo "cc -std=gnu17 $san -DGTEXT_BUILD -I\"$root/include\" -I\"$generated\" $cflags -c \"$src\" -o \"$objdir/$i.o\""
	done | xargs -P "$(nproc)" -I{} sh -c '{}'
	cc -D_GNU_SOURCE $san -o "$objdir/runner" "$root/tools/conformance/html_serializer_suite.c" "$objdir"/*.o \
		-I"$root/include" -I"$generated" $cflags $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	echo "html serializer gate under ASan + UBSan"
	LD_PRELOAD= ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		HTML_SERIALIZER_RUNNER_TIMEOUT=1800 python3 "$suite_py" "$suite" "$objdir/runner" "$exceptions" "$trees" "$extra"
	exit $?
fi

# --control NAME runs one control of the list below and no other (a plant is not even built for the others).
only=
if [ "$1" = "--control" ]; then
	only=$2
	[ -n "$only" ] || { echo "usage: run-html-serializer.sh --control NAME" >&2; exit 2; }
elif [ "$1" != "--controls" ]; then
	exec python3 "$suite_py" "$suite" "$runner" "$exceptions" "$trees" "$extra"
fi

# --controls: the gate is made to fail on purpose, and must, each time, say why. A gate whose assertion no longer
# reaches its subject prints the same clean line as one that passes.
scratch=$(mktemp -d)
log=$(mktemp)
ran=0
trap 'rm -rf "$scratch" "$log"' EXIT

# expect <what> <heading> <command...>: the command must fail, and say the heading.
expect() {
	what=$1 want=$2
	shift 2
	if [ -n "$only" ] && [ "$only" != "$what" ]; then
		return 0
	fi
	ran=$((ran + 1))
	status=0
	"$@" </dev/null >"$log" 2>&1 || status=$?
	if [ "$status" = 0 ] || ! grep -q "$want" "$log"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		tail -30 "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

# The vectors lie: one expected string is changed.
mkdir -p "$scratch/vectors"
cp "$suite"/*.test "$scratch/vectors/"
python3 - "$scratch/vectors/core.test" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
old = '"expected": ["<span title=\\"foo\\">"]'
assert old not in text
old = '"expected": ["<img>"]'
assert old in text
open(path, "w", encoding="utf-8").write(text.replace(old, '"expected": ["<img/>"]', 1))
PY
expect vector-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/vectors" "$runner" "$exceptions" "$trees" "$extra"

# The list is missing everything, has a stale entry, an entry for nothing, a digest that does not match.
: > "$scratch/empty"
expect no-exceptions "UNEXPLAINED FAILURES" python3 "$suite_py" "$suite" "$runner" "$scratch/empty" "$trees" "$extra"
{ cat "$exceptions"; printf 'core.test#1\tstandard\tplanted: this vector passes\t0000000000\n'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/stale" "$trees" "$extra"
{ cat "$exceptions"; printf 'nothing.test#1\tstandard\tplanted: no such vector\t0000000000\n'; } > "$scratch/gone"
expect exception-for-nothing "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/gone" "$trees" "$extra"
awk -F'\t' 'BEGIN{OFS="\t"} /^[a-z]+\.test#/ && !done {$4="0000000000"; done=1} {print}' "$exceptions" > "$scratch/wrongdigest"
expect exception-with-another-failure "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/wrongdigest" "$trees" "$extra"

# The floors.
expect floor-vectors "below the floor" env HTML_SERIALIZER_MIN_VECTORS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$trees" "$extra"
expect floor-passing "below the floor" env HTML_SERIALIZER_MIN_PASS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$trees" "$extra"
expect floor-stable "below the floor" env HTML_SERIALIZER_MIN_STABLE=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions" "$trees" "$extra"

# The runner refuses a request it does not understand, loudly, and runs nothing for it.
if [ -z "$only" ] || [ "$only" = bad-requests ]; then
	n_bad=0
	for bad in "bogus" "tok" "tok quote_attr_values=always" "tok bogus=1 --" "tok -- Z,x41" "tok -- S,-,x61" "tok -- S,-,xzz,0" "tok -- E,-" \
			"tok -- C,y" "tok -- C,x4" "tok encoding=xzz --" "rt" "rt 2 x61" "rt 0" "rt 0 x6" "rt 0 x61 ctx:" "rt 0 x61 form" "rt 0 x61 ctx:x6469 junk"; do
		n_bad=$((n_bad + 1))
		printf '%s\n' "$bad" "tok -- C,x61" | "$runner" > "$log"
		if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
			echo "control bad-request: '$bad' was not answered E_REQUEST (and the next request DONE)" >&2
			cat "$log" >&2
			exit 1
		fi
	done
	echo "control bad-requests: caught ($n_bad)"
	ran=$((ran + 1))
fi

# The serializer with one rule undone (tools/conformance/HTML_SERIALIZER_PLANTS). Each is a defect the vectors, or the
# round trip over the tree vectors, must catch. The plant copies src/html/html_serialize.c, edits it, and builds the
# runner against it in front of the archive.
plant() {
	name=$1 heading=$2 old=$3 new=$4
	if [ -n "$only" ] && [ "$only" != "$name" ]; then
		return 0
	fi
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant"
	python3 - "$root/src/html/html_serialize.c" "$scratch/plant/html_serialize.c" "$old" "$new" <<'PY'
import sys
src, dst, old, new = sys.argv[1:5]
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(src).read()
if text.count(old) != 1:
    raise SystemExit("the plant's text is in %s %d times, not once: %r" % (src, text.count(old), old))
open(dst, "w").write(text.replace(old, new))
PY
	cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/html" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/plant/html_serialize.c" -o "$scratch/plant/html_serialize.o"
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/html_serializer_suite.c" \
		"$scratch/plant/html_serialize.o" -I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" python3 "$suite_py" "$suite" "$scratch/runner-$name" "$exceptions" "$trees" "$extra"
}
while IFS="$(printf '\t')" read -r name kind old new; do
	case $name in ''|'#'*) continue ;; esac
	case $kind in
		F) heading="UNEXPLAINED FAILURES" ;;
		T) heading="UNEXPLAINED ROUND TRIPS" ;;
		S) heading="STALE EXCEPTIONS" ;;
		*) echo "$name: unknown kind '$kind' in HTML_SERIALIZER_PLANTS" >&2; exit 2 ;;
	esac
	plant "$name" "$heading" "$old" "$new"
done < "$root/tools/conformance/HTML_SERIALIZER_PLANTS"

if [ -n "$only" ] && [ "$ran" = 0 ]; then
	echo "control $only: no such control" >&2
	exit 2
fi
echo "html serializer gate: $ran controls, all caught"
