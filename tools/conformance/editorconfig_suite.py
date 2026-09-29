#!/usr/bin/env python3
"""Score this library against editorconfig-core-test's 34 `parser` assertions.

    editorconfig_suite.py <suite-dir> <runner> [--control]

`editorconfig-core-test` is the **only normative INI conformance suite that
exists**: specification 0.17.2 says "a conforming core or plugin must pass the
tests in the core-tests repository". That makes this the one dialect in this
module whose correctness claim is a pass count rather than an agreement with a
reference - and the distinction matters here more than usual, because **both
reference cores score 33 of these 34**.

Of the suite's 202 assertions only these 34 are about the grammar. The other 168
test a filepath glob matcher (130), file discovery and precedence (24), value
semantics (10) and a command line (3). None of those is a text library's job, and
saying so is better than quoting 202 and scoring a sixth of it.

**How an assertion is replayed.** The suite is a set of CMake calls, each naming
an `.editorconfig` file, a filepath, and a `PASS_REGULAR_EXPRESSION` that the
output of `editorconfig -f <file> <path>` must match. There is no `editorconfig`
binary here, so this script is one: it asks `<runner>` for the parse, matches the
filepath against each section's glob itself, merges the matching sections in
document order, and prints `key=value` lines for the regex to judge. The
`_multiline` variants sort the output lines the way `cmake/ec_sort.cmake` does and
anchor the regex, which is what CTest would have done.

**The glob matcher below is the harness's, not the library's**, and that is why
`--control` exists. It replaces `<runner>` with a throwaway INI parser written in
this file, in Python, from the specification's four line rules. The control must
score 34 of 34. If it does, the glob and merge layer is right, and any failure
under the real runner is the library's; if it does not, the harness is broken and
the library's score means nothing. A gate whose harness has never been shown to
work is a gate that cannot fail for the right reason.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import subprocess
import sys

# One `new_ec_test` or `new_ec_test_multiline` call. The arguments are
# whitespace-separated and may be quoted; the suite wraps some calls onto a
# second line, which `re.S` covers.
CALL = re.compile(
    r"""^new_ec_test(?P<ml>_multiline)?\(\s*(?P<name>\w+)\s+(?P<ec>\S+)\s+"""
    r"""(?P<src>"(?:[^"\\]|\\.)*"|\S+)\s+(?P<rx>"(?:[^"\\]|\\.)*"|\S+)\s*\)""",
    re.S | re.M)

# CMake's quoted-argument escapes that stand for a character rather than
# themselves.
CMAKE_ESCAPES = {"n": "\n", "t": "\t", "r": "\r"}


def unquote(tok):
    r"""Undo CMake's quoted-argument escaping, as CMake itself would.

    `\n` is a newline and `\\` is one backslash, so a regex written `[\n\r]+` in
    the suite reaches the regex engine as a class holding the two real
    characters. The literal backslash is then re-escaped, because `[\\]` - the
    suite's way of spelling "a backslash" - unescapes to `[\]`, which CMake's
    regex engine reads as a class holding a backslash and Python's refuses
    outright.
    """
    if tok.startswith('"') and tok.endswith('"'):
        tok = tok[1:-1]
    out = re.sub(r"\\(.)", lambda m: CMAKE_ESCAPES.get(m.group(1), m.group(1)),
                 tok)
    return out.replace("\\", "\\\\")


def assertions(path):
    """Every parser assertion in the suite, in file order."""
    text = open(path).read()
    for m in CALL.finditer(text):
        yield (m.group("name"), m.group("ec"), unquote(m.group("src")),
               unquote(m.group("rx")), bool(m.group("ml")))


# ---------------------------------------------------------------------------
# The glob matcher. The harness's own; see the module docstring.
# ---------------------------------------------------------------------------

def glob_to_regex(glob):
    r"""Translate one EditorConfig section glob into a Python regex.

    The documented set: `*` is any run but `/`, `**` is any run at all, `?` is one
    character but `/`, `[seq]` and `[!seq]` are classes, `{a,b}` is alternation,
    `{n..m}` is a numeric range, and a backslash escapes the next character.
    """
    out = []
    i = 0
    n = len(glob)
    while i < n:
        c = glob[i]
        if c == "\\" and i + 1 < n:
            out.append(re.escape(glob[i + 1]))
            i += 2
        elif c == "*":
            if glob[i:i + 3] == "**/":
                # `**/` must match *no* directories as well as some, or the `**/`
                # this harness prefixes to a slash-free section would require the
                # file to be in a subdirectory and every assertion would resolve
                # to nothing. Both cores do the same: core-py's own fnmatch emits
                # an optional group here.
                out.append("(?:.*/)?")
                i += 3
            elif i + 1 < n and glob[i + 1] == "*":
                out.append(".*")
                i += 2
            else:
                out.append("[^/]*")
                i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        elif c == "[":
            close = glob.find("]", i + 1)
            if close < 0:
                out.append(re.escape(c))
                i += 1
                continue
            body = glob[i + 1:close]
            if body.startswith("!"):
                body = "^" + body[1:]
            out.append("[" + body.replace("\\", "\\\\") + "]")
            i = close + 1
        elif c == "{":
            close = glob.find("}", i + 1)
            if close < 0:
                out.append(re.escape(c))
                i += 1
                continue
            body = glob[i + 1:close]
            rng = re.fullmatch(r"(-?\d+)\.\.(-?\d+)", body)
            if rng:
                lo, hi = int(rng.group(1)), int(rng.group(2))
                out.append("(?:" + "|".join(
                    str(v) for v in range(min(lo, hi), max(lo, hi) + 1)) + ")")
            elif "," in body:
                out.append("(?:" + "|".join(
                    glob_to_regex(part) for part in body.split(",")) + ")")
            else:
                out.append(re.escape("{" + body + "}"))
            i = close + 1
        else:
            out.append(re.escape(c))
            i += 1
    return "".join(out)


def section_matches(section, conf_dir, target):
    r"""Whether `section`'s glob selects `target`, both absolute POSIX paths.

    The anchoring rule, from the specification and both cores: a glob containing
    no `/` matches in any directory below the file, so it is prefixed `**/`; one
    with a leading `/` is relative to the file's own directory; one with a `/`
    elsewhere is also relative to it. A `\;` or `\#` in a section name is a
    literal, which is the glob layer's escape rather than the parser's - the
    parser stores the backslash, and this is where it comes off.
    """
    glob = section.replace("\\#", "#").replace("\\;", ";")
    if "/" in glob:
        if glob.startswith("/"):
            glob = glob[1:]
        pattern = conf_dir.rstrip("/") + "/" + glob
    else:
        pattern = conf_dir.rstrip("/") + "/**/" + glob
    return re.fullmatch(glob_to_regex(pattern), target) is not None


# ---------------------------------------------------------------------------
# The two parsers: the runner under test, and the control.
# ---------------------------------------------------------------------------

def unhex(tok):
    if tok == "-":
        return None
    if tok == ".":
        return ""
    return bytes.fromhex(tok).decode("utf-8", "surrogateescape")


def parse_with_runner(runner, path):
    """Ask the runner. Returns (error, sections) - see editorconfig_suite.c."""
    out = subprocess.run([runner, path], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"{runner} failed on {path}: {out.stderr.strip()}")
    error = None
    sections = []  # [(name or None for the preamble, [(key, value), ...])]
    current = (None, [])
    sections.append(current)
    for line in out.stdout.splitlines():
        if line.startswith("E "):
            error = int(line[2:])
        elif line.startswith("S "):
            current = (unhex(line[2:]), [])
            sections.append(current)
        elif line.startswith("P "):
            _, key, value = line.split(" ", 2)
            current[1].append((unhex(key), unhex(value)))
    return error, sections


def parse_control(_runner, path):
    r"""A throwaway EditorConfig parser, from the specification's four rules.

    Deliberately naive and deliberately *not* the library: its only job is to show
    that the glob and merge layer above scores 34 of 34, so that a failure under
    the library is attributable. The rules, in order: strip the line; a blank line
    or one starting `;` or `#` is ignored; a line in `[` ... last `]` is a section;
    otherwise the part before the first `=` is the key, lowercased, and the rest is
    the value, both stripped. Anything else is invalid.
    """
    raw = open(path, "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    text = raw.decode("utf-8", "surrogateescape")
    sections = [(None, [])]
    current = sections[0]
    # str.strip() would also strip the NEL and the Unicode separators, which is
    # not C's isspace set; spell the set out.
    ws = " \t\v\f\r"
    for line in text.split("\n"):
        line = line.strip(ws)
        if not line or line[0] in ";#":
            continue
        if line.startswith("[") and line.endswith("]"):
            current = (line[1:line.rindex("]")], [])
            sections.append(current)
            continue
        if "=" not in line:
            return 1, sections
        key, value = line.split("=", 1)
        key = key.strip(ws).lower()
        if not key:
            return 1, sections
        current[1].append((key, value.strip(ws)))
    return None, sections


def resolve(sections, conf_path, target):
    """Merge every matching section's pairs in order; the last wins."""
    conf_dir = os.path.dirname(os.path.abspath(conf_path)).replace(os.sep, "/")
    target = os.path.abspath(target).replace(os.sep, "/")
    options = {}
    for name, pairs in sections:
        if name is None:
            continue  # The preamble: `root` steers the upward walk, nothing else.
        if not section_matches(name, conf_dir, target):
            continue
        for key, value in pairs:
            options[key] = value
    return options


def main(argv):
    args = [a for a in argv[1:] if a != "--control"]
    control = "--control" in argv
    if len(args) != 2:
        sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
        return 2
    suite, runner = args
    parse = parse_control if control else parse_with_runner
    directory = os.path.join(suite, "parser")
    listing = os.path.join(directory, "CMakeLists.txt")
    if not os.path.isfile(listing):
        sys.stderr.write(f"{listing} is not there; is {suite} the suite?\n")
        return 2

    total = passed = 0
    for name, ec, src, regex, multiline in assertions(listing):
        total += 1
        conf = os.path.join(directory, ec)
        target = os.path.join(directory, src)
        error, sections = parse(runner, conf)
        if error is not None:
            # A refused document answers nothing, which is what a core does too
            # after reporting the error line - it keeps what it read. Here the
            # whole parse is the unit, so an error means no properties at all.
            text = ""
        else:
            options = resolve(sections, conf, target)
            lines = [f"{k}={v}" for k, v in options.items()]
            if multiline:
                lines.sort()
            text = "".join(line + "\n" for line in lines)
        pattern = "^[\r\n]*" + regex + "$" if multiline else regex
        if re.search(pattern, text):
            passed += 1
        else:
            print(f"FAIL {name}")
            print(f"  want {pattern!r}")
            print(f"  got  {text!r}")
    label = "control" if control else "parser"
    print(f"{passed} of {total} {label} assertions")
    # A zero denominator passes any percentage test, so say so and fail.
    if not total:
        print("FAIL no assertions were found; the suite layout changed")
        return 1
    floor = int(os.environ.get("EC_MIN", total))
    if passed < floor:
        print(f"FAIL {passed} of {total} is below the floor of {floor}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
