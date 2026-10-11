#!/bin/sh
#
# Fetch the two third-party inputs the HTML work reads.
#
#   entities.json     The WHATWG named character reference table, which the
#                     HTML tokenizer's named-reference states and the
#                     generator in tools/html/gen_charrefs.py are built from.
#   html5lib-tests/   The tokenizer, encoding and serializer vectors every
#                     HTML5 parser is scored against.
#   wpt/              web-platform-tests, sparse: only html/syntax/parsing.
#                     The *tree-construction* vectors live here and no longer
#                     in html5lib-tests, whose own README says they "are now
#                     solely maintained on web-platform-tests" (commit
#                     224991e, "Tree construction tests have moved to WPT").
#                     A pin on html5lib-tests alone would score the tokenizer
#                     and nothing that builds a tree.
#
# Neither is committed: both are reproducible from a pin and a URL, and
# committing them would make this repository the second-best copy of somebody
# else's data. The *generated* table is committed instead, so a build needs
# neither the network nor Python.
#
# **Both are pinned by content, not by name.** entities.json is a living
# standard's file at a stable URL, so a URL alone pins nothing: it is verified
# against tools/html/ENTITIES_SHA256 and a mismatch is a failure that leaves no
# file behind. html5lib-tests and wpt are pinned by commit, tools/html/
# HTML5LIB_TESTS_COMMIT and tools/html/WPT_COMMIT, and each checkout is
# verified to be exactly that commit.
# Raising either pin is a deliberate edit to that file, in a commit that says
# what the new data changed.
#
# Everything lands in third_party/html/, which .gitignore excludes.
#
# Usage:  tools/html/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/html"
want_sha=$(cat "$root/tools/html/ENTITIES_SHA256")
want_commit=$(cat "$root/tools/html/HTML5LIB_TESTS_COMMIT")
want_wpt=$(cat "$root/tools/html/WPT_COMMIT")

mkdir -p "$dest"

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

# ---- entities.json ------------------------------------------------------
if [ -s "$dest/entities.json" ] && [ "$(sha256_of "$dest/entities.json")" = "$want_sha" ]; then
	printf 'have    entities.json\n'
else
	printf 'fetch   entities.json\n'
	# --fail so that an HTML error page never lands on disk looking like data.
	curl --fail --silent --show-error --location \
		--output "$dest/entities.json.partial" \
		"https://html.spec.whatwg.org/entities.json"
	got=$(sha256_of "$dest/entities.json.partial")
	if [ "$got" != "$want_sha" ]; then
		rm -f "$dest/entities.json.partial"
		printf 'entities.json: sha256 %s, pinned %s\n' "$got" "$want_sha" >&2
		printf 'The upstream file changed. Read the difference, then update\n' >&2
		printf 'tools/html/ENTITIES_SHA256 and regenerate src/html/charref_table.c.\n' >&2
		exit 1
	fi
	mv "$dest/entities.json.partial" "$dest/entities.json"
fi

# ---- html5lib-tests -----------------------------------------------------
repo="$dest/html5lib-tests"
if [ -d "$repo/.git" ] && [ "$(git -C "$repo" rev-parse HEAD 2>/dev/null)" = "$want_commit" ]; then
	printf 'have    html5lib-tests @ %s\n' "$want_commit"
else
	printf 'fetch   html5lib-tests @ %s\n' "$want_commit"
	rm -rf "$repo.partial"
	mkdir -p "$repo.partial"
	git -C "$repo.partial" init --quiet
	git -C "$repo.partial" remote add origin https://github.com/html5lib/html5lib-tests.git
	# A commit by its hash, shallow: the history is not needed and the suite
	# has a great deal of it.
	git -C "$repo.partial" fetch --quiet --depth 1 origin "$want_commit"
	git -C "$repo.partial" checkout --quiet --detach FETCH_HEAD
	got=$(git -C "$repo.partial" rev-parse HEAD)
	if [ "$got" != "$want_commit" ]; then
		rm -rf "$repo.partial"
		printf 'html5lib-tests: checked out %s, pinned %s\n' "$got" "$want_commit" >&2
		exit 1
	fi
	rm -rf "$repo"
	mv "$repo.partial" "$repo"
fi

# ---- web-platform-tests, html/syntax/parsing only -----------------------
# A blobless, shallow, sparse checkout: the repository is enormous and the part
# read here is about a megabyte.
wpt="$dest/wpt"
if [ -d "$wpt/.git" ] && [ "$(git -C "$wpt" rev-parse HEAD 2>/dev/null)" = "$want_wpt" ]; then
	printf 'have    wpt @ %s\n' "$want_wpt"
else
	printf 'fetch   wpt (html/syntax/parsing) @ %s\n' "$want_wpt"
	rm -rf "$wpt.partial"
	mkdir -p "$wpt.partial"
	git -C "$wpt.partial" init --quiet
	git -C "$wpt.partial" remote add origin https://github.com/web-platform-tests/wpt.git
	git -C "$wpt.partial" sparse-checkout init --cone
	git -C "$wpt.partial" sparse-checkout set html/syntax/parsing
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
