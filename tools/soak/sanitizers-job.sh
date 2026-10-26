#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The test programs of the XML, HTML, Encoding and markup work under a checker, full strength, as one job
# on a build host.
#
# The job directory is $JOB and holds src/<library> for cutil, security, chron, unicode, regex, math and text; PREFIX names an empty
# prefix and RESULTS (default $JOB/results) the directory the results go to:
#
#   JOB=DIR PREFIX=DIR/prefix sh DIR/src/text/tools/soak/sanitizers-job.sh asan|tsan|valgrind|plain [REGEX]
#
# asan: the Makefile's ASan+UBSan build and options; tsan: its ThreadSanitizer build and options; valgrind:
# the release build under valgrind with the Makefile's VALGRIND_FLAGS (leaks all kinds, origins, the repository's
# suppressions); plain: the release build, no checker (the control for the other three: it must pass on the box
# too).  GTEXT_TEST_FULL=1 is set in all of them, so the strided sweeps (testEncodingCjk's, testEncodingFuzzSeeds'
# time allowance) run in full.  REGEX picks the programs (default: Html, Xml, Xhtml, Encoding, Markup, Quadratic).
# The dependencies are built into $PREFIX first, then every tools/*/fetch.sh runs (the data the vector suites read
# is not shipped: it is fetched, as on a fresh clone).  Every program is run even if one fails; the status is the
# number that did not pass.  results/<mode>/<program>.log, results/<mode>.summary.
set -u
MODE=${1:?asan|tsan|valgrind|plain}
FILTER=${2:-^test(Html|Xml|Xhtml|Encoding|Markup|Quadratic)}
R=${RESULTS:-$JOB/results}
mkdir -p "$R/$MODE" "$R/logs"
cd "$JOB/src" || exit 1
# tsan: cutil is built with the checker too. Its atomics and mutexes are the synchronization that other code publishes
# through, and a checker that cannot see inside them reports every such publication as a race.
tsan_dep=""
[ "$MODE" = tsan ] && tsan_dep=1
for l in cutil security chron unicode regex math; do
	extra_dep=""
	[ "$l" = cutil ] && [ -n "$tsan_dep" ] && extra_dep="EXTRA_CFLAGS=-fsanitize=thread EXTRA_LDFLAGS=-fsanitize=thread"
	make -C "$l" -j${JOBS_MAKE:-8} install PREFIX="$PREFIX" $extra_dep > "$R/logs/dep-$l.log" 2>&1 || { echo "dependency $l failed"; tail -30 "$R/logs/dep-$l.log"; exit 1; }
done
cd text || exit 1
for f in tools/*/fetch.sh; do
	echo "fetch: $f"
	sh "$f" > "$R/logs/fetch-$(basename "$(dirname "$f")").log" 2>&1 || { echo "FETCH FAILED: $f"; tail -5 "$R/logs/fetch-$(basename "$(dirname "$f")").log"; }
done
names=$(echo 'print-%: ; @echo $($*)' | make -s --no-print-directory -f Makefile -f - PREFIX="$PREFIX" print-TEST_NAMES 2>/dev/null)
sel=$(echo "$names" | tr ' ' '\n' | grep -E "$FILTER" | sort)
case $MODE in
	asan) dir=build/linux/release-asan/apps; tgt=$(for n in $sel; do printf '%s ' "$dir/$n"; done) ;;
	tsan) dir=build/linux/release-tsan/apps; tgt=$(for n in $sel; do printf '%s ' "$dir/$n"; done) ;;
	valgrind|plain) dir=build/linux/release/apps; tgt=$(for n in $sel; do printf '%s ' "$dir/$n"; done) ;;
	*) echo "unknown mode $MODE"; exit 2 ;;
esac
extra=""
[ "$MODE" = asan ] && extra="$dir/libghoti.io-text-0-asan.so"
make -j${JOBS_MAKE:-8} PREFIX="$PREFIX" $extra $tgt > "$R/logs/build-$MODE.log" 2>&1 || { echo "build failed"; grep -a -E " error|Error [0-9]" "$R/logs/build-$MODE.log" | head -20; exit 1; }
export GTEXT_TEST_FULL=1
# the one test that reads a clock (testEncodingFuzzSeeds) is told how slow the checker is
[ "$MODE" = valgrind ] && export GTEXT_TEST_SLOWDOWN=${GTEXT_TEST_SLOWDOWN:-200}
echo "$MODE, $(echo "$sel" | wc -w) programs, GTEXT_TEST_FULL=1"
fails=""; ok=0
for n in $sel; do
	t0=$(date +%s)
	case $MODE in
		asan) LD_PRELOAD= LD_LIBRARY_PATH="$dir" ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 timeout 14400 "$dir/$n" --gtest_brief=1 > "$R/$MODE/$n.log" 2>&1 ;;
		tsan) LD_PRELOAD= LD_LIBRARY_PATH="$dir" TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:allocator_may_return_null=1 timeout 14400 "$dir/$n" --gtest_brief=1 > "$R/$MODE/$n.log" 2>&1 ;;
		valgrind) LD_LIBRARY_PATH="$dir" timeout 21600 valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 --suppressions=tools/valgrind.supp "$dir/$n" --gtest_brief=1 > "$R/$MODE/$n.log" 2>&1 ;;
		plain) LD_LIBRARY_PATH="$dir" timeout 14400 "$dir/$n" --gtest_brief=1 > "$R/$MODE/$n.log" 2>&1 ;;
	esac
	rc=$?
	t1=$(date +%s)
	p=$(sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' "$R/$MODE/$n.log" | tail -1)
	printf '%-30s rc=%s passed=%s secs=%s\n' "$n" "$rc" "${p:-0}" $((t1 - t0)) | tee -a "$R/$MODE.summary"
	if [ $rc -ne 0 ] || [ "${p:-0}" -eq 0 ]; then fails="$fails $n"; else ok=$((ok + 1)); fi
done
echo "$MODE: $ok programs passed; failed:${fails:- none}" | tee -a "$R/$MODE.summary"
[ -z "$fails" ]
