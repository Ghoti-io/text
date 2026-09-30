#!/usr/bin/env python3
"""Compare this library's git config reader against git itself.

    ini_git_diff.py <ours-runner>

**One reference, and saying so is the point.** The Desktop Entry differential has
two, which disagree with each other, and that disagreement is what makes a clean
run mean something. git config has exactly one implementation: the same program
decides whether a document is legal and what its values are, so there is nothing
to cross-check it against and "we agree" cannot be distinguished from "we are
both wrong the same way".

What stands in for the second reference is the generator's own intent - a
statement of each rule taken from `git-config(1)` and from measurement, written
down before either program is asked. A rule this library and git both got wrong
would still pass `legality`; it would fail `intent`. That is weaker than a second
implementation and it is what there is.

Four comparisons, each with its own denominator printed:

  intent    - our verdict is what the generator meant to emit
  legality  - our accept/reject matches git's
  values    - for documents both accept: the same canonical name/value pairs,
              in order, with a valueless key distinct from an empty one
  lookup    - a single-value read answers the last occurrence, as `--get` does
  rewrite   - an accepted document writes back byte for byte

`lookup` is derived from git's own `--list` output rather than from a second
question put to git, and it was added after the fact: a mutation making
GTEXT_INI_DUPKEY_COLLECT answer the *first* occurrence instead of the last passed
every other score here untouched, because the value lines are produced by walking
the tree and never call the lookup. A score nothing can break is not a score.

**Exclusions are counted, never silent.** One cause, two axes: git reads a config
file with C string functions, so a NUL truncates whatever it is in - a value or a
subsection name - while this library is length-based and keeps the bytes. git's
answer there is not a function of the document, so those documents are excluded
from `values` and counted. They are still scored by `legality` and `intent`,
because both implementations do accept them.

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ini_git_gen  # noqa: E402
import ini_status  # noqa: E402
import oracle_env  # noqa: E402

NAME = "gitconfig"

# Our status codes, read from the header by tools/oracle/ini_status.py. This used to
# be a dict transcribed here, and it went stale twice without anything noticing:
# `GTEXT_INI_E_ENCODING` arrived as 17 while the copy stopped at 16, so an encoding
# refusal printed as `?` in the one place whose job is to say what happened.
CODES = ini_status.names()

#
# Axes on which git's answer is not a function of the document's bytes, with the
# reason printed in the report. Both have the same cause, and it is a deliberate
# deviation rather than a defect: refusing to store a NUL would make this library
# unable to represent a document git accepts.
#
NO_VALUE_ORACLE = {
    "nul-in-value":
        "git reads values as C strings and truncates at the NUL",
    "nul-in-subsection":
        "git reads names as C strings and truncates at the NUL",
}


def unhex(token):
    """Decode one protocol field: `-` absent, `.` empty, otherwise hex."""
    if token == "-":
        return None
    if token == ".":
        return b""
    return bytes.fromhex(token)


def parse_records(text, kind):
    """Split a driver's output into one dict per document."""
    records = []
    current = None
    for line in text.split("\n"):
        if line == "BEGIN":
            current = {"verdict": None, "code": None, "pairs": [],
                       "written": None, "lookups": []}
            continue
        if line == "END":
            records.append(current)
            current = None
            continue
        if current is None:
            continue
        parts = line.split(" ")
        if parts[0] in ("git", "ours"):
            current["verdict"] = parts[1]
            if parts[1] == "err":
                current["code"] = int(parts[2])
        elif parts[0] == "V":
            name = unhex(parts[1])
            raw = parts[2]
            if raw.startswith("!"):
                #
                # Our driver writes `!<code>` when the parse accepted a value the
                # decoder then refused. For this dialect that should be
                # impossible - the parser scanned the same bytes with the same
                # function - so it is carried through as a distinct value and
                # fails the comparison rather than being folded into "absent".
                #
                current["pairs"].append((name, ("decode-error", raw[1:])))
            else:
                current["pairs"].append((name, unhex(raw)))
        elif parts[0] == "L":
            name = unhex(parts[1])
            raw = parts[2]
            current.setdefault("lookups", []).append(
                (name, ("decode-error",) if raw == "!" else unhex(raw)))
        elif parts[0] == "W":
            current["written"] = unhex(parts[1])
    if current is not None:
        raise SystemExit("%s output ended inside a record" % kind)
    return records


def show(value):
    """A short, readable rendering of one protocol value."""
    if value is None:
        return "<no value>"
    if isinstance(value, tuple):
        return "<%s %s>" % value
    return repr(value)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    ours_runner = argv[1]
    count = int(os.environ.get("INI_GIT_ORACLE_COUNT", "20000"))
    seed = int(os.environ.get("INI_GIT_ORACLE_SEED", "20260929"))

    print("provenance: %s" % oracle_env.check_pin(NAME))
    print("population: %d generated documents, seed %d" % (count, seed))

    docs = list(ini_git_gen.documents(seed, count))
    used = ini_git_gen.documents.used
    missing = [a for a in ini_git_gen.AXES if a not in used]

    payload = bytearray()
    for data, _, _ in docs:
        payload += (b"%d\n" % len(data)) + data
    payload += b"-1\n"

    ours = subprocess.run([ours_runner], input=bytes(payload),
                          capture_output=True)
    if ours.returncode != 0:
        print("FAIL: our runner exited %d" % ours.returncode)
        print(ours.stderr.decode("utf-8", "replace")[:4000])
        return 1
    mine = parse_records(ours.stdout.decode("ascii"), "ours")

    command = oracle_env.command(NAME, ["git-config-driver"])
    theirs_proc = subprocess.run(command, input=bytes(payload),
                                 capture_output=True)
    if theirs_proc.returncode != 0:
        print("FAIL: the reference driver exited %d" % theirs_proc.returncode)
        print(theirs_proc.stderr.decode("utf-8", "replace")[:4000])
        return 1
    theirs = parse_records(theirs_proc.stdout.decode("ascii"), "reference")

    #
    # A count mismatch means one side lost or invented a record, and every ratio
    # below would be computed over the wrong denominator. Refuse rather than
    # score.
    #
    if not (len(mine) == len(theirs) == len(docs)):
        print("FAIL: %d documents, %d of our records, %d reference records"
              % (len(docs), len(mine), len(theirs)))
        return 1

    tally = collections.Counter()
    axis_tally = collections.Counter()
    axis_excluded = collections.Counter()
    failures = []

    def note(kind, index, detail):
        tally[kind + "-differ"] += 1
        if len(failures) < 20:
            axes = ",".join(sorted(docs[index][1]))
            failures.append("%s: doc %d [%s]\n    %s"
                            % (kind, index, axes, detail))

    for i, (data, axes, expected) in enumerate(docs):
        axis_tally.update(axes)
        us = mine[i]
        them = theirs[i]
        our_code = CODES.get(us["code"], "?") if us["verdict"] == "err" else "OK"

        # 1. Intent. The generator said what it meant; we either agree or one of
        #    the two is wrong, and either is a finding.
        tally["intent"] += 1
        if expected == "ok":
            if us["verdict"] != "ok":
                note("intent", i, "expected ok, we said %s" % our_code)
            else:
                tally["intent-same"] += 1
        elif our_code != expected:
            note("intent", i, "expected %s, we said %s" % (expected, our_code))
        else:
            tally["intent-same"] += 1

        # 2. Legality, against git. Nothing is excluded here: git either read the
        #    file or it did not, and that verdict is a function of the bytes even
        #    where the values it then reports are not.
        tally["legality"] += 1
        if (us["verdict"] == "ok") != (them["verdict"] == "ok"):
            note("legality", i, "git said %s, we said %s"
                 % (them["verdict"], our_code))
        else:
            tally["legality-same"] += 1

        if us["verdict"] != "ok" or them["verdict"] != "ok":
            continue

        # 3. Values: the same canonical names and values, in the same order.
        blind = [a for a in NO_VALUE_ORACLE if a in axes]
        if blind:
            tally["values-excluded"] += 1
            axis_excluded.update(blind)
        else:
            tally["values"] += 1
            if us["pairs"] != them["pairs"]:
                ours_p = [(n, show(v)) for n, v in us["pairs"]]
                their_p = [(n, show(v)) for n, v in them["pairs"]]
                #
                # Print the first differing position rather than both whole
                # lists: a document with four entries and one wrong value is
                # otherwise eight fields of noise around the finding.
                #
                detail = "ours %r vs git %r" % (ours_p, their_p)
                for a, b in zip(ours_p, their_p):
                    if a != b:
                        detail = "at %r: ours %s, git %s" % (a[0], a[1], b[1])
                        break
                else:
                    if len(ours_p) != len(their_p):
                        detail = ("%d pairs vs git's %d: ours %r, git %r"
                                  % (len(ours_p), len(their_p), ours_p,
                                     their_p))
                note("values", i, detail)
            else:
                tally["values-same"] += 1

        # 4. The single-value lookup. Derived from git's own `--list` order
        #    rather than by asking git again: `git config --get` answers the last
        #    occurrence - measured - so the expected answer for each name is the
        #    last value `--list` printed for it, and the expected order is
        #    first appearance.
        #
        #    This score exists because a mutation making COLLECT answer the first
        #    occurrence instead of the last passed every other comparison here:
        #    the value lines are produced by walking the tree and never call the
        #    lookup at all.
        expected_lookups = []
        last = {}
        for name, value in them["pairs"]:
            if name not in last:
                expected_lookups.append(name)
            last[name] = value
        want = [(name, last[name]) for name in expected_lookups]
        if blind:
            tally["lookup-excluded"] += 1
        else:
            tally["lookup"] += 1
            if us["lookups"] != want:
                detail = "ours %r vs %r" % (us["lookups"], want)
                for a, b in zip(us["lookups"], want):
                    if a != b:
                        detail = ("at %r: ours %s, last git listed %s"
                                  % (a[0], show(a[1]), show(b[1])))
                        break
                note("lookup", i, detail)
            else:
                tally["lookup-same"] += 1

        # 5. The rewrite. Free over this population, and the property most easily
        #    broken by a change to the tree - an entry keeps its line in pieces,
        #    and any of them going missing shows up here. Not excluded for a NUL:
        #    the bytes go back out whatever git makes of them.
        tally["rewrite"] += 1
        if us["written"] != data:
            note("rewrite", i, "wrote %r, input was %r"
                 % (us["written"], data))
        else:
            tally["rewrite-same"] += 1

    print()
    rows = (("intent", "our verdict is what the generator meant"),
            ("legality", "our accept/reject matches git"),
            ("values", "canonical names and values match git config --list"),
            ("lookup", "a single-value read answers the last, as --get does"),
            ("rewrite", "an accepted document writes back byte for byte"))
    ok = True
    for kind, what in rows:
        total = tally[kind]
        same = tally[kind + "-same"]
        excluded = tally[kind + "-excluded"]
        extra = ("; %d excluded" % excluded) if excluded else ""
        print("%-9s %6d of %6d  %s%s" % (kind, same, total, what, extra))
        if total == 0:
            print("  FAIL: nothing was compared, so this score is vacuous")
            ok = False
        if same != total:
            ok = False

    if axis_excluded:
        print()
        print("values exclusions, by axis - git's answer is not a function of "
              "the bytes here, and @ref format_ini says why:")
        for axis in sorted(axis_excluded):
            print("  %-22s %6d  %s" % (axis, axis_excluded[axis],
                                       NO_VALUE_ORACLE[axis]))

    print()
    print("axes: %d of %d exercised" % (len(ini_git_gen.AXES) - len(missing),
                                        len(ini_git_gen.AXES)))
    if missing:
        print("  FAIL: never generated: %s" % ", ".join(missing))
        ok = False
    thin = [a for a in ini_git_gen.AXES if 0 < axis_tally[a] < 5]
    if thin:
        print("  thin (fewer than 5 documents): %s"
              % ", ".join("%s=%d" % (a, axis_tally[a]) for a in thin))

    for line in failures:
        print("  " + line)

    if not ok:
        print("\nFAIL")
        return 1
    print("\nPASS: %d documents, every axis exercised" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
