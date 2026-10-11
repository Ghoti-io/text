#!/bin/sh
#
# Fetch the W3C XML Conformance Test Suite.
#
#   xmlconf/   xmlts20130923.tar.gz, the suite the XML parser is scored
#              against: about 2,000 documents from James Clark, Sun, Fuji
#              Xerox, OASIS/NIST, IBM and the University of Edinburgh, with a
#              catalogue (xmlconf.xml) that says what each one must do.
#
# Not committed: it is reproducible from a pin and a URL, and committing it
# would make this repository the second-best copy of somebody else's data.
#
# **Pinned by content, not by name.** The URL names a dated release, which is
# stable in practice, but a URL pins nothing; the archive is verified against
# tools/xml/XMLCONF_SHA256 before anything is extracted, and a mismatch is a
# failure that leaves nothing behind. Raising the pin is a deliberate edit to
# that file, in a commit that says what the new data changed.
#
# Lands in third_party/xml/, which .gitignore excludes.
#
# Usage:  tools/xml/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/xml"
want_sha=$(cat "$root/tools/xml/XMLCONF_SHA256")
url="https://www.w3.org/XML/Test/xmlts20130923.tar.gz"

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

mkdir -p "$dest"

# The archive is kept beside what it unpacked to, so that "have it" can be
# checked against the pin and not against the presence of a directory.
archive="$dest/xmlts20130923.tar.gz"
if [ -d "$dest/xmlconf" ] && [ -s "$archive" ] \
	&& [ "$(sha256_of "$archive")" = "$want_sha" ]; then
	printf 'have    xmlconf (%s)\n' "$want_sha"
	exit 0
fi

printf 'fetch   xmlconf\n'
rm -rf "$dest/xmlconf.partial" "$archive.partial"
# --fail so that an HTML error page never lands on disk looking like data.
curl --fail --silent --show-error --location --output "$archive.partial" "$url"
got=$(sha256_of "$archive.partial")
if [ "$got" != "$want_sha" ]; then
	rm -f "$archive.partial"
	printf 'xmlts20130923.tar.gz: sha256 %s, pinned %s\n' "$got" "$want_sha" >&2
	printf 'The upstream file changed. Read the difference, then update\n' >&2
	printf 'tools/xml/XMLCONF_SHA256 and the exceptions in tools/conformance/XMLCONF_EXCEPTIONS.\n' >&2
	exit 1
fi
mkdir -p "$dest/xmlconf.partial"
tar -xzf "$archive.partial" -C "$dest/xmlconf.partial"
if [ ! -f "$dest/xmlconf.partial/xmlconf/xmlconf.xml" ]; then
	rm -rf "$dest/xmlconf.partial" "$archive.partial"
	printf 'the archive does not contain xmlconf/xmlconf.xml\n' >&2
	exit 1
fi
rm -rf "$dest/xmlconf"
mv "$dest/xmlconf.partial/xmlconf" "$dest/xmlconf"
rmdir "$dest/xmlconf.partial"
mv "$archive.partial" "$archive"
