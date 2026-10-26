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
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
# The CSS property differential: this library against Chromium and Firefox (tools/oracle/css_prop_diff.py says what is compared and
# what a divergence is).
#
#   tools/oracle/run-css-property-oracle.sh [--stage S] [--work DIR] [--controls]
#
# The stages are css_prop_diff.py's: populate and ours need only the library and the pinned vectors; browsers needs the image
# (containers/cssprop, pinned in containers/IMAGES) and a container engine; classify needs the answers of all three. With no --stage
# the four run in turn. The work directory (default build/css-prop-oracle) holds the population and every reader's answers, so a run
# can be split across hosts: the first two stages in the build container, the third where the engine is, the last anywhere.
#
# CSS_PROP_ORACLE_COUNT and CSS_PROP_ORACLE_SEED size and seed the generated part (the list tools/oracle/css_prop_divergences.txt
# describes the default, 3,000 and seed 1; a hunt is a count of 100,000 or more, in chunks: CSS_PROP_ORACLE_CHUNK=K/T takes every T-th
# generated item from K, and the fixed part, which every chunk has). CSS_PROP_ORACLE_MAX_OURS is the most keys of class `ours` the list may hold (default 0: a finished list holds none; the Makefile
# sets the number a list under construction has, and it only goes down).
#
# --controls makes the gate fail on purpose and requires that it does, each for the right reason: an answer of ours changed four ways,
# an engine's answer changed, a control item misclassified, the list tampered with (a key removed, a key that does not occur, a count
# changed, class `ours`, a class that is not one, a blanket reason), a witness that does not hold, and this library's source compiled
# with one rule undone (tools/oracle/CSS_PROPERTY_ORACLE_PLANTS). It needs the browsers' answers of a run of the default population,
# which it makes (or reads, from CSS_PROP_ORACLE_REPLAY) once.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

stage=all
controls=0
work=${CSS_PROP_ORACLE_WORK:-$root/build/css-prop-oracle}
while [ $# -gt 0 ]; do
	case $1 in
	--stage) stage=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--controls) controls=1; shift ;;
	*) echo "run-css-property-oracle.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

mkdir -p "$root/build"
ours=$root/build/css-prop-ours
build_ours() {
	# build_ours <output> [extra object linked ahead of the archive]
	out=$1
	shift
	cc -std=gnu17 -D_GNU_SOURCE -O1 -Wall -Wextra -o "$out" "$root/tools/oracle/css_prop_ours.c" "$@" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm -lpthread \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
}
build_ours "$ours"

wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}
vectors=$root/build/css-property-vectors
if [ ! -f "$vectors/valid.json" ] || [ -n "$CSS_PROP_ORACLE_REEXTRACT" ]; then
	[ -f "$wpt/css/support/parsing-testcommon.js" ] || { echo "the pinned CSS property pages are not in third_party/css/wpt. Fetch them:  tools/css/fetch.sh" >&2; exit 1; }
	mkdir -p "$vectors"
	python3 -I "$root/tools/css/extract-wpt-props.py" "$wpt" "$vectors" >/dev/null
fi

diff_py="python3 $root/tools/oracle/css_prop_diff.py --ours $ours --wpt $vectors"

if [ $controls = 0 ]; then
	exec $diff_py --work "$work" --stage "$stage"
fi

# ---- the controls ------------------------------------------------------------------------------------------------------

scratch=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$scratch"; rm -f "$log"' EXIT
unset CSS_PROP_ORACLE_COUNT CSS_PROP_ORACLE_SEED CSS_PROP_ORACLE_CHUNK CSS_PROP_ORACLE_SCOPE

# the run the controls are about: the default population, the browsers asked once (or their answers read from a replay directory)
replay=${CSS_PROP_ORACLE_REPLAY:-$scratch/base}
if [ -z "$CSS_PROP_ORACLE_REPLAY" ]; then
	$diff_py --work "$scratch/base" --stage all >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
else
	mkdir -p "$scratch/base"
	cp "$replay/population.tsv" "$scratch/base/"
	$diff_py --work "$scratch/base" --stage ours >/dev/null
	$diff_py --work "$scratch/base" --stage classify --replay "$replay" >"$log" 2>&1 || { echo "the gate does not pass unedited" >&2; cat "$log" >&2; exit 1; }
fi
tail -3 "$log"
base=$scratch/base

expect() {
	what=$1 want=$2
	shift 2
	status=0
	"$@" </dev/null >"$log" 2>&1 || status=$?
	if [ "$status" != 3 ] || ! grep -q "$want" "$log"; then
		echo "control $what: the gate exited $status and did not say '$want'" >&2
		tail -20 "$log" >&2
		exit 1
	fi
	echo "control $what: caught"
}
classify() {
	# classify <extra arguments>: the gate on the base population and answers
	$diff_py --work "$base" --stage classify --replay "$replay" "$@"
}

for p in ours-accepts-a-refused-item ours-refuses-an-accepted-item ours-writes-differently engine-flips-an-acceptance; do
	expect "plant-$p" "UNLISTED" classify --plant "$p"
done
expect plant-control-is-misclassified "CONTROL" classify --plant control-is-misclassified

list=$root/tools/oracle/css_prop_divergences.txt
first=$(awk -F'\t' '!/^#/ && NF == 4 { print $1; exit }' "$list")
if [ -n "$first" ]; then
	awk -F'\t' -v k="$first" '/^#/ || NF != 4 || $1 != k { print }' "$list" > "$scratch/no-key"
	expect list-key-missing "UNLISTED" classify --list "$scratch/no-key"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $2 = $2 + 1; d = 1 } { print }' "$list" > "$scratch/count"
	expect list-count-changed "COUNT" classify --list "$scratch/count"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } $3 != "ours" { $3 = "ours" } { print }' "$list" > "$scratch/ours"
	expect list-class-ours "OURS" classify --list "$scratch/ours"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $3 = "whatever"; d = 1 } { print }' "$list" > "$scratch/class"
	expect list-class-unknown "LIST" classify --list "$scratch/class"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 4 { print; next } !d { $4 = "browser behavior"; d = 1 } { print }' "$list" > "$scratch/blanket"
	expect list-blanket-reason "LIST" classify --list "$scratch/blanket"
	{ cat "$list"; printf 'acc:100:planted-property:x\t1\treference-old\tplanted: a key that never occurs in any population, so it is stale\n'; } > "$scratch/gone"
	expect list-key-gone "GONE" classify --list "$scratch/gone"
	grep -v "^$first	" "$root/tools/oracle/css_prop_witnesses.txt" > "$scratch/no-witness" || true
	expect witness-missing "WITNESS" classify --witnesses "$scratch/no-witness"
	awk -F'\t' 'BEGIN { OFS = "\t" } /^#/ || NF != 5 { print; next } !d { $4 = "OK=a planted answer"; d = 1 } { print }' "$root/tools/oracle/css_prop_witnesses.txt" > "$scratch/bad-witness"
	expect witness-wrong "WITNESS" classify --witnesses "$scratch/bad-witness"
fi
: > "$scratch/empty-list"
expect list-empty "UNLISTED\|CONTROL" classify --list "$scratch/empty-list"
printf 'color\tred\t1\t0\n' > "$scratch/controls"
expect engine-control-wrong "CONTROL" classify --controls "$scratch/controls"

# this library's source with one rule undone, measured against browsers that did not change
plant() {
	name=$1 file=$2 old=$3 new=$4
	base_name=$(basename "$file" .c)
	python3 - "$root/src/css/$file" "$scratch/$file" "$old" "$new" <<'PY'
import sys
src, dst, old, new = sys.argv[1:5]
old = old.replace("\\n", "\n")
new = new.replace("\\n", "\n")
text = open(src).read()
if text.count(old) != 1:
    raise SystemExit("the plant's text is in %s %d times, not once: %r" % (src, text.count(old), old))
open(dst, "w").write(text.replace(old, new))
PY
	cc -std=c17 -O1 -fPIC -DGTEXT_BUILD -I"$root/src/css" -I"$root/src" -I"$root/include" -I"$generated" $cflags \
		-c "$scratch/$file" -o "$scratch/$base_name.o"
	build_ours "$scratch/ours-$name" "$scratch/$base_name.o"
	rm -rf "$scratch/plant-$name"
	mkdir -p "$scratch/plant-$name"
	cp "$base/population.tsv" "$scratch/plant-$name/"
	status=0
	python3 "$root/tools/oracle/css_prop_diff.py" --ours "$scratch/ours-$name" --wpt "$vectors" --work "$scratch/plant-$name" \
		--stage ours >/dev/null 2>&1 || status=$?
	[ $status = 0 ] || { echo "control $name: the planted library could not answer the population" >&2; exit 1; }
	expect "$name" "UNLISTED\|COUNT\|GONE\|CONTROL\|WITNESS" python3 "$root/tools/oracle/css_prop_diff.py" --ours "$scratch/ours-$name" --wpt "$vectors" \
		--work "$scratch/plant-$name" --stage classify --replay "$replay"
}
while IFS="$(printf '\t')" read -r name file old new; do
	case $name in ''|'#'*) continue ;; esac
	plant "$name" "$file" "$old" "$new"
done < "$root/tools/oracle/CSS_PROPERTY_ORACLE_PLANTS"
echo "css property oracle: controls done"
