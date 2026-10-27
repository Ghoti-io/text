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
#   wpt/                web-platform-tests, sparse: the css/ directory (56,344 files, 246 MB on disk when checked out; the
#                       css-syntax tables that tools/css/extract-wpt.py turns into vectors, the value tests of the math, color
#                       and function types that tools/css/extract-wpt-values.py reads, and every property parsing, computed
#                       value and serialization test that tools/css/extract-wpt-props.py reads as data, and the custom property pages of css-variables and
#                       css-properties-values-api that tools/css/extract-wpt-variables.py reads), pinned by commit in
#                       tools/css/WPT_COMMIT (the same commit as the HTML pin, tools/html/WPT_COMMIT, so that the library
#                       is scored against one revision of the suite).
#
#   webref/             The W3C's machine-readable extracts of the CSS specifications (w3c/webref, branch `curated`),
#                       sparse: ed/css (one file per specification series) and the three css-values definition files
#                       of ed/dfns, pinned by commit in tools/css/WEBREF_COMMIT and, file by file, by SHA-256 in
#                       tools/css/WEBREF_SHA256 (the manifest tools/css/gen-css-tables.py also checks). The tables of
#                       src/css/tables/ are generated from it.
#
# None is committed: all are reproducible from a pin and a URL. Each
# checkout is verified to be exactly its commit, and a mismatch leaves
# nothing behind. Raising a pin is a deliberate edit to its file, in a commit
# that says what the new data changed.
#
# The webref checkout is verified three ways: HEAD is the pinned commit, every manifest file hashes as pinned, and
# the checkout holds no file the manifest does not list. Any failure removes the partial checkout and names the file.
#
# Everything lands in third_party/css/, which .gitignore excludes.
#
# Usage:  tools/css/fetch.sh

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/css"
want_cpt=$(cat "$root/tools/css/CSS_PARSING_TESTS_COMMIT")
want_wpt=$(cat "$root/tools/css/WPT_COMMIT")
want_webref=$(cat "$root/tools/css/WEBREF_COMMIT")
manifest="$root/tools/css/WEBREF_SHA256"

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

# ---- web-platform-tests, css/ ----------------------------------------------
wpt="$dest/wpt"
if [ -d "$wpt/.git" ] && [ "$(git -C "$wpt" rev-parse HEAD 2>/dev/null)" = "$want_wpt" ]; then
	# (a checkout made before the property tests were scored has only some directories of css/: add what it lacks, at the same commit)
	if [ ! -d "$wpt/css/support" ] || [ ! -d "$wpt/css/css-grid/parsing" ] || [ ! -d "$wpt/css/css-values" ]; then
		git -C "$wpt" sparse-checkout set css
	fi
	printf 'have    wpt @ %s\n' "$want_wpt"
else
	printf 'fetch   wpt (css/) @ %s\n' "$want_wpt"
	rm -rf "$wpt.partial"
	mkdir -p "$wpt.partial"
	git -C "$wpt.partial" init --quiet
	git -C "$wpt.partial" remote add origin https://github.com/web-platform-tests/wpt.git
	git -C "$wpt.partial" sparse-checkout init --cone
	git -C "$wpt.partial" sparse-checkout set css
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

# ---- w3c/webref, ed/css and the css-values definitions ----------------------
# The files the manifest covers: every file of ed/css and the three definition files of ed/dfns.
webref="$dest/webref"
webref_verify() {
	"$root/tools/css/webref-verify.sh" "$1" "$manifest" "$want_webref"
}
if [ -d "$webref/.git" ] && webref_verify "$webref"; then
	printf 'have    webref @ %s\n' "$want_webref"
else
	printf 'fetch   webref (ed/css, ed/dfns/css-values-{3,4,5}) @ %s\n' "$want_webref"
	rm -rf "$webref.partial"
	mkdir -p "$webref.partial"
	git -C "$webref.partial" init --quiet
	git -C "$webref.partial" remote add origin https://github.com/w3c/webref.git
	git -C "$webref.partial" sparse-checkout init --no-cone
	git -C "$webref.partial" sparse-checkout set /ed/css/ /ed/dfns/css-values-3.json /ed/dfns/css-values-4.json /ed/dfns/css-values-5.json
	git -C "$webref.partial" fetch --quiet --depth 1 --filter=blob:none origin "$want_webref"
	git -C "$webref.partial" checkout --quiet --detach FETCH_HEAD
	if ! webref_verify "$webref.partial"; then
		rm -rf "$webref.partial"
		printf 'webref: nothing was kept. Raising the pin is a deliberate edit of tools/css/WEBREF_COMMIT and WEBREF_SHA256.\n' >&2
		exit 1
	fi
	rm -rf "$webref"
	mv "$webref.partial" "$webref"
fi

# ---- the @webref/css package, the second reading of the same data -------------------------------------------------
# A cross-check of names and syntax (tools/css/check-webref-package.py), pinned by version and by the integrity the registry
# publishes for the tarball (tools/css/WEBREF_PACKAGE). A tarball that does not hash to it is not kept.
pkgpin="$root/tools/css/WEBREF_PACKAGE"
pkg_version=$(awk -F'\t' '$1 == "version" { print $2 }' "$pkgpin")
pkg_url=$(awk -F'\t' '$1 == "tarball" { print $2 }' "$pkgpin")
pkg_integrity=$(awk -F'\t' '$1 == "integrity" { print $2 }' "$pkgpin")
pkgdir="$dest/webref-css"
if [ -f "$pkgdir/package/css.json" ] && [ "$(cat "$pkgdir/INTEGRITY" 2>/dev/null)" = "$pkg_integrity" ]; then
	printf 'have    @webref/css %s\n' "$pkg_version"
else
	printf 'fetch   @webref/css %s\n' "$pkg_version"
	rm -rf "$pkgdir.partial"
	mkdir -p "$pkgdir.partial"
	curl --fail --silent --show-error --location -o "$pkgdir.partial/package.tgz" "$pkg_url"
	got="sha512-$(openssl dgst -sha512 -binary "$pkgdir.partial/package.tgz" | base64 -w0)"
	if [ "$got" != "$pkg_integrity" ]; then
		rm -rf "$pkgdir.partial"
		printf '@webref/css %s: the tarball hashes to %s, pinned %s. Nothing was kept.\n' "$pkg_version" "$got" "$pkg_integrity" >&2
		exit 1
	fi
	tar -xzf "$pkgdir.partial/package.tgz" -C "$pkgdir.partial"
	printf '%s\n' "$pkg_integrity" > "$pkgdir.partial/INTEGRITY"
	rm -rf "$pkgdir"
	mv "$pkgdir.partial" "$pkgdir"
fi
