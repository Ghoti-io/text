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
"""Read the selector tables of web-platform-tests' dom/nodes/selectors.js as data.

    tools/css/extract-wpt-selectors.py <wpt checkout> <out.json>

`selectors.js` holds three arrays that the DOM tests of `querySelector()`, `querySelectorAll()`, `matches()` and `closest()` run: `invalidSelectors` (each must throw a SyntaxError),
`validSelectors` (each with the ids of the elements it matches in the fixture `ParentNode-querySelector-All-content.html`, in tree order, the contexts and the document types it is
excluded from, and the bits of `testType`: 1 for the queries, 16 for `matches()`) and `scopedSelectors` (the same, with a context element `ctx` or reference nodes `ref` they are
matched relative to). The arrays are written as JavaScript object literals, one entry a line, with comments; this reads them with a small reader of the literal syntax (strings,
numbers, arrays, objects, the three `TEST_*` constants and `|`) and fails on anything else, so that a change of the file that the reader does not understand is a failure and not a
silent drop. The output records the commit the file was read from, and the sha256 of the file.
"""

import hashlib
import json
import re
import subprocess
import sys

CONSTANTS = {"TEST_QSA": 1, "TEST_FIND": 4, "TEST_MATCH": 16}


class Reader:
    def __init__(self, text):
        self.s = text
        self.i = 0

    def skip(self):
        s = self.s
        while self.i < len(s):
            c = s[self.i]
            if c in " \t\r\n":
                self.i += 1
            elif s.startswith("//", self.i):
                j = s.find("\n", self.i)
                self.i = len(s) if j < 0 else j
            elif s.startswith("/*", self.i):
                j = s.find("*/", self.i)
                if j < 0:
                    raise SystemExit("an unterminated comment")
                self.i = j + 2
            else:
                break

    def peek(self):
        self.skip()
        return self.s[self.i] if self.i < len(self.s) else ""

    def expect(self, ch):
        if self.peek() != ch:
            raise SystemExit("expected %r at %d, found %r" % (ch, self.i, self.s[self.i:self.i + 30]))
        self.i += 1

    def string(self):
        q = self.s[self.i]
        self.i += 1
        out = []
        while self.s[self.i] != q:
            c = self.s[self.i]
            if c == "\\":
                n = self.s[self.i + 1]
                if n == "u":
                    out.append(chr(int(self.s[self.i + 2:self.i + 6], 16)))
                    self.i += 6
                    continue
                if n == "x":
                    out.append(chr(int(self.s[self.i + 2:self.i + 4], 16)))
                    self.i += 4
                    continue
                out.append({"n": "\n", "t": "\t", "r": "\r", "f": "\f", "0": "\0"}.get(n, n))
                self.i += 2
                continue
            out.append(c)
            self.i += 1
        self.i += 1
        return "".join(out)

    def value(self):
        c = self.peek()
        if c in "\"'":
            return self.string()
        if c == "[":
            self.i += 1
            out = []
            while self.peek() != "]":
                out.append(self.value())
                if self.peek() == ",":
                    self.i += 1
            self.i += 1
            return out
        if c == "{":
            self.i += 1
            out = {}
            while self.peek() != "}":
                m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(self.s, self.i)
                if not m:
                    raise SystemExit("a key at %d: %r" % (self.i, self.s[self.i:self.i + 30]))
                key = m.group(0)
                self.i = m.end()
                self.expect(":")
                out[key] = self.value()
                if self.peek() == ",":
                    self.i += 1
            self.i += 1
            return out
        m = re.compile(r"-?[0-9]+").match(self.s, self.i)
        if m and not re.compile(r"[A-Za-z_]").match(self.s, m.end()):
            self.i = m.end()
            v = int(m.group(0))
        else:
            m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(self.s, self.i)
            if not m or m.group(0) not in CONSTANTS:
                raise SystemExit("a value at %d: %r" % (self.i, self.s[self.i:self.i + 30]))
            self.i = m.end()
            v = CONSTANTS[m.group(0)]
        while self.peek() == "|":
            self.i += 1
            self.skip()
            m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(self.s, self.i)
            if not m or m.group(0) not in CONSTANTS:
                raise SystemExit("a constant at %d" % self.i)
            self.i = m.end()
            v |= CONSTANTS[m.group(0)]
        return v


def main():
    wpt, out = sys.argv[1:3]
    path = wpt + "/dom/nodes/selectors.js"
    text = open(path, encoding="utf-8").read()
    arrays = {}
    for name in ("invalidSelectors", "validSelectors", "scopedSelectors"):
        m = re.search(r"^var %s = \[" % name, text, re.M)
        if not m:
            raise SystemExit("selectors.js has no %s" % name)
        r = Reader(text)
        r.i = m.end() - 1
        arrays[name] = r.value()
    commit = subprocess.run(["git", "-C", wpt, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    result = {
        "commit": commit,
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "invalid": arrays["invalidSelectors"],
        "valid": arrays["validSelectors"],
        "scoped": arrays["scopedSelectors"],
    }
    for kind in ("valid", "scoped"):
        for n, e in enumerate(result[kind]):
            for key in ("name", "selector", "expect", "level", "testType"):
                if key not in e:
                    raise SystemExit("%s entry %d has no %s" % (kind, n, key))
    json.dump(result, open(out, "w", encoding="utf-8"), ensure_ascii=False)
    print("%d invalid, %d valid, %d scoped" % (len(result["invalid"]), len(result["valid"]), len(result["scoped"])))


if __name__ == "__main__":
    main()
