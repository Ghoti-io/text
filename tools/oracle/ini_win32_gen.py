"""Generate Win32 profile-API documents, one construct per document.

**This dialect refuses nothing**, which makes this generator a different shape
from the other five. There is no `REFUSES` table, because every byte sequence is
a legal Win32 document: the key charset is open, the empty key and the empty
section name are both spellable, a line with no separator is a valueless entry,
and an unclosed header is an ordinary line. So `intent` here is not "did we agree
about legality" - it is the assertion that **no document is ever refused**, which
is a property the other dialects cannot state and a fuzz property besides.

What carries the weight instead is that the reference has **two entry points that
disagree**, so every document is scored three ways: the section list, the section
enumeration, and the per-key lookup. `REFERENCE_DIVERGES` names the axes where we
knowingly differ from `GetPrivateProfileString` - the `;` comment rule - and the
gate asserts each one still differs rather than letting agreement creep in.

One construct per document, for the reason the configparser generator records: two
special constructs in one document make it impossible to say which of them a
disagreement is about, and a refusing construct beside a diverging one hides the
divergence entirely.

Copyright 2026 by Corey Pennycuff
"""

# Axes where this dialect deliberately disagrees with `GetPrivateProfileString`.
# The value is what the string API does that we do not; the gate asserts the
# disagreement is still observable, so that a wine that started treating `;` as a
# comment would fail loudly instead of quietly inflating the score.
# Axes where this dialect deliberately or necessarily disagrees with
# `GetPrivateProfileString`. Each entry is (what the string API does that we do
# not, how the gate observes it). The gate asserts the disagreement is **still
# there**, so that a wine which started treating ';' as a comment would fail
# loudly instead of quietly making the `values` score look better.
#
# Two different kinds of disagreement live here and the distinction matters:
#
#   "semi"  a *choice*. `;` is a comment to `GetPrivateProfileSection` and not to
#           `GetPrivateProfileString`, so the reference contradicts itself and this
#           dialect follows the enumeration API. Either answer agrees with half the
#           reference; this half does not hand back a setting its author disabled.
#   "nul"   a *limit of the reference*, not a rule of the format.
#           `GetPrivateProfileStringA` returns a C string, so it cannot express a
#           value containing a NUL and truncates there. A byte-oriented reader can,
#           and does. There is no answer the reference could give that we should
#           copy.
REFERENCE_DIVERGES = {
    "semi-comment-setting": (
        "the string API retrieves ';disabled'; the enumeration API and we do not",
        "semi"),
    "semi-indented": (
        "the string API retrieves a ';' key after leading whitespace",
        "semi"),
    "semi-comment-empty-value": (
        "the string API retrieves ';k' with an empty value",
        "semi"),
    "semi-comment-prose": (
        "a ';' line with no '=' is dropped by both APIs and by us, for two"
        " different reasons - it is here for the reasoning, and by construction"
        " there is no difference to observe",
        "none"),
    "value-nul": (
        "the string API truncates a value at a NUL, because it returns a C"
        " string; we keep both bytes",
        "nul"),
}

# Bytes that are whitespace to this dialect, for the harness's own trimming of a
# section name - `GetPrivateProfileSectionNames` reports the trimmed spelling with
# its case intact, which is neither our stored name nor our canonical one.
SPACE = b" \t\v\f"


def _axes():
    """(label, document bytes) for every measured construct."""
    out = []

    def add(label, doc):
        out.append((label, doc if isinstance(doc, bytes) else doc.encode()))

    # --- headers -----------------------------------------------------------
    add("header-plain", "[a]\nk=v\n")
    add("header-junk-after", "[a]junk\nk=v\n")
    add("header-second-close", "[a]]junk\nk=v\n")
    add("header-inner-open", "[a[b]\nk=v\n")
    add("header-unterminated", "[a\nk=v\n")
    add("header-unterminated-then-good", "[a\nk=v\n[b]\nj=w\n")
    add("header-empty-name", "[]\nk=v\n")
    add("header-space-name", "[ ]\nk=v\n")
    add("header-padded", "[ b ]\nk=v\n")
    add("header-tab-padded", "[\tb\t]\nk=v\n")
    add("header-indented", "   [a]\nk=v\n")
    add("header-name-equals", "[a=b]\nk=v\n")
    add("header-name-semicolon", "[a;b]\nk=v\n")
    add("header-name-hash", "[a#b]\nk=v\n")
    add("header-name-high-byte", b"[\xe9]\nk=v\n")
    add("header-long-name", "[" + "s" * 300 + "]\nk=v\n")
    add("header-duplicate", "[a]\nk=1\n[b]\nj=2\n[a]\nm=3\n")
    add("header-duplicate-case", "[a]\nk=1\n[A]\nj=2\n")
    add("header-two-on-one-line", "[a][b]\nk=v\n")
    add("header-then-comment", "[a] ; c\nk=v\n")
    add("header-then-hash", "[a] # c\nk=v\n")
    add("bare-close-bracket", "[a]\n]\nk=v\n")
    add("junk-before-bracket", "x[a]\nk=v\n")
    add("only-header", "[a]\n")

    # --- entries -----------------------------------------------------------
    add("entry-plain", "[a]\nk = v\n")
    add("entry-no-value", "[a]\nnovalue\n")
    add("entry-no-value-padded", "[a]\nnovalue   \n")
    add("entry-empty-value", "[a]\nk=\n")
    add("entry-empty-key", "[a]\n= v\n")
    add("entry-key-only-space", "[a]\n   = v\n")
    add("entry-value-equals", "[a]\nk=a=b\n")
    add("entry-double-separator", "[a]\nk==v\n")
    add("entry-key-brackets", "[a]\nk[1]=v\n")
    add("entry-key-semicolon", "[a]\nq;x=v\n")
    add("entry-key-hash", "[a]\nq#x=v\n")
    add("entry-key-high-byte", b"[a]\nk\xe9=v\n")
    add("entry-value-high-byte", b"[a]\nk=v\xe9\n")
    add("entry-long-key", "[a]\n" + "k" * 300 + "=v\n")
    add("entry-long-value", "[a]\nk=" + "z" * 2000 + "\n")
    add("entry-dup-key", "[a]\nk=1\nk=2\n")
    add("entry-dup-key-case", "[a]\nk=1\nK=2\n")
    add("entry-dup-key-space", "[a]\nk=1\nk =2\n")
    add("entry-tabs-around-sep", "[a]\nk\t=\tv\t\n")
    add("entry-vt-ff-around-sep", b"[a]\nk\x0b=\x0cv\x0b\n")
    add("entry-fs-gs-rs", b"[a]\nk\x1c=\x1dv\x1e\n")
    add("entry-indented", "[a]\n   k = v\n")
    add("entry-value-all-space", "[a]\nk=   \n")
    add("entry-value-leading-semi", "[a]\nk=;v\n")
    add("entry-value-trailing-backslash", "[a]\nk=v\\\n")
    add("entry-backslash-eol", "[a]\nk=one\\\nmore=two\n")
    add("entry-key-quoted", "[a]\n\"k\"=v\n")

    # --- quoting -----------------------------------------------------------
    add("quote-double", '[a]\nk="v"\n')
    add("quote-single", "[a]\nk='v'\n")
    add("quote-two-pairs", '[a]\nk=""x""\n')
    add("quote-unbalanced-lead", '[a]\nk="x\n')
    add("quote-unbalanced-trail", '[a]\nk=x"\n')
    add("quote-protects-space", '[a]\nk="  x  "\n')
    add("quote-mid-value", '[a]\nk=x" mid "y\n')
    add("quote-then-semi", '[a]\nk="v" ; c\n')
    add("quote-mixed-kinds", "[a]\nk=\"x'\n")
    add("quote-exactly-two", '[a]\nk=""\n')
    add("quote-one", '[a]\nk="\n')
    add("quote-single-exactly-two", "[a]\nk=''\n")

    # --- comments, which is where the two references part -----------------
    add("semi-comment-prose", "[a]\n; just prose\nk=v\n")
    add("semi-comment-setting", "[a]\n;disabled=1\nk=v\n")
    add("semi-indented", "[a]\n   ;disabled=1\nk=v\n")
    add("semi-comment-empty-value", "[a]\n;k=\nj=v\n")
    add("hash-not-comment", "[a]\n#hash=2\nk=v\n")
    add("hash-prose", "[a]\n# just prose\nk=v\n")

    # --- terminators -------------------------------------------------------
    add("term-crlf", "[a]\r\nk=v\r\n")
    add("term-lone-cr", "[a]\rk=v\r")
    add("term-mixed", "[a]\r\nk=v\rj=w\n")
    add("term-no-final-newline", "[a]\nk=v")
    add("term-cr-in-value", b"[a]\nk=a\rb\n")
    add("term-blank-lines", "[a]\n\n   \n\t\nk=v\n")
    add("term-bom", "﻿[a]\nk=v\n")

    # --- documents with no header ------------------------------------------
    add("preamble-only", "k=v\nj=w\n")
    add("preamble-then-section", "p=0\n[a]\nk=v\n")
    add("preamble-empty-header", "p=0\n[]\nk=v\n")
    add("empty-file", "")
    add("only-blank", "\n\n   \n")
    add("only-comment", "; nothing else\n")
    add("value-nul", b"[a]\nk=a\x00b\n")
    return out


AXES = _axes()


def documents(count=None, seed=None):
    """The population: every axis, then repeats if a larger count is asked for.

    Unlike the other INI generators this one is **not random**. Every rule of this
    dialect came from a probe rather than a document, so the axes *are* the
    population; a random combiner would mostly produce documents whose answer is
    already determined by an axis already here, and would reintroduce the
    two-constructs-in-one-document problem the header of this file describes.
    `count` is honoured so the Makefile's knob keeps working, and a count above
    the axis count changes nothing but the run time.
    """
    axes = list(AXES)
    if not count or count >= len(axes):
        return axes
    return axes[:count]
