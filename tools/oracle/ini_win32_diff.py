#!/usr/bin/env python3
"""Compare this library's Win32 dialect against wine's profile API.

**Three scores, because the reference has three entry points and two of them
disagree with each other:**

  names     `GetPrivateProfileSectionNames` against our section list
  sections  `GetPrivateProfileSection` against our entries, raw values
  values    `GetPrivateProfileString` against our decoded values

plus two that need no reference:

  intent    every document must parse. This dialect refuses nothing, and that is
            a property rather than an observation - so a refusal is a defect even
            though no reference can report one.
  rewrite   every document must write back byte for byte.

and one that asserts a disagreement rather than an agreement:

  divergence  for each axis in the generator's REFERENCE_DIVERGES, the named
              difference from `GetPrivateProfileString` must still be observable.
              A wine that started treating ';' as a comment would fail here
              instead of quietly making the `values` score look better.

**The reference is wine, and wine is not Windows.** Every run prints that, because
a differential whose reference may not be the reference has to say so where the
number is read. Two of this dialect's rules are corroborated by Microsoft's own
documentation; the rest rest on this implementation alone.

Copyright 2026 by Corey Pennycuff
"""
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ini_win32_gen as gen
import oracle_env


def hexed(b):
    """Lowercase hex, `.` for empty - the encoding both sides share."""
    return b.hex() if b else "."


def unhexed(s):
    return b"" if s == "." else bytes.fromhex(s)


def split_buffer(answer):
    """A NUL-separated API buffer into its items.

    `-` means the API filled nothing, which is not the same as one empty item -
    the distinction `[]` asks for, and the reason the driver hexes the buffer
    whole instead of stopping at the first NUL.
    """
    if answer == "-":
        return []
    raw = bytes.fromhex(answer)
    items = raw.split(b"\x00")
    while items and items[-1] == b"":
        items.pop()
    return items


def ours(runner, docs):
    """Run our side over every document; one record per document."""
    payload = b"".join(b"%d\n%s" % (len(d), d) for _, d in docs) + b"-1\n"
    finished = subprocess.run([runner], input=payload, stdout=subprocess.PIPE)
    records = []
    current = None
    for line in finished.stdout.decode("utf-8", "surrogateescape").splitlines():
        if line == "BEGIN":
            current = {"ok": None, "status": None, "groups": [], "rewrite": None,
                       "lookups": [], "skipped": 0}
        elif line == "END":
            records.append(current)
            current = None
        elif current is None:
            continue
        elif line == "w32 ok":
            current["ok"] = True
        elif line.startswith("w32 err "):
            current["ok"] = False
            current["status"] = int(line.split()[2])
        elif line.startswith("G "):
            _, canon, name, kind = line.split(" ")
            current["groups"].append({"canon": unhexed(canon),
                                      "name": unhexed(name),
                                      "preamble": kind == "P",
                                      "entries": []})
        elif line.startswith("E "):
            parts = line.split(" ")
            key = unhexed(parts[1])
            raw = None if parts[2] == "-" else unhexed(parts[2])
            dec = None if parts[3] == "-" else unhexed(parts[3])
            if current["groups"]:
                current["groups"][-1]["entries"].append((key, raw, dec))
        elif line.startswith("W "):
            current["rewrite"] = unhexed(line.split(" ", 1)[1])
        elif line.startswith("L "):
            _, qname, qkey, value = line.split(" ")
            current.setdefault("lookups", []).append(
                (unhexed(qname), unhexed(qkey),
                 None if value == "-" else unhexed(value)))
        elif line == "S":
            current["skipped"] = current.get("skipped", 0) + 1
    return records


def _has_nul(expect):
    """Whether an expectation holds a NUL, which the reference cannot express."""
    if isinstance(expect, bytes):
        return b"\x00" in expect
    if isinstance(expect, list):
        return any(b"\x00" in item for item in expect)
    return False


def trimmed(name):
    """A section name as the enumeration API reports it: trimmed, case intact."""
    return name.strip(gen.SPACE)


def plan(record, doc):
    """The asks this document needs, and what we expect each answer to be.

    Both directions are covered on purpose. Iterating over our own entries can
    only ever find a value we got wrong; it can never find an entry the reference
    has and we do not, which is what `names` and `sections` are for.
    """
    asks = []
    if record["ok"] is not True:
        return asks
    #
    # **Two kinds of group the enumeration API does not list**, and they are not
    # the same kind. The preamble has no header to report. A `[]` section has one,
    # and is still absent - measured, `[]` then `k=v` lists no sections at all -
    # because an empty name is not a name the API can hand back.
    #
    # The consequence for a lookup is the measured rule that an empty name finds
    # the **preamble**: `p=0` then `[]` then `k=v` answers `p` and not `k`. So a
    # `[]` section's entries are unreachable by any name, and this harness has to
    # resolve the empty name to the preamble rather than to the first group that
    # happens to carry it - otherwise it would ask the reference a question about a
    # section no caller can reach and score the answer as a disagreement.
    #
    expect = [trimmed(g["name"]) for g in record["groups"]
              if not g["preamble"] and g["canon"]]
    asks.append(("names", None, expect))
    preamble = next((g for g in record["groups"] if g["preamble"]), None)

    def answering(canon):
        if not canon:
            return preamble
        return next((g for g in record["groups"] if g["canon"] == canon), None)

    seen = set()
    for group in record["groups"]:
        canon = group["canon"]
        if canon in seen:
            continue
        seen.add(canon)
        group = answering(canon)
        if group is None:
            # An empty name with no preamble: the reference reports nothing, and
            # there is nothing of ours to compare it against either.
            asks.append(("sections", canon, []))
            continue
        rendered = []
        for key, raw, _ in group["entries"]:
            rendered.append(key if raw is None else key + b"=" + raw)
        asks.append(("sections", canon, rendered))
        # **Ask with the document's own spelling as well as the canonical one.**
        # Asking only with the canonical name means no query ever carries an
        # upper-case letter, so the fold is never exercised - and that blind spot
        # hid a real defect: git's `section.subsection` case rule was applied to
        # every folding dialect, so a Win32 lookup for `[Foo.Bar]` spelled exactly
        # as the file spells it found nothing. A harness that normalizes its own
        # input cannot test a normalization.
        if group["name"] != canon and not group["preamble"]:
            asks.append(("sections", trimmed(group["name"]), rendered))
        # **One ask per canonical key, answered by the first entry with it.** The
        # dialect is first-wins, so asking once per *entry* and expecting that
        # entry's own value is a question the reference never answers: for
        # `k=1` then `k=2` the API returns `1` twice. The first version of this
        # harness did exactly that and reported three duplicate-key axes as
        # value mismatches, which were its own.
        first = {}
        for key, _, dec in group["entries"]:
            folded = key.lower()
            if folded in first:
                continue
            first[folded] = True
            asks.append(("values", (canon, key), dec))
            # The same, for the key: the reference folds both names, and a query
            # that is already folded asks it nothing.
            if key.lower() != key or (group["name"] != canon and
                                      not group["preamble"]):
                asks.append(("values", (trimmed(group["name"]), key), dec))
    # **The library's own lookups, asked of the reference with the same spelling.**
    # This is the only part of the plan whose expectation comes from an accessor
    # rather than from the harness resolving the tree itself - which is what let a
    # lookup defect through both gates once.
    for qname, qkey, value in record.get("lookups", []):
        asks.append(("lookup", (qname, qkey), value))
    # The divergence probes, asked up front rather than in a second pass: for each
    # `;`-led line that has a separator, ask the string API for the key we did not
    # create. A `;` line is dropped by us and by the enumeration API, so this is
    # the only ask that can see the disagreement at all.
    for group in record["groups"]:
        for raw_line in doc.split(b"\n"):
            stripped = raw_line.strip(gen.SPACE + b"\r")
            if not stripped.startswith(b";") or b"=" not in stripped:
                continue
            key = stripped.split(b"=", 1)[0].strip(gen.SPACE)
            asks.append(("divprobe", (group["canon"], key), None))
    return asks


def reference(docs, plans):
    """One container invocation for the whole population."""
    lines = []
    for (_, doc), asks in zip(docs, plans):
        job = []
        for kind, arg, _ in asks:
            if kind == "names":
                job.append("NAMES")
            elif kind == "sections":
                job.append("SECT " + hexed(arg))
            else:
                # "values", "lookup" and "divprobe" are all a GET; they differ in
                # where the expectation came from, not in what is asked.
                job.append("GET %s %s" % (hexed(arg[0]), hexed(arg[1])))
        lines.append(b"DOC %d\n" % len(doc) + doc +
                     ("ASK %d\n" % len(job) + "".join(l + "\n" for l in job))
                     .encode())
    command = oracle_env.command("win32", argv=[])
    finished = subprocess.run(command, input=b"".join(lines),
                              stdout=subprocess.PIPE)
    out = finished.stdout.decode("ascii", "replace").splitlines()
    blocks = []
    current = None
    for line in out:
        if line == "BEGIN":
            current = []
        elif line == "END":
            blocks.append(current)
            current = None
        elif current is not None:
            current.append(line)
    return blocks


def corpus(dirs, cap):
    """Every readable `.ini` file under these directories, as (path, bytes).

    **This is not a population of the format**, and the gate says so where the
    number is printed. The profile API will read any of these files, so they are a
    valid population for "do we agree with the reference about real bytes"; what
    they are not is a sample of files Windows applications wrote. This machine has
    two of those, both inside a wine prefix, which is far too few to gate on -
    the honest description of this corpus is that it is real bytes of the right
    shape from the wrong provenance.
    """
    out = []
    for root in dirs:
        for path in sorted(pathlib.Path(root).rglob("*")):
            if len(out) >= cap:
                break
            if not path.is_file() or path.is_symlink():
                continue
            if path.suffix.lower() not in (".ini", ".cfg"):
                continue
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if len(data) > 1 << 20:
                continue
            out.append((str(path), data))
    return out


def main():
    if len(sys.argv) < 2:
        print("usage: ini_win32_diff.py <ours-runner> [--corpus <dir>...]",
              file=sys.stderr)
        return 2
    runner = sys.argv[1]
    is_corpus = len(sys.argv) > 2 and sys.argv[2] == "--corpus"
    if is_corpus:
        cap = int(os.environ.get("INI_W32_CORPUS_MAX", "5000"))
        docs = corpus(sys.argv[3:], cap)
        if not docs:
            print("no .ini or .cfg files found", file=sys.stderr)
            return 2
    else:
        count = int(os.environ.get("INI_W32_ORACLE_COUNT", "0") or 0)
        docs = gen.documents(count or None)

    print("reference: %s" % oracle_env.version("win32"))
    if is_corpus:
        print("population: %d real .ini/.cfg files from this machine." % len(docs))
        print("      These are NOT files Windows applications wrote - this host has")
        print("      two of those. They are real bytes of the right shape from the")
        print("      wrong provenance, which makes them a valid population for")
        print("      'do we agree with the reference' and not for 'is this format")
        print("      used this way'.")
    print("NOTE: the reference is wine, not Windows. Two of this dialect's rules")
    print("      are corroborated by Microsoft's documentation for")
    print("      GetPrivateProfileString; the rest rest on wine alone.")

    records = ours(runner, docs)
    if len(records) != len(docs):
        print("ours produced %d records for %d documents" %
              (len(records), len(docs)), file=sys.stderr)
        return 2

    plans = [plan(r, d) for (_, d), r in zip(docs, records)]
    blocks = reference(docs, plans)
    if len(blocks) != len(docs):
        print("reference produced %d blocks for %d documents" %
              (len(blocks), len(docs)), file=sys.stderr)
        return 2

    score = {k: [0, 0] for k in ("intent", "names", "sections", "values",
                                 "lookup", "rewrite")}
    diverged = {}
    failures = []
    # **The reference's channel cannot carry a NUL**, and that is a property of
    # every one of its three entry points rather than of one axis: all three return
    # C strings. An expectation containing a NUL is therefore excluded from
    # `sections` and `values` and counted here, the way this repository counts every
    # exclusion - an exclusion nobody counts is a hole in the denominator. The
    # `value-nul` divergence asserts the truncation is still happening.
    nul_excluded = 0

    for (label, doc), record, asks, answers in zip(docs, records, plans, blocks):
        # intent: this dialect refuses nothing.
        score["intent"][1] += 1
        if record["ok"] is True:
            score["intent"][0] += 1
        else:
            failures.append("%s: refused with status %s, and this dialect"
                            " refuses nothing" % (label, record["status"]))
            continue

        score["rewrite"][1] += 1
        if record["rewrite"] == doc:
            score["rewrite"][0] += 1
        else:
            failures.append("%s: rewrite differs\n  in  %r\n  out %r" %
                            (label, doc, record["rewrite"]))

        if len(answers) != len(asks):
            failures.append("%s: %d answers for %d asks" %
                            (label, len(answers), len(asks)))
            continue

        for (kind, arg, expect), answer in zip(asks, answers):
            if kind in ("sections", "values", "lookup") and _has_nul(expect):
                nul_excluded += 1
                continue
            if kind == "names":
                score["names"][1] += 1
                got = split_buffer(answer)
                if got == expect:
                    score["names"][0] += 1
                else:
                    failures.append("%s: names %r != ours %r" %
                                    (label, got, expect))
            elif kind == "sections":
                score["sections"][1] += 1
                got = split_buffer(answer)
                if got == expect:
                    score["sections"][0] += 1
                else:
                    failures.append("%s: section %r: %r != ours %r" %
                                    (label, arg, got, expect))
            elif kind == "lookup":
                score["lookup"][1] += 1
                got = None if answer == "MISSING" else unhexed(answer)
                if got == expect:
                    score["lookup"][0] += 1
                else:
                    failures.append("%s: lookup %r/%r: %r != ours %r" %
                                    (label, arg[0], arg[1], got, expect))
            elif kind == "divprobe":
                continue
            else:
                name, key = arg
                is_div = label in gen.REFERENCE_DIVERGES
                got = None if answer == "MISSING" else unhexed(answer)
                if is_div:
                    # Not scored as agreement. What is checked is that the stated
                    # difference is still there, below.
                    continue
                score["values"][1] += 1
                if got == expect:
                    score["values"][0] += 1
                else:
                    failures.append("%s: get %r/%r: %r != ours %r" %
                                    (label, name, key, got, expect))

        # The divergence itself, observed rather than assumed. Each kind is
        # checked by what it predicts, so an axis that stopped diverging fails
        # here instead of silently joining the `values` score.
        if label in gen.REFERENCE_DIVERGES:
            _, kind = gen.REFERENCE_DIVERGES[label]
            diverged.setdefault(label, False)
            ours_keys = {k.lower() for g in record["groups"]
                         for k, _, _ in g["entries"]}
            if kind == "semi":
                for (kind2, arg, _), answer in zip(asks, answers):
                    if kind2 != "divprobe" or answer == "MISSING":
                        continue
                    if arg[1].lower() not in ours_keys:
                        diverged[label] = True
            elif kind == "nul":
                # Our value contains a NUL and the reference's is our value cut
                # at the first one.
                for (kind2, arg, expect), answer in zip(asks, answers):
                    if kind2 != "values" or not expect or b"\x00" not in expect:
                        continue
                    got = None if answer == "MISSING" else unhexed(answer)
                    if got == expect.split(b"\x00")[0]:
                        diverged[label] = True

    print()
    ok = True
    for name in ("intent", "names", "sections", "values", "lookup", "rewrite"):
        good, total = score[name]
        print("%-10s %d/%d" % (name, good, total))
        if good != total:
            ok = False

    print("%-10s %d excluded: the reference returns C strings and cannot"
          " express a NUL" % ("nul", nul_excluded))

    if is_corpus:
        # No divergence table over a corpus: the labels are filenames. What the
        # corpus is for is the three agreement scores and the rewrite, over bytes
        # nobody wrote for this gate.
        if failures:
            print()
            for line in failures[:40]:
                print("FAIL " + line)
            if len(failures) > 40:
                print("... and %d more" % (len(failures) - 40))
        return 0 if ok and not failures else 1

    stated = set(gen.REFERENCE_DIVERGES)
    observed = {k for k, v in diverged.items() if v}
    # An axis whose kind is "none" is in the table for its reasoning and predicts
    # no observable difference, so it is counted separately rather than being
    # silently forgiven - an exclusion nobody counts is a hole in the denominator.
    expected_unobservable = {k for k, (_, kind) in gen.REFERENCE_DIVERGES.items()
                             if kind == "none"}
    print("divergence %d/%d observed (%d stated, %d not observable by"
          " construction)" % (len(observed),
                              len(stated - expected_unobservable),
                              len(stated), len(expected_unobservable)))
    missing = (stated - expected_unobservable) - observed
    if missing:
        ok = False
        for label in sorted(missing):
            print("  NOT OBSERVED: %s - %s" %
                  (label, gen.REFERENCE_DIVERGES[label][0]))
    surprise = observed & expected_unobservable
    if surprise:
        ok = False
        for label in sorted(surprise):
            print("  UNEXPECTEDLY OBSERVED: %s" % label)

    if failures:
        print()
        for line in failures[:40]:
            print("FAIL " + line)
        if len(failures) > 40:
            print("... and %d more" % (len(failures) - 40))
    return 0 if ok and not failures else 1


if __name__ == "__main__":
    sys.exit(main())
