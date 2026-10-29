#!/usr/bin/env python3
"""The style sheet and selector differential: this library against Chromium (Blink) and Firefox (Gecko, whose style engine is Stylo, built on Servo's `selectors` crate).

    css_sheet_diff.py --work DIR [--seed N] [--count M] [--stage populate|ours|browsers|classify|all] [--ours BINARY] [--replay DIR] [--write-list FILE]

A population (css_sheet_gen.py) is HTML documents with `<style>` elements, nested rules, layers, `@scope`, conditional groups and shadow trees. For each document three
readers answer two kinds of question:

  value     the computed `color`, `background-color`, `margin-top`, `margin-left` and `padding-left` of each element (a shadow tree's too): this library through
            gtext_css_style_compute() with the sheets and the rule index (css-sheet-ours, written as the CSSOM resolves it), the browsers through
            `getComputedStyle(element).getPropertyValue()` (containers/cssprop/sheet_driver.mjs, WebDriver BiDi, the pinned image, no network);
  selector  the elements of the document `querySelectorAll(selector)` returns, in document order, or a syntax error.

**What is a divergence.** A question the three do not answer alike. It is classified by who differs: `ours` (the browsers agree and this library does not: always a
defect of the library, and the gate's ceiling for it is zero), `browsers` (the browsers disagree with each other; the library is with one of them or neither) and
`both` (the library is with neither of two browsers that disagree). tools/oracle/css_sheet_divergences.txt lists each divergence that is not `ours`, by key, with
the reason (the quoted sentence of the served draft that decides it, or the browser's known deviation); the gate fails (exit 3) on a divergence that is not listed, a
listed key that does not occur, and any `ours`.

--replay DIR reads the browsers' answers from an earlier run's directory instead of asking the browsers, and this library is asked again, so a library edited in a
copy can be measured against browsers that did not change.
"""

import argparse
import collections
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import css_sheet_gen  # noqa: E402
import oracle_env  # noqa: E402

LIST = os.path.join(HERE, "css_sheet_divergences.txt")
GATE_EXIT = 3
DRIVER = os.path.join(HERE, "containers", "cssprop", "sheet_driver.mjs")
ENGINES = ["chromium", "firefox"]


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


def populate(work, seed, count):
    path = os.path.join(work, "population.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for i in range(count):
            f.write(json.dumps(css_sheet_gen.Gen(seed, i).document(), ensure_ascii=False) + "\n")
    return path


def load_population(work):
    return [json.loads(line) for line in open(os.path.join(work, "population.jsonl"), encoding="utf-8")]


def run_ours(binary, docs):
    data = []
    for d in docs:
        data.append("D\t%s\n" % esc(d["text"]))
        for host, html in d["shadows"]:
            data.append("S\t%s\t%s\n" % (esc(host), esc(html)))
        for s in d["selectors"]:
            data.append("Z\t%s\n" % esc(s))
        for i, props in d["queries"]:
            data.append("Q\t%s\t%s\n" % (esc(i), ",".join(props)))
        data.append("X\n")
    run = subprocess.run([binary], input="".join(data).encode("utf-8", "surrogateescape"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_sheet_diff: %s exited %d: %s" % (binary, run.returncode, run.stderr.decode("utf-8", "replace")[:600]))
    out, cur = [], {"values": [], "selectors": []}
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if line == "X":
            out.append(cur)
            cur = {"values": [], "selectors": []}
        elif line:
            f = line.split("\t")
            if f[1] == "Z":
                cur["selectors"].append([unesc(f[0]), f[2], unesc(f[3]) if len(f) > 3 else ""])
            else:
                cur["values"].append([unesc(f[0]), f[1], (unesc(f[3]) if len(f) > 3 else None) if f[2] == "OK" else "!" + f[2]])
    if len(out) != len(docs):
        raise SystemExit("css_sheet_diff: %s answered %d documents to %d" % (binary, len(out), len(docs)))
    return out


def run_servo(servo, ours, docs):
    """The Servo `selectors` crate's answers to the selectors of each document, over the tree this library's HTML parser made (css-sheet-ours Y):
    a list (per document) of [selector, "OK"|"SYNTAX"|"NONTS", ids]. NONTS: the selector holds a pseudo-class the crate leaves to its user, which the driver does not model."""
    data = "".join("D\t%s\nY\n" % esc(d["text"]) for d in docs)
    run = subprocess.run([ours], input=data.encode("utf-8", "surrogateescape"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_sheet_diff: %s exited %d: %s" % (ours, run.returncode, run.stderr.decode("utf-8", "replace")[:600]))
    trees, cur = [], None
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if line.startswith("Q\t"):  # each document starts with its quirks flag
            cur = [line]
            trees.append(cur)
        elif line.startswith("T\t"):
            cur.append(line)
    if len(trees) != len(docs):
        raise SystemExit("css_sheet_diff: %d trees for %d documents" % (len(trees), len(docs)))
    out = []
    for d, tree in zip(docs, trees):
        data = "\n".join(tree) + "\n" + "".join("Z\t%s\n" % esc(sel) for sel in d["selectors"]) + "X\n"
        r = subprocess.run([servo], input=data.encode("utf-8", "surrogateescape"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if r.returncode != 0:
            raise SystemExit("css_sheet_diff: %s exited %d: %s" % (servo, r.returncode, r.stderr.decode("utf-8", "replace")[:600]))
        rows = []
        for line in r.stdout.decode("utf-8", "surrogateescape").split("\n"):
            if line and line != "X":
                f = line.split("\t")
                rows.append([unesc(f[0]), f[2], unesc(f[3]) if len(f) > 3 and f[2] == "OK" else ""])
        if len(rows) != len(d["selectors"]):
            raise SystemExit("css_sheet_diff: the servo driver answered %d selectors to %d" % (len(rows), len(d["selectors"])))
        out.append(rows)
    return out


def driver_command(engine):
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s' % (DRIVER, engine)]
    return oracle_env.command("cssprop", inner)


def run_engine(engine, docs):
    data = "".join(json.dumps({"text": d["text"], "shadows": d["shadows"], "queries": d["queries"], "selectors": d["selectors"]}, ensure_ascii=False) + "\n" for d in docs)
    run = subprocess.run(driver_command(engine), input=data.encode("utf-8", "surrogatepass"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_sheet_diff: the %s driver exited %d:\n%s" % (engine, run.returncode, oracle_env.reference_stderr(run.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(docs):
        raise SystemExit("css_sheet_diff: the %s driver answered %d documents to %d" % (engine, len(rows), len(docs)))
    return header, rows


def write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False)


def read_json(path):
    return json.load(open(path, encoding="utf-8"))


# ---- comparing -----------------------------------------------------------------------------------------------------------

def norm_ours(v):
    return v


def items(doc_index, answer):
    """{key: value} for one reader's answer to one document."""
    out = {}
    for i, p, v in answer["values"]:
        out["v:%d:%s:%s" % (doc_index, i, p)] = v
    for sel, kind, ids in answer["selectors"]:
        out["z:%d:%s" % (doc_index, sel)] = kind if kind != "OK" else "OK " + ids
    return out


def compare(docs, ours, browsers):
    """The divergences: [(key, class, {reader: value})], and the counts."""
    rows = []
    total = 0
    skipped = 0
    for k in range(len(docs)):
        o = items(k, ours[k])
        c = items(k, browsers["chromium"][k])
        f = items(k, browsers["firefox"][k])
        for key, ov in o.items():
            total += 1
            cv, fv = c.get(key), f.get(key)
            if ov.startswith("!UNSUPPORTED") or ov.startswith("!ERR"):
                skipped += 1
                rows.append((key, "unsupported", {"ours": ov, "chromium": cv, "firefox": fv}))
                continue
            if ov == cv == fv:
                continue
            if cv == fv:
                cls = "ours"
            elif ov == cv or ov == fv:
                cls = "browsers"
            else:
                cls = "both"
            rows.append((key, cls, {"ours": ov, "chromium": cv, "firefox": fv}))
    return rows, total


def gate_fail(message):
    """A list that is wrong is the gate failing, not the tool: the message on standard output and the gate's exit status."""
    print(message)
    sys.exit(GATE_EXIT)


def read_list(path=None):
    """{feature: (class, count, reason)}: tools/oracle/css_sheet_divergences.txt, tab separated: feature, class, count, reason."""
    listed = {}
    path = path or LIST
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f0 = line.rstrip("\n").split("\t")
            if len(f0) == 4 and f0[1] not in ("browsers", "both", "servo"):
                gate_fail("LIST: %s is of the class %r, which is not one a divergence has once it is listed (a class `ours` is never listed)" % (f0[0], f0[1]))
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) != 4 or not f[2].isdigit() or len(f[3]) < 20:
                gate_fail("LIST: a line of %s is not feature, class, count and a reason (at least twenty characters): %r" % (path, line))
            listed[f[0]] = (f[1], int(f[2]), f[3])
    return listed


FEATURES = [
    # (substring of the selector or of the document's style, feature): the list abstracts a divergence to what it is about
    (':host-context(', 'host-context'),
    ('&::part(', 'part-nested-under-a-selector-that-is-not-the-host'),
    (':host::part(', 'part-rule-with-a-nested-ampersand'),
    ('lang("', 'lang-quoted-string'),
]


def feature_of(key, docs):
    """The feature a divergence is listed under: the smallest thing in the document that explains it, or the key itself."""
    kind, k, rest = key.split(":", 2)
    doc = docs[int(k)]
    if kind in ("z", "s"):
        for needle, name in FEATURES:
            if needle in rest:
                return kind + ":" + name
        return key
    style = doc["text"][: doc["text"].find("</style>")] if "</style>" in doc["text"] else doc["text"]
    style += "\n".join(html for _, html in doc.get("shadows", []))  # the sheets of the shadow trees are style too
    for needle, name in FEATURES:
        if needle in style:
            return "v:" + name
    return key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("CSS_SHEET_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("CSS_SHEET_ORACLE_COUNT", "200")))
    ap.add_argument("--stage", default="all")
    ap.add_argument("--ours", default=None)
    ap.add_argument("--servo", default=None, help="the built selectors-driver (containers/css/selectors-driver): the servo stage and its part of the classification")
    ap.add_argument("--replay", default=None)
    ap.add_argument("--write-list", default=None)
    ap.add_argument("--list", default=None, help="another list of the divergences (the controls make one wrong on purpose)")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    stages = ["populate", "ours", "browsers"] + (["servo"] if a.servo else []) + ["classify"] if a.stage == "all" else [a.stage]
    if "populate" in stages:
        populate(a.work, a.seed, a.count)
    docs = load_population(a.work)
    if "ours" in stages:
        if not a.ours:
            raise SystemExit("css_sheet_diff: --ours BINARY is needed")
        write_json(os.path.join(a.work, "ours.json"), run_ours(a.ours, docs))
    if "browsers" in stages:
        for engine in ENGINES:
            if a.replay:
                header, rows = read_json(os.path.join(a.replay, engine + ".json"))
            else:
                header, rows = run_engine(engine, docs)
            write_json(os.path.join(a.work, engine + ".json"), [header, rows])
    if "servo" in stages:
        if not (a.servo and a.ours):
            raise SystemExit("css_sheet_diff: --servo BINARY and --ours BINARY are needed")
        write_json(os.path.join(a.work, "servo.json"), run_servo(a.servo, a.ours, docs))
    if "classify" in stages:
        src = a.replay or a.work
        ours = read_json(os.path.join(a.work, "ours.json"))
        browsers = {}
        headers = {}
        for engine in ENGINES:
            headers[engine], browsers[engine] = read_json(os.path.join(src, engine + ".json"))
        rows, total = compare(docs, ours, browsers)
        servo_path = os.path.join(a.work, "servo.json")
        if os.path.exists(servo_path):
            servo = read_json(servo_path)
            for k in range(len(docs)):
                for (sel, kind, ids), (osel, okind, oids) in zip(servo[k], ours[k]["selectors"]):
                    if kind == "NONTS":
                        continue
                    total += 1
                    ov = okind if okind != "OK" else "OK " + oids
                    sv = kind if kind != "OK" else "OK " + ids
                    if ov != sv:
                        rows.append(("s:%d:%s" % (k, sel), "servo", {"ours": ov, "servo": sv}))
        counts = collections.Counter(r[1] for r in rows)
        print("css sheet differential: %d documents, %d questions; %s" % (len(docs), total, ", ".join("%s %d" % kv for kv in sorted(counts.items())) or "no divergence"))
        for engine in ENGINES:
            print("  %s %s" % (engine, headers[engine].get("browser")))
        if a.write_list:
            with open(a.write_list, "w", encoding="utf-8") as f:
                for key, cls, vals in rows:
                    f.write("%s\t%s\t%s\n" % (key, cls, json.dumps(vals, ensure_ascii=False)))
        listed = read_list(a.list)
        seen = collections.Counter()
        bad = 0
        shown = 0
        for key, cls, vals in rows:
            if cls == "unsupported":
                continue
            feat = feature_of(key, docs)
            if cls == "ours":
                bad += 1
                if shown < 40:
                    shown += 1
                    print("OURS %s %s" % (key, json.dumps(vals, ensure_ascii=False)))
                continue
            seen[feat] += 1
            if feat not in listed or listed[feat][0] != cls:
                bad += 1
                if shown < 40:
                    shown += 1
                    print("UNLISTED %s %s %s %s" % (cls, feat, key, json.dumps(vals, ensure_ascii=False)))
        problems = []
        for feat, (cls, count, _reason) in listed.items():
            if a.seed == 1 and a.count == 200 and count > 0:  # a count of 0 is a feature that only a hunt has met: the default population does not have it
                if seen[feat] == 0:
                    problems.append("GONE %s does not occur (the list says %d)" % (feat, count))
                elif seen[feat] != count:
                    problems.append("COUNT %s occurs %d times, the list says %d" % (feat, seen[feat], count))
        for p in problems:
            print(p)
        if bad or problems:
            print("css sheet differential: %d unlisted or ours, %d list problems" % (bad, len(problems)))
            return GATE_EXIT
        print("css sheet differential: agrees, or every divergence is listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
