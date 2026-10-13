#!/bin/sh
#
# Fetch the third-party inputs the Encoding Standard work reads.
#
#   encodings.json   The label table: every encoding's name and labels, as the
#                    standard's section "Names and labels" lists them. The
#                    generator tools/encoding/gen_tables.py builds the label
#                    lookup from it.
#   indexes.json     The standard's indexes (the 27 single-byte ones, and the
#                    multi-byte ones that the next stage will read). The
#                    generator builds the single-byte decode tables and their
#                    reverse (encoder) tables from it.
#   wpt/             web-platform-tests, sparse: only encoding/. The decoder and
#                    label vectors (textdecoder-*.any.js and the rest) that
#                    tools/oracle/encoding_wpt.py runs against this library.
#
# Neither JSON file is committed: both are reproducible from a pin and a URL,
# and committing them would make this repository the second-best copy of
# somebody else's data. The *generated* tables are committed instead, so a build
# needs neither the network nor Python.
#
# **Both files are fetched from an immutable address and pinned by content.**
# The standard's own site serves the living standard, which anyone's edit changes
# under every fresh clone; they are fetched instead from the whatwg/encoding
# repository at the commit in tools/encoding/ENCODING_COMMIT (the commit snapshot
# the site names, tools/encoding/STANDARD), and each is verified against its pin
# file (ENCODINGS_SHA256 and INDEXES_SHA256), the same bytes the site served. A
# failed download or a mismatch is a failure that leaves no file behind (the
# partial file is removed on any exit, and so is a stale file that does not match
# its pin). wpt is
# pinned by commit, tools/encoding/WPT_COMMIT, and the checkout is verified to be
# exactly that commit. Raising a pin is a deliberate edit to that file, in a commit
# that says what the new data changed. tools/encoding/STANDARD records which
# revision of the standard the pins were taken from.
#
# Everything lands in third_party/encoding/, which .gitignore excludes.
#
# Usage:  tools/encoding/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/encoding"
want_wpt=$(cat "$root/tools/encoding/WPT_COMMIT")
want_commit=$(cat "$root/tools/encoding/ENCODING_COMMIT")

mkdir -p "$dest"

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

# fetch_pinned <file> <pin file>
partial=
trap '[ -z "$partial" ] || rm -f "$partial"' EXIT HUP INT TERM
fetch_pinned() {
	name=$1
	want_sha=$(cat "$root/tools/encoding/$2")
	if [ -s "$dest/$name" ] && [ "$(sha256_of "$dest/$name")" = "$want_sha" ]; then
		printf 'have    %s\n' "$name"
		return 0
	fi
	# Whatever is there does not match its pin: it is not left to be read.
	rm -f "$dest/$name"
	printf 'fetch   %s @ %s\n' "$name" "$want_commit"
	partial="$dest/$name.partial"
	# --fail so that an HTML error page never lands on disk looking like data.
	if ! curl --fail --silent --show-error --location --output "$partial" \
		"https://raw.githubusercontent.com/whatwg/encoding/$want_commit/$name"; then
		rm -f "$partial"
		partial=
		printf '%s: the download failed\n' "$name" >&2
		exit 1
	fi
	got=$(sha256_of "$partial")
	if [ "$got" != "$want_sha" ]; then
		rm -f "$partial"
		partial=
		printf '%s: sha256 %s, pinned %s\n' "$name" "$got" "$want_sha" >&2
		printf 'The pinned commit does not hold the pinned bytes. Read the difference, then update\n' >&2
		printf 'tools/encoding/ENCODING_COMMIT, %s and src/encoding/tables/.\n' "$2" >&2
		exit 1
	fi
	mv "$partial" "$dest/$name"
	partial=
}

fetch_pinned encodings.json ENCODINGS_SHA256
fetch_pinned indexes.json INDEXES_SHA256

# ---- web-platform-tests, encoding/ only ---------------------------------
# A blobless, shallow, sparse checkout: the repository is enormous and the part
# read here is about twenty megabytes (the legacy multi-byte vectors are most of it).
wpt="$dest/wpt"
if [ -d "$wpt/.git" ] && [ "$(git -C "$wpt" rev-parse HEAD 2>/dev/null)" = "$want_wpt" ]; then
	printf 'have    wpt @ %s\n' "$want_wpt"
else
	printf 'fetch   wpt (encoding) @ %s\n' "$want_wpt"
	rm -rf "$wpt.partial"
	mkdir -p "$wpt.partial"
	git -C "$wpt.partial" init --quiet
	git -C "$wpt.partial" remote add origin https://github.com/web-platform-tests/wpt.git
	git -C "$wpt.partial" sparse-checkout init --cone
	git -C "$wpt.partial" sparse-checkout set encoding
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
