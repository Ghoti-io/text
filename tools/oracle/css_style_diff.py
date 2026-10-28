#!/usr/bin/env python3
"""The computed-style differential: this library against Chromium (Blink) and Firefox (Gecko) on `getComputedStyle()`.

    css_style_diff.py --work DIR [--seed N] [--count M] [--stage populate|metrics|ours|browsers|classify|all] [--schemes light,dark]
                      [--ours BINARY] [--replay DIR] [--plant NAME] [--write-list FILE]

A population (css_style_gen.py) is documents: SVG fixtures, a few thousand values of properties, presentation attributes on SVG elements and generated trees of
HTML and SVG elements with `style` attributes and presentation attributes (no style sheet: that waits for selectors), every tree inside an element with
`display: none` so that the CSSOM resolved value of a property whose value depends on layout is its computed value. For each (element, property) three
readers answer: this library (css-style-ours: `gtext_css_style_compute()`, the value written as the CSSOM resolves it), and the two browsers through the
WebDriver BiDi driver of the pinned image (containers/cssprop/style_driver.mjs: `getComputedStyle(element).getPropertyValue()`), in a 1280 by 800 viewport at
a device pixel ratio of 1, in both color schemes. The font metrics `ex`, `ch`, `cap` and `ic` are measured in Chromium (style_driver.mjs metrics) and fed to this
library's callback.

**What is a divergence.** An item is a divergence when the three do not agree on the value (`ser:`), or on whether the property has one (`acc:`: an engine
that does not have a property computes nothing for it, which is the `support:` of the property, decided by the probes). The key of a divergence is
`<pattern>:<property>:<feature>` as in css_prop_diff.py, the feature being the smallest declaration of the same pattern and property that has it, with numbers,
dimensions and strings abstracted. tools/oracle/css_style_divergences.txt classifies each key (class, reason, the quoted sentence of the served text); the gate
fails (exit 3) on a key that is not listed, a listed key that does not occur, a count that is not the listed one, a class `ours` beyond the ceiling (never in a
finished list), a witness that does not hold and a control that does not come out as it was. The classes are the property differential's:
`standard-newer`, `reference-old`, `reference-wrong`, `legacy-compat`, `unspecified`, `ours`.

**What is not compared.** An element whose computation this library could not make (`UNSUPPORTED`: a property the engine does not compute, a font the metrics
do not have) is counted and not compared; an element the browser cannot reach (the clone of an SVG `<use>`) is not compared either.

--replay DIR reads the browsers' answers from an earlier run's directory instead of asking the browsers, and this library is asked again, so a library edited in a
copy (tools/oracle/CSS_STYLE_PLANTS) is measured against browsers that did not change. --plant NAME corrupts one thing in the data on the way so that the gate can
be shown to fail.
"""

import argparse
import collections
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import css_prop_diff as base  # noqa: E402
import css_prop_gen  # noqa: E402
import css_style_gen  # noqa: E402
import oracle_env  # noqa: E402

ENGINES = base.ENGINES
LIST = os.path.join(HERE, "css_style_divergences.txt")
WITNESSES = os.path.join(HERE, "css_style_witnesses.txt")
GATE_EXIT = 3
STYLE_DRIVER = os.path.join(HERE, "containers", "cssprop", "style_driver.mjs")


# ---- the readers ---------------------------------------------------------------------------------------------------------

def run_ours(binary, docs, metrics, scheme):
    """Ours for every document: a list (per document) of lines (id, property, kind, value)."""
    data = []
    for d in docs:
        data.append("D\t%s\t%s\n" % (d["mode"], css_prop_gen.esc(d["text"])))
        for i, props in d["queries"]:
            data.append("Q\t%s\t%s\n" % (css_prop_gen.esc(i), css_prop_gen.esc(",".join(props))))
        data.append("X\n")
    argv = [binary] + (["--metrics", metrics] if metrics and os.path.exists(metrics) else []) + (["--dark"] if scheme == "dark" else [])
    run = subprocess.run(argv, input="".join(data).encode("utf-8", "surrogateescape"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_style_diff: %s exited %d: %s" % (binary, run.returncode, run.stderr.decode("utf-8", "replace")[:400]))
    out, cur = [], []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if line == "X":
            out.append(cur)
            cur = []
        elif line:
            f = line.split("\t")
            cur.append((css_prop_gen.unesc(f[0]), css_prop_gen.unesc(f[1]), f[2], css_prop_gen.unesc(f[3]) if len(f) > 3 else ""))
    if len(out) != len(docs):
        raise SystemExit("css_style_diff: %s answered %d documents to %d" % (binary, len(out), len(docs)))
    return out


def style_driver_command(engine, mode):
    inner = ["sh", "-c", 'cd /opt/cssprop && exec node "%s" %s %s' % (STYLE_DRIVER, engine, mode)]
    return oracle_env.command("cssprop", inner)


def run_engine(engine, docs, scheme):
    data = "".join(json.dumps({"mode": d["mode"], "text": d["text"], "queries": d["queries"]}, ensure_ascii=False) + "\n" for d in docs).encode("utf-8", "surrogatepass")
    run = subprocess.run(style_driver_command(engine, scheme), input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_style_diff: the %s driver exited %d:\n%s" % (engine, run.returncode, oracle_env.reference_stderr(run.stderr.decode("utf-8", "replace"))[-1500:]))
    lines = run.stdout.decode("utf-8", "replace").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = json.loads(lines[0][1:])
    rows = [json.loads(x) for x in lines[1:]]
    if len(rows) != len(docs):
        raise SystemExit("css_style_diff: the %s driver answered %d documents to %d" % (engine, len(rows), len(docs)))
    return header, rows


def run_metrics(engine, path):
    run = subprocess.run(style_driver_command(engine, "metrics"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if run.returncode != 0:
        raise SystemExit("css_style_diff: the %s metrics run exited %d:\n%s" % (engine, run.returncode, run.stderr.decode("utf-8", "replace")[-1500:]))
    lines = run.stdout.decode("utf-8").split("\n")
    with open(path, "w", encoding="utf-8") as f:
        f.write("# family weight italic size ex ch cap ic: measured in %s by style_driver.mjs\n" % engine)
        for line in lines[1:]:
            if line:
                f.write(line + "\n")
    return json.loads(lines[0][1:])


# ---- the items -------------------------------------------------------------------------------------------------------------

def flatten(docs, schemes):
    """The items of the population, in the order the readers answer: (document index, scheme, id, property, description, origin)."""
    out = []
    for s in schemes:
        for k, d in enumerate(docs):
            for i, p, desc, origin in d["items"]:
                out.append((k, s, i, p, desc, origin))
    return out


def view(rows_by_scheme, docs, schemes, kind):
    """For an engine: the five-column rows aligned with the flattened items; None for an element the browser could not reach."""
    out = []
    for s in schemes:
        for k, d in enumerate(docs):
            answers = rows_by_scheme[s][k]
            n = sum(len(props) for _i, props in d["queries"])
            if len(answers) != n:
                raise SystemExit("css_style_diff: document %s has %d answers for %d questions" % (d["name"], len(answers), n))
            for a in answers:
                value = a[2]
                if value is None:
                    out.append(None)
                elif value.startswith("!"):
                    out.append([0, "", "", 0, value[1:]])
                else:
                    ok = 1 if value != "" else 0
                    out.append([ok, value, "", ok, ""])
    return out


def ours_view(lines_by_scheme, docs, schemes):
    """Ours aligned with the items: ('OK', value) | ('NO',) | ('UNDECIDED',) | None for an element that is not there."""
    out = []
    for s in schemes:
        for k, d in enumerate(docs):
            lines = lines_by_scheme[s][k]
            n = sum(len(props) for _i, props in d["queries"])
            if len(lines) != n:
                raise SystemExit("css_style_diff: ours answered %d lines for %d questions of %s" % (len(lines), n, d["name"]))
            for _id, _prop, kind, value in lines:
                if kind == "OK":
                    out.append(("OK", value))
                elif kind == "NOVALUE":
                    out.append(("NO",))
                elif kind == "NOELEMENT":
                    out.append(None)
                else:
                    out.append(("UNDECIDED", kind, value))
    return out


# ---- the gate ---------------------------------------------------------------------------------------------------------------

def read_witnesses(path):
    """key -> [(document, id, property, scheme, ours expectation, reference expectation)]."""
    out = collections.defaultdict(list)
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 7:
                out[f[0]].append((f[1], css_prop_gen.unesc(f[2]), f[3], f[4], css_prop_gen.unesc(f[5]), css_prop_gen.unesc(f[6])))
    return out


def describe(p, desc, scheme):
    return desc + (" [dark]" if scheme == "dark" else "")


def gate(docs, schemes, flat, ours, answers, headers, args):
    bad = base.header_problems(headers)
    keep = [i for i in range(len(flat)) if ours[i] is not None and answers["chromium"][i] is not None and answers["firefox"][i] is not None]
    items = [(flat[i][3], describe(flat[i][3], flat[i][4], flat[i][1]), flat[i][5]) for i in keep]
    o = [ours[i] for i in keep]
    a = {e: [answers[e][i] for i in keep] for e in ENGINES}
    where = {(flat[i][0], flat[i][1], flat[i][2], flat[i][3]): n for n, i in enumerate(keep)}
    names = {d["name"]: k for k, d in enumerate(docs)}

    # the controls
    for doc, i, prop, want in css_style_gen.CONTROL_ANSWERS:
        for s in schemes:
            n = where.get((names[doc], s, i, prop))
            if n is None:
                bad.append("CONTROL: %s %s %s is not in the answers" % (doc, i, prop))
                continue
            got = (o[n][1] if o[n][0] == "OK" else o[n][0], a["chromium"][n][1], a["firefox"][n][1])
            if got != (want, want, want):
                bad.append("CONTROL: %s %s %s should be %r in all three, ours %r, chromium %r, firefox %r" % (doc, i, prop, want, *got))

    undecided = collections.Counter()
    for n, x in enumerate(o):
        if x[0] == "UNDECIDED":
            undecided[(items[n][0], x[1], x[2] if x[1] == "UNSUPPORTED" else "")] += 1
    features, _undecided, unanswered = base.classify(items, o, a)
    listed, problems = base.read_list(args.list)
    bad += ["LIST: " + p for p in problems]
    for key in sorted(set(features) - set(listed)):
        f = features[key]
        p, v, _ = items[f["rep"]]
        bad.append("UNLISTED: %s  count %d  e.g. %s: %s  ours %s | chromium %s | firefox %s" % (
            key, f["count"], p, json.dumps(v), o[f["rep"]], base.engine_view(a["chromium"][f["rep"]]), base.engine_view(a["firefox"][f["rep"]])))
    for key in sorted(set(listed) - set(features)):
        bad.append("GONE: %s is listed and does not occur" % key)
    for key in sorted(set(listed) & set(features)):
        if int(listed[key][0]) != features[key]["count"]:
            bad.append("COUNT: %s occurs %d times and the list says %s" % (key, features[key]["count"], listed[key][0]))
        if listed[key][3] != features[key]["digest"]:
            bad.append("DIGEST: %s: the %d items of the key are not the ones the list pinned (it says %s, they hash to %s): an answer of this library or "
                       "of an engine changed for one of them; look at each and write the list again" % (key, features[key]["count"], listed[key][3], features[key]["digest"]))
    ours_keys = [k for k in listed if listed[k][1] == "ours" and k in features]
    if len(ours_keys) > args.max_ours:
        bad.append("OURS: %d keys are class ours (defects of this library) and the ceiling is %d; class ours is never allowed in a finished list, "
                   "and the ceiling only goes down (first: %s)" % (len(ours_keys), args.max_ours, ours_keys[0]))
    if sum(undecided.values()) > args.max_unsupported:
        bad.append("UNSUPPORTED: %d answers of this library are UNSUPPORTED and the ceiling is %d (first: %s)" % (sum(undecided.values()), args.max_unsupported, sorted(undecided)[0]))

    wit = read_witnesses(args.witnesses)
    for key in sorted(wit):
        if key not in listed:
            bad.append("WITNESS: %s is a witness of a key that is not listed" % key)
    for key in sorted(set(listed) - set(wit)):
        bad.append("WITNESS: %s has no witness (css_style_witnesses.txt)" % key)
    for key, ws in sorted(wit.items()):
        for doc, i, prop, scheme, ours_want, ref_want in ws:
            n = where.get((names.get(doc, -1), scheme, i, prop))
            if n is None:
                bad.append("WITNESS: %s: %s %s %s (%s) is not in the answers" % (key, doc, i, prop, scheme))
                continue
            got_o = ("OK=" + o[n][1]) if o[n][0] == "OK" else o[n][0]
            if got_o != ours_want:
                bad.append("WITNESS: %s: ours says %r for %s %s %s, the witness says %r" % (key, got_o, doc, i, prop, ours_want))
            refs = [("OK=" + base.engine_view(a[e][n])[1]) if base.engine_view(a[e][n])[0] else "NO" for e in ENGINES]
            if ref_want not in refs and ref_want != "-":
                bad.append("WITNESS: %s: the engines say %r for %s %s %s, the witness says one of them says %r" % (key, refs, doc, i, prop, ref_want))
    return bad, features, listed, undecided, unanswered, items, o, a, keep, flat


def summarize(items, o, a, headers, features, listed, undecided, unanswered, total, schemes):
    acc = collections.Counter()
    for n in range(len(items)):
        acc[(o[n][0] == "OK", base.engine_view(a["chromium"][n])[0], base.engine_view(a["firefox"][n])[0])] += 1
    print("css style differential: %d items compared of %d asked, schemes %s; %s" % (
        len(items), total, "+".join(schemes), "; ".join("%s %s" % (e, headers[e].get("browser", "?")) for e in ENGINES)))
    print("  value present (ours, chromium, firefox): " + ", ".join("%s%s%s %d" % (("1" if x else "0"), ("1" if y else "0"), ("1" if z else "0"), c)
                                                                   for (x, y, z), c in sorted(acc.items())))
    print("  divergence keys: %d, items explained: %d (%d listed)" % (len(features), sum(f["count"] for f in features.values()), len(set(features) & set(listed))))
    print("  answers of this library that are UNSUPPORTED (not compared): %d (%d distinct)" % (sum(undecided.values()), len(undecided)))


def read_rules(path):
    """[(regex, class, reason)] of tools/oracle/css_style_classes.tsv: how a key that is not in the list yet is classified when the list is written."""
    import re
    out = []
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) == 3:
                out.append((re.compile(f[0]), f[1], f[2]))
    return out


def write_list(path, items, o, a, features, listed, headers, witnesses_path, flat, keep, docs, rules=()):
    wit = []
    with open(path, "w", encoding="utf-8") as f:
        for key in sorted(features):
            if key in listed:
                cls, reason = listed[key][1:3]
            elif key.startswith("support:"):
                cls, reason = support_class(key, headers)
            else:
                cls, reason = next(((c, r) for rx, c, r in rules if rx.fullmatch(key)), ("unclassified", "TODO"))
            f.write("%s\t%d\t%s\t%s\t%s\n" % (key, features[key]["count"], cls, features[key]["digest"], reason))
            rep = features[key]["rep"]
            k, s, i, p, _desc, _origin = flat[keep[rep]]
            ours = ("OK=" + o[rep][1]) if o[rep][0] == "OK" else o[rep][0]
            refs = [("OK=" + base.engine_view(a[e][rep])[1]) if base.engine_view(a[e][rep])[0] else "NO" for e in ENGINES]
            ref = next((r for r in refs if r != ours), refs[0])
            wit.append("%s\t%s\t%s\t%s\t%s\t%s\t%s" % (key, docs[k]["name"], css_prop_gen.esc(i), p, s, css_prop_gen.esc(ours), css_prop_gen.esc(ref)))
    if witnesses_path:
        with open(witnesses_path, "w", encoding="utf-8") as f:
            f.write("# tools/oracle/css_style_diff.py: one witness for every key of css_style_divergences.txt: the smallest item of the key, what this library says of it\n"
                    "# (OK=<value>, NO) and what an engine says that differs from that. Columns: key, document, element, property, scheme, ours, reference.\n"
                    "# The run asks every witness again and fails when either answer is not the one written here.\n")
            f.write("\n".join(wit) + "\n")


def support_class(key, headers):
    which, prop = key.split(":")[1:3]
    lacking = which.split("+")
    names = " and ".join(lacking)
    versions = ", ".join("%s %s" % (e, headers[e].get("browser", "?")) for e in lacking)
    cls = "standard-newer" if len(lacking) == 2 else "reference-old"
    verb = "compute nothing for" if len(lacking) == 2 else "computes nothing for"
    who = "neither engine has it" if len(lacking) == 2 else "that engine does not have it"
    why = ("the pin defines %s (tools/css/WEBREF_COMMIT) and %s %s `initial` on a plain element, which every engine that has a property does: %s at %s"
           % (prop, names, verb, who, versions))
    return cls, why


def apply_plant(name, items, o, a, headers, schemes):
    """Corrupt the data so that a gate that works fails."""
    if name == "ours-writes-differently":
        for i, x in enumerate(o):
            if x[0] == "OK" and items[i][2] != "probe" and x[1] == base.engine_view(a["chromium"][i])[1] == base.engine_view(a["firefox"][i])[1]:
                o[i] = ("OK", x[1] + " ")
                return
    elif name == "ours-has-no-value":
        for i, x in enumerate(o):
            if x[0] == "OK" and items[i][2] != "probe" and base.engine_view(a["chromium"][i])[0] and base.engine_view(a["firefox"][i])[0]:
                o[i] = ("NO",)
                return
    elif name == "engine-writes-differently":
        for i, x in enumerate(o):
            if x[0] == "OK" and items[i][2] != "probe" and x[1] == base.engine_view(a["chromium"][i])[1] == base.engine_view(a["firefox"][i])[1]:
                a["chromium"][i] = [1, x[1] + "0", "", 1, ""]
                return
    elif name == "header-names-another-browser":
        headers["firefox"]["browser"] = "157.0"
        return
    elif name == "header-names-another-hash":
        headers["chromium"]["image"] = headers["chromium"]["image"].replace("geckodriver=", "geckodriver=0", 1)
        return
    elif name == "header-is-from-another-driver":
        import re
        headers["firefox"]["image"] = re.sub(r"driver [0-9a-f]{64}", "driver " + "0" * 64, headers["firefox"]["image"])
        return
    elif name == "header-is-missing":
        del headers["chromium"]["image"]
        return
    elif name == "control-is-misclassified":
        for i, it in enumerate(items):
            if it[2] == "control" and a["firefox"][i][0]:
                a["firefox"][i] = [1, "0", "", 1, ""]
                return
    raise SystemExit("css_style_diff: plant %r changed nothing (the population has no item for it)" % name)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("CSS_STYLE_ORACLE_SEED", "1")))
    ap.add_argument("--count", type=int, default=int(os.environ.get("CSS_STYLE_ORACLE_COUNT", "200")))
    ap.add_argument("--scope", default=os.environ.get("CSS_STYLE_ORACLE_SCOPE", "core"), choices=("core", "full"))
    ap.add_argument("--schemes", default=os.environ.get("CSS_STYLE_ORACLE_SCHEMES", "light,dark"))
    ap.add_argument("--stage", default="all")
    ap.add_argument("--ours", default=os.path.join(ROOT, "build", "css-style-ours"))
    ap.add_argument("--wpt", default=os.path.join(ROOT, "build", "css-property-vectors"))
    ap.add_argument("--replay", default=None)
    ap.add_argument("--plant", default=None)
    ap.add_argument("--list", default=LIST)
    ap.add_argument("--witnesses", default=WITNESSES)
    ap.add_argument("--write-list", default=None)
    ap.add_argument("--write-witnesses", default=None)
    ap.add_argument("--classes", default=os.path.join(HERE, "css_style_classes.tsv"))
    ap.add_argument("--max-ours", type=int, default=int(os.environ.get("CSS_STYLE_ORACLE_MAX_OURS", "0")))
    ap.add_argument("--max-unsupported", type=int, default=int(os.environ.get("CSS_STYLE_ORACLE_MAX_UNSUPPORTED", "100000")))
    args = ap.parse_args(argv)
    work = args.work
    os.makedirs(work, exist_ok=True)
    schemes = args.schemes.split(",")
    stages = ("populate", "metrics", "ours", "browsers", "classify") if args.stage == "all" else (args.stage,)
    pop_path = os.path.join(work, "population.jsonl")
    metrics_path = os.path.join(work, "metrics.tsv")
    ours_path = os.path.join(work, "ours.json")

    if "populate" in stages:
        docs = css_style_gen.population(args.seed, args.count, args.ours, args.wpt, args.scope)
        with open(pop_path, "w", encoding="utf-8") as f:
            for d in docs:
                f.write(json.dumps(d, ensure_ascii=False) + "\n")
        print("css style differential: seed %d, %d documents, %d items" % (args.seed, len(docs), sum(len(d["items"]) for d in docs)), flush=True)
    docs = [json.loads(x) for x in open(pop_path, encoding="utf-8")]

    if "browsers" in stages or "metrics" in stages:
        try:
            line = oracle_env.provenance(["cssprop"])
            base.check_driver(oracle_env.version("cssprop"))
        except oracle_env.OracleUnavailable as why:
            return oracle_env.decline("css style differential", why)
        print(line, flush=True)
    if "metrics" in stages:
        run_metrics("chromium", metrics_path)
    src = args.replay or work
    if args.replay and os.path.exists(os.path.join(src, "metrics.tsv")):
        metrics_path = os.path.join(src, "metrics.tsv")

    ours = None
    if "ours" in stages:
        ours = {s: run_ours(args.ours, docs, metrics_path, s) for s in schemes}
        json.dump(ours, open(ours_path, "w"), ensure_ascii=False)
    if ours is None and "classify" in stages:
        if args.replay:
            ours = {s: run_ours(args.ours, docs, metrics_path, s) for s in schemes}
        else:
            ours = {s: [[tuple(x) for x in doc] for doc in rows] for s, rows in json.load(open(ours_path)).items()}

    answers, headers = {}, {}
    if "browsers" in stages:
        for e in ENGINES:
            t0 = time.time()
            rows, hs = {}, {}
            for s in schemes:
                hs[s], rows[s] = run_engine(e, docs, s)
            headers[e] = hs[schemes[0]]
            answers[e] = rows
            json.dump({"header": headers[e], "rows": rows}, open(os.path.join(work, e + ".json"), "w"), ensure_ascii=False)
            print("  %s %s: %d documents x %d schemes in %.0f s" % (e, headers[e].get("browser"), len(docs), len(schemes), time.time() - t0), flush=True)
    if "classify" in stages:
        for e in ENGINES:
            if e not in answers:
                data = json.load(open(os.path.join(src, e + ".json")))
                headers[e], answers[e] = data["header"], data["rows"]
        flat = flatten(docs, schemes)
        ov = ours_view(ours, docs, schemes)
        av = {e: view(answers[e], docs, schemes, e) for e in ENGINES}
        if len(ov) != len(flat) or any(len(av[e]) != len(flat) for e in ENGINES):
            raise SystemExit("css_style_diff: the readers answered %d, %d and %d for %d items (a replay needs the same population)" % (len(ov), len(av["chromium"]), len(av["firefox"]), len(flat)))
        if args.plant:
            keep0 = [i for i in range(len(flat)) if ov[i] is not None and av["chromium"][i] is not None and av["firefox"][i] is not None]
            items0 = [(flat[i][3], describe(flat[i][3], flat[i][4], flat[i][1]), flat[i][5]) for i in keep0]
            o0 = [ov[i] for i in keep0]
            a0 = {e: [av[e][i] for i in keep0] for e in ENGINES}
            apply_plant(args.plant, items0, o0, a0, headers, schemes)
            for n, i in enumerate(keep0):
                ov[i] = o0[n]
                for e in ENGINES:
                    av[e][i] = a0[e][n]
        bad, features, listed, undecided, unanswered, items, o, a, keep, flat = gate(docs, schemes, flat, ov, av, headers, args)
        summarize(items, o, a, headers, features, listed, undecided, unanswered, len(flat), schemes)
        if args.write_list:
            write_list(args.write_list, items, o, a, features, listed, headers, args.write_witnesses, flat, keep, docs, read_rules(args.classes))
        if unanswered:
            bad.append("UNANSWERED: %d items hung an engine (first: %s)" % (len(unanswered), json.dumps(items[unanswered[0]])))
        if bad:
            print("\nCSS STYLE DIFFERENTIAL FAILED (%d problems)" % len(bad))
            for b in bad[:int(os.environ.get("CSS_STYLE_ORACLE_SHOW", "60"))]:
                print("  " + b)
            return GATE_EXIT
        print("css style differential: every divergence is classified, every witness holds, every control is as it was")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
