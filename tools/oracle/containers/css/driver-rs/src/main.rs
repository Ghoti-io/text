// The Servo `cssparser` reference for the CSS differential (one of three in this image).
//
//   cssparser-driver --version
//   cssparser-driver < requests > answers
//
// The protocol is driver.py's: one request per line, `<mode> <options> <hex of the input>`, one
// answer per line, in the representation of css-parsing-tests. Modes: tokens, stylesheet,
// declarations (a list of declarations: at-rules refused, so dropped), blocks (a block's contents:
// declarations and nested rules); any other is {"unsupported": true}.
//
// What cssparser keeps differently is the comparison's to know, not this file's to hide: a number
// is an f32 and has no source spelling (the answer's spelling is empty and its value the f32 widened),
// a percentage is a fraction, and the CSS 2.1 match tokens are tokens. The conversion to the
// representation is this file's, written from the representation's README.
//
// Copyright 2026 by Corey Pennycuff

use cssparser::{
    AtRuleParser, CowRcStr, DeclarationParser, ParseError, Parser, ParserState,
    QualifiedRuleParser, RuleBodyItemParser, RuleBodyParser, StyleSheetParser, Token,
};
use std::io::{self, BufRead, Write};

#[derive(Clone, Debug)]
enum J {
    S(String),
    N(f64),
    B(bool),
    Null,
    A(Vec<J>),
}

fn escape(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 || (c as u32) >= 0x7f => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    out.push_str(&format!("\\u{:04x}", u));
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn number(n: f64, out: &mut String) {
    if n.is_nan() {
        out.push_str("NaN");
    } else if n.is_infinite() {
        out.push_str(if n > 0.0 { "1e999" } else { "-1e999" });
    } else {
        out.push_str(&format!("{:e}", n));
    }
}

fn write(j: &J, out: &mut String) {
    match j {
        J::S(s) => escape(s, out),
        J::N(n) => number(*n, out),
        J::B(b) => out.push_str(if *b { "true" } else { "false" }),
        J::Null => out.push_str("null"),
        J::A(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write(item, out);
            }
            out.push(']');
        }
    }
}

fn s(x: &str) -> J {
    J::S(x.to_string())
}

fn arr(items: Vec<J>) -> J {
    J::A(items)
}

// The component values of what is left of the parser, nested blocks included.
fn collect<'i>(input: &mut Parser<'i>) -> Vec<J> {
    let mut out = Vec::new();
    loop {
        let token = match input.next_including_whitespace_and_comments() {
            Ok(t) => t.clone(),
            Err(_) => break,
        };
        match token {
            Token::Comment(_) => {}
            Token::WhiteSpace(_) => out.push(s(" ")),
            Token::Ident(v) => out.push(arr(vec![s("ident"), s(&v)])),
            Token::AtKeyword(v) => out.push(arr(vec![s("at-keyword"), s(&v)])),
            Token::Hash(v) => out.push(arr(vec![s("hash"), s(&v), s("unrestricted")])),
            Token::IDHash(v) => out.push(arr(vec![s("hash"), s(&v), s("id")])),
            Token::QuotedString(v) => out.push(arr(vec![s("string"), s(&v)])),
            Token::UnquotedUrl(v) => out.push(arr(vec![s("url"), s(&v)])),
            Token::Delim(c) => out.push(s(&c.to_string())),
            Token::Number { value, int_value, .. } => out.push(arr(vec![
                s("number"),
                s(""),
                J::N(value as f64),
                s(if int_value.is_some() { "integer" } else { "number" }),
            ])),
            Token::Percentage { unit_value, int_value, .. } => out.push(arr(vec![
                s("percentage"),
                s(""),
                J::N(unit_value as f64),
                s(if int_value.is_some() { "integer" } else { "number" }),
            ])),
            Token::Dimension { value, int_value, unit, .. } => out.push(arr(vec![
                s("dimension"),
                s(""),
                J::N(value as f64),
                s(if int_value.is_some() { "integer" } else { "number" }),
                s(&unit),
            ])),
            Token::Colon => out.push(s(":")),
            Token::Semicolon => out.push(s(";")),
            Token::Comma => out.push(s(",")),
            Token::IncludeMatch => out.push(s("~=")),
            Token::DashMatch => out.push(s("|=")),
            Token::PrefixMatch => out.push(s("^=")),
            Token::SuffixMatch => out.push(s("$=")),
            Token::SubstringMatch => out.push(s("*=")),
            Token::CDO => out.push(s("<!--")),
            Token::CDC => out.push(s("-->")),
            Token::BadUrl(_) => out.push(arr(vec![s("error"), s("bad-url")])),
            Token::BadString(_) => out.push(arr(vec![s("error"), s("bad-string")])),
            Token::CloseParenthesis => out.push(arr(vec![s("error"), s(")")])),
            Token::CloseSquareBracket => out.push(arr(vec![s("error"), s("]")])),
            Token::CloseCurlyBracket => out.push(arr(vec![s("error"), s("}")])),
            Token::Function(name) => {
                let mut v = vec![s("function"), s(&name)];
                let inner: Result<Vec<J>, ParseError<()>> = input.parse_nested_block(|p| Ok(collect(p)));
                v.extend(inner.unwrap_or_default());
                out.push(arr(v));
            }
            Token::ParenthesisBlock => {
                let mut v = vec![s("()")];
                let inner: Result<Vec<J>, ParseError<()>> = input.parse_nested_block(|p| Ok(collect(p)));
                v.extend(inner.unwrap_or_default());
                out.push(arr(v));
            }
            Token::SquareBracketBlock => {
                let mut v = vec![s("[]")];
                let inner: Result<Vec<J>, ParseError<()>> = input.parse_nested_block(|p| Ok(collect(p)));
                v.extend(inner.unwrap_or_default());
                out.push(arr(v));
            }
            Token::CurlyBracketBlock => {
                let mut v = vec![s("{}")];
                let inner: Result<Vec<J>, ParseError<()>> = input.parse_nested_block(|p| Ok(collect(p)));
                v.extend(inner.unwrap_or_default());
                out.push(arr(v));
            }
        }
    }
    out
}

fn is_ws(j: &J) -> bool {
    matches!(j, J::S(x) if x == " ")
}

// "Consume a declaration": trim white space, and take `! important` off the end.
fn declaration_value(mut values: Vec<J>) -> (Vec<J>, bool) {
    while values.first().map_or(false, is_ws) {
        values.remove(0);
    }
    while values.last().map_or(false, is_ws) {
        values.pop();
    }
    let n = values.len();
    let mut important = false;
    if n >= 2 {
        let last_is_important = matches!(&values[n - 1], J::A(v) if v.len() == 2
            && matches!(&v[0], J::S(k) if k == "ident")
            && matches!(&v[1], J::S(x) if x.eq_ignore_ascii_case("important")));
        if last_is_important {
            let mut k = n - 2;
            while k > 0 && is_ws(&values[k]) {
                k -= 1;
            }
            if matches!(&values[k], J::S(x) if x == "!") {
                values.truncate(k);
                while values.last().map_or(false, is_ws) {
                    values.pop();
                }
                important = true;
            }
        }
    }
    (values, important)
}

struct Rules {
    qualified: bool,
    at_rules: bool,
}

impl<'i> DeclarationParser<'i> for Rules {
    type Declaration = J;
    type Error = ();
    fn parse_value(
        &mut self,
        name: CowRcStr<'i>,
        input: &mut Parser<'i>,
        _start: &ParserState,
    ) -> Result<J, ParseError<()>> {
        let (value, important) = declaration_value(collect(input));
        Ok(arr(vec![s("declaration"), s(&name), arr(value), J::B(important)]))
    }
}

impl<'i> AtRuleParser<'i> for Rules {
    type Prelude = (String, Vec<J>);
    type AtRule = J;
    type Error = ();
    fn parse_prelude(&mut self, name: CowRcStr<'i>, input: &mut Parser<'i>) -> Result<Self::Prelude, ParseError<()>> {
        if !self.at_rules {
            return Err(ParseError::custom(()));
        }
        Ok((name.to_string(), collect(input)))
    }
    fn parse_block(&mut self, prelude: Self::Prelude, _start: &ParserState, input: &mut Parser<'i>) -> Result<J, ParseError<()>> {
        Ok(arr(vec![s("at-rule"), s(&prelude.0), arr(trim(prelude.1)), arr(collect(input))]))
    }
    fn rule_without_block(&mut self, prelude: Self::Prelude, _start: &ParserState) -> Result<J, ()> {
        Ok(arr(vec![s("at-rule"), s(&prelude.0), arr(trim(prelude.1)), J::Null]))
    }
}

// An at-rule's prelude keeps its leading white space in the representation (css-parsing-tests); the
// qualified rule's too. Nothing is trimmed here: this keeps the call site where the choice would be.
fn trim(v: Vec<J>) -> Vec<J> {
    v
}

impl<'i> QualifiedRuleParser<'i> for Rules {
    type Prelude = Vec<J>;
    type QualifiedRule = J;
    type Error = ();
    fn parse_prelude(&mut self, input: &mut Parser<'i>) -> Result<Vec<J>, ParseError<()>> {
        if !self.qualified {
            return Err(ParseError::custom(()));
        }
        Ok(collect(input))
    }
    fn parse_block(&mut self, prelude: Vec<J>, _start: &ParserState, input: &mut Parser<'i>) -> Result<J, ParseError<()>> {
        Ok(arr(vec![s("qualified rule"), arr(prelude), arr(collect(input))]))
    }
}

impl<'i> RuleBodyItemParser<'i, J, ()> for Rules {
    fn parse_declarations(&self) -> bool {
        true
    }
    fn parse_qualified(&self) -> bool {
        self.qualified
    }
}

fn answer(mode: &str, text: &str) -> Option<J> {
    let mut parser = Parser::new(text);
    match mode {
        "tokens" => Some(arr(collect(&mut parser))),
        "stylesheet" => {
            let mut p = Rules { qualified: true, at_rules: true };
            let mut out = Vec::new();
            for item in StyleSheetParser::new(&mut parser, &mut p) {
                if let Ok(j) = item {
                    out.push(j);
                }
            }
            Some(arr(out))
        }
        "declarations" | "blocks" => {
            let mut p = Rules { qualified: mode == "blocks", at_rules: mode == "blocks" };
            let mut out = Vec::new();
            for item in RuleBodyParser::new(&mut parser, &mut p) {
                if let Ok(j) = item {
                    out.push(j);
                }
            }
            Some(arr(out))
        }
        _ => None,
    }
}

fn unhex(h: &str) -> Option<Vec<u8>> {
    if h == "-" {
        return Some(Vec::new());
    }
    if h.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(h.len() / 2);
    let b = h.as_bytes();
    for i in (0..b.len()).step_by(2) {
        let hi = (b[i] as char).to_digit(16)?;
        let lo = (b[i + 1] as char).to_digit(16)?;
        out.push((hi * 16 + lo) as u8);
    }
    Some(out)
}

fn main() {
    if std::env::args().nth(1).as_deref() == Some("--version") {
        println!("cssparser {}", env!("GHOTI_CSSPARSER_VERSION"));
        return;
    }
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut out = io::BufWriter::new(stdout.lock());
    for line in stdin.lock().lines() {
        let line = line.unwrap();
        let parts: Vec<&str> = line.split(' ').collect();
        let mut text = String::new();
        if parts.len() != 3 {
            writeln!(out, "{{\"crash\": \"bad request\"}}").unwrap();
            continue;
        }
        match unhex(parts[2]) {
            Some(bytes) => text.push_str(&String::from_utf8_lossy(&bytes)),
            None => {
                writeln!(out, "{{\"crash\": \"bad hex\"}}").unwrap();
                continue;
            }
        }
        let mode = parts[0];
        let result = std::panic::catch_unwind(|| answer(mode, &text));
        match result {
            Ok(Some(j)) => {
                let mut s = String::new();
                write(&j, &mut s);
                writeln!(out, "{}", s).unwrap();
            }
            Ok(None) => writeln!(out, "{{\"unsupported\": true}}").unwrap(),
            Err(_) => writeln!(out, "{{\"crash\": \"panic\"}}").unwrap(),
        }
    }
}
