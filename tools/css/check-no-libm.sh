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
# Fail if a CSS source references the C library's <math.h>.
#
#   tools/css/check-no-libm.sh [DIR]   check DIR (default src/css), then show the gate fails
#
# Every calculation of the math functions, the colors and the units is IEEE double arithmetic through the `math` library
# (gmath_sin, gmath_atan2, gmath_pow ...), so that a result is the same on every host: the C library's sin(), pow() and the rest differ
# between libms and between versions, and rounding of the last place is exactly what a serialization shows. `<math.h>` is not
# included, and none of its functions, constants or classification macros is named: NaN and the infinities are made by
# arithmetic on a volatile zero, and tested by comparison.
#
# The text is read with comments removed (the preprocessor's -fpreprocessed pass). **The gate is then made to fail**: a copy of
# the directory with a use planted in it must be refused, and a use planted in a comment must not.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
dir=${1:-$root/src/css}

names='sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|hypot|fabs|fabsf|floor|ceil|trunc|round|rint|nearbyint|lround|llround|lrint|fmod|remainder|remquo|copysign|frexp|ldexp|scalbn|scalbln|modf|fmin|fmax|fdim|fma|nan|nanf|isnan|isinf|isfinite|isnormal|signbit|fpclassify|erf|erfc|tgamma|lgamma'
pattern="(#[[:space:]]*include[[:space:]]*<(math|tgmath|complex)\\.h>|(^|[^a-zA-Z0-9_])(${names})[[:space:]]*\\(|(^|[^a-zA-Z0-9_])(INFINITY|NAN|HUGE_VAL|HUGE_VALF|M_PI|M_E|FP_NAN|FP_INFINITE)([^a-zA-Z0-9_]|\$))"

check() {
	found=0
	for f in "$1"/*.c "$1"/*.h; do
		[ -f "$f" ] || continue
		# -dD keeps #include lines of directives that were not followed; -fpreprocessed leaves them alone
		hits=$(cc -fpreprocessed -dD -E -P -x c "$f" 2>/dev/null | grep -nE "$pattern" || true)
		if [ -n "$hits" ]; then
			echo "$f references the C library's <math.h>:" >&2
			echo "$hits" | sed 's/^/    /' >&2
			found=1
		fi
	done
	return $found
}

if ! check "$dir"; then
	echo "src/css must do its arithmetic through the math library only (gmath_*), never <math.h>." >&2
	exit 1
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cp "$dir"/*.c "$dir"/*.h "$scratch"/
first=$(ls "$scratch"/*.c | head -1)
printf '\nstatic double planted(double x) { return sin(x); }\n' >> "$first"
if check "$scratch" >/dev/null 2>&1; then
	echo "the libm gate did not refuse a planted sin()" >&2
	exit 1
fi
cp "$dir"/*.c "$dir"/*.h "$scratch"/
printf '\n#include <math.h>\n' >> "$first"
if check "$scratch" >/dev/null 2>&1; then
	echo "the libm gate did not refuse a planted #include <math.h>" >&2
	exit 1
fi
cp "$dir"/*.c "$dir"/*.h "$scratch"/
printf '\nstatic double planted(void) { return INFINITY; }\n' >> "$first"
if check "$scratch" >/dev/null 2>&1; then
	echo "the libm gate did not refuse a planted INFINITY" >&2
	exit 1
fi
cp "$dir"/*.c "$dir"/*.h "$scratch"/
printf '\n/* no sin(x), pow( or <math.h> here, and no INFINITY */\n' >> "$first"
if ! check "$scratch" >/dev/null 2>&1; then
	echo "the libm gate refused a comment" >&2
	exit 1
fi
echo "check-css-libm: src/css names nothing of <math.h>"
