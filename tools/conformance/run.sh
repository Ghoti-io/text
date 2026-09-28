#!/bin/sh
# Score this library against yaml-test-suite, and optionally against a
# reference implementation for comparison.
#
#   tools/conformance/run.sh              # this library
#   tools/conformance/run.sh js           # js-yaml, if it can be found
#   tools/conformance/run.sh py           # PyYAML, if it is installed
#   tools/conformance/run.sh roundtrip    # parse -> write -> parse, this library
#
# The suite is a corpus of inputs and tests no writer at all.  The roundtrip
# mode runs it backwards - every document the parser accepts is written out
# again and re-read - which costs nothing and measures the half of the module
# the suite cannot see.  YTS_RT_BLOCK asks for block style instead of the
# default flow; YTS_RT_MIN sets a floor on the by-value figure.
#
# The suite is cloned into build/yaml-test-suite on first use and checked out at
# the commit named in tools/conformance/YAML_SUITE_COMMIT.  YTS_MIN sets a floor
# the pass rate must meet, YTS_MIN_CORPUS a floor on how much of the corpus was
# checked at all; YTS_REPORT names a file to
# write the failing cases to.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
suite=${YTS_SUITE:-$root/build/yaml-test-suite}
commit=$(cat "$root/tools/conformance/YAML_SUITE_COMMIT")
which=${1:-ours}

if [ ! -d "$suite/src" ]; then
	echo "fetching yaml-test-suite into $suite"
	git clone https://github.com/yaml/yaml-test-suite.git "$suite"
fi
# The corpus is pinned. A score is a percentage, and a percentage that moves
# because somebody upstream added or changed cases is not a measurement of
# this library - run-json-schema.sh has pinned its suite for that reason since
# it was written, and these three were still tracking whatever the default
# branch held on the day they ran.
if [ "$(git -C "$suite" rev-parse HEAD)" != "$commit" ]; then
	git -C "$suite" fetch --quiet origin "$commit" 2>/dev/null || git -C "$suite" fetch --quiet
	git -C "$suite" checkout --quiet "$commit"
fi

case "$which" in
ours)
	[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
	[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it (see DEP_PCS there)" >&2; exit 1; }
	# chron as well as cutil: yaml_dom.h has included <ghoti.io/chron/chron.h>
	# since !!timestamp stopped being parsed by hand, and this script asked
	# only for cutil - so `make conformance` stopped compiling at that commit
	# and nobody noticed, because it is not one of the targets `make test`
	# runs. Both lookup directories, the way run-json-schema.sh does it.
	pc="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
	cflags=$(PKG_CONFIG_PATH="$pc" pkg-config --cflags $DEP_PCS)
	libs=$(PKG_CONFIG_PATH="$pc" pkg-config --libs $DEP_PCS)
	conformance_library
	runner=$suite/../yts-runner
	# The same rpath the Makefile links with, so the runner finds cutil and
	# chron without the caller having to set LD_LIBRARY_PATH.
	cc -O1 -o "$runner" "$root/tools/conformance/yaml_test_suite.c" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	# The second runner answers the cases that assert an event stream rather
	# than a value - a tenth of the corpus, which nothing asked until it
	# existed.  Only this library has one; the reference implementations below
	# are driven through the JSON interface alone, so a comparison run scores
	# fewer cases and the corpus line says so.
	events=$suite/../yts-events
	cc -O1 -o "$events" "$root/tools/conformance/yaml_event_suite.c" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	YTS_EVENTS=$events
	export YTS_EVENTS
	set -- "$runner"
	;;
roundtrip)
	[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
	pc="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
	cflags=$(PKG_CONFIG_PATH="$pc" pkg-config --cflags $DEP_PCS)
	libs=$(PKG_CONFIG_PATH="$pc" pkg-config --libs $DEP_PCS)
	conformance_library
	runner=$suite/../yts-runner
	rt=$suite/../yts-roundtrip
	for pair in "yaml_test_suite.c $runner" "yaml_roundtrip.c $rt"; do
		set -- $pair
		cc -O1 -o "$2" "$root/tools/conformance/$1" \
			-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
			-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	done
	exec python3 "$root/tools/conformance/yaml_roundtrip.py" "$suite" "$runner" "$rt"
	;;
fastpath)
	[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
	pc="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
	cflags=$(PKG_CONFIG_PATH="$pc" pkg-config --cflags $DEP_PCS)
	libs=$(PKG_CONFIG_PATH="$pc" pkg-config --libs $DEP_PCS)
	conformance_library
	fp=$suite/../yts-fastpath
	cc -O1 -o "$fp" "$root/tools/conformance/yaml_fastpath_diff.c" \
		-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
		-Wl,-rpath,"$PREFIX/lib/ghoti.io"
	exec python3 "$root/tools/conformance/yaml_fastpath.py" "$suite" "$fp"
	;;
js)
	js=$(find / -maxdepth 8 -type d -name js-yaml 2>/dev/null | head -1)
	[ -n "$js" ] || { echo "js-yaml not found" >&2; exit 1; }
	cat > "$suite/../ref-js.js" <<JS
const yaml = require('$js');
let src=''; process.stdin.on('data',d=>src+=d);
process.stdin.on('end',()=>{ try{ const o=[];
  yaml.loadAll(src, d=>o.push(d===undefined?null:d));
  for(const d of o) console.log(JSON.stringify(d===undefined?null:d));
} catch(e){ console.log('FAIL parse: '+e.name); } });
JS
	set -- node "$suite/../ref-js.js"
	;;
py)
	cat > "$suite/../ref-py.py" <<PY
import sys, yaml, json
src = sys.stdin.buffer.read().decode('utf-8','replace')
try:
    for d in yaml.safe_load_all(src):
        print(json.dumps(d, ensure_ascii=False, default=str))
except Exception as e:
    print('FAIL parse: %s' % type(e).__name__)
PY
	set -- python3 "$suite/../ref-py.py"
	;;
*)
	echo "usage: $0 [ours|js|py]" >&2; exit 1 ;;
esac

exec python3 "$root/tools/conformance/yaml_test_suite.py" "$suite" "$@"
