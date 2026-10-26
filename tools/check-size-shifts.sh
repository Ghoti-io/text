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
# Fail if a source shifts a size_t, unsigned long or int by 32 or more places.
#
#   tools/check-size-shifts.sh [DIR...]    check src, include and tests (or the directories given); then show the gate fails
#
# `(size_t)1 << 40` is undefined where size_t has 32 bits: the compiler refuses the build for i686 (-Wshift-count-overflow under
# -Werror), but the 64-bit build, which is the one run most, accepts it. A test once did this twice
# (tests/test-quadratic.cpp, "effectively unbounded" work limits) and nothing noticed until the 32-bit gate ran. The gate reads the
# text with comments removed and refuses a shift by 32 to 63 of a literal or a cast to size_t, unsigned, unsigned long, long, int or
# uintptr_t/ptrdiff_t/ssize_t. A 64-bit operand (uint64_t, int64_t, an ull literal) is fine, and so is a shift whose count is a
# variable or sizeof expression. The gate is then shown to fail: a planted shift is refused, a shift of a uint64_t and a shift in a
# comment are not.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
if [ $# -gt 0 ]; then
	dirs="$*"
else
	dirs="$root/src $root/include $root/tests"
fi

# a narrow type, then (optionally closing paren and) a literal or a bare name, then << and a count of 32..99 (from 32 to 63 matter; larger is refused too)
pattern='((size_t|unsigned|unsigned long|long|int|uintptr_t|ptrdiff_t|ssize_t)\)? *\(?[0-9a-fA-Fx]+[uUlL]*\)?|static_cast<(size_t|unsigned|unsigned long|long|int|uintptr_t|ptrdiff_t|ssize_t)>\([0-9a-fA-Fx]+[uUlL]*\)) *<< *(3[2-9]|[4-9][0-9])([^0-9]|$)'

check() {
	found=0
	for d in "$@"; do
		for f in $(find "$d" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null | sort); do
			hits=$(cc -fpreprocessed -dD -E -P -x c "$f" 2>/dev/null | sed 's/long long/int64type/g' | grep -nE "$pattern" || true)
			if [ -n "$hits" ]; then
				echo "$f shifts a narrow integer by 32 or more places (undefined where it has 32 bits):" >&2
				echo "$hits" | sed 's/^/    /' >&2
				found=1
			fi
		done
	done
	return $found
}

if ! check $dirs; then
	echo "use SIZE_MAX / 2, a uint64_t, or a width check (sizeof(size_t) > 4 ? ... : ...) instead." >&2
	exit 1
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
printf 'static unsigned long planted(void) { return (size_t)1 << 40; }\n' > "$scratch/a.c"
if check "$scratch" >/dev/null 2>&1; then
	echo "the shift gate did not refuse a planted (size_t)1 << 40" >&2
	exit 1
fi
printf 'static unsigned long planted(void) { return static_cast<size_t>(1) << 33; }\n' > "$scratch/a.cpp"
rm -f "$scratch/a.c"
if check "$scratch" >/dev/null 2>&1; then
	echo "the shift gate did not refuse a planted static_cast<size_t>(1) << 33" >&2
	exit 1
fi
rm -f "$scratch/a.cpp"
printf 'static unsigned long long fine(void) { return (unsigned long long)1 << 40; }\n/* (size_t)1 << 40 in a comment */\nstatic unsigned long long ok(void) { return (uint64_t)1 << 40; }\n' > "$scratch/b.c"
if ! check "$scratch"; then
	echo "the shift gate refused a 64-bit shift or a comment" >&2
	exit 1
fi
echo "check-size-shifts: no narrow shift by 32 or more; the gate refuses a planted one and accepts a 64-bit shift"
