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
// The runner prints one JSON object per test on stdout, with status pass or fail; for the HTML pages of the legacy multi-byte vectors (below) it prints one
// object per page with the number of tests that passed ("count") and one per test that failed.
//
// **The HTML pages.** The vectors of the legacy multi-byte encodings (legacy-mb-*/) are pages for a browser: the decode pages read the text a document
// of the encoding becomes in an iframe, the encode pages submit a form or set an `<a>`'s href and read what the browser made of the characters in the
// document's encoding. Their expectations are computed by the pages' own JavaScript (written by the suite's authors from the standard's data, jis0208_index.js
// and the like), so they are an independent reading of the standard. Run here, the browser is a thin emulation (see "the pages" below): the document of
// an iframe is decoded by **this library**, a form's value is encoded by **this library** in HTML mode and serialised as
// application/x-www-form-urlencoded, an href's query is encoded by **this library** in HTML mode and percent-encoded with the URL standard's query set.
// The page scripts run unchanged, in a context of their own.
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
    if (!a.startsWith("OK")) throw new Error("ND: " + a);
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
        if (!a.startsWith("OK")) throw new Error("ND: " + a);
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
    tests.push({ name: String(name), result: { status: "fail", message } });
  }
}
function promise_test(fn, name) {
  tests.push({ name: String(name), run: () => fn({ step: (f) => f(), add_cleanup() {} }) });
}
// An asynchronous test as the pages use it: steps run at once, and the test passes when done() is called without a step having failed.
function async_test(name) {
  const record = { name: String(name), result: null };
  tests.push(record);
  return {
    step(fn) {
      if (record.result) return;
      try {
        fn();
      } catch (e) {
        record.result = { status: "fail", message: String(e && e.stack ? e.message : e).slice(0, 300) };
      }
    },
    done() {
      if (!record.result) record.result = { status: "pass", message: "" };
    },
    add_cleanup() {},
  };
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
    throw new AssertionError((m ? m + ": " : "") + "expected " + ctor.name + " but got " + (e && e.name) + " " + (e && e.message));
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


// ---- the pages -------------------------------------------------------------------------------------------------------------

// This library's codecs over the pipes, for a whole string or a whole document.
function encodeHtml(label, text) {
  const id = 61;
  const labelHex = hexOfString(label);
  const a = call("NE " + id + " " + labelHex + " H");
  if (!a.startsWith("OK")) throw new Error("NE: " + a + " for " + label);
  const r = call("E " + id + " 1 " + hexOfString(text));
  call("FREE " + id);
  const t = r.split(" ");
  if (t[0] !== "OUT") throw new Error("E: " + r);
  return unhex(t[1]);
}

function decodeBytes(label, bytes) {
  const id = 62;
  const a = call("ND " + id + " " + hexOfString(label) + " -");
  if (!a.startsWith("OK")) throw new Error("ND: " + a + " for " + label);
  const r = call("D " + id + " 1 " + hex(bytes)).split(" ");
  call("FREE " + id);
  if (r[0] !== "OUT") throw new Error("D: " + r.join(" "));
  return Buffer.from(unhex(r[1])).toString("utf8");
}

// application/x-www-form-urlencoded's byte serializer: the form submission's last step.
function urlencoded(bytes) {
  let out = "";
  for (const b of bytes) {
    if (b === 0x20) out += "+";
    else if (b === 0x2a || b === 0x2d || b === 0x2e || b === 0x5f || (b >= 0x30 && b <= 0x39) || (b >= 0x41 && b <= 0x5a) || (b >= 0x61 && b <= 0x7a)) out += String.fromCharCode(b);
    else out += "%" + b.toString(16).toUpperCase().padStart(2, "0");
  }
  return out;
}

// The URL standard's query percent-encoding of the bytes an encoder made, for a special scheme ("https"): the C0 controls, space, ", #, <, >, ' and
// everything above 0x7E, and the numeric references the encoder wrote for what it could not write ("percent-encode after encoding": the error's
// `&#N;` is percent-encoded as a whole, & # and ; included, the digits not). A reference is recognised by its shape, so an input that has the two characters
// `&#` of its own cannot be told from one, and the emulation refuses it (no page has one).
function queryEncode(label, text) {
  if (text.includes("&#")) throw new Error("the emulation of the href does not handle &# in the input");
  const bytes = encodeHtml(label, text);
  const asText = Buffer.from(bytes).toString("latin1");
  let out = "";
  for (let i = 0; i < bytes.length; ) {
    const m = /^&#(\d+);/.exec(asText.slice(i, i + 12));
    if (m) {
      out += "%26%23" + m[1] + "%3B";
      i += m[0].length;
      continue;
    }
    const b = bytes[i++];
    if (b <= 0x20 || b === 0x22 || b === 0x23 || b === 0x3c || b === 0x3e || b === 0x27 || b >= 0x7f) out += "%" + b.toString(16).toUpperCase().padStart(2, "0");
    else out += String.fromCharCode(b);
  }
  return out;
}

function decodeEntities(s) {
  return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|amp|lt|gt|quot|apos);/g, (m, e) => {
    if (e === "amp") return "&";
    if (e === "lt") return "<";
    if (e === "gt") return ">";
    if (e === "quot") return '"';
    if (e === "apos") return "'";
    return String.fromCodePoint(e[1] === "x" ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10));
  });
}

function charsetOfFile(full, text) {
  // The HTTP header of the suite's server (a `.headers` file beside the page) overrides the page's own declaration.
  if (fs.existsSync(full + ".headers")) {
    const h = /charset=([^\s;]+)/i.exec(fs.readFileSync(full + ".headers", "latin1"));
    if (h) return h[1];
  }
  const m = /<meta\s+charset\s*=\s*["']?([^"'\s>]+)/i.exec(text);
  return m ? m[1] : "utf-8";
}

// One page of the legacy vectors, run in a context of its own with an emulation of the little of a browser it uses.
async function runPage(kind, file) {
  const full = path.join(testDir, file);
  const dir = path.dirname(full);
  const bytes = fs.readFileSync(full);
  const text = bytes.toString("utf8");
  const pageCharset = charsetOfFile(full, text);
  tests.length = 0;
  let outstanding = 0;
  const listeners = [];
  const created = [];

  function element(tag) {
    const el = { tag, id: "", name: "", value: "", children: [], appendChild(c) { this.children.push(c); return c; } };
    return el;
  }
  const body = element("body");
  body.insertBefore = (c) => body.appendChild(c);
  const documentObject = {
    body,
    readyState: "complete",
    querySelector(sel) {
      if (sel === "meta[charset]") return { getAttribute: () => pageCharset };
      return null;
    },
    getElementById(id) {
      if (id === "scrwin") return frameOfData();
      return created.find((e) => e.id === id) || null;
    },
    getElementsByTagName(tag) {
      return created.filter((c) => c.tag === tag); // (in the order they were made, which is the document's: every element a page makes it attaches)
    },
    createElement(tag) {
      if (tag === "a") {
        const a = { search: "" };
        Object.defineProperty(a, "href", {
          set(v) {
            let url = String(v).replace(/[\t\n\r]/g, "").replace(/^[\x00-\x20]+|[\x00-\x20]+$/g, "");
            const hash = url.indexOf("#");
            if (hash >= 0) url = url.slice(0, hash);
            const q = url.indexOf("?");
            const query = q >= 0 ? queryEncode(pageCharset, url.slice(q + 1)) : "";
            a.search = query === "" ? "" : "?" + query;
          },
          get() { return ""; },
        });
        return a;
      }
      const el = element(tag);
      created.push(el);
      if (tag === "form") {
        el.submit = function () {
          const label = this.acceptCharset;
          const parts = this.children.filter((c) => c.tag === "input").map((c) => urlencoded(Buffer.from(c.name)) + "=" + urlencoded(encodeHtml(label, c.value)));
          const target = created.find((e) => e.id === this.target && e.tag === "iframe");
          outstanding++;
          setTimeout(() => {
            try {
              target.contentWindow = { location: { search: "?" + parts.join("&") } };
              if (target.onload) target.onload();
            } catch (e) {
              tests.push({ name: "(the submission)", result: { status: "fail", message: String(e && e.stack ? e.stack : e).slice(0, 300) } });
            } finally {
              outstanding--;
            }
          }, 0);
        };
      }
      return el;
    },
  };

  // The document an iframe loads (decode pages): the decoded text of the data page, its spans as elements.
  function frameOfData() {
    const m = /<iframe[^>]*\bsrc="([^"]+)"/i.exec(text);
    const dataFile = path.join(dir, m[1]);
    const data = fs.readFileSync(dataFile);
    const label = charsetOfFile(dataFile, data.toString("latin1"));
    const decoded = decodeBytes(label, new Uint8Array(data));
    const nodes = [];
    for (const sp of decoded.matchAll(/<span([^>]*)>([\s\S]*?)<\/span>/g)) {
      const dataset = {};
      for (const at of sp[1].matchAll(/data-([a-z]+)="([^"]*)"/g)) dataset[at[1]] = at[2];
      nodes.push({ dataset, textContent: decodeEntities(sp[2]) });
    }
    return { contentWindow: { document: { querySelectorAll: () => nodes } }, contentDocument: null };
  }

  const sandbox = {
    document: documentObject,
    test, async_test, promise_test, assert_equals, assert_not_equals, assert_true, assert_false, assert_array_equals, assert_unreached, setup, subsetTest,
    addEventListener: (type, fn) => { if (type === "load") listeners.push(fn); },
    alert: (m) => { throw new Error("alert: " + m); },
    setTimeout, console,
  };
  sandbox.window = sandbox;
  const ctx = vm.createContext(sandbox);

  // The scripts in document order: the suite's own (testharness, subset-tests) are provided above; the rest are the page's and its helpers.
  for (const sc of text.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi)) {
    const src = /\bsrc\s*=\s*["']?([^"'\s>]+)/i.exec(sc[1]);
    let code;
    let name;
    if (src) {
      if (src[1].startsWith("/")) continue; // /resources/testharness.js, /common/subset-tests.js: provided
      name = src[1];
      code = fs.readFileSync(path.join(dir, src[1]), "utf8");
    } else {
      name = file + " (inline)";
      code = sc[2];
    }
    vm.runInContext(code, ctx, { filename: name });
  }
  // load: the body's onload attribute (decode pages), and the listeners of the window (form pages).
  const onload = /<body[^>]*\bonload="([^"]*)"/i.exec(text);
  if (onload) vm.runInContext(onload[1], ctx, { filename: file + " (onload)" });
  for (const fn of listeners.splice(0)) fn();
  for (let spin = 0; outstanding > 0 && spin < 100000; spin++) await new Promise((r) => setTimeout(r, 0));
  if (outstanding > 0) tests.push({ name: "(the page)", result: { status: "fail", message: "submissions that never came back" } });

  let passed = 0;
  for (const t of tests) {
    const r = t.result || { status: "fail", message: "the test never finished" };
    if (r.status === "pass") passed++;
    else console.log(JSON.stringify({ file, name: t.name, status: r.status, message: r.message }));
  }
  console.log(JSON.stringify({ file, name: "(page)", status: "pass", count: passed }));
  tests.length = 0;
}

// ---- running files -----------------------------------------------------------------------------------------------------------

for (const entry of files) {
  const colon = entry.indexOf("=");
  if (colon > 0) {
    // kind=file: an HTML page (decode, form, href and inline pages are all run by runPage, which looks at what the page includes)
    try {
      await runPage(entry.slice(0, colon), entry.slice(colon + 1));
    } catch (e) {
      console.log(JSON.stringify({ file: entry.slice(colon + 1), name: "(the page)", status: "fail", message: String(e && e.stack ? e.stack : e).slice(0, 400) }));
    }
    continue;
  }
  const file = entry;
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
    console.log(JSON.stringify({ file, name: "(loading the file)", status: "fail", message: String(loadError).slice(0, 300) }));
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
      status = "fail";
    }
    console.log(JSON.stringify({ file, name: t.name, status, message }));
  }
}
fs.closeSync(reqFd);
fs.closeSync(respFd);
