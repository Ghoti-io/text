#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# check-tsan-plants.sh PREFIX
#
# The ThreadSanitizer gate, seen to fail. A copy of the tracked tree is made in a scratch directory; the unedited tree is built for ThreadSanitizer and each program of
# tools/tsan/tsan-plants.txt is run (the control: it must report nothing); then each plant is applied alone, its program rebuilt and run, and it has to end with a
# ThreadSanitizer report. PREFIX holds the dependencies, cutil built with ThreadSanitizer among them (tools/tsan/require-tsan-cutil.sh refuses otherwise, here as in
# `make test-tsan`). The scratch directory is $TMPDIR (or /tmp) and is removed.
set -u
prefix=${1:?usage: check-tsan-plants.sh PREFIX}
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/tsan-plants.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
# what git tracks; a tree shipped without .git (a build host's job directory) is copied as it is, minus build output and the fetched third-party data
if [ -d "$root/.git" ]; then
	(cd "$root" && git ls-files -z | tar --null -T - -cf -) | (cd "$work" && tar -xf -) || exit 1
else
	(cd "$root" && find . -path ./build -prune -o -path ./third_party -prune -o -type f -print0 | tar --null -T - -cf -) | (cd "$work" && tar -xf -) || exit 1
fi
[ -e "$root/third_party" ] && ln -s "$root/third_party" "$work/third_party"
cd "$work" || exit 1
export PKG_CONFIG_PATH="$prefix/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
dir=build/linux/release-tsan/apps
jobs=${JOBS_MAKE:-4}
build() { make -j"$jobs" PREFIX="$prefix" "$@" >"$work/build.log" 2>&1 || { echo "check-tsan-plants: the build of $* failed" >&2; tail -20 "$work/build.log" >&2; exit 1; }; }
run() { # program -> log in $work/run.log, status on stdout
	LD_PRELOAD= LD_LIBRARY_PATH="$dir" GTEXT_TEST_SLOWDOWN=4 TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:allocator_may_return_null=1 \
		timeout 900 "$dir/$1" --gtest_brief=1 >"$work/run.log" 2>&1
	echo $?
}
programs=$(grep -v '^#' "$root/tools/tsan/tsan-plants.txt" | awk -F'\t' 'NF >= 5 { print $5 }' | sort -u)
for p in $programs; do build "$dir/$p"; done
sh "$root/tools/tsan/require-tsan-cutil.sh" "$dir/$(echo $programs | cut -d' ' -f1)" "$dir" || exit 1
bad=0
for p in $programs; do
	rc=$(run "$p")
	if [ "$rc" != 0 ] || grep -q 'WARNING: ThreadSanitizer' "$work/run.log"; then
		echo "control $p: FAILED without a plant (rc $rc)"; sed -n 1,40p "$work/run.log"; bad=$((bad + 1))
	else
		echo "control $p: clean"
	fi
done
grep -v '^#' "$root/tools/tsan/tsan-plants.txt" | awk -F'\t' 'NF >= 5' | while IFS="$(printf '\t')" read -r name file old new program; do
	python3 -I - "$file" "$old" "$new" <<'PY' || { echo "plant $name: the edit does not apply"; exit 3; }
import sys
f, old, new = sys.argv[1], sys.argv[2].replace('\\n', '\n'), sys.argv[3].replace('\\n', '\n')
s = open(f, encoding='utf-8').read()
if s.count(old) != 1:
    sys.exit(1)
open(f, 'w', encoding='utf-8').write(s.replace(old, new))
PY
	build "$dir/$program"
	rc=$(run "$program")
	if [ "$rc" != 0 ] && grep -q 'WARNING: ThreadSanitizer' "$work/run.log"; then
		echo "plant $name: caught ($program, $(grep -c 'WARNING: ThreadSanitizer' "$work/run.log") report(s), rc $rc)"
	else
		echo "plant $name: NOT caught ($program, rc $rc)"; sed -n 1,20p "$work/run.log"; echo "$name" >> "$work/missed"
	fi
	cp "$root/$file" "$file"
done || exit 1
[ -e "$work/missed" ] && exit 1
[ "$bad" -eq 0 ]
