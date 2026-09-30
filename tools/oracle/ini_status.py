#!/usr/bin/env python3
"""The GTEXT_INI_Status names and their numeric values, read from the header.

**Read rather than transcribed, and the reason is that the transcription went stale
twice without anything noticing.** Three of this module's differentials print a refusal
code by name, the numbers cross a process boundary, and each carried its own copy of the
enum. `GTEXT_INI_E_ENCODING` was added as 17 and both copies stopped at 16, so an
encoding refusal printed as `?` in a diagnostic - not a wrong verdict, but a report that
could not name the thing it was reporting, in exactly the gate whose job is to say what
happened. Then two interpolation codes were added and would have done the same.

A hand-kept mirror of an enum is [[two-passes-one-predicate]] with a process boundary in
the middle: nothing links the two, so nothing can fail when they drift. Reading the
header means inserting a code cannot silently shift what a comparison compares against
either, which is the worse version of the same problem.

Copyright 2026 by Corey Pennycuff
"""

import os

HEADER = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "include", "ghoti.io", "text", "ini", "ini_core.h"))


def codes(header=None):
    """`{"GTEXT_INI_E_OOM": 2, ...}`, in declaration order, with OK at 0."""
    out = {"GTEXT_INI_OK": 0}
    value = 0
    started = False
    for line in open(header or HEADER):
        stripped = line.strip()
        if stripped.startswith("GTEXT_INI_OK"):
            started = True
            value = 1
            continue
        if not started:
            continue
        if stripped.startswith("}"):
            break
        if stripped.startswith("GTEXT_INI_E_"):
            out[stripped.split(",")[0].split()[0]] = value
            value += 1
    return out


def names(header=None):
    """`{2: "E_OOM", ...}`, the spelling the differentials print."""
    return {v: (k[len("GTEXT_INI_"):] if k != "GTEXT_INI_OK" else "OK")
            for k, v in codes(header).items()}


if __name__ == "__main__":
    for number, name in sorted(names().items()):
        print("%3d %s" % (number, name))
