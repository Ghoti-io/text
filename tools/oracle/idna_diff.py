#!/usr/bin/env python3
"""Compare the derived IDNA tables against a pinned independent implementation.

The derivation in gen_tables.py is this repository's reading of RFC 5892
section 2. A reading is a thing that can be wrong, and the first one here was:
it looked for Changes_When_NFKC_Casefolded in the wrong UCD file, found an
empty set, and made every upper-case letter PVALID. Nothing about that is
visible in the output unless something else is asked the same question.

That something else is the `idna` package, which implements IDNA2008 from its
own generated tables. It is not a rewording of this code - different author,
different derivation, same RFC - which is what makes the comparison worth
anything. The same package also carries UTS #46's mapping table, so the second
half of this compares that against ours: two readings of one published file, by
different people.

**The reference is pinned, and that is what this gate gained most from.** It
used to be whatever `import idna` found, which on Debian 13 is 3.10, whose
vendored tables are UCD 15.1.0 against these tables' 17.0.0. Because `idna`
carries its own generated data, no interpreter could have changed that - so
unlike the NFC gate, containerising this one is not about reproducibility
first. It is about being able to *choose* the reference: 3.19 is the release
whose tables are 17.0.0, and at a matching version the gate's blind spot has no
members. It used to have 925.

That blind spot is worth stating, because it is the shape this whole file is
arranged around. A codepoint the reference does not assign gets DISALLOWED from
it by default, and this library usually says DISALLOWED too - so for the 925
codepoints that 17.0.0 assigned and 15.1.0 did not, a DISALLOWED of ours that
should have been PVALID agreed with the reference's ignorance and passed.

**At a matched pin that bucket is closed by construction, not by a check, and
the difference matters.** Its size was the gap between two sources of "is this
assigned"; with one source there is no gap to measure, so a zero printed for it
would be a tautology rather than evidence. What the partition prints instead is
the bucket that can still have members and should not: the codepoints the
reference assigns and this library's UCD does not. That is the same failure
mirrored - the reference ahead rather than behind - and it is what pinning idna
3.20, whose tables are 18.0.0, would put in it today.

**Where "is this codepoint assigned" comes from, and why not from the
interpreter.** The obvious source is `unicodedata.category(cp) != "Cn"`, and
that is what this script used to read from whichever CPython ran it. It answers
for the interpreter's UCD release, which is not the release `idna`'s tables were
generated from - the pinned image is `python:3.14-slim` carrying 16.0.0 with
idna 3.19's 17.0.0 installed over it, so asking there would partition the
codespace by a third version that neither side of the comparison uses. Measured
rather than argued: asking the interpreter gives 294,579 codepoints "where both
versions have an opinion" and a blind spot of 4,803, where the right answer at
this pin is 299,382 and 0. The question is asked of the pinned UCD instead,
after asserting that `idnadata`'s version is that same pin.

Usage:
    tools/oracle/idna_diff.py [--ucd DIR] [--version V]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import subprocess
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "idna"))

import oracle_env
import gen_tables
import gen_uts46


def ask_reference():
    """Run idna_ask.py against the pinned reference and parse what it says.

    Returns (versions, classes, uts46) where `classes` is name -> list of
    half-open ranges and `uts46` is a list of (lo, hi, status, [codepoints]).
    """
    argv = oracle_env.command("idna", ["python3", os.path.join(HERE,
                                                               "idna_ask.py")])
    finished = oracle_env.run(argv, capture_output=True, text=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "the reference failed (exit %d):\n%s"
            % (finished.returncode,
               oracle_env.reference_stderr(finished.stderr.strip())))
    versions = {}
    classes = {}
    uts46 = []
    lines = finished.stdout.splitlines()
    if not lines or not lines[0].startswith("version "):
        raise oracle_env.OracleUnavailable(
            "the reference did not name its versions: %r"
            % (lines[0] if lines else ""))
    fields = lines[0].split()[1:]
    for index in range(0, len(fields) - 1, 2):
        versions[fields[index]] = fields[index + 1]
    for line in lines[1:]:
        parts = line.split()
        if parts[0] == "C":
            classes.setdefault(parts[1], []).append(
                (int(parts[2], 16), int(parts[3], 16)))
        elif parts[0] == "U":
            uts46.append((int(parts[1], 16), int(parts[2], 16), parts[3],
                          [int(p, 16) for p in parts[4:]]))
        else:
            raise oracle_env.OracleUnavailable(
                "the reference emitted a record this does not read: %r" % line)
    for wanted in ("idna", "idnadata", "uts46data"):
        if wanted not in versions:
            raise oracle_env.OracleUnavailable(
                "the reference did not report its %s version" % wanted)
    if not classes or not uts46:
        raise oracle_env.OracleUnavailable(
            "the reference reported %d class ranges and %d UTS #46 runs; one of "
            "its tables did not arrive" % (len(classes), len(uts46)))
    return versions, classes, uts46


def read_ucd_assignments(ucd):
    """(assigned, names) from the pinned UnicodeData.txt.

    First/Last rows are expanded. Without that the CJK and Hangul ranges read as
    unassigned - 258,847 codepoints in the wrong bucket, and `unassigned` is a
    value with meaning in the partition below rather than a gap.

    Ranges get no per-codepoint name, which is what the UCD itself says: the
    name of U+4E00 is in the First row's `<CJK Ideograph, First>` and the
    Consortium derives the rest arithmetically. A disagreement inside a range
    prints the range's label, which is more useful than nothing and honest about
    what the file holds.
    """
    assigned = set()
    names = {}
    first = None
    first_name = None
    for line in open(os.path.join(ucd, "UnicodeData.txt"), encoding="utf-8"):
        fields = line.split(";")
        if len(fields) < 2:
            continue
        cp, name = int(fields[0], 16), fields[1]
        if name.endswith(", First>"):
            first, first_name = cp, name
        elif name.endswith(", Last>"):
            if first is None:
                sys.exit("UnicodeData.txt: a Last row with no First")
            assigned.update(range(first, cp + 1))
            names[first] = first_name
            first, first_name = None, None
        else:
            assigned.add(cp)
            names[cp] = name
    if first is not None:
        sys.exit("UnicodeData.txt: a First row with no Last")
    return assigned, names


def name_of(cp, names, descending):
    """The pinned UCD's name for a codepoint, or its range's label.

    `descending` is the name keys sorted once by the caller; building it per
    codepoint would sort 40,535 keys for each line printed.
    """
    if cp in names:
        return names[cp]
    for start in descending:
        if start <= cp:
            return "in " + names[start]
    return "?"


MAPPING_URLS = (
    # The Consortium publishes the IDNA data two ways and switched between them:
    # `Public/idna/<version>/` exists up to 16.0.0 and stops there, and
    # `Public/<version>/idna/` is where 17.0.0 lives. An earlier version of this
    # script knew only the first, so for any version from 17.0.0 on it got a 404
    # - and *returned 0*, printing "UTS #46 mapping: skipped" for the half of
    # the gate that compares two parsers of one file. Both are tried now and a
    # failure is a failure.
    "https://www.unicode.org/Public/idna/%s/IdnaMappingTable.txt",
    "https://www.unicode.org/Public/%s/idna/IdnaMappingTable.txt",
)


def mapping_table(root, version):
    """The published IdnaMappingTable.txt for `version`, fetching if absent.

    On the host, deliberately: the reference runs with `--network none` and the
    tree mounted read-only, so a fetch from inside it could neither reach the
    network nor write the cache. Doing it here before the reference runs is what
    lets the container stay closed.
    """
    path = os.path.join(root, "third_party", "idna", version,
                        "IdnaMappingTable.txt")
    if os.path.exists(path):
        return path
    tried = []
    for template in MAPPING_URLS:
        url = template % version
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
            if not data.startswith(b"#"):
                raise ValueError("that did not look like a mapping table")
        except (urllib.error.URLError, ValueError, OSError) as exc:
            tried.append("  %s\n    %s" % (url, exc))
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)
        sys.stderr.write("fetched %s\n" % url)
        return path
    raise oracle_env.OracleUnavailable(
        "no IdnaMappingTable.txt for %s, here or published:\n%s\n"
        "This half of the gate holds two parsers of one file against each\n"
        "other, so without the file it checks nothing." % (version,
                                                           "\n".join(tried)))


def check_mapping(root, versions, uts46):
    """The UTS #46 mapping table against python-idna's copy of it.

    Two readings of one published file, by different people, so a disagreement
    is a defect in one of them - a range expanded wrongly, a mapping taken from
    the wrong field.

    The comparison is made against the *reference's* version of the file rather
    than the one this library pins, and that is the whole trick. The two tables
    version independently and their contents really do change: 15.1.0 calls the
    Georgian capitals disallowed and 16.0.0 maps them to their small-letter
    forms. Comparing across versions would report three hundred of those as
    findings and bury a real parsing error among them. Comparing this
    repository's parser against python-idna on the file python-idna was built
    from asks only the question the reference can answer.

    Only mapped and ignored are compared, because they are all this library
    records: it takes validity from RFC 5892, so `valid` and `disallowed` are
    not questions it answers.
    """
    version = versions["uts46data"]
    path = mapping_table(root, version)
    ours = gen_uts46.parse_mapping(path)

    theirs = {}
    for lo, hi, status, target in uts46:
        if status not in ("M", "I"):
            continue
        value = ("mapped", list(target)) if status == "M" else ("ignored", [])
        for cp in range(lo, hi):
            theirs[cp] = value

    disagree = []
    for cp in sorted(set(ours) | set(theirs)):
        if ours.get(cp) != theirs.get(cp):
            disagree.append((cp, ours.get(cp), theirs.get(cp)))

    print("\noracle: UTS #46 mapping %s against python-idna %s"
          % (version, versions["idna"]))
    print("compared %d characters" % len(set(ours) | set(theirs)))
    if not disagree:
        print("no disagreements")
        return 0
    print("%d disagreements:" % len(disagree))
    for cp, mine, yours in disagree[:40]:
        print("  U+%04X  ours=%-28s theirs=%s" % (cp, mine, yours))
    if len(disagree) > 40:
        print("  ... and %d more" % (len(disagree) - 40))
    return 1


def main():
    root = os.path.dirname(HERE)
    root = os.path.dirname(root)
    here_idna = os.path.join(root, "tools", "idna")
    version = open(os.path.join(here_idna, "UCD_VERSION"),
                   encoding="utf-8").read().strip()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=version)
    parser.add_argument("--ucd", default=None)
    args = parser.parse_args()
    ucd = args.ucd or os.path.join(root, "third_party", "ucd", args.version)
    if not os.path.isdir(ucd):
        sys.exit("no UCD at %s - run tools/idna/fetch.sh" % ucd)

    try:
        versions, classes, uts46 = ask_reference()
    except oracle_env.OracleUnavailable as why:
        return oracle_env.decline("check-idna-oracle", why)

    # The assertion that lets the pinned UCD answer for the reference. Not a
    # convenience: without it the partition below is computed from data of one
    # version about tables of another, which is how this gate came to report
    # 4,803 blind-spot codepoints in a configuration whose true answer is 0.
    if versions["idnadata"] != args.version:
        return oracle_env.decline(
            "check-idna-oracle",
            "the reference's tables are UCD %s and this library pins %s.\n"
            "This comparison needs to know which codepoints the reference's own\n"
            "UCD assigns, and the only UCD here is the pinned one - so at a\n"
            "different version it would have to guess, and the guess is where\n"
            "a wrong DISALLOWED of ours passes silently.\n"
            "  make oracle-images     builds the pinned idna (tables %s)\n"
            "  make oracle-version    says what each pin answers"
            % (versions["idnadata"], args.version, args.version))

    ours = gen_tables.derive(ucd)
    assigned, names = read_ucd_assignments(ucd)

    theirs = {}
    for name, ranges in classes.items():
        for lo, hi in ranges:
            for cp in range(lo, hi):
                theirs[cp] = name

    labels = {gen_tables.PVALID: "PVALID", gen_tables.CONTEXTJ: "CONTEXTJ",
              gen_tables.CONTEXTO: "CONTEXTO",
              gen_tables.DISALLOWED: "DISALLOWED"}

    disagree = []
    compared = 0
    unassigned = 0
    reference_only = []
    # **Two buckets here, not the four this used to print, and the reason is
    # worth keeping.** The old partition measured a gap between two sources of
    # "is this assigned": the reference's `unicodedata` and this library's UCD.
    # With assignedness taken from the pinned UCD alone that gap cannot exist,
    # so `skipped` and the 925-codepoint blind spot are closed *by
    # construction* - which is a stronger fix than a zero, and a weaker claim
    # than one. A zero I could assert would be a tautology: the two terms would
    # be the same expression. Saying so is the point; a bucket that cannot have
    # members is not evidence that nothing is wrong.
    #
    # What replaces it is a bucket that *can* have members and should not:
    # `reference_only`, the codepoints the reference calls PVALID, CONTEXTJ or
    # CONTEXTO and the pinned UCD does not assign at all. That is the mirror of
    # the old blind spot - the reference ahead of us rather than behind - and it
    # is the early warning for raising the idna pin past the UCD pin, which
    # 3.20 (tables 18.0.0) would do today. Its zero is measured.
    for cp in range(gen_tables.MAX_CODEPOINT + 1):
        mine = labels[ours[cp]]
        yours = theirs.get(cp, "DISALLOWED")
        if cp not in assigned:
            if cp in theirs:
                reference_only.append((cp, mine, yours))
            else:
                # Neither side assigns it and both default to DISALLOWED. Not a
                # comparison: an agreement between two absences of an opinion.
                unassigned += 1
            continue
        compared += 1
        if mine != yours:
            disagree.append((cp, mine, yours))

    print("oracle: python-idna %s, tables UCD %s, against UCD %s%s"
          % (versions["idna"], versions["idnadata"], args.version,
             "  <- matched, so every assigned codepoint is compared"
             if versions["idnadata"] == args.version else "  <- behind"))
    print("compared %d codepoints, every one that UCD %s assigns"
          % (compared, args.version))
    print("  %d are assigned by neither version and agree by shared default,"
          % unassigned)
    print("    which is 73.1% of the codespace and not a comparison")
    print("  %d are assigned by the reference and not by UCD %s"
          % (len(reference_only), args.version))
    print("  %d total; the first number is the one to quote"
          % (compared + unassigned + len(reference_only)))
    print("the blind spot this gate used to print - 925 codepoints we assign,")
    print("  the reference does not, and both call DISALLOWED - is closed by")
    print("  the matched pin rather than by a check, because at one UCD version")
    print("  there is nothing for it to hold")

    # Identities, asserted so that a wiring change which reintroduced a second
    # UCD is a failure rather than a smaller number nobody reads.
    if compared != len(assigned):
        sys.exit("%d codepoints compared and UCD %s assigns %d"
                 % (compared, args.version, len(assigned)))
    if compared + unassigned + len(reference_only) != gen_tables.MAX_CODEPOINT + 1:
        sys.exit("the buckets sum to %d and the codespace is %d"
                 % (compared + unassigned + len(reference_only),
                    gen_tables.MAX_CODEPOINT + 1))
    if reference_only:
        print("\n%d codepoints the reference assigns and UCD %s does not:"
              % (len(reference_only), args.version))
        for cp, mine, yours in reference_only[:20]:
            print("  U+%04X  ours=%-10s theirs=%-10s" % (cp, mine, yours))
        if len(reference_only) > 20:
            print("  ... and %d more" % (len(reference_only) - 20))
        print("The reference's tables are ahead of tools/idna/UCD_VERSION. That")
        print("is not a defect in either, and it is not something this gate can")
        print("read: raise the UCD pin, or pin an idna whose tables match it.")
        return 1

    status = 0
    if not disagree:
        print("no disagreements")
    else:
        status = 1
        print("%d disagreements:" % len(disagree))
        descending = sorted(names, reverse=True)
        for cp, mine, yours in disagree[:40]:
            print("  U+%04X  ours=%-10s theirs=%-10s  %s"
                  % (cp, mine, yours, name_of(cp, names, descending)))
        if len(disagree) > 40:
            print("  ... and %d more" % (len(disagree) - 40))

    try:
        status |= check_mapping(root, versions, uts46)
    except oracle_env.OracleUnavailable as why:
        return status | oracle_env.decline("check-idna-oracle mapping", why)
    return status


if __name__ == "__main__":
    sys.exit(main())
