#!/usr/bin/env python3
"""How an oracle is spelled, so that no tool here spells one itself.

The pattern is the suite-wide one; `unicode` landed it first, then `chron`,
`font`, `compress` and `regex`. This library is
the sixth and the last with an oracle to take it, and it had two references
living on whatever this machine happened to have installed:

- **CPython's `unicodedata`**, which `check-nfc-oracle` compares NFC against.
  Debian 13 gives UCD 15.1.0 where this library pins 17.0.0, and the gate
  printed the gap rather than closing it. Worth stating precisely, because it
  is the reason the version field below names a UCD and not an interpreter:
  CPython 3.13.5 and 3.13.15 both report `unidata_version == '15.1.0'` and
  disagree about `unicodedata.decomposition()` for all 11,172 Hangul
  syllables. A UCD version is not a sufficient pin. A digest is.

- **`python-idna`**, which `check-idna-oracle` compares the derived IDNA
  property against. Debian 13 packages 3.10, whose *vendored* tables are
  15.1.0, and no interpreter can change them - so this one needs an image
  built here. It is also the pin with the most to gain: 3.19 carries 17.0.0
  exactly, which retires the blind spot that gate used to print.

`command(name)` returns the argv prefix that runs a reference, which is either
this machine's own tool or a `docker run` into an image pinned in
`containers/IMAGES`.

Three properties, in the order they matter:

  1. **It does not fail open.** A missing image, a missing engine, or a version
     that does not match its pin each raises. There is deliberately no "try the
     container, fall back to the host": a gate whose reference is not the one it
     names is worse than one that did not run, because it prints the same green
     line. Host tools have to be asked for by name. This library spent today
     fixing four gates that exited 0 while unable to see, so that rule is not
     abstract here.

  2. **It says which instrument answered.** `provenance()` returns the line
     every gate prints above its numbers. For both references here the *data*
     version is the claim rather than the tool's own release, which is why the
     version field of IMAGES is a UCD and is checked rather than trusted.

  3. **Paths mean the same thing on both sides.** The repository is mounted at
     its own host path, so a path a caller already built resolves unchanged -
     including the pinned UCD under `third_party/`, which both references read.

**Where this library departs from its siblings.** `unicode` relaxes
`check_pin()` in host mode because every image it names is a stock one;
`font` and `compress` assert in both modes because none of theirs is. Like
`regex`, this library has both kinds, so it follows `regex`: host mode reports
rather than asserts, and names the pin it is not. The escape hatch is for a
machine with no container engine, and that is exactly the machine whose
`python3` and `idna` are not the pinned ones.

Modes, from GHOTI_ORACLE_MODE:

  container  (default) run the reference in its pinned image
  host                 run this machine's own tool, and print what it is

Copyright 2026 by Corey Pennycuff
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Two directories up, so this file has to live at <repo>/tools/oracle/.
ROOT = os.path.dirname(os.path.dirname(HERE))
IMAGES = os.path.join(HERE, "containers", "IMAGES")

MODE = os.environ.get("GHOTI_ORACLE_MODE", "container")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "docker")


class OracleUnavailable(Exception):
    """The reference cannot be reached. Never caught into a skip."""


# How to ask each reference for its version, what the answer must contain, and
# which of its lines to read. Host mode has no digest to trust, so it is asked
# here; container mode is asked too, because IMAGES is maintained by hand and a
# version field that has drifted from the image it names is a lie nothing else
# would catch.
#
# Both probes report a *data* version rather than a tool version, because that
# is what decides an answer here. For `python` the interpreter is printed beside
# it - it is not the claim, but it is the thing a reader needs when two
# interpreters carrying one UCD disagree, which is a case this library has met.
# For `idna` the interpreter is not printed at all, and that is deliberate: the
# comparison reads no `unicodedata` from that image, so naming one would suggest
# a dependency the gate does not have.
PROBE = {
    "python": (["python3", "-c",
                "import sys, unicodedata; print('python %s, unicodedata %s'"
                " % (sys.version.split()[0], unicodedata.unidata_version))"],
               "unicodedata ", None),
    # `tomllib` has no version of its own - it is part of CPython - so the
    # interpreter is the claim, and the probe imports the module so that an
    # image without it fails here rather than in the middle of a comparison.
    "tomllib": (["python3", "-c",
                 "import sys, tomllib;"
                 " print('python %s, tomllib in the standard library'"
                 " % sys.version.split()[0])"],
                "tomllib", None),
    # toml++'s driver prints its version, whether the unreleased set is on, and
    # the SHA-256 of the driver source it was compiled from. Asked with no input,
    # it writes that line and stops.
    "tomlpp": (["toml-driver", "--version"], "toml++", None),
    # Both Desktop Entry references in one driver, so the version line names
    # both packages plus the driver's own SHA-256. The driver writes it with no
    # protocol prefix and stops, the way toml-driver does.
    "inidesktop": (["ini-driver", "--version"], "glib ", None),
    # git config's only reference is git, so the version line names the Debian
    # package version plus the driver's own SHA-256. The prefix is `git ` because
    # that is what pins the answers - there is no data version here, unlike the
    # UCD pins above, since a config file's grammar is git's source and nothing
    # else.
    "gitconfig": (["git-config-driver", "--version"], "git ", None),
    # EditorConfig has **two** references and this image carries both, so one
    # digest pins both and there is one probe rather than two. The C driver's line
    # is asked because it is the one that names a source commit; the Python
    # driver's version is checked by the same IMAGES line, which holds both.
    "editorconfig": (["editorconfig-driver", "--version"],
                     "editorconfig-core-c ", None),
    "idna": (["python3", "-c",
              "import idna, idna.idnadata, idna.uts46data;"
              " print('idna %s, idnadata %s, uts46data %s'"
              " % (idna.__version__, idna.idnadata.__version__,"
              " idna.uts46data.__version__))"],
             "idnadata ", None),
}


def _probe(name, default=None):
    """The PROBE entry for a pin, falling back to the base of a `-` name.

    A second pin on the same reference is spelled `<base>-<qualifier>` -
    `python-next` beside `python` - and asks the same questions of a different
    version, so it wants the same probe. `unicode` spells that as one assignment
    per alias (`PROBE["python-next"] = PROBE["python"]`); `regex` made it a rule
    instead, so that a pin added to IMAGES needs nothing added here. Taken from
    `regex` rather than reinvented, along with the reason: a forgotten entry
    there was a `version()` that ran `python-next --version` inside the image,
    got nothing, and reported the empty string as the version.
    """
    if name in PROBE:
        return PROBE[name]
    if "-" in name and name.split("-", 1)[0] in PROBE:
        return PROBE[name.split("-", 1)[0]]
    return default


_pins = None
_cache = {}


def pins():
    """The IMAGES table: name -> (image, version, description)."""
    global _pins
    if _pins is not None:
        return _pins
    _pins = {}
    if not os.path.exists(IMAGES):
        return _pins
    with open(IMAGES, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise OracleUnavailable(
                    "containers/IMAGES: not three tab-separated fields: %r"
                    % line)
            _pins[parts[0]] = (parts[1], parts[2],
                               parts[3] if len(parts) > 3 else "")
    return _pins


def _engine_ok():
    if shutil.which(ENGINE) is None:
        raise OracleUnavailable(
            "%s is not on PATH, and GHOTI_ORACLE_MODE is 'container'.\n"
            "Install it, or run with GHOTI_ORACLE_MODE=host to use this "
            "machine's own tools - which answers a different question, and "
            "says so in the line it prints." % ENGINE)


def _have_image(image):
    finished = subprocess.run([ENGINE, "image", "exists", image],
                              capture_output=True)
    if finished.returncode == 0:
        return True
    # `image exists` is podman's. Fall back to a docker-portable spelling.
    finished = subprocess.run([ENGINE, "image", "inspect", image],
                              capture_output=True)
    return finished.returncode == 0


def ensure(name):
    """Make the reference runnable, or raise saying what is missing."""
    if MODE == "host":
        binary = _probe(name, ([name], "", None))[0][0]
        if shutil.which(binary) is None:
            raise OracleUnavailable(
                "GHOTI_ORACLE_MODE=host and %s is not on PATH" % binary)
        return
    if MODE != "container":
        raise OracleUnavailable("GHOTI_ORACLE_MODE=%r is not a mode" % MODE)
    _engine_ok()
    table = pins()
    if name not in table:
        raise OracleUnavailable(
            "no pin for %r in tools/oracle/containers/IMAGES" % name)
    image = table[name][0]
    if _have_image(image):
        return
    # A built-here image cannot be pulled, and saying "pull failed" for one
    # would send the reader to the registry instead of to the Dockerfile.
    if image.startswith("localhost/"):
        raise OracleUnavailable(
            "the %s image is built here and is not present: %s\n"
            "Build it with: make oracle-images" % (name, image))
    if os.environ.get("GHOTI_ORACLE_PULL", "1") != "1":
        raise OracleUnavailable(
            "image for %s is not present and GHOTI_ORACLE_PULL is off: %s"
            % (name, image))
    sys.stderr.write("oracle: pulling %s\n" % image)
    finished = subprocess.run([ENGINE, "pull", image], capture_output=True,
                              text=True)
    if finished.returncode != 0:
        raise OracleUnavailable(
            "could not pull the pinned image for %s.\n  %s\n%s"
            % (name, image, finished.stderr.strip()))


# Ask one oracle's questions of a different pin, e.g.
#   GHOTI_ORACLE_ALIAS=python=python-next make check-nfc-oracle
# which is why IMAGES carries two CPythons: the gating pin is a released
# interpreter, and the second one carries this library's own UCD version. Only
# one CPython can be installed at a time, and the difference between the two
# runs is a reading of what the reference's age costs.
ALIAS = dict(
    pair.split("=", 1)
    for pair in os.environ.get("GHOTI_ORACLE_ALIAS", "").split(",")
    if "=" in pair)


def command(name, argv=None, scratch=None, readonly=None, env=None):
    """The argv prefix that runs `name`'s reference.

    `argv` is what to run inside, defaulting to the reference's own tool. The
    repository is bind-mounted at its own path, so any path a caller has
    already built resolves without translation - which is what lets
    `tools/oracle/nfc_ask.py` and the pinned UCD under `third_party/` be named
    the same way on both sides.

    `scratch` names directories the reference must be able to *write*, mounted
    at the same path. Nothing here needs one: both references answer on stdout
    and read on stdin. The parameter is kept rather than dropped so that a tool
    which later needs one fails on a path that does not exist instead of
    writing somewhere nobody reads.

    `readonly` names further paths to mount read-only at the same path.

    Everything else is closed: `--network none`, because neither reference has
    business reaching the network - the one fetch this library's oracles make is
    the UTS #46 mapping table, and it happens on the host, before the reference
    runs, precisely so that this can stay off. The tree is read-only, because a
    corpus quietly edited by the thing being compared against it is not a
    comparison.
    """
    name = ALIAS.get(name, name)
    ensure(name)
    inner = argv if argv is not None else [_probe(name, ([name],))[0][0]]
    if MODE == "host":
        return list(inner)
    image = pins()[name][0]
    out = [ENGINE, "run", "--rm", "-i",
           "--network", "none",
           "--volume", "%s:%s:ro" % (ROOT, ROOT)]
    for path in ([readonly] if isinstance(readonly, str) else (readonly or [])):
        out += ["--volume", "%s:%s:ro" % (path, path)]
    for path in ([scratch] if isinstance(scratch, str) else (scratch or [])):
        out += ["--volume", "%s:%s:rw" % (path, path)]
    for key, value in (env or {}).items():
        out += ["--env", "%s=%s" % (key, value)]
    return out + ["--workdir", ROOT, image] + list(inner)


def version(name):
    """What the reference says it is. Runs it; the answer is cached."""
    name = ALIAS.get(name, name)
    key = ("version", name)
    if key in _cache:
        return _cache[key]
    probe, expect, select = _probe(name, ([name, "--version"], "", None))
    finished = subprocess.run(command(name, probe), capture_output=True,
                              text=True)
    #
    # stdout only. `docker` on this machine is a podman shim that prints a
    # banner to stderr on every invocation, and a probe reading both streams
    # reads the banner. The same trap waits for any driver that merges them:
    # the reference's answers and the engine's chatter would interleave on one
    # stream and the extra line would be scored as a disagreement.
    #
    lines = finished.stdout.strip().splitlines()
    if select is not None:
        lines = [line for line in lines if select in line]
    text = lines[0].strip() if lines else ""
    if expect and expect not in text:
        raise OracleUnavailable(
            "%s answered %r, which does not look like a version.\n%s"
            % (name, text, reference_stderr(finished.stderr.strip())))
    _cache[key] = text
    return text


def check_pin(name):
    """Raise unless the reference's version matches containers/IMAGES.

    Container mode asserts. Host mode reports, because the escape hatch exists
    for a machine with no container engine, and that machine is exactly the one
    whose `python3` and `idna` are not the pinned ones - an assertion there
    would close the hatch for everybody it is for. What host mode does instead
    is name the pin it is not; see `provenance()`.
    """
    name = ALIAS.get(name, name)
    table = pins()
    if name not in table:
        return version(name)
    said = table[name][1]
    got = version(name)
    if MODE != "host" and said not in got:
        raise OracleUnavailable(
            "%s: IMAGES says %s and it answers %r" % (name, said, got))
    return got


# A reference's stderr, made fit to print or to record.
#
# Taken from `regex`, where it was found by the control that conversion needed
# anyway: this machine's `docker` is a shell script that prints a banner and
# execs podman, so the banner arrives on the same stream as the reference's own
# diagnostics. It reached a *committed* corpus header there. Nothing here keeps
# a reference's stderr in a committed file, so the stake is lower - but a gate
# that prints the banner in its failure message teaches a reader to ignore the
# line the real error is on.
ENGINE_NOISE = (
    "Emulate Docker CLI using podman.",
)


def reference_stderr(text):
    """A reference's stderr with the engine's lines out and paths relative.

    Both halves are no-ops in host mode on a tree that is where it was, so a
    call site does not have to know which it is talking to.
    """
    lines = [line for line in text.splitlines()
             if not any(noise in line for noise in ENGINE_NOISE)]
    return "\n".join(line.replace(ROOT + os.sep, "") for line in lines)


def decline(where, why):
    """Report a reference this gate cannot reach, and say what that means.

    Returns the exit status the gate should use: 1 under
    GHOTI_ORACLE_REQUIRED=1, otherwise 0 with the word SKIPPED and a reason.

    `oracle_run.py` calls this for a reference it could not resolve *before* the
    gate ran. A gate calls it directly for the other case: the reference
    resolved, answered its version, and only then turned out not to hold what
    this particular comparison needs. One spelling of the protocol rather than
    two, because a second one would drift - and the shape of the line is what a
    reader greps for.
    """
    gate = os.environ.get("GHOTI_ORACLE_GATE", "")
    # The per-gate opt-out, in the shape this library's other unreachable-input
    # messages use: a gate that cannot be run is dropped by name, in the
    # command, so the choice is visible rather than silent. There is
    # deliberately no global switch.
    escape = ""
    if gate:
        escape = ("\nOr drop the gate for the run, so the choice is in the "
                  "command:\n\n"
                  "  make test TEST_GATES='$(filter-out %s,$(ALL_TEST_GATES))'"
                  "\n" % gate)
    if os.environ.get("GHOTI_ORACLE_REQUIRED", "0") == "1":
        sys.stderr.write(
            "\033[0;31m### %s: the reference this gate needs is not "
            "available ###\033[0m\n%s\n%s" % (where, why, escape))
        return 1
    sys.stderr.write("SKIPPED %s\n  %s\n" % (where, why))
    return 0


def provenance(names):
    """One line naming every reference that answered, and how.

    The name printed is the one that *answered*, not the one the gate asked
    for. Under an alias those differ, and printing the requested name makes the
    line name the wrong pin. The alias is shown too, because "which gate was
    this" is the other question the line has to answer.

    In host mode the pin is printed beside the answer wherever the two differ.
    A line that says only `host, unpinned` tells a reader that no pin was
    enforced; it does not tell them that the `idna` which just answered is two
    UCD releases behind the tables it was checking.
    """
    where = "container" if MODE == "container" else "host, unpinned"
    table = pins()
    parts = []
    for name in names:
        resolved = ALIAS.get(name, name)
        label = resolved if resolved == name else "%s as %s" % (resolved, name)
        got = check_pin(name)
        said = table.get(resolved, (None, None))[1]
        if MODE == "host" and said and said not in got:
            parts.append("%s %s [pin: %s]" % (label, got, said))
        else:
            parts.append("%s %s" % (label, got))
    return "oracle(%s): %s" % (where, ", ".join(parts))


if __name__ == "__main__":
    # `make oracle-version`: resolve every pin and say what answered, so that
    # "are the images here and do they match" is one command rather than a gate
    # run.
    status = 0
    for pinned in sorted(pins()):
        try:
            print("%-12s %s" % (pinned, check_pin(pinned)))
        except OracleUnavailable as why:
            sys.stderr.write("%-12s UNAVAILABLE: %s\n" % (pinned, why))
            status = 1
    sys.exit(status)
