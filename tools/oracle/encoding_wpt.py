#!/usr/bin/env python3
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
"""Run the Encoding Standard's own test suite (web-platform-tests, encoding/) against this library.

    python3 tools/oracle/oracle_run.py encoding -- python3 tools/oracle/encoding_wpt.py <encoding-ours> [--plant NAME] [--update]

The suite is a pinned checkout (tools/encoding/WPT_COMMIT, fetched by tools/encoding/fetch.sh) of the standard's `encoding/` directory. Its tests
are written for a browser and for the Encoding API, so they are run by containers/html/wpt.mjs, a small testharness and an Encoding API in Node
whose decoding and encoding is done by **this library**, through a pair of named pipes to tools/oracle/encoding_ours.c. The JavaScript decides nothing
but the API's argument handling (which the tests are about and the library has no counterpart for); so what is scored is the standard's own vectors
against the library's own code, which is independent evidence in a way that tables generated from the standard's indexes cannot be: for the
single-byte encodings the suite's expectations *are* the indexes, but for UTF-8, UTF-16, the labels, streaming, the fatal flag and the byte order mark
handling they are written by people who found the bugs.

**What runs.** The files of RUN below, some of which have a part for the multi-byte encodings (the next stage) whose
tests are **deferred**, not failed, when the library says it has no codec for the encoding (E_UNSUPPORTED). **What does not**, and why, is NOT_RUN and NOT_RUN_DIRECTORIES: the
HTML pages (a browser is the runner), the streams (the Streams API), `encodeInto` (an API of its own), the legacy multi-byte vectors (the next stage),
and the files about the JavaScript bindings. A file or directory that is in neither list fails the run.

The count of passed, deferred and failed tests of each file is in tools/oracle/encoding_wpt_expected.txt, and the run fails on a failure, on a count
that has changed (in either direction: a test that passes now and did not is a change to read, not a gain to keep) and on a file that ran nothing.
`--update` prints the list the run found. When the multi-byte stage lands every deferred count goes to zero and the file is regenerated.

--plant NAME corrupts the run, to show the gate fails (run-encoding-oracle.sh --controls): bridge-drops-a-byte (the bridge loses the last byte of
every input of more than three), expected-count (the list says one test more), no-such-file (a file that is not in the suite).

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import encoding_ref as R  # noqa: E402
import oracle_env  # noqa: E402

ROOT = oracle_env.ROOT
SUITE = os.path.join(ROOT, "third_party", "encoding", "wpt", "encoding")
RUNNER = os.path.join(HERE, "containers", "html", "wpt.mjs")
EXPECTED = os.path.join(HERE, "encoding_wpt_expected.txt")

RUN = [
    "api-basics.any.js",
    "api-invalid-label.any.js",
    "api-replacement-encodings.any.js",
    "api-surrogates-utf8.any.js",
    "iso-2022-jp-decoder.any.js",
    "replacement-encodings.any.js",
    "single-byte-decoder.any.js",
    "textdecoder-byte-order-marks.any.js",
    "textdecoder-copy.any.js",
    "textdecoder-eof.any.js",
    "textdecoder-fatal-single-byte.any.js",
    "textdecoder-fatal-streaming.any.js",
    "textdecoder-fatal.any.js",
    "textdecoder-ignorebom.any.js",
    "textdecoder-labels.any.js",
    "textdecoder-mistakes.any.js",
    "textdecoder-streaming.any.js",
    "textdecoder-utf16-surrogates.any.js",
    "textencoder-constructor-non-utf.any.js",
    "textencoder-utf16-surrogates.any.js",
    "unsupported-encodings.any.js",
]

# Every JavaScript test file of the suite's directory is in RUN or here, by name, and every directory is in NOT_RUN_DIRECTORIES: a bumped WPT_COMMIT that
# adds a file or a directory makes the run fail until somebody has said what to do with it. (The HTML pages, `*.html`, are for a browser and are all skipped.)
NOT_RUN = OrderedDict([
    ("encodeInto.any.js", "TextEncoder.encodeInto: an API of its own, with a destination of fixed size"),
    ("idlharness.any.js", "the Web IDL interface checks"),
    ("textdecoder-arguments.any.js", "JavaScript argument conversion (a detached buffer in an options getter)"),
    ("unsupported-labels.window.js", "a window: iframes and `fetch`"),
    ("single-byte-decoder-iframe.window.js", "a window: the iframe of single-byte-decoder.any.js"),
])
NOT_RUN_DIRECTORIES = OrderedDict([
    ("streams", "TextDecoderStream and TextEncoderStream: the Streams API"),
    ("resources", "helpers of the tests, not tests"),
    ("legacy-mb-japanese", "the multi-byte vectors: the next stage"),
    ("legacy-mb-korean", "the multi-byte vectors: the next stage"),
    ("legacy-mb-schinese", "the multi-byte vectors: the next stage"),
    ("legacy-mb-tchinese", "the multi-byte vectors: the next stage"),
])


def unaccounted():
    """The files and directories of the suite that are neither run nor listed as skipped."""
    out = []
    for name in sorted(os.listdir(SUITE)):
        path = os.path.join(SUITE, name)
        if os.path.isdir(path):
            if name not in NOT_RUN_DIRECTORIES:
                out.append(name + "/")
        elif name.endswith(".js") and name not in RUN and name not in NOT_RUN:
            out.append(name)
    return out


def read_expected(path):
    out = OrderedDict()
    with open(path, encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4 or not all(p.isdigit() for p in parts[1:]):
                raise SystemExit("%s:%d: not four tab-separated fields (file, passed, deferred, failed): %r" % (path, number, line[:80]))
            out[parts[0]] = tuple(int(p) for p in parts[1:])
    return out


def run_suite(binary, files, plant):
    tmp = tempfile.mkdtemp(prefix="encoding-wpt-")
    os.chmod(tmp, 0o777)
    req = os.path.join(tmp, "req")
    resp = os.path.join(tmp, "resp")
    os.mkfifo(req)
    os.mkfifo(resp)
    os.chmod(req, 0o666)
    os.chmod(resp, 0o666)
    try:
        errfile = os.path.join(tmp, "ours.err")  # a file, not a pipe: nobody reads a pipe while the suite runs, and a full one would block the library's runner
        ours = subprocess.Popen([binary, "serve", req, resp], stderr=open(errfile, "wb"))
        args = [tmp, SUITE] + (["--plant", plant] if plant in ("bridge-drops-a-byte",) else []) + files
        command = oracle_env.command("encoding", ["node", RUNNER] + args, scratch=[tmp])
        done = subprocess.run(command, capture_output=True)
        try:
            ours.wait(timeout=20)
        except subprocess.TimeoutExpired:
            ours.kill()
            ours.wait()
        with open(errfile, "rb") as fh:
            err = fh.read().decode("utf-8", "replace")
        if done.returncode != 0:
            raise SystemExit("the runner exited %d:\n%s\n%s" % (
                done.returncode, oracle_env.reference_stderr(done.stderr.decode("utf-8", "replace"))[-2500:], err[-500:]))
        results = [json.loads(l) for l in done.stdout.decode("utf-8").split("\n") if l.startswith("{")]
        return results
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary")
    ap.add_argument("--plant", choices=("bridge-drops-a-byte", "expected-count", "no-such-file", "unlisted-file"))
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(SUITE):
        sys.stderr.write("%s is not here: run tools/encoding/fetch.sh\n" % SUITE)
        return 2
    extra = unaccounted() if args.plant != "unlisted-file" else unaccounted() + ["new-file.any.js"]
    if extra:
        sys.stderr.write("\033[0;31m\n### Files of the suite that are neither run nor listed as skipped ###\033[0m\n  %s\n" % "\n  ".join(extra))
        return 3
    files = list(RUN) + (["no-such-file.any.js"] if args.plant == "no-such-file" else [])
    missing = [f for f in files if not os.path.exists(os.path.join(SUITE, f))]
    if missing:
        sys.stderr.write("\033[0;31m\n### Test files that are not in the suite ###\033[0m\n  %s\n" % "\n  ".join(missing))
        return 3
    R.check_image()
    results = run_suite(args.binary, files, args.plant)

    tally = OrderedDict((f, [0, 0, 0]) for f in files)
    failures = []
    for r in results:
        t = tally[r["file"]]
        t[{"pass": 0, "deferred": 1, "fail": 2}[r["status"]]] += 1
        if r["status"] == "fail":
            failures.append("%s: %s -- %s" % (r["file"], r["name"], r["message"]))
    if args.update:
        for f, (p, d, x) in tally.items():
            print("%s\t%d\t%d\t%d" % (f, p, d, x))
        return 0

    status = 0
    if failures:
        sys.stderr.write("\033[0;31m\n### Failed tests of the standard's suite ###\033[0m\n")
        for line in failures[:40]:
            sys.stderr.write("  %s\n" % line[:260])
        if len(failures) > 40:
            sys.stderr.write("  ... and %d more\n" % (len(failures) - 40))
        status = 3
    empty = [f for f, t in tally.items() if sum(t) == 0]
    if empty:
        sys.stderr.write("\033[0;31m\n### Test files that ran no tests ###\033[0m\n  %s\n" % "\n  ".join(empty))
        status = 3
    expected = read_expected(EXPECTED)
    if args.plant == "expected-count":
        first = next(iter(expected))
        p, d, x = expected[first]
        expected[first] = (p + 1, d, x)
    changed = []
    for f in files:
        got = tuple(tally[f])
        if expected.get(f) != got:
            changed.append("%s: listed %s, found %s" % (f, expected.get(f), got))
    for f in expected:
        if f not in tally:
            changed.append("%s: listed and not run" % f)
    if changed:
        sys.stderr.write("\033[0;31m\n### Counts of the suite's tests that have changed ###\033[0m\n  %s\n" % "\n  ".join(changed))
        status = 3
    passed = sum(t[0] for t in tally.values())
    deferred = sum(t[1] for t in tally.values())
    failed = sum(t[2] for t in tally.values())
    if args.verbose or status == 0:
        for f, (p, d, x) in tally.items():
            print("  %-46s %5d passed %4d deferred %3d failed" % (f, p, d, x))
    print("encoding suite: %d files, %d tests: %d passed, %d deferred to the multi-byte stage, %d failed%s" % (
        len(files), passed + deferred + failed, passed, deferred, failed, "" if status == 0 else "  FAILED"))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
