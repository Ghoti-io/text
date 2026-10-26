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
"""Compare the names and syntax the generated tables are made from with a second reading of the same data.

    tools/css/check-webref-package.py [--package DIR] [--data DIR] [--divergences FILE]

The tables are generated from the per-series files of w3c/webref (``tools/css/webref_model.py`` consolidates them). The
@webref/css package is the same data consolidated by someone else, with a different method and at its own date. The generator
has no other independent reading, so this is it: for every property, at-rule, descriptor, selector and global type or function,
the two must agree on the name, and on the syntax once white space is folded. A difference is not silently absorbed and is not
an error to fix by editing the data: it is **classified** in ``tools/css/WEBREF_PACKAGE_DIVERGENCES`` (kind, name, reason of at
least twenty characters). A difference that is not listed, and a listing for a difference that is gone, both fail.

If the package has not been fetched this fails and says how (``tools/css/fetch.sh``); it never skips. ``CSS_ORACLE_OFFLINE=1`` is
the one way to run without it, and then the output says ``NOT RUN`` and the exit status is 3, so nothing reads it as a pass.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import webref_model as wm  # noqa: E402


def fold(text):
    return re.sub(r"\s+", " ", text or "").strip()


def divergences_of(model, pkg):
    """The differences, as a dict {(kind, name): description}."""
    out = {}

    def note(kind, name, text):
        out[(kind, name)] = text

    # properties
    pp = {}
    for p in pkg["properties"]:
        pp[p["name"]] = p
    for name in sorted(set(model["props"]) - set(pp)):
        note("property", name, "in the pin, not in the package")
    for name in sorted(set(pp) - set(model["props"])):
        note("property", name, "in the package, not in the pin")
    for name, entries in sorted(model["props"].items()):
        if name not in pp:
            continue
        base = wm.base_of(entries, name)
        mine = fold(wm.merged_grammar(base, entries)) if base else None
        theirs = fold(pp[name].get("syntax")) or None
        if mine != theirs:
            note("property-syntax", name, "pin %r, package %r" % (mine, theirs))

    # at-rules: the package lists the subsidiary at-rules (the margin boxes of @page, the value kinds of @font-feature-values) at the
    # root with a `for` naming the parent. The pin has the margin boxes as at-rules of their own and the font-feature-values kinds
    # as descriptors of their parent (descriptor kind 3, a nested at-rule): both readings are the same facts, so the package's
    # subsidiary at-rule counts as an at-rule and, where the pin nests it, as a descriptor of the parent.
    pa = {}
    pdesc = {}
    for a in pkg["atrules"]:
        pa[a["name"]] = a
        for d in a.get("descriptors", []):
            pdesc[(a["name"], d["name"])] = d
    for a in pkg["atrules"]:
        for parent in a.get("for") or []:
            if (parent, a["name"]) in model["descs"]:
                pdesc[(parent, a["name"])] = a
    for name in sorted(set(model["atrules"]) - set(pa)):
        note("at-rule", name, "in the pin, not in the package")
    for name in sorted(set(pa) - set(model["atrules"])):
        if any((parent, name) in model["descs"] for parent in pa[name].get("for") or []):
            continue  # the pin has it as a descriptor of its parent
        note("at-rule", name, "in the package, not in the pin")
    for key in sorted(set(model["descs"]) - set(pdesc)):
        note("descriptor", "%s %s" % key, "in the pin, not in the package")
    for key in sorted(set(pdesc) - set(model["descs"])):
        note("descriptor", "%s %s" % key, "in the package, not in the pin")
    for key, entries in sorted(model["descs"].items()):
        if key not in pdesc or key[1].startswith("@"):
            continue  # a subsidiary at-rule has no grammar of its own
        base = wm.base_of(entries, key)
        mine = fold(wm.merged_grammar(base, entries)) if base else None
        theirs = fold(pdesc[key].get("value") or pdesc[key].get("syntax")) or None
        if mine != theirs:
            note("descriptor-syntax", "%s %s" % key, "pin %r, package %r" % (mine, theirs))

    # types and functions: the package does not say which property a nested one belongs to, so the names are checked against every
    # scope of the pin and the syntax against the global ones
    ptypes = {}
    for t in list(pkg["types"]) + list(pkg["functions"]):
        ptypes.setdefault(wm.type_key(t["name"]), []).append(t)
    pin_names = {k[1] for k in model["types"]}
    for name in sorted(pin_names - set(ptypes)):
        note("type", name, "in the pin, not in the package")
    for name in sorted(set(ptypes) - pin_names):
        note("type", name, "in the package, not in the pin")
    for (scope, name), entries in sorted(model["types"].items()):
        if scope != "" or name not in ptypes:
            continue
        base = wm.base_of(entries, name)
        mine = fold(wm.merged_grammar(base, entries)) if base else None
        theirs = [fold(t.get("syntax")) for t in ptypes[name] if t.get("syntax")]
        theirs = theirs[0] if theirs else None
        if mine != theirs:
            note("type-syntax", name, "pin %r, package %r" % (mine, theirs))

    ps = {s["name"] for s in pkg["selectors"]}
    for name in sorted(set(model["selectors"]) - ps):
        note("selector", name, "in the pin, not in the package")
    for name in sorted(ps - set(model["selectors"])):
        note("selector", name, "in the package, not in the pin")
    return out


def read_divergences(path):
    rows = {}
    problems = []
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 3:
                problems.append("%s:%d: expected kind, name and reason" % (path, n))
                continue
            if len(cols[2].strip()) < 20:
                problems.append("%s:%d (%s %s): the reason is missing or too short to be one" % (path, n, cols[0], cols[1]))
            if (cols[0], cols[1]) in rows:
                problems.append("%s:%d: %s %s twice" % (path, n, cols[0], cols[1]))
            rows[(cols[0], cols[1])] = cols[2]
    return rows, problems


def main(argv):
    args = {"--package": os.path.join(ROOT, "third_party", "css", "webref-css", "package"),
            "--data": wm.DATA,
            "--divergences": os.path.join(HERE, "WEBREF_PACKAGE_DIVERGENCES")}
    i = 1
    while i < len(argv):
        if argv[i] not in args or i + 1 >= len(argv):
            sys.stderr.write(__doc__)
            return 2
        args[argv[i]] = argv[i + 1]
        i += 2
    css_json = os.path.join(args["--package"], "css.json")
    if not os.path.exists(css_json):
        if os.environ.get("CSS_ORACLE_OFFLINE") == "1":
            print("NOT RUN: the @webref/css package is not fetched and CSS_ORACLE_OFFLINE=1. This is not a pass.")
            return 3
        sys.stderr.write("the @webref/css package is not fetched (%s). Fetch it:  tools/css/fetch.sh\n"
                         "(CSS_ORACLE_OFFLINE=1 runs without it and reports NOT RUN, which is not a pass.)\n" % css_json)
        return 1
    with open(css_json, encoding="utf-8") as handle:
        pkg = json.load(handle)
    model = wm.load(args["--data"])
    found = divergences_of(model, pkg)
    listed, problems = read_divergences(args["--divergences"])
    bad = list(problems)
    for key in sorted(set(found) - set(listed)):
        bad.append("UNLISTED: %s %s: %s" % (key[0], key[1], found[key][:300]))
    for key in sorted(set(listed) - set(found)):
        bad.append("STALE: %s %s is listed and the two readings agree now" % key)
    counts = {"property": len(model["props"]), "at-rule": len(model["atrules"]), "descriptor": len(model["descs"]),
              "type": len({k[1] for k in model["types"]}), "selector": len(model["selectors"])}
    print("webref package: compared %d properties, %d at-rules, %d descriptors, %d types and functions, %d selectors; %d classified differences"
          % (counts["property"], counts["at-rule"], counts["descriptor"], counts["type"], counts["selector"], len(found)))
    for line in bad[:60]:
        print("  " + line)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
