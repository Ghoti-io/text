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

// The browser driver of the style sheet and selector differential (tools/oracle/css_sheet_diff.py): what the browsers say of an HTML document with `<style>` elements and
// shadow trees.
//
//   sheet_driver.mjs chromium|firefox      read documents on standard input, write one answer a line
//
// One document is a JSON object {"text": ..., "shadows": [[host id, html], ...], "queries": [[id, [property, ...]], ...], "selectors": [selector, ...]}: an HTML document
// in standards mode; for each shadow entry, an open shadow root attached to the element with that id whose `innerHTML` is set to the html (after the load); the properties
// to read of the element with each id (the document's, then the shadow trees'): `getComputedStyle(element).getPropertyValue(property)`; and for each selector the ids of the
// elements `document.querySelectorAll()` returns, or `SYNTAX` for a selector it throws on. One answer is a JSON object {"values": [[id, property, value], ...], "selectors":
// [[selector, "OK"|"SYNTAX", "id id ..."], ...]}, value being null when no element has the id. The first line of the output is `#` and a JSON object that names the
// engine, the browser's version and the image. The viewport is 1280 by 800 at a device pixel ratio of 1 and the color scheme is light.
//
// The browser is driven as tools/oracle/containers/cssprop/style_driver.mjs drives it (the same image, the same pins, WebDriver BiDi). Nothing is evaluated in the page but
// the function below, a constant of this file; the documents are data. A document that does not answer in time restarts the browser and is answered with the error "hang".

import { execFileSync, spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { createInterface } from 'node:readline';

const require = createRequire('/opt/cssprop/package.json');
const WebSocket = require('ws');

const TIMEOUT_MS = Number(process.env.CSSSTYLE_TIMEOUT_MS || 60000);

// A blank document in standards mode, to start the session in.
const PAGE = 'data:text/html,%3C!doctype%20html%3E%3Ctitle%3Et%3C%2Ftitle%3E';

// The function run in each document. A constant: the questions reach it as one JSON string.
const READ = `(json) => {
  const q = JSON.parse(json);
  const roots = [];
  for (const [hostId, html] of q.shadows) {
    // a host is in the document or in a shadow tree made before (a shadow tree inside a shadow tree)
    let host = document.getElementById(hostId);
    for (const r of roots) {
      host = host || r.getElementById(hostId);
    }
    if (host) {
      const root = host.attachShadow({ mode: 'open' });
      root.innerHTML = html;
      roots.push(root);
    }
  }
  const find = (id) => {
    let e = document.getElementById(id);
    for (const r of roots) {
      e = e || r.getElementById(id);
    }
    return e;
  };
  const values = [];
  for (const [id, properties] of q.queries) {
    const e = find(id);
    for (const p of properties) {
      if (!e) {
        values.push([id, p, null]);
        continue;
      }
      let v;
      try {
        v = getComputedStyle(e).getPropertyValue(p);
      } catch (x) {
        v = '!' + String(x);
      }
      values.push([id, p, v]);
    }
  }
  const selectors = [];
  for (const sel of q.selectors) {
    try {
      selectors.push([sel, 'OK', Array.from(document.querySelectorAll(sel)).map((e) => e.id || '?').join(' ')]);
    } catch (x) {
      selectors.push([sel, 'SYNTAX', '']);
    }
  }
  return JSON.stringify({ values, selectors });
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
  process.stderr.write(`csssheet-driver: ${message}\n`);
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
    const url = 'data:text/html;charset=utf-8,' + encodeURIComponent(doc.text);
    await this.send('browsingContext.navigate', { context: this.context, url, wait: 'complete' });
    const text = await this.call(READ, [{ type: 'string', value: JSON.stringify({ shadows: doc.shadows, queries: doc.queries, selectors: doc.selectors }) }]);
    return JSON.parse(text);
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
    fail('usage: sheet_driver.mjs chromium|firefox');
  }
  const scheme = mode === 'dark' ? 'dark' : 'light';
  let session = new Session(engine, scheme);
  await session.start();
  const header = { engine, browser: session.browserVersion, scheme, compat: session.info.compat, ua: session.info.ua, viewport: '1280x800@1', image: version() };
  process.stdout.write('#' + JSON.stringify(header) + '\n');
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
      answer = { values: doc.queries.flatMap(([id, props]) => props.map((p) => [id, p, '!hang: ' + String(e.message).slice(0, 80)])), selectors: doc.selectors.map((s) => [s, 'HANG', '']) };
    }
    process.stdout.write(JSON.stringify(answer) + '\n');
  }
  await session.stop();
}

main().catch((e) => fail(e.stack || String(e)));
