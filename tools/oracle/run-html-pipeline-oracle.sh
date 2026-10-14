#!/bin/sh
# Check the whole HTML pipeline (bytes in, tree and serialization out) against html5lib and parse5.
#
#   tools/oracle/run-html-pipeline-oracle.sh [--diff | --controls | --control NAME] [-- extra args for html_pipeline_diff.py]
#
# One gate, tools/oracle/html_pipeline_diff.py (its docstring says what it asks), over the pinned image of containers/html (html5lib 1.1 and
# parse5 8.0.1): the public API (tools/oracle/html_pipeline_ours.c, one runner) is given bytes in the encodings of the Encoding Standard with
# and without a byte order mark, `<meta>`, transport label or default, and fragments in random contexts; the encoding it decides, the text it
# reads, the tree it makes, the tree it makes of that text, what it serializes and what it makes of the serialization are held to the
# references, with every divergence reduced to a minimal reproducer and listed in html_pipeline_divergences.txt. The two drivers
# (containers/html/pipeline.py and pipeline.mjs) are read from the tree on the bind mount that makes it visible in the container.
#
# HTML_PIPELINE_ORACLE_COUNT and HTML_PIPELINE_ORACLE_SEED size and seed the generated part (a hunt: the list describes the default; the
# population is about 3.3 times the count, plus the vectors). HTML_PIPELINE_ORACLE_CHUNK bounds the memory of a hunt (items per round).
#
# --controls makes the gate fail on purpose and requires that it does: its answers and its list corrupted (nine ways), and this library
# compiled with one rule undone in each layer (tools/oracle/HTML_PIPELINE_ORACLE_PLANTS: sniffing, the Encoding module, the tokenizer, the
# tree builder, the serializer). Controls run a population of 300 (HTML_PIPELINE_CONTROL_COUNT), held to a summary of the unplanted library
# on the same population, which the first step of --controls makes and requires to pass (the control of the controls).
# --control NAME runs one control and no other.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

# HTML_PIPELINE_PREBUILT=<dir> runs on a machine that cannot compile (a job in the pinned image, tools/evo run --image): the runners are
# the ones `--build-plants <dir>` made on one that can (html-pipeline-ours and runner-<plant>, with their shared libraries in <dir>/lib, which the
# caller puts on LD_LIBRARY_PATH).
prebuilt=${HTML_PIPELINE_PREBUILT:-}
if [ -n "$prebuilt" ]; then
	mkdir -p "$root/build"
	runner=$prebuilt/html-pipeline-ours
	[ -x "$runner" ] || { echo "$runner is not there" >&2; exit 1; }
else
	[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
	[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
	cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
	libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
	conformance_library
	mkdir -p "$root/build"
	runner=$root/build/html-pipeline-ours
	cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$runner" "$root/tools/oracle/html_pipeline_ours.c" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
fi

mode=${1:-all}
[ $# -gt 0 ] && shift
if [ "$1" = "--" ]; then
	shift
fi

run_diff() {
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_pipeline_diff.py" "$runner" "$@"
}

log=$(mktemp)
scratch=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$scratch"' EXIT
only=
from=${HTML_PIPELINE_ORACLE_FROM:-}
ccount=${HTML_PIPELINE_CONTROL_COUNT:-300}

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

# control_run <runner> [args]: the differential on the control population, held to the summary of the unplanted library.
control_run() {
	r=$1
	shift
	python3 "$root/tools/oracle/oracle_run.py" html -- \
		python3 "$root/tools/oracle/html_pipeline_diff.py" "$r" --count "$ccount" --against "$scratch/baseline.json" "$@"
}

# expect <what> <command...>: the command must exit 3.
expect() {
	what=$1
	shift
	if skipped "$what"; then
		return 0
	fi
	ran=$((ran + 1))
	status=0
	"$@" </dev/null >"$scratch/out" 2>"$log" || status=$?
	if [ "$status" != 3 ]; then
		echo "control $what: the gate exited $status, not 3 (it did not notice)" >&2
		tail -n 5 "$scratch/out" >&2
		cat "$log" >&2
		exit 1
	fi
	echo "control $what: caught ($(grep -c '###' "$log") headings)"
}

# This library with one rule undone, compiled into a runner of its own (its object is linked ahead of the archive).
build_plant() {
	name=$1 file=$2 old=$3 new=$4
	rm -rf "$scratch/plant"
	mkdir -p "$scratch/plant"
	python3 - "$root/src/$file" "$scratch/plant/plant.c" "$old" "$new" <<'PY'
import sys
src, dst, old, new = sys.argv[1:5]
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(src).read()
if text.count(old) != 1:
    raise SystemExit("the plant's text is in %s %d times, not once: %r" % (src, text.count(old), old))
open(dst, "w").write(text.replace(old, new))
PY
	dir=$(dirname "$root/src/$file")
	cc -std=gnu17 -O1 -fPIC -DGTEXT_BUILD -I"$dir" -I"$root/src/html" -I"$root/src/encoding" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/plant/plant.c" -o "$scratch/plant/plant.o"
	cc -std=gnu17 -D_GNU_SOURCE -O1 -o "$scratch/runner-$name" "$root/tools/oracle/html_pipeline_ours.c" \
		"$scratch/plant/plant.o" -I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
}

plant() {
	name=$1 file=$2 old=$3 new=$4
	if skipped "$name"; then
		return 0
	fi
	if [ -n "$prebuilt" ]; then
		[ -x "$prebuilt/runner-$name" ] || { echo "$prebuilt/runner-$name is not there" >&2; exit 1; }
		cp "$prebuilt/runner-$name" "$scratch/runner-$name"
	else
		build_plant "$name" "$file" "$old" "$new"
	fi
	expect "$name" control_run "$scratch/runner-$name"
}

# --build-plants <dir>: the main runner, every plant's runner and the shared libraries they need, for a machine that cannot compile.
build_all() {
	out=$1
	mkdir -p "$out/lib"
	cp "$runner" "$out/html-pipeline-ours"
	while IFS="$(printf '\t')" read -r name file old new; do
		case $name in ''|'#'*) continue ;; esac
		build_plant "$name" "$file" "$old" "$new"
		cp "$scratch/runner-$name" "$out/runner-$name"
	done < "$root/tools/oracle/HTML_PIPELINE_ORACLE_PLANTS"
	for lib in $(ldd "$runner" | awk '/ghoti.io/ {print $3}'); do cp -L "$lib" "$out/lib/"; done
	echo "built $(ls "$out" | grep -c runner-) plant runners and the main runner in $out"
}

check_controls() {
	unset HTML_PIPELINE_ORACLE_COUNT HTML_PIPELINE_ORACLE_SEED
	ran=0
	# The control of the controls: the unplanted library on the control population makes the summary, and passes it.
	if [ -z "$only" ] || [ "$only" = baseline ]; then
		python3 "$root/tools/oracle/oracle_run.py" html -- \
			python3 "$root/tools/oracle/html_pipeline_diff.py" "$runner" --count "$ccount" --save-classes "$scratch/baseline.json" >"$scratch/out" 2>"$log" \
			|| { echo "control baseline: the unplanted library fails the control population" >&2; cat "$log" >&2; tail -n 20 "$scratch/out" >&2; exit 1; }
		cp "$scratch/baseline.json" "$root/build/html-pipeline-baseline.json"
		echo "control baseline: the unplanted library passes ($(grep -c . "$scratch/baseline.json") lines of summary)"
		ran=$((ran + 1))
		if [ "$only" = baseline ]; then
			echo "html pipeline differential: 1 control"
			return 0
		fi
	else
		[ -f "$root/build/html-pipeline-baseline.json" ] || { echo "run --control baseline first" >&2; exit 2; }
		cp "$root/build/html-pipeline-baseline.json" "$scratch/baseline.json"
	fi
	# Corruptions of its own answers and of the list.
	for p in ours-extra-line ours-wrong-encoding ours-bytes-differ ours-unstable theirs-wrong driver-wrong stale-entry change-count no-list; do
		expect "diff-$p" control_run "$runner" --plant "$p"
	done
	# This library with a rule undone.
	while IFS="$(printf '\t')" read -r name file old new; do
		case $name in ''|'#'*) continue ;; esac
		plant "$name" "$file" "$old" "$new"
	done < "$root/tools/oracle/HTML_PIPELINE_ORACLE_PLANTS"
	if [ -n "$only" ] && [ "$ran" = 0 ]; then
		echo "control $only: no such control" >&2
		exit 2
	fi
	echo "html pipeline differential: $ran controls, all caught"
}

case "$mode" in
	--build-plants) build_all "$1" ;;
	--control) only=$1; check_controls ;;
	--diff|all) run_diff "$@" ;;
	--controls) check_controls ;;
	*) echo "usage: run-html-pipeline-oracle.sh [--diff | --controls | --control NAME]" >&2; exit 2 ;;
esac
