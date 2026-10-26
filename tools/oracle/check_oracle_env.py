#!/usr/bin/env python3
"""Fail if the oracle pin table or the code that reads it has rotted.

`tools/oracle/containers/IMAGES` is machine-read, not documentation. What reads
it is `oracle_env.py`, and both can go wrong in ways no differential would
report, because a differential that cannot reach its reference declines rather
than lying - so the failure is silent in the other direction: a pin that parses
into the wrong fields, a reference with no way to be asked its version, a
filter that has stopped filtering.

This needs no container engine, no reference and no build, which is why it is in
`make test` while every gate that consults an oracle is not. It takes
milliseconds.

**Every check here is paired with a control** - a planted violation the check
must catch - because a checker whose assertion no longer reaches its subject
reports the same clean line as one that passes. The controls that matter most
are the last two: they are the only thing standing between "this library raised
its UCD pin" and "its IDNA oracle quietly went back to being two releases
behind", which is the state the whole `containers/` directory exists to leave.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle_env

FAILURES = []
# The name a built-here image must carry, so that one can be told from a stock
# one and `make oracle-clean` can find it.
BUILT_HERE_PREFIX = "ghoti-text-oracle-"
UCD_VERSION_FILE = os.path.join(oracle_env.ROOT, "tools", "idna",
                                "UCD_VERSION")


def fail(message):
    FAILURES.append(message)


def images_saying(text):
    """`oracle_env.pins()` as it would read a planted IMAGES file.

    The control mechanism for every check that takes a table: write the table,
    point the module at it, and clear the cache. Restoring all three afterwards
    is the caller's job, which is why each control does it in a `finally`.
    """
    handle = tempfile.NamedTemporaryFile("w", suffix=".IMAGES", delete=False,
                                         encoding="utf-8")
    handle.write(text)
    handle.close()
    oracle_env.IMAGES = handle.name
    oracle_env._pins = None
    return handle.name


def restore_images(saved, planted):
    oracle_env.IMAGES = saved
    oracle_env._pins = None
    os.unlink(planted)


def audit(table):
    """The rules a pin table has to satisfy. Returns a list of complaints."""
    problems = []
    if not table:
        problems.append("containers/IMAGES names no references at all")
    for name, (image, version, _) in table.items():
        if not image or not version:
            problems.append("%s: an empty image or version field" % name)
        # A stock image is pinned by digest and the digest is the whole
        # guarantee; a built-here one cannot be, and carries the convention's
        # prefix. An image that is neither is one nothing pins.
        if image.startswith("localhost/"):
            if BUILT_HERE_PREFIX not in image:
                problems.append(
                    "%s: built here and outside the naming convention: %s"
                    % (name, image))
        elif "@sha256:" not in image:
            problems.append("%s: a stock image with no digest: %s"
                            % (name, image))
    return problems


def check_images_parses():
    """The committed table parses and satisfies every rule."""
    try:
        table = oracle_env.pins()
    except oracle_env.OracleUnavailable as why:
        fail("containers/IMAGES does not parse: %s" % why)
        return {}
    for problem in audit(table):
        fail(problem)
    return table


def check_images_rules_fire():
    """The control: each rule catches its own violation.

    Without this, `audit()` returning nothing is two different facts - the
    table is right, or the rules stopped reaching it - printing one line.
    """
    saved = oracle_env.IMAGES
    cases = [
        ("a stock image with no digest",
         "python\tdocker.io/library/python:3.14-slim\tunicodedata 16.0.0\tx\n"),
        ("built here and outside the naming convention",
         "idna\tlocalhost/some-other-image:1\tidnadata 17.0.0\tx\n"),
        ("an empty image or version field",
         "python\tdocker.io/library/python@sha256:abc\t\tx\n"),
        ("names no references at all",
         "# nothing but a comment\n"),
    ]
    for expect, text in cases:
        planted = images_saying(text)
        try:
            problems = " ".join(audit(oracle_env.pins()))
        except oracle_env.OracleUnavailable as why:
            problems = str(why)
        finally:
            restore_images(saved, planted)
        if expect not in problems:
            fail("control: a table that is %r was not caught (got %r)"
                 % (expect, problems))
    # And a two-field line must not parse at all, rather than parsing into
    # something with an empty version.
    planted = images_saying("python\tdocker.io/library/python@sha256:abc\n")
    try:
        oracle_env.pins()
    except oracle_env.OracleUnavailable:
        pass
    else:
        fail("control: a two-field IMAGES line was accepted")
    finally:
        restore_images(saved, planted)


def check_every_pin_can_be_asked(table):
    """A pin with no probe reports the empty string as its version.

    That is how `regex`'s copy of this started: a `-next` pin had no PROBE
    entry, so `version()` ran `<name> --version` inside the image, got nothing,
    and returned `''`. It failed closed only because `check_pin` then compared
    the pin against the empty string - the next pin whose version field happened
    to be empty would have passed.
    """
    for name in table:
        if oracle_env._probe(name) is None:
            fail("%s: no PROBE entry and no base name that has one" % name)


def check_probe_fallback():
    """`python-next` probes as `python`, and `nosuch-next` still has no probe."""
    if oracle_env._probe("python-next") is not oracle_env.PROBE["python"]:
        fail("_probe: a `-next` pin does not fall back to its base name")
    if oracle_env._probe("nosuch-next") is not None:
        fail("_probe: a name with no base in PROBE resolved to something")


def check_reference_stderr():
    """Both halves fire, and neither eats what it is not for."""
    noisy = ("Emulate Docker CLI using podman. Create /etc/containers/"
             "nodocker to quiet msg.\n"
             "idna 3.19, idnadata 17.0.0\n"
             "  File \"%s/tools/oracle/idna_ask.py\", line 41"
             % oracle_env.ROOT)
    got = oracle_env.reference_stderr(noisy)
    if "Emulate Docker CLI" in got:
        fail("reference_stderr: the engine's banner survived")
    if oracle_env.ROOT in got:
        fail("reference_stderr: an absolute path survived")
    if "idna 3.19, idnadata 17.0.0" not in got:
        fail("reference_stderr: it ate the reference's own output")
    if "tools/oracle/idna_ask.py" not in got:
        fail("reference_stderr: the relative path is not what was left")
    # The control for the control: a string with neither must come back whole.
    plain = "idna 3.19\nTraceback (most recent call last):"
    if oracle_env.reference_stderr(plain) != plain:
        fail("reference_stderr: it changed text that has nothing to remove")


def check_mode_is_closed():
    """An unknown GHOTI_ORACLE_MODE raises rather than picking one."""
    saved = oracle_env.MODE
    try:
        oracle_env.MODE = "whatever"
        try:
            oracle_env.ensure("python")
        except oracle_env.OracleUnavailable:
            pass
        else:
            fail("ensure: an unknown mode was accepted")
    finally:
        oracle_env.MODE = saved


def pinned_ucd():
    with open(UCD_VERSION_FILE, encoding="utf-8") as handle:
        return handle.read().strip()


def matched_pins(table, ucd):
    """The pins whose version field must name this library's own UCD version.

    Two of the three, and for different reasons. `idna`'s is the one that
    decides how much of the codespace the IDNA gate compares at all - a
    reference behind the pin cannot answer for what it does not assign, and the
    blind spot that opens is where a wrong DISALLOWED of ours passes. `python-
    next`'s is what makes `check-nfc-oracle-strict` mean what its name says: at
    a matching UCD a disagreement is necessarily a defect, and at any other
    version it is a finding to be read.

    The gating `python` pin is deliberately not in this list. It is a released
    interpreter and is *expected* to lag; the gate prints the gap.
    """
    problems = []
    for name in ("idna", "python-next"):
        if name not in table:
            problems.append("%s: no pin at all, and this library's %s gate "
                            "needs one at UCD %s" % (name, name, ucd))
            continue
        said = table[name][1]
        if ucd not in said:
            problems.append(
                "%s: IMAGES says %r, which does not name UCD %s from "
                "tools/idna/UCD_VERSION. Raising the UCD pin without raising "
                "this one puts the oracle behind the tables it checks."
                % (name, said, ucd))
    return problems


def check_matched_pins(table):
    """The committed table's matched pins agree with UCD_VERSION."""
    for problem in matched_pins(table, pinned_ucd()):
        fail(problem)


def check_matched_pins_fire():
    """The control: a stale matched pin, and a missing one, are both caught."""
    ucd = pinned_ucd()
    stale = {"idna": ("localhost/ghoti-text-oracle-idna:3.10",
                      "idnadata 15.1.0", ""),
             "python-next": ("docker.io/library/python@sha256:abc",
                             "unicodedata %s" % ucd, "")}
    problems = " ".join(matched_pins(stale, ucd))
    if "does not name UCD %s" % ucd not in problems:
        fail("control: a stale idna pin was not caught (got %r)" % problems)
    if "no pin at all" in problems:
        fail("control: a present pin was reported missing")
    problems = " ".join(matched_pins({}, ucd))
    if problems.count("no pin at all") != 2:
        fail("control: a missing matched pin was not caught (got %r)"
             % problems)


def xml_pin_problems(table):
    """The XML pin must name both references.

    The image holds two parsers that share no code, and a differential against one is
    a weaker thing than the same differential against both. A pin edited down to one
    version (or one that lost its image's tag) would keep every other check here green.
    """
    problems = []
    if "xml" not in table:
        return ["xml: no pin at all, and the XML differential needs one"]
    image, version, _ = table["xml"]
    for lib in ("libxml2 ", "expat "):
        if lib not in version:
            problems.append("xml: the version field %r does not name %s" % (version, lib.strip()))
    # The built-here tag spells both versions, so a rebuilt image cannot keep an old tag.
    # Exactly: the tag's `-` separated segments are the versions, in order. Substring
    # containment would pass 2.9.1 against a 2.9.14 tag, and 2.8 against 2.8.3.
    tag = image.rsplit(":", 1)[-1]
    versions = [w.strip(",") for w in version.split() if w[:1].isdigit()]
    if not versions or tag.split("-") != versions:
        problems.append("xml: the image tag %r does not carry the versions in %r" % (tag, version))
    return problems


def check_xml_pin(table):
    for problem in xml_pin_problems(table):
        fail(problem)


def check_xml_pin_fires():
    """The control: a pin that dropped expat, one with a stale tag, and a missing one."""
    good = {"xml": ("localhost/ghoti-text-oracle-xml:2.9.14-2.8.3", "libxml2 2.9.14, expat 2.8.3", "")}
    if xml_pin_problems(good):
        fail("control: a good xml pin was reported: %r" % xml_pin_problems(good))
    one = {"xml": ("localhost/ghoti-text-oracle-xml:2.9.14", "libxml2 2.9.14", "")}
    if not any("expat" in p for p in xml_pin_problems(one)):
        fail("control: an xml pin without expat was not caught")
    stale = {"xml": ("localhost/ghoti-text-oracle-xml:2.9.14-2.8.2", "libxml2 2.9.14, expat 2.8.3", "")}
    if not any("tag" in p for p in xml_pin_problems(stale)):
        fail("control: an xml pin with a stale tag was not caught")
    for said in ("libxml2 2.9.1, expat 2.8.3", "libxml2 2.9.14, expat 2.8"):
        prefix = {"xml": ("localhost/ghoti-text-oracle-xml:2.9.14-2.8.3", said, "")}
        if not any("tag" in p for p in xml_pin_problems(prefix)):
            fail("control: a version that is only a prefix of the tag's was not caught: %r" % said)
    if not xml_pin_problems({}):
        fail("control: a missing xml pin was not caught")


def html_pin_problems(table):
    """The HTML pin must name both references, and its tag must carry both versions.

    Same shape as the xml pin: the image holds two parsers that share no code, and a
    pin edited down to one (or a tag that no longer matches the versions) would keep
    every other check here green.
    """
    if "html" not in table:
        return ["html: no pin at all, and the HTML differential needs one"]
    image, version, _ = table["html"]
    problems = []
    for lib in ("html5lib ", "parse5 "):
        if lib not in version:
            problems.append("html: the version field %r does not name %s" % (version, lib.strip()))
    tag = image.rsplit(":", 1)[-1]
    versions = [w.strip(",") for w in version.split() if w[:1].isdigit()]
    if not versions or tag.split("-") != versions:
        problems.append("html: the image tag %r does not carry the versions in %r" % (tag, version))
    return problems


def css_pin_problems(table):
    """The CSS pin must name all three references, and its tag must carry their versions.

    The same shape as the html pin: the image holds three parsers that share no code, and a pin edited
    down to fewer (or a tag that no longer matches the versions) would keep every other check green. The
    tag is `<driver protocol version>-<the three versions>`.
    """
    if "css" not in table:
        return ["css: no pin at all, and the CSS differential needs one"]
    image, version, _ = table["css"]
    problems = []
    for lib in ("tinycss2 ", "cssparser ", "csstree "):
        if lib not in version:
            problems.append("css: the version field %r does not name %s" % (version, lib.strip()))
    tag = image.rsplit(":", 1)[-1]
    versions = [w.strip(",") for w in version.split() if w[:1].isdigit()]
    if not versions or tag.split("-")[1:] != versions:
        problems.append("css: the image tag %r does not carry the versions in %r" % (tag, version))
    return problems


def check_css_pin(table):
    for problem in css_pin_problems(table):
        fail(problem)


def check_css_pin_fires():
    """The control: a good pin, one missing a reference, a stale tag, and a missing pin."""
    good = {"css": ("localhost/ghoti-text-oracle-css:1.0-1.5.1-0.38.0-3.2.1", "tinycss2 1.5.1, cssparser 0.38.0, csstree 3.2.1", "")}
    if css_pin_problems(good):
        fail("control: a good css pin was reported: %r" % css_pin_problems(good))
    two = {"css": ("localhost/ghoti-text-oracle-css:1.0-1.5.1-0.38.0", "tinycss2 1.5.1, cssparser 0.38.0", "")}
    if not any("csstree" in p for p in css_pin_problems(two)):
        fail("control: a css pin without csstree was not caught")
    stale = {"css": ("localhost/ghoti-text-oracle-css:1.0-1.5.1-0.38.0-3.2.0", "tinycss2 1.5.1, cssparser 0.38.0, csstree 3.2.1", "")}
    if not any("tag" in p for p in css_pin_problems(stale)):
        fail("control: a css pin with a stale tag was not caught")
    if not css_pin_problems({}):
        fail("control: a missing css pin was not caught")


def dockerfile_args(path):
    """The ARG NAME=value lines of a Dockerfile."""
    out = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("ARG ") and "=" in line:
                name, value = line[4:].strip().split("=", 1)
                out[name] = value
    return out


CONTAINERS = os.path.join(HERE, "containers")
BROWSER_PARTS = ("chrome ", "chromedriver ", "firefox ", "geckodriver ", "ws ")


def cssprop_pin_problems(table, chromium_args=None, cssprop_args=None):
    """The CSS property pin must name the two browsers and their drivers, its tag must carry the two browser versions, the Dockerfile must
    fetch exactly what the pin says, and the Chrome of the sanitizer image must be the same build (one reference of Blink, not two)."""
    if "cssprop" not in table:
        return ["cssprop: no pin at all, and the CSS property differential needs one"]
    image, version, _ = table["cssprop"]
    problems = []
    for part in BROWSER_PARTS:
        if part not in version:
            problems.append("cssprop: the version field %r does not name %s" % (version, part.strip()))
    if "@sha256:" not in image:
        problems.append("cssprop: the image %r is not pinned by digest" % image)
    tag = image.rsplit("@", 1)[0].rsplit(":", 1)[-1]
    named = dict(w.split(" ", 1) for w in (p.strip() for p in version.split(",")) if " " in w)
    if tag != "%s-%s" % (named.get("chrome", "?"), named.get("firefox", "?")):
        problems.append("cssprop: the image tag %r is not <chrome version>-<firefox version> of %r" % (tag, version))
    if cssprop_args is not None:
        for arg, key in (("CHROME_VERSION", "chrome"), ("CHROME_VERSION", "chromedriver"), ("FIREFOX_VERSION", "firefox"),
                         ("GECKODRIVER_VERSION", "geckodriver")):
            if cssprop_args.get(arg) != named.get(key):
                problems.append("cssprop: the Dockerfile fetches %s %s and the pin says %s" % (arg, cssprop_args.get(arg), named.get(key)))
        for arg in ("CHROME_SHA256", "CHROMEDRIVER_SHA256", "FIREFOX_SHA256", "GECKODRIVER_SHA256"):
            if not re.fullmatch(r"[0-9a-f]{64}", cssprop_args.get(arg, "")):
                problems.append("cssprop: the Dockerfile has no SHA-256 for %s" % arg)
    if chromium_args is not None and cssprop_args is not None:
        for arg in ("CHROME_VERSION", "CHROME_SHA256"):
            if chromium_args.get(arg) != cssprop_args.get(arg):
                problems.append("chromium and cssprop fetch different builds of Chrome: %s %s against %s" % (arg, chromium_args.get(arg), cssprop_args.get(arg)))
        if "chromium" in table and chromium_args.get("CHROME_VERSION", "?") not in table["chromium"][1]:
            problems.append("chromium: the pin %r does not name the Dockerfile's Chrome %s" % (table["chromium"][1], chromium_args.get("CHROME_VERSION")))
    return problems


def check_cssprop_pin(table):
    chromium = dockerfile_args(os.path.join(CONTAINERS, "chromium", "Dockerfile"))
    cssprop = dockerfile_args(os.path.join(CONTAINERS, "cssprop", "Dockerfile"))
    for problem in cssprop_pin_problems(table, chromium, cssprop):
        fail(problem)


def check_cssprop_pin_fires():
    """The control: a good pin, a pin missing a driver, a stale tag, an undigested image, a Dockerfile of another version, two builds of Chrome."""
    good_version = "chrome 156.0.8078.47, chromedriver 156.0.8078.47, firefox 158.0.1, geckodriver 0.37.1, ws 8.22.0"
    image = "localhost/ghoti-text-oracle-cssprop:156.0.8078.47-158.0.1@sha256:" + "0" * 64
    args = {"CHROME_VERSION": "156.0.8078.47", "CHROME_SHA256": "1" * 64, "CHROMEDRIVER_SHA256": "2" * 64, "FIREFOX_VERSION": "158.0.1",
            "FIREFOX_SHA256": "3" * 64, "GECKODRIVER_VERSION": "0.37.1", "GECKODRIVER_SHA256": "4" * 64}
    chromium = {"CHROME_VERSION": "156.0.8078.47", "CHROME_SHA256": "1" * 64}
    table = {"cssprop": (image, good_version, ""), "chromium": ("x", "Google Chrome for Testing 156.0.8078.47", "")}
    if cssprop_pin_problems(table, chromium, args):
        fail("control: a good cssprop pin was reported: %r" % cssprop_pin_problems(table, chromium, args))
    cases = [
        ("a pin without a driver", {"cssprop": (image, good_version.replace("geckodriver 0.37.1, ", ""), "")}, chromium, args, "geckodriver"),
        ("a stale tag", {"cssprop": (image.replace("158.0.1", "158.0.0"), good_version, "")}, chromium, args, "tag"),
        ("an image with no digest", {"cssprop": (image.split("@")[0], good_version, "")}, chromium, args, "digest"),
        ("a Dockerfile of another Firefox", table, chromium, dict(args, FIREFOX_VERSION="157.0"), "FIREFOX_VERSION"),
        ("a Dockerfile with no hash", table, chromium, dict(args, CHROMEDRIVER_SHA256="x"), "SHA-256"),
        ("two builds of Chrome", table, dict(chromium, CHROME_SHA256="9" * 64), args, "different builds"),
        ("a chromium pin that names another Chrome", dict(table, chromium=("x", "Google Chrome for Testing 155.0.8059.84", "")), chromium, args, "chromium: the pin"),
    ]
    for what, tbl, chr_args, css_args, expect in cases:
        if not any(expect in p for p in cssprop_pin_problems(tbl, chr_args, css_args)):
            fail("control: %s was not caught" % what)
    if not cssprop_pin_problems({}):
        fail("control: a missing cssprop pin was not caught")


def check_html_pin(table):
    for problem in html_pin_problems(table):
        fail(problem)


def check_html_pin_fires():
    """The control: a pin that dropped parse5, one with a stale tag, and a missing one."""
    good = {"html": ("localhost/ghoti-text-oracle-html:1.1-8.0.1", "html5lib 1.1, parse5 8.0.1", "")}
    if html_pin_problems(good):
        fail("control: a good html pin was reported: %r" % html_pin_problems(good))
    one = {"html": ("localhost/ghoti-text-oracle-html:1.1", "html5lib 1.1", "")}
    if not any("parse5" in p for p in html_pin_problems(one)):
        fail("control: an html pin without parse5 was not caught")
    stale = {"html": ("localhost/ghoti-text-oracle-html:1.1-8.0.0", "html5lib 1.1, parse5 8.0.1", "")}
    if not any("tag" in p for p in html_pin_problems(stale)):
        fail("control: an html pin with a stale tag was not caught")
    prefix = {"html": ("localhost/ghoti-text-oracle-html:1.1-8.0.1", "html5lib 1.1, parse5 8.0", "")}
    if not any("tag" in p for p in html_pin_problems(prefix)):
        fail("control: a version that is only a prefix of the tag's was not caught")
    if not html_pin_problems({}):
        fail("control: a missing html pin was not caught")


def encoding_pin_problems(table):
    """The Encoding pin must be the html pin's image, and its version field must name Node, ICU and Python.

    It asks Node and CPython in the html image, and does not build one: a pin that drifted to another tag would answer from a
    different Node, and one whose line lost ICU would keep every other check here green while the comparison it describes (the
    standard's labels over ICU's converters) changed under it.
    """
    if "encoding" not in table:
        return ["encoding: no pin at all, and the Encoding differential needs one"]
    image, version, _ = table["encoding"]
    problems = []
    for word in ("node ", "icu ", "python "):
        if word not in version:
            problems.append("encoding: the version field %r does not name %s" % (version, word.strip()))
    if "html" not in table:
        problems.append("encoding: there is no html pin whose image it could share")
    elif table["html"][0] != image:
        problems.append("encoding: the image %r is not the html pin's, %r" % (image, table["html"][0]))
    return problems


def check_encoding_pin(table):
    for problem in encoding_pin_problems(table):
        fail(problem)


def check_encoding_pin_fires():
    """The control: a pin without ICU, one on another image, and a missing one."""
    html = ("localhost/ghoti-text-oracle-html:1.1-8.0.1", "html5lib 1.1, parse5 8.0.1", "")
    good = {"html": html, "encoding": (html[0], "node 20.19.2, icu 76.1, python 3.13.5", "")}
    if encoding_pin_problems(good):
        fail("control: a good encoding pin was reported: %r" % encoding_pin_problems(good))
    no_icu = {"html": html, "encoding": (html[0], "node 20.19.2, python 3.13.5", "")}
    if not any("icu" in p for p in encoding_pin_problems(no_icu)):
        fail("control: an encoding pin without ICU was not caught")
    other = {"html": html, "encoding": ("localhost/ghoti-text-oracle-html:1.1-8.0.2", "node 20.19.2, icu 76.1, python 3.13.5", "")}
    if not any("not the html pin" in p for p in encoding_pin_problems(other)):
        fail("control: an encoding pin on another image was not caught")
    if not encoding_pin_problems({"html": html}):
        fail("control: a missing encoding pin was not caught")


def main():
    table = check_images_parses()
    check_images_rules_fire()
    check_every_pin_can_be_asked(table)
    check_probe_fallback()
    check_reference_stderr()
    check_mode_is_closed()
    check_matched_pins(table)
    check_matched_pins_fire()
    check_xml_pin(table)
    check_xml_pin_fires()
    check_html_pin(table)
    check_html_pin_fires()
    check_css_pin(table)
    check_css_pin_fires()
    check_cssprop_pin(table)
    check_cssprop_pin_fires()
    check_encoding_pin(table)
    check_encoding_pin_fires()
    if FAILURES:
        sys.stderr.write("\033[0;31m\n### The oracle pin table is wrong ###"
                         "\033[0m\n")
        for message in FAILURES:
            sys.stderr.write("  %s\n" % message)
        return 1
    print("oracle pins: %d references, all askable; IMAGES parses; the stderr "
          "filter fires both ways; idna and python-next name UCD %s; the xml pin names "
          "libxml2 and expat; the html pin names html5lib and parse5; the css pin names tinycss2, cssparser and csstree; the cssprop pin names Chrome, chromedriver, Firefox and geckodriver at the versions its Dockerfile fetches, and shares its Chrome with the chromium pin; the encoding pin names node, icu and python, in the html image." % (len(table), pinned_ucd()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
