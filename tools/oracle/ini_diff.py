#!/usr/bin/env python3
"""Compare this library's Desktop Entry reader against both references.

    ini_diff.py <ours-runner>

**Three readings, and the third is free.** GLib's `GKeyFile` says what a value
*is*; `desktop-file-validate` says whether a document is *legal*; and the
generator knows what it emitted. The two references disagree with each other
often enough that either alone would score clean while violating the
specification - GKeyFile silently merges the duplicate groups section 3.2
forbids - so the comparison is against both, and the generator's intent
separates "the subject is wrong" from "the generator emitted something other
than what it thinks".

Four comparisons, each with its own denominator printed:

  intent    - our verdict is what the generator meant to emit
  legality  - our accept/reject matches the validator's syntax verdict
  values    - for documents both accept: same groups, keys and raw values
  strings   - our gtext_ini_unescape() agrees with g_key_file_get_string()

**Exclusions are counted, never silent.** Two axes have no oracle at all: a
document with an unknown escape or a trailing lone backslash is parsed by
GKeyFile and refused only at `get_string()`, while the validator accepts it
outright - so nothing outside this repository decides it, and it is excluded
from `strings` and counted. A NUL in a value is excluded from `values`, because
GKeyFile's strings are NUL-terminated and it truncates there by construction.

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ini_gen  # noqa: E402
import ini_status  # noqa: E402
import oracle_env  # noqa: E402

NAME = "inidesktop"

# Our status codes, read from the header by tools/oracle/ini_status.py. This used to
# be a dict transcribed here, and it went stale twice without anything noticing:
# `GTEXT_INI_E_ENCODING` arrived as 17 while the copy stopped at 16, so an encoding
# refusal printed as `?` in the one place whose job is to say what happened.
CODES = ini_status.names()

#
# The validator's diagnostics that are *syntax* refusals, by the substring that
# identifies each. This table is the whole basis of the `legality` comparison and
# is deliberately explicit: a diagnostic matching none of these makes the
# document EXCLUDED and counted, rather than being read as agreement. An
# unclassified message must never pass silently - that is how a validator upgrade
# would quietly turn a comparison into a tautology.
#
SYNTAX_DIAGNOSTICS = (
    "which is not a comment, a group or an entry",
    "but only comments are accepted before the first group",
    "multiple groups may not have the same name",
    "multiple keys named",
    "key names must contain only",
    "is a localized key, but there is no non-localized key",
    #
    # One message, two axes that go opposite ways: an indented comment or entry
    # is refused here and the validator says this, which is agreement; a
    # whitespace-only line draws the *same* message and is accepted here, which
    # is not. The message cannot tell them apart and does not have to - a
    # whitespace-only document is excluded by VALIDATOR_STRICTER before
    # classification runs, so by the time this table is consulted the only
    # remaining sender is an indented line.
    #
    "starts with a space",
    "contains no groups",
    "first group is not",
    "invalid characters",
)

#
# The four axes on which **the validator is stricter than both GKeyFile and this
# library**, with what it says and why this module does not follow it. A document
# carrying one is excluded from `legality` and counted, because scoring it either
# way would be wrong: as a failure it would assert a rule this module has decided
# not to implement, and as a pass it would hide a real difference.
#
# This table is the finding the legality comparison produced on its first run. In
# every one of the four the validator's own message ends "The validation will
# continue", so it is a diagnostic about a document it can still read rather than
# a refusal - which is why GKeyFile reads them too, and why following the
# validator here would mean refusing documents both references accept.
#
VALIDATOR_STRICTER = {
    "whitespace-only-line": (
        "starts with a space",
        "section 3.1 makes a blank line a comment and a line of spaces carries "
        "no data; GKeyFile accepts it, so refusing it would refuse documents "
        "both references read"),
    "header-trailing-space": (
        "ends with a space, but looks like a group",
        "section 3.2 gives the header as [name]; GKeyFile accepts trailing "
        "whitespace after the ], and so does this module"),
    "cr-inside-value": (
        "ending with a carriage return",
        "section 3 separates lines by linefeed, so a CR that is not before an LF "
        "is data; the validator refuses a CR anywhere in the file"),
    "invalid-utf8-value": (
        "contains invalid UTF-8",
        "neither the parser nor GKeyFile validates UTF-8 at parse time; this "
        "module reports it from gtext_ini_unescape(), which the `strings` "
        "comparison scores instead"),
}

# Diagnostics that are about the *content* of a desktop entry rather than its
# syntax - a missing Type, an unregistered menu category - and so say nothing
# about the grammar. Named so that they are skipped deliberately rather than by
# falling through a pattern.
SEMANTIC_DIAGNOSTICS = (
    "required key",
    "category",
    "deprecated",
    "does not look like an absolute path",
    "looks the same as that of key",
    "value is not a valid",
    "can be extended with another",
    "is not a registered",
    "must be one of",
    "hint:",
    "warning: value",
    "not UTF-8 encoded",
    "boolean values in this file",
    "is deprecated",
    "improper",
    "unregistered",
    "is not in the list",
    "should not",
    "extension",
)


def classify(diagnostics):
    """(syntax refusal?, unclassified messages) for one validator run."""
    syntax = False
    unknown = []
    for line in diagnostics:
        body = line.split(": ", 1)[-1] if ": " in line else line
        low = body.lower()
        if any(m in low for m in (d.lower() for d in SYNTAX_DIAGNOSTICS)):
            syntax = True
        elif any(m in low for m in (d.lower() for d in SEMANTIC_DIAGNOSTICS)):
            continue
        else:
            unknown.append(line)
    return syntax, unknown


def unhex(token):
    """The inverse of the drivers' put_hex: `-` absent, `.` empty, else hex."""
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
            current = {"verdict": None, "code": None, "groups": [],
                       "written": None}
            continue
        if line == "END":
            records.append(current)
            current = None
            continue
        if current is None:
            continue
        parts = line.split(" ")
        if parts[0] in ("gk", "ours"):
            current["verdict"] = parts[1]
            if parts[1] == "err":
                current["code"] = int(parts[2])
        elif parts[0] == "G":
            current["groups"].append((unhex(parts[1]), []))
        elif parts[0] == "K" and current["groups"]:
            current["groups"][-1][1].append(
                (unhex(parts[1]), unhex(parts[2]), parts[3], unhex(parts[4])))
        elif parts[0] == "W":
            current["written"] = unhex(parts[1])
        elif parts[0] == "dfv":
            current["dfv_code"] = int(parts[1])
            current["dfv"] = []
        elif parts[0] == "D":
            current.setdefault("dfv", []).append(
                unhex(parts[1]).decode("utf-8", "replace"))
    if current is not None:
        raise SystemExit("%s output ended inside a record" % kind)
    return records


def flatten(record):
    """Groups, keys and raw values, for comparison."""
    return [(name, [(k, v) for k, v, _, _ in entries])
            for name, entries in record["groups"]]


def strings(record):
    """Key -> (status, decoded) for every entry, for the string comparison."""
    out = {}
    for name, entries in record["groups"]:
        for key, _, status, decoded in entries:
            out[(name, key)] = (status, decoded)
    return out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    ours_runner = argv[1]
    count = int(os.environ.get("INI_ORACLE_COUNT", "20000"))
    seed = int(os.environ.get("INI_ORACLE_SEED", "20260928"))

    print("provenance: %s" % oracle_env.check_pin(NAME))
    print("population: %d generated documents, seed %d" % (count, seed))

    docs = list(ini_gen.documents(seed, count))
    used = ini_gen.documents.used
    missing = [a for a in ini_gen.AXES if a not in used]

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

    command = oracle_env.command(NAME, ["ini-driver"])
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
        else:
            if our_code != expected:
                note("intent", i, "expected %s, we said %s"
                     % (expected, our_code))
            else:
                tally["intent-same"] += 1

        # 2. Legality, against the validator. Two kinds of exclusion, both
        #    counted: an axis where the validator is knowingly stricter, and an
        #    unclassified diagnostic - the second must never read as agreement.
        syntax_refusal, unknown = classify(them.get("dfv", []))
        stricter = [a for a in VALIDATOR_STRICTER if a in axes]
        if stricter:
            tally["legality-excluded"] += 1
            axis_excluded.update(stricter)
        elif unknown:
            tally["legality-excluded"] += 1
            if len(failures) < 20 and tally["legality-excluded"] <= 3:
                failures.append("legality-excluded: doc %d unclassified "
                                "validator diagnostic:\n    %s"
                                % (i, unknown[0]))
        else:
            tally["legality"] += 1
            if (us["verdict"] != "ok") == syntax_refusal:
                tally["legality-same"] += 1
            else:
                note("legality", i, "we %s, validator syntax refusal %s (%s)"
                     % ("refused" if us["verdict"] != "ok" else "accepted",
                        syntax_refusal, them.get("dfv", ["none"])[:1]))

        # 3 and 4 need both sides to have produced a tree.
        if us["verdict"] != "ok" or them["verdict"] != "ok":
            if us["verdict"] == "ok" and them["verdict"] != "ok":
                # GKeyFile is the permissive one, so this direction is a
                # finding: we accepted something even it refused.
                note("values", i, "we accepted, GKeyFile refused (code %s)"
                     % them["code"])
            continue

        if any(a in axes for a in ini_gen.NO_VALUE_ORACLE):
            tally["values-excluded"] += 1
        else:
            tally["values"] += 1
            if flatten(us) == flatten(them):
                tally["values-same"] += 1
            else:
                note("values", i, "tree differs\n    ours: %r\n    ref:  %r"
                     % (flatten(us)[:2], flatten(them)[:2]))

        if any(a in axes for a in ini_gen.NO_STRING_ORACLE):
            tally["strings-excluded"] += 1
        elif any(a in axes for a in ini_gen.NO_VALUE_ORACLE):
            tally["strings-excluded"] += 1
        else:
            ours_s = strings(us)
            their_s = strings(them)
            tally["strings"] += 1
            if ours_s.keys() != their_s.keys():
                note("strings", i, "different key sets")
            else:
                bad = [k for k in ours_s
                       if ours_s[k][0] != their_s[k][0]
                       or (ours_s[k][0] == "ok"
                           and ours_s[k][1] != their_s[k][1])]
                if bad:
                    k = bad[0]
                    note("strings", i, "%r: ours %r, ref %r"
                         % (k, ours_s[k], their_s[k]))
                else:
                    tally["strings-same"] += 1

    print()
    rows = (("intent", "our verdict is what the generator meant"),
            ("legality", "our accept/reject matches the validator"),
            ("values", "groups, keys and raw values match GKeyFile"),
            ("strings", "our unescape matches g_key_file_get_string"))
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
        print("legality exclusions, by axis - the validator is stricter here, "
              "and containers/IMAGES and @ref format_ini say why:")
        for axis in sorted(axis_excluded):
            print("  %-22s %6d  %s" % (axis, axis_excluded[axis],
                                       VALIDATOR_STRICTER[axis][0]))

    print()
    print("axes: %d of %d exercised" % (len(ini_gen.AXES) - len(missing),
                                        len(ini_gen.AXES)))
    if missing:
        print("  FAIL: never generated: %s" % ", ".join(missing))
        ok = False
    thin = [a for a in ini_gen.AXES if 0 < axis_tally[a] < 5]
    if thin:
        print("  thin (fewer than 5 documents): %s"
              % ", ".join("%s=%d" % (a, axis_tally[a]) for a in thin))

    for line in failures:
        print("  " + line)

    if not ok:
        print("\nFAIL")
        return 1
    print("\nPASS: %d documents, both references, every axis exercised" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
