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

// The browser driver of the CSS property oracle (tools/oracle/css_prop_diff.py).
//
//   driver.mjs chromium|firefox    read items on standard input, write one answer a line
//   driver.mjs --version           what this image is made of
//
// One item is a JSON array [property, value]; one answer is a JSON array [accepted, value, cssText, supports, error], where
//
//   accepted   1 when `element.style.setProperty(property, value)` left a declaration in a fresh element's style, else 0;
//   value      `element.style.getPropertyValue(property)` afterwards (empty for a shorthand the browser cannot write back as one);
//   cssText    the element's `style.cssText` afterwards;
//   supports   1 when `CSS.supports(property, value)`, else 0;
//   error      the text of an exception either call threw, else "".
//
// The first line of the output is `#` and a JSON object that names the engine, the browser's own version, the document's
// compatMode (the document is a standards-mode one: the page is a data: URL beginning `<!doctype html>`; anything but "CSS1Compat"
// stops the run) and the user agent. The browser is driven through WebDriver BiDi: the classic endpoint of the vendor's driver
// (chromedriver, geckodriver) creates the session with `webSocketUrl`, and everything after that is BiDi commands on the socket
// (`browsingContext.getTree`, `browsingContext.navigate`, `script.callFunction`). Nothing is evaluated in the page but the
// function below, which is a constant of this file; the items are data, passed as one string argument.
//
// The driver reads no network (the container runs with `--network none`; the loopback sockets of the vendor's driver are all it
// needs) and runs one browser at a time. A batch that does not answer in time restarts the browser and is split until the item that
// hangs it is alone; that item is answered with the error "hang" and the browser is restarted again.

import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import { WebSocket } from 'ws';

const BATCH = Number(process.env.CSSPROP_BATCH || 500);
const TIMEOUT_MS = Number(process.env.CSSPROP_TIMEOUT_MS || 60000);

const PAGE = 'data:text/html,%3C!doctype%20html%3E%3Ctitle%3Et%3C%2Ftitle%3E%3Cdiv%20id%3Dt%3E%3C%2Fdiv%3E';

// The function run in the page. A constant: the items reach it as one JSON string.
const MEASURE = `(json) => {
  const items = JSON.parse(json);
  const out = [];
  for (const [property, value] of items) {
    const e = document.createElement('div');
    let accepted = 0, got = '', css = '', supports = 0, error = '';
    try {
      e.style.setProperty(property, value);
      accepted = e.style.length > 0 ? 1 : 0;
      got = e.style.getPropertyValue(property);
      css = e.style.cssText;
    } catch (x) {
      error = String(x);
    }
    try {
      supports = CSS.supports(property, value) ? 1 : 0;
    } catch (x) {
      error += ' supports: ' + String(x);
    }
    out.push([accepted, got, css, supports, error]);
  }
  return JSON.stringify(out);
}`;

const ENGINES = {
  chromium: {
    driver: ['/opt/chromedriver/chromedriver', ['--port=9515', '--allowed-ips=127.0.0.1']],
    port: 9515,
    capabilities: {
      browserName: 'chrome',
      webSocketUrl: true,
      'goog:chromeOptions': {
        binary: '/opt/chrome/chrome',
        args: ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run',
          '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--disable-sync',
          '--no-proxy-server', '--host-resolver-rules=MAP * ~NOTFOUND', '--mute-audio'],
      },
    },
  },
  firefox: {
    driver: ['/opt/geckodriver/geckodriver', ['--port=4444', '--host=127.0.0.1', '--binary=/opt/firefox/firefox']],
    port: 4444,
    capabilities: {
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
        },
      },
    },
  },
};

function fail(message) {
  process.stderr.write(`cssprop-driver: ${message}\n`);
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
  constructor(engine) {
    this.engine = engine;
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
    const value = await http(this.spec.port, 'POST', '/session', { capabilities: { alwaysMatch: this.spec.capabilities } });
    this.id = value.sessionId;
    const url = value.capabilities.webSocketUrl;
    if (!url) {
      fail('the session has no BiDi socket (webSocketUrl)');
    }
    this.browserVersion = value.capabilities.browserVersion;
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
    await this.send('browsingContext.navigate', { context: this.context, url: PAGE, wait: 'complete' });
    const fingerprint = await this.call(`() => JSON.stringify({ compat: document.compatMode, ua: navigator.userAgent })`, []);
    this.info = JSON.parse(fingerprint);
    if (this.info.compat !== 'CSS1Compat') {
      fail(`the document is not in standards mode (compatMode ${this.info.compat})`);
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

  async measure(items) {
    const text = await this.call(MEASURE, [{ type: 'string', value: JSON.stringify(items) }]);
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

function version() {
  // What the image was built from, written by the Dockerfile; the gate (css_prop_ref.py) recomputes the driver's hash from the
  // repository's files and compares it with the one printed here, so an image holding an older copy of this file is seen.
  const lines = readFileSync('/opt/cssprop/VERSIONS', 'utf8').trim();
  const pinned = readFileSync('/opt/cssprop/DRIVER_SHA', 'utf8').trim();
  return `${lines}; driver ${pinned}`;
}

async function main() {
  const arg = process.argv[2];
  if (arg === '--version') {
    process.stdout.write(version() + '\n');
    return;
  }
  if (!ENGINES[arg]) {
    fail('usage: driver.mjs chromium|firefox|--version');
  }
  const items = [];
  const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
  for await (const line of rl) {
    if (line.length) {
      items.push(JSON.parse(line));
    }
  }

  let session = new Session(arg);
  await session.start();
  process.stdout.write('#' + JSON.stringify({ engine: arg, browser: session.browserVersion, compat: session.info.compat, ua: session.info.ua }) + '\n');

  // Answer a run of items; on a failure restart the browser and split, down to the item that hangs it.
  async function answer(run) {
    try {
      return await session.measure(run);
    } catch (e) {
      await session.stop();
      session = new Session(arg);
      await session.start();
      if (run.length === 1) {
        return [[0, '', '', 0, `hang: ${String(e.message).slice(0, 120)}`]];
      }
      const half = run.length >> 1;
      return (await answer(run.slice(0, half))).concat(await answer(run.slice(half)));
    }
  }

  for (let i = 0; i < items.length; i += BATCH) {
    const out = await answer(items.slice(i, i + BATCH));
    process.stdout.write(out.map((row) => JSON.stringify(row)).join('\n') + '\n');
  }
  await session.stop();
}

main().catch((e) => fail(e.stack || String(e)));
