#!/usr/bin/env python3
"""Check that the hand-chosen tree-construction cases still say what both references say.

    html_tree_extra_check.py [--plant KIND]

tools/conformance/html_tree_extra.dat holds cases whose expected trees are NOT this library's: they
are what html5lib 1.1 and parse5 8.0.1 both answer, so that `make conformance-html-tree` can hold the
tree builder to them without a container. This asks both references again and fails if either no
longer gives the expected tree (the pin moved, or a case was edited by hand) or if they no longer
agree with each other. A case with a `#document-fragment` line is asked as a fragment in that
context. html5lib takes a context by HTML name only, so a case in an SVG or MathML context is
asked of parse5 alone, and html5lib must say it cannot (that is the check that it still cannot).

`--plant expectation` changes one expected tree and must fail; the controls of run-html-oracle.sh
require it.

Copyright 2026 by Corey Pennycuff
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "conformance"))

import html_ref  # noqa: E402
import html_tree_suite as suite  # noqa: E402

EXTRA = os.path.join(HERE, "..", "conformance", "html_tree_extra.dat")


def main(argv):
    plant = argv[argv.index("--plant") + 1] if "--plant" in argv else None
    vectors = suite.parse_dat(EXTRA, "html_tree_extra.dat")
    if len(vectors) < 50:
        sys.stderr.write("\033[0;31m### the extra cases have shrunk: %d ###\033[0m\n" % len(vectors))
        return 3
    expected = [v["expected"] for v in vectors]
    if plant == "expectation":
        expected[0] += "\n| <planted>"
    answers = html_ref.ask_all([
        html_ref.request(v["data"], v["scripting"] is True, v["context"] if v["fragment"] else None)
        for v in vectors])
    for name in html_ref.REFERENCES:
        for a in answers[name]:
            if "dump" in a:
                a["dump"] = html_ref.normalize_dump(a["dump"])
    bad = []
    for i, v in enumerate(vectors):
        foreign = v["fragment"] and (v["context"].startswith("svg ") or v["context"].startswith("math "))
        for name in html_ref.REFERENCES:
            got = answers[name][i]
            if foreign and name == "html5lib":
                if "error" not in got:
                    bad.append((v["id"], name + " now answers a foreign context", v["data"][:80]))
                continue
            if got.get("dump") != expected[i]:
                bad.append((v["id"], name, v["data"][:80]))
    print("extra cases: %d; both references reproduce each expected tree: %d" % (
        len(vectors), len(vectors) - len({b[0] for b in bad})))
    if bad:
        sys.stderr.write("\033[0;31m### Extra cases a reference does not reproduce ###\033[0m\n")
        for b in bad[:20]:
            sys.stderr.write("  %s  %s  %r\n" % b)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
