#!/usr/bin/env python3
"""Generate EditorConfig documents nobody chose, tagged with what is in them.

Every document carries four things: its bytes, the **query filename** whose
properties are to be resolved, the set of **axes** it exercises, and whether
specification 0.17.2 makes it legal.

**The intent reading matters more here than in any other differential in this
module**, because neither reference is right. Both cores fail the normative
conformance suite, and the probe in notes/text/INI-DIALECTS.md §A.16 found
nineteen constructs on which at least one of them departs from the specification.
So "both cores agree with us" is not the target and would sometimes be a failure;
what the gate checks is the specification's rule, with each known departure
written down as a standing expectation that the core still departs. A core that
got fixed upstream fails `divergence` loudly instead of silently inflating
`values`.

**Every section name is a literal filename with no glob metacharacter**, and the
query is one of those names or a name no section has. That keeps a filepath glob
matcher out of the differential entirely, which is deliberate: globbing is the
subject of 130 of the suite's 202 assertions and is not a text library's job. The
documents whose section names carry glob syntax are the conformance gate's.

**Every axis must appear in the run and be counted, and a zero fails the gate**,
for the reason the other two generators give: no corpus on this machine contains a
vertical tab in a value, a 5000-byte line or a `#` inside a section name, so a
generator that quietly stopped emitting one would leave the gate blind exactly
where the rule lives.

Copyright 2026 by Corey Pennycuff
"""

import random

# The axes, by name. AXES is the denominator the gate checks for zeros.
AXES = (
    # ---- accepted: the ordinary shapes
    "plain",
    "key-fold",
    "value-case-kept",
    "space-around-equals",
    "tab-around-equals",
    "vtab-before-equals",
    "empty-value",
    "value-inner-space",
    "value-trailing-space",
    "value-leading-space",
    "blank-line",
    "blank-line-tabs",
    "comment-hash",
    "comment-semicolon",
    "comment-indented",
    "no-final-newline",
    "crlf",
    "bom",
    "second-section",
    "duplicate-section",
    "duplicate-key",
    "indented-entry",
    "indented-header",
    "header-trailing-space",
    "utf8-value",
    "preamble-key",
    "root-true",
    "root-in-section",
    # ---- accepted: the key grammar
    "key-with-space",
    "key-with-colon",
    "key-upper",
    "long-key",
    # ---- accepted: the value grammar, where the cores go wrong
    "value-with-equals",
    "value-hash-tight",
    "value-semi-tight",
    "value-hash-space",
    "value-semi-space",
    "value-hash-tab",
    "value-backslash",
    "value-quotes-only",
    "value-quoted-text",
    "long-value",
    "long-line",
    "vtab-in-value",
    "formfeed-in-value",
    "vtab-indent",
    # ---- accepted: the section grammar
    "section-empty",
    "section-inner-space",
    "section-upper",
    "section-hash-inside",
    "section-semi-inside",
    "section-bracket-inside",
    # ---- refused by the specification
    "no-separator",
    "empty-key",
    "ws-only-key",
    "header-no-close",
    "header-remainder",
    "colon-only-separator",
    "cr-only-terminator",
    # ---- no oracle
    "nul-in-value",
    "latin1-byte",
)

# The axes whose document specification 0.17.2 refuses, and the status this
# module reports. "Any line that is not one of the above is invalid" is the whole
# rule, and each of these is a line that is none of the four.
REFUSES = {
    # No `=` at all, so it is not a pair; not blank, not a comment, not a header.
    "no-separator": "E_BAD_LINE",
    # `k:v` has no `=`. Both cores accept it as a pair, inherited from
    # ConfigParser; the specification's pair rule needs the `=`.
    "colon-only-separator": "E_BAD_LINE",
    # The part before the first `=` is empty. core-c reads a property whose name
    # is the empty string; core-py refuses as this does.
    "empty-key": "E_BAD_KEY",
    "ws-only-key": "E_BAD_KEY",
    "header-no-close": "E_BAD_GROUP",
    # After the trim a header line must *end* with `]`. Both cores ignore the
    # remainder silently. With the last-`]` rule `[a] junk]` is instead one
    # section named `a] junk`, which all three agree on - which is why that is a
    # separate axis.
    "header-remainder": "E_BAD_LINE",
    # A lone CR is not a line separator: "LF or CRLF". So `[a]\rk=v` is one line
    # that does not end with `]`. core-c reads the section and discards the rest;
    # core-py splits the line on the CR and reads both.
    "cr-only-terminator": "E_BAD_LINE",
}

# Which reference cannot answer at all, by axis. Counted, not scored - an
# exclusion nobody counts is a hole in the denominator.
NO_C_ORACLE = (
    # core-c reads the file with C string functions and truncates at a NUL.
    "nul-in-value",
)
NO_PY_ORACLE = (
    # core-py opens the file as UTF-8, so a document with an invalid byte never
    # reaches its parser: the failure is the codec's, not a verdict.
    "latin1-byte",
)

# Where a core is **expected to disagree with us**, measured in §A.16. The gate
# asserts the disagreement rather than ignoring it, so that a core fixed upstream
# fails this score instead of quietly inflating `values`.
#
# The assertion is inequality rather than a specific wrong answer: the exact wrong
# answer is a second copy of the core's bug, and what the gate needs to know is
# that the departure is still there.
DIVERGES_C = {
    "value-hash-space": "truncates the value at a whitespace-preceded #",
    "value-semi-space": "truncates the value at a whitespace-preceded ;",
    "value-hash-tab": "counts a tab as the whitespace before a #",
    "long-key": "silently drops a key longer than 1024 bytes",
    "long-value": "silently drops a value longer than 4096 bytes",
    "long-line": "fgets splits a line at 5000 bytes",
    "empty-key": "reads `=v` as a property whose name is the empty string",
    "ws-only-key": "reads `   =v` as a property whose name is the empty string",
    "header-remainder": "ignores what follows the `]`",
    "colon-only-separator": "accepts `:` as a separator",
    "cr-only-terminator": "reads the section and discards the rest of the line",
}
DIVERGES_PY = {
    "value-hash-space": "truncates the value at a whitespace-preceded #",
    "value-semi-space": "truncates the value at a whitespace-preceded ;",
    "value-quotes-only": 'maps the value `""` to the empty string',
    "key-with-colon": "ends the key at a `:`, so `k:e=v` is `k` = `e=v`",
    "comment-indented": "tests the first byte before stripping the line",
    "section-hash-inside": "refuses a `#` inside a section name",
    "section-semi-inside": "refuses a `;` inside a section name",
    "section-empty": "refuses `[]`",
    "vtab-in-value": "splits lines on a vertical tab",
    "formfeed-in-value": "splits lines on a form feed",
    "vtab-before-equals": "splits lines on a vertical tab, so `k\\v=v` has no `=`",
    "header-remainder": "ignores what follows the `]`",
    "colon-only-separator": "accepts `:` as a separator",
    "cr-only-terminator": "splits lines on a CR",
}

# Literal filenames, no glob metacharacter among them. See the module docstring.
NAMES = ("a.c", "b.h", "main.py", "README", "deep.name.txt", "x")
KEYS = ("indent_x", "width", "charset_x", "eol_x", "tabs", "final_nl")
VALUES = ("space", "tab", "4", "utf-8", "true", "false", "lf", "")


class Gen:
    """One document per call, with the axes it used recorded."""

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.used = set()
        self.refusal = None

    def mark(self, axis, axes):
        self.used.add(axis)
        axes.add(axis)
        # The **first** refusing construct in document order, not an arbitrary
        # one from the set. A document can carry two: `[x` on line 1 and `=v` on
        # line 2 is refused for the header, not the key, and a parse stops at the
        # first fault. Marks happen in emission order, so keeping the first mark
        # keeps document order - and the `intent` score is what caught this,
        # eight times in three hundred documents, with `legality` none the wiser.
        if self.refusal is None and axis in REFUSES:
            self.refusal = REFUSES[axis]
        return axis

    def value(self, axes):
        """A raw value, possibly carrying a value-level axis."""
        pick = self.rng.random()
        if pick < 0.04:
            self.mark("value-hash-tight", axes)
            return "a#b"
        if pick < 0.08:
            self.mark("value-semi-tight", axes)
            return "a;b"
        if pick < 0.13:
            self.mark("value-hash-space", axes)
            return "keep # not a comment"
        if pick < 0.18:
            self.mark("value-semi-space", axes)
            return "keep ; not a comment"
        if pick < 0.22:
            self.mark("value-hash-tab", axes)
            return "keep\t# not a comment"
        if pick < 0.26:
            self.mark("value-backslash", axes)
            return r"keep \; not a comment"
        if pick < 0.29:
            self.mark("value-quotes-only", axes)
            return '""'
        if pick < 0.32:
            self.mark("value-quoted-text", axes)
            return '"a b"'
        if pick < 0.36:
            self.mark("value-with-equals", axes)
            return "a=b"
        if pick < 0.40:
            self.mark("value-inner-space", axes)
            return "a value with spaces"
        if pick < 0.43:
            self.mark("empty-value", axes)
            return ""
        if pick < 0.47:
            self.mark("utf8-value", axes)
            return self.rng.choice(("café", "日本語", "straße"))
        if pick < 0.50:
            self.mark("value-case-kept", axes)
            return "MixedCase"
        if pick < 0.53:
            self.mark("long-value", axes)
            return "v" * 4100
        if pick < 0.56:
            self.mark("vtab-in-value", axes)
            return "a\vb"
        if pick < 0.59:
            self.mark("formfeed-in-value", axes)
            return "a\fb"
        if pick < 0.61:
            self.mark("nul-in-value", axes)
            return "a\0b"
        if pick < 0.63:
            self.mark("latin1-byte", axes)
            return "a\udcffb"
        self.mark("plain", axes)
        return self.rng.choice(VALUES)

    def key(self, axes):
        pick = self.rng.random()
        if pick < 0.06:
            self.mark("key-with-space", axes)
            return "ke y"
        if pick < 0.11:
            self.mark("key-with-colon", axes)
            return "k:e"
        if pick < 0.17:
            self.mark("key-upper", axes)
            self.mark("key-fold", axes)
            return "MiXed"
        if pick < 0.21:
            self.mark("long-key", axes)
            return "k" * 1030
        return self.rng.choice(KEYS)

    def entry(self, axes):
        """One entry line, or a refused line."""
        pick = self.rng.random()
        if pick < 0.03:
            self.mark("no-separator", axes)
            return "bare\n"
        if pick < 0.06:
            self.mark("colon-only-separator", axes)
            return "k:v\n"
        if pick < 0.09:
            self.mark("empty-key", axes)
            return "=v\n"
        if pick < 0.11:
            self.mark("ws-only-key", axes)
            return "   =v\n"
        key = self.key(axes)
        value = self.value(axes)
        pick = self.rng.random()
        if pick < 0.12:
            self.mark("space-around-equals", axes)
            sep = " = "
        elif pick < 0.20:
            self.mark("tab-around-equals", axes)
            sep = "\t=\t"
        elif pick < 0.24:
            self.mark("vtab-indent", axes)
            return "\v%s=%s\n" % (key, value)
        elif pick < 0.30:
            # The separator run's own whitespace predicate, which is git's narrow
            # space-or-tab everywhere else and the dialect's here. Without this
            # axis nothing exercised ini_sep_space()'s EditorConfig arm at all -
            # `vtab-indent` goes through the leading-whitespace skip instead.
            self.mark("vtab-before-equals", axes)
            return "%s\v=\v%s\n" % (key, value)
        else:
            sep = "="
        line = "%s%s%s" % (key, sep, value)
        pick = self.rng.random()
        if pick < 0.08:
            self.mark("indented-entry", axes)
            line = "  " + line
        if pick < 0.16:
            self.mark("value-trailing-space", axes)
            line = line + "   "
        if pick < 0.22 and value:
            self.mark("value-leading-space", axes)
            line = "%s=   %s" % (key, value)
        if pick < 0.26:
            self.mark("long-line", axes)
            line = "%s=%s" % (key, "L" * 5000)
        return line + "\n"

    def header(self, name, axes):
        pick = self.rng.random()
        if pick < 0.05:
            self.mark("header-no-close", axes)
            return "[%s\n" % name
        if pick < 0.10:
            self.mark("header-remainder", axes)
            return "[%s] junk\n" % name
        if pick < 0.14:
            self.mark("cr-only-terminator", axes)
            return "[%s]\rk=v\n" % name
        if pick < 0.19:
            self.mark("indented-header", axes)
            return "  [%s]\n" % name
        if pick < 0.24:
            self.mark("header-trailing-space", axes)
            return "[%s]   \n" % name
        return "[%s]\n" % name

    def spare_section_name(self, axes):
        """A section name that is not the query, carrying a name-level axis."""
        pick = self.rng.random()
        if pick < 0.16:
            self.mark("section-empty", axes)
            return ""
        if pick < 0.32:
            self.mark("section-inner-space", axes)
            return "a name.c"
        if pick < 0.48:
            self.mark("section-upper", axes)
            return "UPPER.C"
        if pick < 0.64:
            self.mark("section-hash-inside", axes)
            return "a#b.c"
        if pick < 0.80:
            self.mark("section-semi-inside", axes)
            return "a;b.c"
        self.mark("section-bracket-inside", axes)
        return "a]b.c"

    def document(self):
        axes = set()
        self.refusal = None
        rng = self.rng
        query = rng.choice(NAMES)
        out = []

        if rng.random() < 0.10:
            self.mark("bom", axes)
            out.append("﻿")
        if rng.random() < 0.18:
            self.mark("preamble-key", axes)
            out.append("pre_key=1\n")
        if rng.random() < 0.18:
            self.mark("root-true", axes)
            out.append("root = true\n")
        if rng.random() < 0.12:
            self.mark("comment-hash", axes)
            out.append("# a leading comment\n")
        if rng.random() < 0.12:
            self.mark("comment-semicolon", axes)
            out.append("; a leading comment\n")
        if rng.random() < 0.12:
            self.mark("comment-indented", axes)
            out.append("   # an indented comment\n")

        # The query's own section, always present, so there is something to
        # resolve.
        out.append(self.header(query, axes))
        for _ in range(rng.randint(1, 3)):
            if rng.random() < 0.15:
                self.mark("blank-line", axes)
                out.append("\n")
            if rng.random() < 0.10:
                self.mark("blank-line-tabs", axes)
                out.append("\t \n")
            out.append(self.entry(axes))
        if rng.random() < 0.15:
            self.mark("root-in-section", axes)
            out.append("root=true\n")
        if rng.random() < 0.20:
            self.mark("duplicate-key", axes)
            out.append("%s=second\n" % KEYS[0])
            out.append("%s=third\n" % KEYS[0])

        if rng.random() < 0.30:
            self.mark("duplicate-section", axes)
            out.append(self.header(query, axes))
            out.append(self.entry(axes))
        if rng.random() < 0.35:
            self.mark("second-section", axes)
            spare = self.spare_section_name(axes)
            if spare != query:
                out.append("[%s]\n" % spare)
                out.append("%s=spare\n" % rng.choice(KEYS))

        text = "".join(out)
        # Not together with `cr-only-terminator`: rewriting every LF as CRLF turns
        # `[a]\rk=v\n` into `[a]\r\r\nk=v\r\n`, whose header line is then
        # `[a]\r` - one CR is the terminator's and the other is trailing
        # whitespace - so the document becomes legal and the expectation would be
        # wrong. Two axes that cancel have to be kept apart rather than left to
        # meet one run in fifty.
        if rng.random() < 0.12 and "cr-only-terminator" not in axes:
            self.mark("crlf", axes)
            text = text.replace("\n", "\r\n")
        if rng.random() < 0.10:
            self.mark("no-final-newline", axes)
            text = text.rstrip("\n")

        expected = self.refusal or "ok"
        data = text.encode("utf-8", "surrogateescape")
        return data, query, axes, expected


def documents(seed, count):
    """Yield (bytes, query name, axes, expected) `count` times."""
    gen = Gen(seed)
    for _ in range(count):
        yield gen.document()
    documents.used = gen.used


if __name__ == "__main__":
    import collections
    import sys
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    tally = collections.Counter()
    for data, query, axes, expected in documents(seed, count):
        tally.update(axes)
        sys.stdout.write("--- query %s expected %s axes %s\n"
                         % (query, expected, ",".join(sorted(axes))))
        sys.stdout.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    missing = [a for a in AXES if not tally[a]]
    sys.stderr.write("axes used: %d of %d; missing: %s\n"
                     % (len(AXES) - len(missing), len(AXES),
                        ",".join(missing) or "none"))
