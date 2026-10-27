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

// A probe of how the browsers cascade a few documents (the questions where the cascade's text could be read two ways): what Chromium and Firefox compute for
// each case of tools/oracle/cascade_probe_cases.json, for comparison with what css_cascade.h answers (tests/test-css-cascade-fixes.cpp quotes the result).
//
//   cascade_probe.mjs chromium|firefox < cases.json    one line a case: name, then the browser's answer
//
// A case is {name, html, expr}: `html` is a document (a data: URL, in standards mode; a shadow root is declared with <template shadowrootmode>) and `expr`
// a JavaScript expression run in it whose value is the answer. The browser is driven as tools/oracle/containers/cssprop/driver.mjs drives it, through WebDriver
// BiDi, in the same image (the module `ws` is the image's). The cases are this repository's files, not input from anywhere else.

import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { WebSocket } from 'ws';

const TIMEOUT_MS = 60000;

const ENGINES = {
  chromium: {
    driver: ['/opt/chromedriver/chromedriver', ['--port=9515', '--allowed-ips=127.0.0.1']],
    port: 9515,
    capabilities: {
      browserName: 'chrome',
      webSocketUrl: true,
      'goog:chromeOptions': {
        binary: '/opt/chrome/chrome',
        args: ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run', '--disable-extensions',
          '--disable-background-networking', '--disable-component-update', '--disable-sync', '--no-proxy-server', '--host-resolver-rules=MAP * ~NOTFOUND',
          '--mute-audio'],
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

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function http(port, method, path, body) {
  const response = await fetch(`http://127.0.0.1:${port}${path}`, {
    method,
    headers: { 'content-type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  const json = JSON.parse(await response.text());
  if (!response.ok) {
    throw new Error(`${method} ${path}: ${response.status} ${JSON.stringify(json).slice(0, 300)}`);
  }
  return json.value;
}

async function main() {
  const engine = process.argv[2];
  const spec = ENGINES[engine];
  if (!spec) {
    process.stderr.write('usage: cascade_probe.mjs chromium|firefox < cases.json\n');
    process.exit(2);
  }
  const cases = JSON.parse(readFileSync(0, 'utf8'));
  const [binary, args] = spec.driver;
  const child = spawn(binary, args, { stdio: ['ignore', 'ignore', 'ignore'] });
  let ready = false;
  for (let i = 0; i < 100 && !ready; i++) {
    try {
      await http(spec.port, 'GET', '/status');
      ready = true;
    } catch {
      await sleep(100);
    }
  }
  if (!ready) {
    process.stderr.write(`${binary} did not answer\n`);
    process.exit(2);
  }
  const value = await http(spec.port, 'POST', '/session', { capabilities: { alwaysMatch: spec.capabilities } });
  const ws = new WebSocket(value.capabilities.webSocketUrl);
  await new Promise((resolve, reject) => {
    ws.once('open', resolve);
    ws.once('error', reject);
  });
  const pending = new Map();
  let next = 1;
  ws.on('message', (data) => {
    const message = JSON.parse(data.toString());
    const waiter = pending.get(message.id);
    if (waiter) {
      pending.delete(message.id);
      waiter(message);
    }
  });
  const send = (method, params) => new Promise((resolve, reject) => {
    const id = next++;
    const timer = setTimeout(() => reject(new Error(`${method}: no answer`)), TIMEOUT_MS);
    pending.set(id, (m) => {
      clearTimeout(timer);
      return m.type === 'error' ? reject(new Error(`${method}: ${m.error}: ${m.message}`)) : resolve(m.result);
    });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const tree = await send('browsingContext.getTree', {});
  const context = tree.contexts[0].context;
  process.stdout.write(`# ${engine} ${value.capabilities.browserVersion}\n`);
  for (const c of cases) {
    const url = 'data:text/html;charset=utf-8,' + encodeURIComponent('<!doctype html><title>t</title>' + c.html);
    let answer;
    try {
      await send('browsingContext.navigate', { context, url, wait: 'complete' });
      const result = await send('script.callFunction', {
        functionDeclaration: `() => String(${c.expr})`,
        arguments: [],
        awaitPromise: false,
        target: { context },
        resultOwnership: 'none',
      });
      answer = result.type === 'exception' ? 'exception ' + JSON.stringify(result.exceptionDetails).slice(0, 120) : result.result.value;
    } catch (e) {
      answer = 'error ' + String(e.message).slice(0, 120);
    }
    process.stdout.write(`${c.name}\t${answer}\n`);
  }
  try {
    ws.close();
    await http(spec.port, 'DELETE', `/session/${value.sessionId}`);
  } catch { /* the browser may be gone */ }
  child.kill('SIGKILL');
}

main().then(() => process.exit(0), (e) => {
  process.stderr.write(`cascade_probe: ${e.message}\n`);
  process.exit(2);
});
