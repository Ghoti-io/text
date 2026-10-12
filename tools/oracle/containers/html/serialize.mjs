// parse5's serializer, asked about a tree (the serializer differential, tools/oracle/html_serialize_diff.py).
//
//   node serialize.mjs           read requests on stdin, answer on stdout
//
// The protocol is serialize.py's: a line of JSON {"tree": <node>, "mode": "children" | "outer", "scripting": <bool>} and an
// answer {"out": "<html>"} or {"error": "<why>"}; see that file for the tree. The tree is built with parse5's default tree
// adapter (a template element gets a content fragment, which is where its children go when the tree says so and is empty
// when it does not), and written with parse5.serialize (the root's children) or parse5.serializeOuter (the root's first
// child), `scriptingEnabled` as asked. A processing instruction is not a node of that tree and is skipped, and a CDATA
// section is a text node. Adjacent text nodes are one, which the adapter does and the output does not show.
//
// parse5 is imported by its absolute path in the image, so that this file can be read from the tree (bind mounted at its
// own path) instead of being baked into the image.
//
// Copyright 2026 by Corey Pennycuff
import { createInterface } from 'node:readline';

const parse5 = await import('file:///opt/html-oracle/node_modules/parse5/dist/index.js');
const A = parse5.defaultTreeAdapter;
const NS_HTML = 'http://www.w3.org/1999/xhtml';
const NS_SVG = 'http://www.w3.org/2000/svg';
const NS_MATH = 'http://www.w3.org/1998/Math/MathML';

function fill(parent, nodes, doc) {
    for (const node of nodes) {
        switch (node.k) {
            case 'e': {
                const attrs = node.a.map((a) => {
                    const at = { name: a.n, value: a.v };
                    if (a.ns) {
                        at.namespace = a.ns;
                    }
                    if (a.p) {
                        at.prefix = a.p;
                    }
                    return at;
                });
                // An element in no namespace has to have one for the adapter; one that is not HTML's is as much
                // "not HTML" as none.
                // The name a DOM gives the element: the local name in the three namespaces the standard names, the qualified name in
                // any other.
                const tag = [NS_HTML, NS_SVG, NS_MATH].includes(node.ns) || !node.p ? node.n : `${node.p}:${node.n}`;
                const el = A.createElement(tag, node.ns === null ? 'urn:none' : node.ns, attrs);
                A.appendChild(parent, el);
                if (node.ns === NS_HTML && node.n === 'template') {
                    const content = A.createDocumentFragment();
                    A.setTemplateContent(el, content);
                    fill(content, node.t === null ? [] : node.t, doc);
                } else {
                    fill(el, node.c, doc);
                }
                break;
            }
            case 't':
            case 'k':
                if (node.d.length > 0) {
                    A.insertText(parent, node.d);
                }
                break;
            case 'c':
                A.appendChild(parent, A.createCommentNode(node.d));
                break;
            case 'd':
                A.setDocumentType(doc ?? parent, node.n, node.pub ?? '', node.sys ?? '');
                break;
            default:
                break; // a processing instruction
        }
    }
}

function serialize(request) {
    const tree = request.tree;
    const root = tree.k === 'doc' ? A.createDocument() : A.createDocumentFragment();
    fill(root, tree.c, tree.k === 'doc' ? root : null);
    const options = { scriptingEnabled: !!request.scripting };
    if (request.mode === 'outer') {
        const first = A.getChildNodes(root)[0];
        if (first === undefined) {
            return '';
        }
        return parse5.serializeOuter(first, options);
    }
    return parse5.serialize(root, options);
}

if (process.argv[2] === '--version') {
    const { readFileSync } = await import('node:fs');
    const v = JSON.parse(readFileSync('/opt/html-oracle/node_modules/parse5/package.json', 'utf8')).version;
    console.log(`parse5 ${v} serialize`);
} else {
    const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
        if (line.trim() === '') {
            continue;
        }
        let answer;
        try {
            answer = { out: serialize(JSON.parse(line)) };
        } catch (e) {
            answer = { error: `${e.name}: ${e.message}` };
        }
        // JSON.stringify keeps non-ASCII characters; the host reads UTF-8, but a lone surrogate would not survive
        // that, so everything outside ASCII is written as \u escapes (what Python's ensure_ascii does).
        process.stdout.write(JSON.stringify(answer).replace(/[\u007f-￿]/g, (c) => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0')) + '\n');
    }
}
