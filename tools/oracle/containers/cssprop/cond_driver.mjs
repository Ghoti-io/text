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

// The browser driver of the condition and rule differentials (tools/oracle/css_cond_diff.py): what the browsers say to `matchMedia()`, `CSS.supports()` and
// `CSSStyleSheet.insertRule()`.
//
//   cond_driver.mjs chromium|firefox      read documents on standard input, write one answer a line
//
// One document is a JSON object {"w": 1280, "h": 800, "dpr": 1, "q": [[kind, ...], ...]}: the viewport and the device pixel ratio the questions are asked in (put there by
// WebDriver BiDi `browsingContext.setViewport`), and the questions:
//   ["M", text]               matchMedia(text): ["1"|"0", media]
//   ["S", text]               CSS.supports(text): ["1"|"0"]
//   ["P", property, value]    CSS.supports(property, value): ["1"|"0"]
//   ["R", text]               `@supports <text> {}` inserted with insertRule: ["ok", conditionText, cssText] or ["reject"]
//   ["A", text]               insertRule(text) in an empty constructed style sheet: ["ok", cssText] or ["reject"]
// An exception is answered ["T:" + its name]. The first line of the output is `#` and a JSON object that names the engine, the browser's version and the image, and the
// environment the page measured at the start (the features this driver cannot put elsewhere: the pointer, the preferences, the color depth, the screen), which this library is
// given. Nothing is evaluated in the page but the function below, a constant of this file; the documents are data. A document that does not answer in time restarts the
// browser and is answered with the error "hang".

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
  const out = [];
  for (const q of JSON.parse(json)) {
    let r;
    try {
      if (q[0] === 'M') {
        const m = matchMedia(q[1]);
        r = [m.matches ? '1' : '0', m.media];
      } else if (q[0] === 'S') {
        r = [CSS.supports(q[1]) ? '1' : '0'];
      } else if (q[0] === 'P') {
        r = [CSS.supports(q[1], q[2]) ? '1' : '0'];
      } else if (q[0] === 'R' || q[0] === 'A') {
        const sheet = new CSSStyleSheet();
        try {
          sheet.insertRule(q[0] === 'R' ? '@supports ' + q[1] + ' {}' : q[1]);
          const rule = sheet.cssRules[0];
          r = q[0] === 'R' ? ['ok', rule.conditionText, rule.cssText] : ['ok', rule.cssText];
        } catch (x) {
          r = ['reject'];
        }
      } else {
        r = ['?'];
      }
    } catch (x) {
      r = ['T:' + x.name];
    }
    out.push(r);
  }
  return JSON.stringify(out);
}`;

// What the page says about itself at the start: the features the driver cannot put elsewhere, read the way a page reads them.
const PROBE = `() => {
  const e = { w: innerWidth, h: innerHeight, dpr: devicePixelRatio, 'device-width': screen.width, 'device-height': screen.height, ua: navigator.userAgent };
  const disc = { pointer: 'none coarse fine', hover: 'none hover', 'prefers-color-scheme': 'light dark', 'prefers-reduced-motion': 'no-preference reduce',
    'prefers-contrast': 'no-preference less more custom', 'color-gamut': 'srgb p3 rec2020', 'dynamic-range': 'standard high', scan: 'interlace progressive',
    update: 'none slow fast', 'overflow-block': 'none scroll paged', 'overflow-inline': 'none scroll', 'display-mode': 'fullscreen standalone minimal-ui browser picture-in-picture',
    'forced-colors': 'none active', 'inverted-colors': 'none inverted', scripting: 'none initial-only enabled', 'prefers-reduced-transparency': 'no-preference reduce',
    'prefers-reduced-data': 'no-preference reduce' };
  for (const [f, v] of Object.entries(disc)) {
    e[f] = v.split(' ').filter((k) => matchMedia('(' + f + ':' + k + ')').matches);
  }
  e['any-pointer'] = ['none', 'coarse', 'fine'].filter((k) => matchMedia('(any-pointer:' + k + ')').matches);
  e['any-hover'] = ['none', 'hover'].filter((k) => matchMedia('(any-hover:' + k + ')').matches);
  e.color = []; for (let i = 0; i <= 16; i++) { if (matchMedia('(color:' + i + ')').matches) { e.color.push(i); } }
  e['transform-3d'] = matchMedia('(-webkit-transform-3d)').matches ? 1 : 0;
  return JSON.stringify(e);
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
  process.stderr.write(`csscond-driver: ${message}\n`);
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
    this.info = JSON.parse(await this.call(PROBE, []));
  }

  async viewport(w, h, dpr) {
    await this.send('browsingContext.setViewport', { context: this.context, viewport: { width: w, height: h }, devicePixelRatio: dpr });
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
    await this.viewport(doc.w, doc.h, doc.dpr);
    const text = await this.call(READ, [{ type: 'string', value: JSON.stringify(doc.q) }]);
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
    fail('usage: cond_driver.mjs chromium|firefox');
  }
  const scheme = mode === 'dark' ? 'dark' : 'light';
  let session = new Session(engine, scheme);
  await session.start();
  const info = session.info;
  const header = { engine, browser: session.browserVersion, scheme, ua: info.ua, env: info, image: version() };
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
      answer = doc.q.map(() => ['T:hang']);
    }
    process.stdout.write(JSON.stringify(answer) + '\n');
  }
  await session.stop();
}

main().catch((e) => fail(e.stack || String(e)));
