#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Plant a defect in a scratch copy of the library and show the markup tree's fuzzer finds it.

    PREFIX=... tools/markup/plant-fuzz.py [--time SECONDS] [name ...]

A fuzzer that has never failed has not been shown to be able to. Each plant is one edit to one place in src/markup/, applied to a **copy** of this
repository in a scratch directory (the working tree is never touched), and tests/fuzz/fuzz_markup_tree.cpp is built there and run over the plant's
recorded seed of tests/fuzz/corpus/markup-tree with -runs=0, which is the same every time (the seeds are programs written by
tools/markup/make-fuzz-seeds.py, and `make test` replays all of them: tests/test-markup-tree-fuzz-seeds.cpp). A plant whose seed no longer shows it
falls back to the search, for the time budget (60 seconds by default, FUZZ_TIME) from the seeds, and says so. The fuzzer must stop with a report of its
own (a property of the harness, a sanitizer, libFuzzer). The **control** is the same build with no edit, over every seed, and it must not.

  remove-forgets-the-node         a removed node is not put on the detached list: the nodes the document owns do not add up (the invariants).
  replace-forgets-the-old-child   the same for the child a replace took out.
  node-count-not-released         a freed node is still counted (the invariants).
  stranger-reference-accepted     an insert before a node that is not a child of the parent is not refused: the list is corrupted (the invariants).
  set-data-length-stale           new data is stored and its length is not (the invariants, or the snapshot after a refusal).
  free-leaks-the-template-content a template's content is not freed with it (the allocator: a leak).
  clone-failure-keeps-the-copy    a clone that fails while it copies an attribute leaves the half-built copy on the detached list (a refusal that
                                  changed the tree).

The scratch copy carries `src/`, `include/`, `tests/`, `tools/` and the Makefile, and is built there; it has none of this tree's build, and only the
tracked seeds of the libFuzzer corpus.

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


TARGET = "markup-tree"
SEEDS = "tests/fuzz/corpus/markup-tree/"

PLANTS = {
    "remove-forgets-the-node": ("src/markup/markup_tree.c",
                                "  unlink_child(node);\n  detached_push(node->doc, node);\n  markup_validation_invalidate(node->doc);\n  return GTEXT_MARKUP_OK;\n}\n\nGTEXT_Markup_Status gtext_markup_replace",
                                "  unlink_child(node);\n  markup_validation_invalidate(node->doc);\n  return GTEXT_MARKUP_OK;\n}\n\nGTEXT_Markup_Status gtext_markup_replace"),
    "replace-forgets-the-old-child": ("src/markup/markup_tree.c", "  unlink_child(old_child);\n  detached_push(old_child->doc, old_child);\n",
                                      "  unlink_child(old_child);\n"),
    "node-count-not-released": ("src/markup/markup_tree.c", "  doc->node_count--;\n", "\n"),
    "stranger-reference-accepted": ("src/markup/markup_tree.c",
                                    "      && (reference->parent != parent || (reference->flags & MARKUP_F_CONTENT))) {",
                                    "      && (0 && (reference->parent != parent || (reference->flags & MARKUP_F_CONTENT)))) {"),
    "set-data-length-stale": ("src/markup/markup_tree.c", "  *slot = copy;\n  *len_slot = length;\n  node->cap_log2 = 0;", "  *slot = copy;\n  node->cap_log2 = 0;"),
    "free-leaks-the-template-content": ("src/markup/markup_tree.c",
                                        "    if (cur->flags & MARKUP_F_CONTENT) {\n      ((markup_element *)parent)->content = NULL;\n    }\n    else {\n      parent->first_child = cur->next;\n    }\n    node_release(doc, cur);",
                                        "    if (cur->flags & MARKUP_F_CONTENT) {\n      ((markup_element *)parent)->content = NULL;\n      cur = parent;\n      continue;\n    }\n    else {\n      parent->first_child = cur->next;\n    }\n    node_release(doc, cur);"),
    "clone-failure-keeps-the-copy": ("src/markup/markup_walk.c",
                                     "            opt_text(a->prefix), a->local->text, a->value, a->value_length);\n        if (status != GTEXT_MARKUP_OK) {\n          gtext_markup_node_free(copy);\n          return status;",
                                     "            opt_text(a->prefix), a->local->text, a->value, a->value_length);\n        if (status != GTEXT_MARKUP_OK) {\n          return status;"),
}
for _name, (_f, _old, _new) in list(PLANTS.items()):
    PLANTS[_name] = (_f, _old.replace("\\n", "\n"), _new.replace("\\n", "\n"))

# The seed of tests/fuzz/corpus/markup-tree that shows each plant: the harness aborts on it with the plant and is silent without.
REPRODUCERS = {
    "remove-forgets-the-node": SEEDS + "edit-remove-first.seed",
    "replace-forgets-the-old-child": SEEDS + "edit-replace-child.seed",
    "node-count-not-released": SEEDS + "edit-free-detached.seed",
    "stranger-reference-accepted": SEEDS + "refuse-insert-before-a-stranger.seed",
    "set-data-length-stale": SEEDS + "edit-data-text.seed",
    "free-leaks-the-template-content": SEEDS + "edit-template-ensure.seed",
    "clone-failure-keeps-the-copy": SEEDS + "alloc-clone-attributes-3.seed",
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


def target_of(name):
    return TARGET


def run(name, seconds):
    """(exit status, output) of one plant or of the control; the status is TIMED_OUT if it hung."""
    target = target_of(name)
    scratch = tempfile.mkdtemp(prefix="plant-fuzz-", dir=os.environ.get("TMPDIR", tempfile.gettempdir()))
    budget = seconds * TIMEOUT + 900
    try:
        copy_tree(os.path.join(scratch, "text"))
        text = os.path.join(scratch, "text")
        if not name.startswith("control"):
            path, old, new = PLANTS[name]
            with open(os.path.join(text, path), encoding="utf-8") as fh:
                source = fh.read()
            if source.count(old) != 1:
                raise SystemExit("plant %s: its anchor matches %d times in %s" % (name, source.count(old), path))
            with open(os.path.join(text, path), "w", encoding="utf-8") as fh:
                fh.write(source.replace(old, new))
        prefix = os.environ.get("PREFIX") or os.path.join(WORKSPACE, ".local")
        env = dict(os.environ)
        env["PKG_CONFIG_PATH"] = os.path.join(prefix, "share", "pkgconfig")
        cap = os.environ.get("MEMCAP_TOOL", "")  # a wrapper run as MEMCAP_TOOL -m SIZE -- CMD...; unset, uncapped
        capped = [cap, "-m", os.environ.get("GHOTI_MEMCAP", "6G"), "--"] if os.path.exists(cap) else []
        try:
            built = subprocess.run(capped + ["make", "-C", text, "-j4", "fuzz-" + target, "PREFIX=" + prefix],
                                   env=env, capture_output=True, text=True, timeout=budget)
            if built.returncode != 0:
                return built.returncode, built.stdout + built.stderr
            app = os.path.join(text, make_variables(prefix, ("FUZZ_APP_DIR",))["FUZZ_APP_DIR"], "fuzz_" + target.replace("-", "_"))
            if name.startswith("control"):
                # Every tracked seed, replayed: the unedited library must be silent over all of them.
                done = subprocess.run(capped + [app, "-runs=0", os.path.join(text, SEEDS)], env=env, capture_output=True, text=True, timeout=budget)
                return done.returncode, done.stdout + done.stderr
            done = subprocess.run(capped + [app, "-runs=0", os.path.join(text, REPRODUCERS[name])], env=env, capture_output=True, text=True, timeout=budget)
            out = done.stdout + done.stderr
            if found_by_fuzzer(done.returncode, out):
                return done.returncode, out
            # The recorded seed no longer shows the defect: search for it, and say so.
            sys.stderr.write("plant %s: its seed %s no longer shows the defect; searching\n" % (name, REPRODUCERS[name]))
            cmd = capped + ["make", "-C", text, "-j4", "fuzz-run-" + target, "FUZZ_TIME=%d" % seconds, "FUZZ_ARGS=-seed=1 -max_len=2048", "PREFIX=" + prefix]
            done = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=budget)
            return done.returncode, done.stdout + done.stderr
        except subprocess.TimeoutExpired as hung:
            return TIMED_OUT, "timed out after %ds\n%s" % (budget, (hung.stdout or b"")[-500:])
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# What a fuzzer reporting a defect looks like: a sanitizer or libFuzzer banner, or one of the
# harness's own "property failed" lines. The build's command lines mention the harness names,
# and a compiler error, a kill by the memory cap (exit 137) or a timeout all exit non-zero too, so none
# of those counts: the run must have started (libFuzzer printed its seed) and then reported.
REPORT = re.compile(r"^(fuzz_markup_tree: .+|==\d+==ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer).*|"
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
    names = names or ["control"] + list(PLANTS)
    bad = []
    for name in names:
        if not name.startswith("control") and name not in PLANTS:
            raise SystemExit("unknown plant %r (one of control, %s)" % (name, ", ".join(PLANTS)))
        status, out = run(name, seconds)
        reported = found_by_fuzzer(status, out)
        if name.startswith("control"):
            ok = status == 0
            print("%s (no edit): %s" % (name, "silent, as it must be" if ok else "FAILED (exit %d)" % status))
            if not ok:
                sys.stderr.write(out[-3000:])
        else:
            ok = reported
            line = next((l for l in out.splitlines() if REPORT.match(l)), "")
            print("%-20s %s %s" % (name, "found" if ok else "NOT FOUND in %ds" % seconds, line[:140]))
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
