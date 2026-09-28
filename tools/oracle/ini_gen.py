#!/usr/bin/env python3
"""Generate Desktop Entry documents nobody chose, tagged with what is in them.

Every document carries two things besides its bytes: the set of **axes** it
exercises, and what the Desktop Entry dialect should *do* with it. The second is
the third reading - the generator's own intent - and it is what separates "the
subject is wrong" from "the generator emitted something other than what it
thinks". Two references agreeing with each other and disagreeing with what the
generator meant to emit is a real failure mode, met once already while building
the TOML oracle.

**Every axis must appear in the run and be counted, and a zero fails the gate.**
That is not ceremony. The whole reason this differential exists is that the
corpus of real files on this machine contains no duplicate key, no `;` comment,
no BOM, no CRLF and no non-ASCII name - so a generator that quietly stopped
emitting one of those would put the gate back where the corpus already is, and
print clean.

Copyright 2026 by Corey Pennycuff
"""

import random

# The axes, by name. Each is a property of a generated document that some rule of
# section 3, 4 or 5 decides. AXES is the denominator the gate checks for zeros.
AXES = (
    # Accepted shapes.
    "plain",
    "value-trailing-space",
    "value-all-space",
    "value-empty",
    "space-around-equals",
    "tab-around-equals",
    "locale-key",
    "locale-key-full",
    "escape-set",
    "list-plain",
    "list-terminated",
    "list-escaped-separator",
    "list-trailing-empty",
    "comment-hash",
    "blank-line",
    "whitespace-only-line",
    "header-trailing-space",
    "no-final-newline",
    "second-group",
    "cr-inside-value",
    "utf8-value",
    "long-value",
    # Accepted, but no reference answers the decoded string.
    "unknown-escape",
    "trailing-backslash",
    # Accepted, and the references disagree by construction.
    "nul-in-value",
    "invalid-utf8-value",
    # Refused shapes, one rule each.
    "semicolon-comment",
    "duplicate-group",
    "duplicate-key",
    "key-underscore",
    "key-dot",
    "key-space",
    "key-non-ascii",
    "orphan-locale-key",
    "indented-entry",
    "indented-comment",
    "bom",
    "crlf-header",
    "empty-group-name",
    "junk-after-header",
    "entry-before-group",
    "no-delimiter",
    "unterminated-header",
    "empty-key",
    "group-name-bracket",
    "group-name-control",
    "group-name-non-ascii",
)

# What the Desktop Entry dialect must do with a document carrying the axis. An
# axis absent from here is one that does not by itself decide the verdict.
REFUSES = {
    "semicolon-comment": "E_BAD_LINE",
    "duplicate-group": "E_DUPGROUP",
    "duplicate-key": "E_DUPKEY",
    "key-underscore": "E_BAD_KEY",
    "key-dot": "E_BAD_KEY",
    "key-space": "E_BAD_KEY",
    "key-non-ascii": "E_BAD_KEY",
    "orphan-locale-key": "E_BAD_KEY",
    "indented-entry": "E_BAD_KEY",
    "indented-comment": "E_BAD_LINE",
    "bom": "E_BAD_LINE",
    "crlf-header": "E_BAD_LINE",
    "empty-group-name": "E_BAD_GROUP",
    "junk-after-header": "E_BAD_LINE",
    "entry-before-group": "E_NO_GROUP",
    "no-delimiter": "E_BAD_LINE",
    "unterminated-header": "E_BAD_GROUP",
    "empty-key": "E_BAD_KEY",
    "group-name-bracket": "E_BAD_GROUP",
    "group-name-control": "E_BAD_GROUP",
    "group-name-non-ascii": "E_BAD_GROUP",
}

# Axes on which the two references cannot both be consulted, with the reason.
# ini_diff.py excludes a document carrying one from the comparison it breaks, and
# **counts it**: an exclusion nobody counts is a hole in the denominator.
NO_STRING_ORACLE = ("unknown-escape", "trailing-backslash")
NO_VALUE_ORACLE = ("nul-in-value",)

KEYS = ("Name", "Comment", "Exec", "Icon", "Type", "X-Extra", "GenericName",
        "TryExec", "Path", "Keywords")
LOCALES = ("de", "de_DE", "pt_BR", "sr@latin", "de_DE@euro", "en_GB")
WORDS = ("alpha", "beta", "value", "/usr/bin/true", "A Name", "42", "1.5",
         "true", "false", "", "x")


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
        if pick < 0.06:
            axes.add(self.mark("escape-set"))
            return "a\\sb\\nc\\td\\re\\\\f"
        if pick < 0.12:
            axes.add(self.mark("list-plain"))
            return "one;two;three"
        if pick < 0.16:
            axes.add(self.mark("list-terminated"))
            return "one;two;"
        if pick < 0.20:
            axes.add(self.mark("list-escaped-separator"))
            return "one\\;still-one;two"
        if pick < 0.23:
            axes.add(self.mark("list-trailing-empty"))
            return "one;two;;"
        if pick < 0.27:
            axes.add(self.mark("value-trailing-space"))
            return self.word() + "   "
        if pick < 0.30:
            axes.add(self.mark("value-all-space"))
            return "   "
        if pick < 0.33:
            axes.add(self.mark("value-empty"))
            return ""
        if pick < 0.37:
            axes.add(self.mark("unknown-escape"))
            return "a\\qb"
        if pick < 0.40:
            axes.add(self.mark("trailing-backslash"))
            return "abc\\"
        if pick < 0.43:
            axes.add(self.mark("cr-inside-value"))
            return "a\rb"
        if pick < 0.47:
            axes.add(self.mark("utf8-value"))
            return self.rng.choice(("café", "日本語",
                                    "\U0001F600", "straße"))
        if pick < 0.50:
            axes.add(self.mark("nul-in-value"))
            return "a\x00b"
        if pick < 0.53:
            axes.add(self.mark("invalid-utf8-value"))
            return "a\udcffb"  # A lone surrogate; encoded with surrogateescape.
        if pick < 0.56:
            axes.add(self.mark("long-value"))
            return "x" * self.rng.randint(1200, 4200)
        axes.add(self.mark("plain"))
        return self.word()

    def separator(self, axes):
        pick = self.rng.random()
        if pick < 0.15:
            axes.add(self.mark("space-around-equals"))
            return "  =  "
        if pick < 0.25:
            axes.add(self.mark("tab-around-equals"))
            return "\t=\t"
        return "="

    def entry(self, axes, key=None):
        if key is None:
            key = self.rng.choice(KEYS)
        return key + self.separator(axes) + self.value(axes)

    def document(self):
        """Return (bytes, axes, expected) for one document."""
        axes = set()
        lines = []
        # A leading comment block, sometimes.
        if self.rng.random() < 0.4:
            axes.add(self.mark("comment-hash"))
            lines.append("# a comment")
        if self.rng.random() < 0.3:
            axes.add(self.mark("blank-line"))
            lines.append("")
        if self.rng.random() < 0.15:
            axes.add(self.mark("whitespace-only-line"))
            lines.append("   ")

        header = "[Desktop Entry]"
        if self.rng.random() < 0.12:
            axes.add(self.mark("header-trailing-space"))
            header += "   "
        lines.append(header)

        #
        # Type and Name are required by the specification, and a document
        # missing them draws a semantic diagnostic from the validator that says
        # nothing about the grammar. Emitting them always keeps the `legality`
        # comparison about syntax, which is what it claims to be.
        #
        lines.append("Type=Application")
        lines.append("Name" + self.separator(axes) + self.value(axes))
        pool = [k for k in KEYS if k not in ("Type", "Name")]
        bare = self.rng.sample(pool, self.rng.randint(1, 4))
        for key in bare:
            lines.append(self.entry(axes, key))
            if self.rng.random() < 0.2:
                axes.add(self.mark("comment-hash"))
                lines.append("# between entries")

        # A localized key, with its bare form present (section 5 requires it).
        if self.rng.random() < 0.3:
            base = bare[0]
            locale = self.rng.choice(LOCALES)
            axes.add(self.mark("locale-key"))
            if "_" in locale and "@" in locale:
                axes.add(self.mark("locale-key-full"))
            lines.append("%s[%s]%s%s" % (base, locale, self.separator(axes),
                                         self.value(axes)))

        if self.rng.random() < 0.3:
            axes.add(self.mark("second-group"))
            #
            # An action group the main group does not list draws "action group
            # exists, but there is no matching action" from the validator - a
            # semantic complaint that says nothing about the grammar. Declaring it
            # keeps the document valid so that the legality comparison is about
            # syntax. Found by the differential's unclassified-diagnostic check,
            # which is why that check refuses to treat an unknown message as
            # agreement.
            #
            lines.append("Actions=Open;")
            lines.append("")
            lines.append("[Desktop Action Open]")
            lines.append(self.entry(axes, "Name"))

        expected = "ok"
        # At most one refusing axis per document, so the expected error is
        # unambiguous. A document with two would be refused by whichever the
        # parser reaches first, which is a fact about the parser rather than
        # about the rule, and would make the intent reading useless.
        if self.rng.random() < 0.45:
            axis = self.rng.choice(sorted(REFUSES))
            expected = REFUSES[self.mark(axis)]
            axes.add(axis)
            lines = self.inject(axis, lines, bare)

        text = "\n".join(lines)
        if self.rng.random() < 0.12:
            axes.add(self.mark("no-final-newline"))
        else:
            text += "\n"

        if "bom" in axes:
            text = "﻿" + text
        data = text.encode("utf-8", "surrogateescape")
        if "crlf-header" in axes:
            data = data.replace(b"[Desktop Entry]", b"[Desktop Entry]\r", 1)
        return data, axes, expected

    def inject(self, axis, lines, bare):
        """Put one refusing construct into an otherwise valid document."""
        out = list(lines)
        first_entry = next(i for i, l in enumerate(out) if "=" in l)
        if axis == "semicolon-comment":
            out.insert(first_entry, "; a semicolon comment")
        elif axis == "duplicate-group":
            out.append("[Desktop Entry]")
            out.append("X-Second=1")
        elif axis == "duplicate-key":
            out.insert(first_entry + 1, bare[0] + "=again")
        elif axis == "key-underscore":
            out.insert(first_entry, "bad_key=v")
        elif axis == "key-dot":
            out.insert(first_entry, "bad.key=v")
        elif axis == "key-space":
            out.insert(first_entry, "bad key=v")
        elif axis == "key-non-ascii":
            out.insert(first_entry, "kéy=v")
        elif axis == "orphan-locale-key":
            #
            # `Comment`, not an `X-` key: the validator enforces section 5's
            # "a postfixed key must have its bare form" only for keys it knows to
            # be localestrings, so an orphan `X-Whatever[de]` draws no diagnostic
            # and the comparison would have nothing to compare. Found by the
            # differential's own legality score, which is what it is for.
            #
            out = [l for l in out if not l.startswith("Comment")]
            out.append("Comment[de]=orphan")
        elif axis == "indented-entry":
            out.insert(first_entry, "  Indented=v")
        elif axis == "indented-comment":
            out.insert(first_entry, "  # indented comment")
        elif axis == "bom":
            pass  # Applied to the whole text by the caller.
        elif axis == "crlf-header":
            pass  # Applied to the bytes by the caller.
        elif axis == "empty-group-name":
            out.append("[]")
            out.append("X-In-Empty=1")
        elif axis == "junk-after-header":
            out.append("[Other] junk")
        elif axis == "entry-before-group":
            out.insert(0, "Before=1")
        elif axis == "no-delimiter":
            out.insert(first_entry, "no-delimiter-here")
        elif axis == "unterminated-header":
            out.append("[Unterminated")
        elif axis == "empty-key":
            out.insert(first_entry, "=v")
        elif axis == "group-name-bracket":
            out.append("[Bad[Name]")
            out.append("X-In-Bad=1")
        elif axis == "group-name-control":
            out.append("[Bad\x01Name]")
            out.append("X-In-Bad=1")
        elif axis == "group-name-non-ascii":
            out.append("[Café]")
            out.append("X-In-Bad=1")
        else:
            raise AssertionError("no injection for axis %r" % axis)
        return out


def documents(seed, count):
    """Yield (bytes, axes, expected) `count` times."""
    gen = Gen(seed)
    for _ in range(count):
        yield gen.document()
    # The axes the generator actually used, for the gate's zero check.
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
