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
# Score the HTML parser's choice of encoding against html5lib-tests' encoding vectors.
#
#   tools/conformance/run-html-encoding.sh [--controls | --control NAME]
#
# The vectors are third_party/html/html5lib-tests/encoding (tests1.dat, tests2.dat, test-yahoo-jp.dat; the scripted/ and chardet/
# directories are out of scope, and the scorer says why and checks that they are as listed), put there by tools/html/fetch.sh. They
# are not fetched here: if the data is absent this fails and says how to get it; it never skips.
#
# --controls makes the gate fail on purpose and requires that it does, each time for the reason it names: a vector that lies,
# an exceptions file that is empty, stale, or for nothing, the floors, a directory the scorer was not told about, requests the
# runner must refuse, and the library's sniffing and decoding with one rule undone, each of which a vector must see.
# --control NAME runs one.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${HTML_ENCODING_SUITE:-$root/third_party/html/html5lib-tests/encoding}

if [ ! -f "$suite/tests1.dat" ]; then
	echo "$suite/tests1.dat is not here." >&2
	echo "Fetch the pinned vectors:  tools/html/fetch.sh" >&2
	exit 1
fi

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/html-encoding-runner
cc -O1 -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/html_encoding_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

suite_py=$root/tools/conformance/html_encoding_suite.py
exceptions=$root/tools/conformance/HTML_ENCODING_EXCEPTIONS
export HTML_ENCODING_MIN_VECTORS=${HTML_ENCODING_MIN_VECTORS:-82}
export HTML_ENCODING_MIN_PASS=${HTML_ENCODING_MIN_PASS:-82}

only=
if [ "$1" = "--control" ]; then
	only=$2
	[ -n "$only" ] || { echo "usage: run-html-encoding.sh --control NAME" >&2; exit 2; }
elif [ "$1" != "--controls" ]; then
	exec python3 "$suite_py" "$suite" "$runner" "$exceptions"
fi

scratch=$(mktemp -d)
ran=0
log=$(mktemp)
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

# A vector lies: an expected encoding is changed.
mkdir -p "$scratch/vectors"
cp -r "$suite"/* "$scratch/vectors/"
python3 - "$scratch/vectors/tests1.dat" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="latin-1").read()
at = text.index("#encoding\n") + len("#encoding\n")
text = text[:at] + "koi8-r\n" + text[at:].split("\n", 1)[1]
open(path, "w", encoding="latin-1").write(text)
PY
expect vector-expectation "UNEXPLAINED FAILURES" python3 "$suite_py" "$scratch/vectors" "$runner" "$exceptions"

# The list is missing an entry, has a stale one, one for nothing, one that locks another answer.
: > "$scratch/none"
{ echo '# HTML encoding exceptions (planted)'; printf 'tests1.dat#1\tparse\twindows-1252\tkoi8-r\tplanted: the control test passes\n'; echo '# end of exceptions'; } > "$scratch/stale"
expect stale-exception "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/stale"
{ echo '# HTML encoding exceptions (planted)'; printf 'nothing.dat#1\tparse\twindows-1252\tkoi8-r\tplanted: no such vector\n'; echo '# end of exceptions'; } > "$scratch/gone"
expect exception-for-nothing "STALE EXCEPTIONS" python3 "$suite_py" "$suite" "$runner" "$scratch/gone"
# The file itself: empty, or cut short, is not "no exceptions".
expect empty-exceptions-file "EXCEPTIONS FILE" python3 "$suite_py" "$suite" "$runner" "$scratch/none"
head -n 3 "$exceptions" > "$scratch/cut"
expect truncated-exceptions-file "EXCEPTIONS FILE" python3 "$suite_py" "$suite" "$runner" "$scratch/cut"
# A vector that fails with an entry that locks the wrong answer: the vector is made to fail by the plant below, in a copy.
# (done with the library plants.)

# The floors.
expect floor-passing "below the floor" env HTML_ENCODING_MIN_PASS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions"
expect floor-loaded "below the floor" env HTML_ENCODING_MIN_VECTORS=99999 python3 "$suite_py" "$suite" "$runner" "$exceptions"

# The out-of-scope directories are as listed, or the gate fails: a new directory, a missing one, another file in one.
mkdir -p "$scratch/dir1" && cp -r "$suite"/* "$scratch/dir1/" && mkdir "$scratch/dir1/unlisted" 
expect unlisted-directory "OUT OF SCOPE, BUT NOT AS LISTED" python3 "$suite_py" "$scratch/dir1" "$runner" "$exceptions"
mkdir -p "$scratch/dir2" && cp -r "$suite"/* "$scratch/dir2/" && rm -r "$scratch/dir2/chardet"
expect missing-directory "OUT OF SCOPE, BUT NOT AS LISTED" python3 "$suite_py" "$scratch/dir2" "$runner" "$exceptions"
mkdir -p "$scratch/dir3" && cp -r "$suite"/* "$scratch/dir3/" && echo x > "$scratch/dir3/scripted/another.dat"
expect extra-file-in-a-directory "OUT OF SCOPE, BUT NOT AS LISTED" python3 "$suite_py" "$scratch/dir3" "$runner" "$exceptions"

# The runner refuses a request it does not understand, loudly, and runs nothing for it.
if [ -z "$only" ] || [ "$only" = bad-requests ]; then
	good="parse x3c703e"
	n_bad=0
	for bad in "bogus x3c703e" "parse 3c703e" "parse x3c7" "parse xzz" "parse" "" "sniff" "parse x3c703e extra" "parser-split:x x3c703e" \
			"parser-split: x3c703e" "parser-split:99 x3c703e" "parser-bytes" "sniff x3c703" "split:1 x3c703e"; do
		n_bad=$((n_bad + 1))
		printf '%s\n' "$bad" "$good" | "$runner" > "$log"
		if [ "$(grep -c '^END E_REQUEST' "$log")" != 1 ] || [ "$(grep -c '^END DONE' "$log")" != 1 ]; then
			echo "control bad-request: '$bad' was not answered E_REQUEST (and the next request DONE)" >&2
			cat "$log" >&2
			exit 1
		fi
	done
	echo "control bad-requests: caught ($n_bad)"
	ran=$((ran + 1))
fi

# The library with one rule undone. The plant copies src/ (the files read each other's headers), edits one file, and builds the
# runner with the edited objects in front of the archive. These are the defects the vectors see; the sniffing differential
# (tools/oracle/run-html-sniff-oracle.sh) plants those the vectors do not (a comment's end, a duplicate attribute, the position at
# the end of the window, a `meta` in the body restarting the parse).
plant_with() {
	how=$1
	shift
	name=$1 heading=$2 file=$3 pattern=$4 replacement=$5
	if [ -n "$only" ] && [ "$only" != "$name" ]; then
		return 0
	fi
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant/src"
	cp -r "$root/src/html" "$root/src/markup" "$root/src/encoding" "$scratch/plant/src/"
	if [ "$how" = lit ]; then
		python3 - "$scratch/plant/src/$file" "$pattern" "$replacement" <<'PY'
import sys
path, old, new = sys.argv[1:4]
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(path).read()
if old not in text:
    raise SystemExit("the plant's text is not in %s: %r" % (path, old))
open(path, "w").write(text.replace(old, new))
PY
	else
		sed -i "s#$pattern#$replacement#g" "$scratch/plant/src/$file"
	fi
	if cmp -s "$root/src/$file" "$scratch/plant/src/$file"; then
		echo "control $name: the plant did not change $file ('$pattern' is not there)" >&2
		exit 1
	fi
	objs=
	for f in html/html_sniff.c html/html_api.c html/html_tokenizer.c html/html_tree.c html/html_tree_modes.c encoding/encoding_decode.c; do
		o="$scratch/plant/$(echo $f | tr / _).o"
		cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$scratch/plant/src/html" -I"$scratch/plant/src/encoding" -I"$root/include" -I"$generated" $cflags \
			-c "$scratch/plant/src/$f" -o "$o"
		objs="$objs $o"
	done
	cc -O1 -D_GNU_SOURCE -o "$scratch/runner-$name" "$root/tools/conformance/html_encoding_suite.c" $objs \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "$heading" python3 "$suite_py" "$suite" "$scratch/runner-$name" "$exceptions"
}
plant_lit() {
	plant_with lit "$@"
}
fails="UNEXPLAINED FAILURES"
plant_lit prescan-meta-ignores-http-equiv "$fails" html/html_sniff.c 'if (a.value_len == 12 && memcmp(a.value, "content-type", 12) == 0) {' 'if (0) {'
plant_lit prescan-content-needs-no-pragma "$fails" html/html_sniff.c 'if (need_pragma >= 0 && !(need_pragma == 1 && !got_pragma) && charset_state == CS_ENCODING) {' 'if (need_pragma >= 0 && charset_state == CS_ENCODING) {'
plant_lit prescan-reads-no-content "$fails" html/html_sniff.c 'else if (a.name_len == 7 && memcmp(a.name, "content", 7) == 0) {' 'else if (0) {'
plant_lit prescan-reads-no-charset-attribute "$fails" html/html_sniff.c 'else if (a.name_len == 7 && memcmp(a.name, "charset", 7) == 0) {' 'else if (0) {'
plant_lit prescan-utf-16-is-kept "$fails" html/html_sniff.c 'if (charset == GTEXT_ENCODING_UTF_16BE || charset == GTEXT_ENCODING_UTF_16LE) {\n          charset = GTEXT_ENCODING_UTF_8;\n        }\n        else if' 'if (0) {\n          charset = GTEXT_ENCODING_UTF_8;\n        }\n        else if'
plant_lit prescan-takes-the-charset-without-its-quote "$fails" html/html_sniff.c 'const char * close = memchr(s + position + 1, c, n - position - 1);\n      if (close) {' 'const char * close = memchr(s + position + 1, c, n - position - 1);\n      if (0) {'
plant_lit prescan-uppercase-names-not-folded "$fails" html/html_sniff.c 'return (b >= '"'"'A'"'"' && b <= '"'"'Z'"'"') ? (unsigned char)(b + 0x20) : b;' 'return b;'
plant_lit prescan-unquoted-value-runs-on "$fails" html/html_sniff.c 'if (b == 0x09 || b == 0x0A || b == 0x0C || b == 0x0D || b == 0x20 || b == 0x3E) {\n      return true;\n    }\n    a->value' 'if (b == 0x3E) {\n      return true;\n    }\n    a->value'
plant_lit the-label-is-not-trimmed "$fails" html/html_sniff.c 'return gtext_encoding_for_label(s + position + 1, (size_t)(close - (s + position + 1)));' 'return gtext_encoding_for_label(s + position + 1, (size_t)(close - (s + position + 1)) + 1);'
plant_lit the-default-is-utf-8 "$fails" html/html_api.c 'else if (fallback != GTEXT_ENCODING_NONE) {' 'else if (0) {'
plant_lit the-window-is-20-bytes "$fails" html/html_api.c 'const GTEXT_Encoding declared = html_prescan(d, n < GTEXT_HTML_PRESCAN_BYTES ? n : GTEXT_HTML_PRESCAN_BYTES);' 'const GTEXT_Encoding declared = html_prescan(d, n < 20 ? n : 20);'
plant_lit a-late-meta-never-restarts "$fails" html/html_tree.c 'tb->enc_declared = (int)found;\n  return ht_tree_fail(tb, GTEXT_HTML_TREE_E_ENCODING);' 'tb->enc_tentative = false;\n  return true;'
plant_lit utf-16-in-a-meta-is-believed "$fails" html/html_tree.c 'if (found == GTEXT_ENCODING_UTF_16BE || found == GTEXT_ENCODING_UTF_16LE) {\n    found = GTEXT_ENCODING_UTF_8;\n  }' 'if (0) {\n    found = GTEXT_ENCODING_UTF_8;\n  }'
echo "html encoding gate: $ran controls, each failed as it should."
