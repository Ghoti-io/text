#!/usr/bin/env python3
"""The every-grammar-compiles gate.

Reads the report of tools/css/css_registry_report.c (what became of every grammar of the pin, whether the initial values parse) and
tools/css/UNRESOLVED_REFERENCES (the gaps of the pin, each with a reason), and fails on:

* a grammar that does not compile and is not explained by a listed entry (UNLISTED), and a listed entry that is no longer true
  (STALE): the gap closed, the text changed, or the pin was raised;
* a reference nothing resolves that is not listed, and a listed one that now resolves;
* an initial value that does not parse as a value of its property and is not listed, and a listed one that now parses;
* an entry with no reason, a floor not reached.

The file's columns (tab separated; '#' starts a comment):

  reference  <name>        the first referrer ("kind name")   the referrer's grammar    reason
  grammar    kind name     -                                  the grammar's text        reason
  initial    kind name     -                                  the initial value text    reason

A `reference` is a `<type>` or `<'property'>` that no definition of the pin, no native type of the grammar engine and no function type of
the library answers. A `grammar` is a property or type whose text is not a Value Definition Syntax definition, or a property the
pin names and gives no grammar. An `initial` is a property or descriptor whose initial value is prose or does not match its own grammar.
A shorthand whose initial value is "see individual properties" is not listed: its initial value is its longhands'.

Usage:  check-grammars.py REPORT UNRESOLVED_REFERENCES [--floors P,D,T] [--quiet]
"""

import os
import sys

PROP_SHORTHAND = 0x01


def load_report(path):
    rep = {"P": [], "D": [], "T": [], "R": [], "G": []}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            cols = line.rstrip("\n").split("\t")
            if cols[0] in rep:
                rep[cols[0]].append(cols)
    return rep


def load_list(path):
    entries = []
    problems = []
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 5 or cols[0] not in ("reference", "grammar", "initial"):
                problems.append("line %d: expected kind, name, referrer, text, reason" % n)
                continue
            if len(cols[4].strip()) < 20:
                problems.append("line %d (%s %s): the reason is missing or too short to be one" % (n, cols[0], cols[1]))
            entries.append((n,) + tuple(cols))
    return entries, problems


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    floors = None
    quiet = "--quiet" in sys.argv
    if "--floors" in sys.argv:
        floors = [int(x) for x in sys.argv[sys.argv.index("--floors") + 1].split(",")]
        args = [a for a in args if a != sys.argv[sys.argv.index("--floors") + 1]]
    rep = load_report(args[0])
    entries, problems = load_list(args[1])

    unlisted, stale = [], []

    listed = {}
    for e in entries:
        key = (e[1], e[2])
        if key in listed:
            problems.append("line %d: %s %s is listed twice" % (e[0], e[1], e[2]))
        listed[key] = e

    used = set()

    def want(kind, name, referrer, text, what):
        """Check one thing that must be listed; return True if it is."""
        key = (kind, name)
        e = listed.get(key)
        if e is None:
            unlisted.append("%s %s: %s" % (kind, name, what))
            return False
        used.add(key)
        if e[4] != text:
            stale.append("line %d: %s %s: the text the entry quotes is not the pin's now\n    listed: %s\n    pin:    %s" % (e[0], kind, name, e[4], text))
        if kind == "reference" and e[3] != referrer:
            stale.append("line %d: %s %s: first referrer is now %r, not %r" % (e[0], kind, name, referrer, e[3]))
        return True

    # --- references nothing answers ---
    missing_names = set()
    for _, ref, rkind, rname, grammar in rep["R"]:
        missing_names.add(ref)
        want("reference", ref, "%s %s" % (rkind, rname), grammar, "no definition answers it")

    # --- grammars that are not definitions at all ---
    failed_types = set()
    for _, key, text in rep["G"]:
        want("grammar", key, "", text, "does not compile and nothing says why")
        if key.startswith("type "):
            failed_types.add(key.split(" ", 1)[1])

    # --- every non-ok state is explained by a listed gap ---
    def explained(detail):
        if detail.startswith("<") and detail in missing_names:
            return True
        if detail in failed_types or (detail.startswith("<") and detail[1:-1] in failed_types):
            return True
        return False

    for cols in rep["P"]:
        _, name, state, detail = cols[:4]
        if state in ("unresolved", "failed") and "@" not in detail and not explained(detail):
            unlisted.append("property %s: %s on %r, which no listed gap explains" % (name, state, detail))
    for cols in rep["D"]:
        _, at, name, state, detail = cols[:5]
        if state in ("unresolved", "failed") and "@" not in detail and not explained(detail):
            unlisted.append("descriptor %s %s: %s on %r, which no listed gap explains" % (at, name, state, detail))
    for cols in rep["T"]:
        _, name, scope, state, detail = cols[:5]
        if state in ("unresolved", "failed") and "@" not in detail and not explained(detail):
            unlisted.append("type %s: %s on %r, which no listed gap explains" % (name, state, detail))

    # --- initial values ---
    for cols in rep["P"]:
        _, name, state, detail, parses, ser, wit, init, text, flags = cols[:10]
        if init in ("alias", "skipped") or state != "ok":
            continue
        shorthand = int(flags) & PROP_SHORTHAND
        if shorthand and text.strip().lower() == "see individual properties":
            continue
        if init == "ok":
            continue
        why = "no initial value" if init == "none" else "the initial value %r is %s" % (text, init)
        want("initial", "property " + name, "", text, why)
    for cols in rep["D"]:
        _, at, name, state, detail, parses, ser, wit, init, text, kind = cols[:11]
        if init in ("n/a", "skipped", "ok"):
            continue
        if init == "none" and not text:
            continue  # a media feature or descriptor the pin gives no initial value for: nothing to check
        want("initial", "descriptor %s %s" % (at, name), "", text, "the initial value %r is %s" % (text, init))

    # --- a listed entry that nothing needed ---
    for key, e in listed.items():
        if key not in used:
            stale.append("line %d: %s %s: nothing needs this entry any more" % (e[0], key[0], key[1]))

    # --- floors ---
    pok = sum(1 for c in rep["P"] if c[2] == "ok")
    dok = sum(1 for c in rep["D"] if c[3] == "ok")
    tok = sum(1 for c in rep["T"] if c[3] == "ok")
    if floors:
        for what, have, floor in (("properties", pok, floors[0]), ("descriptors", dok, floors[1]), ("types and functions", tok, floors[2])):
            if have < floor:
                unlisted.append("below the floor: %d %s compile, the floor is %d" % (have, what, floor))

    if not quiet:
        print("grammars: %d properties compile of %d, %d descriptors of %d, %d types and functions of %d; %d listed gaps, %d references"
              % (pok, sum(1 for c in rep["P"] if c[2] != "alias"), dok, len(rep["D"]), tok, len(rep["T"]), len(entries), len(rep["R"])))
    bad = False
    if problems:
        print("LIST PROBLEMS")
        for p in problems:
            print("  " + p)
        bad = True
    if unlisted:
        print("UNLISTED (%d)" % len(unlisted))
        for u in unlisted[:60]:
            print("  " + u)
        bad = True
    if stale:
        print("STALE ENTRIES (%d)" % len(stale))
        for s in stale[:60]:
            print("  " + s)
        bad = True
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
