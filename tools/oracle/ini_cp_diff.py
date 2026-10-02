#!/usr/bin/env python3
"""Compare this library's configparser reader against CPython's `configparser`.

    ini_cp_diff.py <ours>

INI_CP_ORACLE_COUNT and INI_CP_ORACLE_SEED size and seed the population; the reference
is reached through tools/oracle/oracle_env.py, which resolves and version-checks the
pinned image before anything is compared.

Seven scores:

  `intent`     our accept/refuse against what tools/oracle/ini_cp_gen.py's REFUSES
               says the dialect means. **The only independent reading there is**, and
               it matters more here than for any other dialect in this module: this
               one has no specification, so without it a rule both the reference and
               this library got wrong would pass every other score. For git, systemd
               and EditorConfig a document can be checked against prose; here the
               generator's table is the prose.
  `verdict`    our accept/refuse against the reference's.
  `values`     every section, key and **joined** value, in document order, over the
               documents free of a construct either side is known to answer
               differently.
  `rewrite`    every accepted document writes back byte for byte. No reference.
  `divergence` per axis, that each known departure is **still observable**. Three of
               them, all one thing: `configparser` works on Python `str`, so its
               whitespace and its case folding are Unicode's and a byte-oriented
               reader cannot ask either question of a single byte. A change to the
               fold or the whitespace set fails this loudly rather than quietly
               inflating `values`.
  `interp`     gtext_ini_value_interpolate() against `BasicInterpolation` and
               `ExtendedInterpolation`, the things it reimplements: verdict and value
               for every non-excluded document under **both** styles, so two records
               per document.
  `pin`        per axis, that `interpolation=None` is still load-bearing - that the
               reference's own *default* still answers differently from the pinned
               configuration. See ini_cp_gen.PIN_INTERPOLATION.
  `defaults`   the **defaults chain**, under a fifth and sixth configuration in which
               `default_section` is `DEFAULT` and `[DEFAULT]`'s inheritance is live.
               Two scores in one: that the value agrees for every key our own tree
               holds, and - `defaults-pin` - that the pinned configuration still
               *refuses* what the live one resolves, so the fifth configuration is
               keeping something in rather than duplicating the third.

**`default_section` was the pin that excluded a rule by construction**, which is a
worse shape than a pin nothing asserts. Pinning it to a name no document can spell
makes `[DEFAULT]` an ordinary section on both sides - correct, since inheritance is a
lookup policy over a parsed tree and not a rule of the grammar - and it silently took
the whole defaults chain out of scope. §22.7 of notes/text/INI-DIALECTS.md is the cost:
`${sect:key}` did not consult the defaults, the comment over the wrong code claimed the
rule had been *measured* when it had been inferred, and no instrument here could fail.
The unit test that carries it now was written from a probe, so nothing in this tree would
notice if the reference changed. The `defaults` score is that gap closed: the reference
is asked twice more, with the inheritance live.

Under those two runs the reference's `items(section)` carries the defaults' keys into
every section, exactly as the pin's rationale says. So **only the keys our own tree holds
are compared there**, and the completeness of the key set stays where it already was -
`values`, under the pinned configuration, which compares every key exhaustively. A score
that compared the full key set under the live configuration would be asserting that this
module copies a lookup policy into its tree, which it deliberately does not.

**`interpolation=None` was the one pin nothing could fail on**, and the last two
scores are why this file runs the reference three times. A pin is wider than an
exclusion: an exclusion names a document and keeps the knowledge, while a pin removes
a behaviour from the comparison. The four `%`/`$` axes were generated and scored all
along - under a configuration in which both sides answer `100%` with `100%`, which is
agreement about nothing. Now the departure is asserted and the reimplementation is
measured against the reference rather than against a reading of CPython's source.

**The reference is one implementation and it is also the specification.** That is the
weakest position any dialect in this module is in - weaker than git's lone reference,
because there at least `git-config(1)` exists to disagree with git. So two things
carry the weight instead: `intent`, above, and the local corpus gate
`make conformance-ini-configparser`, whose 703 real files include 224 the reference
refuses and so can score a refusal that no generated document chose.

**The channel is a file**, not a string, because Python's universal-newline
translation is applied to one and not the other and they disagree about a lone CR.
See tools/oracle/containers/configparser/driver.py.

Copyright 2026 by Corey Pennycuff
"""

import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ini_cp_gen  # noqa: E402
import ini_status  # noqa: E402
import oracle_env  # noqa: E402

NAME = "configparser"

# The configurations each side is asked for, and the arguments that select each. `none`
# is the pinned one every existing score reads; `basic` and `extended` exist so that the
# interpolation pin can be failed rather than printed; the two `-defaults` runs turn
# `default_section` live so that the defaults chain is in the comparison at all.
#
# Both sides take the same flags, which is not a coincidence to be relied on quietly:
# `tools/oracle/ini_cp_ours.c` was given the same spellings precisely so that this table
# has one column instead of two and cannot say different things to the two sides.
DEFAULT_SECTION = "DEFAULT"
RUNS = {
    "none": ["--interpolation=none"],
    "basic": ["--interpolation=basic"],
    "extended": ["--interpolation=extended"],
    "basic-defaults": ["--interpolation=basic",
                       "--default-section=" + DEFAULT_SECTION],
    "extended-defaults": ["--interpolation=extended",
                          "--default-section=" + DEFAULT_SECTION],
}


def as_tree(body):
    """A record's `G`/`E` lines as [(section, {key: value})], in document order.

    Only the `defaults` score needs this: every other comparison is of the body lines
    verbatim, which is the stronger check and the right one wherever both sides emit the
    same key set. Here they deliberately do not.
    """
    tree = []
    for line in body:
        if line.startswith("G "):
            tree.append((line[2:], {}))
        elif line.startswith("E ") and tree:
            key, _, value = line[2:].partition(" ")
            tree[-1][1][key] = value
    return tree

# How the reference spells an interpolation refusal, against the code this module
# reports. Compared by *name* only for these three, because they are the refusals the
# `interp` score exists for; a parse-level refusal is left to `verdict`, which already
# owns it and compares accept-against-refuse rather than reason-against-reason.
#
# `InterpolationDepthError` shares a code with `InterpolationSyntaxError` on purpose:
# both say the document is wrong, and `configparser`'s own limit is a cap rather than
# a cycle detector, so the two are not separable by anything a caller could act on.
INTERPOLATION_REFUSALS = {
    "InterpolationSyntaxError": "GTEXT_INI_E_INTERPOLATION",
    "InterpolationDepthError": "GTEXT_INI_E_INTERPOLATION",
    "InterpolationMissingOptionError": "GTEXT_INI_E_INTERPOLATION_MISSING",
}

# The driver inside the image is this file's neighbour, and the repository is
# bind-mounted at its own path - so it is named the same way on both sides and there is
# no built image to keep in step with it. What pins the reference is the interpreter
# digest in containers/IMAGES plus this file, which is in the tree the gate ran from.
DRIVER = os.path.join(HERE, "containers", "configparser", "driver.py")


def parse_records(text, count, who):
    """Split a driver's output into one record per document."""
    records = []
    current = None
    for line in text.split("\n"):
        if line == "BEGIN":
            current = {"status": None, "body": [], "rewrite": None}
            continue
        if line == "END":
            if current is not None:
                records.append(current)
            current = None
            continue
        if current is None:
            continue
        if line.startswith("cp "):
            current["status"] = line[3:]
            continue
        if line.startswith("W "):
            current["rewrite"] = line[2:]
            continue
        if line:
            current["body"].append(line)
    if len(records) != count:
        sys.stderr.write("%s answered %d of %d documents\n"
                         % (who, len(records), count))
    return records


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__)
    ours = argv[1]
    count = int(os.environ.get("INI_CP_ORACLE_COUNT", "20000"))
    seed = int(os.environ.get("INI_CP_ORACLE_SEED", "20260929"))

    print("provenance: %s" % oracle_env.check_pin(NAME))
    print("population: %d generated documents, seed %d" % (count, seed))
    print("configuration: interpolation=None, strict=True, allow_no_value=False,")
    print("               inline_comment_prefixes=None, empty_lines_in_values=True,")
    print("               default_section=<unspellable>, read from a file")
    print("               and two more runs with default_section=%s, where the "
          "[DEFAULT]" % DEFAULT_SECTION)
    print("               inheritance is live - the pin that excluded the defaults "
          "chain from")
    print("               this gate by construction rather than by naming a "
          "document")
    print("our side:       max_output=SIZE_MAX, so the one deliberate departure - a "
          "bound on")
    print("               the expansion, which the reference has none of - cannot "
          "read as a")
    print("               disagreement here. IniInterpolation.TheOutputIsBounded "
          "asserts it.")
    print("               interpolation is the one pin this gate also *asserts*: the "
          "reference")
    print("               is run again with Basic and with Extended, both to score "
          "our own")
    print("               pass and to fail if the default stops answering "
          "differently")

    documents = list(ini_cp_gen.documents(seed, count))
    payload = bytearray()
    for data, _axes, _expected in documents:
        payload += (b"%d\n" % len(data)) + data
    payload += b"-1\n"

    # Both sides, once per configuration. Ten processes rather than two, and the
    # reference is reached through the same pinned image each time - `check_pin` runs
    # once because it is the image that is pinned, not the invocation.
    mine = {}
    theirs = {}
    for which, flags in RUNS.items():
        proc = subprocess.run([ours] + flags, input=bytes(payload),
            capture_output=True)
        if proc.returncode != 0:
            print("FAIL: our runner exited %d under %s"
                  % (proc.returncode, which))
            print(proc.stderr.decode("utf-8", "replace")[:3000])
            return 1
        mine[which] = parse_records(
            proc.stdout.decode("utf-8", "surrogateescape"), count,
            "our runner (%s)" % which)
        if len(mine[which]) != count:
            return 1

        command = oracle_env.command(NAME, ["python3", DRIVER] + flags)
        proc = subprocess.run(command, input=bytes(payload), capture_output=True)
        if proc.returncode != 0:
            print("FAIL: the reference driver exited %d under %s"
                  % (proc.returncode, which))
            print(proc.stderr.decode("utf-8", "replace")[:3000])
            return 1
        theirs[which] = parse_records(
            proc.stdout.decode("utf-8", "surrogateescape"), count,
            "the reference (%s)" % which)
        if len(theirs[which]) != count:
            return 1

    # Read from the header, so inserting a code cannot silently shift what this
    # compares against. See tools/oracle/ini_status.py.
    codes = ini_status.codes()
    intent = [0, 0]
    verdict = [0, 0]
    values = [0, 0]
    rewrite = [0, 0]
    excluded = 0
    diverged = set()
    divergence = [0, 0]
    interp = [0, 0]
    pin = [0, 0]
    defaults = [0, 0]
    defaults_pin = [0, 0]
    defaults_axes = collections.Counter()
    pinned_axes = collections.Counter()
    used = set()
    refusals = collections.Counter()
    failures = []

    for i, (data, axes, expected) in enumerate(documents):
        used |= axes
        ours_rec = mine["none"][i]
        ref_rec = theirs["none"][i]
        we_refused = ours_rec["status"].startswith("err")
        they_refused = ref_rec["status"].startswith("err")
        if they_refused:
            refusals[ref_rec["status"][4:]] += 1

        # --- intent: our verdict against the generator's own reading
        intent[1] += 1
        if expected is None:
            if we_refused:
                failures.append("doc %d [%s]: we refused what nothing should refuse "
                    "(%s)" % (i, ",".join(sorted(axes)), ours_rec["status"]))
            else:
                intent[0] += 1
        else:
            # The table names a status without its prefix, and the header declares
            # it with one. Looking it up by the short name gave `None` for every
            # entry, which compared unequal to every real code and scored 164 of 500
            # while every one of those documents was in fact right.
            want = codes.get("GTEXT_INI_" + expected)
            got = int(ours_rec["status"].split()[1]) if we_refused else 0
            if got == want:
                intent[0] += 1
            else:
                failures.append("doc %d [%s]: expected %s, got %s"
                    % (i, ",".join(sorted(axes)), expected, ours_rec["status"]))

        # --- the reference's channel cannot carry some documents at all
        if axes & set(ini_cp_gen.NO_ORACLE):
            excluded += 1
            continue

        # --- pin: that `interpolation=None` is still load-bearing
        #
        # **The population is a function of the generator alone** - the axes it put in
        # the document and the verdict its own table expects - and never of what
        # either reader answered. A denominator that depended on our parse would
        # shrink in exactly the case this score exists to catch: a defect that stopped
        # producing the values whose treatment is being asserted would also stop
        # producing the asks.
        pin_axes = axes & set(ini_cp_gen.PIN_INTERPOLATION)
        if pin_axes and expected is None and \
                not axes & set(ini_cp_gen.REFERENCE_DIVERGES):
            for axis in sorted(pin_axes):
                where, shape = ini_cp_gen.PIN_INTERPOLATION[axis]
                pin[1] += 1
                other = theirs[where][i]
                pinned = theirs["none"][i]
                differs = (other["status"] != pinned["status"] or
                           other["body"] != pinned["body"])
                if not differs:
                    failures.append("doc %d [%s]: the reference's %s configuration "
                        "answers this exactly as the pinned one does, so pinning "
                        "interpolation=None is no longer keeping anything out of "
                        "this gate\n      input %r"
                        % (i, ",".join(sorted(axes)), where, data[:200]))
                    continue
                # The shape is asserted only where the axis is **alone**, because two
                # `%` axes in one document make the stronger claim a statement about
                # which of them the reference reached first. `value-percent` refuses
                # the document, and a `value-percent-pair` beside it would then be
                # credited with a refusal that was not its own.
                if len(pin_axes) == 1 and shape != "differ":
                    refused = other["status"].startswith("err")
                    if shape == "refuse" and not refused:
                        failures.append("doc %d [%s]: the reference's %s "
                            "configuration no longer refuses this\n      input %r"
                            % (i, ",".join(sorted(axes)), where, data[:200]))
                        continue
                    if shape == "change" and refused:
                        failures.append("doc %d [%s]: the reference's %s "
                            "configuration refuses this, where the table says it "
                            "changes a value\n      input %r"
                            % (i, ",".join(sorted(axes)), where, data[:200]))
                        continue
                if len(pin_axes) == 1:
                    pinned_axes[axis] += 1
                pin[0] += 1

        # --- the known divergences, asserted rather than scored
        #
        # **Before the verdict comparison and not after it**, because a divergence can
        # show up as either answer and the table cannot say which in advance. A
        # no-break space at a value's edge is a value difference between two documents
        # both sides accept; a no-break space used as *indentation* is a verdict
        # difference, because the reference reads a continuation and this reader reads
        # an entry with no separator and refuses the document. The first version of
        # this loop checked for a divergence only among the documents both sides had
        # accepted, so `nbsp-indent` never reached the check at all: it failed the
        # verdict score as an ordinary disagreement, and the divergence score reported
        # the axis as never observed. Two symptoms, one cause, and the one that named
        # it was the "never observed" assertion - which is exactly what that assertion
        # is for.
        divergent = axes & set(ini_cp_gen.REFERENCE_DIVERGES)
        if divergent:
            divergence[1] += 1
            if we_refused != they_refused or ours_rec["body"] != ref_rec["body"]:
                divergence[0] += 1
                diverged |= divergent
            else:
                failures.append("doc %d [%s]: the reference no longer diverges here, "
                    "so this exclusion is now hiding an agreement rather than a "
                    "difference" % (i, ",".join(sorted(axes))))
            continue

        # --- interp: our interpolation against the reference's, under both styles
        #
        # **After the divergence check and not before it.** The first version of this
        # ran over every non-excluded document, on the reasoning that a document
        # carrying a Unicode divergence still has an interpolation answer. It does,
        # and the answer is contaminated: a no-break space at a value's edge is
        # stripped by the reference under *every* configuration, so that document
        # differs here for a reason that has nothing to do with `%` and this score
        # reported 625 of 756 while gtext_ini_value_interpolate() was correct on all
        # of them. `values` already owns those documents, and so this skips exactly
        # what `values` skips. The comparison is of accept-against-refuse plus the body,
        # the same shape `verdict` and `values` use together - with one addition: when
        # the reference's refusal is an *interpolation* refusal, the code we report is
        # compared too. Those are the refusals this score exists for, so leaving them
        # at "we also refused" would let a wrong reason pass.
        for style in ("basic", "extended"):
            interp[1] += 1
            ours_s = mine[style][i]
            ref_s = theirs[style][i]
            we_r = ours_s["status"].startswith("err")
            they_r = ref_s["status"].startswith("err")
            reason = ref_s["status"][4:] if they_r else None
            want_code = INTERPOLATION_REFUSALS.get(reason)
            if we_r != they_r:
                failures.append("doc %d [%s] %s: we %s, the reference %s"
                    "\n      input %r" % (i, ",".join(sorted(axes)), style,
                        "refused" if we_r else "accepted",
                        "refused (%s)" % reason if they_r else "accepted", data[:200]))
            elif want_code is not None and \
                    int(ours_s["status"].split()[1]) != codes[want_code]:
                failures.append("doc %d [%s] %s: the reference raised %s and we "
                    "reported %s, not %s\n      input %r"
                    % (i, ",".join(sorted(axes)), style, reason,
                       ours_s["status"], want_code, data[:200]))
            elif not we_r and ours_s["body"] != ref_s["body"]:
                first = next((n for n, (a, b) in enumerate(zip(ref_s["body"],
                    ours_s["body"])) if a != b),
                    min(len(ref_s["body"]), len(ours_s["body"])))
                failures.append("doc %d [%s] %s: record %d differs\n      ref %s\n"
                    "      our %s\n      input %r"
                    % (i, ",".join(sorted(axes)), style, first,
                       ref_s["body"][first] if first < len(ref_s["body"])
                           else "<missing>",
                       ours_s["body"][first] if first < len(ours_s["body"])
                           else "<missing>", data[:200]))
            else:
                interp[0] += 1

        # --- defaults: the chain the pinned `default_section` excludes by construction
        #
        # **Scored over the axes the generator built for it, and over nothing else.**
        # The population is a function of the generator - which axis it put in the
        # document - and never of what either reader answered, for the reason the `pin`
        # score gives: a denominator that depended on our parse would shrink in exactly
        # the case this score exists to catch.
        chain_axes = axes & set(ini_cp_gen.DEFAULTS_AXES)
        for axis in sorted(chain_axes):
            style = ini_cp_gen.DEFAULTS_CHAIN.get(axis, ("basic", None))[0]
            label = style + "-defaults"
            ours_d = mine[label][i]
            ref_d = theirs[label][i]
            we_r = ours_d["status"].startswith("err")
            they_r = ref_d["status"].startswith("err")

            # `defaults-pin`: that turning the inheritance on is what made this
            # resolve. Without this the fifth configuration could be duplicating the
            # third and the score would read exactly the same.
            shape = ini_cp_gen.DEFAULTS_CHAIN.get(axis, (None, None))[1]
            if shape == "resolve":
                defaults_pin[1] += 1
                pinned = theirs[style][i]
                reason = pinned["status"][4:] if \
                    pinned["status"].startswith("err") else None
                if reason != "InterpolationMissingOptionError":
                    failures.append("doc %d [%s]: the pinned configuration answers "
                        "this %s, where the whole point of the axis is that it cannot "
                        "see the defaults - so default_section=%s is no longer keeping "
                        "the chain out of this gate\n      input %r"
                        % (i, ",".join(sorted(axes)),
                           "with %s" % reason if reason else "without refusing",
                           DEFAULT_SECTION, data[:200]))
                elif they_r:
                    failures.append("doc %d [%s]: the reference refuses this with the "
                        "inheritance live (%s), so the axis is not exercising the "
                        "chain\n      input %r"
                        % (i, ",".join(sorted(axes)), ref_d["status"][4:], data[:200]))
                else:
                    defaults_pin[0] += 1

            defaults[1] += 1
            if we_r != they_r:
                failures.append("doc %d [%s] %s: we %s, the reference %s"
                    "\n      input %r" % (i, ",".join(sorted(axes)), label,
                        "refused" if we_r else "accepted",
                        "refused (%s)" % ref_d["status"][4:] if they_r
                            else "accepted", data[:200]))
                continue
            if we_r:
                defaults[0] += 1
                defaults_axes[axis] += 1
                continue

            # **Our key set, not theirs**, and the sections compared pairwise. With the
            # inheritance live the reference carries the defaults' keys into every
            # section while this module keeps them where the document put them, so an
            # extra key on their side is the pin's own rationale and not a difference.
            # A key of *ours* they do not have is still a failure, and so is a section.
            our_tree = as_tree(ours_d["body"])
            ref_tree = as_tree(ref_d["body"])
            ref_sections = dict(ref_tree)
            trouble = None
            if [name for name, _ in our_tree] != [name for name, _ in ref_tree]:
                trouble = ("the sections differ: ours %s, theirs %s"
                    % ([n for n, _ in our_tree], [n for n, _ in ref_tree]))
            else:
                for name, entries in our_tree:
                    for key, value in entries.items():
                        if key not in ref_sections[name]:
                            trouble = ("section %s has our key %s and the reference "
                                "does not" % (name, key))
                        elif ref_sections[name][key] != value:
                            trouble = ("section %s key %s: ref %s, ours %s"
                                % (name, key, ref_sections[name][key], value))
                        if trouble:
                            break
                    if trouble:
                        break
            if trouble:
                failures.append("doc %d [%s] %s: %s\n      input %r"
                    % (i, ",".join(sorted(axes)), label, trouble, data[:200]))
            else:
                defaults[0] += 1
                defaults_axes[axis] += 1

        # --- verdict
        verdict[1] += 1
        if we_refused == they_refused:
            verdict[0] += 1
        else:
            failures.append("doc %d [%s]: we %s, the reference %s\n      input %r"
                % (i, ",".join(sorted(axes)),
                   "refused" if we_refused else "accepted",
                   "refused" if they_refused else "accepted", data[:200]))

        if we_refused or they_refused:
            continue

        # --- rewrite, which needs no reference
        rewrite[1] += 1
        if ours_rec["rewrite"] == data.hex() or (not data and
                ours_rec["rewrite"] == "."):
            rewrite[0] += 1
        else:
            failures.append("doc %d [%s]: the rewrite is not the input"
                % (i, ",".join(sorted(axes))))

        # --- values
        values[1] += 1
        if ours_rec["body"] == ref_rec["body"]:
            values[0] += 1
        else:
            first = next((n for n, (a, b) in enumerate(zip(ref_rec["body"],
                ours_rec["body"])) if a != b),
                min(len(ref_rec["body"]), len(ours_rec["body"])))
            body_ref = ref_rec["body"]
            body_our = ours_rec["body"]
            failures.append("doc %d [%s]: record %d differs\n      ref %s\n"
                "      our %s\n      input %r"
                % (i, ",".join(sorted(axes)), first,
                   body_ref[first] if first < len(body_ref) else "<missing>",
                   body_our[first] if first < len(body_our) else "<missing>",
                   data[:200]))

    for line in failures[:15]:
        print("FAIL " + line)
    print("intent      %6d of %6d  (our verdict against the generator's reading)"
          % tuple(intent))
    print("verdict     %6d of %6d  (our verdict against the reference's)"
          % tuple(verdict))
    print("values      %6d of %6d  (sections, keys and joined values)" % tuple(values))
    print("rewrite     %6d of %6d  (byte for byte)" % tuple(rewrite))
    print("divergence  %6d of %6d  (excluded, and the departure still visible)"
          % tuple(divergence))
    print("interp      %6d of %6d  (gtext_ini_value_interpolate() against "
          "Basic and Extended)" % tuple(interp))
    print("pin         %6d of %6d  (interpolation=None still keeps a behaviour "
          "out)" % tuple(pin))
    print("defaults    %6d of %6d  (the [DEFAULT] chain, for every key our tree "
          "holds)" % tuple(defaults))
    print("defaults-pin%6d of %6d  (default_section=<unspellable> still keeps the "
          "chain out)" % tuple(defaults_pin))
    for axis in ini_cp_gen.DEFAULTS_AXES:
        style, shape = ini_cp_gen.DEFAULTS_CHAIN.get(axis, ("basic", "precedence"))
        print("    %-30s %6d  %s, %s" % (axis, defaults_axes[axis], style, shape))
    for axis in ini_cp_gen.PIN_INTERPOLATION:
        where, shape = ini_cp_gen.PIN_INTERPOLATION[axis]
        print("    %-30s %6d  alone, reference %s %ss"
              % (axis, pinned_axes[axis], where, shape))
    print("excluded    %6d          (the reference's channel cannot read them: %s)"
          % (excluded, ", ".join(ini_cp_gen.NO_ORACLE)))
    print("the reference refused %d documents:" % sum(refusals.values()))
    for kind, n in refusals.most_common():
        print("    %-30s %6d" % (kind, n))

    missing_axes = [a for a in ini_cp_gen.AXES if a not in used]
    print("axes        %6d of %6d" % (len(ini_cp_gen.AXES) - len(missing_axes),
                                      len(ini_cp_gen.AXES)))
    missing_div = [a for a in ini_cp_gen.REFERENCE_DIVERGES if a not in diverged]

    # An axis whose shape is `differ` needs no isolated document to be asserted on,
    # but every axis needs at least one document that carried it *alone*, because
    # that is the only document where the claim is about that axis and nothing else.
    missing_pin = [a for a in ini_cp_gen.PIN_INTERPOLATION if not pinned_axes[a]]

    missing_chain = [a for a in ini_cp_gen.DEFAULTS_AXES if not defaults_axes[a]]

    bad = bool(failures) or intent[0] != intent[1] or verdict[0] != verdict[1] \
        or values[0] != values[1] or rewrite[0] != rewrite[1] \
        or divergence[0] != divergence[1] or interp[0] != interp[1] \
        or pin[0] != pin[1] or defaults[0] != defaults[1] \
        or defaults_pin[0] != defaults_pin[1]
    if missing_axes:
        print("FAIL these axes never appeared, so the run says nothing about them: %s"
              % ", ".join(missing_axes))
        bad = True
    if missing_div:
        print("FAIL these divergences were never observed, so the reference may have "
              "been fixed and the exclusion is now dead weight: %s"
              % ", ".join(missing_div))
        bad = True
    if not excluded:
        print("FAIL nothing was excluded, so the exclusion table describes no "
              "document this run generated")
        bad = True
    if missing_pin:
        print("FAIL these axes never appeared alone in a clean document, so nothing "
              "here asserts what the interpolation pin is keeping out for them: %s"
              % ", ".join(missing_pin))
        bad = True
    if not pin[1]:
        print("FAIL nothing asserted the interpolation pin, so interpolation=None is "
              "a printed line again rather than a measurement")
        bad = True
    if missing_chain:
        print("FAIL these defaults-chain axes never appeared, so this run says nothing "
              "about the chain the pinned default_section excludes: %s"
              % ", ".join(missing_chain))
        bad = True
    if not defaults_pin[1]:
        print("FAIL nothing asserted the default_section pin, so the two live runs "
              "may be answering exactly what the pinned ones do")
        bad = True
    print("FAIL" if bad else "PASS: every document agrees under all five "
          "configurations, or diverges for a reason this run proved is still there")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
