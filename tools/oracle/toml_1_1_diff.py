#!/usr/bin/env python3
"""Compare this library's v1.1.0 reading against toml++'s, over the relaxations
they both implement.

    toml_1_1_diff.py <runner> [--count N] [--seed N]

**Why this gate is narrower than the 1.0.0 one.** Nothing outside this repository
reads the v1.1.0 draft, so until now the cases the two toml-test manifests
disagree over were checked against toml-test's decision and against nothing else.
toml++ 3.4.0 is the nearest thing to a second reader, and it is not a v1.1.0
implementation: `TOML_ENABLE_UNRELEASED_FEATURES` turns on eight items
cherry-picked from the TOML master branch and the issue list, and the two sets
overlap rather than coincide.

So the overlap is written down, by name, and it is what this compares:

  in both, and compared here
    omitted seconds in a time of day          (toml#671)
    newlines and trailing commas inline       (toml#516)
    the `\\e` escape                           (toml#790)
    the `\\xHH` escape                         (toml#796)

  toml++ has it and v1.1.0 does not, so it is never generated
    hex floating-point values                 (toml#562)
    `+` in a bare key                         (toml#644)

  v1.1.0 has it and this library does not read it, so there is nothing to compare
    unicode in an unquoted key                (toml#687, toml#891)

  v1.1.0 tightened it and toml++ is not known to follow, so it is asked of the
  corpus and not of this reference
    a lone carriage return in a multi-line string

That list is here rather than in containers/IMAGES because it decides what the
code generates, and a list of exclusions kept somewhere the code does not read
is a list that drifts away from the run.

The documents are the 1.0.0 generator's, with the four relaxations applied to the
spelling: same value tree, spelled in ways only a 1.1.0 reader accepts. So a
disagreement is about the relaxation and not about the rest of the format, which
the tomllib differential already covers over 60,000 documents.

Copyright 2026 by Corey Pennycuff
"""

import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "conformance"))

import oracle_env  # noqa: E402
import toml_gen  # noqa: E402
from toml_native import (expected_native, normalise_datetime,  # noqa: E402
                         same_native)

GATE = "check-toml-1-1-oracle"
# The relaxations this gate is about, and every one of them has to turn up in a
# run. Four v1.1.0 items, counted as five because the inline-table relaxation is
# two independent choices - a newline, and a trailing comma - and this library
# offers them as two write-side bits for that reason.
WANTED = ("escape-e", "escape-x", "no-seconds", "inline-newline",
          "inline-trailing-comma")
NAME = "tomlpp"
DRIVER_SOURCE = os.path.join(HERE, "containers", "tomlpp", "driver.cpp")


def driver_sha():
    with open(DRIVER_SOURCE, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


class Gen11(toml_gen.Gen):
    """The 1.0.0 generator, spelling the four shared relaxations.

    Only the spelling changes. The value tree is the base class's, so a
    disagreement here is about a relaxation rather than about anything the
    tomllib differential already covers.

    Each relaxation records itself in `used`. That is what makes "did this
    document say anything about v1.1.0?" an answer rather than a guess: reading
    it back out of the text would be a second, weaker parser of the thing being
    generated, and a document with none of them is a v1.0.0 document that would
    pass this gate while saying nothing.
    """

    def __init__(self, seed):
        super().__init__(seed)
        self.used = set()

    def text(self):
        # U+001B on purpose. The 1.0.0 generator has no reason to reach for it and
        # does not, so without this `\e` would never have anything to escape and
        # the gate would report zero documents for it - which it does report, and
        # then fails, rather than passing three relaxations out of five.
        value = super().text()
        if self.rng.random() < 0.25:
            at = self.rng.randint(0, len(value))
            value = value[:at] + '\x1b' + value[at:]
        return value

    def spell_string(self, value):
        spelt = super().spell_string(value)
        # `\e` for U+001B and `\xHH` for any escaped control character, in a
        # basic string only: a literal string escapes nothing, and a multi-line
        # one is left alone so that the plain form is exercised too.
        if spelt.startswith('"') and not spelt.startswith('"""'):
            if self.rng.random() < 0.5 and '\\u001B' in spelt:
                spelt = spelt.replace('\\u001B', '\\e')
                self.used.add('escape-e')
            if self.rng.random() < 0.5:
                # Every code the base spelling escapes as `\u00HH`, which is the
                # range `\xHH` names: v1.1.0 says `\xHH` is U+00HH, a scalar
                # value and not a byte.
                for code in list(range(0x00, 0x20)) + [0x7F]:
                    if code == 0x1B:
                        continue
                    if '\\u%04X' % code in spelt:
                        spelt = spelt.replace('\\u%04X' % code,
                                              '\\x%02X' % code)
                        self.used.add('escape-x')
        return spelt

    def datetime_value(self):
        value, spelt = super().datetime_value()
        # A time of day written without its seconds. The base generator picks a
        # second uniformly, so waiting for a `:00` to turn up would reach this
        # relaxation once in sixty documents; the seconds are forced to zero here
        # and the value recomputed from the forced spelling, which is the same
        # thing said in one step instead of by rejection sampling.
        head = None
        if len(spelt) >= 8 and spelt[2] == ':' and spelt[5] == ':':
            head = 0
        elif len(spelt) >= 19 and spelt[13] == ':' and spelt[16] == ':':
            head = 11
        if head is None or self.rng.random() >= 0.5:
            return value, spelt
        tail = spelt[head + 8:]
        if tail.startswith('.'):
            # A fractional second cannot be written without the second it is a
            # fraction of.
            return value, spelt
        self.used.add('no-seconds')
        return (('date-time',
                 normalise_datetime(spelt[:head + 5] + ':00' + tail)),
                spelt[:head + 5] + tail)

    def inline_table(self, depth):
        value, spelt = super().inline_table(depth)
        # Newlines and a trailing comma, which 1.0.0 admits in neither place.
        if spelt != '{}' and self.rng.random() < 0.5:
            inner = spelt[1:-1].strip()
            comma = ',' if self.rng.random() < 0.5 else ''
            spelt = '{\n  ' + inner.replace(', ', ',\n  ') + comma + '\n}'
            self.used.add('inline-newline')
            if comma:
                self.used.add('inline-trailing-comma')
        return value, spelt


def documents(seed, count):
    for i in range(count):
        gen = Gen11(seed + i)
        text, values = gen.document()
        yield seed + i, text, values, frozenset(gen.used)


def ask_reference(texts):
    """toml++'s reading of each document, or an error string."""
    argv = oracle_env.command(NAME, ["toml-driver"])
    request = []
    for text in texts:
        body = text.encode("utf-8")
        request.append(b"%d\n" % len(body))
        request.append(body)
    finished = subprocess.run(argv, input=b"".join(request),
                              capture_output=True)
    if finished.returncode != 0:
        raise oracle_env.OracleUnavailable(
            "the reference exited %d\n%s"
            % (finished.returncode,
               oracle_env.reference_stderr(
                   finished.stderr.decode("utf-8", "replace"))))
    lines = finished.stdout.decode("utf-8", "replace").split("\n")
    if not lines or not lines[0].startswith("version "):
        raise oracle_env.OracleUnavailable(
            "the reference did not say what it is: %r" % lines[:1])
    # The driver carries the SHA-256 of the source it was compiled from, so an
    # image built from an older copy of driver.cpp is caught here rather than
    # answering confidently from code nobody is reading.
    want = driver_sha()
    if want not in lines[0]:
        raise oracle_env.OracleUnavailable(
            "the image's driver was built from a different driver.cpp\n"
            "  it says: %s\n  containers/tomlpp/driver.cpp is %s\n"
            "Rebuild it with: make oracle-images" % (lines[0], want))
    answers = [line for line in lines[1:] if line]
    if len(answers) != len(texts):
        raise oracle_env.OracleUnavailable(
            "asked %d documents and got %d answers" % (len(texts), len(answers)))
    out = []
    for answer in answers:
        if answer.startswith("ok "):
            out.append(("ok", json.loads(answer[3:])))
        else:
            out.append(("err", answer[4:]))
    return out


def ask_library(runner, text):
    """This library's v1.1.0 reading, as tagged JSON, or an error string."""
    finished = subprocess.run([runner, "--version=1.1.0"],
                              input=text.encode("utf-8"),
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
            sys.exit("usage: toml_1_1_diff.py <runner> [--count N] [--seed N]")
    if not runner or not os.path.exists(runner):
        sys.exit("toml_1_1_diff.py needs the built toml-test runner")

    cases = list(documents(seed, count))
    relaxed = [(s, t, v) for s, t, v, used in cases if used]
    seen = {}
    for _s, _t, _v, used in cases:
        for name in used:
            seen[name] = seen.get(name, 0) + 1
    print("generated %d documents from seed %d, %d of them spelling at least "
          "one v1.1.0 relaxation" % (len(cases), seed, len(relaxed)))
    for name in sorted(WANTED):
        print("    %-22s %d documents" % (name, seen.get(name, 0)))
    # Every relaxation in its own right, in the denominator rather than beside
    # it. A run that exercised three of the five would print a clean line for
    # the fourth, which is the shape of a gate that cannot see.
    missing = sorted(name for name in WANTED if not seen.get(name))
    if missing:
        sys.stderr.write(
            "FAIL: no document spelled %s, so this run says nothing about "
            "%s. Raise TOML_ORACLE_COUNT, or fix the generator.\n"
            % (", ".join(missing), "it" if len(missing) == 1 else "them"))
        return 1

    try:
        theirs = ask_reference([t for _s, t, _v in relaxed])
    except oracle_env.OracleUnavailable as why:
        return oracle_env.decline(GATE, why)

    intent_bad = []
    reader_bad = []
    compared = 0
    for (case_seed, text, values), (kind, payload) in zip(relaxed, theirs):
        if kind == "err":
            reader_bad.append((case_seed, "the reference refused it: %s"
                               % payload, text))
            continue
        try:
            reference = expected_native(payload)
        except ValueError as exc:
            reader_bad.append((case_seed, "its tagged JSON did not convert: %s"
                               % exc, text))
            continue
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

    print("  %d compared against toml++" % compared)
    if intent_bad:
        print("\n  the generator and toml++ disagree, which is the generator "
              "being wrong:")
        report(intent_bad)
    if reader_bad:
        print("\n  this library and toml++ disagree:")
        report(reader_bad)
    if intent_bad or reader_bad:
        return 1
    if compared == 0:
        sys.stderr.write(
            "FAIL: nothing was compared, so this gate measured nothing\n")
        return 1
    print("\033[0;32mNo disagreements over %d documents spelling a v1.1.0 "
          "relaxation.\033[0m" % compared)
    return 0


def report(rows):
    for case_seed, why, text in rows[:10]:
        print("    seed %d: %s" % (case_seed, why))
        for line in text.splitlines()[:12]:
            print("      | %s" % line)
    if len(rows) > 10:
        print("    ... and %d more" % (len(rows) - 10))
    print("    reproduce one with: TOML_ORACLE_SEED=<seed> "
          "TOML_ORACLE_COUNT=1 make check-toml-1-1-oracle")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
