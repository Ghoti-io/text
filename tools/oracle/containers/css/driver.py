#!/usr/bin/env python3
"""The tinycss2 reference for the CSS differential (one of three in this image).

    driver.py --version | --packages
    driver.py tinycss2 < requests > answers

One request per line: `<mode> <options> <hex of the input>`, where `<options>` is `-` or a comma-
separated list of `proto=<hex of a label>` and `env=<hex of a label>`, and the input is UTF-8 text
(for `bytes`, the stylesheet's bytes). One answer per line: a JSON array in the representation of
css-parsing-tests (its README, "Result representation"), or `{"unsupported": true}` for a mode the
reference has no entry point for, or `{"crash": "<exception>"}` (an exception is an answer too: a
reference that cannot answer says so and the comparison counts it).

Modes: tokens (parse a list of component values), stylesheet, declarations (a list of
declarations), blocks (a block's contents, nesting included), bytes (a stylesheet from bytes with
its encoding: the answer is [rules, encoding name]).

The conversion to the representation is this file's, written from that README; it reads no
reference code. Copyright 2026 by Corey Pennycuff.
"""

import json
import sys

import tinycss2
import tinycss2.ast as ast
import webencodings


def number_type(is_integer):
    return "integer" if is_integer else "number"


def values(nodes):
    out = []
    for node in nodes:
        item = value(node)
        if item is not None:
            out.append(item)
    return out


def value(node):
    kind = node.type
    if kind == "whitespace":
        return " "
    if kind == "comment":
        return None
    if kind == "ident":
        return ["ident", node.value]
    if kind == "at-keyword":
        return ["at-keyword", node.value]
    if kind == "hash":
        return ["hash", node.value, "id" if node.is_identifier else "unrestricted"]
    if kind == "string":
        return ["string", node.value]
    if kind == "url":
        return ["url", node.value]
    if kind == "number":
        return ["number", node.representation, node.value, number_type(node.is_integer)]
    if kind == "percentage":
        return ["percentage", node.representation, node.value, number_type(node.is_integer)]
    if kind == "dimension":
        return ["dimension", node.representation, node.value, number_type(node.is_integer), node.unit]
    if kind == "unicode-range":
        return ["unicode-range", node.start, node.end]
    if kind == "literal":
        return node.value
    if kind == "function":
        return ["function", node.name] + values(node.arguments)
    if kind == "() block":
        return ["()"] + values(node.content)
    if kind == "[] block":
        return ["[]"] + values(node.content)
    if kind == "{} block":
        return ["{}"] + values(node.content)
    if kind == "error":
        return ["error", node.kind]
    return ["unknown", kind]


def declaration(node):
    return ["declaration", node.name, values(node.value), bool(node.important)]


def rule(node):
    kind = node.type
    if kind == "qualified-rule":
        return ["qualified rule", values(node.prelude), values(node.content) if node.content is not None else None]
    if kind == "at-rule":
        return ["at-rule", node.at_keyword, values(node.prelude), values(node.content) if node.content is not None else None]
    if kind == "declaration":
        return declaration(node)
    if kind == "error":
        return ["error", node.kind]
    if kind in ("whitespace", "comment"):
        return None
    return ["unknown", kind]


def rules(nodes):
    out = []
    for node in nodes:
        item = rule(node)
        if item is not None:
            out.append(item)
    return out


def unhex(token):
    return b"" if token == "-" else bytes.fromhex(token)


def answer(mode, opts, data):
    if mode == "tokens":
        return values(tinycss2.parse_component_value_list(data.decode("utf-8", "replace"), skip_comments=True))
    if mode == "stylesheet":
        return rules(tinycss2.parse_stylesheet(data.decode("utf-8", "replace"), skip_comments=True, skip_whitespace=True))
    if mode == "declarations":
        return rules(tinycss2.parse_declaration_list(data.decode("utf-8", "replace"), skip_comments=True, skip_whitespace=True))
    if mode == "blocks":
        return rules(tinycss2.parse_blocks_contents(data.decode("utf-8", "replace"), skip_comments=True, skip_whitespace=True))
    if mode == "bytes":
        proto = opts.get("proto")
        env = opts.get("env")
        result = tinycss2.parse_stylesheet_bytes(
            data,
            protocol_encoding=proto.decode("ascii") if proto is not None else None,
            environment_encoding=webencodings.lookup(env.decode("ascii")) if env is not None else None,
            skip_comments=True, skip_whitespace=True)
        return [rules(result[0]), result[1].name]
    return None


def main(argv):
    if len(argv) >= 2 and argv[1] == "--version":
        print("tinycss2 %s" % tinycss2.__version__)
        return 0
    if len(argv) >= 2 and argv[1] == "--packages":
        print("webencodings %s" % webencodings.VERSION)
        return 0
    out = sys.stdout
    for line in sys.stdin:
        parts = line.rstrip("\n").split(" ")
        if len(parts) != 3:
            out.write(json.dumps({"crash": "bad request"}) + "\n")
            continue
        mode, opt_text, hexed = parts
        opts = {}
        if opt_text != "-":
            for item in opt_text.split(","):
                key, _, val = item.partition("=")
                opts[key] = unhex(val)
        try:
            result = answer(mode, opts, unhex(hexed))
            if result is None:
                out.write('{"unsupported": true}\n')
            else:
                out.write(json.dumps(result, ensure_ascii=True, separators=(",", ":")) + "\n")
        except Exception as exc:  # an exception is an answer
            out.write(json.dumps({"crash": "%s: %s" % (type(exc).__name__, str(exc)[:200])}, ensure_ascii=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
