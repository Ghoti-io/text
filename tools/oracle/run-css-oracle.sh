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
# Check this library's CSS Syntax against tinycss2, cssparser and csstree.
#
#   tools/oracle/run-css-oracle.sh [--controls] [-- extra arguments for css_diff.py]
#
# One pinned image (containers/css) holds the three references; tools/oracle/css_diff.py reads a population
# (css_gen.py: the css-parsing-tests and WPT inputs, soup, nested stylesheets, mutations, bytes) with this
# library and with each reference that has an entry point for the question, and every divergence is of a class
# of tools/oracle/css_divergences.txt (an experiment: css_classes.py) or fails the run. CSS_ORACLE_COUNT and
# CSS_ORACLE_SEED size and seed the generated part (the list describes the default 20000 and seed 1; any other
# is a hunt, held to the classes and not to the counts). Needs the built-here image: make oracle-images.
#
# --controls makes the gate fail on purpose, each way it should, and requires that it does: what this library
# says corrupted in six ways, the list tampered with three ways, and the library with one rule undone in each of
# its decoder, stream, tokenizer, parser, serializer and number conversion (one plant at a time, in a copy of
# the file, never in the tree). A gate whose assertion no longer reaches its subject prints the same clean line
# as one that passes.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

# CSS_ORACLE_PREPARE=1 only compiles what the gate runs (the runner and a runner for each plant) into build/css-oracle/ and
# stops; CSS_ORACLE_PREBUILT=1 then runs the gate on those without a compiler, on a machine whose container engine is not
# the one the library is built in (the build host: the library is built in an image, and the references run beside it).
mkdir -p "$root/build/css-oracle"
runner=$root/build/css-oracle/ours
if [ "$CSS_ORACLE_PREBUILT" != 1 ]; then
	[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
	[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
	cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
	libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
	conformance_library
	cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_syntax_suite.c" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	python3 "$root/tools/css/extract-wpt.py" "$root/third_party/css/wpt" "$root/build/css-syntax-wpt-vectors.json" >/dev/null
fi

count=${CSS_ORACLE_COUNT:-20000}
seed=${CSS_ORACLE_SEED:-1}

gate() {
	python3 "$root/tools/oracle/oracle_run.py" css -- \
		python3 "$root/tools/oracle/css_diff.py" "$runner" --count "$count" --seed "$seed" "$@"
}

if [ "$1" = "--" ]; then
	shift
fi
if [ "$1" != "--controls" ] && [ "$CSS_ORACLE_PREPARE" != 1 ]; then
	exec python3 "$root/tools/oracle/oracle_run.py" css -- \
		python3 "$root/tools/oracle/css_diff.py" "$runner" --count "$count" --seed "$seed" "$@"
fi
[ "$1" = "--controls" ] && shift

log=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$scratch"' EXIT

# expect <what> <detail> <command...>: the command must fail, and say the heading and the detail.
expect() {
	what=$1 detail=$2
	shift 2
	[ "$CSS_ORACLE_PREPARE" = 1 ] && return 0
	# CSS_ORACLE_ONLY=<egrep> runs the controls whose names match (the build host splits the run over two jobs).
	if [ -n "$CSS_ORACLE_ONLY" ] && ! echo "$what" | grep -Eq "$CSS_ORACLE_ONLY"; then
		return 0
	fi
	status=0
	"$@" </dev/null >/dev/null 2>"$log" || status=$?
	if [ "$status" = 0 ] || ! grep -q "the CSS differential failed" "$log" || ! grep -q "$detail" "$log"; then
		echo "control $what: the gate exited $status and did not say 'the CSS differential failed' with '$detail'" >&2
		cat "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

# The unedited gate passes: the controls are measured against it.
[ "$CSS_ORACLE_PREPARE" = 1 ] || gate >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; tail -40 "$log" >&2; exit 1; }
[ "$CSS_ORACLE_PREPARE" = 1 ] || echo "unedited gate: $(tail -1 "$log")"

# What this library says, corrupted (a smaller population: the corruption is everywhere).
small="--count 2500 --seed 7"
runner_diff() {
	python3 "$root/tools/oracle/oracle_run.py" css -- python3 "$root/tools/oracle/css_diff.py" "$runner" $small "$@"
}
expect ours-drops-a-token "no known class explains" runner_diff --plant ours-drops-a-token
expect ours-number "no known class explains" runner_diff --plant ours-number
expect ours-rule-extent "no known class explains" runner_diff --plant ours-rule-extent
expect ours-encoding "no known class explains" runner_diff --plant ours-encoding
expect ours-forgets-important "no known class explains" runner_diff --plant ours-forgets-important

# The list tampered with: a class nobody falls in, a count changed, no list.
expect stale-entry "stale entry" gate --plant stale-entry
expect change-count "the list says" gate --plant change-count
expect no-list "is not in css_divergences.txt" gate --plant no-list

# The library with one rule undone, in a copy of the file: its object is linked ahead of the archive, so it supplies the
# symbols of that file and the archive's copy is never pulled in.
plant() {
	name=$1 detail=$2 file=$3 pattern=$4 replacement=$5
	base=$(basename "$file" .c)
	if [ "$CSS_ORACLE_PREBUILT" != 1 ]; then
		cp "$root/src/css/$file" "$scratch/$file"
		sed -i "s#$pattern#$replacement#g" "$scratch/$file"
		if cmp -s "$root/src/css/$file" "$scratch/$file"; then
			echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
			exit 1
		fi
		cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -w -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
			-c "$scratch/$file" -o "$scratch/$base.o"
		cc -O1 -D_GNU_SOURCE -o "$root/build/css-oracle/runner-$name" "$root/tools/conformance/css_syntax_suite.c" "$scratch/$base.o" \
			-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	fi
	expect "$name" "$detail" python3 "$root/tools/oracle/oracle_run.py" css -- \
		python3 "$root/tools/oracle/css_diff.py" "$root/build/css-oracle/runner-$name" $small --baseline "$runner"
}
# (A planted defect is caught either way: the divergences it makes fall in no class, or fall in classes whose lines this
# population's list does not have - the gate fails in both, and the second is as much a catch as the first.)
unexplained="no known class explains\|is not in css_divergences.txt"
plant number-exponent "$unexplained" css_tokenizer.c "if (c == 'e' || c == 'E') {" 'if (0) {'
plant escape-seven-digits "$unexplained" css_tokenizer.c 'for (int i = 0; i < 5 \&\& is_hex' 'for (int i = 0; i < 6 \&\& is_hex'
plant cdc-swallowed "$unexplained" css_tokenizer.c "else if (a == '-' \&\& b == '>') {" 'else if (0) {'
plant number-as-float "$unexplained" css_core.c '  \*out = v;' '  *out = (double)(float)v;'
plant important-case "$unexplained" css_parser.c '\&\& css_ascii_ieq(\&imp->value, "important")) {' '\&\& imp->value.length == 9 \&\& memcmp(imp->value.data, "important", 9) == 0) {'
plant custom-property-rule "$unexplained" css_parser.c 'if (looks_like_custom_property(p, base)) {' 'if (0) {'
plant remnants-skipped "$unexplained" css_parser.c 'remnants(p, s, false);' '(void)0;'
plant bom-over-label "$unexplained" css_decode.c 'if (length >= 3 \&\& b\[0\] == 0xEF \&\& b\[1\] == 0xBB \&\& b\[2\] == 0xBF) {' 'if (0) {'
plant charset-utf16 "$unexplained" css_decode.c 'if (e == GTEXT_ENCODING_UTF_16BE || e == GTEXT_ENCODING_UTF_16LE) {' 'if (0) {'
plant protocol-label "$unexplained" css_decode.c 'if (options \&\& options->protocol_label) {' 'if (0) {'
plant stream-encoding "stream" css_stream.c 'info.encoding.encoding = s->dec.encoding;' 'info.encoding.encoding = GTEXT_ENCODING_UTF_8;'
plant no-comment-between-tokens "round-trip" css_serialize.c 'puts_(w, "/\*\*/");' '(void)0;'
plant identifier-digit "round-trip" css_serialize.c 'else if (mode != ID_UNRESTRICTED \&\& index == 0 \&\& c >= .0. \&\& c <= .9.) {' 'else if (0) {'
