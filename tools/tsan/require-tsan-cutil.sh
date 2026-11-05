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
# require-tsan-cutil.sh EXECUTABLE LIBRARY_DIR
#
# Refuse a ThreadSanitizer run whose cutil is not built with ThreadSanitizer.
#
# cutil's atomics and mutexes are functions of cutil. ThreadSanitizer sees an atomic only in code it instrumented, so against a plain cutil it cannot see the release
# store that publishes a lazily compiled grammar, nor the last decrement of a reference count that hands an object from its last reader to the thread that frees it, and
# it reports both as data races. They are not races (the same program is silent with cutil built for it), but a gate that prints them teaches its reader to ignore
# its reports, so it is refused here instead of run.
#
# EXECUTABLE is a test program built for ThreadSanitizer; LIBRARY_DIR the directory its own shared library is loaded from. The cutil that the loader would use for the program
# (what ldd resolves, with LD_LIBRARY_PATH as the gate sets it) has to reference the ThreadSanitizer runtime's entry points. Exit 0: it does. Exit 1, with the invocation
# that builds one: it does not. Exit 2: the check could not be made (no ldd, no cutil among the program's libraries).
set -u
exe=${1:?usage: require-tsan-cutil.sh EXECUTABLE LIBRARY_DIR}
dir=${2:?usage: require-tsan-cutil.sh EXECUTABLE LIBRARY_DIR}
command -v ldd >/dev/null 2>&1 || { echo "require-tsan-cutil: ldd is not available; cannot tell which cutil the tests would load" >&2; exit 2; }
lib=$(LD_PRELOAD= LD_LIBRARY_PATH="$dir" ldd "$exe" 2>/dev/null | sed -n 's/^[[:space:]]*libghoti\.io-cutil[^[:space:]]* => \([^[:space:]]*\) .*/\1/p' | head -n 1)
if [ -z "$lib" ] || [ ! -f "$lib" ]; then
	echo "require-tsan-cutil: $exe does not load a cutil library (ldd found none)" >&2
	exit 2
fi
if grep -a -q '__tsan_func_entry' "$lib"; then
	exit 0
fi
cat >&2 <<EOF
ThreadSanitizer needs a cutil built with ThreadSanitizer; the one these tests load is not:
  $lib
Its atomics are functions of cutil, which ThreadSanitizer cannot see inside, so it would report the hand-off of a lazily compiled grammar and of the last reference to a
rule index as data races that are not. Build cutil that way into a prefix of its own, with the dependencies of this library beside it, and pass that prefix:

  make -C <cutil> install PREFIX=<tsan-prefix> EXTRA_CFLAGS=-fsanitize=thread EXTRA_LDFLAGS=-fsanitize=thread
  (the other dependencies, plain, into the same prefix, in the order of the suite's manifest)
  PKG_CONFIG_PATH=<tsan-prefix>/share/pkgconfig make test-tsan-quiet PREFIX=<tsan-prefix>

tools/soak/sanitizers-job.sh tsan does all of it for a job directory holding the sources.
EOF
exit 1
