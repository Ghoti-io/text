#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# After tools/*/fetch.sh has run in a tree: every pin of the three fetch scripts of the XML, HTML and
# Encoding work matches what is on disk, checked here and not by the scripts that fetched it, and every
# pin file in tools/ is read by some fetch script (a pin nothing reads pins nothing).
#
#   tools/consumer/check-pins.sh [TREE]
set -u
root=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
bad=0; n=0
sha() { sha256sum "$1" | cut -d' ' -f1; }
check() { # what got want
  n=$((n + 1))
  if [ "$2" = "$3" ] && [ -n "$3" ]; then printf '  ok   %s\n' "$1"; else printf '  FAIL %s: have %s, pinned %s\n' "$1" "$2" "$3"; bad=$((bad + 1)); fi
}
t="$root/third_party"
check xml/xmlts20130923.tar.gz "$(sha "$t/xml/xmlts20130923.tar.gz" 2>/dev/null)" "$(cat "$root/tools/xml/XMLCONF_SHA256")"
[ -f "$t/xml/xmlconf/xmlconf.xml" ] && check "xml/xmlconf/xmlconf.xml (extracted)" yes yes || check "xml/xmlconf/xmlconf.xml (extracted)" missing yes
check html/entities.json "$(sha "$t/html/entities.json" 2>/dev/null)" "$(cat "$root/tools/html/ENTITIES_SHA256")"
check html/html5lib-tests "$(git -C "$t/html/html5lib-tests" rev-parse HEAD 2>/dev/null)" "$(cat "$root/tools/html/HTML5LIB_TESTS_COMMIT")"
check html/wpt "$(git -C "$t/html/wpt" rev-parse HEAD 2>/dev/null)" "$(cat "$root/tools/html/WPT_COMMIT")"
check encoding/encodings.json "$(sha "$t/encoding/encodings.json" 2>/dev/null)" "$(cat "$root/tools/encoding/ENCODINGS_SHA256")"
check encoding/indexes.json "$(sha "$t/encoding/indexes.json" 2>/dev/null)" "$(cat "$root/tools/encoding/INDEXES_SHA256")"
check encoding/encoding.bs "$(sha "$t/encoding/encoding.bs" 2>/dev/null)" "$(cat "$root/tools/encoding/ENCODING_BS_SHA256")"
check encoding/wpt "$(git -C "$t/encoding/wpt" rev-parse HEAD 2>/dev/null)" "$(cat "$root/tools/encoding/WPT_COMMIT")"
# a pin nothing reads
for dir in xml html encoding; do
  for f in "$root"/tools/$dir/*SHA256 "$root"/tools/$dir/*COMMIT; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    n=$((n + 1))
    if grep -q "$b" "$root/tools/$dir/fetch.sh"; then printf '  ok   tools/%s/%s is read by fetch.sh\n' "$dir" "$b"
    elif grep -q "$b" "$root/tools/$dir/fetch.sh" "$root"/tools/*/fetch.sh 2>/dev/null; then printf '  ok   tools/%s/%s is read by another fetch.sh\n' "$dir" "$b"
    else printf '  FAIL tools/%s/%s is read by no fetch script\n' "$dir" "$b"; bad=$((bad + 1)); fi
  done
done
printf 'check-pins: %s checks, %s failed\n' "$n" "$bad"
[ "$bad" -eq 0 ]
