#!/usr/bin/env python3
"""Compare this library's NFC against a pinned CPython's, over everything.

The normalisation is ghoti.io-unicode's, reached through the thin adapter in
src/idna/nfc.c. It can be wrong in ways nothing else here notices: a
composition exclusion missed, the canonical ordering made unstable, the "not
blocked from the starter" test written as a plain comparison - each of those
produces a normaliser that is right about almost every string and wrong about a
few. unicode runs its own, larger oracle against CPython; this one asks the
question through the call path that text actually uses, which is the part a
migration can break.

Two halves, in two places, and which half goes where is forced rather than
chosen:

- **Ours** comes from a C driver linked against the archive, so it runs on the
  host. That is the point of the gate: it exercises `gtext_nfc()`, the buffer
  sizing and the 4x expansion contract, not just the algorithm underneath.
- **The reference** is `unicodedata.normalize` in a CPython pinned by digest in
  tools/oracle/containers/IMAGES, reached through `tools/oracle/nfc_ask.py`.
  It used to be whatever interpreter ran this script, which on this machine is
  two Unicode releases behind the tables being checked - and, less obviously,
  is not even a stable reference at one UCD version: CPython 3.13.5 and 3.13.15
  both report UCD 15.1.0 and answer `unicodedata.decomposition()` differently
  for every Hangul syllable.

What is compared:

- every codepoint on its own, which covers the singleton decompositions and
  every Hangul syllable;
- a starter followed by one combining mark, over every mark with a non-zero
  combining class, which is the composition table;
- a starter followed by two marks, which is where canonical ordering and the
  blocking rule live - an unstable sort or a wrong blocking test shows up here
  and nowhere else;
- the L, V and T jamo in every combination, which is Hangul composition.

**The population comes from the UCD this library pins, never from the
reference.** A mark too new for the reference is still generated, still asked,
and comes back `skip` - counted by name rather than never entering the
population. A harness drawing its subjects from the reference can never ask
about anything the reference does not know; this one asks, gets no answer, and
says so. That is also why EXPECTED below is checked rather than printed: the
population is a fact about the pinned UCD, and a parse that silently dropped
marks would run a smaller comparison and still report no disagreements.

Usage:
    tools/oracle/nfc_diff.py [--ucd DIR] [--version V] [--strict]

`--strict` requires the reference's UCD to equal the pin, which makes `skip`
impossible and a disagreement necessarily a defect. Without it the reference is
allowed to be older, the skipped count is the size of that gap, and a
disagreement is a finding until someone has read it.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

MAX_CODEPOINT = 0x10FFFF

# What the pinned UCD's data implies about the size of the comparison, so that
# a smaller run is a failure rather than a quieter success. Raising
# tools/idna/UCD_VERSION lands here deliberately: re-measure, add the row, and
# the diff of this dict is the record of what the new release added.
#
#   marks      non-zero Canonical_Combining_Class in UnicodeData.txt
#   starters   a canonical decomposition and CCC 0
#   classes    distinct combining classes among the marks, one representative
#              each for the two-mark sequences
#   sequences  the whole population, which is what compared + skipped must sum
#              to
#   unassigned sequences carrying a codepoint *this* UCD does not assign. They
#              are skipped by every reference, at every version, so they are the
#              part of the skip count that is structural rather than skew -
#              73.1% of the codespace is unassigned at 17.0.0 and the gate
#              cannot exceed 81.5% coverage by any pin.
EXPECTED = {
    "17.0.0": {
        "marks": 968,
        "starters": 2077,
        "classes": 55,
        "sequences": 4411532,
        "unassigned": 814730,
    },
}

DRIVER = r"""
/* Written by tools/oracle/nfc_diff.py. Reads sequences, writes their NFC. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "idna/nfc_internal.h"

int main(void) {
  char line[4096];
  uint32_t in[64];
  uint32_t out[512];
  while (fgets(line, sizeof line, stdin)) {
    size_t n = 0;
    char * at = line;
    while (n < sizeof(in) / sizeof(in[0])) {
      char * end = NULL;
      unsigned long value = strtoul(at, &end, 16);
      if (end == at) {
        break;
      }
      in[n++] = (uint32_t)value;
      at = end;
    }
    size_t got = 0;
    if (!gtext_nfc(in, n, out, sizeof(out) / sizeof(out[0]), &got)) {
      printf("ERROR\n");
      continue;
    }
    for (size_t i = 0; i < got; i++) {
      printf("%s%04X", i ? " " : "", out[i]);
    }
    printf("\n");
  }
  return 0;
}
"""


def build_driver(root, workdir):
    """The C half of the comparison, linked against the archive the Makefile
    just built.

    ARCHIVE comes from the environment for the reason
    tools/conformance/lib.sh gives at length: this used to glob
    `build/<platform>/release/apps/*.a` and take whatever was there. The absent
    case was handled and the *wrong* one was not - `make check-nfc-oracle
    BUILD=debug` compared the release archive, and a glob is what let that
    through. There is no fallback: a script run by hand says what to set.
    """
    archive = os.environ.get("ARCHIVE")
    if not archive:
        sys.exit("ARCHIVE must be set; the Makefile passes it\n"
                 "  (by hand, ARCHIVE=build/<platform>/<build>/apps/"
                 "libghoti.io-text-0.a)")
    if not os.path.exists(archive):
        sys.exit("%s is not there; build the library first (make)" % archive)

    source = os.path.join(workdir, "driver.c")
    with open(source, "w", encoding="ascii") as handle:
        handle.write(DRIVER)
    binary = os.path.join(workdir, "driver")
    generated = os.path.join(
        os.path.dirname(os.path.dirname(archive)), "generated")
    # The archive's NFC is a call into ghoti.io-unicode now, so the driver
    # needs that library on its link line. pkg-config is asked the same way
    # the Makefile asks, along the same PKG_CONFIG_PATH: a driver linked
    # against a different build of unicode than the archive was would answer
    # for whichever one the loader picked.
    branch = "-0"
    unicode_flags = []
    for kind in ("--cflags", "--libs"):
        probe = subprocess.run(
            ["pkg-config", kind, "ghoti.io-unicode" + branch],
            capture_output=True, text=True)
        if probe.returncode != 0:
            sys.exit("pkg-config could not find ghoti.io-unicode" + branch
                     + "; point PKG_CONFIG_PATH at its .pc file")
        unicode_flags += probe.stdout.split()

    # An rpath for every -L the linker was given, so the driver finds the
    # shared library without the caller having exported LD_LIBRARY_PATH. A
    # gate that only passes when the environment happens to be right is a gate
    # that passes for the wrong reason.
    for flag in list(unicode_flags):
        if flag.startswith("-L") and len(flag) > 2:
            unicode_flags.append("-Wl,-rpath," + flag[2:])

    command = [
        os.environ.get("CC", "cc"), "-O1", "-o", binary, source,
        "-I", os.path.join(root, "include"),
        "-I", generated,
        "-I", os.path.join(root, "src"),
        archive,
    ] + unicode_flags + ["-lm"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("could not build the NFC driver:\n" + result.stderr)
    return binary


def read_ucd(ucd):
    """The marks, starters and combining classes this library's UCD gives."""
    marks = []
    starters = []
    ccc = {}
    path = os.path.join(ucd, "UnicodeData.txt")
    for line in open(path, encoding="utf-8"):
        fields = line.split(";")
        if len(fields) < 6:
            continue
        cp = int(fields[0], 16)
        if int(fields[3]) != 0:
            marks.append(cp)
            ccc[cp] = int(fields[3])
        elif fields[5] and not fields[5].startswith("<"):
            starters.append(cp)
    if not marks or not starters:
        sys.exit("UnicodeData.txt yielded no marks or no starters")
    return marks, starters, ccc


def assigned_set(ucd):
    """Every codepoint this library's UCD assigns.

    UnicodeData.txt lists the large blocks as First/Last *pairs* rather than row
    by row, so a reader that takes one line per codepoint sees 40,535 assigned
    where 17.0.0 assigns 299,382 - and the 258,847 it drops come back as
    `unassigned`, which is a value with meaning here rather than a gap.
    """
    assigned = set()
    first = None
    for line in open(os.path.join(ucd, "UnicodeData.txt"), encoding="utf-8"):
        fields = line.split(";")
        if len(fields) < 2:
            continue
        cp, name = int(fields[0], 16), fields[1]
        if name.endswith(", First>"):
            first = cp
        elif name.endswith(", Last>"):
            if first is None:
                sys.exit("UnicodeData.txt: a Last row with no First")
            assigned.update(range(first, cp + 1))
            first = None
        else:
            assigned.add(cp)
    if first is not None:
        sys.exit("UnicodeData.txt: a First row with no Last")
    return assigned


def sequences(marks, starters, ccc):
    """Everything the two implementations are asked about."""
    for cp in range(0, MAX_CODEPOINT + 1):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        yield (cp,)
    for starter in starters:
        for mark in marks:
            yield (starter, mark)
    # Two marks, so that ordering and blocking are exercised. Every ordered
    # pair of marks would be forty million sequences; every pair drawn from
    # one mark per combining class, against every mark, is the part of that
    # space where the classes actually differ.
    # The class comes from the pinned UCD, like the marks themselves. It used
    # to come from unicodedata.combining(), which is the reference - and for a
    # mark the reference does not assign that returns 0, so the 46 marks newer
    # than the host's UCD all collapsed into a spurious class-0 bucket that
    # merged four real classes (9, 220, 230 and 234). It cost no coverage,
    # because all four are also held by long-assigned marks, and it added a
    # 56th representative whose sequences were skipped in their entirety. But
    # the key was wrong, and a class held *only* by marks the reference does
    # not know would have lost its representative to that bucket.
    #
    # This is the same substitution one line below the one the population
    # already avoided, which is the useful part: checking that `marks` comes
    # from the pin says nothing about the line that groups them.
    per_class = {}
    for mark in marks:
        per_class.setdefault(ccc[mark], mark)
    for starter in starters[:24]:
        for first in per_class.values():
            for second in marks:
                yield (starter, first, second)
    # Hangul, in both directions.
    for lead in range(0x1100, 0x1113):
        for vowel in range(0x1161, 0x1176):
            yield (lead, vowel)
            for trail in range(0x11A8, 0x11C3):
                yield (lead, vowel, trail)


def write_population(path, marks, starters, ccc):
    """The questions, once, as a file both sides read byte for byte.

    One file rather than two generations of the same sequence: the driver and
    the reference are handed identical bytes, so a divergence cannot be a
    difference in how the question was spelled. It also bounds memory - four
    million answers held twice is not something to do for no reason.
    """
    count = 0
    with open(path, "w", encoding="ascii") as handle:
        for seq in sequences(marks, starters, ccc):
            handle.write(" ".join("%04X" % cp for cp in seq) + "\n")
            count += 1
    return count


def ask(argv, question, answer, what):
    """Run one side against the population file, answers to `answer`."""
    with open(question, "rb") as stdin, open(answer, "wb") as stdout:
        finished = subprocess.run(argv, stdin=stdin, stdout=stdout,
                                  stderr=subprocess.PIPE)
    if finished.returncode != 0:
        sys.exit("%s failed (exit %d):\n%s"
                 % (what, finished.returncode,
                    oracle_env.reference_stderr(
                        finished.stderr.decode("utf-8", "replace").strip())))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    version = open(os.path.join(root, "tools", "idna", "UCD_VERSION"),
                   encoding="utf-8").read().strip()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=version)
    parser.add_argument("--ucd", default=None)
    parser.add_argument("--strict", action="store_true",
                        help="require the reference's UCD to equal the pin")
    args = parser.parse_args()
    ucd = args.ucd or os.path.join(root, "third_party", "ucd", args.version)

    if not os.path.isdir(ucd):
        sys.exit("no %s; run tools/idna/fetch.sh" % ucd)

    expected = EXPECTED.get(args.version)
    if expected is None:
        sys.exit(
            "no population figures for UCD %s in EXPECTED.\n"
            "Raising the UCD pin changes how much this gate compares, so the\n"
            "new figures are measured and written down rather than inferred:\n"
            "run this gate, read the counts it reports, and add the row."
            % args.version)

    marks, starters, ccc = read_ucd(ucd)
    classes = len({ccc[mark] for mark in marks})
    for name, got in (("marks", len(marks)), ("starters", len(starters)),
                      ("classes", classes)):
        if got != expected[name]:
            sys.exit("UCD %s gives %d %s and EXPECTED says %d. Either the\n"
                     "data moved under the pin or this script stopped reading\n"
                     "all of it; both are failures rather than a smaller run."
                     % (args.version, got, name, expected[name]))

    # The whole argv, built once: `command()` with no `argv` returns the prefix
    # ending in the reference's own interpreter, so appending another `python3`
    # asks it to run a file called python3.
    try:
        reference = oracle_env.command(
            "python", ["python3", os.path.join(here, "nfc_ask.py")])
    except oracle_env.OracleUnavailable as why:
        return oracle_env.decline("check-nfc-oracle", why)
    ucd_said = oracle_env.check_pin("python")
    if args.strict and args.version not in ucd_said:
        return oracle_env.decline(
            "check-nfc-oracle --strict",
            "the reference carries %r and --strict needs UCD %s.\n"
            "GHOTI_ORACLE_ALIAS=python=python-next selects the pin that does."
            % (ucd_said, args.version))

    with tempfile.TemporaryDirectory() as workdir:
        binary = build_driver(root, workdir)
        question = os.path.join(workdir, "sequences.txt")
        asked = write_population(question, marks, starters, ccc)
        if asked != expected["sequences"]:
            sys.exit("the population is %d sequences and EXPECTED says %d"
                     % (asked, expected["sequences"]))

        ours_path = os.path.join(workdir, "ours.txt")
        theirs_path = os.path.join(workdir, "theirs.txt")
        ask([binary], question, ours_path, "the NFC driver")
        ask(reference, question, theirs_path, "the reference")

        checked = 0
        skipped = 0
        structural = 0
        disagreements = []
        assigned = assigned_set(ucd)

        with open(question, encoding="ascii") as asked_lines, \
             open(ours_path, encoding="ascii") as ours_lines, \
             open(theirs_path, encoding="ascii") as theirs_lines:
            header = theirs_lines.readline().split()
            if len(header) < 3 or header[0] != "version":
                sys.exit("the reference did not name its version: %r"
                         % " ".join(header))
            reference_ucd = header[2]
            for seq_text, ours, theirs in zip(asked_lines, ours_lines,
                                              theirs_lines):
                seq_text = seq_text.strip()
                ours = ours.strip()
                theirs = theirs.strip()
                if theirs == "skip":
                    skipped += 1
                    if any(int(t, 16) not in assigned
                           for t in seq_text.split()):
                        structural += 1
                    continue
                checked += 1
                if ours != theirs and len(disagreements) < 20:
                    disagreements.append((seq_text, ours, theirs))
                elif ours != theirs:
                    disagreements.append(None)
            # Nothing may be left over on any of the three. A short answer file
            # is the failure this protocol is most exposed to, and zip() would
            # hide it as a smaller comparison.
            for handle, what in ((asked_lines, "questions"),
                                 (ours_lines, "our answers"),
                                 (theirs_lines, "the reference's answers")):
                if handle.readline():
                    sys.exit("%s outlasted the others: the three files are not "
                             "line-for-line" % what)

        if checked + skipped != asked:
            sys.exit("%d compared plus %d skipped is not the %d asked"
                     % (checked, skipped, asked))
        if structural != expected["unassigned"]:
            sys.exit("%d skipped sequences are unassigned in UCD %s and "
                     "EXPECTED says %d" % (structural, args.version,
                                           expected["unassigned"]))
        # What --strict forbids is a skip *for age*, not a skip. A matched
        # reference still cannot answer for a codepoint UCD 17.0.0 leaves
        # unassigned - 814,730 of the sequences asked - and the first version of
        # this required `skipped == 0`, which no pin can satisfy. Structural and
        # skew are different quantities and only one of them closes.
        if args.strict and skipped != structural:
            sys.exit("--strict and %d sequences were skipped for the "
                     "reference's age; its UCD is %s and the pin is %s"
                     % (skipped - structural, reference_ucd, args.version))

        print("reference UCD %s; these tables are UCD %s%s"
              % (reference_ucd, args.version,
                 "  <- behind" if reference_ucd != args.version else
                 "  <- matched, so a disagreement is a defect"))
        print("compared %d of %d sequences (%.1f%%), skipped %d"
              % (checked, asked, 100.0 * checked / asked, skipped))
        print("  %d of those skips are codepoints UCD %s does not assign "
              "either," % (structural, args.version))
        print("    so no reference reaches them at any version;")
        print("  %d are the reference's age, and close when the pin matches"
              % (skipped - structural))
        if disagreements:
            print("\ndisagreements:")
            for item in [d for d in disagreements if d][:20]:
                seq, ours, theirs = item
                print("  %-24s ours %-24s reference %s" % (seq, ours, theirs))
            if len(disagreements) > 20:
                print("  ... and %d more" % (len(disagreements) - 20))
            return 1
        print("no disagreements")
        return 0


if __name__ == "__main__":
    sys.exit(main())
