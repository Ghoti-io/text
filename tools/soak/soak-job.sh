#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The Phase 3 fuzz soak, run as one job on the EVO-X2 (see tools/evo at the workspace root).
#
#   tools/evo new text-soak cutil unicode chron regex text
#   tools/evo run JOB -m 40G -t 3h -- sh /work/src/text/tools/soak/soak-job.sh SECONDS [harness...]
#
# (the job is mounted at $JOB; JOB, PREFIX and RESULTS are set by tools/evo).  Builds the
# dependencies into $PREFIX, builds every harness once, runs each harness with two seeds in
# parallel for SECONDS (-seed=1 and -seed=2, separate output corpora, the tracked seeds read
# as a second corpus), then reduces: -merge=1 of seeds + both runs into results/merged/<name>.
# Results: results/logs/<name>-s<seed>.log, .exit, results/artifacts/, results/merged/,
# results/summary.txt (tools/soak/summary.py).
set -u
T=${1:-1860}
shift
ALL="xml:fuzz_xml xml-writer:fuzz_xml_writer html-tokenizer:fuzz_html_tokenizer html-tree:fuzz_html_tree html:fuzz_html html-serialize:fuzz_html_serialize encoding:fuzz_encoding markup-tree:fuzz_markup_tree"
if [ $# -gt 0 ]; then
	SEL=""
	for w in "$@"; do for p in $ALL; do [ "${p%%:*}" = "$w" ] && SEL="$SEL $p"; done; done
else
	SEL="$ALL"
fi
R=${RESULTS:-$JOB/results}
mkdir -p "$R/logs" "$R/artifacts" "$R/corpus" "$R/merged"
cd "$JOB/src" || exit 1
for l in cutil unicode chron regex; do
	make -C "$l" -j8 install PREFIX="$PREFIX" > "$R/logs/dep-$l.log" 2>&1 || { echo "dependency $l failed"; tail -30 "$R/logs/dep-$l.log"; exit 1; }
done
cd text || exit 1
# The harnesses read the pinned data (fuzz_encoding aborts without third_party/encoding): it is not shipped, so it is fetched, as on a fresh clone.
# (The first soak of Phase 3k ran without it: fuzz_encoding aborted in its first four executions. The script now fails if a fetch does.)
for f in tools/encoding/fetch.sh tools/html/fetch.sh tools/xml/fetch.sh; do
	sh "$f" > "$R/logs/fetch-$(basename "$(dirname "$f")").log" 2>&1 || { echo "fetch failed: $f"; tail -5 "$R/logs/fetch-$(basename "$(dirname "$f")").log"; exit 1; }
done
for p in $SEL; do mkdir -p "tests/fuzz/corpus/${p%%:*}"; done
targets=""
for p in $SEL; do targets="$targets fuzz-${p%%:*}"; done
make -j8 $targets PREFIX="$PREFIX" > "$R/logs/build.log" 2>&1 || { echo "build failed"; tail -40 "$R/logs/build.log"; exit 1; }
first=${SEL# }; first=${first%% *}; BIN=$(dirname "$(find build -name "${first##*:}" -type f | head -1)")
echo "harness binaries in $BIN; $T seconds, seeds 1 and 2"
date +%s > "$R/started"
for p in $SEL; do
	n=${p%%:*}; b=${p##*:}
	for s in 1 2; do
		mkdir -p "$R/corpus/$n-s$s"
		(
			"$BIN/$b" "$R/corpus/$n-s$s" "tests/fuzz/corpus/$n" -seed=$s -max_total_time="$T" \
				-timeout=30 -rss_limit_mb=3500 -print_final_stats=1 \
				-artifact_prefix="$R/artifacts/$n-s$s-" > "$R/logs/$n-s$s.log" 2>&1
			echo $? > "$R/logs/$n-s$s.exit"
		) &
	done
done
wait
date +%s > "$R/ended"
for p in $SEL; do
	n=${p%%:*}; b=${p##*:}
	mkdir -p "$R/merged/$n"
	"$BIN/$b" -merge=1 -rss_limit_mb=3500 "$R/merged/$n" "tests/fuzz/corpus/$n" "$R/corpus/$n-s1" "$R/corpus/$n-s2" > "$R/logs/$n-merge.log" 2>&1
	echo $? > "$R/logs/$n-merge.exit"
	# the seed corpus alone, for the coverage the soak started from
	"$BIN/$b" -runs=0 "tests/fuzz/corpus/$n" > "$R/logs/$n-seed.log" 2>&1
	du -sk "$R/corpus/$n-s1" "$R/corpus/$n-s2" "$R/merged/$n" > "$R/logs/$n-sizes.txt"
	ls "$R/corpus/$n-s1" "$R/corpus/$n-s2" | wc -l > "$R/logs/$n-nfiles.txt"
done
python3 tools/soak/summary.py "$R" > "$R/summary.txt" 2>&1 || true
exit 0
