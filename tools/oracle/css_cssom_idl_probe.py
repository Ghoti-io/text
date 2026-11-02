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
"""Which members of the CSSOM interface ledger Chromium and Firefox have (C5e).

    css_cssom_idl_probe.py [--out FILE]      ask both browsers and write the answers (default: tools/oracle/css_cssom_idl_probe.json)
    css_cssom_idl_probe.py --check           require the recorded answers to cover exactly the members of tools/css/cssom_ledger.tsv

The browsers are the pinned ones (containers/cssprop, one driver, no network). The answers are recorded, not asked in `make test`: a ledger row whose status is `out` for a member
that a browser ships, or `implemented` for one that neither ships, is a statement the record can be read against. A dictionary key has no object to ask and is recorded as null.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(HERE))
LEDGER = os.path.join(ROOT, "tools", "css", "cssom_ledger.tsv")
RECORD = os.path.join(HERE, "css_cssom_idl_probe.json")
DRIVER = os.path.join(HERE, "containers", "cssprop", "cssom_driver.mjs")
ENGINES = ["chromium", "firefox"]


def rows():
    out = []
    for line in open(LEDGER, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        out.append((f[0], f[1], f[2]))
    return out


def ask(engine, items):
    import oracle_env
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s' % (DRIVER, engine)]
    doc = {"probe": [[i, m, "const" if k == "const" else ("key" if k == "key" else "member")] for i, m, k in items]}
    run = subprocess.run(oracle_env.command("cssprop", inner), input=(json.dumps(doc) + "\n").encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_cssom_idl_probe: the %s driver exited %d:\n%s" % (engine, run.returncode, run.stderr.decode("utf-8", "replace")[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    header = json.loads(lines[0][1:])
    answer = json.loads(lines[1])
    return header, answer["p"]


def main():
    items = rows()
    if "--check" in sys.argv:
        rec = json.load(open(RECORD))
        want = ["%s.%s" % (i, m) for i, m, _ in items]
        for e in ENGINES:
            if sorted(rec[e]) != sorted(want):
                print("css_cssom_idl_probe: the record of %s does not cover the ledger (%d recorded, %d rows)" % (e, len(rec[e]), len(want)))
                return 1
        print("css_cssom_idl_probe: the record covers the %d members of the ledger for both browsers" % len(want))
        return 0
    out = RECORD
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    rec = {"header": {}}
    for e in ENGINES:
        header, p = ask(e, items)
        rec["header"][e] = {"browser": header["browser"], "image": header["image"]}
        rec[e] = {"%s.%s" % (i, m): (None if k == "key" else v) for (i, m, k), v in zip(items, p)}
    json.dump(rec, open(out, "w"), indent=1, sort_keys=True)
    open(out, "a").write("\n")
    for e in ENGINES:
        have = sum(1 for v in rec[e].values() if v)
        print("%s %s: %d of %d members present" % (e, rec["header"][e]["browser"], have, len(items)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
