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
"""The CSSOM interface ledger gate (C5e).

    check-cssom-ledger.py [--controls] [--ledger FILE] [--idl DIR]

tools/css/cssom_ledger.tsv has a row for every member of every interface of the CSSOM family in the pinned WebIDL (the files of tools/css/WEBREF_IDL_SHA256, read by
tools/css/cssom_idl.py): the interfaces the 27 kinds of rule and the object model name (``CSS*``, ``MediaList``, ``StyleSheet``, ``StyleSheetList``, ``LinkStyle``,
``DocumentOrShadowRoot``, ``ElementCSSInlineStyle``, ``MediaQueryList`` and its event) and the two members of ``Window`` that CSSOM adds. A member is

* ``implemented``: the column names the functions of the installed headers that are the member, and a test program of ``tests/`` that calls the first of them;
* ``binding``: the member exists only in a script binding (a Promise, an event handler, an event object), with the reason;
* ``out``: this library has no such member, with the reason.

The gate fails (exit 1, every problem listed) on a member with no row, a row for a member the IDL does not have, a duplicate, a hash of an IDL file that is not the pinned one, an
``implemented`` row whose function is not declared in an installed header or whose test does not exist or does not call it, a status that is not one of the three, and a reason
too short to be one. ``--controls`` makes each mistake in a copy of the ledger and requires the report.
"""
import glob
import hashlib
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import cssom_idl  # noqa: E402

LEDGER = os.path.join(HERE, "cssom_ledger.tsv")
MANIFEST = os.path.join(HERE, "WEBREF_IDL_SHA256")
IDL_DIR = os.path.join(ROOT, "third_party", "css", "webref-idl")
SCOPE = re.compile(r'^(CSS[A-Za-z]*|MediaList|StyleSheet|StyleSheetList|LinkStyle|DocumentOrShadowRoot|ElementCSSInlineStyle|MediaQueryList|MediaQueryListEvent|MediaQueryListEventInit|PropertyDefinition|Window)$')
WINDOW_MEMBERS = ("getComputedStyle", "matchMedia")
STATUSES = ("implemented", "binding", "out")


def idl_members(idl_dir):
    problems = []
    files = []
    for line in open(MANIFEST, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        sha, name = line.split()
        path = os.path.join(idl_dir, name)
        if not os.path.exists(path):
            problems.append("%s is missing (tools/css/fetch.sh)" % name)
            continue
        if hashlib.sha256(open(path, "rb").read()).hexdigest() != sha:
            problems.append("%s does not hash as pinned" % name)
        files.append(path)
    members = {}
    for path in files:
        defs, _, _ = cssom_idl.read(open(path, encoding="utf-8").read())
        f = os.path.basename(path)[:-4]
        for _, kind, name, ms in defs:
            if not SCOPE.match(name) or (f == "css-view-transitions" and name != "CSSViewTransitionRule"):
                continue
            for mk, mn, _ in ms:
                if name == "Window" and mn not in WINDOW_MEMBERS:
                    continue
                members[(name, mn)] = (mk, f)
    return members, problems


def installed_symbols():
    syms = set()
    for h in glob.glob(os.path.join(ROOT, "include", "ghoti.io", "text", "css", "*.h")):
        syms.update(re.findall(r'\b(gtext_[a-z0-9_]+)\s*\(', open(h, encoding="utf-8").read()))
    return syms


def with_support(path):
    """The text of a test program and of the support headers of tests/ it includes (the wrappers a test calls the API through)."""
    text = open(path, encoding="utf-8").read()
    for name in re.findall(r'#include\s+"([^"]+)"', text):
        inc = os.path.join(os.path.dirname(path), name)
        if os.path.exists(inc):
            text += with_support(inc)
    return text


def check(ledger, idl_dir):
    problems = []
    members, p = idl_members(idl_dir)
    problems += p
    rows = {}
    syms = installed_symbols()
    texts = {}
    for n, line in enumerate(open(ledger, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 7:
            problems.append("line %d has %d fields, not seven" % (n, len(f)))
            continue
        iface, member, kind, idl_file, status, detail, test = f
        key = (iface, member)
        if key in rows:
            problems.append("%s.%s is listed twice" % key)
        rows[key] = f
        if key not in members:
            problems.append("STALE %s.%s: the pinned IDL has no such member" % key)
            continue
        if (kind, idl_file) != members[key]:
            problems.append("%s.%s: the IDL says %s in %s, the row %s in %s" % (iface, member, members[key][0], members[key][1], kind, idl_file))
        if status not in STATUSES:
            problems.append("%s.%s: status %r is not one of %s" % (iface, member, status, ", ".join(STATUSES)))
        elif status == "implemented":
            names = detail.split(",")
            for s in names:
                if s not in syms:
                    problems.append("%s.%s: %s is not declared in an installed header" % (iface, member, s))
            path = os.path.join(ROOT, "tests", test)
            if not os.path.exists(path):
                problems.append("%s.%s: the test %s does not exist" % (iface, member, test))
            else:
                if path not in texts:
                    texts[path] = with_support(path)
                if names[0] not in texts[path]:
                    problems.append("%s.%s: %s does not call %s" % (iface, member, test, names[0]))
        else:
            if len(detail.split()) < 8:
                problems.append("%s.%s: the reason is too short to be one" % (iface, member))
            if test != "-":
                problems.append("%s.%s: a %s row has no test (column seven is -)" % (iface, member, status))
    for key in sorted(members):
        if key not in rows:
            problems.append("MISSING %s.%s (%s of %s) has no row" % (key + members[key]))
    return problems, len(rows), len(members)


def main():
    args = sys.argv[1:]
    ledger = LEDGER
    idl = IDL_DIR
    controls = False
    while args:
        a = args.pop(0)
        if a == "--controls":
            controls = True
        elif a == "--ledger":
            ledger = args.pop(0)
        elif a == "--idl":
            idl = args.pop(0)
        else:
            sys.exit("usage: check-cssom-ledger.py [--controls] [--ledger FILE] [--idl DIR]")
    problems, rows, members = check(ledger, idl)
    if problems:
        for p in problems:
            print("LEDGER " + p)
        return 1
    print("CSSOM ledger: %d members of the pinned IDL, %d rows, every one accounted for" % (members, rows))
    if not controls:
        return 0
    lines = open(ledger, encoding="utf-8").read().split("\n")
    data = [i for i, l in enumerate(lines) if l and not l.startswith("#")]
    first_impl = next(i for i in data if lines[i].split("\t")[4] == "implemented")
    first_out = next(i for i in data if lines[i].split("\t")[4] == "out")
    mistakes = {
        "a member with no row": ("MISSING", lambda L: L[:first_impl] + L[first_impl + 1:]),
        "a row for a member the IDL does not have": ("STALE", lambda L: L + ["CSSStyleSheet\tnotAMember\tattribute\tcssom\timplemented\tgtext_cssom_sheet_type\ttest-cssom-sheet.cpp"]),
        "a row twice": ("twice", lambda L: L + [L[first_impl]]),
        "an implemented row with no such function": ("not declared", lambda L: L[:first_impl] + [L[first_impl].replace("gtext_cssom_", "gtext_cssom_nosuch_", 1)] + L[first_impl + 1:]),
        "an implemented row whose test does not call it": ("does not call", lambda L: L[:first_impl] + ["\t".join(L[first_impl].split("\t")[:5] + ["gtext_cssom_sheet_type", "test-cssom-util.cpp"])] + L[first_impl + 1:]),
        "an implemented row with no test file": ("does not exist", lambda L: L[:first_impl] + ["\t".join(L[first_impl].split("\t")[:6] + ["test-no-such.cpp"])] + L[first_impl + 1:]),
        "a reason too short": ("too short", lambda L: L[:first_out] + ["\t".join(L[first_out].split("\t")[:5] + ["no", "-"])] + L[first_out + 1:]),
        "an unknown status": ("status", lambda L: L[:first_impl] + [L[first_impl].replace("\timplemented\t", "\tdone\t", 1)] + L[first_impl + 1:]),
    }
    for what, (want, edit) in mistakes.items():
        with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False, encoding="utf-8") as t:
            t.write("\n".join(edit(lines)))
            name = t.name
        try:
            found, _, _ = check(name, idl)
        finally:
            os.unlink(name)
        if not any(want in p for p in found):
            print("CONTROL %s: the gate did not report %r (it said %r)" % (what, want, found[:3]))
            return 1
        print("control %s: caught" % what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
