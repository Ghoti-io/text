#!/usr/bin/env python3
"""The known ways the CSS references differ from this library, as experiments.

A class is a *neutralizer*: a function from an input to the input with one cause of difference taken
out. A divergence is explained by a set of classes when neutralizing exactly those makes this
library and the reference agree about the neutralized input (css_diff.py asks both again). That is the
test, and it is a controlled one: the class does not describe the divergence by looking at it, it
removes the cause and sees the difference go.

Each class carries the sentence of the served CSS Syntax draft (retrieved 2026-10-21) or of the
reference's own documentation that decides which side is right, and a statement of who holds what.
tools/oracle/css_divergences.txt is the record of which classes occur where, with a witness and a count.

Copyright 2026 by Corey Pennycuff
"""

import re

CLASSES = {}


def cls(name, who, reason, when=None):
    """Register a class. `when(mode, ref, ours, theirs)`, if given, says whether the class can apply to a
    divergence at all (a feature of the two answers); the neutralizer is the experiment that proves it."""
    def register(fn):
        CLASSES[name] = {"fn": fn, "who": who, "reason": reason, "when": when}
        return fn
    return register


# A byte-oriented regex over the input (UTF-8 bytes, possibly ill-formed).

_SURROGATE_ESCAPE = re.compile(rb"\\0{0,2}[dD][89a-fA-F][0-9a-fA-F]{2}(?![0-9a-fA-F])")


@cls("escaped-surrogate", "tinycss2",
     'tinycss2 keeps the lone surrogate an escape names; the draft (4.3.7, "consume an escaped code point") says: '
     '"If this number is zero, or is for a surrogate, or is greater than the maximum allowed code point, return U+FFFD".')
def n_surrogate(data):
    return _SURROGATE_ESCAPE.sub(b"x", data)


_MATCH = re.compile(rb"([~|^$*])=")


@cls("css21-match-tokens", "tinycss2 cssparser csstree",
     "The references tokenize ~= |= ^= $= *= (and || in tinycss2) as tokens of their own, as CSS 2.1 and the 2021 drafts did; the "
     'served draft has no such tokens: its list of tokens (4) has none, and "<delim-token>" is what a code point that begins no other '
     "token becomes, so each is two delim tokens.")
def n_match(data):
    out = _MATCH.sub(rb"\1 =", data)
    while b"||" in out:
        out = out.replace(b"||", b"| |")
    return out


_NON_ASCII = re.compile(rb"[\x80-\xff]+")


@cls("non-ascii-ident-code-points", "tinycss2 cssparser csstree",
     'The references take every code point from U+0080 as an ident code point (the CSS Syntax 3 definition of the 2021 drafts); the served '
     'draft (4.2) lists them: "U+00B7, U+00C0 to U+00D6, U+00D8 to U+00F6, U+00F8 to U+037D, U+037F to U+1FFF, U+200C, U+200D, U+203F, '
     'U+2040, U+2070 to U+218F, U+2C00 to U+2FEF, U+3001 to U+D7FF, U+F900 to U+FDCF, U+FDF0 to U+FFFD, or any code point at or above U+10000".')
def n_non_ascii(data):
    return _NON_ASCII.sub(b"x", data)


_URANGE = re.compile(rb"[uU]\+")


@cls("unicode-range-everywhere", "tinycss2",
     'tinycss2 makes a unicode-range token of u+ and what follows wherever it finds it; the served draft (4.3.14, 5.5.6) produces one only '
     'where "consume the value of a unicode-range descriptor" asks for it: "Note: This token is not produced by the tokenizer in normal use".')
def n_urange(data):
    return _URANGE.sub(b"x+", data)


_BREAKS = re.compile(rb"[\r\n\f]")


@cls("bad-string-newline", "csstree",
     'csstree consumes the line break that ends a string; the draft (4.3.5, "consume a string token") says: "newline: This is a parse '
     'error. Reconsume the current input code point, create a <bad-string-token>, and return it", so the line break is a white space '
     "token of its own.")
def n_breaks(data):
    return _BREAKS.sub(b" ", data)


@cls("backslash-at-eof", "csstree",
     'A backslash that ends the input is a delim token in csstree; the draft (4.3.8, "check if two code points are a valid escape") says: '
     '"If the second code point is a newline, return false. Otherwise, return true", and a missing second code point is not a newline, so '
     'it begins an escape, and "consume an escaped code point" returns U+FFFD for the end of the input.')
def n_backslash(data):
    return data + b"x" if data.endswith(b"\\") else data


@cls("nul-code-point", "csstree",
     'csstree reads U+0000 as it is; the draft (3.3, "filter code points") says: "Replace any U+0000 NULL or surrogate code points in input '
     'with U+FFFD REPLACEMENT CHARACTER".')
def n_nul(data):
    return data.replace(b"\x00", b"x")


_URL_EOF_WS = re.compile(rb"([uU][rR][lL]\()[ \t\r\n\f]+(\)|$)")


@cls("url-white-space-only", "csstree",
     'The value of a url( token that is nothing but white space (closed, or unclosed at the end of the input) keeps it in csstree; the draft (4.3.6, "consume a url '
     'token") says: "Consume as much whitespace as possible", then the value is built from what follows.')
def n_url_eof(data):
    return _URL_EOF_WS.sub(rb"\1\2", data)


_DASH_ESC = re.compile(rb"-\\(?=[\r\n\f]|$)")


@cls("hyphen-before-invalid-escape", "cssparser",
     'cssparser reads a hyphen followed by a backslash and a line break as an identifier; the draft (4.3.10, "check if three code points '
     'would start an ident sequence") says: "U+002D HYPHEN-MINUS: If the second code point is an ident-start code point or a U+002D '
     'HYPHEN-MINUS, or the second and third code points are a valid escape, return true. Otherwise, return false", and a backslash and a '
     'line break are not a valid escape, so the hyphen is a delim.')
def n_dash(data):
    return _DASH_ESC.sub(b"-x", data)


@cls("custom-property-blocks", "tinycss2 cssparser csstree",
     'The references read a custom property whose value has a {}-block in it as ending at the block, and keep the qualified rule that '
     'begins `--name:` at the top level; the served draft (5.5.6, 5.5.3) lets the value of a custom property be anything up to the '
     'semicolon ("If decl\'s name is a custom property name string, then set decl\'s original text to the segment of the original source '
     'text string corresponding to the tokens of decl\'s value", and the {}-block restriction is for a name that is not one), reads `!important` off '
     'it, and says of a qualified rule: "If the first two non-<whitespace-token> values of rule\'s prelude are an <ident-token> whose '
     'value starts with "--" followed by a <colon-token>, then: If nested is true, consume the remnants of a bad declaration from input, '
     'with nested set to true, and return nothing. If nested is false, return nothing".')
def n_custom(data):
    return data.replace(b"--", b"-x")


_CHARSET = re.compile(rb"@([cC][hH][aA][rR][sS][eE][tT])")


@cls("charset-at-rule-skipped", "cssparser",
     "cssparser does not return an @charset at-rule from a stylesheet; the served draft (8.3) says: \"there is no actual at-rule named "
     "@charset. When a stylesheet is actually parsed, any occurrences of an @charset rule must be treated as an unrecognized rule, and "
     "thus dropped as invalid when the stylesheet is grammar-checked\", which is a check above syntax: the syntax layer returns the "
     "at-rule, as it returns any other.")
def n_charset(data):
    return _CHARSET.sub(rb"@x\1", data)


_AT = re.compile(rb"@")


@cls("at-rule-in-declaration-list", "tinycss2",
     'tinycss2 returns the at-rules of a list of declarations; the served draft (5.4.5, "parse a list of declarations") says: "Otherwise, '
     'if the next token from input is an <at-keyword-token>, consume an at-rule from input and discard the result."',
     when=lambda mode, ref, ours, theirs: mode == "declarations")
def n_at(data):
    return _AT.sub(b"x", data)


_COMMENT = re.compile(rb"/\*.*?(?:\*/|$)", re.S)
_WS_AFTER_COLON = re.compile(rb":[ \t\r\n\f]+")
_WS_BEFORE_END = re.compile(rb"[ \t\r\n\f]+(?=[;}!]|$)")


@cls("declaration-value-white-space", "tinycss2",
     'tinycss2 keeps the white space after the colon and before the end of a declaration in its value; the served draft (5.5.6, '
     '"consume a declaration") says: "Discard whitespace from input." (after the colon) and "While the last item in decl\'s value is a '
     '<whitespace-token>, remove that token."')
def n_declws(data):
    return _WS_BEFORE_END.sub(b"", _WS_AFTER_COLON.sub(b":", _COMMENT.sub(b"", data)))


def _has_brace_value(v):
    """Whether an answer has a declaration (not a custom property) whose value is a {}-block and something more."""
    if isinstance(v, list):
        if len(v) == 4 and v[0] == "declaration" and isinstance(v[1], str) and not v[1].startswith("--") and isinstance(v[2], list):
            vals = [x for x in v[2] if x != " "]
            if len(vals) > 1 and any(isinstance(x, list) and x and x[0] == "{}" for x in vals):
                return True
        return any(_has_brace_value(x) for x in v)
    return False


@cls("brace-block-value-and-more", "tinycss2 cssparser csstree",
     'The references keep a declaration whose value is a {}-block and something else; the served draft (5.5.6, "consume a declaration") '
     'says: "If decl\'s name is not a custom property name string, and decl\'s value contains a top-level simple block with an associated '
     'token of <{-token>, and also contains any other non-<whitespace-token> value, return nothing. (That is, a top-level {}-block is only '
     'allowed as the entire value of a non-custom property.)", and the text is then read again as a qualified rule.',
     when=lambda mode, ref, ours, theirs: mode != "tokens")
def n_brace(data):
    return data.replace(b"{", b"(").replace(b"}", b")")


def _has_close_brace_error(v):
    if isinstance(v, list):
        if v == ["error", "}"]:
            return True
        return any(_has_close_brace_error(x) for x in v)
    return False


def _unmatched_close_braces(data):
    """The input with each } that closes nothing (outside strings and comments, by a plain scan) as a semicolon."""
    out = bytearray(data)
    depth = 0
    i = 0
    n = len(data)
    quote = None
    while i < n:
        c = data[i:i + 1]
        if quote:
            if c == b"\\":
                i += 1
            elif c == quote:
                quote = None
        elif c in (b'"', b"'"):
            quote = c
        elif data[i:i + 2] == b"/*":
            j = data.find(b"*/", i + 2)
            i = n if j < 0 else j + 1
        elif c == b"\\":
            i += 1
        elif c == b"{":
            depth += 1
        elif c == b"}":
            if depth == 0:
                out[i] = 0x3B
            else:
                depth -= 1
        i += 1
    return bytes(out)


@cls("unmatched-close-brace-in-block", "tinycss2 cssparser",
     'A } that closes nothing inside a block is a token of a prelude or a value in the references; in the served draft (5.5.3, 5.5.4, 5.5.6) '
     'it ends the declaration or the rule (the stop token when nested) and is a parse error: "close-brace-in-qualified-rule" is the library\'s '
     'name for the one in a prelude.',
     when=lambda mode, ref, ours, theirs: mode in ("blocks", "declarations", "stylesheet") and (_has_close_brace_error(theirs) or True))
def n_close_brace(data):
    return _unmatched_close_braces(data)


@cls("important-after-delim-bang", "tinycss2",
     'tinycss2 takes `!important` as the flag only when no other `!` precedes it; the served draft (5.5.6, "consume a declaration") '
     'says: "If the last two non-<whitespace-token>s in decl\'s value are a <delim-token> with the value "!" followed by an <ident-token> '
     'with a value that is an ASCII case-insensitive match for "important", remove them from decl\'s value and set decl\'s important flag '
     'to true", whatever else the value holds.')
def n_bang(data):
    return data.replace(b"!", b"~")


_UR = re.compile(rb"unicode-range", re.I)


@cls("unicode-range-important", "tinycss2 cssparser",
     'For a unicode-range declaration the references take `!important` off the value like any other; the served draft (5.5.6) reads the '
     'value of that descriptor again from "the segment of the original source text string corresponding to the tokens returned by the '
     'consume a list of component values call", which includes the `!important` the flag was taken from.')
def n_ur_important(data):
    return _UR.sub(b"unicode-rang", data)


@cls("bang-word-is-important", "csstree",
     'csstree sets the important flag for `!` followed by any word (`!mrtant`); the served draft (5.5.6) takes the flag only from "a '
     '<delim-token> with the value "!" followed by an <ident-token> with a value that is an ASCII case-insensitive match for "important"".',
     when=lambda mode, ref, ours, theirs: mode == "toplevel")
def n_bang_word(data):
    return data.replace(b"!", b"~")


_ESC_URL = re.compile(rb"\\?([uU])\\?([rR])\\?([lL])\\?\(")


@cls("escaped-url-name", "csstree",
     'csstree reads `\\url(` as a function; the served draft (4.3.4, "consume an ident-like token") says: "Let string be the result of '
     'consuming an ident sequence. If string\'s value is an ASCII case-insensitive match for "url", and the next input code point is U+0028 '
     'LEFT PARENTHESIS ((), consume it", and the value of an ident sequence is its unescaped value.')
def n_esc_url(data):
    return _ESC_URL.sub(rb"\1\2\3(", data)


@cls("span-includes-comments", "csstree",
     "csstree's rule extents include the comments inside and after a rule's last token; ours ends a rule at its last token (CSS Syntax "
     "has no comment tokens, 4.3.2: \"consume comments\" is not a token).",
     when=lambda mode, ref, ours, theirs: mode == "toplevel")
def n_comments(data):
    return _COMMENT.sub(b"", data)


def _has_bad_url(v):
    if isinstance(v, list):
        if v == ["error", "bad-url"]:
            return True
        return any(_has_bad_url(x) for x in v)
    return False


_URL_OPEN = re.compile(rb"([uU])([rR])([lL])\(")


@cls("bad-url-remnants-past-paren", "csstree",
     "csstree's bad-url token takes more than the draft's remnants with it (the character after the closing parenthesis, for one); "
     'the served draft (4.3.15, "consume the remnants of a bad url") says: "Repeatedly consume the next input code point from the '
     'stream: U+0029 RIGHT PARENTHESIS ()): Return. EOF: This is a parse error. Return."',
     when=lambda mode, ref, ours, theirs: _has_bad_url(ours) or _has_bad_url(theirs))
def n_bad_url(data):
    return _URL_OPEN.sub(rb"\1x\3(", data)
