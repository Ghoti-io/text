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
# Sourced by an oracle gate script (not run): run_child CMD... runs CMD as a child and waits for it, and when the script is sent SIGTERM, SIGHUP or SIGINT it passes SIGTERM on to
# the child, waits for it (the child removes the containers it started: tools/oracle/oracle_env.py) and exits 128 + the signal. A shell that waits on a foreground command does not
# run its traps until the command ends, which for a gate killed part-way is never: the child went on running with its containers. The exit status of run_child is the child's.
run_child_pid=
run_child_stop() {
	if [ -n "$run_child_pid" ]; then
		kill -TERM "$run_child_pid" 2>/dev/null || true
		wait "$run_child_pid" 2>/dev/null || true
	fi
	exit "$1"
}
trap 'run_child_stop 143' TERM
trap 'run_child_stop 129' HUP
trap 'run_child_stop 130' INT
run_child() {
	"$@" &
	run_child_pid=$!
	run_child_rc=0
	wait "$run_child_pid" || run_child_rc=$?
	run_child_pid=
	return "$run_child_rc"
}
