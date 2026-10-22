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
"""Plant a defect in a scratch copy of the library and show the Encoding fuzzer finds it.

    tools/encoding/plant-fuzz.py [--time SECONDS] [name ...]

A fuzzer that has never failed has not been shown to be able to. Each plant here is one edit to one place in src/encoding/, applied to a **copy** of
this repository in a scratch directory (the working tree is never touched), and tests/fuzz/fuzz_encoding.cpp is run on the copy for the time budget
(60 seconds by default, the same `FUZZ_TIME` the Makefile's fuzz-run-* targets use), or, if the defect has a recorded reproducer (REPRODUCERS), over that
file with -runs=0, which is the same every time. It must stop with a report. The **control** is the same run with no edit, and it must not.

Twenty-two plants are replayed from a recorded input (the files `reproducer-*.seed` of tests/fuzz/corpus/encoding, which `make test` replays as well), so the
check is the same every time: that shows the harness *can* see the defect, not that the search reaches it. Nine are deliberately **searched for** from the
seeds, the way a real find would be: if the search stops finding them quickly the check fails.

replayed:

  utf8-restore-lost          the byte that ended a truncated sequence is not read again: the ASCII byte after an error is lost (the reference decoder).
  utf16-restore-swapped      the two bytes of a unit that followed a leading surrogate are read again in the wrong order (the reference decoder).
  single-byte-null-ignored   a byte the index has no code point for is decoded as U+0000 and not as an error (the byte-by-byte model, fatal against replacement).
  sniff-held-bytes-dropped   the bytes held back while a byte order mark was possible are lost when it turns out not to be one (the whole against the cut input).
  decoder-leak               a decoder is not freed (the counting allocator).
  encoder-reference-hex      a numeric reference is written `&#x` (the encoder's model).
  limit-off-by-one           an output exactly at the limit is refused (the limit is exact).
  sink-asked-again           a sink that refused is given the rest of the buffer when the call ends (the sink is not called again).
  fatal-offset-short         the offset of an unencodable code point is the stream's position and not the code point's start (the offset of the model).
  replacement-per-byte       the replacement decoder writes U+FFFD for every byte (one for the stream).
  decoder-input-bytes-run-end   input_bytes of a limit failure inside a run of ASCII is the end of the run and not the byte (the prefix property).
  encoder-input-bytes-run-end   the same for the encoder.
  buffer-growth-absorbed     a failed growth of the output buffer is ignored (the allocation-failure sweep: a success that is not the normal answer, or a crash).
  cjk-gb18030-restore-two    gb18030's decoder restores two bytes of a four-byte form broken at its last byte and not three (the multi-byte model).
  cjk-pair-second-lost       the second code point of one of Big5's four pairs is not written (the model).
  cjk-pair-written-apart     the two code points of such a pair are written one at a time, so that an output limit can fall between them (the model's boundaries).
  cjk-iso-escape-eoq-restore-lost   an escape sequence cut by the end of the input does not give back its first byte (the model).
  cjk-error-code-point-ignored   the encoder names the control that ISO-2022-JP refuses and not U+FFFD, so a reference reads `&#14;` (the model).
  cjk-sjis-eudc-upper        the last private use pointer of Shift_JIS (10715) is an error (the model).
  cjk-eucjp-0212-flag-kept   EUC-JP's "jis0212" survives a character (the model).
  cjk-iso-flush-escape       the escape written when ISO-2022-JP's input ends in the jis0208 or Roman state is the Roman one (the model).
  cjk-iso-reference-escape   a reference is written in the jis0208 state without the escape back to ASCII before it (the model).

searched (from the seeds: the corpus is replayed first, and the seeds of the multi-byte encodings reach these):

  cjk-euc-kr-trail-lower     EUC-KR's trail byte 0x41 is an error (the model).
  cjk-big5-trail-offset      Big5's offset for a trail byte above 0x7F is 0x61 and not 0x62 (the model).
  cjk-gb18030-ranges-gap     one more pointer of the ranges index is in the gap (the model).

  utf8-overlong-e0           the lower boundary after 0xE0 is not raised: an overlong form is accepted (the reference decoder).
  utf16-trail-range          the last trailing surrogate is not one (the reference decoder).
  x-user-defined-offset      0x80 to 0xFF map one code point too high (the byte-by-byte model against the encoder).
  decoder-not-reset-after-flush   the state of a stream survives its end (the cut against the whole).
  bom-removal-strips-every-mark   every leading U+FEFF is removed and not one (the reference decoder).
  encoder-malformed-not-counted   an ill-formed sequence written as U+FFFD is not counted as a replacement (the model's count).

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

TARGET = "encoding"

PLANTS = {
    "utf8-restore-lost": ("src/encoding/encoding_decode.c", "    out->restore_n = 1; /* Restore byte to ioQueue. */\n", ""),
    "utf16-restore-swapped": ("src/encoding/encoding_decode.c",
                              "    out->restore[0] = s->big_endian ? byte1 : byte2;\n    out->restore[1] = s->big_endian ? byte2 : byte1;",
                              "    out->restore[0] = s->big_endian ? byte2 : byte1;\n    out->restore[1] = s->big_endian ? byte1 : byte2;"),
    "single-byte-null-ignored": ("src/encoding/encoding_decode.c",
                                 "  if (cp == 0xFFFF) { /* the index has no code point for this byte */", "  if (0) { /* the index has no code point for this byte */"),
    "sniff-held-bytes-dropped": ("src/encoding/encoding_decode.c", "  run_items(dec, copy, n, false, taken, false, call);\n", ""),
    "decoder-leak": ("src/encoding/encoding_decode.c", "  gtext_allocator_free(&alloc, decoder->map_block);\n  gtext_allocator_free(&alloc, decoder);", "  gtext_allocator_free(&alloc, decoder->map_block);"),
    "encoder-reference-hex": ("src/encoding/encoding_encode.c", "  b[n++] = '#';", "  b[n++] = 'x';"),
    "limit-off-by-one": ("src/encoding/encoding_internal.h", "  if (n > out->limit - out->total) {\n    out->status = GTEXT_ENCODING_E_LIMIT_OUTPUT;",
                         "  if (n >= out->limit - out->total) {\n    out->status = GTEXT_ENCODING_E_LIMIT_OUTPUT;"),
    "sink-asked-again": ("src/encoding/encoding_core.c", "  const size_t n = out->n;\n  out->n = 0;\n  bool accepted;", "  const size_t n = out->n;\n  bool accepted;"),
    "fatal-offset-short": ("src/encoding/encoding_encode.c", "      call->error_offset = enc->seq_start;\n      return false;\n    }\n    if (!counted) {", "      call->error_offset = enc->pos;\n      return false;\n    }\n    if (!counted) {"),
    "replacement-per-byte": ("src/encoding/encoding_decode.c", "    state->replacement_error_returned = true;\n", ""),
    "buffer-growth-absorbed": ("src/encoding/encoding_core.c", "    if (!grown) {\n      b->oom = true;\n      return false;\n    }\n    if (!b->base) {", "    if (!grown) {\n      return true;\n    }\n    if (!b->base) {"),
    "decoder-input-bytes-run-end": ("src/encoding/encoding_decode.c", "count_in ? i + accepted + 1 : 0", "count_in ? j : 0"),
    "encoder-input-bytes-run-end": ("src/encoding/encoding_encode.c", "call->taken = i + accepted + 1;", "call->taken = j;"),
    "utf8-overlong-e0": ("src/encoding/encoding_decode.c", "        s->lower = 0xA0;\n", ""),
    "utf16-trail-range": ("src/encoding/encoding_decode.c", "    if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {\n      out->cp[0] = 0x10000", "    if (code_unit >= 0xDC00 && code_unit <= 0xDFFE) {\n      out->cp[0] = 0x10000"),
    "x-user-defined-offset": ("src/encoding/encoding_decode.c", "0xF780u + (uint32_t)byte - 0x80u", "0xF781u + (uint32_t)byte - 0x80u"),
    "decoder-not-reset-after-flush": ("src/encoding/encoding_decode.c",
                                      "  stream_start(dec, status == GTEXT_ENCODING_E_MALFORMED && !flush && dec->active == GTEXT_ENCODING_ISO_2022_JP);\n  return status;",
                                      "  return status;"),
    "bom-removal-strips-every-mark": ("src/encoding/encoding_decode.c", "    dec->strip_pending = false;\n    if (cp == 0xFEFF) {", "    if (cp == 0xFEFF) {"),
    "encoder-malformed-not-counted": ("src/encoding/encoding_encode.c", "    call->replacements++;\n    if (!encode_cp(enc, 0xFFFD, true, call)) {", "    if (!encode_cp(enc, 0xFFFD, true, call)) {"),
}


def _oracle_plant(oracle_name):
    """A plant of tools/oracle/ENCODING_ORACLE_PLANTS (the same defects the differential's controls plant), by its name there."""
    for path in (os.path.join(ROOT, "tools", "oracle", "ENCODING_ORACLE_PLANTS"), os.path.join(ROOT, "tools", "encoding", "UNIT_PLANTS")):
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 4 and parts[0] == oracle_name:
                    return ("src/encoding/" + parts[1], parts[2], parts[3])
    raise SystemExit("no plant %s in ENCODING_ORACLE_PLANTS or UNIT_PLANTS" % oracle_name)


for _name, _oracle in (("cjk-gb18030-restore-two", "gb18030-restore-two"), ("cjk-pair-second-lost", "cjk-pair-second-lost"),
                       ("cjk-pair-written-apart", "cjk-pair-written-apart"), ("cjk-iso-escape-eoq-restore-lost", "iso-escape-eoq-restore-lost"),
                       ("cjk-error-code-point-ignored", "cjk-error-code-point-ignored"), ("cjk-sjis-eudc-upper", "shift-jis-eudc-upper"),
                       ("cjk-eucjp-0212-flag-kept", "euc-jp-0212-flag-kept"), ("cjk-iso-flush-escape", "iso-enc-flush-escape"),
                       ("cjk-iso-reference-escape", "iso-enc-no-escape-before-reference"), ("cjk-euc-kr-trail-lower", "euc-kr-trail-lower"),
                       ("cjk-big5-trail-offset", "big5-trail-offset"), ("cjk-gb18030-ranges-gap", "gb18030-ranges-gap"),
                       ("cjk-iso-resume-keeps-all", "iso-resume-keeps-the-whole-handler"), ("cjk-iso-resume-keeps-escape", "iso-resume-keeps-a-half-read-escape"),
                       ("cjk-iso-resume-keeps-output-flag", "iso-resume-keeps-the-output-flag"), ("cjk-iso-fatal-names-fffd", "iso-fatal-status-names-u-fffd")):
    PLANTS[_name] = _oracle_plant(_oracle)
for _name, (_f, _old, _new) in list(PLANTS.items()):
    PLANTS[_name] = (_f, _old.replace("\\n", "\n"), _new.replace("\\n", "\n"))


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
    # The harness reads the pinned indexes (the multi-byte model) from the directory it runs in; nothing else of third_party/ is needed.
    os.makedirs(os.path.join(dest, "third_party", "encoding"), exist_ok=True)
    for name in ("indexes.json", "encodings.json"):
        shutil.copy(os.path.join(ROOT, "third_party", "encoding", name), os.path.join(dest, "third_party", "encoding", name))


# A plant whose defect has a recorded reproducer is found by replaying that file, which is the same every time, and not by a time-boxed search, which is
# not. The others keep the search.
REPRODUCERS = {
    "utf8-restore-lost": "tests/fuzz/corpus/encoding/reproducer-utf8-error-then-ascii.seed",
    "utf16-restore-swapped": "tests/fuzz/corpus/encoding/reproducer-utf16-lead-then-unit.seed",
    "single-byte-null-ignored": "tests/fuzz/corpus/encoding/reproducer-sb-unmapped-byte.seed",
    "sniff-held-bytes-dropped": "tests/fuzz/corpus/encoding/reproducer-sniff-held-prefix.seed",
    "decoder-leak": "tests/fuzz/corpus/encoding/reproducer-allocation-sweep-decode.seed",
    "encoder-reference-hex": "tests/fuzz/corpus/encoding/reproducer-encoder-reference.seed",
    "limit-off-by-one": "tests/fuzz/corpus/encoding/reproducer-limit-exactly-the-length.seed",
    "sink-asked-again": "tests/fuzz/corpus/encoding/reproducer-sink-refuses-late.seed",
    "fatal-offset-short": "tests/fuzz/corpus/encoding/reproducer-encoder-fatal-offset.seed",
    "replacement-per-byte": "tests/fuzz/corpus/encoding/reproducer-replacement-two-bytes.seed",
    "buffer-growth-absorbed": "tests/fuzz/corpus/encoding/reproducer-allocation-sweep-decode.seed",
    "decoder-input-bytes-run-end": "tests/fuzz/corpus/encoding/reproducer-limit-in-an-ascii-run.seed",
    "encoder-input-bytes-run-end": "tests/fuzz/corpus/encoding/reproducer-limit-in-an-ascii-run.seed",
    "cjk-gb18030-restore-two": "tests/fuzz/corpus/encoding/reproducer-cjk-gb18030-restore.seed",
    "cjk-pair-second-lost": "tests/fuzz/corpus/encoding/reproducer-cjk-big5-pair.seed",
    "cjk-pair-written-apart": "tests/fuzz/corpus/encoding/reproducer-cjk-big5-pair.seed",
    "cjk-iso-escape-eoq-restore-lost": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-escape-cut.seed",
    "cjk-error-code-point-ignored": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-control-html-reference.seed",
    "cjk-sjis-eudc-upper": "tests/fuzz/corpus/encoding/reproducer-cjk-sjis-eudc.seed",
    "cjk-eucjp-0212-flag-kept": "tests/fuzz/corpus/encoding/reproducer-cjk-eucjp-flag.seed",
    "cjk-iso-flush-escape": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-flush-escape.seed",
    "cjk-iso-reference-escape": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-reference-escape.seed",
    "cjk-iso-fatal-names-fffd": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-control-reference.seed",
    "cjk-iso-resume-keeps-all": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-resume-after-fatal.seed",
    "cjk-iso-resume-keeps-escape": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-resume-after-fatal.seed",
    "cjk-iso-resume-keeps-output-flag": "tests/fuzz/corpus/encoding/reproducer-cjk-iso-resume-output-flag.seed",
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
            if name in REPRODUCERS:
                built = subprocess.run(capped + ["make", "-C", text, "-j4", "fuzz-" + target, "PREFIX=" + prefix],
                                       env=env, capture_output=True, text=True, timeout=budget)
                if built.returncode != 0:
                    return built.returncode, built.stdout + built.stderr
                app = os.path.join(text, make_variables(prefix, ("FUZZ_APP_DIR",))["FUZZ_APP_DIR"], "fuzz_" + target.replace("-", "_"))
                done = subprocess.run(capped + [app, "-runs=0", os.path.join(text, REPRODUCERS[name])], env=env,
                                      capture_output=True, text=True, timeout=budget)
                return done.returncode, done.stdout + done.stderr
            cmd = capped + ["make", "-C", text, "-j4", "fuzz-run-" + target, "FUZZ_TIME=%d" % seconds,
                            "FUZZ_ARGS=-seed=1 -max_len=2048", "PREFIX=" + prefix]
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
REPORT = re.compile(r"^(fuzz_encoding: .+|==\d+==ERROR: (AddressSanitizer|LeakSanitizer|libFuzzer).*|"
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
