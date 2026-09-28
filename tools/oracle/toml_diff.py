#!/usr/bin/env python3
"""Compare this library's TOML reader against a pinned `tomllib`, over documents
nobody chose.

    toml_diff.py <runner> [--count N] [--seed N]

**Why generated documents.** The fuzzers check this library against itself - two
of its own readers, and its writer against its reader - and toml-test is the only
place a second implementation appears, holding the cases somebody chose. A
generator is the third thing. `tools/oracle/toml_gen.py` builds a value tree and
then spells it, so every document is valid v1.0.0 by construction and the
spelling varies independently of the value: the same value as a bare, quoted or
dotted key, as a `[header]`, a `[[array of tables]]` or an inline table, as a
basic, literal or multi-line string, with underscores in an integer and any of
the six offsets in a date-time.

Three readings, not two. The generator says what it meant, this library says what
it read, and the reference says what it read. The generator's claim is not a third
implementation - it is the test author - but it is what catches a generator that
emits something other than what it thinks, which is the failure that would
otherwise print as a finding. It did: a first version put an inline table's line
after a `[header]`, so both readers agreed with the text and disagreed with the
generator.

`tomllib` reads TOML 1.0.0 only, so the documents are 1.0.0.

Copyright 2026 by Corey Pennycuff
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "conformance"))

import json  # noqa: E402

import oracle_env  # noqa: E402
import toml_gen  # noqa: E402
import toml_reference  # noqa: E402
from toml_native import expected_native, same_native  # noqa: E402

GATE = "check-toml-oracle"


def ask_library(runner, text):
    """This library's reading, as toml-test tagged JSON, or an error string."""
    finished = subprocess.run([runner], input=text.encode("utf-8"),
                              capture_output=True, timeout=30)
    if finished.returncode != 0:
        detail = finished.stderr.decode("utf-8", "replace").strip()
        return ("err", detail.split("\n")[0] if detail
                else "exit %d" % finished.returncode)
    try:
        return ("ok", json.loads(finished.stdout.decode("utf-8")))
    except (ValueError, UnicodeDecodeError) as why:
        return ("err", "its output is not JSON: %s" % why)


def main(argv):
    runner = None
    count = int(os.environ.get("TOML_ORACLE_COUNT", "3000"))
    seed = int(os.environ.get("TOML_ORACLE_SEED", "1"))
    rest = list(argv[1:])
    while rest:
        arg = rest.pop(0)
        if arg == "--count":
            count = int(rest.pop(0))
        elif arg == "--seed":
            seed = int(rest.pop(0))
        elif runner is None:
            runner = arg
        else:
            sys.exit("usage: toml_diff.py <runner> [--count N] [--seed N]")
    if not runner or not os.path.exists(runner):
        sys.exit("toml_diff.py needs the built toml-test runner as its argument")

    cases = list(toml_gen.documents(seed, count))
    print("generated %d documents from seed %d" % (len(cases), seed))

    try:
        theirs = toml_reference.read_all([text for _s, text, _v in cases])
    except toml_reference.OracleUnavailable as why:
        return oracle_env.decline(GATE, why)

    intent_bad = []
    reader_bad = []
    refused = 0
    compared = 0
    for (case_seed, text, values), answer in zip(cases, theirs):
        kind, payload = answer
        if kind == "err":
            # The generator builds only valid v1.0.0, so a refusal by either
            # side is a finding rather than a case to skip - and a refusal by
            # *both* would still be one, because agreeing to reject a valid
            # document is not agreement about the document.
            refused += 1
            reader_bad.append((case_seed, "the reference refused it: %s"
                               % payload, text))
            continue
        reference = payload
        bad = same_native(reference, values)
        if bad:
            intent_bad.append((case_seed, bad, text))
            continue
        mine_kind, mine = ask_library(runner, text)
        if mine_kind == "err":
            reader_bad.append((case_seed, "this library refused it: %s" % mine,
                               text))
            continue
        try:
            mine_native = expected_native(mine)
        except ValueError as exc:
            reader_bad.append((case_seed, "its tagged JSON did not convert: %s"
                               % exc, text))
            continue
        bad = same_native(mine_native, reference)
        compared += 1
        if bad:
            reader_bad.append((case_seed, bad, text))

    print("  %d compared against the reference, %d refused by it" %
          (compared, refused))
    if intent_bad:
        print("\n  the generator and the reference disagree, which is the "
              "generator being wrong:")
        report(intent_bad)
    if reader_bad:
        print("\n  this library and the reference disagree:")
        report(reader_bad)
    if intent_bad or reader_bad:
        return 1
    if compared == 0:
        sys.stderr.write(
            "FAIL: nothing was compared, so this gate measured nothing\n")
        return 1
    print("\033[0;32mNo disagreements over %d generated documents.\033[0m"
          % compared)
    return 0


def report(rows):
    for case_seed, why, text in rows[:10]:
        print("    seed %d: %s" % (case_seed, why))
        for line in text.splitlines()[:12]:
            print("      | %s" % line)
    if len(rows) > 10:
        print("    ... and %d more" % (len(rows) - 10))
    print("    reproduce one with: TOML_ORACLE_SEED=<seed> "
          "TOML_ORACLE_COUNT=1 make check-toml-oracle")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
