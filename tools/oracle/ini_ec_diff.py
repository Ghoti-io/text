#!/usr/bin/env python3
"""Compare this library's EditorConfig reader against both reference cores.

    ini_ec_diff.py <ours>

INI_EC_ORACLE_COUNT and INI_EC_ORACLE_SEED size and seed the population; the
references are reached through tools/oracle/oracle_env.py, which resolves and
version-checks the pinned image before anything is compared.

Six scores, and the split is the argument:

  `intent`         our accept/refuse against **specification 0.17.2**, as
                   tools/oracle/ini_ec_gen.py's REFUSES writes it down. This is
                   the score that matters most here, because neither reference is
                   right: both fail the normative conformance suite.
  `values-c`       our resolved properties against editorconfig-core-c, over
                   documents carrying no construct core-c is known to get wrong.
  `values-py`      the same against editorconfig-core-py.
  `divergence-c`   over the constructs core-c *is* known to get wrong, that it
                   still gets them wrong. Scored **per axis**, not per document:
                   at least one document must show the departure. A core fixed
                   upstream fails this loudly rather than quietly inflating
                   `values`.
  `divergence-py`  the same for core-py.
  `rewrite`        every accepted document writes back byte for byte. No
                   reference: a property.

**Two references that disagree is the good case**, and this pin is the second one
in the module after Desktop Entry's. But their agreement is worth less than
GKeyFile's and desktop-file-validate's, because the sharing here is not
incidental: both cores descend from Python's `ConfigParser`, and the two places
they agree while contradicting the specification - truncating a value at a
whitespace-preceded `#` or `;` - are exactly the inherited behaviour. Where they
disagree with *each other* the specification decides, and `intent` is how it gets
a vote.

A refusal on our side resolves to no properties, so `values` compares "what a
reader of this document sees". Whether the document *should* have been refused is
`intent`'s question, not a core's - a core's verdict is not an authority here.

Copyright 2026 by Corey Pennycuff
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ini_ec_gen  # noqa: E402
import oracle_env  # noqa: E402

NAME = "editorconfig"

def status_codes(header):
    """Map GTEXT_INI_E_* names to their values by reading the public header.

    Read rather than transcribed: a remembered constant is a transcription that
    nothing checks, and this repository has been bitten by one. A name the header
    does not have is an error here rather than a silent zero.
    """
    codes = {}
    value = 0
    started = False
    for line in open(header):
        stripped = line.strip()
        if stripped.startswith("GTEXT_INI_OK"):
            started = True
            codes["GTEXT_INI_OK"] = 0
            value = 1
            continue
        if not started:
            continue
        if stripped.startswith("}"):
            break
        if stripped.startswith("GTEXT_INI_E_"):
            name = stripped.split(",")[0].split()[0]
            codes[name] = value
            value += 1
    return codes


def unhex(token):
    if token == "-":
        return None
    if token == ".":
        return b""
    return bytes.fromhex(token)


def parse_stream(text):
    """Split a driver's output into one record per document."""
    records = []
    current = None
    for line in text.split("\n"):
        if line == "BEGIN":
            current = {"status": None, "values": [], "rewrite": None}
        elif line == "END":
            records.append(current)
            current = None
        elif current is None:
            continue
        elif line.startswith("ec "):
            current["status"] = line[3:]
        elif line.startswith("V "):
            _, key, value = line.split(" ", 2)
            current["values"].append((unhex(key), unhex(value)))
        elif line.startswith("W "):
            current["rewrite"] = unhex(line[2:])
    return records


def batch(documents):
    """The shared batch input: document, then query name, both length-framed."""
    blob = b""
    for data, query, _axes, _expected in documents:
        blob += b"%d\n" % len(data) + data
        name = query.encode("utf-8", "surrogateescape")
        blob += b"%d\n" % len(name) + name
    return blob + b"-1\n"


def run(command, blob, label):
    result = subprocess.run(command, input=blob, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write("%s failed (%d): %s\n"
                         % (label, result.returncode,
                            result.stderr.decode(errors="replace")[-400:]))
        raise SystemExit(1)
    return parse_stream(result.stdout.decode("utf-8", "surrogateescape"))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    ours = argv[1]
    count = int(os.environ.get("INI_EC_ORACLE_COUNT", "20000"))
    seed = int(os.environ.get("INI_EC_ORACLE_SEED", "20260929"))

    print("provenance: %s" % oracle_env.check_pin(NAME))
    print("population: %d generated documents, seed %d" % (count, seed))

    header = os.path.join(HERE, "..", "..", "include", "ghoti.io", "text",
                          "ini", "ini_core.h")
    codes = status_codes(os.path.normpath(header))
    for name in ("GTEXT_INI_E_BAD_LINE", "GTEXT_INI_E_BAD_KEY",
                 "GTEXT_INI_E_BAD_GROUP"):
        if name not in codes:
            sys.stderr.write("%s is not in %s; the enum changed\n"
                             % (name, header))
            return 2

    documents = list(ini_ec_gen.documents(seed, count))
    blob = batch(documents)
    mine = run([ours], blob, "ours")
    theirs_c = run(oracle_env.command(NAME, ["editorconfig-driver"]), blob,
                   "core-c")
    theirs_py = run(oracle_env.command(NAME, ["editorconfig-py-driver"]), blob,
                    "core-py")
    for label, records in (("ours", mine), ("core-c", theirs_c),
                           ("core-py", theirs_py)):
        if len(records) != len(documents):
            sys.stderr.write("%s answered %d of %d documents\n"
                             % (label, len(records), len(documents)))
            return 1

    intent_ok = intent_total = 0
    values = {"c": [0, 0], "py": [0, 0]}
    excluded = {"c": 0, "py": 0}
    diverged = {"c": set(), "py": set()}
    rewrite_ok = rewrite_total = 0
    used = set()
    failures = []

    for i, (data, query, axes, expected) in enumerate(documents):
        used |= axes
        ours_rec = mine[i]
        # ---- intent
        intent_total += 1
        if expected == "ok":
            want = "ok"
        else:
            want = "err %d" % codes["GTEXT_INI_" + expected]
        if ours_rec["status"] == want:
            intent_ok += 1
        elif len(failures) < 12:
            failures.append("intent #%d want %r got %r axes %s\n%r"
                            % (i, want, ours_rec["status"],
                               ",".join(sorted(axes)), data))

        # ---- rewrite: a property, over accepted documents only
        if ours_rec["status"] == "ok":
            rewrite_total += 1
            if ours_rec["rewrite"] == data:
                rewrite_ok += 1
            elif len(failures) < 12:
                failures.append("rewrite #%d\n  in  %r\n  out %r"
                                % (i, data, ours_rec["rewrite"]))

        # ---- the two references
        for tag, records, no_oracle, diverges in (
                ("c", theirs_c, ini_ec_gen.NO_C_ORACLE, ini_ec_gen.DIVERGES_C),
                ("py", theirs_py, ini_ec_gen.NO_PY_ORACLE,
                 ini_ec_gen.DIVERGES_PY)):
            record = records[i]
            if record["status"].startswith("fail") or \
                    any(a in no_oracle for a in axes):
                excluded[tag] += 1
                continue
            same = record["values"] == ours_rec["values"]
            divergent = [a for a in axes if a in diverges]
            if divergent:
                if not same:
                    diverged[tag].update(divergent)
                continue
            values[tag][1] += 1
            if same:
                values[tag][0] += 1
            elif len(failures) < 12:
                failures.append("values-%s #%d axes %s\n  ours %r\n  them %r\n"
                                "  %r" % (tag, i, ",".join(sorted(axes)),
                                          ours_rec["values"], record["values"],
                                          data))

    for line in failures:
        print(line)

    print("intent          %d / %d" % (intent_ok, intent_total))
    print("rewrite         %d / %d" % (rewrite_ok, rewrite_total))
    print("values-c        %d / %d   excluded %d"
          % (values["c"][0], values["c"][1], excluded["c"]))
    print("values-py       %d / %d   excluded %d"
          % (values["py"][0], values["py"][1], excluded["py"]))
    print("divergence-c    %d / %d axes still departing"
          % (len(diverged["c"]), len(ini_ec_gen.DIVERGES_C)))
    print("divergence-py   %d / %d axes still departing"
          % (len(diverged["py"]), len(ini_ec_gen.DIVERGES_PY)))

    bad = False
    missing_axes = [a for a in ini_ec_gen.AXES if a not in used]
    print("axes            %d / %d exercised"
          % (len(ini_ec_gen.AXES) - len(missing_axes), len(ini_ec_gen.AXES)))
    if missing_axes:
        # An axis nobody emitted is a blind spot, not a pass: the construct it
        # names appears in no corpus on this machine.
        print("FAIL axes never exercised: %s" % ",".join(missing_axes))
        bad = True
    for tag, table in (("c", ini_ec_gen.DIVERGES_C),
                       ("py", ini_ec_gen.DIVERGES_PY)):
        absent = sorted(set(table) - diverged[tag])
        if absent:
            # Either the core was fixed upstream, or the generator stopped
            # producing a document where the departure is visible. Both mean this
            # gate no longer says what it claims.
            print("FAIL core-%s no longer departs on: %s" % (tag, ",".join(absent)))
            for axis in absent:
                print("     expected: %s" % table[axis])
            bad = True
    if intent_ok != intent_total or rewrite_ok != rewrite_total:
        bad = True
    for tag in ("c", "py"):
        if values[tag][0] != values[tag][1]:
            bad = True
        if not values[tag][1]:
            print("FAIL values-%s scored nothing; the exclusions ate the run" % tag)
            bad = True
    print("FAIL" if bad else "OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
