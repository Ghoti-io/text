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
#
# Score the at-rule layer against the at-rule parsing vectors of the pinned web-platform-tests (tools/css/extract-wpt-atrules.py). Every failure is a line of
# CSS_ATRULES_EXCEPTIONS (kind, input, reason); a stale line fails. The floor is the least number of vectors the pages must give.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"
wpt=${CSS_PROPERTY_WPT:-$root/third_party/css/wpt}
[ -d "$wpt/css/css-animations/parsing" ] || { echo "the pinned pages are not in third_party/css/wpt. Fetch them:  tools/css/fetch.sh" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (tools/css/extract-wpt-atrules.py)" >&2; exit 1; }
[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-atrules-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_atrules_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"
vectors=$root/build/css-atrules-vectors.tsv
python3 -I "$root/tools/css/extract-wpt-atrules.py" "$wpt" "$vectors"
n=$(grep -vc '^#' "$vectors" || true)
floor=${CSS_ATRULES_FLOOR:-90}
[ "$n" -ge "$floor" ] || { echo "only $n vectors were read; the floor is $floor" >&2; exit 1; }
"$runner" "$vectors" "$root/tools/conformance/CSS_ATRULES_EXCEPTIONS"
