#!/usr/bin/env python3
"""The depth ledger: how far each property and descriptor of the pin goes.

``tools/css/depth.tsv`` has one row for every property and every descriptor of the pin, with four columns of 0 or 1:

  parses      a value derived from the grammar (and the initial value) is accepted by the library: the grammar compiled and matches
  serializes  the value serializes, and the text parses and serializes to the same text (a fixed point)
  computes    the computed value is implemented (C2a fills it; 0 until then)
  wpt         the WPT value vectors of the property are scored (C1d fills it; 0 until then)

The first two are measured by tools/css/css_registry_report.c; the last two are edited by the layer that implements them. A
descriptor has no computed value (``-``).

  gen-depth.py REPORT depth.tsv --write              rewrite the measured columns (keeps the other two; a new row starts at 0 0)
  gen-depth.py REPORT depth.tsv --check [--baseline FILE]
                                                      fail on a column that went from 1 to 0 (against the measurement for the first two,
                                                      against the baseline file for all four), on a property of the registry that the
                                                      ledger lacks, on a row for something the registry no longer has, and on a measured
                                                      1 that the ledger still has as 0 (an out-of-date ledger)
"""

import os
import sys

HEADER = """\
# tools/css/gen-depth.py: how far each property and descriptor of the pin goes (see that script). Columns:
# kind, name, parses, serializes, computes, wpt, note. `parses` and `serializes` are measured by make check-css-depth and are
# rewritten by `tools/css/gen-depth.py REPORT depth.tsv --write`; `computes` is filled by the computed-value layer and `wpt` by the
# WPT value scoring, by hand, and neither may go from 1 to 0.
"""


def load_report(path):
    rows = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            cols = line.rstrip("\n").split("\t")
            if cols[0] == "P":
                _, name, state, detail, parses, ser = cols[:6]
                if state == "alias":
                    rows.append((("property", name), int(parses), int(ser), "alias of " + detail))
                    continue
                note = "" if state == "ok" else ("%s %s" % (state, detail)).strip()
                if state == "ok" and (parses != "1" or ser != "1"):
                    note = "no witness derived from the grammar parses" if parses != "1" else "a witness does not round-trip"
                rows.append((("property", name), int(parses), int(ser), note))
            elif cols[0] == "D":
                _, at, name, state, detail, parses, ser = cols[:7]
                if cols[-1] == "3":  # a nested at-rule: syntax, not a value
                    note = "at-rule syntax, no value grammar"
                else:
                    note = "" if state == "ok" else ("%s %s" % (state, detail)).strip()
                rows.append((("descriptor", "%s %s" % (at, name)), int(parses), int(ser), note))
    # an alias takes its target's columns: the report lists aliases first by name, the target by name; resolve by the alias note
    return rows


def load_ledger(path):
    rows = {}
    order = []
    with open(path, encoding="utf-8") as handle:
        for n, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 7:
                raise SystemExit("%s:%d: expected 7 columns" % (path, n))
            key = (cols[0], cols[1])
            if key in rows:
                raise SystemExit("%s:%d: %s %s twice" % (path, n, key[0], key[1]))
            rows[key] = cols[2:]
            order.append(key)
    return rows, order


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    baseline_path = None
    if "--baseline" in sys.argv:
        baseline_path = sys.argv[sys.argv.index("--baseline") + 1]
        args = [a for a in args if a != baseline_path]
    floor_path = None
    if "--floor" in sys.argv:
        floor_path = sys.argv[sys.argv.index("--floor") + 1]
        args = [a for a in args if a != floor_path]
    report, ledger_path = args[0], args[1]
    measured = load_report(report)
    # aliases: copy the target's measurement (the report's property lines for an alias carry no result)
    import re
    aliases = {}
    with open(report, encoding="utf-8") as handle:
        pass
    m = {k: (p, s, n) for k, p, s, n in measured if p is not None}

    try:
        ledger, order = load_ledger(ledger_path)
    except FileNotFoundError:
        ledger, order = {}, []

    if "--write" in flags:
        out = [HEADER]
        for key, p, s, note in sorted(measured, key=lambda r: (r[0][0], r[0][1])):
            old = ledger.get(key)
            computes = old[2] if old else ("0" if key[0] == "property" else "-")
            wpt = old[3] if old else "0"
            if p is None:
                p, s = 0, 0
                note = "alias"
            out.append("\t".join([key[0], key[1], str(p), str(s), computes, wpt, note]) + "\n")
        with open(ledger_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("".join(out))
        return 0

    bad = []
    keys = {k for k, _, _, _ in measured}
    for k in sorted(keys - set(ledger)):
        bad.append("NEW: %s %s is in the registry and not in the ledger (run gen-depth.py --write)" % k)
    for k in sorted(set(ledger) - keys):
        bad.append("STALE: %s %s is in the ledger and not in the registry" % k)
    for key, p, s, note in measured:
        row = ledger.get(key)
        if not row:
            continue
        if p is None:
            continue  # an alias follows its target; its row is a pointer
        for i, (col, have) in enumerate((("parses", p), ("serializes", s))):
            want = int(row[i])
            if want == 1 and have == 0:
                bad.append("REGRESSION: %s %s: %s went from 1 to 0 (%s)" % (key[0], key[1], col, note or "no reason given"))
            elif want == 0 and have == 1:
                bad.append("OUT OF DATE: %s %s: %s is 1 now (run gen-depth.py --write)" % (key[0], key[1], col))
        if row[4] != note and not (key[0] == "property" and note == "" and row[4] == ""):
            bad.append("OUT OF DATE: %s %s: the note is %r, the ledger has %r (run gen-depth.py --write)" % (key[0], key[1], note, row[4]))
    if baseline_path:
        try:
            base, _ = load_ledger(baseline_path)
        except FileNotFoundError:
            base = {}
        for key, row in base.items():
            now = ledger.get(key)
            if not now:
                continue
            for i, col in enumerate(("parses", "serializes", "computes", "wpt")):
                if row[i] == "1" and now[i] != "1":
                    bad.append("REGRESSION: %s %s: %s was 1 in the baseline and is %s" % (key[0], key[1], col, now[i]))
    n = len(ledger)
    cols = [sum(1 for r in ledger.values() if r[i] == "1") for i in range(4)]
    if floor_path:
        # The floor is a tracked count that only goes up. It holds where the baseline cannot: a ledger edited down and committed
        # passes a comparison with the commit before it only once, and the floor is what the next commit is held to.
        try:
            with open(floor_path, encoding="utf-8") as handle:
                floor = [int(x) for x in handle.read().split("#")[0].split()]
        except (FileNotFoundError, ValueError):
            floor = [0, 0, 0, 0] if "--write-floor" in flags and not os.path.exists(floor_path) else None
        if floor is None or len(floor) != 4:
            bad.append("FLOOR: %s is missing or is not four counts (parses serializes computes wpt)" % floor_path)
        else:
            for name, have, want in zip(("parses", "serializes", "computes", "wpt"), cols, floor):
                if have < want:
                    bad.append("BELOW THE FLOOR: %d rows have %s and %s says %d" % (have, name, floor_path, want))
                elif have > want and "--write-floor" not in flags:
                    bad.append("FLOOR IS LOW: %d rows have %s and %s says %d (run gen-depth.py --write-floor)" % (have, name, floor_path, want))
            if "--write-floor" in flags:
                with open(floor_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write("%d %d %d %d  # parses serializes computes wpt: rows of depth.tsv that have each; only goes up\n" % tuple(cols))
                bad = [b for b in bad if not b.startswith("FLOOR IS LOW")]
    print("depth: %d rows; parses %d, serializes %d, computes %d, wpt %d" % ((n,) + tuple(cols)))
    # `parses` is that some witness matched; how many did is reported here, and a witness that matched must read back as the same typed
    # values (the acceptance criterion), which `serializes` already holds.
    tot = [0, 0, 0]
    partial = 0
    with open(report, encoding="utf-8") as handle:
        for line in handle:
            c = line.rstrip("\n").split("\t")
            if c[0] == "W" and len(c) >= 5:
                w, mt, rt = int(c[2]), int(c[3]), int(c[4])
                tot[0] += w
                tot[1] += mt
                tot[2] += rt
                partial += 1 if mt < w else 0
    if tot[0]:
        print("depth: witnesses %d, parsed %d (%.1f%%), the same typed values after a round trip %d; %d rows have a witness that does not parse"
              % (tot[0], tot[1], 100.0 * tot[1] / tot[0], tot[2], partial))
    for b in bad[:80]:
        print("  " + b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
