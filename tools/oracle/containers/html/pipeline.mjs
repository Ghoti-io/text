// parse5 for the whole-pipeline differential (tools/oracle/html_pipeline_diff.py); pipeline.py describes the protocol.
//
//   node pipeline.mjs           one JSON request per line on stdin, one JSON answer per line on stdout
//
// parse5 reads strings only, so there is one operation: {"op": "string", "hex": <UTF-8 of the text>, "context": <"td" | "svg path" |
// "math mi" | null>, "scripting": <bool>}, answered with {"dump", "errors", "ser"}: the tree in the tree-construction dump format
// (driver.mjs's, copied here and checked against it), and parse5.serialize() of it. Read from the tree on the bind mount.
//
// Copyright 2026 by Corey Pennycuff
import { createInterface } from 'node:readline';

const parse5 = await import('file:///opt/html-oracle/node_modules/parse5/dist/index.js');
const { parse, parseFragment, serialize, defaultTreeAdapter } = parse5;

const NS_HTML = 'http://www.w3.org/1999/xhtml';
const NS_SVG = 'http://www.w3.org/2000/svg';
const NS_MATH = 'http://www.w3.org/1998/Math/MathML';

// The dump format's order for attribute labels: UTF-16 code units, which is what a JavaScript string comparison is (the pinned driver sorts by
// code point, which differs only for a label with a character above U+FFFF).
function cmp(a, b) {
  return a < b ? -1 : a > b ? 1 : 0;
}

function dump(root) {
  const lines = [];
  const emit = (d, s) => lines.push('| ' + '  '.repeat(d) + s);
  const stack = [[root, 0]];
  while (stack.length) {
    const [node, depth] = stack.pop();
    const kids = [];
    switch (node.nodeName) {
      case '#document':
      case '#document-fragment':
        for (const c of node.childNodes) kids.push([c, depth]);
        break;
      case '#text':
        emit(depth, '"' + node.value + '"');
        continue;
      case '#comment':
        emit(depth, '<!-- ' + node.data + ' -->');
        continue;
      case '#documentType':
        if (node.publicId || node.systemId) {
          emit(depth, `<!DOCTYPE ${node.name} "${node.publicId || ''}" "${node.systemId || ''}">`);
        } else {
          emit(depth, `<!DOCTYPE ${node.name}>`);
        }
        continue;
      default: {
        const ns = node.namespaceURI;
        const shown = ns === NS_HTML ? node.tagName : (ns === NS_SVG ? 'svg ' : ns === NS_MATH ? 'math ' : '? ') + node.tagName;
        emit(depth, `<${shown}>`);
        const attrs = node.attrs.map((a) => [a.prefix ? a.prefix + ' ' + a.name : a.name, a.value]);
        attrs.sort((a, b) => cmp(a[0], b[0]));
        for (const [label, value] of attrs) emit(depth + 1, `${label}="${value}"`);
        if (ns === NS_HTML && node.tagName === 'template') {
          emit(depth + 1, 'content');
          for (const c of node.content.childNodes) kids.push([c, depth + 2]);
        } else {
          for (const c of node.childNodes) kids.push([c, depth + 1]);
        }
      }
    }
    for (let i = kids.length - 1; i >= 0; i--) stack.push(kids[i]);
  }
  return lines.join('\n');
}

function ask(req) {
  let errors = 0;
  const options = { scriptingEnabled: !!req.scripting, onParseError: () => { errors++; } };
  const text = Buffer.from(req.hex, 'hex').toString('utf8');
  let root;
  if (req.context === null || req.context === undefined) {
    root = parse(text, options);
  } else {
    const words = req.context.split(' ');
    let ns = NS_HTML;
    let name = words[0];
    if (words.length === 2) {
      ns = words[0] === 'svg' ? NS_SVG : words[0] === 'math' ? NS_MATH : null;
      if (ns === null) throw new Error('unknown context namespace ' + words[0]);
      name = words[1];
    }
    const ctx = defaultTreeAdapter.createElement(name, ns, []);
    root = parseFragment(ctx, text, options);
  }
  return { dump: dump(root), errors, ser: serialize(root, { scriptingEnabled: !!req.scripting }) };
}

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
const out = [];
for await (const line of rl) {
  if (!line.trim()) continue;
  let answer;
  try {
    answer = ask(JSON.parse(line));
  } catch (e) {
    answer = { error: `${e && e.name}: ${e && e.message}` };
  }
  out.push(JSON.stringify(answer).replace(/[\u007f-￿]/g, (c) => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0')));
  if (out.length >= 1000) { process.stdout.write(out.join('\n') + '\n'); out.length = 0; }
}
if (out.length) process.stdout.write(out.join('\n') + '\n');
