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

**What runs.** The JavaScript files of RUN below (tests of the Encoding API, three of them for the multi-byte encodings), and the HTML pages of the legacy multi-byte
vectors (`legacy-mb-*/`: decode pages and decode-errors pages that read the text a document of the encoding becomes; form pages and href pages that read what a
browser makes of each character in a document of the encoding; the `<a>` pages with a list of characters): see the header of containers/html/wpt.mjs for what of the
browser is emulated and what is not. Every page is classified by what it includes (`classify()`), and a page that is not run is in NOT_RUN_PAGES with its reason: the
browser's own loader (a byte order mark deciding a document's encoding, sniffing, subresources), rendering (the reftests of a truncated sequence at the end of a file), and
a form posted to a `data:` URL and read back through `postMessage`. **What does not**, and why, is NOT_RUN and NOT_RUN_DIRECTORIES: the streams (the Streams API),
`encodeInto` (an API of its own), and the files about the JavaScript bindings. A file or directory that is in none of the lists fails the run.

The count of passed and failed tests of each file is in tools/oracle/encoding_wpt_expected.txt, and the run fails on a failure, on a count
that has changed (in either direction: a test that passes now and did not is a change to read, not a gain to keep) and on a file that ran nothing.
`--update` prints the list the run found. A file named in RUN, NOT_RUN, NOT_RUN_PAGES or NOT_RUN_DIRECTORIES that is not in the suite, one that is listed as not run
and has a line in the counts (it runs), and one listed twice, fail the run.

**The one page that cannot be emulated** (iso2022jp-encode-form-errors-stateful.html: a form posted to a `data:` URL in an iframe and read back through postMessage) is
not run, and is not left to a copy of its vector either: `check_stateful_page()` reads the page's own two expected strings (the form's value and the ISO-2022-JP bytes
the page asserts) out of its source, and requires this library's encoder, in the HTML mode a form uses, to write exactly those bytes, and this library's decoder to
read them back to the page's own `utf16` string.

--plant NAME corrupts the run, to show the gate fails (run-encoding-oracle.sh --controls): bridge-drops-a-byte (the bridge loses the last byte of
every input of more than three), expected-count (the list says one test more), no-such-file (a file that is not in the suite), unlisted-file, stale-listing (a file listed as not run that
is not in the suite), stateful-expectation (the page's expected bytes are changed).

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import re
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
    "legacy-mb-schinese/gb18030/gb18030-decoder.any.js",
    "legacy-mb-schinese/gbk/gbk-decoder.any.js",
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

# Every JavaScript test file and every HTML page of the suite's directory is run, or here, by name, and every directory is in NOT_RUN_DIRECTORIES: a bumped
# WPT_COMMIT that adds a file or a directory makes the run fail until somebody has said what to do with it.
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
])
# The HTML pages that are tests and are not run, with the reason (a page that includes testharness.js and is neither emulated by wpt.mjs nor here fails the run).
NOT_RUN_PAGES = OrderedDict([
    ("bom-handling.html", "the browser's loader: `document.characterSet` of a page that starts with a byte order mark"),
    ("json-document-utf8.html", "the browser's loader: the encoding of a JSON document fetched into an iframe"),
    ("remove-only-one-bom.html", "the browser's loader: the text of iframes whose documents start with two byte order marks"),
    ("sharedarraybuffer.https.html", "needs a cross-origin isolated https page (a SharedArrayBuffer); the decoding of a view over one is in textdecoder-copy.any.js and the unit tests"),
    ("sniffing.html", "the browser's loader: a page must not be sniffed as UTF-8"),
    ("utf-32-from-win1252.html", "the browser's loader: UTF-32 documents and subresources"),
    ("utf-32.html", "the browser's loader: UTF-32 documents and subresources"),
    ("eof-shift_jis.html", "a reftest: the rendering of a document that ends in a truncated Shift_JIS sequence (the decode is in the unit tests and the differential)"),
    ("eof-utf-8-one.html", "a reftest: the rendering of a document that ends in a truncated UTF-8 sequence"),
    ("eof-utf-8-two.html", "a reftest: the rendering of a document that ends in a truncated UTF-8 sequence"),
    ("eof-utf-8-three.html", "a reftest: the rendering of a document that ends in a truncated UTF-8 sequence"),
    ("legacy-mb-japanese/iso-2022-jp/iso2022jp-encode-form-errors-stateful.html",
     "a form posted to a data: URL in an iframe whose document is read back through postMessage; its expectations are read out of its source and held to the library by check_stateful_page() (and its vector is in tests/test-encoding-cjk.cpp)"),
])


def classify(path):
    """What kind of page a *.html file of the suite is, by what it includes: decode, form, href, reftest, data (a fixture the other pages read), or other."""
    with open(path, "rb") as fh:
        text = fh.read().decode("latin-1")
    if "testharness.js" not in text:
        return "reftest" if re.search(r"rel=[\"']?match", text) else "data"
    if "decode-common.js" in text or 'getElementById("scrwin")' in text:
        return "decode"
    if "encode-form-common.js" in text:
        return "form"
    if "encode-href-common.js" in text or re.search(r"createElement\([\"']a[\"']\)", text):
        return "href"
    return "other"


def pages():
    """[(kind, relative path)] of the pages that are run, and the files and directories of the suite that are neither run nor listed."""
    run, unlisted = [], []
    for root, dirs, files in os.walk(SUITE):
        rel_root = os.path.relpath(root, SUITE)
        dirs[:] = sorted(d for d in dirs if not (rel_root == "." and d in NOT_RUN_DIRECTORIES) and d != ".git")
        for name in sorted(files):
            rel = name if rel_root == "." else os.path.join(rel_root, name)
            if name.endswith(".html"):
                kind = classify(os.path.join(root, name))
                if kind in ("decode", "form", "href") and rel not in NOT_RUN_PAGES:
                    run.append((kind, rel))
                elif kind in ("data",):
                    continue
                elif rel not in NOT_RUN_PAGES:
                    unlisted.append(rel)
            elif name.endswith((".any.js", ".window.js")):
                if rel not in RUN and rel not in NOT_RUN:
                    unlisted.append(rel)
    return run, unlisted


def unaccounted():
    """The files of the suite that are neither run nor listed as skipped."""
    return pages()[1]


def read_expected(path):
    out = OrderedDict()
    with open(path, encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3 or not all(p.isdigit() for p in parts[1:]):
                raise SystemExit("%s:%d: not three tab-separated fields (file, passed, failed): %r" % (path, number, line[:80]))
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


STATEFUL_PAGE = "legacy-mb-japanese/iso-2022-jp/iso2022jp-encode-form-errors-stateful.html"


def js_string(text):
    """The value of a JavaScript string literal of the page (single-quoted; \\xNN is the only escape the page uses)."""
    return re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), text)


def check_stateful_page(binary, plant=None):
    """The expectations of the page that cannot be run, read from its source: the form's value, the bytes the browser must post and the text they read back as.
    Returns a list of failure lines (empty when this library's encoder writes the page's bytes and its decoder reads them back to the page's text)."""
    path = os.path.join(SUITE, STATEFUL_PAGE)
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    value = re.search(r"name: 'utf16',\s*value: '([^']*)'", text)
    expect = re.search(r"assert_equals\(\s*iso2022jp,\s*'([^']*)'\);", text)
    decoded = re.search(r"assert_equals\(utf16, '([^']*)'\);", text)
    if not (value and expect and decoded):
        return ["%s: the page's expectations are not where this check reads them (the form's value, the two assert_equals)" % STATEFUL_PAGE]
    want_bytes = js_string(expect.group(1)).encode("latin-1")
    if plant == "stateful-expectation":
        want_bytes = want_bytes.replace(b"\x1b(B", b"", 1)
    ask = ["NE 0 %s H" % b"iso-2022-jp".hex(), "E 0 1 %s" % js_string(value.group(1)).encode("utf-8").hex(),
           "ND 1 %s -" % b"iso-2022-jp".hex(), "D 1 1 %s" % want_bytes.hex()]
    done = subprocess.run([binary, "serve"], input=("\n".join(ask) + "\n").encode("ascii"), capture_output=True)
    answers = done.stdout.decode("ascii").split("\n")
    if done.returncode != 0 or len(answers) < 4:
        return ["%s: the library's runner failed" % STATEFUL_PAGE]
    out = []
    enc = answers[1].split()
    if enc[0] != "OUT" or bytes.fromhex(enc[1] if enc[1] != "-" else "") != want_bytes:
        out.append("%s: the form's value is written as %s, the page expects %r" % (STATEFUL_PAGE, answers[1][:200], want_bytes))
    dec = answers[3].split()
    got = bytes.fromhex(dec[1] if dec[0] == "OUT" and dec[1] != "-" else "").decode("utf-8") if dec[0] == "OUT" else None
    if got != js_string(decoded.group(1)):
        out.append("%s: the page's bytes read back as %r, the page expects %r" % (STATEFUL_PAGE, got, js_string(decoded.group(1))))
    return out


def stale_listings(expected):
    """Lines for what the lists name and the suite or the counts contradict: a file or directory that is not there, one listed as not run that has counts
    (so it runs), and one named twice."""
    out = []
    for name, table, want_dir in (("RUN", RUN, False), ("NOT_RUN", NOT_RUN, False), ("NOT_RUN_PAGES", NOT_RUN_PAGES, False), ("NOT_RUN_DIRECTORIES", NOT_RUN_DIRECTORIES, True)):
        for item in table:
            full = os.path.join(SUITE, item)
            if not (os.path.isdir(full) if want_dir else os.path.isfile(full)):
                out.append("%s names %s, which is not in the suite" % (name, item))
    for name, table in (("NOT_RUN", NOT_RUN), ("NOT_RUN_PAGES", NOT_RUN_PAGES)):
        for item in table:
            if item in expected:
                out.append("%s names %s, which has counts in encoding_wpt_expected.txt: it runs" % (name, item))
            if item in RUN:
                out.append("%s names %s, which is also in RUN" % (name, item))
    if len(set(RUN)) != len(RUN):
        out.append("RUN names a file twice")
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary")
    ap.add_argument("--plant", choices=("bridge-drops-a-byte", "expected-count", "no-such-file", "unlisted-file", "stale-listing", "stateful-expectation"))
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(SUITE):
        sys.stderr.write("%s is not here: run tools/encoding/fetch.sh\n" % SUITE)
        return 2
    page_list, extra = pages()
    if args.plant == "unlisted-file":
        extra = extra + ["new-file.any.js"]
    if extra:
        sys.stderr.write("\033[0;31m\n### Files of the suite that are neither run nor listed as skipped ###\033[0m\n  %s\n" % "\n  ".join(extra))
        return 3
    files = list(RUN) + [rel for _, rel in page_list] + (["no-such-file.any.js"] if args.plant == "no-such-file" else [])
    missing = [f for f in files if not os.path.exists(os.path.join(SUITE, f))]
    if missing:
        sys.stderr.write("\033[0;31m\n### Test files that are not in the suite ###\033[0m\n  %s\n" % "\n  ".join(missing))
        return 3
    expected_early = read_expected(EXPECTED)
    if args.plant == "stale-listing":
        NOT_RUN["no-longer-in-the-suite.any.js"] = "planted"
    stale = stale_listings(expected_early)
    if stale:
        sys.stderr.write("\033[0;31m\n### Listed files of the suite that are gone, or that run ###\033[0m\n  %s\n" % "\n  ".join(stale))
        return 3
    R.check_image()
    entries = list(RUN) + ["%s=%s" % (kind, rel) for kind, rel in page_list] + (["no-such-file.any.js"] if args.plant == "no-such-file" else [])
    results = run_suite(args.binary, entries, args.plant)

    tally = OrderedDict((f, [0, 0]) for f in files)
    failures = []
    for r in results:
        t = tally[r["file"]]
        if r["status"] == "pass":
            t[0] += r.get("count", 1)
        else:
            t[1] += 1
            failures.append("%s: %s -- %s" % (r["file"], r["name"], r["message"]))
    if args.update:
        for f, (p, x) in tally.items():
            print("%s\t%d\t%d" % (f, p, x))
        return 0

    status = 0
    failures.extend(check_stateful_page(args.binary, args.plant))
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
        p, x = expected[first]
        expected[first] = (p + 1, x)
    changed = []
    for f in files:
        got = tuple(tally[f])
        if expected.get(f) != got:
            changed.append("%s: listed %s, found %s" % (f, expected.get(f), got))
    for f in expected:
        if f not in tally:
            changed.append("%s: listed and not run" % f)
    if changed:
        sys.stderr.write("\033[0;31m\n### Counts of the suite's tests that have changed ###\033[0m\n  %s\n" % "\n  ".join(changed[:60]))
        status = 3
    passed = sum(t[0] for t in tally.values())
    failed = sum(t[1] for t in tally.values())
    if args.verbose:
        for f, (p, x) in tally.items():
            print("  %-70s %7d passed %3d failed" % (f, p, x))
    elif status == 0:
        for f, (p, x) in tally.items():
            if f in RUN:
                print("  %-70s %7d passed %3d failed" % (f, p, x))
        by_kind = OrderedDict()
        for kind, rel in page_list:
            k = by_kind.setdefault(kind, [0, 0, 0])
            k[0] += 1
            k[1] += tally[rel][0]
            k[2] += tally[rel][1]
        for kind, (n, p, x) in by_kind.items():
            print("  %-70s %7d passed %3d failed" % ("%d %s pages of the legacy multi-byte vectors" % (n, kind), p, x))
    if status == 0:
        print("  %-70s the page's own expected bytes and text, read from its source: written and read back by the library" % STATEFUL_PAGE.split("/")[-1])
    print("encoding suite: %d JavaScript files and %d pages, %d tests: %d passed, %d failed; %d pages are for a browser and are listed with the reason%s" % (
        len(RUN), len(page_list), passed + failed, passed, failed, len(NOT_RUN_PAGES), "" if status == 0 else "  FAILED"))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
