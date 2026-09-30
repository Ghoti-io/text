#!/usr/bin/env python3
"""Generate systemd unit documents nobody chose, tagged with what is in them.

Every document is a **valid unit skeleton** with the construct under test added,
because `systemd-analyze verify` has to get far enough to parse before it will say
anything, and it refuses a service with no `ExecStart=` before reporting on the rest.

The value under test always goes on **`Environment=`**, and that is the only channel
there is: no `systemd-analyze` verb prints a parsed setting, but `Environment=`
validates each word as `NAME=VALUE` and reports a failure *verbatim* - one message
per word, after unquoting, unescaping and word splitting. So every word this
generator emits is deliberately **not** a valid assignment, or systemd would accept
it silently and the oracle would go quiet. A word with no `=` is invalid; so is one
whose name starts with a digit, which is how the `value-with-equals` axis stays
visible.

**Every axis must appear in the run and be counted, and a zero fails the gate.** The
reason is sharper here than for any other dialect: of the 164 unit files on this
machine, **zero** use a line continuation or a `;` comment, so the two rules the
whole dialect turns on have no instances outside what this file makes.

Copyright 2026 by Corey Pennycuff
"""

import random

HEAD = "[Unit]\nDescription=p\n\n[Service]\nExecStart=/bin/true\n"
# Spelled by codepoint rather than pasted, so that an invisible character never
# sits in this file where a reader cannot see it.
BOM = chr(0xFEFF)

AXES = (
    # ---- the value grammar, echoed word by word
    "plain",
    "two-words",
    "quoted-double",
    "quoted-single",
    "quote-toggle-mid",
    "quote-then-bare",
    "quote-mid-word",
    "two-quoted-runs",
    "quoted-empty",
    "quote-unclosed",
    "single-in-double",
    "double-in-single",
    "esc-tab",
    "esc-space",
    "esc-backslash",
    "esc-quote",
    "esc-single",
    "esc-alert",
    "esc-backspace",
    "esc-formfeed",
    "esc-vtab",
    "esc-newline",
    "esc-in-single-quotes",
    "esc-unknown-letter",
    "esc-hex",
    "esc-octal",
    "esc-u4",
    "esc-U8",
    "esc-bad-octal",
    "esc-bad-hex",
    "esc-nul",
    "esc-surrogate",
    "esc-cr",
    "long-value",
    "value-with-equals",
    "value-hash-tight",
    "value-hash-spaced",
    "value-semi-spaced",
    "tabs-between-words",
    "literal-vtab",
    "literal-formfeed",
    "empty-value",
    "utf8-value",
    # ---- the line grammar
    "continuation",
    "continuation-flush",
    "continuation-in-quotes",
    "continuation-over-hash",
    "continuation-over-semi",
    "continuation-chained",
    "continuation-blank-ends",
    "continuation-at-eof",
    "continuation-in-header",
    "continuation-in-key",
    "comment-hash",
    "comment-semicolon",
    "comment-indented",
    "comment-trailing-backslash",
    "blank-line",
    "indented-entry",
    "space-around-equals",
    "tab-around-equals",
    "vtab-before-equals",
    "vtab-indent",
    "trailing-whitespace",
    "key-with-space",
    "key-with-dash",
    "duplicate-key",
    "duplicate-section",
    "second-section",
    "section-with-space",
    "header-trailing-space",
    "indented-header",
    "crlf",
    "lone-cr",
    "bom",
    "bom-before-comment",
    "no-final-newline",
    # ---- refused
    "no-separator",
    "empty-key",
    "header-no-close",
    "header-remainder",
    "header-remainder-assignment",
    "preamble-entry",
)

# What specification 0.17.2 of systemd's syntax - and measurement - says this module
# reports. systemd itself **warns and skips** every one of these and keeps the file;
# this module refuses the document, which @ref format_ini records as a deliberate
# departure. The differential compares the *presence* of a grammar fault, which is
# the one thing both agree on.
REFUSES = {
    "no-separator": "E_BAD_LINE",
    "empty-key": "E_BAD_KEY",
    "header-no-close": "E_BAD_GROUP",
    "header-remainder": "E_BAD_LINE",
    "header-remainder-assignment": "E_BAD_LINE",
    "preamble-entry": "E_NO_GROUP",
}

# Where **systemd itself is wrong** and the gate asserts that it still is, per axis,
# rather than excluding the document and losing the knowledge.
#
# `bom-before-comment` is a systemd defect, pinned down by six probes: the byte-order
# mark is skipped **after** the comment test and after the leading-whitespace skip, so
# a first line of `<BOM># c` is not seen as a comment. It then falls to the assignment
# branch, which reports "Assignment outside of section." before it has even looked for
# an `=`. Measured: `<BOM>[Unit]` is fine and `<BOM>` alone is fine, while `<BOM># c`,
# `<BOM>xyz`, `<BOM>k=v` and `<BOM>   # c` all draw the spurious complaint. This module
# skips the mark properly, as every other dialect here does and as the specification's
# silence allows.
REFERENCE_GRAMMAR_DIVERGES = {
    "bom-before-comment": "skips the BOM after the comment test, so the first line "
                          "is read as an assignment outside any section",
}

# Where systemd's *reporting channel* cannot carry the answer, so the words score
# has no oracle. Counted, not ignored.
#
#   - a CR in a word: the logger rewrites it to an LF, measured by minimal pair -
#     all four spellings of a carriage return arrive as 0x0a while an 0x0e escape
#     arrives as 0x0e, so the rewriting is the logger's and not the decoder's;
#   - a long word: the logger truncates the message at 2,097 bytes.
#
# Both axes are still **generated**, so the exclusion has something to exclude and
# the count means something. A table naming an axis nothing emits would be a hole
# pretending to be a decision.
NO_WORD_ORACLE = ("esc-cr", "long-value")

# Words that are never a valid `NAME=VALUE`, so systemd always echoes them.
WORDS = ("one", "two", "alpha", "x", "42", "/usr/bin/true")


class Gen:
    """One document per call, with the axes it used recorded."""

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.used = set()
        self.refusal = None
        self.at_eof = False

    def mark(self, axis, axes):
        self.used.add(axis)
        axes.add(axis)
        # The **first** refusing construct in document order. A parse stops at its
        # first fault, and a document can carry two.
        if self.refusal is None and axis in REFUSES:
            self.refusal = REFUSES[axis]
        return axis

    def value(self, axes):
        """A raw value for `Environment=`, carrying a value-level axis."""
        pick = self.rng.random()
        table = (
            (0.03, "two-words", "one two"),
            (0.06, "quoted-double", 'a "b c" d'),
            (0.09, "quoted-single", "a 'b c' d"),
            (0.11, "quote-toggle-mid", 'x"y z"'),
            (0.13, "quote-then-bare", '"a"b'),
            (0.15, "quote-mid-word", 'a"b c"'),
            (0.17, "two-quoted-runs", '"a" "b"'),
            (0.19, "quoted-empty", '"" x'),
            (0.21, "quote-unclosed", '"a b'),
            (0.23, "single-in-double", '"a\'b"'),
            (0.25, "double-in-single", "'a\"b'"),
            (0.27, "esc-tab", "a\\tb"),
            (0.29, "esc-space", "a\\sb"),
            (0.31, "esc-backslash", "a\\\\b"),
            (0.33, "esc-quote", 'a\\"b'),
            (0.35, "esc-single", "a\\'b"),
            (0.37, "esc-alert", "a\\ab"),
            (0.39, "esc-backspace", "a\\bb"),
            (0.41, "esc-formfeed", "a\\fb"),
            (0.43, "esc-vtab", "a\\vb"),
            (0.45, "esc-newline", "a\\nb"),
            (0.47, "esc-in-single-quotes", "'a\\tb'"),
            (0.49, "esc-unknown-letter", "a\\qb"),
            (0.51, "esc-hex", "a\\x41b"),
            (0.53, "esc-octal", "a\\101b"),
            (0.55, "esc-u4", "a\\u00e9b"),
            (0.57, "esc-U8", "a\\U0001F600b"),
            (0.59, "esc-bad-octal", "a\\10b"),
            (0.61, "esc-bad-hex", "a\\xZZb"),
            (0.63, "esc-nul", "a\\x00b"),
            (0.65, "esc-surrogate", "a\\ud800b"),
            # Both of these are generated so that NO_WORD_ORACLE has something to
            # exclude: a decoded CR is rewritten by systemd's logger, and a word over
            # 2,097 bytes is truncated by it.
            (0.655, "esc-cr", "a\\rb"),
            (0.66, "long-value", "L" * 2500),
            # `1=x` is an assignment whose name starts with a digit, so systemd
            # refuses it as an assignment and therefore echoes it - which is how this
            # axis stays visible at all.
            (0.67, "value-with-equals", "1=x"),
            (0.69, "value-hash-tight", "a#b"),
            (0.71, "value-hash-spaced", "one # two"),
            (0.73, "value-semi-spaced", "one ; two"),
            (0.75, "tabs-between-words", "one\ttwo"),
            (0.77, "literal-vtab", "a\vb"),
            (0.79, "literal-formfeed", "a\fb"),
            (0.81, "empty-value", ""),
            (0.84, "utf8-value", "café"),
        )
        for bound, axis, text in table:
            if pick < bound:
                self.mark(axis, axes)
                return text
        self.mark("plain", axes)
        return self.rng.choice(WORDS)

    def entry(self, axes):
        """One `Environment=` line, or a line the specification refuses."""
        pick = self.rng.random()
        if pick < 0.03:
            self.mark("no-separator", axes)
            return "bare\n"
        if pick < 0.06:
            self.mark("empty-key", axes)
            return "=one\n"

        key = "Environment"
        pick = self.rng.random()
        if pick < 0.04:
            self.mark("key-with-space", axes)
            key = "Environ ment"
        elif pick < 0.08:
            self.mark("key-with-dash", axes)
            key = "Environment-x"
        elif pick < 0.12:
            self.mark("continuation-in-key", axes)
            key = "Environ\\\nment"

        value = self.value(axes)
        pick = self.rng.random()
        if pick < 0.08:
            self.mark("space-around-equals", axes)
            sep = " = "
        elif pick < 0.14:
            self.mark("tab-around-equals", axes)
            sep = "\t=\t"
        elif pick < 0.18:
            # A vertical tab is **not** whitespace to systemd, so this leaves it in
            # the key and the setting becomes unknown - which is a semantic
            # complaint, not a grammar one.
            self.mark("vtab-before-equals", axes)
            sep = "\v="
        else:
            sep = "="

        line = "%s%s%s" % (key, sep, value)
        pick = self.rng.random()
        if pick < 0.06:
            self.mark("indented-entry", axes)
            line = "   " + line
        elif pick < 0.10:
            self.mark("vtab-indent", axes)
            line = "\v" + line
        if self.rng.random() < 0.08:
            self.mark("trailing-whitespace", axes)
            line = line + "   "

        # The continuations, which only make sense on a line that has a value.
        pick = self.rng.random()
        if value and pick < 0.05:
            self.mark("continuation", axes)
            return "%s%s%s \\\n\tmore\n" % (key, sep, value)
        if value and pick < 0.09:
            self.mark("continuation-flush", axes)
            return "%s%s%s\\\nmore\n" % (key, sep, value)
        if pick < 0.12:
            self.mark("continuation-in-quotes", axes)
            return '%s%s"W1\\\nW2"\n' % (key, sep)
        if value and pick < 0.15:
            self.mark("continuation-over-hash", axes)
            return "%s%s%s\\\n# a comment\nmore\n" % (key, sep, value)
        if value and pick < 0.18:
            self.mark("continuation-over-semi", axes)
            return "%s%s%s\\\n; a comment\nmore\n" % (key, sep, value)
        if value and pick < 0.21:
            self.mark("continuation-chained", axes)
            return "%s%s%s \\\nmid \\\nend\n" % (key, sep, value)
        if value and pick < 0.24:
            self.mark("continuation-blank-ends", axes)
            return "%s%s%s\\\n\nEnvironment=after\n" % (key, sep, value)
        if value and pick < 0.27:
            # The last line of the document, with nothing after the backslash. The
            # caller stops emitting after this: a following `Environment=` line would
            # be **joined on** and would then look like a valid assignment, which
            # systemd accepts silently - so the oracle would go quiet about a word we
            # still report.
            self.mark("continuation-at-eof", axes)
            self.at_eof = True
            return "%s%s%s\\\n" % (key, sep, value)
        return line + "\n"

    def document(self):
        axes = set()
        self.refusal = None
        rng = self.rng
        out = []

        # A byte-order mark, and whether anything precedes the first section header.
        # Two axes rather than one, because systemd answers them differently - see
        # REFERENCE_GRAMMAR_DIVERGES - and a document carrying both a known divergence
        # and an agreement can be scored as neither.
        bom = rng.random() < 0.10
        leading = rng.random() < 0.5 if bom else True
        if bom:
            self.mark("bom-before-comment" if leading else "bom", axes)
            out.append(BOM)
        # With a mark and no leading line the next thing **must** be the section
        # header, because that is the one case systemd gets right. Gating only the
        # hash-comment emitter left the other three free to follow a mark, and six of
        # the first 375 `grammar` comparisons failed on exactly that - the document was
        # marked as an agreement and carried the divergence anyway.
        allow_leading = leading
        # A preamble entry and an unknown section are kept apart: to systemd an unknown
        # section makes every later assignment "outside of section", which is the same
        # words a genuine preamble entry draws, and nothing in the message tells them
        # apart. Two axes that collide are separated here rather than disentangled
        # downstream.
        unknown_section_allowed = True
        if not bom and rng.random() < 0.08:
            self.mark("preamble-entry", axes)
            out.append("Environment=before\n")
            unknown_section_allowed = False
        if allow_leading and (bom or rng.random() < 0.10):
            self.mark("comment-hash", axes)
            out.append("# a leading comment\n")
        if allow_leading and rng.random() < 0.10:
            self.mark("comment-semicolon", axes)
            out.append("; a leading comment\n")
        if allow_leading and rng.random() < 0.08:
            self.mark("comment-indented", axes)
            out.append("   # an indented comment\n")
        if allow_leading and rng.random() < 0.06:
            # Measured: a comment's own trailing backslash does **not** continue it.
            self.mark("comment-trailing-backslash", axes)
            out.append("# a comment \\\n")

        out.append(HEAD)

        pick = rng.random()
        if not unknown_section_allowed and pick < 0.34:
            pick = 0.9  # No extra section; see unknown_section_allowed above.
        if pick < 0.05:
            self.mark("header-no-close", axes)
            out.append("[Install\n")
        elif pick < 0.08:
            self.mark("header-remainder", axes)
            out.append("[Install] junk\n")
        elif pick < 0.10:
            # A remainder that **is** an assignment. `[Install] junk` is refused by
            # this module whatever ::header_remainder_is_entry says, because a
            # remainder with no `=` is a bad line either way - so flipping that flag
            # moved no score until this axis existed. The blind spot was the
            # generator's, not the gate's.
            self.mark("header-remainder-assignment", axes)
            out.append("[Install] WantedBy=multi-user.target\n")
        elif pick < 0.14:
            self.mark("continuation-in-header", axes)
            out.append("[Ins\\\ntall]\n")
            out.append("WantedBy=multi-user.target\n")
        elif pick < 0.18:
            self.mark("section-with-space", axes)
            # No assignment after it: to systemd an *unknown* section makes every
            # following assignment "outside of section", which is a semantic
            # consequence wearing a grammar fault's words. Not generating the
            # ambiguity is better than teaching the classifier to see through it.
            out.append("[Ins tall]\n")
        elif pick < 0.22:
            self.mark("indented-header", axes)
            out.append("   [Install]\n")
            out.append("WantedBy=multi-user.target\n")
        elif pick < 0.26:
            self.mark("header-trailing-space", axes)
            out.append("[Install]   \n")
            out.append("WantedBy=multi-user.target\n")
        elif pick < 0.30:
            self.mark("duplicate-section", axes)
            out.append("[Service]\n")
        elif pick < 0.34:
            self.mark("second-section", axes)
            out.append("[Install]\nWantedBy=multi-user.target\n[Service]\n")

        # Back into [Service] before the entries under test. `Environment=` is the
        # only channel that echoes a parsed value, and it echoes one **only in the
        # section that defines it** - left in [Install] it is an unknown key and the
        # oracle goes silent, which cost 33 of the first 138 words comparisons.
        if out[-1] != HEAD:
            out.append("[Service]\n")
        if rng.random() < 0.12:
            self.mark("blank-line", axes)
            out.append("\n")
        self.at_eof = False
        for _ in range(rng.randint(1, 2)):
            out.append(self.entry(axes))
            if self.at_eof:
                break
        if not self.at_eof and rng.random() < 0.12:
            self.mark("duplicate-key", axes)
            out.append("Environment=again\n")

        text = "".join(out)
        if rng.random() < 0.08:
            self.mark("crlf", axes)
            text = text.replace("\n", "\r\n")
        elif rng.random() < 0.06:
            # A lone CR is a terminator to systemd and to nothing else here. Kept
            # apart from `crlf`, which would turn every one of these into a CRLF and
            # leave the axis marked but unexercised.
            self.mark("lone-cr", axes)
            text = text.replace("\n", "\r")
        if rng.random() < 0.08:
            self.mark("no-final-newline", axes)
            text = text.rstrip("\n\r")

        expected = self.refusal or "ok"
        return text.encode("utf-8", "surrogateescape"), axes, expected


def documents(seed, count):
    """Yield (bytes, axes, expected) `count` times."""
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
    for data, axes, expected in documents(seed, count):
        tally.update(axes)
        sys.stdout.write("--- expected %s axes %s\n"
                         % (expected, ",".join(sorted(axes))))
        sys.stdout.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    missing = [a for a in AXES if not tally[a]]
    sys.stderr.write("axes used: %d of %d; missing: %s\n"
                     % (len(AXES) - len(missing), len(AXES),
                        ",".join(missing) or "none"))
