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

// The browser driver of the computed-style differential (tools/oracle/css_style_diff.py): what `getComputedStyle()` says of the elements of a document.
//
//   style_driver.mjs chromium|firefox [light|dark]     read documents on standard input, write one answer a line
//   style_driver.mjs chromium|firefox metrics          write the font metrics table of the metrics the differential feeds this library
//
// One document is a JSON object {"mode": "h"|"x", "text": ..., "queries": [[id, [property, ...]], ...]}: an HTML document or an XML (SVG) one, and for
// each element with that id the properties to read. One answer is a JSON array of [id, property, value], value being null when the document has no such
// element, and `getComputedStyle(element).getPropertyValue(property)` otherwise (a custom property included), after the document's load. The viewport is
// 1280 by 800 at a device pixel ratio of 1, the default font size is the browser's own (16px), and the color scheme is the one named: the first line of
// the output is `#` and a JSON object that names the engine, the browser's version, the scheme and the image.
//
// The browser is driven as tools/oracle/containers/cssprop/driver.mjs drives it (the same image, the same pins, WebDriver BiDi), and the module `ws` is
// the image's. Nothing is evaluated in the page but the two functions below, which are constants of this file; the documents are data. The driver reads no
// network and runs one browser at a time. A document that does not answer in time restarts the browser and is answered with the error "hang".

import { execFileSync, spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { createInterface } from 'node:readline';

const require = createRequire('/opt/cssprop/package.json');
const WebSocket = require('ws');

const TIMEOUT_MS = Number(process.env.CSSSTYLE_TIMEOUT_MS || 60000);

// A blank document in standards mode, to start the session in.
const PAGE = 'data:text/html,%3C!doctype%20html%3E%3Ctitle%3Et%3C%2Ftitle%3E';

// The function run in each document. A constant: the queries reach it as one JSON string.
const READ = `(json) => {
  const queries = JSON.parse(json);
  const out = [];
  for (const [id, properties] of queries) {
    const e = document.getElementById(id);
    for (const p of properties) {
      if (!e) {
        out.push([id, p, null]);
        continue;
      }
      let v;
      try {
        v = getComputedStyle(e).getPropertyValue(p);
      } catch (x) {
        v = '!' + String(x);
      }
      out.push([id, p, v]);
    }
  }
  return JSON.stringify(out);
}`;

// The metrics: the width of 1ex, 1ch, 1cap and 1ic in a font, measured on a rendered element.
const METRICS = `(json) => {
  const rows = JSON.parse(json);
  const out = [];
  const host = document.body;
  for (const [family, weight, italic, size] of rows) {
    const e = document.createElement('div');
    e.style.cssText = 'position:absolute;left:0;top:0;white-space:pre;font-family:' + family + ';font-weight:' + weight + ';font-style:' + (italic ? 'italic' : 'normal') + ';font-size:' + size + 'px';
    host.appendChild(e);
    const w = [];
    for (const unit of ['ex', 'ch', 'cap', 'ic']) {
      const p = document.createElement('div');
      p.style.cssText = 'display:block;position:absolute;left:0;top:0;width:1' + unit;
      e.appendChild(p);
      w.push(p.getBoundingClientRect().width);
      e.removeChild(p);
    }
    host.removeChild(e);
    out.push([family, weight, italic ? 1 : 0, size].concat(w));
  }
  return JSON.stringify(out);
}`;

const ENGINES = {
  chromium: {
    driver: ['/opt/chromedriver/chromedriver', ['--port=9515', '--allowed-ips=127.0.0.1']],
    port: 9515,
    capabilities: (scheme) => ({
      browserName: 'chrome',
      webSocketUrl: true,
      'goog:chromeOptions': {
        binary: '/opt/chrome/chrome',
        args: ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run',
          '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--disable-sync',
          '--no-proxy-server', '--host-resolver-rules=MAP * ~NOTFOUND', '--mute-audio'],
      },
    }),
  },
  firefox: {
    driver: ['/opt/geckodriver/geckodriver', ['--port=4444', '--host=127.0.0.1', '--binary=/opt/firefox/firefox']],
    port: 4444,
    capabilities: (scheme) => ({
      browserName: 'firefox',
      webSocketUrl: true,
      'moz:firefoxOptions': {
        binary: '/opt/firefox/firefox',
        args: ['-headless'],
        prefs: {
          'app.update.auto': false,
          'app.update.enabled': false,
          'datareporting.policy.dataSubmissionEnabled': false,
          'browser.shell.checkDefaultBrowser': false,
          'network.proxy.type': 1,
          'network.proxy.http': '127.0.0.1',
          'network.proxy.http_port': 9,
          'network.proxy.ssl': '127.0.0.1',
          'network.proxy.ssl_port': 9,
          'remote.log.level': 'Fatal',
          'layout.css.prefers-color-scheme.content-override': scheme === 'dark' ? 0 : 1,
        },
      },
    }),
  },
};

function fail(message) {
  process.stderr.write(`cssstyle-driver: ${message}\n`);
  process.exit(2);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function http(port, method, path, body) {
  const response = await fetch(`http://127.0.0.1:${port}${path}`, {
    method,
    headers: { 'content-type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  const text = await response.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {
    throw new Error(`${method} ${path}: ${response.status} ${text.slice(0, 200)}`);
  }
  if (!response.ok) {
    throw new Error(`${method} ${path}: ${response.status} ${JSON.stringify(json).slice(0, 300)}`);
  }
  return json.value;
}

class Session {
  constructor(engine, scheme) {
    this.engine = engine;
    this.scheme = scheme;
    this.spec = ENGINES[engine];
    this.child = null;
    this.ws = null;
    this.id = null;
    this.next = 1;
    this.pending = new Map();
    this.context = null;
    this.info = null;
  }

  async start() {
    const [binary, args] = this.spec.driver;
    this.child = spawn(binary, args, { stdio: ['ignore', 'ignore', 'ignore'] });
    this.child.on('error', (e) => fail(`${binary}: ${e.message}`));
    let ready = false;
    for (let i = 0; i < 100 && !ready; i++) {
      try {
        await http(this.spec.port, 'GET', '/status');
        ready = true;
      } catch {
        await sleep(100);
      }
    }
    if (!ready) {
      fail(`${binary} did not answer on port ${this.spec.port}`);
    }
    const value = await http(this.spec.port, 'POST', '/session', { capabilities: { alwaysMatch: this.spec.capabilities(this.scheme) } });
    this.id = value.sessionId;
    const url = value.capabilities.webSocketUrl;
    if (!url) {
      fail('the session has no BiDi socket (webSocketUrl)');
    }
    this.browserVersion = value.capabilities.browserVersion;
    if (this.engine === 'chromium' && this.scheme === 'dark') {
      // Chromium has no preference for the color scheme of a headless browser; the DevTools protocol, through the vendor driver's own endpoint, emulates it
      await http(this.spec.port, 'POST', `/session/${this.id}/goog/cdp/execute`, {
        cmd: 'Emulation.setEmulatedMedia',
        params: { features: [{ name: 'prefers-color-scheme', value: 'dark' }] },
      });
    }
    this.ws = new WebSocket(url);
    await new Promise((resolve, reject) => {
      this.ws.once('open', resolve);
      this.ws.once('error', reject);
    });
    this.ws.on('message', (data) => {
      let message;
      try {
        message = JSON.parse(data.toString());
      } catch {
        return;
      }
      const waiter = this.pending.get(message.id);
      if (waiter) {
        this.pending.delete(message.id);
        waiter(message);
      }
    });
    const tree = await this.send('browsingContext.getTree', {});
    this.context = tree.contexts[0].context;
    await this.send('browsingContext.setViewport', { context: this.context, viewport: { width: 1280, height: 800 }, devicePixelRatio: 1 });
    await this.send('browsingContext.navigate', { context: this.context, url: PAGE, wait: 'complete' });
    this.info = JSON.parse(await this.call(`() => JSON.stringify({ compat: document.compatMode, ua: navigator.userAgent, w: innerWidth, h: innerHeight, dpr: devicePixelRatio, dark: matchMedia('(prefers-color-scheme: dark)').matches })`, []));
    if (this.info.compat !== 'CSS1Compat') {
      fail(`the document is not in standards mode (compatMode ${this.info.compat})`);
    }
    if (this.info.w !== 1280 || this.info.h !== 800 || this.info.dpr !== 1) {
      fail(`the viewport is ${this.info.w}x${this.info.h} at ${this.info.dpr}, not 1280x800 at 1`);
    }
    if (this.info.dark !== (this.scheme === 'dark')) {
      fail(`the color scheme the browser reports is not ${this.scheme}`);
    }
  }

  send(method, params) {
    const id = this.next++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method}: no answer in ${TIMEOUT_MS} ms`));
      }, TIMEOUT_MS);
      this.pending.set(id, (message) => {
        clearTimeout(timer);
        if (message.type === 'error') {
          reject(new Error(`${method}: ${message.error}: ${message.message}`));
        } else {
          resolve(message.result);
        }
      });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }

  async call(declaration, args) {
    const result = await this.send('script.callFunction', {
      functionDeclaration: declaration,
      arguments: args,
      awaitPromise: false,
      target: { context: this.context },
      resultOwnership: 'none',
    });
    if (result.type === 'exception') {
      throw new Error(`script: ${JSON.stringify(result.exceptionDetails).slice(0, 300)}`);
    }
    return result.result.value;
  }

  async read(doc) {
    const url = (doc.mode === 'x' ? 'data:image/svg+xml;charset=utf-8,' : 'data:text/html;charset=utf-8,') + encodeURIComponent(doc.text);
    await this.send('browsingContext.navigate', { context: this.context, url, wait: 'complete' });
    const text = await this.call(READ, [{ type: 'string', value: JSON.stringify(doc.queries) }]);
    return JSON.parse(text);
  }

  async metrics(rows) {
    await this.send('browsingContext.navigate', { context: this.context, url: PAGE, wait: 'complete' });
    return JSON.parse(await this.call(METRICS, [{ type: 'string', value: JSON.stringify(rows) }]));
  }

  async stop() {
    try {
      this.ws && this.ws.close();
    } catch { /* closing */ }
    try {
      this.id && await http(this.spec.port, 'DELETE', `/session/${this.id}`);
    } catch { /* the browser may already be gone */ }
    if (this.child) {
      this.child.kill('SIGKILL');
      await new Promise((resolve) => this.child.once('exit', resolve));
    }
  }
}

function measured() {
  const run = (binary, re) => {
    const text = execFileSync(binary, ['--version'], { encoding: 'utf8', timeout: 30000, stdio: ['ignore', 'pipe', 'ignore'] });
    const m = re.exec(text);
    if (!m) {
      fail(`${binary} --version said ${JSON.stringify(text.slice(0, 80))}`);
    }
    return m[1];
  };
  const wsVersion = JSON.parse(readFileSync('/opt/cssprop/node_modules/ws/package.json', 'utf8')).version;
  const pins = readFileSync('/opt/cssprop/PINS', 'utf8').trim();
  return [
    `chrome ${run('/opt/chrome/chrome', /^Google Chrome for Testing (\d+(?:\.\d+)+)/)}`,
    `chromedriver ${run('/opt/chromedriver/chromedriver', /^ChromeDriver (\d+(?:\.\d+)+)/)}`,
    `firefox ${run('/opt/firefox/firefox', /^Mozilla Firefox (\d+(?:\.\d+)*)/)}`,
    `geckodriver ${run('/opt/geckodriver/geckodriver', /^geckodriver (\d+(?:\.\d+)+)/)}`,
    `ws ${wsVersion}`,
    `sha256 ${pins}`,
  ].join(', ');
}

function version() {
  const recorded = readFileSync('/opt/cssprop/VERSIONS', 'utf8').trim();
  const pinned = readFileSync('/opt/cssprop/DRIVER_SHA', 'utf8').trim();
  return `${measured()}, ${recorded}; driver ${pinned}`;
}

async function main() {
  const engine = process.argv[2];
  const mode = process.argv[3] || 'light';
  if (!ENGINES[engine]) {
    fail('usage: style_driver.mjs chromium|firefox [light|dark|metrics]');
  }
  const scheme = mode === 'dark' ? 'dark' : 'light';
  let session = new Session(engine, scheme);
  await session.start();
  const header = { engine, browser: session.browserVersion, scheme, compat: session.info.compat, ua: session.info.ua, viewport: '1280x800@1', image: version() };
  process.stdout.write('#' + JSON.stringify(header) + '\n');
  if (mode === 'metrics') {
    const rows = [];
    for (const family of ['serif', 'sans-serif', 'monospace']) {
      for (const weight of [400, 700]) {
        for (const italic of [0, 1]) {
          for (const size of [10, 12, 16, 20, 32]) {
            rows.push([family, weight, italic, size]);
          }
        }
      }
    }
    for (const r of await session.metrics(rows)) {
      process.stdout.write(r.join('\t') + '\n');
    }
    await session.stop();
    return;
  }
  const docs = [];
  const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
  for await (const line of rl) {
    if (line.length) {
      docs.push(JSON.parse(line));
    }
  }
  for (const doc of docs) {
    let answer;
    try {
      answer = await session.read(doc);
    } catch (e) {
      await session.stop();
      session = new Session(engine, scheme);
      await session.start();
      answer = doc.queries.flatMap(([id, props]) => props.map((p) => [id, p, '!hang: ' + String(e.message).slice(0, 80)]));
    }
    process.stdout.write(JSON.stringify(answer) + '\n');
  }
  await session.stop();
}

main().catch((e) => fail(e.stack || String(e)));
