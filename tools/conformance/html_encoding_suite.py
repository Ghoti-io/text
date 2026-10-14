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
"""Score the HTML parser's choice of encoding against html5lib-tests' encoding vectors.

    html_encoding_suite.py <encoding dir> <runner> <exceptions file> [--propose] [--verbose]

The vectors are `encoding/tests1.dat`, `tests2.dat` and `test-yahoo-jp.dat` of the pinned html5lib-tests tree
(tools/html/fetch.sh): a document (`#data`) and the encoding the reader must decide on (`#encoding`), with
the default windows-1252 ("you may have to set your defaults appropriately" says the first one). As html5lib's own
harness does, each is held to two readers: the prescan alone (`sniff`, skipped for a document longer than the
1024 bytes the prescan reads) and the whole parser (`parse`, the encoding it ends with: a restart included).
The incremental parser is run too, fed a byte at a time and cut at every offset (every offset up to 300 bytes, 24
spread over the others), and **must answer exactly as the whole parse did**: the decision cannot depend on the cutting.

**Scope.** `scripted/` (a `document.write` writes the `<meta>`: it needs script execution, which this library never does) and
`chardet/` (a file for a detector to guess from, with no `#data`: this library guesses nothing and says why in html.md)
are out of scope and listed below with that reason; a file of either that is not as listed, or any other entry of the
directory, fails the gate (an unlisted directory is a population nobody scored).

**The exceptions file** (HTML_ENCODING_EXCEPTIONS) is one line for each vector the parser answers differently from the
vector, `<id> TAB <reader> TAB <expected> TAB <got> TAB <reason>`: the reader is `sniff` or `parse`; the reason says what
the served standard says that html5lib's vector does not (the sentence is in tools/oracle/html_sniff_divergences.txt).
The gate fails on

  UNEXPLAINED FAILURES  a vector that does not pass and is not listed
  STALE EXCEPTIONS      a listed vector that now passes, answers something else, or does not exist
  below the floor       fewer vectors loaded than HTML_ENCODING_MIN_VECTORS, or passing than HTML_ENCODING_MIN_PASS

Copyright 2026 by Corey Pennycuff
"""
import os
import subprocess
import sys

VECTOR_FILES = ("tests1.dat", "tests2.dat", "test-yahoo-jp.dat")
# directory -> (the files it holds, why the vectors in it are not scored)
OUT_OF_SCOPE = {
    "scripted": (("tests1.dat",), "the document is written by a script (document.write); this library never runs script"),
    "chardet": (("test_big5.txt",), "a file for a character-statistics detector to guess; this library guesses nothing from content"),
}
PRESCAN_BYTES = 1024
TIMEOUT = int(os.environ.get("HTML_ENCODING_RUNNER_TIMEOUT", "300"))


def parse_dat(path, name):
    with open(path, "rb") as fh:
        data = fh.read()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    tests, current, section = [], None, None
    for line in lines:
        if line in (b"#data", b"#encoding"):
            if line == b"#data":
                current = {"data": [], "encoding": []}
                tests.append(current)
            section = line[1:].decode()
            continue
        if current is not None and section:
            current[section].append(line)
    out = []
    for n, t in enumerate(tests, 1):
        # html5lib's harness removes the one line feed that ends each section.
        body = b"\n".join(t["data"])
        enc = b"\n".join(t["encoding"]).strip().decode("ascii")
        if not enc:
            raise SystemExit("%s: vector %d has no #encoding" % (name, n))
        out.append({"id": "%s#%d" % (name, n), "data": body, "expected": enc.lower()})
    return out


def load(dirpath):
    vectors = []
    for name in VECTOR_FILES:
        path = os.path.join(dirpath, name)
        if not os.path.isfile(path):
            raise SystemExit("%s is not here. Fetch the pinned vectors:  tools/html/fetch.sh" % path)
        vectors.extend(parse_dat(path, name))
    return vectors


def check_scope(dirpath):
    """Problems with the directory's entries: anything neither scored nor listed."""
    problems = []
    known = set(VECTOR_FILES) | set(OUT_OF_SCOPE)
    for entry in sorted(os.listdir(dirpath)):
        if entry not in known:
            problems.append("%s is neither scored nor listed as out of scope" % entry)
    for d, (files, _) in OUT_OF_SCOPE.items():
        p = os.path.join(dirpath, d)
        if not os.path.isdir(p):
            problems.append("%s/ is listed as out of scope and is not there" % d)
            continue
        have = tuple(sorted(os.listdir(p)))
        if have != tuple(sorted(files)):
            problems.append("%s/ holds %s, not the %s that were listed" % (d, ", ".join(have) or "nothing", ", ".join(files)))
    return problems


EXCEPTIONS_FIRST = "# HTML encoding exceptions"
EXCEPTIONS_LAST = "# end of exceptions"


def load_exceptions(path):
    table = {}
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    meaningful = [l for l in lines if l.strip()]
    if not meaningful or not meaningful[0].startswith(EXCEPTIONS_FIRST) or meaningful[-1] != EXCEPTIONS_LAST:
        raise SystemExit("EXCEPTIONS FILE %s: it must begin with '%s' and end with '%s' (a truncated or empty file is not 'no exceptions')" % (
            path, EXCEPTIONS_FIRST, EXCEPTIONS_LAST))
    if True:
        for n, raw in enumerate(lines, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            parts = raw.split("\t")
            if len(parts) != 5 or parts[1] not in ("sniff", "parse") or not parts[4].strip():
                raise SystemExit("%s:%d: not <id> <sniff|parse> <expected> <got> <reason>" % (path, n))
            if (parts[0], parts[1]) in table:
                raise SystemExit("%s:%d: %s %s is listed twice" % (path, n, parts[0], parts[1]))
            table[(parts[0], parts[1])] = (parts[2], parts[3], parts[4])
    return table


def run_batch(runner, requests):
    data = ("\n".join(requests) + "\n").encode("ascii")
    try:
        done = subprocess.run([runner], input=data, capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise SystemExit("the runner did not answer %d requests within %d seconds" % (len(requests), TIMEOUT))
    if done.returncode != 0:
        raise SystemExit("the runner exited %d:\n%s" % (done.returncode, done.stderr.decode("utf-8", "replace")[-1500:]))
    answers, cur = [], {}
    for line in done.stdout.decode("utf-8", "replace").split("\n"):
        if not line:
            continue
        key, _, value = line.partition(" ")
        if key == "END":
            cur["end"] = value
            answers.append(cur)
            cur = {}
        else:
            cur[key.lower()] = value
    if len(answers) != len(requests):
        raise SystemExit("sent %d requests, got %d answers" % (len(requests), len(answers)))
    return answers


def ask(runner, requests):
    out = []
    for lo in range(0, len(requests), 2000):
        out.extend(run_batch(runner, requests[lo:lo + 2000]))
    return out


def cuts_of(n):
    if n <= 300:
        return list(range(n + 1))
    return sorted(set(round(i * n / 23) for i in range(24)) | {0, n, 1023, 1024, 1025} & set(range(n + 1)))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    propose = "--propose" in argv
    if len(args) != 3:
        raise SystemExit(__doc__)
    dirpath, runner, exceptions_path = args
    min_vectors = int(os.environ.get("HTML_ENCODING_MIN_VECTORS", "0"))
    min_pass = int(os.environ.get("HTML_ENCODING_MIN_PASS", "0"))
    vectors = load(dirpath)
    scope_problems = check_scope(dirpath)
    listed = load_exceptions(exceptions_path)

    requests, owners = [], []
    for v in vectors:
        hx = "x" + v["data"].hex()
        readers = [("parse", "parse %s" % hx), ("cut:bytes", "parser-bytes %s" % hx)]
        if len(v["data"]) <= PRESCAN_BYTES:
            readers.insert(0, ("sniff", "sniff %s" % hx))
        readers.extend(("cut:%d" % k, "parser-split:%d %s" % (k, hx)) for k in cuts_of(len(v["data"])))
        for reader, request in readers:
            requests.append(request)
            owners.append((v, reader))
    answers = ask(runner, requests)

    by_vector = {}
    for (v, reader), a in zip(owners, answers):
        by_vector.setdefault(v["id"], {"v": v, "answers": {}})["answers"][reader] = a

    outcome = {}   # (id, reader) -> got (lower case name)
    cut_problems = []
    for vid, rec in by_vector.items():
        got = {}
        for reader, a in rec["answers"].items():
            if a.get("end") != "DONE":
                cut_problems.append("%s %s: the runner answered %s" % (vid, reader, a.get("end")))
                continue
            got[reader] = (a["encoding"].lower(), a["source"], a["confidence"], a["reparsed"])
        base = got.get("parse")
        for reader, g in got.items():
            if reader.startswith("cut:") and base is not None and g != base:
                cut_problems.append("%s: THE DECISION DEPENDS ON THE CUTTING (%s gave %s, the whole parse %s)" % (vid, reader, g, base))
        for reader in ("sniff", "parse"):
            if reader in got:
                outcome[(vid, reader)] = got[reader][0]

    if propose:
        for (vid, reader), g in sorted(outcome.items()):
            v = by_vector[vid]["v"]
            if g != v["expected"]:
                print("%s\t%s\t%s\t%s\tPROPOSED" % (vid, reader, v["expected"], g))
        return 0

    unexplained, stale = [], []
    passes = scored = 0
    for vid, rec in by_vector.items():
        v = rec["v"]
        ok = True
        for reader in ("sniff", "parse"):
            if (vid, reader) not in outcome:
                continue
            g = outcome[(vid, reader)]
            tag = listed.get((vid, reader))
            if g == v["expected"]:
                if tag:
                    stale.append("%s %s now passes (listed %s -> %s)" % (vid, reader, tag[0], tag[1]))
            else:
                ok = False
                if not tag:
                    unexplained.append("%s %s: expected %s, got %s" % (vid, reader, v["expected"], g))
                elif tag[0] != v["expected"] or tag[1] != g:
                    stale.append("%s %s answers %s, not the %s that was listed (expected %s)" % (vid, reader, g, tag[1], v["expected"]))
        scored += 1
        passes += 1 if ok else 0
    ids = {v["id"] for v in vectors}
    for (vid, reader) in listed:
        if vid not in ids or (vid, reader) not in outcome:
            stale.append("%s %s is not a vector (or not one for that reader)" % (vid, reader))

    print("vectors: %d in %d files; out of scope: %s" % (
        len(vectors), len(VECTOR_FILES), "; ".join("%s/ (%s)" % (d, why) for d, (_, why) in OUT_OF_SCOPE.items())))
    print("passing: %d of %d; listed exceptions: %d (readers: sniff %d, parse %d)" % (
        passes, scored, len(listed), sum(1 for k in listed if k[1] == "sniff"), sum(1 for k in listed if k[1] == "parse")))
    print("the incremental parser, fed a byte at a time and cut at %d offsets in all, answers as the whole parse does" % (
        sum(1 for (_, r) in owners if r.startswith("cut:") and r != "cut:bytes")))
    failed = False
    for title, items in (("UNEXPLAINED FAILURES", unexplained), ("STALE EXCEPTIONS", stale), ("CUTTING CHANGES THE DECISION", cut_problems),
                         ("OUT OF SCOPE, BUT NOT AS LISTED", scope_problems)):
        if items:
            failed = True
            sys.stderr.write("\033[0;31m\n### %s (%d) ###\033[0m\n" % (title, len(items)))
            for line in items[:60]:
                sys.stderr.write("  %s\n" % line)
    if len(vectors) < min_vectors:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d vectors loaded, floor %d ###\033[0m\n" % (len(vectors), min_vectors))
    if passes < min_pass:
        failed = True
        sys.stderr.write("\033[0;31m\n### below the floor: %d passing, floor %d ###\033[0m\n" % (passes, min_pass))
    if failed:
        return 3
    print("html encoding: every vector passes or is a listed exception; none is stale.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
