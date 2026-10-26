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
# Fail if a generated table of the CSS registry would need a relocation when the library is loaded.
#
#   tools/css/check-table-relocations.sh OBJDIR     check the objects of src/css/tables compiled into OBJDIR; then show the gate fails
#
# The tables of src/css/tables/ are const data that holds no pointer (a string is an offset and a length into one pool, a reference
# to a row is its index), so that they sit in a read-only section that every process maps from the same pages, and the loader
# has nothing to patch at load time (several hundred thousand relocations otherwise). The check reads each object: no `.data`,
# `.data.rel*` or `.bss` section that holds anything, and no relocation section other than the debugging information's. It is then
# made to fail on an object compiled from a table that does hold pointers.
set -e

objdir=${1:?usage: check-table-relocations.sh OBJDIR}

check() {
	# $1: an object; prints a problem and returns 1
	obj=$1
	bad=0
	for sec in $(readelf -SW "$obj" | sed -n 's/^ *\[ *[0-9]*\] \(\.[^ ]*\) .*/\1/p'); do
		case "$sec" in
		.data|.data.rel*|.bss*|.tbss|.tdata)
			size=$(readelf -SW "$obj" | awk -v s="$sec" '$2 == s { print $6 }' | head -1)
			if [ "${size:-0}" != "00000000" ] && [ -n "$size" ] && [ "$size" != "0" ]; then
				echo "$obj: section $sec holds data; a table must be const and hold no pointer" >&2
				bad=1
			fi
			;;
		.rela.rodata*|.rela.data*|.rel.rodata*|.rel.data*)
			echo "$obj: $sec: the table needs relocations" >&2
			bad=1
			;;
		esac
	done
	return $bad
}

status=0
count=0
for obj in "$objdir"/css_tables_*.o; do
	[ -f "$obj" ] || continue
	count=$((count + 1))
	check "$obj" || status=1
done
if [ "$count" -lt 5 ]; then
	echo "check-table-relocations: only $count table objects in $objdir (expected 5): build the library first" >&2
	exit 1
fi
if [ "$status" -ne 0 ]; then
	exit 1
fi

# the gate made to fail: a const table of pointers, and a writable table
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
printf 'const char *const gtext_planted[] = {"a", "bb", "ccc"};\n' > "$scratch/ptr.c"
printf 'static const char s[] = "x"; const char *p = s; int gtext_writable[4] = {1,2,3,4};\n' > "$scratch/rw.c"
printf 'static const unsigned gtext_ok[4] = {1,2,3,4}; unsigned gtext_get(int i) { return gtext_ok[i & 3]; }\n' > "$scratch/ok.c"
for f in ptr rw ok; do
	cc -std=c17 -O1 -fPIC -c "$scratch/$f.c" -o "$scratch/$f.o"
done
if check "$scratch/ptr.o" 2>/dev/null; then
	echo "check-table-relocations: the gate accepted a table of pointers" >&2
	exit 1
fi
if check "$scratch/rw.o" 2>/dev/null; then
	echo "check-table-relocations: the gate accepted a writable table" >&2
	exit 1
fi
if ! check "$scratch/ok.o" 2>/dev/null; then
	echo "check-table-relocations: the gate refused a const table of integers" >&2
	exit 1
fi
echo "check-table-relocations: $count table objects hold no writable data and need no relocation (a table of pointers and a writable one are refused)"
