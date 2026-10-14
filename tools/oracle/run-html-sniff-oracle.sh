#!/bin/sh
# Check the HTML encoding sniffing against html5lib's.
#
#   tools/oracle/run-html-sniff-oracle.sh [--diff | --controls | --control NAME] [-- extra args for html_sniff_diff.py]
#
# One reference in the pinned container (containers/html: html5lib 1.1, the sniffer of its input stream with `useChardet` off) and a
# second reading of the standard written out in Python (html_sniff_model.py), and one gate:
#
#   tools/oracle/html_sniff_diff.py   documents of html_sniff_gen.py put to gtext_html_sniff_encoding(), to the whole parser (a restart
#                                     included) and to html5lib, under transport labels that are certain, tentative or absent, a
#                                     caller default or none, the `<meta>` honoured or ignored. With every html5lib departure
#                                     switched off the model must answer as this library does, always; each disagreement with html5lib
#                                     must be reproduced by the smallest set of departures (named in html_sniff_divergences.txt with the
#                                     rule of the standard it departs from, a document that shows it and a control on which the two
#                                     agree); the incremental parser, cut anywhere, must answer as the whole parser.
#
# The driver that answers (containers/html/sniff.py) is read from the tree, on the bind mount that makes the tree visible to the
# container at the same path.
#
# HTML_SNIFF_ORACLE_COUNT and HTML_SNIFF_ORACLE_SEED size and seed the generated part (a hunt: the list describes the default; a chunk
# of 20,000 documents takes a few minutes).
#
# --controls makes the gate fail on purpose and requires that it does, each for the right reason: its answer corrupted, the list
# corrupted, a witness broken, and this library's sniffer compiled with one rule undone (tools/oracle/HTML_SNIFF_ORACLE_PLANTS).
# --control NAME runs one control and no other.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/html-sniff-ours
cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$runner" "$root/tools/oracle/html_sniff_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

mode=${1:-all}
[ $# -gt 0 ] && shift
if [ "$1" = "--" ]; then
	shift
fi

run_diff() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_sniff_diff.py" "$runner" "$@"
}

log=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$scratch"' EXIT
only=
from=${HTML_SNIFF_ORACLE_FROM:-}

# skipped <control>: true when this control is not to be run: another one was asked for (--control NAME), or it comes before the one a
# resumed run starts at (HTML_SNIFF_ORACLE_FROM=NAME, so that the list can be run in pieces when one call may not take as long as all of it).
skipped() {
	if [ -n "$only" ] && [ "$only" != "$1" ]; then
		return 0
	fi
	if [ -n "$from" ]; then
		if [ "$from" = "$1" ]; then
			from=
		else
			return 0
		fi
	fi
	return 1
}

# expect <what> <heading> <command...>: the command must exit 3 and say the heading.
expect() {
	what=$1 want=$2
	shift 2
	if skipped "$what"; then
		return 0
	fi
	ran=$((ran + 1))
	status=0
	"$@" </dev/null >"$scratch/out" 2>"$log" || status=$?
	if [ "$status" != 3 ] || ! grep -q "$want" "$scratch/out"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		cat "$scratch/out" "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}

# This library's sniffer with one rule undone, compiled into a runner of its own (its object is linked ahead of the archive, so it
# supplies every symbol of its file and the archive's copy is never pulled in).
plant() {
	name=$1 file=$2 old=$3 new=$4
	if skipped "$name"; then
		return 0
	fi
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant"
	python3 - "$root/src/html/$file" "$scratch/plant/$file" "$old" "$new" <<'PY'
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
		-c "$scratch/plant/$file" -o "$scratch/plant/plant.o"
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/runner-$name" "$root/tools/oracle/html_sniff_ours.c" \
		"$scratch/plant/plant.o" -I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "The model and this library disagree\|This library.s runner\|Divergences that no departure\|The incremental parser's answer differs\|The restart is not what\|Listed departures\|Departures with no entry\|A witness does not hold\|Label tables differ" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_sniff_diff.py" "$scratch/runner-$name"
}

check_controls() {
	# The controls are about the default population, which the list describes, whatever size a hunt asked for.
	unset HTML_SNIFF_ORACLE_COUNT HTML_SNIFF_ORACLE_SEED
	ran=0
	# The differential, against corruptions of its own answers and of the list.
	expect diff-ours-wrong "The model and this library disagree" run_diff --plant ours-wrong
	expect diff-theirs-wrong "Divergences that no departure" run_diff --plant theirs-wrong
	expect diff-incremental-wrong "The incremental parser's answer differs" run_diff --plant incremental-wrong
	expect diff-label-wrong "Label tables differ" run_diff --plant label-wrong
	expect diff-stale-entry "Listed departures that are gone" run_diff --plant stale-entry
	expect diff-change-count "Listed departures whose count has changed" run_diff --plant change-count
	expect diff-no-list "Departures with no entry" run_diff --plant no-list
	expect diff-witness "A witness does not hold" run_diff --plant witness
	expect diff-control "A witness does not hold" run_diff --plant control
	# ... and against this library's sniffer with a rule undone.
	while IFS="$(printf '\t')" read -r name file old new; do
		case $name in ''|'#'*) continue ;; esac
		plant "$name" "$file" "$old" "$new"
	done < "$root/tools/oracle/HTML_SNIFF_ORACLE_PLANTS"
	if [ -n "$only" ] && [ "$ran" = 0 ]; then
		echo "control $only: no such control" >&2
		exit 2
	fi
	echo "html sniff differential: $ran controls, all caught"
}

case "$mode" in
	--control) only=$1; check_controls ;;
	--diff|all) run_diff "$@" ;;
	--controls) check_controls ;;
	*) echo "usage: run-html-sniff-oracle.sh [--diff | --controls | --control NAME]" >&2; exit 2 ;;
esac
