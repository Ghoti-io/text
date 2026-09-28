#!/bin/sh
#
# Fetch the published JSON Schema meta-schemas, for every dialect this
# library reads that has them.
#
# Nineteen documents: 2020-12's nine, 2019-09's seven, and one each for
# draft-07, draft-06 and draft-04. They are what a `$ref` to
# https://json-schema.org/draft/<date>/schema and the vocabulary meta-schemas
# beneath it resolve to, and what a `$ref` to
# http://json-schema.org/draft-0N/schema resolves to. The library embeds them,
# so that validating a schema against its own dialect needs neither a resolver
# nor a socket; tools/metaschema/gen_metaschema.py turns what this script
# fetches into the committed C file, and `make check-metaschema` fails if the
# two disagree.
#
# The three older drafts publish a single document each rather than a root plus a
# vocabulary set: `$vocabulary` does not exist before 2019-09, so the keyword
# set *is* the draft and there is nothing to split it into. They are also
# spelled differently in two ways that matter here. The path is
# `/draft-07/schema` rather than `/draft/2020-12/schema`, and the `$id` inside
# carries an empty fragment - `http://json-schema.org/draft-07/schema#` - which
# is the URI a document in the wild refers to them by. What is embedded is
# keyed without the fragment, because that is what a reference resolves to
# before its fragment is read. draft-04's is spelled `id` rather than `$id`,
# which is the rename draft-06 made and one of the three reasons draft-04 needs
# its own reading.
#
# This is the opposite of what tools/idna/fetch.sh does with the UCD, and the
# difference is the point. The UCD versions: 17.0.0 supersedes 16.0.0, and a
# committed copy would be a stale copy of somebody else's data. These URIs do
# not version. The whole reference model of 2020-12 rests on each of them
# naming one fixed document forever, so there is no drift to be behind - and
# the alternative to embedding them is a validator that opens a connection in
# the middle of a compile, to a URI it read out of the document it was handed.
#
# format-assertion is fetched although the 2020-12 root meta-schema does not
# reference it: it is the meta-schema of the one optional vocabulary in that
# dialect, and a schema that declares that vocabulary refers to it. 2019-09
# has no such split - one `format` vocabulary, whose assertion behaviour the
# implementation chooses - so its set is seven rather than nine, and there is
# no `unevaluated` because 2019-09 keeps those two keywords in `applicator`.
#
# Everything lands in third_party/json-schema/<date>/, which .gitignore
# excludes.
#
# Usage:  tools/metaschema/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)

# json-schema.org negotiates on Accept, and a request that does not ask for
# JSON is answered with the human-readable page about the document rather than
# the document. Asking explicitly is not politeness, it is the difference
# between fetching a schema and fetching HTML that describes one.
fetch() {
	url="$1"
	out="$2"
	printf '  %s\n' "$url"
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL -H 'Accept: application/schema+json' -o "$out" "$url"
	elif command -v wget >/dev/null 2>&1; then
		wget -q --header='Accept: application/schema+json' -O "$out" "$url"
	else
		echo "need curl or wget" >&2
		exit 1
	fi
	# A negotiated HTML page is still a 200, so the transfer succeeding says
	# nothing. Every one of these documents is a JSON object.
	case $(head -c 1 "$out") in
	'{') ;;
	*)
		echo "$url did not return a JSON object" >&2
		exit 1
		;;
	esac
}

fetch_dialect() {
	draft="$1"
	shift
	dest="$root/third_party/json-schema/$draft"
	base="https://json-schema.org/draft/$draft"
	mkdir -p "$dest/meta"
	echo "fetching the $draft meta-schemas into $dest"
	fetch "$base/schema" "$dest/schema.json"
	for name in "$@"; do
		fetch "$base/meta/$name" "$dest/meta/$name.json"
	done
}

# The older drafts' single document. `$id` carries `#` and the published path
# has no `/draft/` segment, so neither fetch_dialect's URI shape nor its
# vocabulary loop applies.
fetch_old_dialect() {
	draft="$1"
	dest="$root/third_party/json-schema/$draft"
	mkdir -p "$dest"
	echo "fetching the $draft meta-schema into $dest"
	fetch "https://json-schema.org/$draft/schema" "$dest/schema.json"
}

fetch_dialect 2020-12 core applicator unevaluated validation meta-data \
	format-annotation format-assertion content
fetch_dialect 2019-09 core applicator validation meta-data format content
fetch_old_dialect draft-07
fetch_old_dialect draft-06
fetch_old_dialect draft-04

echo "done; regenerate with tools/metaschema/gen_metaschema.py"
