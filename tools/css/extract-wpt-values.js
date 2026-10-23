// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Text.
//
// Ghoti.io Text is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Turn the value tests of web-platform-tests' css/css-values and css/css-color/parsing into vectors.
//
//   node tools/css/extract-wpt-values.js WPT_ROOT OUT.json
//
// The pages' own scripts are run in a sandbox whose test helpers are replaced by recorders: what is recorded
// is what the page asks, not what a browser answers. Only the specified-value stage is kept (the computed and
// used stages belong to the cascade, C2). A vector is
//
//   {file, n, kind, prop, value, expected, approx, epsilon}
//
// kind "valid": `value` must be accepted by the grammar of `prop` and be written as `expected`;
// kind "invalid": `value` must not be accepted;
// kind "equiv": `value` and `expected` (test_math_specified) must both be accepted and be written alike
// (within `approx` when it is given).
//
// This file reads the pinned pages and runs their scripts only to learn which values they test: it never
// reads another implementation's code.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = process.argv[2];
const outFile = process.argv[3];
if (!root || !outFile) {
  console.error('usage: extract-wpt-values.js WPT_ROOT OUT.json');
  process.exit(2);
}

const MATH_PROP = {
  number: 'scale', integer: 'z-index', length: 'flex-basis', angle: 'rotate', time: 'transition-delay',
  resolution: 'image-resolution', flex: 'grid-template-rows',
};

function files() {
  const out = [];
  const vals = path.join(root, 'css/css-values');
  for (const f of fs.readdirSync(vals).sort()) {
    if (f.endsWith('.html') && !f.endsWith('-ref.html')) out.push(path.join(vals, f));
  }
  const col = path.join(root, 'css/css-color/parsing');
  for (const f of fs.readdirSync(col).sort()) {
    if (f.endsWith('.html') && /valid|alpha-color-parsing/.test(f)) out.push(path.join(col, f));
  }
  return out;
}

const vectors = [];
let current = null;
let counter = 0;

function record(v) {
  vectors.push(Object.assign({file: current, n: counter++}, v));
}

function elementStub() {
  const style = new Proxy({}, {get: () => '', set: () => true});
  const el = new Proxy({style, id: 'target'}, {get: (t, k) => (k in t ? t[k] : () => elementStub()), set: () => true});
  return el;
}

function makeSandbox() {
  const doc = {
    getElementById: () => elementStub(), querySelector: () => elementStub(), createElement: () => elementStub(),
    querySelectorAll: () => [], body: elementStub(), documentElement: elementStub(),
  };
  const noop = () => {};
  const sb = {
    document: doc, window: {}, console: {log: noop, error: noop, warn: noop},
    CSS: {supports: () => true, registerProperty: noop}, getComputedStyle: () => new Proxy({}, {get: () => ''}),
    test: (fn) => { try { fn(); } catch (e) { /* page code that needs a browser */ } },
    promise_test: noop, async_test: noop, setup: noop, done: noop, add_completion_callback: noop,
    assert_equals: noop, assert_not_equals: noop, assert_true: noop, assert_false: noop, assert_array_equals: noop,
    assert_approx_equals: noop, assert_throws_js: noop, assert_in_array: noop, assert_array_approx_equals: noop,
    requestAnimationFrame: noop, setTimeout: noop, Exception: Error,
    test_valid_value(prop, value, expected) {
      let exp = arguments.length < 3 ? String(value) : expected;
      let alts = [];
      if (Array.isArray(exp)) { alts = exp.slice(1); exp = exp[0]; }
      record({kind: 'valid', prop, value: String(value), expected: exp, alts: alts.join('\u0001')});
    },
    test_invalid_value(prop, value) {
      record({kind: 'invalid', prop, value: String(value)});
    },
    test_specified_serialization(prop, t, e) {
      record({kind: 'valid', prop, value: String(t), expected: e});
    },
    test_math_specified(t, e, opts) {
      opts = opts || {};
      const type = opts.type === undefined ? 'length' : opts.type;
      const prop = opts.prop || MATH_PROP[type];
      if (!prop) return;
      record({kind: 'equiv', prop, value: String(t), expected: String(e), approx: opts.approx || 0});
    },
    fuzzy_test_valid_color(specified, serialized, epsilon) {
      if (!serialized) serialized = specified;
      record({kind: 'valid', prop: 'color', value: String(specified), expected: serialized, epsilon: epsilon || 0.01});
    },
  };
  // the numeric pages' own wrappers for invalid values go through test_invalid_value; the random-valued and
  // media-query helpers need a document and are not recorded
  sb.test_math_used = noop;
  sb.test_math_computed = noop;
  sb.test_computed_value = noop;
  sb.fuzzy_test_computed_color = noop;
  return sb;
}

for (const file of files()) {
  current = path.relative(root, file);
  counter = 0;
  const html = fs.readFileSync(file, 'utf8');
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g;
  let m;
  const sb = makeSandbox();
  const ctx = vm.createContext(sb);
  while ((m = re.exec(html))) {
    try {
      vm.runInContext(m[1], ctx, {timeout: 5000});
    } catch (e) {
      /* a script that needs the browser itself stops here; what it recorded before stays */
    }
  }
}

fs.writeFileSync(outFile, JSON.stringify(vectors, null, 0) + '\n');
console.log(`${vectors.length} vectors from ${files().length} pages`);
