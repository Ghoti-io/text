"""The consolidated model of the pinned w3c/webref data.

Shared by gen-css-tables.py (which writes src/css/tables/) and gen-depth.py. Nothing here writes a file.

What is read: ``ed/css/*.json`` (one file per specification series; the key ``value`` is the grammar) and the three
``ed/dfns/css-values-{3,4,5}.json`` files, after the files have been checked against the manifest
``tools/css/WEBREF_SHA256``.

Consolidation (documented, and held by tests/test-css-registry.cpp through the generated tables):

* A file's *series* is its name without a trailing ``-N`` and its *level* is that N (1 when there is none):
  ``css-fonts-5.json`` is level 5 of the series ``css-fonts``, ``css-fonts.json`` level 1.
* Each name is resolved **once per scope** (a property by its name; a descriptor by its at-rule and name; a type
  or function by its name, and by the property when the pin nests it inside that property). The *base definition*
  is the entry with a grammar (``value``) in the highest level of its series. Two different series that both give
  a grammar for one name and scope are an error: the generator refuses it, and the choice would be written into
  ``tools/css/consolidate.tsv``.
* ``newValues`` of every entry (any level) are alternatives appended to the base grammar with `` | ``, in series
  order (the order of the level), so that ``width`` of css-sizing reads ``auto | <box-size> | <anchor-size()>``.
  ``|`` has the lowest precedence of the Value Definition Syntax, so appending never changes how the base reads.
* Every other field (initial, inherited, animation type, computed value, applies-to, percentages, canonical order,
  logical group, longhands) is the base entry's own. A field the base lacks is **not** filled in from another
  level: it is absent. (``continue``: css-overflow-5 names ``auto`` as the initial value and extends the grammar of
  css-overflow-4 by ``overflow | paginate | fragments``; the grammar still has ``normal`` and not ``auto``, the base
  entry's ``normal`` is the initial value, and the divergence is one of the pin's own.)
* A legacy alias (``legacyAliasOf``) is a name for its target: its own grammar copy, initial value and the like are
  not kept.
"""

import glob
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "third_party", "css", "webref")


def die(msg):
    sys.stderr.write("gen-css-tables: %s\n" % msg)
    raise SystemExit(1)


def read_manifest(path):
    entries = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            digest, name = line.split("  ", 1)
            entries[name] = digest
    return entries


def verify_manifest(data_dir, manifest_path):
    """Check every file against the manifest; the file list must match both ways."""
    manifest = read_manifest(manifest_path)
    held = set()
    for sub in ("ed/css", "ed/dfns"):
        for dirpath, _, files in os.walk(os.path.join(data_dir, sub)):
            for name in files:
                held.add(os.path.relpath(os.path.join(dirpath, name), data_dir))
    for name in sorted(set(manifest) - held):
        die("%s is in the manifest and not in the checkout (run tools/css/fetch.sh)" % name)
    for name in sorted(held - set(manifest)):
        die("%s is in the checkout and not in the manifest" % name)
    for name, digest in sorted(manifest.items()):
        with open(os.path.join(data_dir, name), "rb") as handle:
            got = hashlib.sha256(handle.read()).hexdigest()
        if got != digest:
            die("%s hashes %s, pinned %s (run tools/css/fetch.sh)" % (name, got, digest))
    return hashlib.sha256(open(manifest_path, "rb").read()).hexdigest()


SERIES_RE = re.compile(r"^(.*?)(?:-(\d+))?$")


def series_level(stem):
    m = SERIES_RE.match(stem)
    return m.group(1), int(m.group(2) or 1)


class Entry:
    """One definition of a name in one file."""

    def __init__(self, stem, raw):
        self.stem = stem
        self.series, self.level = series_level(stem)
        self.raw = raw

    def key(self):
        return (self.level, self.stem)


def base_of(entries, what):
    """The base definition: the entry with a grammar at the highest level of one series; None if no entry has one."""
    full = [e for e in entries if "value" in e.raw]
    if not full:
        return None
    series = {e.series for e in full}
    if len(series) > 1:
        die("%s: grammars from several series (%s); write the choice into tools/css/consolidate.tsv"
            % (what, ", ".join(sorted(e.stem for e in full))))
    full.sort(key=Entry.key)
    if len(full) > 1 and full[-1].level == full[-2].level:
        die("%s: two grammars at one level (%s)" % (what, ", ".join(e.stem for e in full)))
    return full[-1]


def merged_grammar(base, entries):
    parts = [base.raw["value"].strip()]
    extra = [e for e in entries if "newValues" in e.raw]
    extra.sort(key=lambda e: (e.series, e.level, e.stem))
    for e in extra:
        add = e.raw["newValues"].strip()
        if add:
            parts.append(add)
    return " | ".join(parts)


def type_key(name):
    """`<length>` is the type `length`; `rgb()` is the function `rgb()`; `<calc-size()>` is `calc-size()`."""
    if name.startswith("<") and name.endswith(">"):
        return name[1:-1]
    return name


def load(data_dir=DATA, manifest_path=None):
    if manifest_path is None:
        manifest_path = os.path.join(ROOT, "tools", "css", "WEBREF_SHA256")
    manifest_hash = verify_manifest(data_dir, manifest_path)

    props = {}      # name -> [Entry]
    atrules = {}    # name -> [Entry]
    descs = {}      # (atrule, name) -> [Entry]
    types = {}      # (scope, key) -> [Entry]; scope is '' or a property name
    selectors = {}  # name -> [Entry]
    stems = []

    for path in sorted(glob.glob(os.path.join(data_dir, "ed", "css", "*.json"))):
        stem = os.path.basename(path)[:-5]
        stems.append(stem)
        with open(path, encoding="utf-8") as handle:
            doc = json.load(handle)
        for p in doc["properties"]:
            props.setdefault(p["name"], []).append(Entry(stem, p))
            for v in p.get("values", []):
                if v["type"] in ("function", "type"):
                    types.setdefault((p["name"], type_key(v["name"])), []).append(Entry(stem, v))
        for a in doc["atrules"]:
            atrules.setdefault(a["name"], []).append(Entry(stem, a))
            for d in a["descriptors"]:
                descs.setdefault((a["name"], d["name"]), []).append(Entry(stem, d))
        for v in doc["values"]:
            types.setdefault(("", type_key(v["name"])), []).append(Entry(stem, v))
        for s in doc["selectors"]:
            selectors.setdefault(s["name"], []).append(Entry(stem, s))

    dfns = {}
    for n in (3, 4, 5):
        path = os.path.join(data_dir, "ed", "dfns", "css-values-%d.json" % n)
        with open(path, encoding="utf-8") as handle:
            dfns[n] = json.load(handle)["dfns"]

    return {
        "props": props,
        "atrules": atrules,
        "descs": descs,
        "types": types,
        "selectors": selectors,
        "dfns": dfns,
        "stems": stems,
        "manifest_hash": manifest_hash,
    }
