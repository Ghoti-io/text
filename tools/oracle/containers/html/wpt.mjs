// The web-platform-tests runner for the Encoding module (tools/oracle/encoding_wpt.py): the `encoding/` tests of the standard's own suite, written for
// a browser, run here in Node against **this library** through a pair of named pipes.
//
//   node wpt.mjs <pipe dir> <wpt encoding/ dir> [--plant NAME] <file>...
//
// A test file is a JavaScript file (`*.any.js`) for the testharness API and the Encoding API's `TextDecoder` and `TextEncoder`; this runner is a
// minimal testharness (test, promise_test, assert_*) and a `TextDecoder`, `TextEncoder` and `XMLHttpRequest` (only what the tests use of it: a
// `data:` URL decoded with the standard's "decode", the byte order mark sniffed over the charset of `overrideMimeType`) whose decoding and encoding
// is done by tools/oracle/encoding_ours.c, the other end of the pipes. So what is run is the standard's own vectors against the library's own code;
// the JavaScript here decides nothing but the API's argument handling (a label that names no encoding, or `replacement`, is a RangeError; a decoder
// in fatal mode that fails is a TypeError), which the tests are about and the library does not have.
//
// An answer from this library that says the encoding has no codec yet (the multi-byte encodings of the next stage) makes the test "deferred", not
// failed: the runner prints one JSON object per test on stdout, with status pass, fail or deferred.
//
// Copyright 2026 by Corey Pennycuff
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";

const [pipeDir, testDir, ...rest] = process.argv.slice(2);
let plant = null;
const files = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === "--plant") plant = rest[++i];
  else files.push(rest[i]);
}

// ---- the pipes ------------------------------------------------------------------------------------------------------------

const reqFd = fs.openSync(path.join(pipeDir, "req"), "w");
const respFd = fs.openSync(path.join(pipeDir, "resp"), "r");
let pending = "";
const chunk = Buffer.alloc(1 << 16);

function call(line) {
  const out = Buffer.from(line + "\n", "latin1");
  let off = 0;
  while (off < out.length) off += fs.writeSync(reqFd, out, off, out.length - off);
  for (;;) {
    const nl = pending.indexOf("\n");
    if (nl >= 0) {
      const answer = pending.slice(0, nl);
      pending = pending.slice(nl + 1);
      return answer;
    }
    const n = fs.readSync(respFd, chunk, 0, chunk.length, null);
    if (n === 0) throw new Error("the library's runner closed the pipe");
    pending += chunk.toString("latin1", 0, n);
  }
}

const hex = (bytes) => (bytes.length === 0 ? "-" : Buffer.from(bytes.buffer, bytes.byteOffset, bytes.length).toString("hex"));
const unhex = (s) => (s === "-" ? new Uint8Array(0) : new Uint8Array(Buffer.from(s, "hex")));
const hexOfString = (s) => hex(new Uint8Array(Buffer.from(s, "utf8")));

class UnsupportedEncoding extends Error {
  constructor(message) {
    super(message);
    this.name = "UnsupportedEncoding";
  }
}

// ---- the Encoding API over the pipes ------------------------------------------------------------------------------------

let nextId = 0;
const generation = new Array(64).fill(0);

function bytesOf(input) {
  if (input === undefined) return new Uint8Array(0);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer)) return new Uint8Array(input);
  throw new TypeError("the input is not a buffer source");
}

class TextDecoder {
  #id;
  #gen;
  #name;
  #fatal;
  #ignoreBOM;
  #unsupported = false;
  constructor(label = "utf-8", options = {}) {
    const l = call("L " + hexOfString(String(label)));
    const name = l.split(" ")[1];
    if (name === "none" || name === "replacement") throw new RangeError("the label " + JSON.stringify(String(label)) + " is not supported");
    this.#name = name;
    this.#fatal = !!(options && options.fatal);
    this.#ignoreBOM = !!(options && options.ignoreBOM);
    this.#id = nextId++ % 60;
    this.#gen = ++generation[this.#id];
    const flags = (this.#fatal ? "f" : "") + (this.#ignoreBOM ? "" : "r");
    const a = call("ND " + this.#id + " " + hexOfString(String(label)) + " " + (flags || "-"));
    if (a.startsWith("ERR E_UNSUPPORTED")) this.#unsupported = true;
    else if (!a.startsWith("OK")) throw new Error("ND: " + a);
  }
  get encoding() {
    return this.#name;
  }
  get fatal() {
    return this.#fatal;
  }
  get ignoreBOM() {
    return this.#ignoreBOM;
  }
  decode(input, options) {
    if (this.#unsupported) throw new UnsupportedEncoding("E_UNSUPPORTED: no codec for " + this.#name);
    if (generation[this.#id] !== this.#gen) throw new Error("decoder slot reused");
    let bytes = bytesOf(input);
    const stream = !!(options && options.stream);
    if (plant === "bridge-drops-a-byte" && bytes.length > 3) bytes = bytes.subarray(0, bytes.length - 1);
    const a = call("D " + this.#id + " " + (stream ? 0 : 1) + " " + hex(bytes));
    const t = a.split(" ");
    if (t[0] === "OUT") return Buffer.from(unhex(t[1])).toString("utf8");
    if (t[1] === "E_MALFORMED") throw new TypeError("the data is not valid " + this.#name);
    throw new Error("D: " + a);
  }
}

class TextEncoder {
  #id;
  constructor(label) {
    // The encoder takes no label: any argument is ignored, and the encoding is UTF-8.
    this.#id = 63;
    const a = call("NE " + this.#id + " " + hexOfString("utf-8") + " R");
    if (!a.startsWith("OK")) throw new Error("NE: " + a);
  }
  get encoding() {
    return "utf-8";
  }
  encode(input = "") {
    // USVString: a lone surrogate becomes U+FFFD (the Web IDL conversion, which is not the library's).
    const text = String(input).toWellFormed();
    const a = call("E " + this.#id + " 1 " + hexOfString(text));
    const t = a.split(" ");
    if (t[0] !== "OUT") throw new Error("E: " + a);
    return unhex(t[1]);
  }
}

// XMLHttpRequest, as far as the tests use it: GET of a `data:` URL, the charset of overrideMimeType, responseText, onload.
class XMLHttpRequest {
  open(method, url) {
    this.url = url;
  }
  overrideMimeType(mime) {
    this.mime = mime;
  }
  send() {
    setTimeout(() => {
      try {
        const m = /^data:text\/plain,(.*)$/s.exec(this.url);
        const bytes = new Uint8Array(Buffer.from(m[1].replace(/%([0-9A-Fa-f]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))), "latin1"));
        const cs = /charset="?([^";]*)"?/i.exec(this.mime || "");
        let label = cs ? cs[1] : "utf-8";
        let l = call("L " + hexOfString(label)).split(" ")[1];
        if (l === "none") label = "utf-8";
        const id = 62;
        const a = call("ND " + id + " " + hexOfString(label) + " s");
        if (a.startsWith("ERR E_UNSUPPORTED")) throw new UnsupportedEncoding("E_UNSUPPORTED: no codec for " + label);
        const r = call("D " + id + " 1 " + hex(bytes)).split(" ");
        this.responseText = Buffer.from(unhex(r[1])).toString("utf8");
        this.statusText = "OK";
        if (this.onload) this.onload();
      } catch (e) {
        this.statusText = String(e);
        if (this.onerror) this.onerror(e);
      }
    }, 0);
  }
}

// ---- a minimal testharness ---------------------------------------------------------------------------------------------------

const tests = [];
// test() runs its function at once, as testharness does: a loop in a test file registers many tests with closures over a variable it goes on to change.
function test(fn, name) {
  try {
    fn({ step: (f) => f(), add_cleanup() {} });
    tests.push({ name: String(name), result: { status: "pass", message: "" } });
  } catch (e) {
    const message = String(e && e.stack ? e.message : e).slice(0, 300);
    tests.push({ name: String(name), result: { status: e instanceof UnsupportedEncoding || /E_UNSUPPORTED/.test(message) ? "deferred" : "fail", message } });
  }
}
function promise_test(fn, name) {
  tests.push({ name: String(name), run: () => fn({ step: (f) => f(), add_cleanup() {} }) });
}
function async_test() {
  throw new UnsupportedEncoding("async_test is not supported by this runner");
}
class AssertionError extends Error {}
function fail(message, detail) {
  throw new AssertionError((message ? message + ": " : "") + detail);
}
function show(v) {
  try {
    return typeof v === "string" ? JSON.stringify(v) : String(v);
  } catch {
    return "<value>";
  }
}
const assert_equals = (a, b, m) => {
  if (!(a === b || (a !== a && b !== b))) fail(m, "expected " + show(b) + " but got " + show(a));
};
const assert_not_equals = (a, b, m) => {
  if (a === b) fail(m, "got the disallowed value " + show(a));
};
const assert_true = (v, m) => {
  if (v !== true) fail(m, "expected true got " + show(v));
};
const assert_false = (v, m) => {
  if (v !== false) fail(m, "expected false got " + show(v));
};
const assert_array_equals = (a, b, m) => {
  if (a.length !== b.length) fail(m, "lengths differ: " + a.length + " vs " + b.length);
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) fail(m, "element " + i + ": " + show(a[i]) + " vs " + show(b[i]));
};
const assert_throws_js = (ctor, fn, m) => {
  try {
    fn();
  } catch (e) {
    if (e instanceof ctor) return;
    throw e instanceof UnsupportedEncoding ? e : new AssertionError((m ? m + ": " : "") + "expected " + ctor.name + " but got " + (e && e.name) + " " + (e && e.message));
  }
  fail(m, "expected " + ctor.name + " but nothing was thrown");
};
const assert_unreached = (m) => fail(m, "reached unreachable code");
const format_value = (v) => (typeof v === "string" ? JSON.stringify(v) : String(v));
const setup = (f) => (typeof f === "function" ? f() : undefined);
const subsetTest = (testFunc, ...args) => testFunc(...args);
const createBuffer = (kind, length) => (kind === "SharedArrayBuffer" ? new SharedArrayBuffer(length) : new ArrayBuffer(length));

Object.assign(globalThis, {
  TextDecoder, TextEncoder, XMLHttpRequest, test, promise_test, async_test, assert_equals, assert_not_equals, assert_true, assert_false,
  assert_array_equals, assert_throws_js, assert_unreached, format_value, setup, subsetTest, createBuffer,
  location: { search: "" },
});

// ---- running files -----------------------------------------------------------------------------------------------------------

for (const file of files) {
  tests.length = 0;
  const full = path.join(testDir, file);
  const source = fs.readFileSync(full, "utf8");
  let prelude = "";
  for (const m of source.matchAll(/^\/\/ META: script=(.+)$/gm)) {
    const script = m[1].trim();
    if (script.startsWith("/")) continue; // /common/... : provided above
    prelude += fs.readFileSync(path.join(path.dirname(full), script), "utf8") + "\n";
  }
  // The declarations of one file must not collide with the next one's (two files include the same resources/encodings.js with its top-level `const`), so
  // each runs in a function scope.
  globalThis.location = { search: /META: variant=\?TextDecoder/.test(source) ? "?TextDecoder" : "" };
  let loadError = null;
  try {
    vm.runInThisContext("(function () {\n" + prelude + "\n" + source + "\n})();", { filename: file });
  } catch (e) {
    loadError = e;
  }
  if (loadError) {
    console.log(JSON.stringify({ file, name: "(loading the file)", status: loadError instanceof UnsupportedEncoding ? "deferred" : "fail", message: String(loadError).slice(0, 300) }));
    continue;
  }
  const todo = tests.slice();
  for (const t of todo) {
    if (t.result) {
      console.log(JSON.stringify({ file, name: t.name, status: t.result.status, message: t.result.message }));
      continue;
    }
    let status = "pass";
    let message = "";
    try {
      await t.run();
    } catch (e) {
      message = String(e && e.stack ? e.message : e).slice(0, 300);
      status = e instanceof UnsupportedEncoding || /E_UNSUPPORTED/.test(message) ? "deferred" : "fail";
    }
    console.log(JSON.stringify({ file, name: t.name, status, message }));
  }
}
fs.closeSync(reqFd);
fs.closeSync(respFd);
