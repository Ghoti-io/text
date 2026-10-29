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
"""The condition and rule differentials: this library against Chromium (Blink) and Firefox (Gecko, whose style engine is Stylo).

    css_cond_diff.py --kind cond|rule --work DIR [--seed N] [--count M] [--stage populate|browsers|ours|classify|all] [--ours BINARY] [--replay DIR] [--list FILE]

kind `cond`: media query lists (`matchMedia(text)`: whether it matches, and `.media`), supports conditions (`CSS.supports(text)`), declarations (`CSS.supports(property, value)`)
asked in several viewports and device pixel ratios (css_cond_env.tsv).
kind `rule`: at-rules inserted with `CSSStyleSheet.insertRule()` (accepted or thrown on; the rule that stands: its prelude, the descriptors it kept by name, its margin
rules) and the conditionText of `@supports` rules.

The three readers answer the same questions (the driver is containers/cssprop/cond_driver.mjs, WebDriver BiDi, the pinned image, no network). The browsers are put in each
document's viewport and ratio; every other feature of the environment is read from the page, once, and this library is given the same values, so a feature a browser cannot be
moved off is compared at the one value it has.

**What is a divergence.** A question the three do not answer alike (`matchMedia()` is two questions: whether it matches, and its `.media`, compared up to white space, the case of
an identifier or a unit and the spelling of a number; a rule is one: accepted or not, its prelude and the descriptors it kept by name). By who differs it is `ours` (the browsers
agree and this library does not), `browsers` (they disagree with each other; the library is with one of them) or `both` (the library is with neither of two browsers that
disagree). **None may stay unexplained.** Each divergence is abstracted to a feature (css_cond_features.py: the leaf of the question, a media feature, a declaration, a supports
function, a descriptor of a rule, that is in the divergent questions and in no agreeing one), and the list (css_cond_divergences.txt, css_rule_divergences.txt) gives each
feature its class, its count in the default population (seed 1, 200 documents) and the reason, a sentence of the served text or the browsers' known deviation: `standard-newer`
(the standard says what this library does and the browsers have not caught up), `reference-old` (the browsers keep what the standard changed), `reference-wrong` (neither the
standard nor the other browser agrees with one), `reference-narrow` (the browsers do not implement what the standard defines), `legacy-compat` (a behavior kept for the web) or
`env-fixed` (a feature the browsers cannot be moved off, or a metric they have fonts for). A defect of this library is fixed, never listed. The gate fails (exit 3) on a divergence
whose feature is not listed, a listed feature that does not occur, a count that moved, and a reason that is short or blanket.

--replay DIR reads the browsers' answers from an earlier run's directory instead of asking the browsers, and this library is asked again, so a library edited in a copy can be
measured against browsers that did not change.
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import css_cond_features  # noqa: E402
import css_cond_gen  # noqa: E402
import css_rule_gen  # noqa: E402
import oracle_env  # noqa: E402

GATE_EXIT = 3
DRIVER = os.path.join(HERE, "containers", "cssprop", "cond_driver.mjs")
ENGINES = ["chromium", "firefox"]
CLASSES = ("standard-newer", "reference-old", "reference-wrong", "reference-narrow", "legacy-compat", "env-fixed")
KINDS = {
    "cond": dict(gen=css_cond_gen, list=os.path.join(HERE, "css_cond_divergences.txt"), features=css_cond_features.COND),
    "rule": dict(gen=css_rule_gen, list=os.path.join(HERE, "css_rule_divergences.txt"), features=css_cond_features.RULE),
}


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def unesc(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            i += 1
            out.append({"n": "\n", "t": "\t"}.get(s[i], s[i]))
        else:
            out.append(s[i])
        i += 1
    return "".join(out)


def write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False)


def read_json(path):
    return json.load(open(path, encoding="utf-8"))


def populate(kind, work, seed, count):
    gen = KINDS[kind]["gen"]
    path = os.path.join(work, "population.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for i in range(count):
            f.write(json.dumps(gen.Gen(seed, i).document(), ensure_ascii=False) + "\n")
    return path


def load_population(work):
    return [json.loads(line) for line in open(os.path.join(work, "population.jsonl"), encoding="utf-8")]


def driver_command(engine):
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s' % (DRIVER, engine)]
    return oracle_env.command("cssprop", inner)


def run_engine(engine, docs):
    data = "".join(json.dumps({"w": d["w"], "h": d["h"], "dpr": d["dpr"], "q": d["q"]}, ensure_ascii=False) + "\n" for d in docs)
    run = subprocess.run(driver_command(engine), input=data.encode("utf-8", "surrogatepass"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_cond_diff: the %s driver exited %d:\n%s" % (engine, run.returncode, oracle_env.reference_stderr(run.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(docs):
        raise SystemExit("css_cond_diff: the %s driver answered %d documents to %d" % (engine, len(rows), len(docs)))
    return header, rows


def env_lines(env):
    """The `E` lines that give this library the environment the page measured."""
    out = []
    for k in ("pointer", "hover", "prefers-color-scheme", "prefers-reduced-motion", "prefers-contrast", "color-gamut", "dynamic-range", "update", "overflow-block", "overflow-inline",
              "display-mode", "forced-colors", "inverted-colors", "scripting", "prefers-reduced-transparency", "prefers-reduced-data"):
        v = env.get(k)
        if v:
            out.append(("E", k, v[0]))
    ap = {"none": 1, "coarse": 2, "fine": 4}
    ah = {"none": 1, "hover": 2}
    out.append(("E", "any-pointer", str(sum(ap[x] for x in env.get("any-pointer", [])))))
    out.append(("E", "any-hover", str(sum(ah[x] for x in env.get("any-hover", [])))))
    if env.get("color"):
        out.append(("E", "color", str(env["color"][0])))
    out.append(("E", "transform-3d", str(env.get("transform-3d", 0))))
    out.append(("E", "device-width", str(env["device-width"])))
    out.append(("E", "device-height", str(env["device-height"])))
    # a feature the page's browser lacks (it answers neither the feature nor its negation) is not evaluated here either: unknown, as it is there
    out.append(("E", "unsupported", ",".join(sorted(env.get("unsupported", [])))))
    return out


def run_ours(binary, docs, env):
    data = ["\t".join(e) + "\n" for e in env_lines(env)]
    for d in docs:
        data.append("D\t%s\t%s\t%s\n" % (d["w"], d["h"], d["dpr"]))
        for q in d["q"]:
            data.append("%s\t%s\n" % (q[0], "\t".join(esc(x) for x in q[1:])))
        data.append("X\n")
    run = subprocess.run([binary], input="".join(data).encode("utf-8", "surrogateescape"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_cond_diff: %s exited %d: %s" % (binary, run.returncode, run.stderr.decode("utf-8", "replace")[:600]))
    out, cur = [], []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if line == "X":
            out.append(cur)
            cur = []
        elif line:
            cur.append(line.split("\t"))
    if len(out) != len(docs):
        raise SystemExit("css_cond_diff: %s answered %d documents to %d" % (binary, len(out), len(docs)))
    return out


# ---- reading an answer as a comparable value ----------------------------------------------------------------------------------

def ws(s):
    return re.sub(r"\s+", " ", s).strip()


def split_top(body):
    """The pieces of a block's contents at its top-level semicolons and closing braces (strings, parentheses and braces nest)."""
    out, depth, cur, quote, i = [], 0, [], None, 0
    while i < len(body):
        c = body[i]
        if quote:
            cur.append(c)
            if c == "\\" and i + 1 < len(body):
                cur.append(body[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "({[":
            depth += 1
            cur.append(c)
        elif c in ")}]":
            depth -= 1
            cur.append(c)
            if c == "}" and depth == 0:
                out.append("".join(cur))
                cur = []
        elif c == ";" and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur))
    return [p.strip() for p in out if p.strip()]


def signature(css):
    """What a rule stands for, reduced to what both sides write alike: its prelude (white space collapsed, case kept) and, of its block, the descriptors it kept by name and the nested
    rules the same way."""
    css = css.strip()
    brace = css.find("{")
    if brace < 0:
        return ws(css.rstrip(";"))
    head = ws(css[:brace])
    head = re.sub(r'"([A-Za-z_][A-Za-z0-9_-]*)"', r"\1", head)  # a name written as a string and as the identifier it spells is one name
    end = css.rfind("}")
    body = css[brace + 1:end if end > brace else len(css)]
    parts = []
    for piece in split_top(body):
        if piece.startswith("@") or ("{" in piece and ":" not in piece.split("{", 1)[0]):
            parts.append(signature(piece))
        else:
            parts.append(ws(piece.split(":", 1)[0]).lower())
    return head + " {" + ",".join(sorted(parts)) + "}"


NUMBER = re.compile(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?(?![A-Za-z%])|[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")


def norm_media(s):
    """`.media` up to what the standards leave to the engine: white space, the case of an identifier or a unit, and the spelling of a number (the browsers write six significant
    digits, an exponent for a large one, a sign only when it is minus, and clamp a unitless integer to 32 bits)."""
    def number(m):
        t = m.group(0)
        f = float(t)
        unitless = m.end() >= len(s) or not (s[m.end()].isalpha() or s[m.end()] == "%")
        if unitless and abs(f) > 2147483647 and "." not in t and "e" not in t.lower():
            return "2147483647" if f > 0 else "-2147483648"
        return "%.6g" % f
    return re.sub(r"\s+", "", NUMBER.sub(number, s.lower()))


def comparable(q, ans):
    """The values a reader's answer to a question is compared as: [(part, value)]."""
    k = q[0]
    if ans and isinstance(ans[0], str) and ans[0].startswith("T:"):
        return [("", "!" + ans[0])]
    if k == "M":
        return [("m", ans[0]), ("s", norm_media(ans[1]))]
    if k in ("S", "P"):
        return [("", ans[0])]
    if k == "R":
        return [("", "reject" if ans[0] == "reject" else "ok " + norm_media(ans[1] if len(ans) > 1 else ""))]
    if k == "A":
        return [("", "reject" if ans[0] == "reject" else "ok " + signature(ans[1] if len(ans) > 1 else ""))]
    return [("", "?")]


def ours_value(q, ans):
    k = q[0]
    if k == "M":
        if ans[1] == "E":
            return [("m", "E"), ("s", "E")]
        return [("m", ans[1]), ("s", norm_media(unesc(ans[2])))]
    if k in ("S", "P"):
        return [("", ans[1])]
    if k == "R":
        return [("", "reject" if ans[1] == "reject" else "ok " + norm_media(conditionText(unesc(ans[2]))))]
    if k == "A":
        return [("", "reject" if ans[1] == "reject" else "ok " + signature(unesc(ans[2])))]
    return [("", "?")]


def conditionText(rule_css):
    m = re.match(r"\s*@supports\s*(.*?)\s*\{", rule_css, re.S)
    return m.group(1) if m else rule_css


def key_of(i, j, q):
    return "%d:%d:%s" % (i, j, q[0])


def compare(docs, ours, browsers):
    """The divergences: [(key, class, {reader: value})], and the number of questions (a `matchMedia()` is two: whether it matches, and its `.media`)."""
    rows = []
    total = 0
    for i, d in enumerate(docs):
        for j, q in enumerate(d["q"]):
            cvs = comparable(q, browsers["chromium"][i][j])
            fvs = comparable(q, browsers["firefox"][i][j])
            ocs = ours_value(q, ours["chromium"][i][j])
            ofs = ours_value(q, ours["firefox"][i][j])
            for (part, cv), (_, fv), (_, oc), (_, of) in zip(cvs, fvs, ocs, ofs):
                total += 1
                a, b = oc == cv, of == fv
                if a and b:
                    continue  # this library answers each browser as that browser is (their environments differ, or they agree)
                if cv == fv:
                    cls = "ours"
                else:
                    cls = "both" if not a and not b else "browsers"
                rows.append((key_of(i, j, q) + part, cls, {"query": q, "part": part, "ours": [oc, of], "chromium": cv, "firefox": fv}))
    return rows, total


def gate_fail(message):
    print(message)
    sys.exit(GATE_EXIT)


def read_list(path):
    """{feature: (class, count, reason)}: tab separated, feature, class, count, reason."""
    listed = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) != 4 or not f[2].isdigit() or len(f[3]) < 20:
                gate_fail("LIST: a line of %s is not feature, class, count and a reason (at least twenty characters): %r" % (path, line))
            if f[1] not in CLASSES:
                gate_fail("LIST: %s is of the class %r, which is not one of %s (a class `ours` is never listed)" % (f[0], f[1], ", ".join(CLASSES)))
            listed[f[0]] = (f[1], int(f[2]), f[3])
    return listed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kind", required=True, choices=sorted(KINDS))
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("CSS_COND_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("CSS_COND_ORACLE_COUNT", "200")))
    ap.add_argument("--stage", default="all")
    ap.add_argument("--ours", default=None)
    ap.add_argument("--replay", default=None)
    ap.add_argument("--write-list", default=None)
    ap.add_argument("--write-skeleton", default=None, help="the features not on the list, as lines to fill in")
    ap.add_argument("--list", default=None, help="another list of the divergences (the controls make one wrong on purpose)")
    a = ap.parse_args()
    cfg = KINDS[a.kind]
    os.makedirs(a.work, exist_ok=True)
    stages = ["populate", "browsers", "ours", "classify"] if a.stage == "all" else [a.stage]
    if "populate" in stages:
        populate(a.kind, a.work, a.seed, a.count)
    docs = load_population(a.work)
    headers = {}
    if "browsers" in stages:
        for engine in ENGINES:
            if a.replay:
                header, rows = read_json(os.path.join(a.replay, engine + ".json"))
            else:
                header, rows = run_engine(engine, docs)
            write_json(os.path.join(a.work, engine + ".json"), [header, rows])
    src = a.replay or a.work
    if "ours" in stages:
        if not a.ours:
            raise SystemExit("css_cond_diff: --ours BINARY is needed")
        ours = {}
        for engine in ENGINES:
            header = read_json(os.path.join(src, engine + ".json"))[0]
            ours[engine] = run_ours(a.ours, docs, header["env"])
        write_json(os.path.join(a.work, "ours.json"), ours)
    if "classify" in stages:
        ours = read_json(os.path.join(a.work, "ours.json"))
        browsers = {}
        for engine in ENGINES:
            headers[engine], browsers[engine] = read_json(os.path.join(src, engine + ".json"))
        rows, total = compare(docs, ours, browsers)
        counts = collections.Counter(r[1] for r in rows)
        print("css %s differential: %d documents, %d questions; %s" % (a.kind, len(docs), total, ", ".join("%s %d" % kv for kv in sorted(counts.items())) or "no divergence"))
        for engine in ENGINES:
            print("  %s %s" % (engine, headers[engine].get("browser")))
        if a.write_list:
            with open(a.write_list, "w", encoding="utf-8") as f:
                for key, cls, vals in rows:
                    f.write("%s\t%s\t%s\n" % (key, cls, json.dumps(vals, ensure_ascii=False)))
        listed = read_list(a.list or cfg["list"])
        # the stats a feature is chosen by: for each leaf family, in how many divergent questions it is and in how many questions at all
        stats = {}
        divergent = {r[0] for r in rows}
        for i, d in enumerate(docs):
            for j, q in enumerate(d["q"]):
                parts = ("m", "s") if q[0] == "M" else ("",)
                for part in parts:
                    kind = q[0] + part
                    isdiv = key_of(i, j, q) + part in divergent
                    for fam in set(css_cond_features.leaf_families(q)):
                        e = stats.setdefault((kind, fam), [0, 0])
                        e[1] += 1
                        e[0] += isdiv
        seen = collections.Counter()
        unlisted = collections.OrderedDict()
        for key, cls, vals in rows:
            feat = css_cond_features.feature_of(cfg["features"], vals["query"], vals, stats)
            seen[feat] += 1
            if feat not in listed:
                unlisted.setdefault(feat, []).append((cls, vals))
        bad = sum(len(v) for v in unlisted.values())
        for feat, items in list(unlisted.items())[:80]:
            cls, vals = items[0]
            print("UNLISTED %s x%d [%s] %s" % (feat, len(items), cls, json.dumps(vals, ensure_ascii=False)[:600]))
        if a.write_skeleton:
            with open(a.write_skeleton, "w", encoding="utf-8") as f:
                for feat, items in unlisted.items():
                    f.write("%s\tTODO\t%d\t%s\n" % (feat, len(items), json.dumps(items[0][1], ensure_ascii=False)))
        problems = []
        for feat, (cls, count, _reason) in listed.items():
            if a.seed == 1 and a.count == 200 and count > 0:
                if seen[feat] == 0:
                    problems.append("GONE %s does not occur (the list says %d)" % (feat, count))
                elif seen[feat] != count:
                    problems.append("COUNT %s occurs %d times, the list says %d" % (feat, seen[feat], count))
        for p in problems[:40]:
            print(p)
        if bad or problems:
            print("css %s differential: %d unlisted, %d list problems" % (a.kind, bad, len(problems)))
            return GATE_EXIT
        print("css %s differential: agrees, or every divergence is listed" % a.kind)
    return 0


if __name__ == "__main__":
    sys.exit(main())
