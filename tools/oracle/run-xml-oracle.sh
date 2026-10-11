#!/bin/sh
# Compare this library's XML parser against libxml2 and expat.
#
#   tools/oracle/run-xml-oracle.sh [--propose | --plant KIND | --controls]
#
# Two references in one pinned container (containers/xml), run namespace-aware and
# not; every input scored, the refused ones included; every disagreement listed in
# tools/oracle/xml_disagreements.txt with a class and a reason. See xml_diff.py.
#
# XML_ORACLE_COUNT and XML_ORACLE_SEED size and seed the generated part of the
# population. Needs the built-here image: make oracle-images.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library

mkdir -p "$root/build"
runner=$root/build/xml-oracle-ours
cc -O1 -o "$runner" "$root/tools/oracle/xml_ours.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

# --controls: the gate must be seen to fail. Every planted corruption of our
# answers (xml_diff.py --plant) has to make it exit 3 with the heading for that
# kind of failure; a plant that passes, or that dies of something else (a missing
# image looks like a failure too), is itself a failure of this script.
if [ "$1" = "--controls" ]; then
	log=$(mktemp)
	trap 'rm -f "$log"' EXIT
	# <plant> <heading it must produce> <what the difference must look like>
	while read -r plant want detail; do
		status=0
		python3 "$root/tools/oracle/oracle_run.py" xml -- \
			python3 "$root/tools/oracle/xml_diff.py" "$runner" --plant "$plant" </dev/null >/dev/null 2>"$log" || status=$?
		if [ "$status" != 3 ] || ! grep -q "$want" "$log" || ! grep -q "$detail" "$log"; then
			echo "control $plant: the gate exited $status and did not say '$want' with '$detail'" >&2
			cat "$log" >&2
			exit 1
		fi
		echo "control $plant: caught"
	done <<'LIST'
flip-verdict Verdict.disagreements we.\(accept\|refuse\)
drop-attribute Event-list.differences theirs.A
alter-text Event-list.differences ours.T
lose-whitespace Event-list.differences theirs.T
no-list Verdict.disagreements we.\(accept\|refuse\)
stale-entry Listed.disagreements.that.are.gone \[ours=
change-signature signature.has.changed \(changed\)
LIST
	exit 0
fi

exec python3 "$root/tools/oracle/oracle_run.py" xml -- \
	python3 "$root/tools/oracle/xml_diff.py" "$runner" "$@"
