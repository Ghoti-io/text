#!/usr/bin/env python3
"""Score this library against the JSONPath Compliance Test Suite.

    tools/conformance/jsonpath_cts.py <suite dir> <runner>

The suite is one file, cts.json, holding 706 cases. Each is either a query that
must be refused (`invalid_selector`) or a query with a document and the node
list it must select - `result`, or `results` where more than one order is
acceptable because the specification leaves object member order to the
implementation.

Three outcomes are counted, not two. A query this library refuses as
*unsupported* is not attempted, and is reported separately rather than as a
pass or a failure. Scoring it as a pass where the case happens to be an
invalid_selector one would count an unimplemented feature as conformance;
scoring it as a failure would bury the cases that do work.

Environment:
    JPC_MIN         floor on the pass rate over attempted cases (default 100)
    JPC_VERBOSE     print every failing case

What is compared is both the node list - the suite's `result` or `results` - and
the normalized path of each result, its `result_paths`. Comparing only the values
would pass a query that selected the right nodes by the wrong route, and the
paths are what say which route: `$['a'][0]` is not `$['a'][1]` even where the two
hold equal values.
"""

import json
import os
import subprocess
import sys

# RFC 9485 makes ^ and $ ordinary characters. These two cases anchor them,
# which is what a naive wrap of the pattern in ^(?:...)$ does to a caret that
# happens to sit at the start. ghoti.io-regex refuses that reading. The cases
# stay named here so a change that starts agreeing with them is visible.
SUITE_DISAGREES = {
    "functions, match, explicit caret",
    "functions, match, explicit dollar",
}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    suite, runner = sys.argv[1], sys.argv[2]
    with open(os.path.join(suite, "cts.json"), encoding="utf-8") as f:
        cases = json.load(f)["tests"]

    attempted = passed = 0
    unsupported = 0
    disagrees = 0
    failures = []

    for case in cases:
        selector = case["selector"].encode("utf-8")
        document = (json.dumps(case["document"]).encode("utf-8")
                    if "document" in case else b"")
        # The selector goes through stdin length-prefixed rather than as an
        # argument: the suite has selectors containing a NUL, which argv cannot
        # carry.
        request = b"%d\n%s%s" % (len(selector), selector, document)
        try:
            out = subprocess.run([runner], input=request,
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 timeout=30).stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            attempted += 1
            failures.append((case["name"], "timed out"))
            continue

        lines = out.split("\n")
        line = lines[0]
        path_line = next((l for l in lines[1:] if l.startswith("PATHS ")), None)
        if case["name"] in SUITE_DISAGREES:
            # Neither subject in these two cases contains a literal ^ or $,
            # so the I-Regexp answer is an empty node list. Agreeing with the
            # suite means the anchors leaked back in.
            disagrees += 1
            got = None
            if line.startswith("OK "):
                try:
                    got = json.loads(line[3:])
                except json.JSONDecodeError:
                    got = None
            if got == case.get("result"):
                failures.append((case["name"],
                                 "now agrees with the suite; drop it from SUITE_DISAGREES"))
            elif got != []:
                failures.append((case["name"],
                                 "I-Regexp answer is []; got %s" % line[:80]))
            continue

        if line.startswith("UNSUPPORTED"):
            unsupported += 1
            continue

        attempted += 1
        if case.get("invalid_selector"):
            if line.startswith("INVALID"):
                passed += 1
            else:
                failures.append((case["name"], "accepted an invalid selector: " + line[:80]))
            continue

        if not line.startswith("OK "):
            failures.append((case["name"], "refused a valid selector: " + line[:80]))
            continue

        try:
            got = json.loads(line[3:])
        except json.JSONDecodeError as exc:
            failures.append((case["name"], "unreadable output (%s): %s" % (exc, line[:80])))
            continue

        # The normalized paths, where the runner printed them and the case says
        # what they should be. A result that holds the right values by the wrong
        # route is still wrong.
        got_paths = None
        if path_line is not None:
            try:
                got_paths = json.loads(path_line[len("PATHS "):])
            except json.JSONDecodeError as exc:
                failures.append((case["name"], "unreadable paths (%s)" % exc))
                continue

        if "results" in case:
            index = next((i for i, alternative in enumerate(case["results"])
                          if got == alternative), None)
            if index is None:
                failures.append((case["name"], "got %s, none of the %d accepted orders"
                                 % (json.dumps(got)[:60], len(case["results"]))))
            elif (got_paths is not None and "results_paths" in case
                  and got_paths != case["results_paths"][index]):
                failures.append((case["name"], "values match order %d, paths do not: %s"
                                 % (index, json.dumps(got_paths)[:60])))
            else:
                passed += 1
        elif got != case["result"]:
            failures.append((case["name"], "got %s want %s"
                             % (json.dumps(got)[:60], json.dumps(case["result"])[:60])))
        elif (got_paths is not None and "result_paths" in case
              and got_paths != case["result_paths"]):
            failures.append((case["name"], "paths: got %s want %s"
                             % (json.dumps(got_paths)[:60],
                                json.dumps(case["result_paths"])[:60])))
        else:
            passed += 1

    total = len(cases)
    rate = 100.0 * passed / attempted if attempted else 0.0
    if failures and os.environ.get("JPC_VERBOSE"):
        for name, why in failures:
            print("  FAIL %s: %s" % (name, why))
    elif failures:
        for name, why in failures[:10]:
            print("  FAIL %s: %s" % (name, why))
        if len(failures) > 10:
            print("  ... and %d more (JPC_VERBOSE=1 for all)" % (len(failures) - 10))

    print("JSONPath Compliance Test Suite: %d cases" % total)
    print("  attempted             %5d" % attempted)
    print("  passed                %5d  (%.1f%% of attempted, %.1f%% of the suite)"
          % (passed, rate, 100.0 * passed / total))
    print("  not attempted         %5d  (refused as unsupported)" % unsupported)
    print("  suite disagrees       %5d  (^ and $ are ordinary characters in I-Regexp;"
          % disagrees)
    print("                              the suite anchors them in match())")
    print("A percentage over attempted cases means nothing without those two counts.")
    print("Both the node list and the normalized path of each result are compared:")
    print("a result that holds the right values by the wrong route is a failure.")

    floor = float(os.environ.get("JPC_MIN", "100"))
    if rate + 1e-9 < floor:
        print("pass rate %.1f%% is below the floor of %.1f%%" % (rate, floor))
        return 1
    print("pass rate %.1f%% meets the floor of %.1f%%" % (rate, floor))
    return 0


if __name__ == "__main__":
    sys.exit(main())
