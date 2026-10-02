#!/usr/bin/env python3
"""Generate `configparser` documents nobody chose, tagged with what is in them.

**This dialect has no specification**, so this generator is the only place the rules
are written down in a form something can be checked against - which makes its
`REFUSES` table a third reading rather than a convenience. Two readers agreeing with
each other while both disagreeing with what the generator meant to emit is a failure
mode this repository has met, and for a dialect whose reference *is* the
specification it is the only way a shared mistake becomes visible at all.

**Every axis must appear in the run and be counted, and a zero fails the gate.** The
local corpus is unusually good here - 703 real files, 479 of them documents - and it
still contains **no lone CR, no byte-order mark and no `[DEFAULT]` section**, and one
CRLF file out of 703. Those are what this makes.

Spelled by codepoint rather than pasted wherever a character is invisible, so that no
run depends on a byte a reader of this file cannot see.

Copyright 2026 by Corey Pennycuff
"""

import random

BOM = chr(0xFEFF)
NBSP = chr(0x00A0)      # Python calls this whitespace; a byte reader cannot.
E_ACUTE_UPPER = chr(0x00C9)  # `str.lower()` folds it; an ASCII fold does not.

AXES = (
    # ---- the separator and the key
    "sep-eq",
    "sep-colon",
    "colon-then-eq",
    "eq-then-colon",
    "sep-repeated",
    "key-space",
    "key-upper",
    "key-bracket",
    "key-hash",
    "key-semi",
    "key-tab-before-sep",
    "key-fs-before-sep",
    "key-dotted",
    "empty-key",
    "key-only-whitespace",
    "no-separator",
    # ---- the value
    "value-plain",
    "value-empty",
    "value-only-whitespace",
    "value-trailing-space",
    "value-quoted",
    "value-inline-semi",
    "value-inline-hash",
    "value-leading-semi",
    "value-leading-hash",
    "value-percent",
    "value-percent-pair",
    "value-interpolation",
    "value-extended-interpolation",
    "value-backslash",
    "value-backslash-at-eol",
    "value-vtab",
    "value-formfeed",
    "value-fs",
    "value-nul",
    "value-long",
    "value-utf8",
    # ---- the continuation
    "cont-space",
    "cont-tab",
    "cont-vtab",
    "cont-fs",
    "cont-two-lines",
    "cont-blank-between",
    "cont-comment-between",
    "cont-trailing-blank",
    "cont-trailing-comment",
    "cont-empty-first-line",
    "cont-looks-like-entry",
    "cont-looks-like-header",
    "cont-deeper-then-shallower",
    "cont-at-eof",
    "entry-indented",
    "entry-indented-pair",
    "cont-indent-equal",
    "cont-after-header",
    "cont-after-no-separator",
    # ---- the header
    "header-plain",
    "header-space-inside",
    "header-bracket-inside",
    "header-hash-inside",
    "header-semi-inside",
    "header-whitespace-name",
    "header-remainder",
    "header-remainder-assignment",
    "header-remainder-comment",
    "header-case-pair",
    "header-default-section",
    # ---- the defaults chain: a [DEFAULT] another section inherits from
    "default-basic-reference",
    "default-extended-reference",
    "default-extended-path",
    "default-overridden",
    "header-empty",
    "header-no-close",
    "header-duplicate",
    # ---- the document
    "preamble-entry",
    "leading-comments",
    "comment-hash",
    "comment-semi",
    "comment-indented",
    "blank-lines",
    "trailing-comment",
    "duplicate-key",
    "duplicate-key-folded",
    "bom",
    "crlf",
    "lone-cr",
    "lone-cr-in-value",
    "cr-at-eof",
    # ---- where the reference is Unicode-aware and this reader is not
    "nbsp-value-boundary",
    "nbsp-indent",
    "nonascii-upper-key",
    # ---- where the reference's channel cannot carry the document at all
    "invalid-utf8",
)

# What this module must refuse, and with which status. The generator's own reading of
# the dialect, independent of both implementations - the `intent` score.
REFUSES = {
    "empty-key": "E_BAD_KEY",
    "key-only-whitespace": "E_BAD_KEY",
    "no-separator": "E_BAD_LINE",
    "header-empty": "E_BAD_GROUP",
    "header-no-close": "E_BAD_GROUP",
    "header-duplicate": "E_DUPGROUP",
    "preamble-entry": "E_NO_GROUP",
    "duplicate-key": "E_DUPKEY",
    "duplicate-key-folded": "E_DUPKEY",
    "bom": "E_BAD_LINE",
    "lone-cr-in-value": "E_BAD_LINE",
    "cont-indent-equal": "E_BAD_LINE",
    "cont-after-header": "E_BAD_LINE",
    "cont-after-no-separator": "E_BAD_LINE",
}

# Where **this reader diverges on purpose** and the gate asserts the departure is
# still observable, per axis, rather than excluding the document and losing the
# knowledge. Both are one thing: `configparser` works on Python `str`, so its
# whitespace and its case folding are Unicode's, and a byte-oriented reader cannot ask
# either question of one byte. @ref format_ini states them; these axes prove they are
# still there, so that a change to the fold or the whitespace set fails loudly instead
# of quietly inflating `values`.
# Each entry is `(what the reference does, what this module does about the document)`.
# The second field is what lets `intent` score a divergent document rather than skip
# one: two of these three change a *value* and the document is accepted either way,
# while `nbsp-indent` changes the **verdict** - the reference reads a continuation
# where this reader reads an entry with no separator, and refuses. A table with only
# the first field made `intent` call that refusal a defect.
REFERENCE_DIVERGES = {
    "nbsp-value-boundary": ("strips a no-break space from a value's edge, which a byte "
                            "reader cannot see as whitespace", None),
    "nbsp-indent": ("counts a no-break space as indentation, so the line continues",
                    "E_BAD_LINE"),
    "nonascii-upper-key": ("lower-cases a non-ASCII letter in a key, which an ASCII "
                           "fold leaves alone", None),
}

# Where the **pinned configuration is load-bearing**, per axis: which non-default
# configuration of the reference the departure lives in, and what it does there.
#
# `interpolation=None` is the one pin that a printed line cannot discharge, and the
# reason is that a pin is **wider than an exclusion**. An exclusion names a document
# and keeps the knowledge; a pin removes a behaviour from the comparison entirely, so
# for as long as this table did not exist nothing in this gate would have failed if
# this module's handling of `%` changed, or if the reference's had. The axes were
# already generated and already scored - under a configuration in which both sides
# answer `100%` with `100%`, which is agreement about nothing.
#
# So `tools/oracle/ini_cp_diff.py` runs the reference twice more and asserts, per axis,
# that its *default* still answers differently from the pinned one. Three shapes:
#
#   refuse  the reference refuses a document the pinned configuration accepts
#   change  it accepts and returns a different value
#   differ  either, because which one depends on what the document happens to hold -
#           `%(alpha)s` resolves when `alpha` is in the section and raises when it is
#           not, and the generator does not choose
#
# **`value-extended-interpolation` needs the third configuration and not the default**,
# measured: `BasicInterpolation` leaves `${sect:alpha}` alone, because `$` is not its
# trigger byte. An assertion that looked for a divergence under `basic` for every axis
# in this table would have failed on that one and been right to.
PIN_INTERPOLATION = {
    "value-percent": ("basic", "refuse"),
    "value-percent-pair": ("basic", "change"),
    "value-interpolation": ("basic", "differ"),
    "value-extended-interpolation": ("extended", "differ"),
}

# The defaults chain, which the pinned configuration excludes **by construction** and
# not by naming a document.
#
# `default_section` is pinned to a name no document can spell, so `[DEFAULT]` is an
# ordinary section on both sides and there is no inheritance in the gate at all. That
# pin is correct - value inheritance is a lookup policy over a parsed tree rather than a
# rule of the grammar, and leaving it on would make every section's item list include
# another section's keys - but it took a *rule* out of scope with it, and §22.7 of
# notes/text/INI-DIALECTS.md is what that cost: `${sect:key}` did not consult the
# defaults, the comment over the wrong code said "measured rather than inferred from
# symmetry", and it had been inferred. No instrument could have caught it. The unit test
# that carries it now was written from a probe, so if the reference changed, nothing
# would fail.
#
# So these four documents are generated for a **fifth and sixth configuration** of the
# reference, in which `default_section` is `DEFAULT` and the inheritance is live. Each
# entry is `(which interpolation style reaches it, what the live configuration does
# that the pinned one does not)`:
#
#   resolve  the pinned configuration raises InterpolationMissingOptionError, because
#            the key is in a section it cannot see; the live one resolves it
#
# **`default-overridden` is deliberately not here**, and the reason is worth keeping:
# a section holding its own copy of the key resolves to that copy under *both*
# configurations, so it asserts nothing about the pin. What it does assert is
# precedence - that the defaults group does not shadow the section's own entry - which
# is a correctness claim the `defaults` score makes and the pin assertion cannot.
DEFAULTS_CHAIN = {
    "default-basic-reference": ("basic", "resolve"),
    "default-extended-reference": ("extended", "resolve"),
    "default-extended-path": ("extended", "resolve"),
}

# Every axis the `defaults` score is scored over, which is the table above plus the
# precedence case. Named once, because a score and its denominator drifting apart is
# how `interp` came to report 625 of 756 while being right about all of them.
DEFAULTS_AXES = tuple(DEFAULTS_CHAIN) + ("default-overridden",)

# Where the reference's **channel** cannot carry the document, so there is no oracle
# for it - not a disagreement about the grammar. `read()` decodes as UTF-8 and raises
# `UnicodeDecodeError` before `configparser` sees a single line, while this reader is
# byte-oriented and has no opinion. Still generated, so the exclusion has something to
# exclude and its count means something.
NO_ORACLE = ("invalid-utf8",)

KEYS = ("alpha", "beta", "gamma", "delta", "opt")
VALUES = ("one", "two", "a value", "42", "/usr/bin/true")


class Gen:
    """One document per call, with the axes it used recorded.

    **Each document carries at most one special construct**, and that is the shape the
    first version did not have. It offered every axis from every table independently,
    and three collisions followed, each of which looked like a library defect until it
    was read:

      - A refusing axis landing beside a *divergence* axis made the divergence
        unobservable: both sides refused the document, so the departure the exclusion
        exists to record could not be seen, and the gate reported the axis as never
        observed while also failing the verdict score. Two symptoms, one cause.
      - Two refusing axes in one document made the `intent` expectation a guess about
        which fault a parser reaches first, which is a claim about implementation
        order rather than about the dialect.
      - A key drawn at random twice in one section is a **duplicate key**, which this
        dialect refuses - so a document meant to be clean was refused for a reason the
        generator had not recorded.

    So a mode is chosen first and the tables offer only what that mode allows: `clean`
    documents carry nothing special, `refusing` documents carry exactly one refusing
    construct, and `divergent` documents carry exactly one construct the two readers
    answer differently. Coverage does not suffer - every axis is still counted and a
    zero still fails the gate - and each score now has a population whose members
    cannot spoil one another.
    """

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.used = set()
        self.refusal = None
        self.mode = "clean"
        self.special = None
        self.special_spent = False
        self.keys_used = set()

    def mark(self, axis, axes):
        self.used.add(axis)
        axes.add(axis)
        if self.refusal is None and axis in REFUSES:
            self.refusal = REFUSES[axis]
        if self.refusal is None and axis in REFERENCE_DIVERGES:
            self.refusal = REFERENCE_DIVERGES[axis][1]
        return axis

    def wants(self, axis):
        """Whether this document is the one carrying @p axis.

        A refusing or divergent axis appears only in a document whose mode chose it,
        which is what keeps one from hiding another.
        """
        return self.special == axis

    def fresh_key(self):
        """A key no other entry in this document has used.

        Duplicate keys are a refusal here, so a clean document must not produce one by
        accident - and drawing from a five-name list twice in one section did.
        """
        for name in KEYS:
            if name not in self.keys_used:
                self.keys_used.add(name)
                return name
        name = "opt%d" % len(self.keys_used)
        self.keys_used.add(name)
        return name

    def key(self, axes):
        if self.wants("nonascii-upper-key") and not self.special_spent:
            # **Once per document.** Returning it for every entry gave two entries the
            # same key, which this dialect refuses - so both readers refused the
            # document, the bodies were both empty, and the divergence the axis exists
            # to record could not be seen. The gate said "the reference no longer
            # diverges here", which was true of that document and not of the rule.
            self.special_spent = True
            self.mark("nonascii-upper-key", axes)
            return "OPT" + E_ACUTE_UPPER
        pick = self.rng.random()
        table = (
            (0.06, "key-space", "opt ion"),
            (0.12, "key-upper", "OptIon"),
            (0.18, "key-bracket", "opt[x]"),
            (0.24, "key-hash", "opt#x"),
            (0.30, "key-semi", "opt;x"),
            (0.36, "key-dotted", "opt.ion"),
            (0.42, "key-tab-before-sep", "opt\t"),
            (0.48, "key-fs-before-sep", "opt\x1c"),
        )
        for bound, axis, text in table:
            if pick < bound:
                # Each of these is one fixed spelling, so two entries carrying the
                # same axis in one section would be a duplicate key. Taken once.
                if text.strip(" \t\x1c") in self.keys_used:
                    break
                self.keys_used.add(text.strip(" \t\x1c"))
                self.mark(axis, axes)
                return text
        return self.fresh_key()

    def value(self, axes):
        """A one-line raw value, carrying a value-level axis."""
        if self.wants("nbsp-value-boundary") and not self.special_spent:
            self.special_spent = True
            self.mark("nbsp-value-boundary", axes)
            return NBSP + "v" + NBSP
        if self.wants("invalid-utf8"):
            # **Inside a value**, so that this module accepts the document and only the
            # reference's *channel* fails: `read()` decodes as UTF-8 and raises before
            # `configparser` sees a line. Appended after the last terminator instead,
            # the bytes became a line of their own with no separator and this module
            # refused too - which is a disagreement about nothing, not a channel limit.
            self.mark("invalid-utf8", axes)
            return "�INVALID�"
        pick = self.rng.random()
        table = (
            (0.03, "value-empty", ""),
            (0.06, "value-only-whitespace", "   "),
            (0.09, "value-trailing-space", "v   "),
            (0.13, "value-quoted", '"q v"'),
            (0.17, "value-inline-semi", "a ; c"),
            (0.21, "value-inline-hash", "a # c"),
            (0.25, "value-leading-semi", ";black"),
            (0.29, "value-leading-hash", "#ff0000"),
            (0.33, "value-percent", "100%"),
            (0.37, "value-percent-pair", "a%%b"),
            (0.41, "value-interpolation", "%(alpha)s"),
            (0.45, "value-extended-interpolation", "${sect:alpha}"),
            (0.49, "value-backslash", "a\\nb"),
            (0.53, "value-backslash-at-eol", "a\\"),
            (0.57, "value-vtab", "a\vb"),
            (0.61, "value-formfeed", "a\fb"),
            (0.65, "value-fs", "a\x1cb"),
            (0.69, "value-nul", "a\x00b"),
            (0.72, "value-long", "L" * 3000),
            (0.76, "value-utf8", "café 中文"),
        )
        for bound, axis, text in table:
            if pick < bound:
                self.mark(axis, axes)
                return text
        self.mark("value-plain", axes)
        return self.rng.choice(VALUES)

    def separator(self, axes):
        if self.rng.random() < 0.25:
            self.mark("sep-colon", axes)
            return ":"
        self.mark("sep-eq", axes)
        return "="

    def needs_plain_entry(self):
        """Whether this document's special construct lives in a key or a value.

        Such an entry must take the ordinary key-separator-value path, because three of
        the shorthand branches below never call key() or value() and one -
        `cont-empty-first-line` - calls value() and then **discards the line it built**.
        The axis was still marked, so the gate saw it in the document and then found the
        two readers agreeing, and reported that the reference no longer diverges. It
        diverged perfectly well; the construct was simply not in the bytes.
        """
        return self.special in ("nbsp-value-boundary", "nonascii-upper-key",
                                "invalid-utf8")

    def entry(self, axes, first_in_section):
        """One entry, possibly with continuation lines, as a list of lines."""
        # The refusing entry-level constructs, each in a document of its own.
        if self.wants("empty-key"):
            self.mark("empty-key", axes)
            return ["= v"]
        if self.wants("key-only-whitespace") and first_in_section:
            # **Only as the first entry of its section**, and the reason is the
            # dialect: a line whose key is only whitespace is by definition an indented
            # line, and an indented line while an entry is open is a *continuation* of
            # that entry's value rather than a key at all. So this construct exists
            # only where no entry is open, which is directly after a header.
            self.mark("key-only-whitespace", axes)
            return ["   = v"]
        if self.wants("no-separator"):
            self.mark("no-separator", axes)
            return ["lonely"]
        if self.wants("cont-after-no-separator"):
            self.mark("cont-after-no-separator", axes)
            return ["lonely", "  more"]
        if self.wants("cont-indent-equal"):
            self.mark("cont-indent-equal", axes)
            return ["  opt = 1", "  more"]
        if self.wants("duplicate-key"):
            self.mark("duplicate-key", axes)
            return ["dup = 1", "dup = 2"]
        if self.wants("duplicate-key-folded"):
            self.mark("duplicate-key-folded", axes)
            return ["dup = 1", "DUP = 2"]
        if self.wants("lone-cr-in-value"):
            self.mark("lone-cr-in-value", axes)
            return ["opt = a\rb"]

        pick = 1.0 if self.needs_plain_entry() else self.rng.random()
        if pick < 0.05:
            self.mark("entry-indented", axes)
            return ["  " + self.key(axes) + " = " + self.value(axes)]
        if pick < 0.09:
            self.mark("entry-indented-pair", axes)
            return ["  " + self.fresh_key() + " = 1", "  " + self.fresh_key() + " = 2"]
        if pick < 0.13:
            self.mark("colon-then-eq", axes)
            return [self.fresh_key() + " : = v"]
        if pick < 0.17:
            self.mark("eq-then-colon", axes)
            return [self.fresh_key() + " = : v"]
        if pick < 0.21:
            self.mark("sep-repeated", axes)
            return [self.fresh_key() + " = a = b"]

        key = self.key(axes)
        sep = self.separator(axes)
        value = self.value(axes)
        lines = ["%s %s %s" % (key, sep, value)]
        if self.wants("nbsp-indent") and not self.special_spent:
            self.special_spent = True
            self.mark("nbsp-indent", axes)
            lines.append(NBSP + "continued")
            return lines
        cont = self.rng.random()
        if cont < 0.05:
            self.mark("cont-space", axes)
            lines.append("  continued")
        elif cont < 0.09:
            self.mark("cont-tab", axes)
            lines.append("\tcontinued")
        elif cont < 0.12:
            self.mark("cont-vtab", axes)
            lines.append("\vcontinued")
        elif cont < 0.15:
            self.mark("cont-fs", axes)
            lines.append("\x1ccontinued")
        elif cont < 0.19:
            self.mark("cont-two-lines", axes)
            lines += ["  first", "  second"]
        elif cont < 0.23:
            self.mark("cont-blank-between", axes)
            lines += ["", "  after the blank"]
        elif cont < 0.27:
            self.mark("cont-comment-between", axes)
            lines += ["# skipped", "  after the comment"]
        elif cont < 0.30:
            self.mark("cont-trailing-blank", axes)
            lines += ["  kept", "", ""]
        elif cont < 0.33:
            self.mark("cont-trailing-comment", axes)
            lines += ["  kept", "; dropped"]
        elif cont < 0.36:
            self.mark("cont-looks-like-entry", axes)
            lines.append("  looks = like an entry")
        elif cont < 0.39:
            self.mark("cont-looks-like-header", axes)
            lines.append("  [not a header]")
        elif cont < 0.42:
            self.mark("cont-deeper-then-shallower", axes)
            lines += ["      deep", "  shallow"]
        elif cont < 0.46 and not self.needs_plain_entry() and \
                not axes & set(PIN_INTERPOLATION):
            # **Not over a value something asserts about**, and this is the same trap
            # needs_plain_entry() exists for, met a second time from the other side.
            # That predicate covers the constructs a *mode* chose; these four are
            # chosen inside value() by the roll, so the generator cannot know in
            # advance and the guard has to read what value() marked. The line built
            # here is discarded, so a document recorded as carrying
            # `value-interpolation` went out with no `%(alpha)s` in it at all - and
            # the pin assertion then found the reference's Basic configuration
            # answering it exactly as the pinned one did, which was true of those
            # bytes and says nothing about the rule.
            self.mark("cont-empty-first-line", axes)
            lines = ["%s %s" % (key, sep), "  the whole value"]
        return lines

    def header(self, axes, name):
        if self.wants("header-empty"):
            self.mark("header-empty", axes)
            return "[]"
        if self.wants("header-no-close"):
            self.mark("header-no-close", axes)
            return "[" + name
        if self.wants("header-duplicate"):
            # The first header has to be the *same name* as the second, so it cannot
            # also carry a name-shaping axis: `[sect]x]` followed by `[sect]` is two
            # different sections and no duplicate at all. That collision scored a
            # document as "expected E_DUPGROUP, got ok" while both readers were right.
            self.mark("header-plain", axes)
            return "[%s]" % name
        pick = self.rng.random()
        table = (
            (0.05, "header-space-inside", "[ %s ]" % name),
            (0.10, "header-bracket-inside", "[%s]x]" % name),
            (0.15, "header-hash-inside", "[%s#x]" % name),
            (0.20, "header-semi-inside", "[%s;x]" % name),
            (0.25, "header-whitespace-name", "[ ]"),
            (0.30, "header-remainder", "[%s]junk" % name),
            (0.35, "header-remainder-assignment", "[%s] k = v" % name),
            (0.40, "header-remainder-comment", "[%s] # c" % name),
            (0.44, "header-default-section", "[DEFAULT]"),
        )
        for bound, axis, text in table:
            if pick < bound:
                self.mark(axis, axes)
                return text
        self.mark("header-plain", axes)
        return "[%s]" % name

    def defaults_document(self, axis):
        """A `[DEFAULT]` and a section that inherits from it, for one axis.

        **A whole document from a template rather than an axis offered to the tables**,
        because this shape needs two sections with agreeing content and the tables build
        one section from independent draws. The same reasoning as the mode mechanism
        above: a construct that needs company cannot be assembled by parts that do not
        know about each other.

        Clean under the pinned configuration, which is what lets these documents be
        scored by every existing score as well as the new one. With
        `interpolation=None` nothing resolves and `[DEFAULT]` is an ordinary section, so
        `values` compares three entries across two sections; with a style on and the
        pinned `default_section`, the reference raises
        `InterpolationMissingOptionError` and this module reports
        `E_INTERPOLATION_MISSING`, which `interp` already owns. Only the fifth and sixth
        configurations resolve them, and that is the whole point of the axis.

        **One terminator, and no CR mutation.** The tail of document() would otherwise
        rewrite every newline here, which is a legal document either way but makes the
        record about line endings as well as about inheritance. At most one special
        construct per document is the rule this file is built on.
        """
        axes = set()
        self.refusal = None
        self.mark(axis, axes)
        # `shared` lives in [DEFAULT] alone for the three resolving axes, so the
        # reference cannot find it without the inheritance: that is what makes the
        # pinned configuration refuse and the live one answer.
        body = {
            "default-basic-reference": "alpha = %(shared)s",
            "default-extended-reference": "alpha = ${shared}",
            # §22.7's exact shape. A two-part path goes through `parser.get()` and a
            # bare name through the section's own `map`, which reads as two different
            # lookups - and both chain the defaults. This is the document that says so.
            "default-extended-path": "alpha = ${sect:shared}",
            "default-overridden": "alpha = %(shared)s",
        }[axis]
        lines = ["[DEFAULT]", "shared = from-default", "[sect]"]
        if axis == "default-overridden":
            # The section's own copy, which must win. Written before the reference to
            # it, because configparser resolves at lookup rather than at read and the
            # order in the file is therefore not what decides - stating that here so a
            # reader does not take the order as the claim.
            lines.append("shared = from-sect")
        else:
            lines.append("own = from-sect")
        lines.append(body)
        return ("\n".join(lines) + "\n").encode("utf-8"), axes, None

    def document(self):
        axes = set()
        self.refusal = None
        self.keys_used = set()
        self.special_spent = False
        self.special = None
        roll = self.rng.random()
        if roll < 0.30:
            self.special = self.rng.choice(sorted(REFUSES))
        elif roll < 0.45:
            self.special = self.rng.choice(sorted(REFERENCE_DIVERGES))
        elif roll < 0.50:
            self.special = "invalid-utf8"
        elif roll < 0.58:
            self.special = self.rng.choice(sorted(DEFAULTS_AXES))
            return self.defaults_document(self.special)

        lines = []
        prefix = ""

        if self.wants("bom"):
            # A byte-order mark, and the first line after it is a **plain header** on
            # purpose. The mark is not skipped by this dialect, so whatever follows it
            # is the start of an ordinary line - and which refusal that produces
            # depends on what the line contains: `<BOM>[sect]` has no separator and is
            # a bad line, while `<BOM>[sect] k = v` has one and is an entry before any
            # section. Pinning the first line pins the expectation to one code instead
            # of making the table a claim about which fault a parser reaches first.
            self.mark("bom", axes)
            prefix = BOM
            lines.append("[sect]")
        else:
            if self.rng.random() < 0.18:
                self.mark("leading-comments", axes)
                lines.append("# a leading comment")
                if self.rng.random() < 0.5:
                    self.mark("comment-semi", axes)
                    lines.append("; and another")
                lines.append("")
            if self.wants("preamble-entry"):
                self.mark("preamble-entry", axes)
                lines.append("early = value")
            lines.append(self.header(axes, "sect"))

        entries = self.rng.randint(1, 3)
        for index in range(entries):
            if self.rng.random() < 0.12:
                self.mark("comment-hash", axes)
                lines.append("# between entries")
            if self.rng.random() < 0.10:
                self.mark("comment-indented", axes)
                lines.append("   # indented, and still a comment")
            if self.rng.random() < 0.12:
                self.mark("blank-lines", axes)
                lines.append("")
            lines += self.entry(axes, index == 0)

        if self.wants("cont-after-header"):
            self.mark("cont-after-header", axes)
            lines += ["[other]", "  orphan"]
        elif self.wants("header-duplicate"):
            self.mark("header-duplicate", axes)
            lines += ["[sect]", "later = 1"]
        elif self.rng.random() < 0.12:
            self.mark("header-case-pair", axes)
            lines += ["[Sect]", "later = 1"]

        if self.rng.random() < 0.12:
            self.mark("trailing-comment", axes)
            lines.append("; the last word")

        text = "\n".join(lines)
        if self.rng.random() < 0.07:
            self.mark("cont-at-eof", axes)
        else:
            text += "\n"

        terminator = self.rng.random()
        if terminator < 0.06:
            self.mark("crlf", axes)
            text = text.replace("\n", "\r\n")
        elif terminator < 0.11 and not self.wants("lone-cr-in-value"):
            # Not beside a value holding its own CR: the replacement would turn that
            # document's one deliberate lone CR into every terminator in it, and the
            # axis would stop being about a value at all.
            self.mark("lone-cr", axes)
            text = text.replace("\n", "\r")
        elif terminator < 0.15 and text.endswith("\n"):
            self.mark("cr-at-eof", axes)
            text = text[:-1] + "\r"

        data = (prefix + text).encode("utf-8")
        if self.wants("invalid-utf8"):
            # The placeholder the value put in, replaced by bytes that are not UTF-8.
            data = data.replace("�INVALID�".encode("utf-8"), b"\xffINV\xfe")
        return data, axes, self.refusal


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
