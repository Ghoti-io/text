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
#   wpt/                web-platform-tests, sparse: resources/ and common/ (the harness the scoring interpreter's page record runs pages under; tools/css/WPT_HARNESS_TREES pins
#                       their trees at the same commit and the fetch checks them, offline too) and the css/ directory (56,344 files, 246 MB on disk when checked out; the
#                       css-syntax tables that tools/css/extract-wpt.py turns into vectors, the value tests of the math, color
#                       and function types that tools/css/extract-wpt-values.py reads, and every property parsing, computed
#                       value and serialization test that tools/css/extract-wpt-props.py reads as data, and the custom property pages of css-variables and
#                       css-properties-values-api that tools/css/extract-wpt-variables.py reads), pinned by commit in
#                       tools/css/WPT_COMMIT (the same commit as the HTML pin, tools/html/WPT_COMMIT, so that the library
#                       is scored against one revision of the suite). The selector queries' tables (dom/nodes: selectors.js and the pages that use them)
#                       are read by tools/css/extract-wpt-selectors.py.
#
#   webref/             The W3C's machine-readable extracts of the CSS specifications (w3c/webref, branch `curated`),
#                       sparse: ed/css (one file per specification series) and the three css-values definition files
#                       of ed/dfns, pinned by commit in tools/css/WEBREF_COMMIT and, file by file, by SHA-256 in
#                       tools/css/WEBREF_SHA256 (the manifest tools/css/gen-css-tables.py also checks). The tables of
#                       src/css/tables/ are generated from it.
#
#   drafts/             The Editors' Drafts of CSS modules that the served-text class of the scoring interpreter quotes (css-values-5, css-fonts-4,
#                       css-mixins-1, css-grid-3, css-writing-modes-4, css-forms-1, css-color-4, css-values-4, css-pseudo-4, css-lists-3, css-text-decor-4,
#                       css-text-4, css-tables-3, css-ui-4, css-multicol-2, css-flexbox-1, cssom-1, css-syntax-3, css-animations-1, css-animations-2, web-animations-1, css-easing-1, css-easing-2), as the Bikeshed sources (Overview.bs) of w3c/csswg-drafts at the commit
#                       in tools/css/DRAFTS_COMMIT, each pinned by SHA-256 in tools/css/DRAFTS_SHA256. A commit does not move; the rendered pages at drafts.csswg.org do.
#                       A file already fetched is checked offline.
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
# resources/ and common/ are checked against the trees WPT_HARNESS_TREES pins (the commit holds them, so this needs no network) and must be unmodified.
wpt_harness_ok() {
	[ -f "$1/resources/testharness.js" ] || return 1
	[ -z "$(git -C "$1" status --porcelain -- resources common 2>/dev/null)" ] || return 1
	while read -r dir tree; do
		case "$dir" in '#'*|'') continue ;; esac
		[ "$(git -C "$1" rev-parse "HEAD:$dir" 2>/dev/null)" = "$tree" ] || return 1
	done < "$root/tools/css/WPT_HARNESS_TREES"
}
if [ -d "$wpt/.git" ] && [ "$(git -C "$wpt" rev-parse HEAD 2>/dev/null)" = "$want_wpt" ]; then
	# (a checkout made before the property tests were scored has only some directories of css/: add what it lacks, at the same commit; one made before the page record has no harness)
	if [ ! -d "$wpt/css/support" ] || [ ! -d "$wpt/css/css-grid/parsing" ] || [ ! -d "$wpt/css/css-values" ] || [ ! -f "$wpt/dom/nodes/selectors.js" ] || [ ! -f "$wpt/resources/testharness.js" ]; then
		git -C "$wpt" sparse-checkout set css dom/nodes resources common
	fi
	wpt_harness_ok "$wpt" || { printf 'wpt: resources/ or common/ is not the pinned tree (tools/css/WPT_HARNESS_TREES)\n' >&2; exit 1; }
	printf 'have    wpt @ %s\n' "$want_wpt"
else
	printf 'fetch   wpt (css/, dom/nodes, resources/, common/) @ %s\n' "$want_wpt"
	rm -rf "$wpt.partial"
	mkdir -p "$wpt.partial"
	git -C "$wpt.partial" init --quiet
	git -C "$wpt.partial" remote add origin https://github.com/web-platform-tests/wpt.git
	git -C "$wpt.partial" sparse-checkout init --cone
	git -C "$wpt.partial" sparse-checkout set css dom/nodes resources common
	git -C "$wpt.partial" fetch --quiet --depth 1 --filter=blob:none origin "$want_wpt"
	git -C "$wpt.partial" checkout --quiet --detach FETCH_HEAD
	got=$(git -C "$wpt.partial" rev-parse HEAD)
	if [ "$got" != "$want_wpt" ]; then
		rm -rf "$wpt.partial"
		printf 'wpt: checked out %s, pinned %s\n' "$got" "$want_wpt" >&2
		exit 1
	fi
	if ! wpt_harness_ok "$wpt.partial"; then
		rm -rf "$wpt.partial"
		printf 'wpt: resources/ or common/ is not the pinned tree (tools/css/WPT_HARNESS_TREES). Nothing was kept.\n' >&2
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

# ---- the Editors' Drafts that the served-text class quotes ---------------------------------------------------------
# tools/css/DRAFTS_COMMIT is a commit of w3c/csswg-drafts and tools/css/DRAFTS_SHA256 lists, for each draft, the SHA-256 of its Bikeshed source (Overview.bs), the name it
# is kept under and its path in the repository. A commit does not change, so a stranger fetches the same bytes on any day (the rendered pages at drafts.csswg.org follow
# the newest commit and do not). Nothing is kept unless HEAD is the pinned commit and every file hashes as pinned.
drafts_manifest="$root/tools/css/DRAFTS_SHA256"
want_drafts=$(cat "$root/tools/css/DRAFTS_COMMIT")
drafts="$dest/drafts"
drafts_verify() { # dir: every manifest file present and hashing as pinned, and nothing else in it
	bad=0
	while read -r sum file path; do
		case "$sum" in '#'*|'') continue ;; esac
		if [ ! -f "$1/$file" ]; then
			bad=1
		elif [ "$(sha256sum "$1/$file" | cut -d' ' -f1)" != "$sum" ]; then
			printf 'drafts: %s does not hash as pinned (%s)\n' "$file" "$sum" >&2
			bad=1
		fi
	done < "$drafts_manifest"
	for f in "$1"/*; do
		[ -e "$f" ] || continue
		grep -q "^[0-9a-f]*[[:space:]]$(basename "$f")[[:space:]]" "$drafts_manifest" || { printf 'drafts: %s is not in tools/css/DRAFTS_SHA256\n' "$(basename "$f")" >&2; bad=1; }
	done
	return $bad
}
if [ -d "$drafts" ] && drafts_verify "$drafts"; then
	printf 'have    drafts @ %s (tools/css/DRAFTS_SHA256)\n' "$want_drafts"
else
	printf 'fetch   drafts (csswg-drafts, the sources of tools/css/DRAFTS_SHA256) @ %s\n' "$want_drafts"
	rm -rf "$drafts.src" "$drafts.partial"
	mkdir -p "$drafts.src" "$drafts.partial"
	git -C "$drafts.src" init --quiet
	git -C "$drafts.src" remote add origin https://github.com/w3c/csswg-drafts.git
	git -C "$drafts.src" sparse-checkout init --no-cone
	# shellcheck disable=SC2046
	git -C "$drafts.src" sparse-checkout set $(grep -v '^#' "$drafts_manifest" | awk 'NF { print "/" $3 }')
	git -C "$drafts.src" fetch --quiet --depth 1 --filter=blob:none origin "$want_drafts"
	git -C "$drafts.src" checkout --quiet --detach FETCH_HEAD
	got=$(git -C "$drafts.src" rev-parse HEAD)
	if [ "$got" != "$want_drafts" ]; then
		rm -rf "$drafts.src" "$drafts.partial"
		printf 'drafts: checked out %s, pinned %s. Nothing was kept.\n' "$got" "$want_drafts" >&2
		exit 1
	fi
	while read -r sum file path; do
		case "$sum" in '#'*|'') continue ;; esac
		cp "$drafts.src/$path" "$drafts.partial/$file"
	done < "$drafts_manifest"
	rm -rf "$drafts.src"
	if ! drafts_verify "$drafts.partial"; then
		rm -rf "$drafts.partial"
		printf 'drafts: a source does not hash as pinned. Nothing was kept. Raising the pin is a deliberate edit of tools/css/DRAFTS_COMMIT and DRAFTS_SHA256, in one commit with the quoted sentences of tools/conformance/CSS_CSSOM_SCRIPT_SERVED re-read against the new text.\n' >&2
		exit 1
	fi
	rm -rf "$drafts"
	mv "$drafts.partial" "$drafts"
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

# ---- w3c/webref, ed/idl: the WebIDL of the CSSOM family ---------------------------------------------------------------
# The interface ledger (tools/css/cssom_ledger.tsv) is checked against these files of the same pinned commit as the CSS data:
# tools/css/WEBREF_IDL_SHA256 lists them with their SHA-256, and a checkout with a missing, changed or extra file is not kept.
idl_manifest="$root/tools/css/WEBREF_IDL_SHA256"
idl="$dest/webref-idl"
idl_verify() {
	bad=0
	while read -r sum file; do
		case "$sum" in '#'*|'') continue ;; esac
		if [ ! -f "$1/$file" ]; then
			printf 'webref-idl: %s is missing\n' "$file" >&2
			bad=1
		elif [ "$(sha256sum "$1/$file" | cut -d' ' -f1)" != "$sum" ]; then
			printf 'webref-idl: %s does not hash as pinned\n' "$file" >&2
			bad=1
		fi
	done < "$idl_manifest"
	[ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" = "$want_webref" ] || { printf 'webref-idl: not at %s\n' "$want_webref" >&2; bad=1; }
	return $bad
}
if [ -d "$idl/.git" ] && idl_verify "$idl"; then
	printf 'have    webref-idl @ %s\n' "$want_webref"
else
	printf 'fetch   webref-idl (ed/idl, the files of WEBREF_IDL_SHA256) @ %s\n' "$want_webref"
	rm -rf "$idl.partial"
	mkdir -p "$idl.partial"
	git -C "$idl.partial" init --quiet
	git -C "$idl.partial" remote add origin https://github.com/w3c/webref.git
	git -C "$idl.partial" sparse-checkout init --no-cone
	# shellcheck disable=SC2046
	git -C "$idl.partial" sparse-checkout set $(grep -v '^#' "$idl_manifest" | sed 's/^[0-9a-f]*  /\//')
	git -C "$idl.partial" fetch --quiet --depth 1 --filter=blob:none origin "$want_webref"
	git -C "$idl.partial" checkout --quiet --detach FETCH_HEAD
	if ! idl_verify "$idl.partial"; then
		rm -rf "$idl.partial"
		printf 'webref-idl: nothing was kept. Raising the pin is a deliberate edit of tools/css/WEBREF_COMMIT and WEBREF_IDL_SHA256.\n' >&2
		exit 1
	fi
	rm -rf "$idl"
	mv "$idl.partial" "$idl"
fi
