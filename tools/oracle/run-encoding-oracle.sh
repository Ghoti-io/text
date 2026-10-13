#!/bin/sh
# Check the Encoding module against the Encoding Standard's own test suite and against Node's and Python's decoders.
#
#   tools/oracle/run-encoding-oracle.sh [--wpt | --diff | --controls | --control NAME] [-- extra arguments for encoding_diff.py]
#
# Two gates over one pinned image (containers/IMAGES, `encoding`: Node 20.19.2 with ICU 76.1 and Python 3.13.5, the html image):
#
#   tools/oracle/encoding_wpt.py    the web-platform-tests `encoding/` directory (pinned by tools/encoding/WPT_COMMIT) run in Node against this library,
#                                   through a pair of named pipes: the JavaScript tests, and the HTML pages of the legacy multi-byte vectors with the little
#                                   of a browser they use emulated (its header says what; the pages that need a browser are listed with the reason). The
#                                   count of each file is in tools/oracle/encoding_wpt_expected.txt.
#   tools/oracle/encoding_diff.py   the library against Node's TextDecoder/TextEncoder and CPython's codecs, over the populations its docstring lists (the
#                                   multi-byte ones in tools/oracle/encoding_mb.py, against the standard's handlers written again in encoding_model.py).
#                                   Every difference is explained by the standard's data and is a line of tools/oracle/encoding_divergences.txt.
#
# ENCODING_ORACLE_COUNT, ENCODING_ORACLE_SEED and ENCODING_ORACLE_FATAL_STRIDE size and seed the differential's generated part (a hunt: the list describes
# the default). `-- --only utf --utf-bytes 3 --fatal-stride 1` is the exhaustive fatal comparison, about five minutes a UTF. The multi-byte decoders take about
# fifteen minutes in all (every sequence of three bytes in both modes against Node and each Python codec, and the stratified populations of four bytes and of
# ISO-2022-JP's escape sequences): `--diff --only mb-decode --mb-only shift_jis` runs one encoding, in two minutes or so, and checks the lines of the list for it.
#
# --controls makes the gates fail on purpose and requires that they do, each for the right reason: the runner's bridge corrupted, the counts corrupted, a
# file missing; the differential's answers corrupted, the list corrupted, the standard's data corrupted; and this library's sources compiled with one rule
# undone (one for each line of tools/oracle/ENCODING_ORACLE_PLANTS), each of which the suite or the differential must see. `--control NAME` runs one.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

# The runner is built into a directory of its own, so that several runs (a batch of `--control NAME` at once) do not rewrite each other's.
log=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$scratch"' EXIT
runner=$scratch/encoding-ours
cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$runner" "$root/tools/oracle/encoding_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"
# ... and with it the C++ model of the multi-byte decoders as an enumerator (tools/oracle/encoding_model_enum.cpp), which the differential compares the
# library with on every sequence of every population. It uses the library for its JSON parser and the label lookup, and for nothing it checks.
g++ -std=c++20 -O2 -Wall -Wextra -o "$scratch/encoding-model" "$root/tools/oracle/encoding_model_enum.cpp" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

mode=${1:-all}
[ $# -gt 0 ] && shift
if [ "$1" = "--" ]; then
	shift
fi

run_wpt() {
	runner_path=$1
	shift
	python3 "$root/tools/oracle/oracle_run.py" encoding -- python3 "$root/tools/oracle/encoding_wpt.py" "$runner_path" "$@"
}

run_diff() {
	runner_path=$1
	shift
	python3 "$root/tools/oracle/oracle_run.py" encoding -- python3 "$root/tools/oracle/encoding_diff.py" "$runner_path" "$@"
}

only=

HEADINGS="Failed tests of the standard's suite\|Listed files of the suite that are gone, or that run\|Counts of the suite's tests that have changed\|Test files that ran no tests\|Test files that are not in the suite\|Files of the suite that are neither run nor listed\|The library disagrees with the standard's data\|Divergences with no entry\|Listed divergences that are gone\|Listed divergences whose count has changed"

# expect <what> <heading> <command...>: the command must exit 3 and say the heading.
expect() {
	what=$1 want=$2
	shift 2
	if [ -n "$only" ] && [ "$only" != "$what" ]; then
		return 0
	fi
	ran=$((ran + 1))
	status=0
	"$@" </dev/null >/dev/null 2>"$log" || status=$?
	if [ "$status" != 3 ] || ! grep -q "$want" "$log"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		cat "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

# This library's sources with one rule undone, compiled into a runner of its own (the object is linked ahead of the archive, so it supplies every symbol of
# its file and the archive's copy is never pulled in). The suite is asked first, because it takes two seconds; what it does not see the differential must.
plant() {
	name=$1 file=$2 old=$3 new=$4 extra=$5
	if [ -n "$only" ] && [ "$only" != "$name" ]; then
		return 0
	fi
	ran=$((ran + 1))
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant"
	cp -r "$root/src/encoding" "$scratch/plant/encoding"
	python3 - "$scratch/plant/encoding/$file" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1:4]
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(path).read()
if text.count(old) != 1:
    raise SystemExit("the plant's text is in %s %d times, not once: %r" % (path, text.count(old), old))
open(path, "w").write(text.replace(old, new))
PY
	cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$scratch/plant/encoding" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/plant/encoding/$file" -o "$scratch/plant/mutant.o"
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/runner-$name" "$root/tools/oracle/encoding_ours.c" "$scratch/plant/mutant.o" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -Wl,-rpath,"$PREFIX/lib/ghoti.io"
	status=0
	run_wpt "$scratch/runner-$name" </dev/null >/dev/null 2>"$log" || status=$?
	if [ "$status" = 3 ] && grep -q "$HEADINGS" "$log"; then
		echo "control $name: caught by the suite"
		return 0
	fi
	# The cheap populations first (the streams and the texts of the multi-byte encodings, twenty seconds), then the rest.
	status=0
	run_diff "$scratch/runner-$name" --only mb-stream --only mb-encode-text </dev/null >/dev/null 2>"$log" || status=$?
	if [ "$status" = 3 ] && grep -q "$HEADINGS" "$log"; then
		echo "control $name: caught by the differential"
		return 0
	fi
	status=0
	# A plant whose defect only longer sequences show names the population in a fifth column of the list, and only that population is run.
	if [ -n "$extra" ]; then
		# shellcheck disable=SC2086
		run_diff "$scratch/runner-$name" $extra </dev/null >/dev/null 2>"$log" || status=$?
	else
		run_diff "$scratch/runner-$name" --only labels --only single-byte --only sb-encode --only streaming --only ignore-bom \
			--only encode-modes --only sniff --only buffer --only utf8-encode --only utf --utf-bytes 2 --count 12000 \
			--only mb-decode --mb-bytes 2 --no-strata --only mb-encode \
			</dev/null >/dev/null 2>"$log" || status=$?
	fi
	if [ "$status" != 3 ] || ! grep -q "$HEADINGS" "$log"; then
		echo "control $name: neither the suite nor the differential saw it (the differential exited $status)" >&2
		cat "$log" >&2
		exit 1
	fi
	echo "control $name: caught by the differential"
}

check_controls() {
	# The controls are about the default population, which the lists describe, whatever size a hunt asked for.
	unset ENCODING_ORACLE_COUNT ENCODING_ORACLE_SEED ENCODING_ORACLE_FATAL_STRIDE
	ran=0
	# The suite's runner, against corruptions of its bridge and its list.
	expect wpt-bridge "Failed tests of the standard's suite" run_wpt "$runner" --plant bridge-drops-a-byte
	expect wpt-count "Counts of the suite's tests that have changed" run_wpt "$runner" --plant expected-count
	expect wpt-no-file "Test files that are not in the suite" run_wpt "$runner" --plant no-such-file
	expect wpt-unlisted "Files of the suite that are neither run nor listed" run_wpt "$runner" --plant unlisted-file
	expect wpt-stale-listing "Listed files of the suite that are gone, or that run" run_wpt "$runner" --plant stale-listing
	expect wpt-stateful-page "Failed tests of the standard's suite" run_wpt "$runner" --plant stateful-expectation
	# The differential, against corruptions of its own answers, of the list and of the standard's data (the exhaustive UTF population is left out of
	# the controls: it is the same code and takes minutes).
	for p in ours-extra-char ours-lost-char; do
		expect "diff-$p" "$HEADINGS" run_diff "$runner" --except utf --except mb-decode --except mb-encode --except mb-stream --except mb-encode-text --plant "$p"
	done
	expect diff-stale-entry "Listed divergences that are gone" run_diff "$runner" --except utf --except mb-decode --except mb-encode --except mb-stream --except mb-encode-text --plant stale-entry
	expect diff-change-count "Listed divergences whose count has changed" run_diff "$runner" --except utf --except mb-decode --except mb-encode --except mb-stream --except mb-encode-text --plant change-count
	expect diff-no-list "Divergences with no entry" run_diff "$runner" --except utf --except mb-decode --except mb-encode --except mb-stream --except mb-encode-text --plant no-list
	expect diff-witness "The library disagrees with the standard's data" run_diff "$runner" --except utf --except mb-decode --except mb-encode --except mb-stream --except mb-encode-text --plant witness
	# The multi-byte populations, each against the same corruptions (one encoding, up to two bytes: the corruptions do not depend on the size).
	for p in ours-extra-char ours-lost-char; do
		expect "mb-diff-$p" "$HEADINGS" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only shift_jis --plant "$p"
	done
	expect mb-diff-stale-entry "Listed divergences that are gone" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only gb18030 --plant stale-mb-entry
	expect mb-diff-change-count "Listed divergences whose count has changed" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only shift_jis --plant change-mb-count
	expect mb-diff-witness "The library disagrees with the standard's data" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only shift_jis --mb-only euc-jp --plant mb-witness
	expect mb-diff-cpp-model-witness "The library disagrees with the standard's data" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only shift_jis --plant cpp-model-witness
	expect mb-diff-stale-witness "The library disagrees with the standard's data" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only euc-kr --plant stale-witness
	expect mb-diff-change-signature "Listed divergences whose count has changed\|Listed divergences that are gone\|Divergences with no entry" run_diff "$runner" --only mb-decode --mb-bytes 2 --no-strata --mb-only shift_jis --plant change-signature
	# ... and against this library's sources with a rule undone.
	while IFS="$(printf '\t')" read -r name file old new extra; do
		case $name in ''|'#'*) continue ;; esac
		plant "$name" "$file" "$old" "$new" "$extra"
	done < "$root/tools/oracle/ENCODING_ORACLE_PLANTS"
	if [ -n "$only" ] && [ "$ran" = 0 ]; then
		echo "control $only: no such control" >&2
		exit 1
	fi
	echo "encoding oracle: $ran controls, all caught"
}

case "$mode" in
	--control) only=$1; check_controls ;;
	--wpt) run_wpt "$runner" "$@" ;;
	--diff) run_diff "$runner" "$@" ;;
	all) run_wpt "$runner" && run_diff "$runner" "$@" ;;
	--controls) check_controls ;;
	*) echo "usage: run-encoding-oracle.sh [--wpt | --diff | --controls | --control NAME]" >&2; exit 2 ;;
esac
