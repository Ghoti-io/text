// The csstree reference for the CSS differential (one of three in this image).
//
//   node driver.mjs --version
//   node driver.mjs csstree < requests > answers
//
// The protocol is driver.py's: one request per line, `<mode> <options> <hex of the input>`, one
// answer per line. Modes:
//
//   tokens     csstree's tokenizer, nested by the brackets as CSS Syntax 5.4.9 nests them, in the
//              representation of css-parsing-tests (see driver.py);
//   toplevel   csstree's parser (positions on, no value, prelude or custom property parsing): the
//              top-level rules and at-rules of a stylesheet, each with its start and end as a
//              UTF-16 code unit offset (a JavaScript string's) and, for a rule, the names and
//              importance of the declarations in its block. csstree predates CSS Nesting: it reads a
//              nested rule as raw text, which is why only the top level is compared.
//
// Any other mode is {"unsupported": true}. An exception is an answer: {"crash": "..."}.
// The conversion is this file's, written from the representation's README; it reads no part of
// the reference but its documented API (tokenize, parse, the decoders in utils).
//
// Copyright 2026 by Corey Pennycuff

import * as csstree from 'css-tree';
import { readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';

const T = csstree.tokenTypes;
const NAMES = csstree.tokenNames;

if (process.argv[2] === '--version') {
  const pkg = JSON.parse(readFileSync(new URL('./node_modules/css-tree/package.json', import.meta.url), 'utf8'));
  console.log(`csstree ${pkg.version}`);
  process.exit(0);
}

// A number; an infinite one is written as a marker that ascii() turns into a JSON number (JSON.stringify writes null).
function num(text) {
  const v = Number(text);
  return v === Infinity ? '\u0001INF' : v === -Infinity ? '\u0001-INF' : v;
}

function isIdentStart(c) {
  return /[A-Za-z_\u0080-\u{10FFFF}]/u.test(c);
}

// "Check if three code points would start an ident sequence" for the code points of `s` (after the hash).
function wouldStartIdent(s) {
  const cps = Array.from(s);
  const [a, b, c] = cps;
  if (a === undefined) return false;
  if (a === '-') {
    if (b !== undefined && (isIdentStart(b) || b === '-')) return true;
    return b === '\\' && c !== undefined && c !== '\n';
  }
  if (isIdentStart(a)) return true;
  return a === '\\' && b !== undefined && b !== '\n';
}

const NUMBER = /^[+-]?(?:\d*\.\d+|\d+)(?:[eE][+-]?\d+)?/;

// Whether a string token's last character is its own closing quote and not an escaped one.
function stringIsClosed(text) {
  const quote = text[0];
  let i = 1;
  while (i < text.length) {
    if (text[i] === '\\') {
      i += 2;
    } else if (text[i] === quote) {
      return i === text.length - 1;
    } else {
      i += 1;
    }
  }
  return false;
}

function convert(css) {
  // Flat list of [kind, ...] items first, then the bracket structure.
  const flat = [];
  csstree.tokenize(css, (type, start, end) => {
    const text = css.slice(start, end);
    switch (type) {
      case T.WhiteSpace: flat.push({ k: 'v', v: ' ' }); break;
      case T.Comment: break;
      case T.Ident: flat.push({ k: 'v', v: ['ident', csstree.ident.decode(text)] }); break;
      case T.AtKeyword: flat.push({ k: 'v', v: ['at-keyword', csstree.ident.decode(text.slice(1))] }); break;
      case T.Hash: {
        const name = text.slice(1);
        flat.push({ k: 'v', v: ['hash', csstree.ident.decode(name), wouldStartIdent(name) ? 'id' : 'unrestricted'] });
        break;
      }
      case T.String: {
        // (the decoder takes the last character for the closing quote; a string the end of the input closed has none)
        const closed = stringIsClosed(text);
        flat.push({ k: 'v', v: ['string', csstree.string.decode(closed ? text : text + text[0])] });
        if (!closed) flat.push({ k: 'v', v: ['error', 'eof-in-string'] });
        break;
      }
      case T.BadString: flat.push({ k: 'v', v: ['error', 'bad-string'] }); break;
      case T.Url:
        flat.push({ k: 'v', v: ['url', csstree.url.decode(text)] });
        if (!text.endsWith(')')) flat.push({ k: 'v', v: ['error', 'eof-in-url'] });
        break;
      case T.BadUrl: flat.push({ k: 'v', v: ['error', 'bad-url'] }); break;
      case T.Delim: flat.push({ k: 'v', v: text }); break;
      case T.Number: {
        flat.push({ k: 'v', v: ['number', text, num(text), /^[+-]?\d+$/.test(text) ? 'integer' : 'number'] });
        break;
      }
      case T.Percentage: {
        const n = text.slice(0, -1);
        flat.push({ k: 'v', v: ['percentage', n, num(n), /^[+-]?\d+$/.test(n) ? 'integer' : 'number'] });
        break;
      }
      case T.Dimension: {
        const m = NUMBER.exec(text);
        const n = m[0];
        flat.push({ k: 'v', v: ['dimension', n, num(n), /^[+-]?\d+$/.test(n) ? 'integer' : 'number', csstree.ident.decode(text.slice(n.length))] });
        break;
      }
      case T.CDO: flat.push({ k: 'v', v: '<!--' }); break;
      case T.CDC: flat.push({ k: 'v', v: '-->' }); break;
      case T.Colon: flat.push({ k: 'v', v: ':' }); break;
      case T.Semicolon: flat.push({ k: 'v', v: ';' }); break;
      case T.Comma: flat.push({ k: 'v', v: ',' }); break;
      case T.Function: flat.push({ k: 'open', v: ['function', csstree.ident.decode(text.slice(0, -1))], close: ')' }); break;
      case T.LeftParenthesis: flat.push({ k: 'open', v: ['()'], close: ')' }); break;
      case T.LeftSquareBracket: flat.push({ k: 'open', v: ['[]'], close: ']' }); break;
      case T.LeftCurlyBracket: flat.push({ k: 'open', v: ['{}'], close: '}' }); break;
      case T.RightParenthesis: flat.push({ k: 'close', c: ')' }); break;
      case T.RightSquareBracket: flat.push({ k: 'close', c: ']' }); break;
      case T.RightCurlyBracket: flat.push({ k: 'close', c: '}' }); break;
      case T.EOF: break;
      default: flat.push({ k: 'v', v: ['unknown', NAMES[type]] });
    }
  });
  // An opening token is closed only by its mirror; any other closing token is an error token, in place.
  const root = [];
  const stack = [{ items: root, close: null }];
  for (const item of flat) {
    const top = stack[stack.length - 1];
    if (item.k === 'v') top.items.push(item.v);
    else if (item.k === 'open') {
      const node = item.v.slice();
      top.items.push(node);
      stack.push({ items: node, close: item.close, headLength: item.v.length });
    } else if (stack.length > 1 && top.close === item.c) stack.pop();
    else top.items.push(['error', item.c]);
  }
  return root;
}

function toplevel(css) {
  // A fresh syntax for each stylesheet: the module's own parser keeps state from the input before (found by running the
  // population: after one input, the next sometimes never returns and allocates until the process dies).
  const ast = csstree.fork({}).parse(css, {
    positions: true, parseValue: false, parseRulePrelude: false, parseAtrulePrelude: false, parseCustomProperty: false,
  });
  const out = [];
  for (const node of ast.children) {
    if (node.type === 'Rule') {
      const decls = [];
      for (const child of node.block.children) {
        if (child.type === 'Declaration') decls.push([child.property, child.important ? true : false]);
      }
      out.push(['rule', node.loc.start.offset, node.loc.end.offset, decls]);
    } else if (node.type === 'Atrule') {
      out.push(['at', node.name, node.loc.start.offset, node.loc.end.offset, node.block !== null]);
    }
  }
  return out;
}

// JSON.stringify leaves non-ASCII characters as they are; the protocol is ASCII.
function ascii(text) {
  text = text.replace(/"\\u0001INF"/g, '1e999').replace(/"\\u0001-INF"/g, '-1e999');
  return text.replace(/[\u007f-\uffff]/g, (c) => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
}

function unhex(token) {
  return token === '-' ? Buffer.alloc(0) : Buffer.from(token, 'hex');
}

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
const lines = [];
rl.on('line', (line) => lines.push(line));
rl.on('close', () => {
  const out = [];
  for (const line of lines) {
    const parts = line.split(' ');
    if (parts.length !== 3) {
      out.push(JSON.stringify({ crash: 'bad request' }));
      continue;
    }
    const [mode, , hexed] = parts;
    try {
      const css = unhex(hexed).toString('utf8');
      if (mode === 'tokens') out.push(ascii(JSON.stringify(convert(css))));
      else if (mode === 'toplevel') out.push(ascii(JSON.stringify(toplevel(css))));
      else out.push('{"unsupported": true}');
    } catch (e) {
      out.push(ascii(JSON.stringify({ crash: `${e.name}: ${String(e.message).slice(0, 200)}` })));
    }
  }
  process.stdout.write(out.join('\n') + '\n');
});
