#!/usr/bin/env python3
"""Plant a defect in a scratch copy of the library and show a fuzzer finds it.

    tools/xml/plant-fuzz.py [--time SECONDS] [name ...]

A fuzzer that has never failed has not been shown to be able to. A plant whose defect has a recorded reproducer (REPRODUCERS) is found by running the harness
over that file with -runs=0, which is the same every time; only the others are searched for.
There are ten plants
(seven for the fuzzers, three for the differential) and two controls. Each plant here is
one edit to one line of the library, applied to a **copy** of this repository in a
scratch directory (the working tree is never touched), and the harness it is meant
for is run on the copy for the harness time budget (60 seconds by default, the same
`FUZZ_TIME` the Makefile's fuzz-run-* targets use). It must stop with a report. The
**control** is the same run with no edit, and it must not.

  event-column     the event parser forgets to restore the column when it retries a
                   construct that straddles two pieces of input: a tree built one byte
                   at a time has different locations from the whole-document parse.
                   fuzz_xml.
  limit-after-run  the fix fuzz_xml found is taken out again: an attribute value's limit is
                   checked only when a run of ordinary bytes ends. The recorded reproducer
                   in the seed corpus fails at once. fuzz_xml.
  leak-attr-pool   a parse that has attributes does not release their pool. fuzz_xml
                   (and any ASan run, which is the point: the harness's fault allocator
                   and LeakSanitizer both see it).
  utf16-pair       a UTF-16 surrogate pair decodes to the code point one above the
                   right one: what the writer writes in UTF-16 is read back as a
                   different tree. fuzz_xml's round trip.
  escape-amp       the writer stops escaping `&`. fuzz_xml_writer.
  oracle-attr-prefix  the parser stops refusing an attribute whose prefix is not bound.
                   The reference differential (tools/oracle/run-xml-oracle.sh), which has
                   to fail with exit status 3: libxml2 and expat refuse those documents.
  oracle-hunt-utf16  the parser refuses a valid UTF-16 document with a supplementary character,
                   which both references accept. The differential's *hunt* (another
                   population, XML_ORACLE_COUNT/XML_ORACLE_SEED) must not file that as a
                   reference's quirk because a UTF-8 control agrees: it has to exit 3 with
                   "no class explains".
  oracle-attr-space  an attribute value's literal line feed is kept instead of becoming
                   a space. The differential's event lists differ.
  strict-comment   the writer refuses a comment that has a hyphen anywhere, which XML
                   allows: a writer too strict. fuzz_xml_writer's strict mode, which is
                   the only thing that can find this - a refusal is always a permitted
                   answer to the other properties.

  xhtml-strict-ignored  the writer's xhtml_strict stops refusing hazards: a tree with a
                   hazard is written as if it had none. fuzz_xml_writer's xhtml
                   properties (strict agrees with the hazard count).

The scratch copy carries `src/`, `include/`, `tests/`, `tools/` and the Makefile, and is
built there; it has none of this tree's build, and only the tracked seeds of the libFuzzer
corpus. The pinned suite under `third_party/` is linked, not copied, for the oracle plants.
`control-oracle` is the oracle run on an unedited copy, and must pass.

Copyright 2026 by Corey Pennycuff
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORKSPACE = os.path.dirname(os.path.dirname(ROOT))

PLANTS = {
    "event-column": ("xml", "src/xml/xml_stream.c",
                     "    p->line = line;\n    p->column = column;\n    memset(&p->err, 0, sizeof(p->err));",
                     "    p->line = line;\n    memset(&p->err, 0, sizeof(p->err));"),
    "limit-after-run": ("xml", "src/xml/xml_parser.c",
                        "      if (p->attr_pool.length - start + (i - p->pos) > p->max_attribute_bytes) {\n        break;\n      }\n",
                        ""),
    "leak-attr-pool": ("xml", "src/xml/xml_parser.c",
                       "  markup_buf_free(&p->attr_pool);\n", ""),
    "utf16-pair": ("xml", "src/xml/xml_encoding.c",
                   "const uint32_t cp = 0x10000 + ((c->high - 0xD800) << 10) + (u - 0xDC00);",
                   "const uint32_t cp = 0x10001 + ((c->high - 0xD800) << 10) + (u - 0xDC00);"),
    "escape-amp": ("xml-writer", "src/markup/markup_escape.c",
                   "    case '&':\n      return \"&amp;\";", "    case '&':\n      return NULL;"),
    "strict-comment": ("xml-writer", "src/xml/xml_writer.c",
                       "if (has_seq(data, len, \"--\", 2) || (len > 0 && data[len - 1] == '-')) {",
                       "if (has_seq(data, len, \"-\", 1)) {"),
    "xhtml-strict-ignored": ("xml-writer", "src/xml/xml_writer.c",
                             "  if (w->o.xhtml_strict) {\n    return refuse_x(w, n, why);\n  }\n  return GTEXT_XML_OK;",
                             "  return GTEXT_XML_OK;"),
    "oracle-attr-prefix": ("oracle", "src/xml/xml_parser.c",
                           "    if (!xml_scope_lookup(p, (const char *)a->name_ptr, a->colon, &a->ns)) {\n      xml_scope_release(p, mark);\n      return xml_fail_at(p, GTEXT_XML_E_NAMESPACE,\n          \"the attribute's prefix is not bound to a namespace\", &a->where);\n    }",
                           "    if (!xml_scope_lookup(p, (const char *)a->name_ptr, a->colon, &a->ns)) {\n      a->ns = \"urn:unbound\";\n    }"),
    "oracle-hunt-utf16": ("oracle", "src/xml/xml_encoding.c",
                          "        if (u < 0xDC00 || u > 0xDFFF) {\n          return conv_invalid(c, \"a high surrogate not followed by a low one\");",
                          "        if (1) {\n          return conv_invalid(c, \"a high surrogate not followed by a low one\");"),
    "oracle-attr-space": ("oracle", "src/xml/xml_parser.c",
                          "      status = xml_append(p, &p->attr_pool, \" \", 1);\n      xml_consume(p,\n          b == '\\r'",
                          "      status = xml_append(p, &p->attr_pool, b == '\\n' ? \"\\n\" : \" \", 1);\n      xml_consume(p,\n          b == '\\r'"),
}


def copy_tree(dest):
    skip = shutil.ignore_patterns("build", "third_party", "crash-*", "leak-*", "timeout-*", "slow-unit-*",
                                  "__pycache__", "docs", ".git")
    corpus = os.path.join(ROOT, "tests", "fuzz", "corpus")

    def ignore(directory, names):
        out = set(skip(directory, names))
        # The corpus: tracked seeds only, so the copy starts where a clean checkout does
        # (and does not race a fuzzer that is writing into the same directory).
        if os.path.dirname(directory) == corpus:
            out |= {n for n in names if not n.endswith(".seed")}
        return out

    shutil.copytree(ROOT, dest, ignore=ignore, symlinks=True, ignore_dangling_symlinks=True)


# A plant whose defect has a recorded reproducer is found by replaying that file, which is the
# same every time, and not by a time-boxed search, which is not. The others keep the search.
REPRODUCERS = {
    "limit-after-run": ("fuzz_xml", "tests/fuzz/corpus/xml/reproducer-attribute-limit-before-a-bad-byte.seed"),
}


def make_variables(prefix, names):
    """The Makefile's own values for `names`, so that nothing here is a second copy of them."""
    rules = "print-%: ; @echo $($*)\n"
    out = {}
    for name in names:
        done = subprocess.run(["make", "-s", "-C", ROOT, "-f", "Makefile", "-f", "-", "PREFIX=" + prefix,
                               "print-" + name], input=rules, capture_output=True, text=True)
        out[name] = done.stdout.strip()
        if done.returncode != 0 or not out[name]:
            raise SystemExit("could not read %s from the Makefile: %s" % (name, done.stderr.strip()[-300:]))
    return out


TIMEOUT = 3  # a multiple of the fuzz budget; a build and a run each fit well inside it
TIMED_OUT = -999


def run(name, seconds):
    """(exit status, output, target) of one plant or control; the status is TIMED_OUT if it hung."""
    if name == "control":
        target, path, old, new = "xml", None, None, None
    elif name in ("control-oracle", "control-hunt"):
        target, path, old, new = "oracle", None, None, None
    else:
        target, path, old, new = PLANTS[name]
    scratch = tempfile.mkdtemp(prefix="plant-fuzz-", dir=os.environ.get("TMPDIR", tempfile.gettempdir()))
    budget = seconds * TIMEOUT + 900
    try:
        copy_tree(os.path.join(scratch, "text"))
        text = os.path.join(scratch, "text")
        if path:
            with open(os.path.join(text, path), encoding="utf-8") as fh:
                source = fh.read()
            if source.count(old) != 1:
                raise SystemExit("plant %s: its anchor matches %d times in %s" % (name, source.count(old), path))
            with open(os.path.join(text, path), "w", encoding="utf-8") as fh:
                fh.write(source.replace(old, new))
        prefix = os.environ.get("PREFIX") or os.path.join(WORKSPACE, ".local")
        env = dict(os.environ)
        env["PKG_CONFIG_PATH"] = os.path.join(prefix, "share", "pkgconfig")
        try:
            if target == "oracle":
                # The reference differential on a library built from the edited copy. The
                # pinned suite is data, not source, and is shared rather than copied.
                variables = make_variables(prefix, ("DEP_PCS", "APP_DIR", "STATIC_TARGET"))
                os.symlink(os.path.join(ROOT, "third_party"), os.path.join(text, "third_party"))
                built = subprocess.run(["make", "-C", text, "-j4", "PREFIX=" + prefix], env=env,
                                       capture_output=True, text=True, timeout=budget)
                if built.returncode != 0:
                    return built.returncode, built.stdout + built.stderr, target
                env["PREFIX"] = prefix
                env.setdefault("DEP_PCS", variables["DEP_PCS"])
                env.setdefault("ARCHIVE", os.path.join(text, variables["APP_DIR"], variables["STATIC_TARGET"]))
                env["GHOTI_ORACLE_REQUIRED"] = "1"
                if name in ("oracle-hunt-utf16", "control-hunt"):
                    env["XML_ORACLE_COUNT"], env["XML_ORACLE_SEED"] = "20000", "7"
                done = subprocess.run([os.path.join(text, "tools", "oracle", "run-xml-oracle.sh")], env=env,
                                      capture_output=True, text=True, timeout=budget)
                return done.returncode, done.stdout + done.stderr, target
            cap = os.path.join(WORKSPACE, "tools", "memcap")
            if name in REPRODUCERS:
                harness, repro = REPRODUCERS[name]
                capped = [cap, "-m", os.environ.get("GHOTI_MEMCAP", "6G"), "--"] if os.path.exists(cap) else []
                built = subprocess.run(capped + ["make", "-C", text, "-j4", "fuzz-" + target, "PREFIX=" + prefix],
                                       env=env, capture_output=True, text=True, timeout=budget)
                if built.returncode != 0:
                    return built.returncode, built.stdout + built.stderr, target
                app = os.path.join(text, make_variables(prefix, ("FUZZ_APP_DIR",))["FUZZ_APP_DIR"], harness)
                done = subprocess.run(capped + [app, "-runs=0", os.path.join(text, repro)], env=env,
                                      capture_output=True, text=True, timeout=budget)
                return done.returncode, done.stdout + done.stderr, target
            cmd = ([cap, "-m", os.environ.get("GHOTI_MEMCAP", "6G"), "--"] if os.path.exists(cap) else []) + [
                "make", "-C", text, "-j4", "fuzz-run-" + target, "FUZZ_TIME=%d" % seconds, "FUZZ_ARGS=-seed=1",
                "PREFIX=" + prefix]
            done = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=budget)
            return done.returncode, done.stdout + done.stderr, target
        except subprocess.TimeoutExpired as hung:
            return TIMED_OUT, "timed out after %ds\n%s" % (budget, (hung.stdout or b"")[-500:]), target
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# What a fuzzer reporting a defect looks like: a sanitizer or libFuzzer banner, or one of the
# harness's own "property failed" lines. The build's command lines mention the harness names,
# and a compiler error, a kill by memcap (exit 137) or a timeout all exit non-zero too, so none
# of those counts: the run must have started (libFuzzer printed its seed) and then reported.
REPORT = re.compile(r"^(fuzz_xml(_writer)?: .+|==\d+==ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer).*|"
                    r".*runtime error: .*|SUMMARY: (AddressSanitizer|libFuzzer|UndefinedBehaviorSanitizer).*)$", re.M)


def found_by_fuzzer(status, out):
    if status in (0, TIMED_OUT, 137, -9) or "INFO: Seed:" not in out:
        return False
    return REPORT.search(out) is not None


def main(argv):
    seconds = 60
    names = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--time":
            seconds = int(args.pop(0))
        else:
            names.append(a)
    names = names or ["control", "control-oracle"] + list(PLANTS)
    bad = []
    for name in names:
        if not name.startswith("control") and name not in PLANTS:
            raise SystemExit("unknown plant %r (one of control, control-oracle, %s)" % (name, ", ".join(PLANTS)))
        status, out, target = run(name, seconds)
        if name == "oracle-hunt-utf16":
            # A hunt is never silent on an unedited tree: it leaves a few mutants with two causes
            # unexplained. So the plant is found if the hunt on the edited copy fails with
            # something the hunt on the unedited copy does not: a document this parser refuses as
            # an invalid sequence that both references accept.
            _status, base, _target = run("control-hunt", seconds)
            before = set(re.findall(r"^  (gen:\w+) ", base, re.M))
            new = [m for m in re.finditer(r"^  (gen:\w+) .*\n    we refuse \(INVALID_SEQUENCE\), xml2 and expat accept",
                                          out, re.M) if m.group(1) not in before]
            reported = status == 3 and "no class explains" in out and bool(new)
        elif target == "oracle":
            reported = status == 3 and "no entry in tools/oracle/xml_disagreements.txt" in out
        else:
            reported = found_by_fuzzer(status, out)
        if name.startswith("control"):
            ok = status == 0
            print("%s (no edit): %s" % (name, "silent, as it must be" if ok else "FAILED (exit %d)" % status))
            if not ok:
                sys.stderr.write(out[-3000:])
        else:
            ok = reported
            line = next((l for l in out.splitlines() if REPORT.match(l) or l.strip().startswith("we accept")), "")
            print("%-18s (%s): %s %s" % (name, target, "found" if ok else "NOT FOUND in %ds" % seconds, line[:140]))
            if not ok:
                sys.stderr.write("exit %s; the end of the output:\n%s\n" % (status, out[-1500:]))
        if not ok:
            bad.append(name)
    if bad:
        sys.stderr.write("planted-defect check failed for: %s\n" % ", ".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
