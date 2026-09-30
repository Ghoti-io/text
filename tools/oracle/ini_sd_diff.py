#!/usr/bin/env python3
"""Compare this library's systemd reader against systemd itself.

    ini_sd_diff.py <ours>

INI_SD_ORACLE_COUNT and INI_SD_ORACLE_SEED size and seed the population; the
reference is reached through tools/oracle/oracle_env.py, which resolves and
version-checks the pinned image before anything is compared.

Four scores:

  `intent`   our accept/refuse against what `systemd.syntax(7)` and measurement say,
             as tools/oracle/ini_sd_gen.py's REFUSES writes it down.
  `grammar`  whether systemd reported a **grammar** fault, against whether we
             refused the document. This is the comparison that makes the gate
             possible at all, and it needed the instrument to be understood first:
             `systemd-analyze verify` **exits 0 on a syntax error**, warns, and skips
             the line, so its exit status is about semantics and only its
             diagnostics are about the grammar. Reading the status would have scored
             every broken document as legal.
  `words`    our gtext_ini_value_words() against the words systemd echoed through
             `Environment=`, which is the only channel that reports a parsed value.
  `rewrite`  every accepted document writes back byte for byte. No reference.
  `divergence` over the constructs systemd itself gets wrong, that it **still** gets
             them wrong - scored per axis. One so far: it skips a byte-order mark
             after the comment test, so a `<BOM># c` first line is read as an
             assignment outside any section. A reference fixed upstream fails this
             loudly rather than quietly inflating `grammar`.

**One reference, and it disagrees with this module on purpose.** systemd keeps a file
containing a bad line and drops the line; this module refuses the document. That is a
deliberate departure - a reader whose caller cannot see a warning must not silently
lose a setting - so the scores compare the *presence* of a grammar fault rather than
the recovery, which is the part both agree on. @ref format_ini says so where the
number is quoted.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ini_sd_gen  # noqa: E402
import oracle_env  # noqa: E402

NAME = "systemd"

# The diagnostics `config_parse()` itself emits: a fault in the **line grammar**.
# Everything else systemd says is either a semantic complaint about a setting it does
# not know, or a setting's own value parser refusing its argument - and the second of
# those is compared against our words call instead, because that is the same layer.
GRAMMAR_FAULTS = (
    b"Missing '=', ignoring line.",
    b"Missing key name before '=', ignoring line.",
    b"Invalid section header",
    b"Assignment outside of section.",
    b"Line too long",
)

# `Environment=`'s per-word complaint: the value channel.
WORD_PREFIX = b"Invalid environment assignment, ignoring: "

# The setting's own parser refusing its whole argument - an unclosed quote or an
# unknown escape letter. Compared against gtext_ini_value_words() failing, not
# against the parse failing: for systemd, quoting and escaping belong to the setting
# rather than to the grammar, which is what the dialect's `quoted_values` false and
# `escapes_in_grammar` false record.
VALUE_FAULT = b"Invalid syntax, ignoring:"


def unhex(token):
    if token == "-":
        return None
    if token == ".":
        return b""
    return bytes.fromhex(token)


def parse_ours(text):
    """Split our driver's output into one record per document."""
    records = []
    current = None
    for line in text.split("\n"):
        if line == "BEGIN":
            current = {"status": None, "words": [], "rewrite": None}
        elif line == "END":
            records.append(current)
            current = None
        elif current is None:
            continue
        elif line.startswith("sd "):
            current["status"] = line[3:]
        elif line == "Q" or line.startswith("Q "):
            current["words"].append([unhex(t) for t in line.split()[1:]])
        elif line.startswith("Qerr "):
            current["words"].append(int(line.split()[1]))
        elif line.startswith("W "):
            current["rewrite"] = unhex(line[2:])
    return records


def parse_reference(blob, count):
    """Split the reference's raw output into per-document message lists.

    Split on the temporary path prefix rather than on newlines, because **a
    diagnostic can contain a newline**: a decoded `\\n` inside a reported value
    arrives as a real one. A line-oriented read would cut such a message in two and
    attribute its tail to nothing, which is the kind of quiet loss that makes a
    differential agree for the wrong reason.
    """
    messages = [[] for _ in range(count)]
    for batch in re.finditer(rb"^BATCH (\d+) (\d+) ([0-9a-f.]+)\nOUT ([0-9a-f.]+)$",
                             blob, re.M):
        first = int(batch.group(1))
        directory = unhex(batch.group(3).decode())
        out = unhex(batch.group(4).decode())
        # Every diagnostic starts with the file's path or its bare basename.
        pattern = (re.escape(directory) + rb"/u(\d{4})\.service:|"
                   rb"(?<![\w/])u(\d{4})\.service:")
        hits = list(re.finditer(pattern, out))
        for index, hit in enumerate(hits):
            which = int(hit.group(1) or hit.group(2))
            start = hit.end()
            end = hits[index + 1].start() if index + 1 < len(hits) else len(out)
            body = out[start:end]
            # An optional `<line>:` prefix, then the message.
            body = re.sub(rb"^\d+:\s*", b"", body).rstrip(b"\n")
            if first + which < count:
                messages[first + which].append(body)
    return messages


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    ours = argv[1]
    count = int(os.environ.get("INI_SD_ORACLE_COUNT", "5000"))
    seed = int(os.environ.get("INI_SD_ORACLE_SEED", "20260929"))

    print("provenance: %s" % oracle_env.check_pin(NAME))
    print("population: %d generated documents, seed %d" % (count, seed))

    documents = list(ini_sd_gen.documents(seed, count))
    payload = bytearray()
    for data, _axes, _expected in documents:
        payload += (b"%d\n" % len(data)) + data
    payload += b"-1\n"

    mine_proc = subprocess.run([ours], input=bytes(payload), capture_output=True)
    if mine_proc.returncode != 0:
        print("FAIL: our runner exited %d" % mine_proc.returncode)
        print(mine_proc.stderr.decode("utf-8", "replace")[:3000])
        return 1
    mine = parse_ours(mine_proc.stdout.decode("utf-8", "surrogateescape"))
    if len(mine) != count:
        print("FAIL: our runner answered %d of %d documents" % (len(mine), count))
        return 1

    command = oracle_env.command(NAME, ["systemd-unit-driver"])
    theirs_proc = subprocess.run(command, input=bytes(payload), capture_output=True)
    if theirs_proc.returncode != 0:
        print("FAIL: the reference driver exited %d" % theirs_proc.returncode)
        print(theirs_proc.stderr.decode("utf-8", "replace")[:3000])
        return 1
    theirs = parse_reference(theirs_proc.stdout, count)

    codes = {}
    value = 0
    started = False
    header = os.path.normpath(os.path.join(HERE, "..", "..", "include", "ghoti.io",
                                           "text", "ini", "ini_core.h"))
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

    intent = [0, 0]
    grammar = [0, 0]
    diverged = set()
    words = [0, 0]
    rewrite = [0, 0]
    excluded = 0
    used = set()
    failures = []

    for i, (data, axes, expected) in enumerate(documents):
        used |= axes
        record = mine[i]
        messages = theirs[i]

        intent[1] += 1
        want = "ok" if expected == "ok" else "err %d" % codes["GTEXT_INI_" + expected]
        if record["status"] == want:
            intent[0] += 1
        elif len(failures) < 10:
            failures.append("intent #%d want %r got %r axes %s\n%r"
                            % (i, want, record["status"], ",".join(sorted(axes)),
                               data))

        # ---- grammar: did the reference report a line-grammar fault?
        # "Assignment outside of section." means two different things, and only one
        # of them is a grammar fault. A genuine preamble entry is one; an entry in a
        # section systemd does not *know* is the other, because an unknown section
        # is dropped and everything after it is then outside a section. So that
        # message is not counted when the document also drew an "Unknown section".
        unknown_section = any(m.startswith(b"Unknown section") for m in messages)
        their_fault = False
        for message in messages:
            if unknown_section and \
                    message.startswith(b"Assignment outside of section."):
                continue
            if any(fault in message for fault in GRAMMAR_FAULTS):
                their_fault = True
        our_fault = record["status"] != "ok"
        divergent = [a for a in axes if a in ini_sd_gen.REFERENCE_GRAMMAR_DIVERGES]
        if divergent:
            # A construct the reference is known to get wrong. Out of `grammar`'s
            # denominator and into a per-axis assertion that the departure is still
            # observable - the same instrument the EditorConfig gate uses, and for the
            # same reason: an exclusion nobody checks stops meaning anything the day
            # upstream changes.
            if their_fault and not our_fault:
                diverged.update(divergent)
            continue
        grammar[1] += 1
        if their_fault == our_fault:
            grammar[0] += 1
        elif len(failures) < 10:
            failures.append("grammar #%d reference=%s ours=%s axes %s\n%r\n  %r"
                            % (i, their_fault, our_fault, ",".join(sorted(axes)),
                               data, messages))

        if record["status"] == "ok":
            rewrite[1] += 1
            if record["rewrite"] == data:
                rewrite[0] += 1
            elif len(failures) < 10:
                failures.append("rewrite #%d\n  in  %r\n  out %r"
                                % (i, data, record["rewrite"]))

        # ---- words, over documents neither side faulted on
        if their_fault or our_fault:
            continue
        if any(a in ini_sd_gen.NO_WORD_ORACLE for a in axes):
            excluded += 1
            continue
        their_words = [m[len(WORD_PREFIX):] for m in messages
                       if m.startswith(WORD_PREFIX)]
        their_value_fault = any(m.startswith(VALUE_FAULT) for m in messages)
        our_value_fault = any(isinstance(w, int) for w in record["words"])
        our_words = [w for group in record["words"] if not isinstance(group, int)
                     for w in group]
        words[1] += 1
        if their_value_fault or our_value_fault:
            # The setting's own parser refused its argument. Both sides must agree
            # *that* it did; what each then reports about the words is undefined,
            # because systemd discards the whole setting.
            if their_value_fault == our_value_fault:
                words[0] += 1
            elif len(failures) < 10:
                failures.append("words #%d value fault reference=%s ours=%s axes %s"
                                "\n%r\n  %r" % (i, their_value_fault,
                                                our_value_fault,
                                                ",".join(sorted(axes)), data,
                                                messages))
        elif their_words == our_words:
            words[0] += 1
        elif len(failures) < 10:
            failures.append("words #%d axes %s\n  ours %r\n  them %r\n  %r"
                            % (i, ",".join(sorted(axes)), our_words, their_words,
                               data))

    for line in failures:
        print(line)
    print("intent     %6d of %6d  our verdict is what the generator meant"
          % (intent[0], intent[1]))
    print("grammar    %6d of %6d  a line-grammar fault is reported by both or neither"
          % (grammar[0], grammar[1]))
    print("words      %6d of %6d  Environment= splits the same way; %d excluded"
          % (words[0], words[1], excluded))
    print("rewrite    %6d of %6d  an accepted document writes back byte for byte"
          % (rewrite[0], rewrite[1]))
    print("divergence %6d of %6d  axes where systemd is still wrong"
          % (len(diverged), len(ini_sd_gen.REFERENCE_GRAMMAR_DIVERGES)))

    missing = [a for a in ini_sd_gen.AXES if a not in used]
    print("axes: %d of %d exercised" % (len(ini_sd_gen.AXES) - len(missing),
                                        len(ini_sd_gen.AXES)))
    bad = False
    absent = sorted(set(ini_sd_gen.REFERENCE_GRAMMAR_DIVERGES) - diverged)
    if absent:
        print("FAIL systemd no longer departs on: %s" % ",".join(absent))
        for axis in absent:
            print("     expected: %s"
                  % ini_sd_gen.REFERENCE_GRAMMAR_DIVERGES[axis])
        bad = True
    if missing:
        print("FAIL axes never exercised: %s" % ",".join(missing))
        bad = True
    for label, score in (("intent", intent), ("grammar", grammar),
                         ("words", words), ("rewrite", rewrite)):
        if score[0] != score[1]:
            bad = True
        if not score[1]:
            print("FAIL %s scored nothing; the exclusions ate the run" % label)
            bad = True
    print("FAIL" if bad else "PASS: %d documents, every axis exercised" % count)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
