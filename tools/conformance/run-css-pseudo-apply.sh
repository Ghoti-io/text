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
# Hold the table of src/css/css_pseudo_apply.c (the properties that apply to ::first-line, ::first-letter and ::marker) to the recorded answers of Chromium and Firefox
# (tools/conformance/css_pseudo_answers.jsonl) and to the sentences of the pinned drafts that CSS_PSEUDO_SERVED quotes; with --controls, show the gate fail.

set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/conformance/lib.sh"

[ -n "$PREFIX" ] || { echo "PREFIX must be set, as for any build here" >&2; exit 1; }
[ -n "$DEP_PCS" ] || { echo "DEP_PCS must be set; the Makefile passes it" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is needed (tools/css/check-pseudo-apply.py)" >&2; exit 1; }
[ -d "$root/third_party/css/drafts" ] || { echo "the pinned drafts are not in third_party/css/drafts. Fetch them:  tools/css/fetch.sh" >&2; exit 1; }
cflags=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --cflags $DEP_PCS)
libs=$(PKG_CONFIG_PATH="$PREFIX/share/pkgconfig" pkg-config --libs $DEP_PCS)
conformance_library
mkdir -p "$root/build"
runner=$root/build/css-pseudo-runner
cc -O1 -g -D_GNU_SOURCE -o "$runner" "$root/tools/conformance/css_cssom_script_suite.c" \
	-I"$root/include" -I"$generated" $cflags "$archive" $libs -lm \
	-Wl,-rpath,"$PREFIX/lib/ghoti.io"

python3 -I "$root/tools/css/check-pseudo-apply.py" "$runner"
if [ "$1" = "--controls" ]; then
	python3 -I "$root/tools/css/check-pseudo-apply.py" "$runner" --controls
fi
