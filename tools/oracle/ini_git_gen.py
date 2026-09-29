#!/usr/bin/env python3
"""Generate git config documents nobody chose, tagged with what is in them.

Every document carries two things besides its bytes: the set of **axes** it
exercises, and what the git config dialect should *do* with it. The second is the
third reading - the generator's own intent - and here it carries more weight than
it does in the Desktop Entry differential, because git config has only one
reference. Two implementations agreeing tells you nothing when one of them is the
only authority there is; what the intent reading adds is a statement of the rule
written down from `git-config(1)` and from measurement, independently of what
either program does. A rule this library and git both got wrong would pass
`legality` and fail `intent`.

**Every axis must appear in the run and be counted, and a zero fails the gate.**
The reason is the same one the Desktop Entry generator gives, and it is sharper
here: there is no corpus of git config files on this machine that exercises a
subsection, a continuation, a quoted value or a valueless key together, so a
generator that quietly stopped emitting one would leave the gate blind in exactly
the place the feature lives.

Copyright 2026 by Corey Pennycuff
"""

import random

# The axes, by name. Each is a construct some rule of `git-config(1)` "Syntax"
# decides. AXES is the denominator the gate checks for zeros.
AXES = (
    # ---- accepted: the ordinary shapes
    "plain",
    "section-fold",
    "key-fold",
    "key-digits",
    "key-dash",
    "section-digits",
    "section-dots",
    "section-dash",
    "tab-before-equals",
    "space-around-equals",
    "empty-value",
    "value-trailing-space",
    "value-leading-space",
    "utf8-value",
    "blank-line",
    "comment-hash",
    "comment-semicolon",
    "no-final-newline",
    "second-section",
    "indented-entry",
    "indented-header",
    # ---- accepted: subsections, both spellings
    "quoted-subsection",
    "quoted-subsection-case",
    "dotted-subsection",
    "empty-subsection",
    "subsection-bracket",
    "subsection-backslash-drops",
    "subsection-escaped-quote",
    "subsection-comment-char",
    "subsection-utf8",
    "tab-before-subsection",
    # ---- accepted: the value grammar
    "inline-comment-hash",
    "inline-comment-semicolon",
    "inline-comment-no-space",
    "quoted-value",
    "quoted-keeps-space",
    "quoted-toggle-mid",
    "quoted-then-bare",
    "two-quoted-runs",
    "quoted-protects-comment",
    "quoted-empty",
    "escape-tab",
    "escape-newline",
    "escape-backspace",
    "escape-quote",
    "escape-backslash",
    "escape-at-end",
    # ---- accepted: continuation
    "continuation",
    "continuation-in-quotes",
    "continuation-space-before",
    "continuation-at-eof",
    "comment-eats-continuation",
    # ---- accepted: the ones no other dialect here has
    "valueless-key",
    "header-remainder-entry",
    "header-remainder-valueless",
    "header-comment",
    "duplicate-key",
    "duplicate-section",
    "preamble-entry",
    "preamble-valueless",
    # ---- accepted: bytes a plausible reader gets wrong
    "crlf",
    "bom",
    "interior-cr",
    "vtab-is-content",
    "invalid-utf8-value",
    # ---- accepted, but git's answer is not a function of the bytes
    "nul-in-value",
    "nul-in-subsection",
    # ---- refused, one rule each
    "key-starts-digit",
    "key-starts-dash",
    "key-underscore",
    "key-dot",
    "key-utf8",
    "section-underscore",
    "section-utf8",
    "empty-section-name",
    "no-space-before-quote",
    "space-after-subsection",
    "two-quoted-subsections",
    "junk-after-close-bracket",
    "unterminated-header",
    "unknown-escape",
    "octal-escape",
    "backslash-space",
    "unterminated-quote",
    "valueless-key-then-comment",
    "empty-key",
    "continuation-on-header",
    "continuation-on-valueless-key",
    "cr-between-key-and-equals",
    "vtab-before-key",
    "backslash-lone-cr",
    "nul-in-key",
    "nul-in-section",
)

#
# What the git config dialect must do with a document carrying the axis, and why.
# An axis absent from here is one that does not by itself decide the verdict.
#
# The codes are this library's, and several are *less* specific than the rule
# sounds: git reports one "bad config line" for all of these, so the code is not
# under comparison - only accept versus refuse is. What it is here for is the
# intent reading, where saying "E_BAD_KEY" rather than "refused" catches a parse
# that refuses the right document for the wrong reason.
#
REFUSES = {
    # A key must begin with a letter. `-k` reaches the name check because `-` is
    # a key character; `1k` stops the scan before it.
    "key-starts-digit": "E_BAD_KEY",
    "key-starts-dash": "E_BAD_KEY",
    # These end the key scan at a byte outside the charset, and what follows is
    # then neither `=` nor the end of the line.
    "key-underscore": "E_BAD_LINE",
    "key-dot": "E_BAD_LINE",
    "key-utf8": "E_BAD_LINE",
    # A section name is `A-Za-z0-9-.` and nothing else, and the empty name is
    # refused where an empty *subsection* is not.
    "section-underscore": "E_BAD_GROUP",
    "section-utf8": "E_BAD_GROUP",
    "empty-section-name": "E_BAD_GROUP",
    # The subsection spelling is exactly `[name "sub"]`: whitespace before the
    # quote is required and nothing may follow the closing quote.
    "no-space-before-quote": "E_BAD_GROUP",
    "space-after-subsection": "E_BAD_GROUP",
    "two-quoted-subsections": "E_BAD_GROUP",
    "unterminated-header": "E_BAD_GROUP",
    # `[a]b]` - the header closes at the first `]`, and `b]` is then read as the
    # remainder, where `]` is not a key character.
    "junk-after-close-bracket": "E_BAD_LINE",
    # git's escape set is `\" \\ \n \t \b` and "other char escape sequences
    # (including octal escape sequences) are invalid".
    "unknown-escape": "E_BAD_ESCAPE",
    "octal-escape": "E_BAD_ESCAPE",
    "backslash-space": "E_BAD_ESCAPE",
    # A backslash before a lone CR is neither a continuation nor an escape: git
    # has no `\r`.
    "backslash-lone-cr": "E_BAD_ESCAPE",
    "unterminated-quote": "E_BAD_LINE",
    # Measured, and in `git-config(1)` nowhere: a valueless key may not carry a
    # trailing comment, though a header may.
    "valueless-key-then-comment": "E_BAD_LINE",
    "empty-key": "E_BAD_KEY",
    # A continuation is part of the *value* grammar, so a backslash at the end of
    # a header line or of a valueless key is just a stray byte.
    "continuation-on-header": "E_BAD_KEY",
    "continuation-on-valueless-key": "E_BAD_LINE",
    # CR is whitespace to git's top-level loop and *not* to the run between a key
    # and its `=`, which is matched against space and tab literally.
    "cr-between-key-and-equals": "E_BAD_LINE",
    # `\v` is a control character to git's own ctype table, not space, so it can
    # neither be skipped as indentation nor start a key.
    "vtab-before-key": "E_BAD_KEY",
    # A NUL is outside every name charset git has.
    "nul-in-key": "E_BAD_LINE",
    "nul-in-section": "E_BAD_GROUP",
}

#
# Axes on which git's answer is not a function of the document's bytes, with the
# reason. ini_git_diff.py excludes a document carrying one from the comparison it
# breaks, and **counts it**: an exclusion nobody counts is a hole in the
# denominator.
#
# Both are the same cause. git reads a config file with C string functions, so a
# NUL truncates whatever it is in - a value, or a subsection name - while this
# library is length-based throughout and keeps the bytes. That is a deliberate
# deviation with a test of its own, not a defect: refusing to store a NUL would
# make the library unable to represent a document git *accepts*.
#
NO_VALUE_ORACLE = ("nul-in-value", "nul-in-subsection")

SECTIONS = ("core", "user", "remote", "branch", "alias", "diff", "merge", "http")
KEYS = ("name", "email", "url", "editor", "bare", "tool", "path", "ff",
        "autocrlf", "abbrev")
SUBS = ("origin", "upstream", "my branch", "feature/x", "v1.0", "a.b")
WORDS = ("alpha", "true", "false", "42", "1.5", "/usr/bin/vim", "a value",
         "x", "")


class Gen:
    """One document per call, with the axes it used recorded."""

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.used = set()

    def mark(self, axis):
        self.used.add(axis)
        return axis

    def word(self):
        return self.rng.choice(WORDS)

    def value(self, axes):
        """A raw value, possibly carrying a value-level axis."""
        pick = self.rng.random()
        if pick < 0.05:
            axes.add(self.mark("escape-tab"))
            return "a\\tb"
        if pick < 0.09:
            axes.add(self.mark("escape-newline"))
            return "a\\nb"
        if pick < 0.12:
            axes.add(self.mark("escape-backspace"))
            return "a\\bb"
        if pick < 0.15:
            axes.add(self.mark("escape-quote"))
            return "a\\\"b"
        if pick < 0.18:
            axes.add(self.mark("escape-backslash"))
            return "a\\\\b"
        if pick < 0.21:
            # An escape at the very end is *content*, so it is not trimmed as
            # trailing whitespace even when it spells one. The discriminating case
            # for the scanner's last-content-byte tracking.
            axes.add(self.mark("escape-at-end"))
            return "a\\t"
        if pick < 0.25:
            axes.add(self.mark("quoted-value"))
            return '"%s"' % self.word()
        if pick < 0.28:
            axes.add(self.mark("quoted-keeps-space"))
            return '"  spaced  "'
        if pick < 0.31:
            axes.add(self.mark("quoted-toggle-mid"))
            return 'x" mid "y'
        if pick < 0.34:
            axes.add(self.mark("quoted-then-bare"))
            return '"a"b'
        if pick < 0.37:
            axes.add(self.mark("two-quoted-runs"))
            return '"a" "b"'
        if pick < 0.40:
            axes.add(self.mark("quoted-protects-comment"))
            return '"v # not a comment ; either"'
        if pick < 0.42:
            axes.add(self.mark("quoted-empty"))
            return '""'
        if pick < 0.45:
            axes.add(self.mark("inline-comment-hash"))
            return "%s # trailing" % (self.word() or "x")
        if pick < 0.48:
            axes.add(self.mark("inline-comment-semicolon"))
            return "%s ; trailing" % (self.word() or "x")
        if pick < 0.50:
            axes.add(self.mark("inline-comment-no-space"))
            return "%s#tight" % (self.word() or "x")
        if pick < 0.53:
            axes.add(self.mark("empty-value"))
            return ""
        if pick < 0.56:
            axes.add(self.mark("value-trailing-space"))
            return (self.word() or "x") + "   "
        if pick < 0.58:
            # Dropped by the scanner before any content is recorded, so it comes
            # back missing - which is why the writer refuses to emit one.
            axes.add(self.mark("value-leading-space"))
            return "   " + (self.word() or "x")
        if pick < 0.62:
            axes.add(self.mark("utf8-value"))
            return self.rng.choice(("café", "日本語", "\U0001F600", "straße"))
        if pick < 0.64:
            axes.add(self.mark("interior-cr"))
            return "a\rb"
        if pick < 0.66:
            axes.add(self.mark("vtab-is-content"))
            return "a\x0b"
        if pick < 0.68:
            axes.add(self.mark("nul-in-value"))
            return "a\x00b"
        if pick < 0.70:
            axes.add(self.mark("invalid-utf8-value"))
            return "a\udcffb"  # A lone surrogate; encoded with surrogateescape.
        axes.add(self.mark("plain"))
        return self.word()

    def separator(self, axes):
        pick = self.rng.random()
        if pick < 0.15:
            axes.add(self.mark("space-around-equals"))
            return "  =  "
        if pick < 0.25:
            axes.add(self.mark("tab-before-equals"))
            return "\t= "
        return " = "

    def key(self, axes):
        key = self.rng.choice(KEYS)
        pick = self.rng.random()
        if pick < 0.10:
            axes.add(self.mark("key-fold"))
            return key.capitalize()
        if pick < 0.16:
            axes.add(self.mark("key-digits"))
            return key + "2"
        if pick < 0.22:
            axes.add(self.mark("key-dash"))
            return key + "-x"
        return key

    def entry(self, axes):
        line = self.key(axes) + self.separator(axes) + self.value(axes)
        if self.rng.random() < 0.10:
            axes.add(self.mark("indented-entry"))
            line = "\t" + line
        return line

    def header(self, axes):
        """A section header, in one of the spellings git has."""
        section = self.rng.choice(SECTIONS)
        pick = self.rng.random()
        if pick < 0.08:
            axes.add(self.mark("section-fold"))
            section = section.capitalize()
        elif pick < 0.12:
            axes.add(self.mark("section-digits"))
            section = section + "2"
        elif pick < 0.16:
            axes.add(self.mark("section-dash"))
            section = section + "-x"

        pick = self.rng.random()
        if pick < 0.10:
            axes.add(self.mark("quoted-subsection"))
            head = '[%s "%s"]' % (section, self.rng.choice(SUBS))
        elif pick < 0.14:
            axes.add(self.mark("quoted-subsection-case"))
            head = '[%s "MixedCase"]' % section
        elif pick < 0.18:
            # The deprecated spelling, which lower-cases the subsection - so this
            # and a quoted `[a "SubB"]` are *different* groups.
            axes.add(self.mark("dotted-subsection"))
            head = "[%s.%s]" % (section, self.rng.choice(("sub", "SubB")))
        elif pick < 0.21:
            axes.add(self.mark("section-dots"))
            head = "[%s.a.b.c]" % section
        elif pick < 0.24:
            axes.add(self.mark("empty-subsection"))
            head = '[%s ""]' % section
        elif pick < 0.27:
            axes.add(self.mark("subsection-bracket"))
            head = '[%s "b]c["]' % section
        elif pick < 0.30:
            # A backslash inside a subsection name *drops*: `\t` is the letter t.
            # The same two bytes one line later are a tab.
            axes.add(self.mark("subsection-backslash-drops"))
            head = '[%s "x\\ty"]' % section
        elif pick < 0.33:
            axes.add(self.mark("subsection-escaped-quote"))
            head = '[%s "x\\"y"]' % section
        elif pick < 0.36:
            axes.add(self.mark("subsection-comment-char"))
            head = '[%s "a#b;c"]' % section
        elif pick < 0.39:
            axes.add(self.mark("subsection-utf8"))
            head = '[%s "café"]' % section
        elif pick < 0.42:
            axes.add(self.mark("tab-before-subsection"))
            head = '[%s\t"%s"]' % (section, self.rng.choice(SUBS))
        elif pick < 0.45:
            axes.add(self.mark("nul-in-subsection"))
            head = '[%s "a\x00b"]' % section
        else:
            head = "[%s]" % section

        if self.rng.random() < 0.08:
            axes.add(self.mark("indented-header"))
            head = "  " + head
        if self.rng.random() < 0.08:
            axes.add(self.mark("header-comment"))
            head += " ; a comment after the header"
        elif self.rng.random() < 0.08:
            axes.add(self.mark("header-remainder-entry"))
            head += " " + self.key(axes) + " = " + (self.word() or "x")
        elif self.rng.random() < 0.06:
            axes.add(self.mark("header-remainder-valueless"))
            head += " " + self.rng.choice(KEYS)
        return head

    def document(self):
        """Return (bytes, axes, expected) for one document."""
        axes = set()
        lines = []

        #
        # Three axes are properties of the whole text rather than of a line, so
        # they are chosen here and read back at the bottom of this function.
        #
        # They were marked into the generator's own `used` set at first, outside
        # the per-document `axes` set that document() reads - so every one of them
        # counted as exercised while no document carried it. The axis check caught
        # it on the first run, which is what it is for; the fix is that **one path
        # marks an axis**, and that path is the `axes` set the document carries.
        #
        if self.rng.random() < 0.05:
            axes.add(self.mark("continuation-at-eof"))
        if self.rng.random() < 0.06:
            axes.add(self.mark("bom"))
        if self.rng.random() < 0.10:
            axes.add(self.mark("crlf"))

        if self.rng.random() < 0.35:
            axes.add(self.mark("comment-hash"))
            lines.append("# a comment")
        if self.rng.random() < 0.2:
            axes.add(self.mark("comment-semicolon"))
            lines.append("; also a comment")
        if self.rng.random() < 0.25:
            axes.add(self.mark("blank-line"))
            lines.append("")

        # A preamble: git accepts a variable before any section, and names it with
        # no section prefix at all. `git-config(1)` says it cannot.
        if self.rng.random() < 0.10:
            axes.add(self.mark("preamble-entry"))
            lines.append(self.entry(axes))
        elif self.rng.random() < 0.06:
            axes.add(self.mark("preamble-valueless"))
            lines.append(self.rng.choice(KEYS))

        lines.append(self.header(axes))
        for _ in range(self.rng.randint(1, 4)):
            lines.append(self.entry(axes))
            if self.rng.random() < 0.12:
                axes.add(self.mark("comment-hash"))
                lines.append("# between entries")

        if self.rng.random() < 0.15:
            axes.add(self.mark("valueless-key"))
            lines.append(self.rng.choice(KEYS))

        if self.rng.random() < 0.12:
            # Every occurrence is a value: `--get` answers the last and
            # `--get-all` all of them, in order.
            axes.add(self.mark("duplicate-key"))
            lines.append("url = one")
            lines.append("url = two")

        if self.rng.random() < 0.25:
            axes.add(self.mark("second-section"))
            lines.append(self.header(axes))
            lines.append(self.entry(axes))

        if self.rng.random() < 0.10:
            # Repeated headers merge: `--list` prints both entries under one name.
            axes.add(self.mark("duplicate-section"))
            lines.append("[core]")
            lines.append("editor = vi")
            lines.append("[core]")
            lines.append("bare = false")

        # Continuations are applied to the text rather than built line by line,
        # because a continuation *is* the absence of a line break.
        continuation = None
        if self.rng.random() < 0.18:
            continuation = self.rng.choice((
                "continuation", "continuation-in-quotes",
                "continuation-space-before", "comment-eats-continuation"))
            axes.add(self.mark(continuation))

        expected = "ok"
        #
        # At most one refusing axis per document, so the expected error is
        # unambiguous. A document with two would be refused by whichever the
        # parser reaches first, which is a fact about the parser rather than about
        # the rule, and would make the intent reading useless.
        #
        if self.rng.random() < 0.40:
            axis = self.rng.choice(sorted(REFUSES))
            expected = REFUSES[self.mark(axis)]
            axes.add(axis)
            lines = self.inject(axis, lines)
            continuation = None

        if continuation == "continuation":
            lines.append("url = one\\")
            lines.append("two")
        elif continuation == "continuation-in-quotes":
            lines.append('url = "one\\')
            lines.append('two"')
        elif continuation == "continuation-space-before":
            lines.append("url = one \\")
            lines.append("two")
        elif continuation == "comment-eats-continuation":
            # The backslash is comment text, so the line ends here and `path` is
            # an entry of its own rather than a joined value.
            lines.append("url = one # c\\")
            lines.append("path = two")

        text = "\n".join(lines)
        if self.rng.random() < 0.10:
            axes.add(self.mark("no-final-newline"))
        else:
            text += "\n"

        if "continuation-at-eof" in axes:
            #
            # A *line of its own*, not a backslash appended to whatever the last
            # line happened to be. The first version appended one, and when the
            # last line was a valueless key or a header that made the document
            # illegal while the generator still claimed it was legal - so `intent`
            # disagreed with us on 14 documents in 20,000 and `legality` did not,
            # which is the signature of the generator being the wrong one.
            #
            text = text.rstrip("\n") + "\nurl = dangling\\"

        if "bom" in axes:
            text = "﻿" + text
        data = text.encode("utf-8", "surrogateescape")
        if "crlf" in axes:
            data = data.replace(b"\n", b"\r\n")
        return data, axes, expected

    def inject(self, axis, lines):
        """Put one refusing construct into an otherwise valid document."""
        out = list(lines)
        first = next((i for i, l in enumerate(out) if l.startswith("[")), 0) + 1
        if axis == "key-starts-digit":
            out.insert(first, "1bad = v")
        elif axis == "key-starts-dash":
            out.insert(first, "-bad = v")
        elif axis == "key-underscore":
            out.insert(first, "bad_key = v")
        elif axis == "key-dot":
            out.insert(first, "bad.key = v")
        elif axis == "key-utf8":
            out.insert(first, "kéy = v")
        elif axis == "section-underscore":
            out.append("[bad_section]")
            out.append("k = v")
        elif axis == "section-utf8":
            out.append("[café]")
            out.append("k = v")
        elif axis == "empty-section-name":
            out.append("[]")
            out.append("k = v")
        elif axis == "no-space-before-quote":
            out.append('[remote"origin"]')
            out.append("url = v")
        elif axis == "space-after-subsection":
            out.append('[remote "origin" ]')
            out.append("url = v")
        elif axis == "two-quoted-subsections":
            out.append('[remote "a" "b"]')
            out.append("url = v")
        elif axis == "junk-after-close-bracket":
            out.append("[core]b]")
            out.append("k = v")
        elif axis == "unterminated-header":
            out.append("[unterminated")
        elif axis == "unknown-escape":
            out.insert(first, "url = a\\qb")
        elif axis == "octal-escape":
            out.insert(first, "url = a\\101b")
        elif axis == "backslash-space":
            out.insert(first, "url = a\\ b")
        elif axis == "unterminated-quote":
            out.insert(first, 'url = "unterminated')
        elif axis == "valueless-key-then-comment":
            out.insert(first, "bare ; a comment")
        elif axis == "empty-key":
            out.insert(first, "= v")
        elif axis == "continuation-on-header":
            out.append("[core]\\")
            out.append("k = v")
        elif axis == "continuation-on-valueless-key":
            out.insert(first, "bare\\")
        elif axis == "cr-between-key-and-equals":
            out.insert(first, "url\r= v")
        elif axis == "vtab-before-key":
            out.insert(first, "\x0burl = v")
        elif axis == "backslash-lone-cr":
            out.insert(first, "url = a\\\rb")
        elif axis == "nul-in-key":
            out.insert(first, "ur\x00l = v")
        elif axis == "nul-in-section":
            out.append("[co\x00re]")
            out.append("k = v")
        else:
            raise AssertionError("no injection for axis %r" % axis)
        return out


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
