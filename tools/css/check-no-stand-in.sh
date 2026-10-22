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
# Fail if a CSS source still reads or writes a number through the host.
#
#   tools/css/check-no-stand-in.sh [DIR]   check DIR (default src/css), then show the gate fails
#
# A CSS number is converted by the math library (gmath_decimal_parse) and written from its
# shortest digits (gmath_decimal_shortest): integer arithmetic that reads no locale, no
# rounding mode and no C library, so a stylesheet means the same number on every host. The
# conversion this library had before, gtext_number_strtod (and gtext_number_format_double for
# the serializer), goes through the C library's strtod and printf, which do read the host.
# Nothing in src/css may call them or anything like them again.
#
# The text is read with comments removed (the preprocessor's -fpreprocessed pass), so a
# comment that explains why there is no printf here is not a use of one.
#
# **The gate is then made to fail**: a copy of the directory with a use planted in it must be
# refused, and a copy with a use planted in a comment must not. A check that has never been
# seen to refuse is not a check.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
dir=${1:-$root/src/css}

# A call of anything that converts a number through the host, or the library's old stand-ins.
pattern='(gtext_number_[a-z_]+|text_number_internal|(^|[^a-zA-Z0-9_])(strtod|strtof|strtold|strtol|strtoll|strtoul|atof|atoi|atol|sscanf|snprintf|sprintf|vsnprintf|vsprintf|printf|fprintf|setlocale|localeconv|newlocale|uselocale))[[:space:]]*([(]|.h)'

check() {
	found=0
	for f in "$1"/*.c "$1"/*.h; do
		[ -f "$f" ] || continue
		hits=$(cc -fpreprocessed -dD -E -P -x c "$f" 2>/dev/null | grep -nE "$pattern" || true)
		if [ -n "$hits" ]; then
			echo "$f still reaches the host for a number:" >&2
			echo "$hits" | sed 's/^/    /' >&2
			found=1
		fi
	done
	return $found
}

if ! check "$dir"; then
	echo "src/css must convert and write numbers through the math library only (gmath_decimal_*)." >&2
	exit 1
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cp "$dir"/*.c "$dir"/*.h "$scratch"/
first=$(ls "$scratch"/*.c | head -1)
printf '\nstatic double planted(const char * s) { return strtod(s, 0); }\n' >> "$first"
if check "$scratch" >/dev/null 2>&1; then
	echo "the stand-in gate did not refuse a planted strtod" >&2
	exit 1
fi
cp "$dir"/*.c "$dir"/*.h "$scratch"/
printf '\nstatic double planted(const char * s) { return gtext_number_strtod(0, s, 0); }\n' >> "$first"
if check "$scratch" >/dev/null 2>&1; then
	echo "the stand-in gate did not refuse a planted gtext_number_strtod" >&2
	exit 1
fi
cp "$dir"/*.c "$dir"/*.h "$scratch"/
printf '\n/* no strtod(), printf( or gtext_number_strtod here */\n' >> "$first"
if ! check "$scratch" >/dev/null 2>&1; then
	echo "the stand-in gate refused a comment" >&2
	exit 1
fi
echo "No CSS source reaches the host for a number (and the gate refuses a planted use)."
