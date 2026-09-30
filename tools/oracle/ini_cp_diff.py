#!/usr/bin/env python3
"""Compare this library's configparser reader against CPython's `configparser`.

    ini_cp_diff.py <ours>

INI_CP_ORACLE_COUNT and INI_CP_ORACLE_SEED size and seed the population; the reference
is reached through tools/oracle/oracle_env.py, which resolves and version-checks the
pinned image before anything is compared.

Five scores:

  `intent`     our accept/refuse against what tools/oracle/ini_cp_gen.py's REFUSES
               says the dialect means. **The only independent reading there is**, and
               it matters more here than for any other dialect in this module: this
               one has no specification, so without it a rule both the reference and
               this library got wrong would pass every other score. For git, systemd
               and EditorConfig a document can be checked against prose; here the
               generator's table is the prose.
  `verdict`    our accept/refuse against the reference's.
  `values`     every section, key and **joined** value, in document order, over the
               documents free of a construct either side is known to answer
               differently.
  `rewrite`    every accepted document writes back byte for byte. No reference.
  `divergence` per axis, that each known departure is **still observable**. Three of
               them, all one thing: `configparser` works on Python `str`, so its
               whitespace and its case folding are Unicode's and a byte-oriented
               reader cannot ask either question of a single byte. A change to the
               fold or the whitespace set fails this loudly rather than quietly
               inflating `values`.

**The reference is one implementation and it is also the specification.** That is the
weakest position any dialect in this module is in - weaker than git's lone reference,
because there at least `git-config(1)` exists to disagree with git. So two things
carry the weight instead: `intent`, above, and the local corpus gate
`make conformance-ini-configparser`, whose 703 real files include 224 the reference
refuses and so can score a refusal that no generated document chose.

**The channel is a file**, not a string, because Python's universal-newline
translation is applied to one and not the other and they disagree about a lone CR.
See tools/oracle/containers/configparser/driver.py.

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ini_cp_gen  # noqa: E402
import oracle_env  # noqa: E402

NAME = "configparser"

# The driver inside the image is this file's neighbour, and the repository is
# bind-mounted at its own path - so it is named the same way on both sides and there is
# no built image to keep in step with it. What pins the reference is the interpreter
# digest in containers/IMAGES plus this file, which is in the tree the gate ran from.
DRIVER = os.path.join(HERE, "containers", "configparser", "driver.py")


def parse_records(text, count, who):
    """Split a driver's output into one record per document."""
    records = []
    current = None
    for line in text.split("\n"):
        if line == "BEGIN":
            current = {"status": None, "body": [], "rewrite": None}
            continue
        if line == "END":
            if current is not None:
                records.append(current)
            current = None
            continue
        if current is None:
            continue
        if line.startswith("cp "):
            current["status"] = line[3:]
            continue
        if line.startswith("W "):
            current["rewrite"] = line[2:]
            continue
        if line:
            current["body"].append(line)
    if len(records) != count:
        sys.stderr.write("%s answered %d of %d documents\n"
                         % (who, len(records), count))
    return records


def status_codes():
    """The GTEXT_INI_Status names and their numeric values, read from the header.

    Read rather than duplicated, so that inserting a code cannot silently shift what
    this file compares against.
    """
    header = os.path.normpath(os.path.join(HERE, "..", "..", "include", "ghoti.io",
                                           "text", "ini", "ini_core.h"))
    codes = {}
    value = 0
    started = False
    for line in open(header):
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
            codes[stripped.split(",")[0].split()[0]] = value
            value += 1
    return codes


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__)
    ours = argv[1]
    count = int(os.environ.get("INI_CP_ORACLE_COUNT", "20000"))
    seed = int(os.environ.get("INI_CP_ORACLE_SEED", "20260929"))

    print("provenance: %s" % oracle_env.check_pin(NAME))
    print("population: %d generated documents, seed %d" % (count, seed))
    print("configuration: interpolation=None, strict=True, allow_no_value=False,")
    print("               inline_comment_prefixes=None, empty_lines_in_values=True,")
    print("               default_section=<unspellable>, read from a file")

    documents = list(ini_cp_gen.documents(seed, count))
    payload = bytearray()
    for data, _axes, _expected in documents:
        payload += (b"%d\n" % len(data)) + data
    payload += b"-1\n"

    mine_proc = subprocess.run([ours], input=bytes(payload), capture_output=True)
    if mine_proc.returncode != 0:
        print("FAIL: our runner exited %d" % mine_proc.returncode)
        print(mine_proc.stderr.decode("utf-8", "replace")[:3000])
        return 1
    mine = parse_records(mine_proc.stdout.decode("utf-8", "surrogateescape"),
        count, "our runner")
    if len(mine) != count:
        return 1

    command = oracle_env.command(NAME, ["python3", DRIVER])
    theirs_proc = subprocess.run(command, input=bytes(payload), capture_output=True)
    if theirs_proc.returncode != 0:
        print("FAIL: the reference driver exited %d" % theirs_proc.returncode)
        print(theirs_proc.stderr.decode("utf-8", "replace")[:3000])
        return 1
    theirs = parse_records(theirs_proc.stdout.decode("utf-8", "surrogateescape"),
        count, "the reference")
    if len(theirs) != count:
        return 1

    codes = status_codes()
    intent = [0, 0]
    verdict = [0, 0]
    values = [0, 0]
    rewrite = [0, 0]
    excluded = 0
    diverged = set()
    divergence = [0, 0]
    used = set()
    refusals = collections.Counter()
    failures = []

    for i, (data, axes, expected) in enumerate(documents):
        used |= axes
        ours_rec = mine[i]
        ref_rec = theirs[i]
        we_refused = ours_rec["status"].startswith("err")
        they_refused = ref_rec["status"].startswith("err")
        if they_refused:
            refusals[ref_rec["status"][4:]] += 1

        # --- intent: our verdict against the generator's own reading
        intent[1] += 1
        if expected is None:
            if we_refused:
                failures.append("doc %d [%s]: we refused what nothing should refuse "
                    "(%s)" % (i, ",".join(sorted(axes)), ours_rec["status"]))
            else:
                intent[0] += 1
        else:
            # The table names a status without its prefix, and the header declares
            # it with one. Looking it up by the short name gave `None` for every
            # entry, which compared unequal to every real code and scored 164 of 500
            # while every one of those documents was in fact right.
            want = codes.get("GTEXT_INI_" + expected)
            got = int(ours_rec["status"].split()[1]) if we_refused else 0
            if got == want:
                intent[0] += 1
            else:
                failures.append("doc %d [%s]: expected %s, got %s"
                    % (i, ",".join(sorted(axes)), expected, ours_rec["status"]))

        # --- the reference's channel cannot carry some documents at all
        if axes & set(ini_cp_gen.NO_ORACLE):
            excluded += 1
            continue

        # --- the known divergences, asserted rather than scored
        #
        # **Before the verdict comparison and not after it**, because a divergence can
        # show up as either answer and the table cannot say which in advance. A
        # no-break space at a value's edge is a value difference between two documents
        # both sides accept; a no-break space used as *indentation* is a verdict
        # difference, because the reference reads a continuation and this reader reads
        # an entry with no separator and refuses the document. The first version of
        # this loop checked for a divergence only among the documents both sides had
        # accepted, so `nbsp-indent` never reached the check at all: it failed the
        # verdict score as an ordinary disagreement, and the divergence score reported
        # the axis as never observed. Two symptoms, one cause, and the one that named
        # it was the "never observed" assertion - which is exactly what that assertion
        # is for.
        divergent = axes & set(ini_cp_gen.REFERENCE_DIVERGES)
        if divergent:
            divergence[1] += 1
            if we_refused != they_refused or ours_rec["body"] != ref_rec["body"]:
                divergence[0] += 1
                diverged |= divergent
            else:
                failures.append("doc %d [%s]: the reference no longer diverges here, "
                    "so this exclusion is now hiding an agreement rather than a "
                    "difference" % (i, ",".join(sorted(axes))))
            continue

        # --- verdict
        verdict[1] += 1
        if we_refused == they_refused:
            verdict[0] += 1
        else:
            failures.append("doc %d [%s]: we %s, the reference %s\n      input %r"
                % (i, ",".join(sorted(axes)),
                   "refused" if we_refused else "accepted",
                   "refused" if they_refused else "accepted", data[:200]))

        if we_refused or they_refused:
            continue

        # --- rewrite, which needs no reference
        rewrite[1] += 1
        if ours_rec["rewrite"] == data.hex() or (not data and
                ours_rec["rewrite"] == "."):
            rewrite[0] += 1
        else:
            failures.append("doc %d [%s]: the rewrite is not the input"
                % (i, ",".join(sorted(axes))))

        # --- values
        values[1] += 1
        if ours_rec["body"] == ref_rec["body"]:
            values[0] += 1
        else:
            first = next((n for n, (a, b) in enumerate(zip(ref_rec["body"],
                ours_rec["body"])) if a != b),
                min(len(ref_rec["body"]), len(ours_rec["body"])))
            body_ref = ref_rec["body"]
            body_our = ours_rec["body"]
            failures.append("doc %d [%s]: record %d differs\n      ref %s\n"
                "      our %s\n      input %r"
                % (i, ",".join(sorted(axes)), first,
                   body_ref[first] if first < len(body_ref) else "<missing>",
                   body_our[first] if first < len(body_our) else "<missing>",
                   data[:200]))

    for line in failures[:15]:
        print("FAIL " + line)
    print("intent      %6d of %6d  (our verdict against the generator's reading)"
          % tuple(intent))
    print("verdict     %6d of %6d  (our verdict against the reference's)"
          % tuple(verdict))
    print("values      %6d of %6d  (sections, keys and joined values)" % tuple(values))
    print("rewrite     %6d of %6d  (byte for byte)" % tuple(rewrite))
    print("divergence  %6d of %6d  (excluded, and the departure still visible)"
          % tuple(divergence))
    print("excluded    %6d          (the reference's channel cannot read them: %s)"
          % (excluded, ", ".join(ini_cp_gen.NO_ORACLE)))
    print("the reference refused %d documents:" % sum(refusals.values()))
    for kind, n in refusals.most_common():
        print("    %-30s %6d" % (kind, n))

    missing_axes = [a for a in ini_cp_gen.AXES if a not in used]
    print("axes        %6d of %6d" % (len(ini_cp_gen.AXES) - len(missing_axes),
                                      len(ini_cp_gen.AXES)))
    missing_div = [a for a in ini_cp_gen.REFERENCE_DIVERGES if a not in diverged]

    bad = bool(failures) or intent[0] != intent[1] or verdict[0] != verdict[1] \
        or values[0] != values[1] or rewrite[0] != rewrite[1] \
        or divergence[0] != divergence[1]
    if missing_axes:
        print("FAIL these axes never appeared, so the run says nothing about them: %s"
              % ", ".join(missing_axes))
        bad = True
    if missing_div:
        print("FAIL these divergences were never observed, so the reference may have "
              "been fixed and the exclusion is now dead weight: %s"
              % ", ".join(missing_div))
        bad = True
    if not excluded:
        print("FAIL nothing was excluded, so the exclusion table describes no "
              "document this run generated")
        bad = True
    print("FAIL" if bad else "PASS: every document agrees, or diverges for a reason "
          "this run proved is still there")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
