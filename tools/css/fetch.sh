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
# Fetch the two third-party inputs the CSS Syntax work is scored against.
#
#   css-parsing-tests/  The JSON vectors of tokenizer, parser and An+B
#                       behavior that tinycss2 and rust-cssparser are scored
#                       against, pinned by commit in
#                       tools/css/CSS_PARSING_TESTS_COMMIT.
#   wpt/                web-platform-tests, sparse: only css/css-syntax, css/css-values and css/css-color (the value
#                       tests of the math, color and function types: tools/css/extract-wpt-values.py reads them),
#                       pinned by commit in tools/css/WPT_COMMIT (the same
#                       commit as the HTML pin, tools/html/WPT_COMMIT, so
#                       that the library is scored against one revision of
#                       the suite). tools/css/extract-wpt.py turns its tables
#                       into vectors.
#
# Neither is committed: both are reproducible from a pin and a URL. Each
# checkout is verified to be exactly its commit, and a mismatch leaves
# nothing behind. Raising a pin is a deliberate edit to its file, in a commit
# that says what the new data changed.
#
# Everything lands in third_party/css/, which .gitignore excludes.
#
# Usage:  tools/css/fetch.sh

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/css"
want_cpt=$(cat "$root/tools/css/CSS_PARSING_TESTS_COMMIT")
want_wpt=$(cat "$root/tools/css/WPT_COMMIT")

mkdir -p "$dest"

# ---- css-parsing-tests ----------------------------------------------------
repo="$dest/css-parsing-tests"
if [ -d "$repo/.git" ] && [ "$(git -C "$repo" rev-parse HEAD 2>/dev/null)" = "$want_cpt" ]; then
	printf 'have    css-parsing-tests @ %s\n' "$want_cpt"
else
	printf 'fetch   css-parsing-tests @ %s\n' "$want_cpt"
	rm -rf "$repo.partial"
	mkdir -p "$repo.partial"
	git -C "$repo.partial" init --quiet
	git -C "$repo.partial" remote add origin https://github.com/SimonSapin/css-parsing-tests.git
	git -C "$repo.partial" fetch --quiet --depth 1 origin "$want_cpt"
	git -C "$repo.partial" checkout --quiet --detach FETCH_HEAD
	got=$(git -C "$repo.partial" rev-parse HEAD)
	if [ "$got" != "$want_cpt" ]; then
		rm -rf "$repo.partial"
		printf 'css-parsing-tests: checked out %s, pinned %s\n' "$got" "$want_cpt" >&2
		exit 1
	fi
	rm -rf "$repo"
	mv "$repo.partial" "$repo"
fi

# ---- web-platform-tests, css/css-syntax only ------------------------------
wpt="$dest/wpt"
if [ -d "$wpt/.git" ] && [ "$(git -C "$wpt" rev-parse HEAD 2>/dev/null)" = "$want_wpt" ]; then
	# (a checkout made before the value tests were scored has only css/css-syntax: add what it lacks, at the same commit)
	if [ ! -d "$wpt/css/css-values" ] || [ ! -d "$wpt/css/css-color/parsing" ]; then
		git -C "$wpt" sparse-checkout set css/css-syntax css/css-values css/css-color
	fi
	printf 'have    wpt @ %s\n' "$want_wpt"
else
	printf 'fetch   wpt (css/css-syntax, css-values, css-color) @ %s\n' "$want_wpt"
	rm -rf "$wpt.partial"
	mkdir -p "$wpt.partial"
	git -C "$wpt.partial" init --quiet
	git -C "$wpt.partial" remote add origin https://github.com/web-platform-tests/wpt.git
	git -C "$wpt.partial" sparse-checkout init --cone
	git -C "$wpt.partial" sparse-checkout set css/css-syntax css/css-values css/css-color
	git -C "$wpt.partial" fetch --quiet --depth 1 --filter=blob:none origin "$want_wpt"
	git -C "$wpt.partial" checkout --quiet --detach FETCH_HEAD
	got=$(git -C "$wpt.partial" rev-parse HEAD)
	if [ "$got" != "$want_wpt" ]; then
		rm -rf "$wpt.partial"
		printf 'wpt: checked out %s, pinned %s\n' "$got" "$want_wpt" >&2
		exit 1
	fi
	rm -rf "$wpt"
	mv "$wpt.partial" "$wpt"
fi
