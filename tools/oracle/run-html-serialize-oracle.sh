#!/bin/sh
# Check the HTML serializer against html5lib and parse5.
#
#   tools/oracle/run-html-serialize-oracle.sh [--diff | --controls | --control NAME] [-- extra args for html_serialize_diff.py]
#
# Two references in one pinned container (containers/html), the same image the other HTML gates use, and one gate:
#
#   tools/oracle/html_serialize_diff.py   the same tree put to html5lib 1.1's serializer, parse5 8.0.1's and
#                                         src/html/html_serialize.c, at the standard's settings, over the trees of the
#                                         tree-construction vectors and of generated and mutated documents (parsed by this
#                                         library), and over trees no parser makes. Every divergence is explained by
#                                         features of the tree (verified by taking them out) and is a line of
#                                         tools/oracle/html_serialize_divergences.txt.
#
# The drivers that answer it (containers/html/serialize.py and serialize.mjs) are read from the tree, on the bind mount that
# makes the tree visible to the container at the same path; the image's own drivers, whose hash the pin checks, are the
# parse requests'.
#
# HTML_SERIALIZE_ORACLE_COUNT and HTML_SERIALIZE_ORACLE_SEED size and seed the generated part (a hunt: the list describes the
# default, 3,000 and seed 1; a chunk of 20,000 takes a few minutes).
#
# --controls makes the gate fail on purpose and requires that it does, each for the right reason: its answer corrupted,
# the list corrupted, the witnesses, and this library's serializer compiled with one rule undone
# (tools/oracle/HTML_SERIALIZE_ORACLE_PLANTS). --control NAME runs one control and no other.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/html-serialize-ours
cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$runner" "$root/tools/oracle/html_serialize_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

mode=${1:-all}
[ $# -gt 0 ] && shift
if [ "$1" = "--" ]; then
	shift
fi

run_diff() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_serialize_diff.py" "$runner" "$@"
}

log=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$scratch"' EXIT
only=

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

# This library's serializer with one rule undone, compiled into a runner of its own (its object is linked ahead of the
# archive, so it supplies every symbol of html_serialize.c and the archive's copy is never pulled in).
plant() {
	name=$1 old=$2 new=$3
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
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/runner-$name" "$root/tools/oracle/html_serialize_ours.c" \
		"$scratch/plant/html_serialize.o" -I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	expect "$name" "Divergences with no entry\|Listed divergences that are gone\|Listed divergences whose count has changed\|The serializer failed\|rebuilt from its ops\|Divergences that no feature\|A witness of a reference deviation does not hold" \
		python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_serialize_diff.py" "$scratch/runner-$name"
}

check_controls() {
	# The controls are about the default population, which the list describes, whatever size a hunt asked for.
	unset HTML_SERIALIZE_ORACLE_COUNT HTML_SERIALIZE_ORACLE_SEED
	ran=0
	# The differential, against corruptions of its own answers and of the list.
	for p in ours-extra-char ours-lost-char; do
		expect "diff-$p" "Divergences that no feature of the tree explains" run_diff --plant "$p"
	done
	expect diff-stale-entry "Listed divergences that are gone" run_diff --plant stale-entry
	expect diff-change-count "Listed divergences whose count has changed" run_diff --plant change-count
	expect diff-no-list "Divergences with no entry" run_diff --plant no-list
	expect diff-witness "A witness of a reference deviation does not hold" run_diff --plant witness
	# ... and against this library's serializer with a rule undone.
	while IFS="$(printf '\t')" read -r name old new; do
		case $name in ''|'#'*) continue ;; esac
		plant "$name" "$old" "$new"
	done < "$root/tools/oracle/HTML_SERIALIZE_ORACLE_PLANTS"
	if [ -n "$only" ] && [ "$ran" = 0 ]; then
		echo "control $only: no such control" >&2
		exit 2
	fi
	echo "html serializer differential: $ran controls, all caught"
}

case "$mode" in
	--control) only=$1; check_controls ;;
	--diff|all) run_diff "$@" ;;
	--controls) check_controls ;;
	*) echo "usage: run-html-serialize-oracle.sh [--diff | --controls | --control NAME]" >&2; exit 2 ;;
esac
