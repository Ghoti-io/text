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
# Verify a checkout of the pinned w3c/webref data.
#
# Usage:  tools/css/webref-verify.sh CHECKOUT MANIFEST COMMIT
#
# Succeeds when the checkout's HEAD is COMMIT (skipped when COMMIT is "-", for a copy that has no .git), every
# file the manifest lists hashes as pinned, and the checkout holds no file under ed/css or ed/dfns that the manifest
# does not list. Otherwise prints each problem, naming the file, to standard error and fails.

set -u

dir=$1
manifest=$2
want=$3

bad=0
if [ "$want" != "-" ]; then
	got=$(git -C "$dir" rev-parse HEAD 2>/dev/null)
	if [ "$got" != "$want" ]; then
		printf 'webref: checked out %s, pinned %s\n' "${got:-nothing}" "$want" >&2
		bad=1
	fi
fi

sha() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

while read -r sum file; do
	case "$sum" in '#'*|'') continue ;; esac
	if [ ! -f "$dir/$file" ]; then
		printf 'webref: %s is missing\n' "$file" >&2
		bad=1
		continue
	fi
	have=$(sha "$dir/$file")
	if [ "$have" != "$sum" ]; then
		printf 'webref: %s hashes %s, pinned %s\n' "$file" "$have" "$sum" >&2
		bad=1
	fi
done < "$manifest"

listed=$(grep -v '^#' "$manifest" | sed 's/^[0-9a-f]*  //' | LC_ALL=C sort)
held=$(cd "$dir" && { find ed/css ed/dfns -type f; } | LC_ALL=C sort)
if [ "$listed" != "$held" ]; then
	printf '%s\n%s\n' "$listed" "$held" | LC_ALL=C sort | uniq -u | while read -r f; do
		printf 'webref: %s is not in the manifest\n' "$f" >&2
	done
	bad=1
fi
exit $bad
