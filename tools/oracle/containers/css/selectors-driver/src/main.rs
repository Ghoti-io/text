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

// The driver of the Servo `selectors` crate for the selector differential (tools/oracle/css_sheet_diff.py): whether a selector parses, how the crate writes it, and which
// elements of a tree it matches. It is Servo's own parser and matcher (the crate Stylo, Firefox's style engine, is built on); everything here is the glue the crate asks of
// its user: the types of the selector, the parser's switches, and the element interface over a tree held in memory.
//
//   selectors-driver                    read a tree and selectors on standard input, answer on standard output
//   selectors-driver --version
//
// Input, one tab-separated line each (a field escapes backslash, line feed and tab as \\, \n, \t; a list of attributes is `name RS value` pairs joined by US):
//
//   Q QUIRKS                                            1 when the document is in quirks mode (class and id compare ignoring ASCII case), else 0
//   T INDEX PARENT LOCAL ID CLASSES ATTRIBUTES TEXT     an element, in document order: PARENT is -1 for the root; TEXT is 1 when it has a non-empty text child
//   Z SELECTOR                                          a selector to parse and match against every element
//   X                                                   answer every Z, then forget the tree
//
// Output for a Z: `SELECTOR TAB Z TAB OK TAB serialization TAB ids` (the ids of the elements it matches, in document order, joined by spaces), `... TAB SYNTAX`, or
// `... TAB NONTS` for a selector that holds a pseudo-class the crate leaves to its user (:hover, :checked, ...): those are parsed here as anything and never match, so such a
// selector is not compared. A line `X` ends the answers of a tree.

use cssparser::{serialize_identifier, CowRcStr, Parser as CssParser, ToCss};
use selectors::attr::{AttrSelectorOperation, CaseSensitivity, NamespaceConstraint};
use selectors::bloom::BloomFilter;
use selectors::context::{
    MatchingContext, MatchingForInvalidation, MatchingMode, NeedsSelectorFlags, QuirksMode, SelectorCaches,
};
use selectors::matching::{matches_selector_list, ElementSelectorFlags};
use selectors::parser::{
    self, ParseRelative, Parser, SelectorImpl, SelectorList, SelectorParseError, SelectorParseErrorKind,
};
use selectors::{Element, OpaqueElement};
use precomputed_hash::PrecomputedHash;
use std::collections::hash_map::DefaultHasher;
use std::fmt::{self, Write as FmtWrite};
use std::hash::{Hash, Hasher};
use std::io::{self, BufRead, Write};
use std::rc::Rc;

#[derive(Clone, Debug, Default, Eq, Hash, PartialEq)]
struct Atom(String);

impl ToCss for Atom {
    fn to_css<W: fmt::Write>(&self, dest: &mut W) -> fmt::Result {
        serialize_identifier(&self.0, dest)
    }
}

impl<'a> From<&'a str> for Atom {
    fn from(s: &'a str) -> Self {
        Atom(s.to_string())
    }
}

impl PrecomputedHash for Atom {
    fn precomputed_hash(&self) -> u32 {
        let mut h = DefaultHasher::new();
        self.0.hash(&mut h);
        h.finish() as u32
    }
}

#[derive(Clone, Debug, Default, Eq, Hash, PartialEq)]
struct Value(String);

impl ToCss for Value {
    fn to_css<W: fmt::Write>(&self, dest: &mut W) -> fmt::Result {
        dest.write_char('"')?;
        write!(cssparser::CssStringWriter::new(dest), "{}", &self.0)?;
        dest.write_char('"')
    }
}

impl AsRef<str> for Value {
    fn as_ref(&self) -> &str {
        &self.0
    }
}

impl<'a> From<&'a str> for Value {
    fn from(s: &'a str) -> Self {
        Value(s.to_string())
    }
}

/// A pseudo-class the crate leaves to its user: any name, never matched, written as it was read.
#[derive(Clone, Debug, Eq, PartialEq)]
struct PseudoClass(String);

impl parser::NonTSPseudoClass for PseudoClass {
    fn is_active_or_hover(&self) -> bool {
        self.0 == "hover" || self.0 == "active"
    }

    fn is_user_action_state(&self) -> bool {
        self.is_active_or_hover()
    }
}

impl ToCss for PseudoClass {
    fn to_css<W: fmt::Write>(&self, dest: &mut W) -> fmt::Result {
        dest.write_char(':')?;
        serialize_identifier(&self.0, dest)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct PseudoElement(String);

impl parser::PseudoElement for PseudoElement {}

impl ToCss for PseudoElement {
    fn to_css<W: fmt::Write>(&self, dest: &mut W) -> fmt::Result {
        dest.write_str("::")?;
        serialize_identifier(&self.0, dest)
    }
}

#[derive(Clone, Debug, PartialEq)]
struct Imp;

impl SelectorImpl for Imp {
    type ExtraMatchingData<'a> = std::marker::PhantomData<&'a ()>;
    type AttrValue = Value;
    type Identifier = Atom;
    type LocalName = Atom;
    type NamespaceUrl = Atom;
    type NamespacePrefix = Atom;
    type BorrowedLocalName = Atom;
    type BorrowedNamespaceUrl = Atom;
    type NonTSPseudoClass = PseudoClass;
    type PseudoElement = PseudoElement;
}

struct P {
    nonts: std::cell::Cell<bool>,
}

impl<'i> Parser<'i> for P {
    type Impl = Imp;
    type Error = SelectorParseErrorKind;

    fn parse_slotted(&self) -> bool {
        true
    }
    fn parse_nth_child_of(&self) -> bool {
        true
    }
    fn parse_is_and_where(&self) -> bool {
        true
    }
    fn parse_has(&self) -> bool {
        true
    }
    fn parse_parent_selector(&self) -> bool {
        true
    }
    fn parse_part(&self) -> bool {
        true
    }
    fn parse_host(&self) -> bool {
        true
    }

    fn parse_non_ts_pseudo_class(&self, name: CowRcStr<'i>) -> Result<PseudoClass, SelectorParseError> {
        self.nonts.set(true);
        Ok(PseudoClass(name.to_ascii_lowercase()))
    }

    fn parse_non_ts_functional_pseudo_class(
        &self,
        name: CowRcStr<'i>,
        parser: &mut CssParser<'i>,
        _after_part: bool,
    ) -> Result<PseudoClass, SelectorParseError> {
        self.nonts.set(true);
        // the argument is read as anything up to the closing parenthesis
        while parser.next_including_whitespace_and_comments().is_ok() {}
        Ok(PseudoClass(name.to_ascii_lowercase()))
    }

    fn parse_pseudo_element(&self, name: CowRcStr<'i>) -> Result<PseudoElement, SelectorParseError> {
        Ok(PseudoElement(name.to_ascii_lowercase()))
    }
}

// ---- the tree ---------------------------------------------------------------------------------------------------------------

struct Node {
    local: Atom,
    id: String,
    classes: Vec<String>,
    attrs: Vec<(String, String)>,
    parent: Option<usize>,
    children: Vec<usize>,
    text: bool,
}

struct Tree {
    nodes: Vec<Node>,
}

#[derive(Clone)]
struct El {
    tree: Rc<Tree>,
    ix: usize,
}

impl fmt::Debug for El {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "El({})", self.ix)
    }
}

impl El {
    fn node(&self) -> &Node {
        &self.tree.nodes[self.ix]
    }
    fn at(&self, ix: usize) -> El {
        El { tree: self.tree.clone(), ix }
    }
    fn position(&self) -> (Option<usize>, usize) {
        let n = self.node();
        match n.parent {
            Some(p) => (Some(p), self.tree.nodes[p].children.iter().position(|&c| c == self.ix).unwrap()),
            None => (None, 0),
        }
    }
}

fn ci_eq(a: &str, b: &str, cs: CaseSensitivity) -> bool {
    match cs {
        CaseSensitivity::CaseSensitive => a == b,
        CaseSensitivity::AsciiCaseInsensitive => a.eq_ignore_ascii_case(b),
    }
}

impl Element for El {
    type Impl = Imp;

    fn opaque(&self) -> OpaqueElement {
        OpaqueElement::new(&self.tree.nodes[self.ix])
    }
    fn parent_element(&self) -> Option<El> {
        self.node().parent.map(|p| self.at(p))
    }
    fn parent_node_is_shadow_root(&self) -> bool {
        false
    }
    fn containing_shadow_host(&self) -> Option<El> {
        None
    }
    fn is_pseudo_element(&self) -> bool {
        false
    }
    fn prev_sibling_element(&self) -> Option<El> {
        let (p, i) = self.position();
        match p {
            Some(p) if i > 0 => Some(self.at(self.tree.nodes[p].children[i - 1])),
            _ => None,
        }
    }
    fn next_sibling_element(&self) -> Option<El> {
        let (p, i) = self.position();
        match p {
            Some(p) if i + 1 < self.tree.nodes[p].children.len() => Some(self.at(self.tree.nodes[p].children[i + 1])),
            _ => None,
        }
    }
    fn first_element_child(&self) -> Option<El> {
        self.node().children.first().map(|&c| self.at(c))
    }
    fn is_html_element_in_html_document(&self) -> bool {
        true
    }
    fn has_local_name(&self, name: &Atom) -> bool {
        self.node().local.0.eq_ignore_ascii_case(&name.0) && true
    }
    fn has_namespace(&self, ns: &Atom) -> bool {
        ns.0 == "http://www.w3.org/1999/xhtml"
    }
    fn is_same_type(&self, other: &El) -> bool {
        self.node().local == other.node().local
    }
    fn attr_matches(&self, ns: &NamespaceConstraint<&Atom>, local_name: &Atom, operation: &AttrSelectorOperation<&Value>) -> bool {
        if let NamespaceConstraint::Specific(n) = ns {
            if !n.0.is_empty() {
                return false;
            }
        }
        self.node().attrs.iter().any(|(k, v)| k.eq_ignore_ascii_case(&local_name.0) && operation.eval_str(v))
    }
    fn match_non_ts_pseudo_class(&self, _pc: &PseudoClass, _context: &mut MatchingContext<Imp>) -> bool {
        false
    }
    fn match_pseudo_element(&self, _pe: &PseudoElement, _context: &mut MatchingContext<Imp>) -> bool {
        false
    }
    fn apply_selector_flags(&self, _flags: ElementSelectorFlags) {}
    fn is_link(&self) -> bool {
        false
    }
    fn is_html_slot_element(&self) -> bool {
        false
    }
    fn has_id(&self, id: &Atom, cs: CaseSensitivity) -> bool {
        !self.node().id.is_empty() && ci_eq(&self.node().id, &id.0, cs)
    }
    fn has_class(&self, name: &Atom, cs: CaseSensitivity) -> bool {
        self.node().classes.iter().any(|c| ci_eq(c, &name.0, cs))
    }
    fn has_custom_state(&self, _name: &Atom) -> bool {
        false
    }
    fn imported_part(&self, _name: &Atom) -> Option<Atom> {
        None
    }
    fn is_part(&self, _name: &Atom) -> bool {
        false
    }
    fn is_empty(&self) -> bool {
        self.node().children.is_empty() && !self.node().text
    }
    fn is_root(&self) -> bool {
        self.node().parent.is_none()
    }
    fn add_element_unique_hashes(&self, _filter: &mut BloomFilter) -> bool {
        false
    }
}

// ---- the protocol ------------------------------------------------------------------------------------------------------------

fn unesc(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars();
    while let Some(c) = it.next() {
        if c == '\\' {
            match it.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some(o) => out.push(o),
                None => {}
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\n', "\\n").replace('\t', "\\t")
}

fn answer(nodes: Vec<Node>, selectors: &[String], quirks: bool, out: &mut impl Write) {
    let tree = Rc::new(Tree { nodes });
    for sel in selectors {
        let parser = P { nonts: std::cell::Cell::new(false) };
        let mut css = CssParser::new(sel);
        let parsed = SelectorList::parse(&parser, &mut css, ParseRelative::No);
        let list = match parsed {
            Ok(l) if css.is_exhausted() => l,
            _ => {
                writeln!(out, "{}\tZ\tSYNTAX", esc(sel)).unwrap();
                continue;
            }
        };
        if parser.nonts.get() {
            writeln!(out, "{}\tZ\tNONTS", esc(sel)).unwrap();
            continue;
        }
        let mut text = String::new();
        list.to_css(&mut text).unwrap();
        let mut ids: Vec<String> = Vec::new();
        let mut caches = SelectorCaches::default();
        for ix in 0..tree.nodes.len() {
            let el = El { tree: tree.clone(), ix };
            let mut ctx = MatchingContext::new(
                MatchingMode::Normal,
                None,
                &mut caches,
                if quirks { QuirksMode::Quirks } else { QuirksMode::NoQuirks },
                NeedsSelectorFlags::No,
                MatchingForInvalidation::No,
            );
            if matches_selector_list(&list, &el, &mut ctx) {
                let id = &tree.nodes[ix].id;
                ids.push(if id.is_empty() { "?".to_string() } else { id.clone() });
            }
        }
        writeln!(out, "{}\tZ\tOK\t{}\t{}", esc(sel), esc(&ids.join(" ")), esc(&text)).unwrap();
    }
    writeln!(out, "X").unwrap();
}

fn main() {
    if std::env::args().nth(1).as_deref() == Some("--version") {
        println!("selectors 0.41.0, cssparser 0.38.0");
        return;
    }
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut out = stdout.lock();
    let mut nodes: Vec<Node> = Vec::new();
    let mut selectors: Vec<String> = Vec::new();
    let mut quirks = false;
    for line in stdin.lock().lines() {
        let line = line.unwrap();
        let f: Vec<&str> = line.split('\t').collect();
        match f[0] {
            "T" if f.len() == 8 => {
                let parent: i64 = f[2].parse().unwrap();
                let ix: usize = f[1].parse().unwrap();
                let classes: Vec<String> = unesc(f[5]).split_whitespace().map(|s| s.to_string()).collect();
                let attrs: Vec<(String, String)> = unesc(f[6])
                    .split('\u{1f}')
                    .filter(|s| !s.is_empty())
                    .map(|p| {
                        let mut it = p.splitn(2, '\u{1e}');
                        (it.next().unwrap().to_string(), it.next().unwrap_or("").to_string())
                    })
                    .collect();
                let node = Node {
                    local: Atom(unesc(f[3])),
                    id: unesc(f[4]),
                    classes,
                    attrs,
                    parent: if parent < 0 { None } else { Some(parent as usize) },
                    children: Vec::new(),
                    text: f[7] == "1",
                };
                assert_eq!(ix, nodes.len());
                if parent >= 0 {
                    nodes[parent as usize].children.push(ix);
                }
                nodes.push(node);
            }
            "Q" if f.len() == 2 => quirks = f[1] == "1",
            "Z" if f.len() == 2 => selectors.push(unesc(f[1])),
            "X" => {
                answer(std::mem::take(&mut nodes), &selectors, quirks, &mut out);
                selectors.clear();
                quirks = false;
                out.flush().unwrap();
            }
            _ => {}
        }
    }
}
