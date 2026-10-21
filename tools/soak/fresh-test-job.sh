#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# `suite/install.sh --test` for text, as one job on a build host, from a tree that has no third_party/ (a copy of what git
# tracks, so a fresh clone's state): the dependencies are built and installed into $PREFIX, every tools/*/fetch.sh
# runs (network), the pins are checked (tools/consumer/check-pins.sh), and `make test` runs, gates included.
#
# The job directory is $JOB and holds src/<library> for cutil, unicode, chron, regex and text; PREFIX names an empty
# prefix and RESULTS (default $JOB/results) the directory the results go to:
#
#   JOB=DIR PREFIX=DIR/prefix sh DIR/src/text/tools/soak/fresh-test-job.sh
set -u
R=${RESULTS:-$JOB/results}
mkdir -p "$R"
cd "$JOB/src" || exit 1
for l in cutil unicode chron regex; do
	make -C "$l" -j8 install PREFIX="$PREFIX" > "$R/dep-$l.log" 2>&1 || { echo "dependency $l failed"; tail -20 "$R/dep-$l.log"; exit 1; }
done
cd text || exit 1
test ! -d third_party || { echo "third_party/ exists: not a fresh tree"; exit 1; }
for f in tools/*/fetch.sh; do
	echo "== $f"
	sh "$f" > "$R/fetch-$(basename "$(dirname "$f")").log" 2>&1 || { echo "FETCH FAILED"; tail -5 "$R/fetch-$(basename "$(dirname "$f")").log"; exit 1; }
done
sh tools/consumer/check-pins.sh . || exit 1
# The two gates that ask a reference parser in a container (idna and nfc oracles) cannot run in a container: they need docker, which the job
# does not have, and they are not this work's. Every other gate runs.
make -j8 test PREFIX="$PREFIX" TEST_GATES='$(filter-out check-idna-oracle check-nfc-oracle,$(ALL_TEST_GATES))' > "$R/make-test.log" 2>&1
rc=$?
echo "make test exit $rc"
tail -40 "$R/make-test.log"
exit $rc
