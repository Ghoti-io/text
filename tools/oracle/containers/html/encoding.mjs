// The Node half of the Encoding references (tools/oracle/encoding_ref.py): `TextDecoder` and `TextEncoder`, which in a full-ICU
// build implement the Encoding Standard's API (decode, label lookup, streaming, fatal, ignoreBOM) over ICU's converters.
//
// Read from the repository by the gate (the bind mount makes the tree visible in the container), not copied into the image: the
// image is the html image and is not rebuilt for this. What pins the reference is the image's Node and ICU, which the version line
// of the `encoding` pin in containers/IMAGES names and the gate checks.
//
// Every string is hex, "-" for none. Requests (stdin, one per line) and answers (stdout, one per line):
//
//   L <label>                       `new TextDecoder(label).encoding`            -> L <name> | L none (a RangeError)
//   D <label> <flags> <chunks>      decode, the chunks (comma-separated hex) fed with {stream: true} but the last. flags: f fatal,
//                                   i ignoreBOM (letters, "-" for none)           -> OUT <utf8> | ERR <error name>
//   E <utf8>                        `new TextEncoder().encode(string)`, the string the standard's decode of the bytes   -> OUT <utf8>
//   PING                            -> PONG
//
// and, as a command line, `node encoding.mjs enum <label> <n> <flags> <start> <count>`, the records of tools/oracle/encoding_ours.c, and
// `node encoding.mjs enumx <label> <flags> <spec>`, the records of the product of byte sets of the spec (see encoding_ours.c).
//
// Copyright 2026 by Corey Pennycuff
import { createInterface } from "node:readline";
import { writeSync } from "node:fs";

function unhex(s) {
  if (s === "-") return new Uint8Array(0);
  return Uint8Array.from(Buffer.from(s, "hex"));
}

function hex(buf) {
  return buf.length === 0 ? "-" : Buffer.from(buf).toString("hex");
}

function options(flags) {
  return { fatal: flags.includes("f"), ignoreBOM: flags.includes("i") };
}

function decode(label, flags, chunks) {
  const dec = new TextDecoder(label, options(flags));
  let out = "";
  chunks.forEach((c, i) => {
    out += dec.decode(c, { stream: i < chunks.length - 1 });
  });
  return out;
}

function answer(line) {
  const t = line.trim().split(/\s+/);
  try {
    switch (t[0]) {
      case "PING":
        return "PONG";
      case "L": {
        try {
          return "L " + new TextDecoder(Buffer.from(t[1] === "-" ? "" : t[1], "hex").toString("utf8")).encoding;
        } catch (e) {
          return "L none";
        }
      }
      case "D": {
        const label = Buffer.from(t[1] === "-" ? "" : t[1], "hex").toString("utf8");
        const chunks = t[3] === "-" ? [new Uint8Array(0)] : t[3].split(",").map(unhex);
        try {
          return "OUT " + hex(Buffer.from(decode(label, t[2], chunks), "utf8"));
        } catch (e) {
          return "ERR " + e.name;
        }
      }
      case "E": {
        const text = new TextDecoder("utf-8", { ignoreBOM: true }).decode(unhex(t[1]));
        return "OUT " + hex(new TextEncoder().encode(text));
      }
      default:
        return "ERR E_REQUEST";
    }
  } catch (e) {
    return "ERR " + e.name;
  }
}

function parseSpec(text) {
  return text.split(":").map((set) => {
    const bytes = new Set();
    for (const item of set.split(",")) {
      const [lo, hi] = item.split("-").map((x) => parseInt(x, 16));
      for (let b = lo; b <= (hi === undefined ? lo : hi); b++) bytes.add(b);
    }
    return [...bytes].sort((a, b) => a - b);
  });
}

if (process.argv[2] === "enumx") {
  const [label, flags, specText] = [process.argv[3], process.argv[4], process.argv[5]];
  const name = Buffer.from(label === "-" ? "" : label, "hex").toString("utf8");
  const dec = new TextDecoder(name, options(flags));
  const sets = parseSpec(specText);
  const n = sets.length;
  const input = new Uint8Array(n);
  const idx = new Array(n).fill(0);
  let rec = Buffer.alloc(1 << 20);
  let used = 0;
  const flush = () => {
    let off = 0;
    while (off < used) off += writeSync(1, rec, off, used - off);
    used = 0;
  };
  for (;;) {
    for (let k = 0; k < n; k++) input[k] = sets[k][idx[k]];
    let r;
    try {
      r = Buffer.from(dec.decode(input), "utf8");
    } catch (e) {
      r = null;
    }
    if (used + 300 > rec.length) flush();
    if (r === null) {
      rec[used++] = 255;
    } else {
      rec[used++] = r.length;
      r.copy(rec, used);
      used += r.length;
    }
    let k = n - 1;
    while (k >= 0 && ++idx[k] === sets[k].length) {
      idx[k] = 0;
      k--;
    }
    if (k < 0) break;
  }
  flush();
} else if (process.argv[2] === "enum") {
  const [label, n, flags, start, count] = [process.argv[3], Number(process.argv[4]), process.argv[5], BigInt(process.argv[6]), BigInt(process.argv[7])];
  const name = Buffer.from(label === "-" ? "" : label, "hex").toString("utf8");
  const dec = new TextDecoder(name, options(flags));
  const input = new Uint8Array(n);
  let rec = Buffer.alloc(1 << 20);
  let used = 0;
  const flush = () => {
    let off = 0;
    while (off < used) off += writeSync(1, rec, off, used - off);
    used = 0;
  };
  for (let v = start; v < start + count; v++) {
    for (let k = 0; k < n; k++) input[k] = Number((v >> BigInt(8 * (n - 1 - k))) & 0xffn);
    let r;
    try {
      r = Buffer.from(dec.decode(input), "utf8");
    } catch (e) {
      r = null;
    }
    if (used + 300 > rec.length) flush();
    if (r === null) {
      rec[used++] = 255;
    } else {
      rec[used++] = r.length;
      r.copy(rec, used);
      used += r.length;
    }
  }
  flush();
} else {
  const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
  rl.on("line", (line) => {
    if (line.trim() !== "") process.stdout.write(answer(line) + "\n");
  });
}
