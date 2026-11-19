/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The runner of the scoring interpreter (tools/css/cssom_eval.py makes the programs, tools/conformance/css_cssom_script_suite.py scores them). A program is a page: the page's HTML,
 * parsed by the library's HTML parser into a Markup tree, and the ops that the page's script performed on it, in the order it performed them, every loop unrolled and every function
 * inlined. No JavaScript is in here: an op is one call of the library, on the document a consumer would build (the Markup binding, the style attribute, the rule index, the computed
 * style), so the gate scores the integration and not a test double.
 *
 * Input, tab separated with `\\`, `\t`, `\n`, `\r` and `\0` escaped: `P file quirks tests`, `H html`, then `O op field...` lines. Output: `R file index PASS`, `R file index FAIL
 * message`, or `R file index LAYOUT property` (the test read a property whose used value needs a layout the library was not given: it is neither passed nor failed), or
 * `R file index TIMELINE property` (the test read a property that an animation targets, and the animation was given no time: the same), for each test,
 * and `E file message` for a program that could not be run as the page ran it.
 *
 * Values. A value is a type and a text: `s` string, `n` number, `b` boolean, `N` null, `u` undefined. An operand is `s:text`, `n:3`, `b:true`, `N:`, `u:` or `r:<register>`. An
 * assertion compares as `testharness.js` does (strict equality, the same type and the same value).
 *
 * Ops (elements are `eN`, style declarations `sN`, registers `rN`): elem E byid ID | sel ROOT SELECTOR | qsa ROOT SELECTOR INDEX | body | html | head | nav FROM
 * parentElement|firstElementChild|lastElementChild|nextElementSibling|previousElementSibling; gcs S E PSEUDO; style S E; sget R S PROP; sattr R S NAME; sprio R S PROP; sitem R S INDEX;
 * slen R S; scss R S; sset S PROP VALUE PRIORITY; sattrset S NAME VALUE; scsstext S VALUE; sremove R S PROP; setattr E NAME VALUE; getattr R E NAME; hasattr R E NAME; rmattr E NAME;
 * classadd|classremove E TOKEN...; classtoggle R E TOKEN; classhas R E TOKEN; cmp R eq|ne|leq|lne A B; not R A; assert_equals A B MSG; assert_not_equals A B MSG; assert_true A MSG;
 * assert_false A MSG; assert_in_array A B... MSG; assert_approx_equals A B EPS MSG; assert_greater_than|assert_less_than|assert_greater_than_equal|assert_less_than_equal A B MSG;
 * create E TAG NAMESPACE | ctext E DATA | ccomment E DATA | cfrag E | clone E SRC true|false (nodes are `eN` too: an element, a text node, a fragment); append|prepend|replacechildren|before|
 * after|replacewith NODE ARG... (an ARG is `e:name` or `s:text`); appendchild PARENT CHILD; insertbefore PARENT CHILD REF (`N:` for none); removechild PARENT CHILD; replacechild PARENT
 * NEW OLD; remove NODE; tcget R NODE; tcset NODE VALUE; dset NODE VALUE; ihget|ohget R NODE; ihset NODE VALUE; nread R NODE nodeType|nodeName|tagName|localName|namespaceURI|nodeValue|
 * data|isConnected|hasChildNodes|childElementCount; contains R A B; matches R E SEL; closest E2 E SEL; child E PARENT elements|nodes INDEX; nchildren R PARENT elements|nodes; qlen R ROOT SEL;
 * the nav of elem also takes firstChild|lastChild|nextSibling|previousSibling|parentNode. After a change of the tree the Markup binding is synced, so a style element made by the script has
 * its sheet.
 * Time and events (a promise_test; tools/css/cssom_eval.py, Loop, writes them where the page waits): ptest (the test is a promise_test: a read of a computed style is a style update of the whole document, as
 * in a browser); frame R (one frame of 16 ms: the style recalc that starts and stops transitions, the time moves on, the events the transition tracker owes are dispatched; R is the time, as the
 * timestamp of a requestAnimationFrame callback is; the ops after it are that callback); advance MS (frames until MS have passed: a timer); lw W E TYPE... (an EventWatcher W on the element, listening for the
 * types); lstop W; lf E TYPE MSG (a handler of the page that only calls assert_unreached: the event, at the element or below it, fails the test with MSG); ewait W BUDGET first|all N TYPE... REG... (wait_for():
 * frames pass, and the events are dispatched to the listeners, until the N types have come in that order; BUDGET is the frames of the watcher's timeout, 0 for none; REG is, for each type, the registers
 * of the event's type, propertyName, elapsedTime and pseudoElement); ewaito E TYPE REG REG REG REG (a once listener that resolves a promise: until the event comes); ebatch s:PSEUDO N NAME... TIME... REG... (the
 * events and the expectations sorted by property name and compared, as helper.js assert_end_event_batch_equal does); tlnow R (document.timeline.currentTime). The time is the document timeline's: 0 at
 * the start of a page, and it only moves in these ops. An event that no listener is waiting for fails the test of the EventWatcher it reaches, as testharness.js's handler does, and a test that waits more
 * than 625 frames (the ten seconds the harness gives it) fails.
 * Animations (Element.getAnimations(), the members of the objects it returns; the Web Animations model of the library, animation/world.h): ga L ROOT PSEUDO SUBTREE (the animations of an element, or ROOT `doc`, after a
 * style update of the whole document: L is a list of them); alen R L; aat A L INDEX (the animation, or undefined past the end: a member read of it fails the test as the TypeError of the page does); aget R A MEMBER
 * (animationName and transitionProperty, which are `undefined` on the other kind, playState, pending, currentTime, id); aset A id VALUE; tget R A timing|computed MEMBER (getTiming() and getComputedTiming() of its
 * effect: delay, endDelay, fill, iterationStart, iterations, duration, direction, easing, and for the computed ones endTime, activeDuration, localTime, progress, currentIteration). An animation is read as it is
 * when the op runs; one that is no longer in the document's animations reads as idle, with null times.
 * `sattr R S NAME flush` is a read whose value the page drops (it needs no layout, whatever the property is). `elem R nav E ...` on an element whose creation the runner skipped because the test had failed is null.
 * fail MSG; test INDEX NAME; cleanup (the ops after it run whether or not the test failed); end. An op on an element that is null (a lookup that found nothing) fails the test, as
 * the TypeError of the page does.
 */

#include <ghoti.io/text/animation.h>
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_FIELDS 40

/* ---- strings -------------------------------------------------------------------------------------------------------------------------------- */

static char * dupn(const char * s, size_t n) {
  char * d = malloc(n + 1);
  if (!d) {
    abort();
  }
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

static char * dups(const char * s) {
  return dupn(s, strlen(s));
}

static char * fmt(const char * f, ...) __attribute__((format(printf, 1, 2)));
static char * fmt(const char * f, ...) {
  char buf[2048];
  __builtin_va_list ap;
  __builtin_va_start(ap, f);
  vsnprintf(buf, sizeof buf, f, ap);
  __builtin_va_end(ap);
  return dups(buf);
}

typedef struct {
  char * f[MAX_FIELDS];
  size_t l[MAX_FIELDS];
  size_t n;
} op;

static size_t unescape_into(char * s) {
  char * out = s;
  for (const char * p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      *out++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == '0' ? '\0' : *p;
    }
    else {
      *out++ = *p;
    }
  }
  *out = '\0';
  return (size_t)(out - s);
}

static int parse_op(char * line, op * o) {
  o->n = 0;
  char * p = line;
  for (;;) {
    char * tab = strchr(p, '\t');
    if (o->n == MAX_FIELDS) {
      return 0;
    }
    o->f[o->n++] = p;
    if (!tab) {
      break;
    }
    *tab = '\0';
    p = tab + 1;
  }
  for (size_t i = 0; i < o->n; i++) {
    o->l[i] = unescape_into(o->f[i]);
  }
  return 1;
}

/* ---- values and registers -------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char kind;   /* s n b N u */
  char * s;
  size_t n;
} value;

static value val(char kind, const char * s, size_t n) {
  value v;
  v.kind = kind;
  v.s = dupn(s ? s : "", n);
  v.n = n;
  return v;
}

static void value_free(value * v) {
  free(v->s);
  v->s = NULL;
}


/* A register holds a value and where it came from: a register that a read of a property whose used value needs a layout (or that an animation targets, with no time) filled is
 * tainted with that property, and a test that asserts on it needs that layout, wherever the read was made. The page of a script test often reads a style at its top level and asserts on
 * the answer in the test that follows (`let v = getComputedStyle(e).marginLeft; test(() => assert_equals(v, "40px"))`): the read is before the test, so the test is the one to be told. */
typedef struct {
  char * name;
  value v;
  char * layout;     /* the property whose used value needs a layout, or NULL */
  char * timeline;   /* the property an animation targets that has no time, or NULL */
} reg;

/* ---- the world ---------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * name;
  GTEXT_Markup_Node * node;   /* NULL: a lookup that found nothing */
} elem;

typedef struct {
  char * name;
  GTEXT_CSSOM_Decl * decl;
  int computed;
  GTEXT_Markup_Node * node;
  char * elem_name;   /* the element the declaration was made for, and the pseudo-element argument of getComputedStyle: to make it again when the document is parsed again */
  char * pseudo;
} style;

typedef struct {
  char * name;
  int kind;    /* a sheet, a rule or a media list */
  int state;   /* the object, null, or undefined */
  void * ptr;  /* retained */
} object;

typedef struct {
  char * name;
  GTEXT_Markup_Node ** nodes;   /* a static NodeList: the elements a querySelectorAll() found when it was called */
  size_t n;
} nodelist;

/* A shadow tree: its root is a fragment of the page's Markup document (not a node of the document's own tree: the tree of the host does not hold it), `host` the element it is attached
   to; its scope is registered with the CSSOM document and its `<style>` elements are read by a binding of their own (the binding's `root` option). */
typedef struct {
  GTEXT_Markup_Node * root;
  GTEXT_Markup_Node * host;
  int open;
  int declarative;   /* made by a template at parse time: attachShadow() on its host clears and returns it (DOM, "attach a shadow root") */
  GTEXT_CSSOM_Scope * scope;
  GTEXT_CSSOM_Markup_Binding * binding;
} shadow;

/* ---- time, frames and events: the document timeline and the events the transition tracker reports (the ops `frame`, `advance`, `ewait`, `ewaito`, `lw`, `lf`, `lstop`, `ebatch`, `tlnow`) ---- */

/* An event of a frame, waiting to be dispatched to the listeners. */
typedef struct {
  GTEXT_Markup_Node * target;
  const char * type;      /* transitionrun, transitionstart, transitionend, transitioncancel */
  char * property;
  char * pseudo;
  double elapsed;
  int resolved;           /* the scheduled event time is resolved */
  double scheduled;       /* ... and is this (ms on the timeline) */
} qevent;

/* A listener of the page. kind 0 is an EventWatcher's (testharness.js: its handler asserts that it is waiting for the event's type, and records it), kind 1 is a handler that only calls
   assert_unreached (the event fails the test with its message). A listener is for the events at its node and at the nodes below it: transition events bubble. */
typedef struct {
  int kind;
  GTEXT_Markup_Node * node;
  char * type;
  size_t watcher;
  char * message;
} listener;

#define EVENT_RECORD 64

typedef struct {
  char * name;
  int active;       /* stop_watching() has not been called */
  int waiting;      /* a wait_for() is outstanding */
  int finished;     /* the outstanding wait_for() has had all its events */
  char ** wait_types;
  size_t nwait, pos;
  qevent rec[EVENT_RECORD];
  size_t nrec;
} watcher;

/* An Animation object the program holds: which animation it is, and what it was when it was found (an animation that has left the document's animations reads as idle, with this timing). */
typedef struct animh {
  char * name;
  int missing;                  /* the index was past the end: the value is undefined */
  GTEXT_Anim_Kind kind;
  uint64_t id;
  char * pname;                 /* animationName or transitionProperty */
  char * easing;
  GTEXT_Anim_Effect_Timing timing;
  char * idtext;                /* `id`, which a page sets */
} animh;

typedef struct animlist {
  char * name;
  animh * items;
  size_t n;
} animlist;

/* An effect the program holds (a `KeyframeEffect` it made, or `animation.effect` when it read it: NULL when the animation had none), and the list `getKeyframes()` returned. */
typedef struct {
  char * name;
  GTEXT_Anim_Effect * e;
} effh;

typedef struct {
  char * name;
  GTEXT_Anim_Keyframes * k;
} kfl;

/* What the page can do to an Animation (`play()`, `finish()`, `currentTime = ...`, `ready`) is done to the Animation of the model that holds the animation (animation/world.h,
 * gtext_anim_world_control()), made the first time the program touches that animation. The promises of an Animation are generations (animation.h): the model says when each settles, in notes
 * the runner drains after every step of time, and `pwait` lets frames pass until the generation it waits for has. */
typedef struct {
  uint32_t gen;
  int state;                    /* 1 fulfilled, 2 rejected */
} psettle;

typedef struct ctl {
  GTEXT_Anim_Kind kind;
  uint64_t id;
  GTEXT_Anim_Animation * a;     /* the world's, or the runner's own for an animation that had left the document before the program touched it (idle: it has no effect) */
  int orphan;
  psettle * p[2];               /* the promises that have settled: [0] ready, [1] finished */
  size_t np[2], cap[2];
} ctl;

typedef struct {
  GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Conditions * conditions;
  GTEXT_CSSOM_Document * doc;
  GTEXT_CSSOM_Transition_Tracker * tracker;
  GTEXT_Anim_World * anim;
  GTEXT_Markup_Document * markup;
  GTEXT_CSS_Markup_Adapter * adapter;
  GTEXT_CSS_Selector_Match_Options match_options;
  GTEXT_CSSOM_Markup_Binding * binding;
  GTEXT_CSS_Tree * ua_tree;
  GTEXT_CSS_Sheet * ua_sheet;
  GTEXT_CSSOM_Attribute_Binding attr;
  elem * elems;
  size_t nelems, capelems;
  style * styles;
  size_t nstyles, capstyles;
  reg * regs;
  size_t nregs, capregs;
  nodelist * lists;
  size_t nlists, caplists;
  object * objs;
  size_t nobjs, capobjs;
  shadow * shadows;
  size_t nshadows, capshadows;
  uint64_t epoch;   /* what changed the flat tree without changing the Markup document: a shadow root attached */
  GTEXT_CSS_Element_Interface iface;   /* the adapter's, with the shadow trees */
  double now_ms;               /* the document timeline: the time of the last frame (0 before the first); a transition begun by the page starts at it */
  long test_frames;            /* frames run since the test began (the harness gives up on a test after ten seconds) */
  int animates;                /* an op of the animation family has run: a read of a computed style is a style update of the whole document, as a script that makes or reads animations needs it to be */
  int async;                   /* a promise_test has run in this world: a read of a computed style is an update of the style of the whole document, as it is in a browser (a style change event for every element) */
  int recalc_done;             /* the style of every element has been brought up to date at these generations of the Markup and CSSOM documents */
  uint64_t recalc_markup, recalc_cssom;
  GTEXT_Markup_Node ** tracked;   /* the elements of the document as of the last style recalc, in tree order: the elements the tracker knows */
  size_t ntracked, captracked;
  GTEXT_Markup_Node ** known;     /* every element the tracker has been told of, for the target of an event of an element that has left the document since */
  size_t nknown, capknown;
  char ** skipped;                /* the names of the elements an op made that the runner skipped because the test had failed: the cleanup that asks for one finds none, as a page's would */
  size_t nskipped, capskipped;
  listener * listeners;
  size_t nlisteners, caplisteners;
  watcher * watchers;
  size_t nwatchers, capwatchers;
  qevent * queue;              /* the events of the frames that have not been dispatched */
  size_t nqueue, capqueue, headqueue;
  struct animh * ahs;          /* the Animation objects the program holds */
  size_t nahs, capahs;
  struct animlist * alists;    /* the results of getAnimations() */
  size_t nalists, capalists;
  effh * effs;                 /* the effects the program holds */
  size_t neffs, capeffs;
  kfl * kfls;                  /* the keyframe lists getKeyframes() returned */
  size_t nkfls, capkfls;
  struct ctl * ctls;           /* the Animations the program has touched (an Animation of the model holds the timing state of the animation) and the promises each has settled */
  size_t nctls, capctls;
} world;

/* The user agent's style sheet is the part of the HTML rendering section that the display and the body's margin need; the library has no user agent sheet of its own. */
/* `[inert] { interactivity: inert }` is the rule CSS UI 4 ("Preventing All Interaction: the interactivity property") says to insert into the HTML style sheet.
 * `::backdrop { position: fixed; inset: 0 }` is the rule CSS Positioned Layout 4 ("The ::backdrop Pseudo-Element") says a user agent should have in its UA-level style sheet. */
static const char UA_SHEET[] =
    "html, address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, "
    "h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block }\n"
    "body { display: block; margin: 8px }\n"
    "li { display: list-item }\n"
    "table { display: table } caption { display: table-caption } colgroup { display: table-column-group } col { display: table-column } thead { display: table-header-group }\n"
    "tbody { display: table-row-group } tfoot { display: table-footer-group } tr { display: table-row } td, th { display: table-cell }\n"
    "[hidden], area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template, title { display: none }\n"
    "input, button, select, textarea { display: inline-block }\n"
    "[inert] { interactivity: inert }\n"
    "::backdrop { position: fixed; inset: 0 }\n";

/* The same question asked of a fuller sheet, and of none: a failure whose answer moves with the user agent sheet of this runner is a failure that may be the runner's (the gate lists it
 * apart from the library's defects). The fuller sheet is typed from the rendering section of the HTML Standard as far as the properties a computed read can see; it is an instrument for
 * that comparison, not a claim of conformance. Selected by the environment variable CSSOM_RUNNER_UA: `none` (no sheet and no system color palette), `full`; otherwise the sheet above. */
static const char UA_SHEET_FULL[] =
    "html, address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, "
    "h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block }\n"
    "body { display: block; margin: 8px }\n"
    "li { display: list-item }\n"
    "table { display: table } caption { display: table-caption } colgroup { display: table-column-group } col { display: table-column } thead { display: table-header-group }\n"
    "tbody { display: table-row-group } tfoot { display: table-footer-group } tr { display: table-row } td, th { display: table-cell }\n"
    "[hidden], area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template, title { display: none }\n"
    "input, button, select, textarea { display: inline-block }\n"
    "[inert] { interactivity: inert }\n"
    "::backdrop { position: fixed; inset: 0 }\n"
    "p, dl, multicol { margin-block: 1em } blockquote, figure { margin-block: 1em; margin-inline: 40px } center { text-align: center } hr { margin-block: 0.5em; margin-inline: auto; border-style: inset; border-width: 1px; color: gray }\n"
    "h1 { font-size: 2em; margin-block: 0.67em } h2 { font-size: 1.5em; margin-block: 0.83em } h3 { font-size: 1.17em; margin-block: 1em } h4 { margin-block: 1.33em } h5 { font-size: 0.83em; margin-block: 1.67em } h6 { font-size: 0.67em; margin-block: 2.33em }\n"
    "h1, h2, h3, h4, h5, h6, b, strong, th { font-weight: bold } i, cite, em, var, address, dfn { font-style: italic } dt { } dd { margin-inline-start: 40px }\n"
    "u, ins { text-decoration: underline } s, strike, del { text-decoration: line-through } a:any-link { text-decoration: underline } abbr[title], acronym[title] { text-decoration: underline dotted }\n"
    "pre, listing, xmp, plaintext { white-space: pre } textarea { white-space: pre-wrap } code, kbd, samp, tt, pre, listing, xmp, plaintext { font-family: monospace }\n"
    "small, sub, sup { font-size: smaller } big { font-size: larger } sub { vertical-align: sub } sup { vertical-align: super } mark { background-color: yellow; color: black }\n"
    "ul, ol, dir, menu { margin-block: 1em; padding-inline-start: 40px } ul, dir, menu { list-style-type: disc } ol { list-style-type: decimal }\n"
    "table { box-sizing: border-box; border-spacing: 2px; border-collapse: separate } td, th { padding: 1px } th { text-align: center } caption { text-align: center }\n"
    "fieldset { margin-inline: 2px; padding-block: 0.35em 0.625em; padding-inline: 0.75em; border: 2px groove } img { } svg:not(:root) { overflow: hidden }\n";

/* The system colors are the user's: a computed value keeps the keyword until the consumer's environment says what it is (css_element.h resolve_color). The runner is the consumer here and
 * answers every system color of CSS Color 4 and the deprecated ones' counterparts with the sRGB value Chromium gives it in each scheme (a palette is the consumer's to choose, and the two
 * browsers choose differently for several: the computed-value gate supplies the same values for the names it asks about). Any other name is not answered. */
static bool runner_resolve_color(void * user, const GTEXT_CSS_Color * color, GTEXT_CSS_Color_Scheme scheme, double rgba[4]) {
  (void)user;
  if (!color || color->kind != GTEXT_CSS_COLOR_KEYWORD || (color->keyword != GTEXT_CSS_COLOR_KW_SYSTEM && color->keyword != GTEXT_CSS_COLOR_KW_DEPRECATED)) {
    return false;
  }
  char w[32];
  if (color->name.length >= sizeof w) {
    return false;
  }
  for (size_t i = 0; i < color->name.length; i++) {
    w[i] = (char)tolower((unsigned char)color->name.data[i]);
  }
  w[color->name.length] = '\0';
  const bool dark = scheme == GTEXT_CSS_SCHEME_DARK;
  /* the sRGB value of each system color in the light and the dark scheme: Chromium's (the browsers differ in several, and the consumer's palette is the consumer's) */
  static const struct {
    const char * name;
    double light[4], dark[4];
  } palette[] = {
      {"canvas", {255, 255, 255, 1}, {18, 18, 18, 1}},
      {"canvastext", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"linktext", {0, 0, 238, 1}, {158, 158, 255, 1}},
      {"visitedtext", {85, 26, 139, 1}, {208, 173, 240, 1}},
      {"activetext", {255, 0, 0, 1}, {255, 0, 0, 1}},
      {"buttonface", {239, 239, 239, 1}, {107, 107, 107, 1}},
      {"buttontext", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"buttonborder", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"field", {255, 255, 255, 1}, {59, 59, 59, 1}},
      {"fieldtext", {0, 0, 0, 1}, {255, 255, 255, 1}},
      {"highlight", {0, 65, 198, 0.8}, {0, 65, 198, 0.8}},
      {"highlighttext", {255, 255, 255, 1}, {255, 255, 255, 1}},
      {"selecteditem", {25, 103, 210, 1}, {153, 200, 255, 1}},
      {"selecteditemtext", {255, 255, 255, 1}, {59, 59, 59, 1}},
      {"mark", {255, 255, 0, 1}, {255, 255, 0, 1}},
      {"marktext", {0, 0, 0, 1}, {0, 0, 0, 1}},
      {"graytext", {128, 128, 128, 1}, {128, 128, 128, 1}},
      {"accentcolor", {0, 117, 255, 1}, {0, 117, 255, 1}},
      {"accentcolortext", {255, 255, 255, 1}, {255, 255, 255, 1}},
      {"appworkspace", {255, 255, 255, 1}, {18, 18, 18, 1}},
      {"scrollbar", {255, 255, 255, 1}, {18, 18, 18, 1}},
  };
  for (size_t i = 0; i < sizeof palette / sizeof *palette; i++) {
    if (strcmp(w, palette[i].name) == 0) {
      for (int k = 0; k < 4; k++) {
        rgba[k] = dark ? palette[i].dark[k] : palette[i].light[k];
      }
      return true;
    }
  }
  return false;
}

static int g_palette = 1;
static const char * g_ua_sheet = UA_SHEET;
static size_t g_ua_len = sizeof UA_SHEET - 1;

/* The conditions the environment of the compute context answers `if(media())` with, and the styles a query container is read from, are the world's: the context, the conditions object and
   the document are made in that order and each needs the next, so the callbacks reach them through the world. */
/* The runner is the caller of the library, and its document timeline stands at 0: every animation began there, so the time of each is 0 (the pages that need more are timing pages, which the
   scoring interpreter does not read). With CSSOM_RUNNER_TIMELINE=none it gives no time at all, which is the case the controls show: the property an animation targets then says
   ABSENT_TIMELINE and the test is not credited. The version of the timeline never moves. */
static int g_timeline = 1;

static GTEXT_CSS_Status runner_animation_time(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Anim_Effect * effect, double * time_ms, bool * known) {
  /* the CSS animations the tracker made have the time the tracker gives them (their start is the style change event that made them); one it has not made yet (no style change event of its element) has the
     document timeline's 0, which is where every animation of a page began */
  const world * w = user;
  GTEXT_CSSOM_Transition_Tracker * t = w ? w->tracker : NULL;
  if (t && g_timeline != 0) {
    const GTEXT_CSS_Status st = gtext_cssom_transition_tracker_animation_time(t, element, pseudo, effect, time_ms, known);
    if (st != GTEXT_CSS_OK || *known) {
      return st;
    }
  }
  *time_ms = 0.0;
  *known = g_timeline != 0;
  return GTEXT_CSS_OK;
}

static uint64_t runner_timeline_version(void * user) {
  (void)user;
  return 0;
}

/* The runner is also the caller that keeps the transitions: the library keeps nothing between calls (css_transition.h), so the world has a tracker (css_cssom_computed.h), the document asks it
   for the sets of an element, and the runner holds the style change events: a read of a computed style (`sget` and the others) is one, as a read that wants an up-to-date style is in a
   browser, at the document timeline's 0 like every other time here. CSSOM_RUNNER_TRANSITIONS=none keeps no tracker: nothing then starts, which is the case the controls show. */
static int g_transitions = 1;

static GTEXT_CSS_Status runner_transitions(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Transition_Set ** set, double * time_ms) {
  const world * w = user;
  *set = NULL;
  *time_ms = 0.0;
  if (!w || !w->tracker) {
    return GTEXT_CSS_OK;
  }
  return gtext_cssom_transition_tracker_lookup(w->tracker, element, pseudo, set, time_ms);
}

static uint64_t runner_transition_version(void * user) {
  const world * w = user;
  return w && w->tracker ? gtext_cssom_transition_tracker_version(w->tracker) : 0;
}

static GTEXT_CSS_Tristate runner_condition(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element) {
  (void)element;
  const world * w = user;
  return gtext_css_conditions_condition(w->conditions, kind, text);
}

static GTEXT_CSS_Status runner_condition_status(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Tristate * answer,
    const char ** detail) {
  const world * w = user;
  return gtext_css_conditions_condition_element(w->conditions, kind, text, element, NULL, answer, detail);
}

static const GTEXT_CSS_Computed_Style * runner_container_style(void * user, GTEXT_CSS_Element_Handle element) {
  const world * w = user;
  GTEXT_CSS_Container_Interface ci;
  if (!w->doc || gtext_cssom_document_container_interface(w->doc, &ci) != GTEXT_CSS_OK) {
    return NULL;
  }
  return ci.computed_style(ci.user, element);
}

static int is_element(const GTEXT_Markup_Node * n);
static int local_is(const GTEXT_Markup_Node * n, const char * name);
static int attr_get(const GTEXT_Markup_Node * n, const char * name, const char ** value, size_t * len);

/* ---- shadow trees ---------------------------------------------------------------------------------------------------------------------------- */

/* The element interface is the Markup adapter's for everything but the shadow trees, which the adapter does not see (css_markup.h: a Markup tree has no shadow roots). The callbacks that are
   ours reach the world through this pointer, as the callbacks of the adapter reach the adapter; one world is open at a time. */
static world * g_world;

static GTEXT_Markup_Node * tree_top(const GTEXT_Markup_Node * n) {
  GTEXT_Markup_Node * top = (GTEXT_Markup_Node *)n;
  for (GTEXT_Markup_Node * p = gtext_markup_node_parent(top); p; p = gtext_markup_node_parent(top)) {
    top = p;
  }
  return top;
}

static shadow * shadow_by_root(world * w, const GTEXT_Markup_Node * root) {
  for (size_t i = 0; i < w->nshadows; i++) {
    if (w->shadows[i].root == root) {
      return &w->shadows[i];
    }
  }
  return NULL;
}

static shadow * shadow_by_host(world * w, const GTEXT_Markup_Node * host) {
  for (size_t i = 0; i < w->nshadows; i++) {
    if (w->shadows[i].host == host) {
      return &w->shadows[i];
    }
  }
  return NULL;
}

/* the shadow tree a node is in, or NULL */
static shadow * shadow_of_node(world * w, const GTEXT_Markup_Node * n) {
  return w->nshadows ? shadow_by_root(w, tree_top(n)) : NULL;
}

/* the value of an attribute of an element ("" for none), in a buffer that stays valid as long as the element's attribute does */
static const char * attr_or_empty(const GTEXT_Markup_Node * el, const char * name, size_t * len) {
  const char * v = "";
  size_t l = 0;
  if (!attr_get(el, name, &v, &l)) {
    v = "";
    l = 0;
  }
  if (len) {
    *len = l;
  }
  return v;
}

/* The slot of a shadow tree that an element with this `slot` attribute is assigned to (DOM, "find a slot": the first `slot` element in tree order whose `name` is the same). */
static GTEXT_Markup_Node * shadow_find_slot(const shadow * sh, const char * name, size_t name_len) {
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(sh->root, sh->root, false); n; n = gtext_markup_node_next_in_order(sh->root, n, false)) {
    if (is_element(n) && local_is(n, "slot")) {
      size_t l;
      const char * v = attr_or_empty(n, "name", &l);
      if (l == name_len && memcmp(v, name, l) == 0) {
        return n;
      }
    }
  }
  return NULL;
}

/* the slot an element is assigned to: its parent is a shadow host with a shadow root, and it is an element (a text node is assigned too, and is not asked about here) */
static GTEXT_Markup_Node * assigned_slot_of(world * w, const GTEXT_Markup_Node * el) {
  GTEXT_Markup_Node * p = gtext_markup_node_parent(el);
  if (!w->nshadows || !is_element(p) || !is_element(el)) {
    return NULL;
  }
  const shadow * sh = shadow_by_host(w, p);
  if (!sh) {
    return NULL;
  }
  size_t l;
  const char * v = attr_or_empty(el, "slot", &l);
  return shadow_find_slot(sh, v, l);
}

static const GTEXT_CSS_Element_Interface * base_iface(void) {
  return gtext_css_markup_interface(g_world->adapter);
}

static GTEXT_CSS_Element_Handle cb_inheritance_parent(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  world * w = g_world;
  const GTEXT_CSS_Element_Interface * b = base_iface();
  if (w->nshadows) {
    GTEXT_Markup_Node * slot = assigned_slot_of(w, h);
    if (slot) {
      return slot;   /* the flat tree: a slotted node is a child of its slot */
    }
    if (!is_element(gtext_markup_node_parent(h))) {
      const shadow * sh = shadow_of_node(w, h);
      if (sh) {
        return sh->host;   /* the top of a shadow tree: its parent in the flat tree is the host */
      }
    }
  }
  return b->parent(b->user, h);
}

static const void * cb_tree_scope(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  const shadow * sh = shadow_of_node(g_world, h);
  const GTEXT_CSS_Element_Interface * b = base_iface();
  return sh ? (const void *)sh->root : b->tree_scope(b->user, h);
}

static GTEXT_CSS_Element_Handle cb_containing_host(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  const shadow * sh = shadow_of_node(g_world, h);
  return sh ? sh->host : NULL;
}

static const void * cb_shadow_scope(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  const shadow * sh = g_world->nshadows ? shadow_by_host(g_world, h) : NULL;
  return sh ? (const void *)sh->root : NULL;
}

static GTEXT_CSS_Element_Handle cb_assigned_slot(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  return assigned_slot_of(g_world, h);
}

/* `::part(name)` of a host (CSS Shadow Parts 1): the element is in the host's shadow tree and has the name in its `part` attribute, or it is in the shadow tree of a nested host whose
   `exportparts` forwards one of its part names, as the same name or another, into the host's tree, and so on up to the host asked about. */
static bool cb_has_part(void * user, GTEXT_CSS_Element_Handle h, GTEXT_CSS_Element_Handle host, const GTEXT_CSS_String * name) {
  (void)user;
  world * w = g_world;
  if (!is_element(h)) {
    return false;
  }
  char names[64][64];
  size_t count = 0;
  /* the names the element has from the point of view of its own tree's host */
  {
    size_t n;
    const char * v = attr_or_empty(h, "part", &n);
    size_t i = 0;
    while (i < n && count < 64) {
      while (i < n && isspace((unsigned char)v[i])) {
        i++;
      }
      size_t j = i;
      while (j < n && !isspace((unsigned char)v[j])) {
        j++;
      }
      if (j > i && j - i < 64) {
        memcpy(names[count], v + i, j - i);
        names[count][j - i] = '\0';
        count++;
      }
      i = j;
    }
  }
  const shadow * sh = shadow_of_node(w, h);
  while (sh && sh->host != host) {
    /* forward through sh->host's exportparts into the tree that holds that host */
    char out[64][64];
    size_t nout = 0;
    size_t n;
    const char * v = attr_or_empty(sh->host, "exportparts", &n);
    size_t i = 0;
    while (i < n) {
      size_t j = i;
      while (j < n && v[j] != ',') {
        j++;
      }
      /* "inner" or "inner : outer", white space around either */
      size_t a = i, b = j;
      while (a < b && isspace((unsigned char)v[a])) {
        a++;
      }
      while (b > a && isspace((unsigned char)v[b - 1])) {
        b--;
      }
      size_t colon = a;
      while (colon < b && v[colon] != ':') {
        colon++;
      }
      size_t ie = colon, os = colon < b ? colon + 1 : a, oe = b;
      while (ie > a && isspace((unsigned char)v[ie - 1])) {
        ie--;
      }
      while (os < oe && isspace((unsigned char)v[os])) {
        os++;
      }
      if (ie > a && oe > os && ie - a < 64 && oe - os < 64) {
        for (size_t k = 0; k < count; k++) {
          if (strlen(names[k]) == ie - a && memcmp(names[k], v + a, ie - a) == 0 && nout < 64) {
            memcpy(out[nout], v + os, oe - os);
            out[nout][oe - os] = '\0';
            nout++;
          }
        }
      }
      i = j + 1;
    }
    for (size_t k = 0; k < nout; k++) {
      memcpy(names[k], out[k], sizeof names[k]);
    }
    count = nout;
    sh = shadow_of_node(w, sh->host);
  }
  if (!sh || sh->host != host) {
    return false;
  }
  for (size_t k = 0; k < count; k++) {
    if (strlen(names[k]) == name->length && memcmp(names[k], name->data, name->length) == 0) {
      return true;
    }
  }
  return false;
}

/* Whether the element is connected (its shadow-including root is the document), and part of the flat tree: an element that is a child of a shadow host is only in it when it is assigned to
   a slot that is in it. The bits of GTEXT_CSSOM_Config::element_state; the third, a rendered browsing context, is always true. */
static uint32_t runner_element_state(void * user, const void * element) {
  world * w = user;
  const GTEXT_Markup_Node * doc = gtext_markup_document_root(w->markup);
  uint32_t bits = GTEXT_CSSOM_ELEMENT_IN_RENDERED_CONTEXT;
  bool flat = true, conn = false;
  const GTEXT_Markup_Node * e = element;
  for (size_t guard = 0; guard < 4096; guard++) {
    GTEXT_Markup_Node * p = gtext_markup_node_parent(e);
    if (is_element(p)) {
      if (w->nshadows && shadow_by_host(w, p)) {
        const GTEXT_Markup_Node * slot = assigned_slot_of(w, e);
        if (!slot) {
          flat = false;
          e = p;   /* not in the flat tree: the rest of the question is about the host */
        }
        else {
          e = slot;
        }
      }
      else {
        e = p;
      }
      continue;
    }
    const GTEXT_Markup_Node * top = p ? p : e;
    if (top == doc) {
      conn = true;
      break;
    }
    const shadow * sh = w->nshadows ? shadow_by_root(w, top) : NULL;
    if (!sh) {
      break;
    }
    e = sh->host;
  }
  if (conn) {
    bits |= GTEXT_CSSOM_ELEMENT_CONNECTED;
  }
  if (flat) {
    bits |= GTEXT_CSSOM_ELEMENT_IN_FLAT_TREE;
  }
  return bits;
}

/* the tree version of the config: the Markup document's, and the shadow roots attached (which change the flat tree and nothing the document counts) */
static uint64_t runner_tree_version(void * user) {
  const world * w = user;
  return gtext_markup_document_generation(w->markup) + w->epoch;
}

static void world_interface(world * w) {
  w->iface = *gtext_css_markup_interface(w->adapter);
  w->iface.inheritance_parent = cb_inheritance_parent;
  w->iface.tree_scope = cb_tree_scope;
  w->iface.containing_host = cb_containing_host;
  w->iface.shadow_scope = cb_shadow_scope;
  w->iface.assigned_slot = cb_assigned_slot;
  w->iface.has_part = cb_has_part;
}

/* DOM, "valid shadow host name": an HTML element of one of these names, or a valid custom element name */
static int valid_shadow_host(const GTEXT_Markup_Node * host) {
  static const char * const hosts[] = {"article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span"};
  const char * ln = gtext_markup_element_local_name(host);
  const char * ns = gtext_markup_element_namespace(host);
  if (!ln || !ns || strcmp(ns, GTEXT_MARKUP_NS_HTML) != 0) {
    return 0;
  }
  for (size_t i = 0; i < sizeof hosts / sizeof *hosts; i++) {
    if (strcmp(ln, hosts[i]) == 0) {
      return 1;
    }
  }
  /* a valid custom element name: starts with a lower case letter, has a hyphen, no upper case; and is not one of the names SVG and MathML reserve */
  static const char * const reserved[] = {"annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri", "font-face-format", "font-face-name", "missing-glyph"};
  if (!(ln[0] >= 'a' && ln[0] <= 'z') || !strchr(ln, '-')) {
    return 0;
  }
  for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) {
    if (strcmp(ln, reserved[i]) == 0) {
      return 0;
    }
  }
  for (const char * c = ln; *c; c++) {
    if (*c >= 'A' && *c <= 'Z') {
      return 0;
    }
  }
  return 1;
}

/* Attach a shadow root to a host: a fragment of the document's Markup (`root`, which may be a `<template>`'s content: a declarative shadow root) is registered as the host's tree. */
static GTEXT_CSS_Status shadow_attach(world * w, GTEXT_Markup_Node * host, GTEXT_Markup_Node * root, int open, shadow ** out) {
  if (w->nshadows == w->capshadows) {
    w->capshadows = w->capshadows ? w->capshadows * 2 : 8;
    w->shadows = realloc(w->shadows, w->capshadows * sizeof *w->shadows);
    if (!w->shadows) {
      abort();
    }
  }
  const shadow * outer = shadow_of_node(w, host);
  GTEXT_CSSOM_Scope_Options so = gtext_cssom_scope_options_default();
  so.handle = root;
  so.shadow = true;
  so.parent = outer ? outer->scope : NULL;
  so.implicit_root = host;
  shadow sh;
  memset(&sh, 0, sizeof sh);
  sh.root = root;
  sh.host = host;
  sh.open = open;
  GTEXT_CSS_Status st = gtext_cssom_scope_new(w->doc, &so, &sh.scope);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
  mo.root = root;
  st = gtext_cssom_markup_binding_new(w->doc, sh.scope, w->markup, &mo, &sh.binding);
  if (st != GTEXT_CSS_OK) {
    gtext_cssom_scope_release(sh.scope);
    return st;
  }
  w->shadows[w->nshadows] = sh;
  *out = &w->shadows[w->nshadows++];
  w->epoch++;
  return gtext_cssom_markup_sync(sh.binding, NULL);
}

/* The declarative shadow roots of a parse (HTML, "a start tag whose tag name is template": a template with a `shadowrootmode` attribute that is a child of an element becomes that
   element's shadow root and is not in the tree): the template is taken out of its host and its content is the host's shadow tree. Done for the content of each too. */
static void shadow_adopt_declarative(world * w, GTEXT_Markup_Node * root) {
  GTEXT_Markup_Node ** found = NULL;
  size_t n = 0, cap = 0;
  for (GTEXT_Markup_Node * x = gtext_markup_node_next_in_order(root, root, false); x; x = gtext_markup_node_next_in_order(root, x, false)) {
    if (is_element(x) && local_is(x, "template") && gtext_markup_element_template_content(x)) {
      const char * v;
      size_t l;
      if (attr_get(x, "shadowrootmode", &v, &l) && ((l == 4 && strncasecmp(v, "open", 4) == 0) || (l == 6 && strncasecmp(v, "closed", 6) == 0)) && is_element(gtext_markup_node_parent(x)) && valid_shadow_host(gtext_markup_node_parent(x))) {
        if (n == cap) {
          cap = cap ? cap * 2 : 8;
          found = realloc(found, cap * sizeof *found);
        }
        found[n++] = x;
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    GTEXT_Markup_Node * t = found[i];
    GTEXT_Markup_Node * host = gtext_markup_node_parent(t);
    GTEXT_Markup_Node * content = gtext_markup_element_template_content(t);
    const char * v;
    size_t l;
    attr_get(t, "shadowrootmode", &v, &l);
    shadow * sh = NULL;
    if (shadow_by_host(w, host)) {
      continue;   /* a second one on one host is left where it is (HTML: it is not a shadow root) */
    }
    gtext_markup_remove(t);
    if (shadow_attach(w, host, content, l == 4, &sh) == GTEXT_CSS_OK) {
      sh->declarative = 1;
      shadow_adopt_declarative(w, content);
    }
  }
  free(found);
}

static int world_open(world * w, const char * html, size_t html_len) {
  memset(w, 0, sizeof *w);
  if (gtext_css_registry_new(NULL, &w->registry) != GTEXT_CSS_OK) {
    return 0;
  }
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(NULL);
  env.viewport.width = 1280;
  env.viewport.height = 800;
  env.device.width = 1280;
  env.device.height = 800;
  GTEXT_HTML_Result hr;
  memset(&hr, 0, sizeof hr);
  if (gtext_html_parse(html, html_len, NULL, &w->markup, &hr) != GTEXT_HTML_OK) {
    return 0;
  }
  gtext_html_result_free(&hr);
  if (gtext_css_markup_adapter_new(w->markup, NULL, &w->adapter) != GTEXT_CSS_OK) {
    return 0;
  }
  w->match_options = gtext_css_markup_match_options(w->adapter);
  g_world = w;
  world_interface(w);
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = w->registry;
  c.element_interface = &w->iface;
  c.match_options = &w->match_options;
  c.tree_version = &runner_tree_version;
  c.tree_version_user = w;
  c.element_state = &runner_element_state;
  c.element_state_user = w;
  GTEXT_CSSOM_Layout_Interface layout;
  memset(&layout, 0, sizeof layout);
  layout.size = sizeof layout;
  layout.animation_time = runner_animation_time;
  layout.timeline_version = runner_timeline_version;
  layout.user = w;
  layout.transitions = runner_transitions;
  layout.transition_version = runner_transition_version;
  c.layout = &layout;
  /* the runner is a consumer with an environment of its own (the palette, and conditions that are the world's); the document makes the compute context with it and with the registrations of the
     `@property` rules of its sheets, and the conditions object that reads a query container's style is given to the document once it exists */
  GTEXT_CSS_Environment cenv = gtext_css_environment_default();
  cenv.user = w;
  cenv.condition = runner_condition;
  cenv.condition_status = runner_condition_status;
  if (g_palette) {
    cenv.resolve_color = runner_resolve_color;
  }
  c.environment = &cenv;
  if (gtext_cssom_document_new(&c, &w->doc) != GTEXT_CSS_OK) {
    return 0;
  }
  {
    GTEXT_CSS_Container_Interface container;
    if (gtext_cssom_document_container_interface(w->doc, &container) != GTEXT_CSS_OK) {
      return 0;
    }
    container.user = w;
    container.computed_style = runner_container_style;
    GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
    co.media = &env;
    co.element_interface = c.element_interface;
    co.container = &container;
    if (gtext_css_conditions_new(w->registry, &co, &w->conditions) != GTEXT_CSS_OK || gtext_cssom_document_set_conditions(w->doc, w->conditions, NULL) != GTEXT_CSS_OK) {
      return 0;
    }
  }
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  if (g_ua_len) {
    if (gtext_css_parse_stylesheet(g_ua_sheet, g_ua_len, NULL, &w->ua_tree, &pr) != GTEXT_CSS_OK ||
        gtext_css_sheet_compile(w->ua_tree, w->registry, NULL, &w->ua_sheet, NULL) != GTEXT_CSS_OK ||
        gtext_cssom_document_register_sheet(w->doc, GTEXT_CSS_ORIGIN_USER_AGENT, w->ua_sheet, NULL) != GTEXT_CSS_OK) {
      return 0;
    }
  }
  if (g_transitions && gtext_cssom_transition_tracker_new(w->doc, NULL, &w->tracker) != GTEXT_CSS_OK) {
    return 0;
  }
  if (w->tracker && gtext_anim_world_new(w->doc, w->tracker, NULL, &w->anim) != GTEXT_ANIM_OK) {
    return 0;
  }
  GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
  if (gtext_cssom_markup_binding_new(w->doc, gtext_cssom_document_scope(w->doc), w->markup, &mo, &w->binding) != GTEXT_CSS_OK ||
      gtext_cssom_markup_sync(w->binding, NULL) != GTEXT_CSS_OK) {
    return 0;
  }
  shadow_adopt_declarative(w, gtext_markup_document_root(w->markup));
  w->attr = gtext_cssom_markup_inline_binding();
  w->capelems = w->capstyles = w->capregs = 32;
  w->elems = calloc(w->capelems, sizeof *w->elems);
  w->styles = calloc(w->capstyles, sizeof *w->styles);
  w->regs = calloc(w->capregs, sizeof *w->regs);
  return 1;
}

static void obj_release(object * o);

static void events_free(world * w);

static void world_close(world * w) {
  events_free(w);
  for (size_t i = 0; i < w->nobjs; i++) {
    obj_release(&w->objs[i]);
    free(w->objs[i].name);
  }
  free(w->objs);
  for (size_t i = 0; i < w->nstyles; i++) {
    gtext_cssom_decl_release(w->styles[i].decl);
    free(w->styles[i].name);
    free(w->styles[i].elem_name);
    free(w->styles[i].pseudo);
  }
  for (size_t i = 0; i < w->nelems; i++) {
    free(w->elems[i].name);
  }
  for (size_t i = 0; i < w->nlists; i++) {
    free(w->lists[i].name);
    free(w->lists[i].nodes);
  }
  free(w->lists);
  for (size_t i = 0; i < w->nregs; i++) {
    free(w->regs[i].name);
    free(w->regs[i].layout);
    free(w->regs[i].timeline);
    value_free(&w->regs[i].v);
  }
  free(w->elems);
  free(w->styles);
  free(w->regs);
  for (size_t i = 0; i < w->nshadows; i++) {
    gtext_cssom_markup_binding_free(w->shadows[i].binding);
    gtext_cssom_scope_release(w->shadows[i].scope);
  }
  free(w->shadows);
  gtext_cssom_markup_binding_free(w->binding);
  for (size_t i = 0; i < w->nahs; i++) {
    free(w->ahs[i].name);
    free(w->ahs[i].pname);
    free(w->ahs[i].easing);
    free(w->ahs[i].idtext);
  }
  free(w->ahs);
  for (size_t i = 0; i < w->nalists; i++) {
    for (size_t k = 0; k < w->alists[i].n; k++) {
      free(w->alists[i].items[k].name);
      free(w->alists[i].items[k].pname);
      free(w->alists[i].items[k].easing);
      free(w->alists[i].items[k].idtext);
    }
    free(w->alists[i].items);
    free(w->alists[i].name);
  }
  free(w->alists);
  for (size_t i = 0; i < w->nctls; i++) {
    if (w->ctls[i].orphan) {
      gtext_anim_animation_free(w->ctls[i].a);
    }
    free(w->ctls[i].p[0]);
    free(w->ctls[i].p[1]);
  }
  free(w->ctls);
  for (size_t i = 0; i < w->neffs; i++) {
    free(w->effs[i].name);
  }
  free(w->effs);
  for (size_t i = 0; i < w->nkfls; i++) {
    free(w->kfls[i].name);
    gtext_anim_keyframes_free(w->kfls[i].k);
  }
  free(w->kfls);
  gtext_anim_world_free(w->anim);
  gtext_cssom_transition_tracker_free(w->tracker);
  gtext_cssom_document_release(w->doc);
  gtext_css_sheet_free(w->ua_sheet);
  gtext_css_tree_free(w->ua_tree);
  gtext_css_conditions_free(w->conditions);
  gtext_css_markup_adapter_free(w->adapter);
  gtext_markup_document_free(w->markup);
  gtext_css_registry_free(w->registry);
  memset(w, 0, sizeof *w);
}

static elem * find_elem(world * w, const char * name) {
  for (size_t i = w->nelems; i-- > 0;) {
    if (strcmp(w->elems[i].name, name) == 0) {
      return &w->elems[i];
    }
  }
  return NULL;
}

static void put_elem(world * w, const char * name, GTEXT_Markup_Node * node) {
  elem * e = find_elem(w, name);
  if (!e) {
    if (w->nelems == w->capelems) {
      w->capelems *= 2;
      w->elems = realloc(w->elems, w->capelems * sizeof *w->elems);
    }
    e = &w->elems[w->nelems++];
    e->name = dups(name);
  }
  e->node = node;
}

static nodelist * find_list(world * w, const char * name) {
  for (size_t i = w->nlists; i-- > 0;) {
    if (strcmp(w->lists[i].name, name) == 0) {
      return &w->lists[i];
    }
  }
  return NULL;
}

/* takes ownership of `nodes` */
static void put_list(world * w, const char * name, GTEXT_Markup_Node ** nodes, size_t n) {
  nodelist * l = find_list(w, name);
  if (l) {
    free(l->nodes);
  }
  else {
    if (w->nlists == w->caplists) {
      w->caplists = w->caplists ? w->caplists * 2 : 8;
      w->lists = realloc(w->lists, w->caplists * sizeof *w->lists);
    }
    l = &w->lists[w->nlists++];
    l->name = dups(name);
  }
  l->nodes = nodes;
  l->n = n;
}

static style * find_style(world * w, const char * name) {
  for (size_t i = w->nstyles; i-- > 0;) {
    if (strcmp(w->styles[i].name, name) == 0) {
      return &w->styles[i];
    }
  }
  return NULL;
}

static void put_style(world * w, const char * name, GTEXT_CSSOM_Decl * d, int computed, GTEXT_Markup_Node * node, const char * elem_name, const char * pseudo) {
  if (w->nstyles == w->capstyles) {
    w->capstyles *= 2;
    w->styles = realloc(w->styles, w->capstyles * sizeof *w->styles);
  }
  style * s = &w->styles[w->nstyles++];
  s->name = dups(name);
  s->decl = d;
  s->computed = computed;
  s->node = node;
  s->elem_name = dups(elem_name);
  s->pseudo = dups(pseudo);
}

static reg * find_reg(world * w, const char * name) {
  for (size_t i = w->nregs; i-- > 0;) {
    if (strcmp(w->regs[i].name, name) == 0) {
      return &w->regs[i];
    }
  }
  return NULL;
}

static void put_reg(world * w, const char * name, value v) {
  reg * r = find_reg(w, name);
  if (r) {
    value_free(&r->v);
    r->v = v;
    free(r->layout);
    free(r->timeline);
    r->layout = r->timeline = NULL;
    return;
  }
  if (w->nregs == w->capregs) {
    w->capregs *= 2;
    w->regs = realloc(w->regs, w->capregs * sizeof *w->regs);
  }
  w->regs[w->nregs].name = dups(name);
  w->regs[w->nregs].v = v;
  w->regs[w->nregs].layout = NULL;
  w->regs[w->nregs].timeline = NULL;
  w->nregs++;
}

/* The operand of an op. The value is a copy the caller frees. */
static object * find_obj(world * w, const char * name);

static animh * find_ah(world * w, const char * name);
static GTEXT_Markup_Node * node_of_handle(world * w, const void * handle);
static effh * find_eh(world * w, const char * name);

static value operand(world * w, const char * f, size_t l, int * ok) {
  *ok = 1;
  if (l >= 2 && f[1] == ':') {
    switch (f[0]) {
    case 's':
      return val('s', f + 2, l - 2);
    case 'n':
      return val('n', f + 2, l - 2);
    case 'b':
      return val('b', f + 2, l - 2);
    case 'N':
      return val('N', "", 0);
    case 'u':
      return val('u', "", 0);
    case 'e': {
      /* an element: two lookups that found the same node are the same object; a lookup that found nothing is null */
      elem * e = find_elem(w, f + 2);
      if (e) {
        if (!e->node) {
          return val('N', "", 0);
        }
        char id[32];
        snprintf(id, sizeof id, "%p", (void *)e->node);
        return val('e', id, strlen(id));
      }
      break;
    }
    case 'o': {
      /* a sheet, a rule or a media list: two lookups that found the same object are the same object */
      object * ob = find_obj(w, f + 2);
      if (ob) {
        if (ob->state != 0 || !ob->ptr) {
          return val(ob->state == 1 ? 'N' : 'u', "", 0);
        }
        char id[32];
        snprintf(id, sizeof id, "%p", ob->ptr);
        return val('e', id, strlen(id));
      }
      break;
    }
    case 'a': {
      /* an Animation object: two handles that found the same animation are the same object (the binding has one for each) */
      animh * h = find_ah(w, f + 2);
      if (h) {
        if (h->missing) {
          return val('u', "", 0);
        }
        char id[64];
        snprintf(id, sizeof id, "animation:%d:%llu", (int)h->kind, (unsigned long long)h->id);
        return val('e', id, strlen(id));
      }
      break;
    }
    case 'f': {
      /* an effect: two handles that are the same effect are the same object; an animation with no effect has null */
      effh * h = find_eh(w, f + 2);
      if (h) {
        if (!h->e) {
          return val('N', "", 0);
        }
        char id[40];
        snprintf(id, sizeof id, "effect:%p", (void *)h->e);
        return val('e', id, strlen(id));
      }
      break;
    }
    case 'r': {
      reg * r = find_reg(w, f + 2);
      if (r) {
        return val(r->v.kind, r->v.s, r->v.n);
      }
      break;
    }
    default:
      break;
    }
  }
  *ok = 0;
  return val('u', "", 0);
}

/* ---- the Markup tree -------------------------------------------------------------------------------------------------------------------- */

static int is_element(const GTEXT_Markup_Node * n) {
  return n && gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT;
}

static int local_is(const GTEXT_Markup_Node * n, const char * name) {
  const char * l = gtext_markup_element_local_name(n);
  return l && strcmp(l, name) == 0;
}

static int attr_get(const GTEXT_Markup_Node * n, const char * name, const char ** value, size_t * len) {
  GTEXT_Markup_Attribute a;
  if (!gtext_markup_element_get_attribute(n, NULL, name, &a)) {
    return 0;
  }
  *value = a.value;
  *len = a.value_length;
  return 1;
}

static GTEXT_Markup_Node * root_element(world * w) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  for (GTEXT_Markup_Node * n = gtext_markup_node_first_child(root); n; n = gtext_markup_node_next_sibling(n)) {
    if (is_element(n)) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * find_by_local(world * w, const char * name) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, 0); n; n = gtext_markup_node_next_in_order(root, n, 0)) {
    if (is_element(n) && local_is(n, name)) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * find_by_id(world * w, const char * id, size_t len) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w->markup);
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, 0); n; n = gtext_markup_node_next_in_order(root, n, 0)) {
    const char * v;
    size_t vl;
    if (is_element(n) && attr_get(n, "id", &v, &vl) && vl == len && memcmp(v, id, len) == 0) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * element_sibling(GTEXT_Markup_Node * n, int forward) {
  for (GTEXT_Markup_Node * s = forward ? gtext_markup_node_next_sibling(n) : gtext_markup_node_previous_sibling(n); s;
       s = forward ? gtext_markup_node_next_sibling(s) : gtext_markup_node_previous_sibling(s)) {
    if (is_element(s)) {
      return s;
    }
  }
  return NULL;
}

/* ---- the declaration ops -------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  int failed;
  char * message;
  int layout;
  char * layout_property;
  int timeline;
  char * timeline_property;
} testrun;

/* `backgroundColor` -> `background-color`; `cssFloat` -> `float`; `webkitX` -> `-webkit-x`. */
static char * idl_to_css(const char * s, size_t n) {
  char * out = malloc(n * 2 + 3);
  size_t o = 0;
  if (n == 8 && memcmp(s, "cssFloat", 8) == 0) {
    return dups("float");
  }
  if (memchr(s, '-', n)) {
    return dupn(s, n);
  }
  if ((n > 6 && memcmp(s, "webkit", 6) == 0 && isupper((unsigned char)s[6])) || (n > 3 && memcmp(s, "moz", 3) == 0 && isupper((unsigned char)s[3])) ||
      (n > 2 && memcmp(s, "ms", 2) == 0 && isupper((unsigned char)s[2]))) {
    out[o++] = '-';
  }
  for (size_t i = 0; i < n; i++) {
    if (isupper((unsigned char)s[i])) {
      out[o++] = '-';
      out[o++] = (char)tolower((unsigned char)s[i]);
    }
    else {
      out[o++] = s[i];
    }
  }
  out[o] = '\0';
  return out;
}

/* A read of `css_name` into the register `reg_name`: the test being run is told when the value is one that needs a layout or a timeline, and so is the register, which tells the test that asserts on it
 * later (note_taint()). */
static void note_layout(world * w, testrun * t, style * s, const char * css_name, const char * reg_name) {
  if (!s->computed) {
    return;
  }
  GTEXT_CSSOM_Origin origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  bool found = false;
  if (gtext_cssom_computed_origin(s->decl, css_name, strlen(css_name), &origin, &found) != GTEXT_CSS_OK || !found) {
    return;
  }
  reg * r = reg_name ? find_reg(w, reg_name) : NULL;
  if (origin == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT) {
    if (!t->layout) {
      t->layout = 1;
      t->layout_property = dups(css_name);
    }
    if (r && !r->layout) {
      r->layout = dups(css_name);
    }
  }
  if (origin == GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE) {
    if (!t->timeline) {
      t->timeline = 1;
      t->timeline_property = dups(css_name);
    }
    if (r && !r->timeline) {
      r->timeline = dups(css_name);
    }
  }
}

/* An operand that is a register read before the test: what it was read from is what the test needs. */
static int g_taint = 1;   /* CSSOM_RUNNER_TAINT=none: a register carries nothing of where it was read from (the control that shows the taint is what credits no test of a layout) */

static void note_taint(world * w, testrun * t, const char * operand_field) {
  if (!g_taint || !operand_field || operand_field[0] != 'r' || operand_field[1] != ':') {
    return;
  }
  const reg * r = find_reg(w, operand_field + 2);
  if (!r) {
    return;
  }
  if (r->layout && !t->layout) {
    t->layout = 1;
    t->layout_property = dups(r->layout);
  }
  if (r->timeline && !t->timeline) {
    t->timeline = 1;
    t->timeline_property = dups(r->timeline);
  }
}

/* A register made from others (cmp, not) is as tainted as they are. */
static void inherit_taint(world * w, const char * dest, const char * a, const char * b) {
  reg * d = find_reg(w, dest);
  if (!d) {
    return;
  }
  const char * from[2] = {a, b};
  for (int i = 0; i < 2; i++) {
    if (!from[i] || from[i][0] != 'r' || from[i][1] != ':') {
      continue;
    }
    const reg * r = find_reg(w, from[i] + 2);
    if (r && r->layout && !d->layout) {
      d->layout = dups(r->layout);
    }
    if (r && r->timeline && !d->timeline) {
      d->timeline = dups(r->timeline);
    }
  }
}

static value take_string(world * w, char * buf, size_t n) {
  value v = val('s', buf ? buf : "", buf ? n : 0);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), buf);
  return v;
}

static GTEXT_CSSOM_Document * g_doc;   /* the document of the program being run, for the name of what a failed read was missing */

static char * status_failure(const char * what, GTEXT_CSS_Status st) {
  if (st == GTEXT_CSS_E_UNSUPPORTED && g_doc) {
    GTEXT_CSSOM_Computed_Failure f;
    memset(&f, 0, sizeof f);
    if (gtext_cssom_computed_failure(g_doc, &f) == GTEXT_CSS_OK && f.callback[0]) {
      return fmt("%s: unsupported, needs %s", what, f.callback);
    }
  }
  {
    /* an exception the page's script would catch is written "Name: what", which is what assert_throws_dom() is given */
    const char * exception = gtext_cssom_exception_name(st);
    if (exception) {
      return fmt("%s: %s", exception, what);
    }
  }
  return fmt("%s: %s", what, gtext_css_status_string(st));
}

/* ---- the harness assertions --------------------------------------------------------------------------------------------------------------------- */

static void fmt_value(const value * v, char * out, size_t cap) {
  switch (v->kind) {
  case 's': {
    size_t o = 0;
    if (cap < 8) {
      return;
    }
    out[o++] = '"';
    for (size_t i = 0; i < v->n && o + 4 < cap; i++) {
      if (v->s[i] == '"' || v->s[i] == '\\') {
        out[o++] = '\\';
      }
      out[o++] = v->s[i];
    }
    out[o++] = '"';
    out[o] = '\0';
    break;
  }
  case 'N':
    snprintf(out, cap, "null");
    break;
  case 'u':
    snprintf(out, cap, "undefined");
    break;
  case 'e':
    snprintf(out, cap, "object \"[object Element]\"");
    break;
  default:
    snprintf(out, cap, "%s", v->s);
    break;
  }
}

/* signed: testharness.js's same_value() (assert_equals, assert_not_equals) tells +0 from -0; `===` does not */
static int same_as(const value * a, const value * b, int signed_zero) {
  if (a->kind != b->kind) {
    return 0;
  }
  if (a->kind == 'n') {
    const double x = strtod(a->s, NULL), y = strtod(b->s, NULL);
    if (signed_zero && x == 0 && y == 0) {
      return signbit(x) == signbit(y);
    }
    return x == y || (x != x && y != y);
  }
  if (a->kind == 'N' || a->kind == 'u') {
    return 1;
  }
  return a->n == b->n && memcmp(a->s, b->s, a->n) == 0;
}

static int same(const value * a, const value * b) {
  return same_as(a, b, 0);
}

static char * mismatch(const char * name, const char * msg, const value * expected, const value * got) {
  char e[400], g[400];
  fmt_value(expected, e, sizeof e);
  fmt_value(got, g, sizeof g);
  return fmt("%s: %s%sexpected %s but got %s", name, msg, *msg ? " " : "", e, g);
}

/* ---- the document ops: creating, inserting, removing and reading nodes ------------------------------------------------------------------------ */

static char * lower_ascii(const char * s, size_t n) {
  char * d = dupn(s, n);
  for (char * p = d; *p; p++) {
    if (*p >= 'A' && *p <= 'Z') {
      *p = (char)(*p + 32);
    }
  }
  return d;
}

/* the exception a Markup status stands for, as the page's script would see it */
static char * markup_exception(const char * what, GTEXT_Markup_Status ms) {
  switch (ms) {
  case GTEXT_MARKUP_E_HIERARCHY:
    return fmt("HierarchyRequestError: %s: the new child cannot be inserted there", what);
  case GTEXT_MARKUP_E_NAME:
    return fmt("InvalidCharacterError: %s: the name is not valid", what);
  case GTEXT_MARKUP_E_WRONG_DOCUMENT:
    return fmt("WrongDocumentError: %s", what);
  default:
    return fmt("%s failed: %d", what, (int)ms);
  }
}

/* after a change of the tree the binding re-reads the style elements (it returns at once when nothing changed) */
static void sync_world(world * w) {
  if (w->binding) {
    gtext_cssom_markup_sync(w->binding, NULL);
  }
  for (size_t i = 0; i < w->nshadows; i++) {
    gtext_cssom_markup_sync(w->shadows[i].binding, NULL);
  }
}

static int node_is_inclusive_ancestor(const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * n) {
  for (; n; n = gtext_markup_node_parent(n)) {
    if (n == a) {
      return 1;
    }
  }
  return 0;
}

/* connected: the shadow-including root is the document (a node in a shadow tree is connected when its host is) */
static int node_connected(world * w, const GTEXT_Markup_Node * n) {
  for (size_t guard = 0; guard < 4096; guard++) {
    const GTEXT_Markup_Node * top = tree_top(n);
    if (top == gtext_markup_document_root(w->markup)) {
      return 1;
    }
    const shadow * sh = shadow_by_root(w, top);
    if (!sh) {
      return 0;
    }
    n = sh->host;
  }
  return 0;
}

/* An argument that is a node or a string (the argument list of append(), before(), replaceChildren()...): a string is made a text node, null is the text "null". Returns the node. */
static GTEXT_Markup_Node * argument_node(world * w, const char * f, size_t l, char ** failure) {
  *failure = NULL;
  GTEXT_Markup_Node * n = NULL;
  if (l >= 2 && f[0] == 'e' && f[1] == ':') {
    elem * e = find_elem(w, f + 2);
    if (!e) {
      *failure = dups("the op names an element the program did not make");
      return NULL;
    }
    if (e->node) {
      return e->node;
    }
    GTEXT_Markup_Status ms = gtext_markup_create_text(w->markup, "null", 4, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("createTextNode", ms);
    }
    return n;
  }
  if (l >= 2 && f[0] == 's' && f[1] == ':') {
    GTEXT_Markup_Status ms = gtext_markup_create_text(w->markup, f + 2, l - 2, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("createTextNode", ms);
    }
    return n;
  }
  *failure = dups("the argument of the op is neither a node nor a string");
  return NULL;
}

/* "convert nodes into a node": one node as it is, several in a fragment */
static GTEXT_Markup_Node * argument_nodes(world * w, const op * o, size_t first, char ** failure) {
  *failure = NULL;
  if (o->n == first + 1) {
    return argument_node(w, o->f[first], o->l[first], failure);
  }
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_Markup_Status ms = gtext_markup_create_fragment(w->markup, &frag);
  if (ms != GTEXT_MARKUP_OK) {
    *failure = markup_exception("createDocumentFragment", ms);
    return NULL;
  }
  for (size_t i = first; i < o->n; i++) {
    GTEXT_Markup_Node * n = argument_node(w, o->f[i], o->l[i], failure);
    if (!n) {
      return NULL;
    }
    ms = gtext_markup_append_child(frag, n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("append", ms);
      return NULL;
    }
  }
  return frag;
}

static const char * node_name_of(const GTEXT_Markup_Node * n, char ** owned) {
  *owned = NULL;
  switch (gtext_markup_node_kind(n)) {
  case GTEXT_MARKUP_KIND_DOCUMENT:
    return "#document";
  case GTEXT_MARKUP_KIND_FRAGMENT:
    return "#document-fragment";
  case GTEXT_MARKUP_KIND_TEXT:
    return "#text";
  case GTEXT_MARKUP_KIND_CDATA:
    return "#cdata-section";
  case GTEXT_MARKUP_KIND_COMMENT:
    return "#comment";
  case GTEXT_MARKUP_KIND_DOCTYPE: {
    const char * d = gtext_markup_doctype_name(n);
    return d ? d : "";
  }
  case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
    const char * d = gtext_markup_pi_target(n);
    return d ? d : "";
  }
  case GTEXT_MARKUP_KIND_ELEMENT: {
    /* the qualified name; an element of the HTML namespace in an HTML document is upper case */
    const char * local = gtext_markup_element_local_name(n);
    const char * prefix = gtext_markup_element_prefix(n);
    const char * ns = gtext_markup_element_namespace(n);
    char * q = prefix && *prefix ? fmt("%s:%s", prefix, local) : dups(local);
    if (ns && strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0) {
      for (char * p = q; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
          *p = (char)(*p - 32);
        }
      }
    }
    *owned = q;
    return q;
  }
  default:
    return "";
  }
}

static int node_type_of(const GTEXT_Markup_Node * n) {
  switch (gtext_markup_node_kind(n)) {
  case GTEXT_MARKUP_KIND_ELEMENT:
    return 1;
  case GTEXT_MARKUP_KIND_TEXT:
    return 3;
  case GTEXT_MARKUP_KIND_CDATA:
    return 4;
  case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
    return 7;
  case GTEXT_MARKUP_KIND_COMMENT:
    return 8;
  case GTEXT_MARKUP_KIND_DOCUMENT:
    return 9;
  case GTEXT_MARKUP_KIND_DOCTYPE:
    return 10;
  case GTEXT_MARKUP_KIND_FRAGMENT:
    return 11;
  default:
    return 0;
  }
}

static int is_character_data(const GTEXT_Markup_Node * n) {
  const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
  return k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA || k == GTEXT_MARKUP_KIND_COMMENT || k == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION;
}

static void put_bool(world * w, const char * reg, int b) {
  put_reg(w, reg, val('b', b ? "true" : "false", b ? 4 : 5));
}

static void put_num(world * w, const char * reg, size_t n) {
  char buf[32];
  snprintf(buf, sizeof buf, "%zu", n);
  put_reg(w, reg, val('n', buf, strlen(buf)));
}

static GTEXT_Markup_Node * first_child_of(GTEXT_Markup_Node * n) {
  return gtext_markup_node_first_child(n);
}

/* the children of an element, in order, as a list to hold while they are moved */
static GTEXT_Markup_Node ** child_list(GTEXT_Markup_Node * parent, size_t * count) {
  size_t n = 0;
  for (GTEXT_Markup_Node * c = first_child_of(parent); c; c = gtext_markup_node_next_sibling(c)) {
    n++;
  }
  GTEXT_Markup_Node ** list = malloc((n ? n : 1) * sizeof *list);
  size_t i = 0;
  for (GTEXT_Markup_Node * c = first_child_of(parent); c; c = gtext_markup_node_next_sibling(c)) {
    list[i++] = c;
  }
  *count = n;
  return list;
}

/* Replace all the children of `parent` with `node` (NULL: with nothing), as the DOM's "replace all": on a refusal the tree is as it was. */
static char * replace_all(GTEXT_Markup_Node * parent, GTEXT_Markup_Node * node, const char * what) {
  size_t count;
  GTEXT_Markup_Node ** old = child_list(parent, &count);
  for (size_t i = 0; i < count; i++) {
    gtext_markup_remove(old[i]);
  }
  GTEXT_Markup_Status ms = node ? gtext_markup_append_child(parent, node) : GTEXT_MARKUP_OK;
  if (ms != GTEXT_MARKUP_OK) {
    for (size_t i = 0; i < count; i++) {
      gtext_markup_append_child(parent, old[i]);
    }
    free(old);
    return markup_exception(what, ms);
  }
  free(old);
  return NULL;
}

static GTEXT_HTML_Namespace html_namespace_of(const GTEXT_Markup_Node * el) {
  const char * ns = gtext_markup_element_namespace(el);
  if (ns && strcmp(ns, "http://www.w3.org/2000/svg") == 0) {
    return GTEXT_HTML_NS_SVG;
  }
  if (ns && strcmp(ns, "http://www.w3.org/1998/Math/MathML") == 0) {
    return GTEXT_HTML_NS_MATHML;
  }
  return GTEXT_HTML_NS_HTML;
}

/* innerHTML = text: the fragment parsing algorithm in the context of the element, the result cloned into the document and put in place of the children */
static char * set_inner_html(world * w, GTEXT_Markup_Node * el, const char * text, size_t len) {
  GTEXT_HTML_Fragment_Context ctx;
  memset(&ctx, 0, sizeof ctx);
  /* a shadow root's innerHTML is parsed in the context of its host */
  const shadow * home = gtext_markup_node_kind(el) == GTEXT_MARKUP_KIND_FRAGMENT ? shadow_by_root(w, el) : NULL;
  GTEXT_Markup_Node * context = home ? home->host : el;
  ctx.ns = html_namespace_of(context);
  ctx.local_name = gtext_markup_element_local_name(context);
  ctx.quirks = gtext_markup_document_mode(w->markup);
  for (const GTEXT_Markup_Node * p = el; p; p = gtext_markup_node_parent(p)) {
    if (gtext_markup_node_kind(p) == GTEXT_MARKUP_KIND_ELEMENT && strcmp(gtext_markup_element_local_name(p), "form") == 0) {
      ctx.form_ancestor = true;
    }
  }
  GTEXT_Markup_Document * scratch = NULL;
  GTEXT_Markup_Node * frag = NULL;
  GTEXT_HTML_Result hr;
  memset(&hr, 0, sizeof hr);
  GTEXT_HTML_Status hs = gtext_html_parse_fragment(&ctx, text, len, NULL, &scratch, &frag, &hr);
  gtext_html_result_free(&hr);
  if (hs != GTEXT_HTML_OK) {
    return fmt("innerHTML: the fragment parse failed: %d", (int)hs);
  }
  GTEXT_Markup_Node * copy = NULL;
  GTEXT_Markup_Status ms = gtext_markup_clone(w->markup, frag, true, &copy);
  gtext_markup_document_free(scratch);
  if (ms != GTEXT_MARKUP_OK) {
    return markup_exception("innerHTML", ms);
  }
  GTEXT_Markup_Node * target = el;
  if (!home && strcmp(gtext_markup_element_local_name(el), "template") == 0 && gtext_markup_element_template_content(el)) {
    target = gtext_markup_element_template_content(el);
  }
  return replace_all(target, copy, "innerHTML");
}

static char * serialize_to_reg(world * w, const char * reg, GTEXT_Markup_Node * n, GTEXT_HTML_Serialize_Scope scope) {
  char * out = NULL;
  size_t len = 0;
  GTEXT_HTML_Serialize_Result sr;
  memset(&sr, 0, sizeof sr);
  GTEXT_HTML_Status hs = gtext_html_serialize_buffer(n, scope, NULL, &out, &len, &sr);
  if (hs != GTEXT_HTML_OK) {
    return fmt("serialization failed: %d", (int)hs);
  }
  put_reg(w, reg, val('s', out, len));
  gtext_html_serialize_buffer_free(w->markup, NULL, out);
  return NULL;
}

/* A node's own place among the lists a script reads: the child at INDEX of the elements (`children`) or of all the nodes (`childNodes`), and how many. */
static GTEXT_Markup_Node * child_at(GTEXT_Markup_Node * parent, int elements_only, size_t index, size_t * total) {
  size_t i = 0;
  GTEXT_Markup_Node * found = NULL;
  for (GTEXT_Markup_Node * c = first_child_of(parent); c; c = gtext_markup_node_next_sibling(c)) {
    if (elements_only && !is_element(c)) {
      continue;
    }
    if (i == index && !found) {
      found = c;
    }
    i++;
  }
  if (total) {
    *total = i;
  }
  return found;
}

/* The document ops. Returns 1 when the op was one of these (its failure, if any, in *failure). */
static int dom_op(world * w, const op * o, int * program_error, char ** failure) {
  const char * name = o->f[0];
  *failure = NULL;
#define NEEDN(n_) do { if (o->n != (n_)) { *program_error = 1; *failure = fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); return 1; } } while (0)
#define NODE(var, fld) elem * var = find_elem(w, o->f[fld]); if (!var) { *program_error = 1; *failure = fmt("the op %s names the node %s, which the program did not make", name, o->f[fld]); return 1; } \
  if (!var->node) { *failure = dups("TypeError: the node is null"); return 1; }
  GTEXT_Markup_Status ms;
  if (strcmp(name, "create") == 0) {
    NEEDN(4);   /* create E TAG NAMESPACE (the namespace is "" for createElement in an HTML document) */
    char * tag = o->f[3][0] ? dupn(o->f[2], o->l[2]) : lower_ascii(o->f[2], o->l[2]);
    GTEXT_Markup_Node * n = NULL;
    if (!gtext_markup_is_xml_name(tag, strlen(tag))) {
      *failure = fmt("InvalidCharacterError: createElement: the name \"%s\" is not valid", tag);
    }
    else {
      ms = gtext_markup_create_element(w->markup, o->f[3][0] ? o->f[3] : GTEXT_MARKUP_NS_HTML, NULL, tag, &n);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception("createElement", ms);
      }
      else {
        put_elem(w, o->f[1], n);
      }
    }
    free(tag);
    return 1;
  }
  if (strcmp(name, "ctext") == 0 || strcmp(name, "ccomment") == 0) {
    NEEDN(3);
    GTEXT_Markup_Node * n = NULL;
    ms = name[1] == 't' ? gtext_markup_create_text(w->markup, o->f[2], o->l[2], &n) : gtext_markup_create_comment(w->markup, o->f[2], o->l[2], &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception(name[1] == 't' ? "createTextNode" : "createComment", ms);
    }
    else {
      put_elem(w, o->f[1], n);
    }
    return 1;
  }
  if (strcmp(name, "attach") == 0) {
    NEEDN(4);   /* attach R E MODE: element.attachShadow({mode}): R is the shadow root */
    NODE(host, 2);
    if (!is_element(host->node)) {
      *program_error = 1;
      *failure = dups("attach on a node that is not an element");
      return 1;
    }
    const int open = strcmp(o->f[3], "open") == 0;
    if (!open && strcmp(o->f[3], "closed") != 0) {
      *failure = dups("TypeError: attachShadow: the mode is not open or closed");
      return 1;
    }
    /* DOM, "attach a shadow root": an element that is a valid shadow host; one that has a root already is an error, unless that root was made by a template and has this mode, which is
       cleared and returned */
    if (!valid_shadow_host(host->node)) {
      *failure = dups("NotSupportedError: attachShadow: this element does not support attachShadow");
      return 1;
    }
    shadow * existing = shadow_by_host(w, host->node);
    if (existing) {
      if (!existing->declarative || existing->open != open) {
        *failure = dups("NotSupportedError: attachShadow: the element already is a shadow host");
        return 1;
      }
      char * cleared = replace_all(existing->root, NULL, "attachShadow");
      free(cleared);
      existing->declarative = 0;
      sync_world(w);
      put_elem(w, o->f[1], existing->root);
      return 1;
    }
    GTEXT_Markup_Node * root = NULL;
    ms = gtext_markup_create_fragment(w->markup, &root);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("attachShadow", ms);
      return 1;
    }
    shadow * made = NULL;
    GTEXT_CSS_Status st = shadow_attach(w, host->node, root, open, &made);
    if (st != GTEXT_CSS_OK) {
      *failure = status_failure("attachShadow", st);
      return 1;
    }
    put_elem(w, o->f[1], root);
    return 1;
  }
  if (strcmp(name, "cfrag") == 0) {
    NEEDN(2);
    GTEXT_Markup_Node * n = NULL;
    ms = gtext_markup_create_fragment(w->markup, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("createDocumentFragment", ms);
    }
    else {
      put_elem(w, o->f[1], n);
    }
    return 1;
  }
  if (strcmp(name, "clone") == 0) {
    NEEDN(4);   /* clone E SRC DEEP */
    NODE(src, 2);
    GTEXT_Markup_Node * n = NULL;
    ms = gtext_markup_clone(w->markup, src->node, strcmp(o->f[3], "true") == 0, &n);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("cloneNode", ms);
    }
    else {
      put_elem(w, o->f[1], n);
    }
    return 1;
  }
  if (strcmp(name, "append") == 0 || strcmp(name, "prepend") == 0 || strcmp(name, "replacechildren") == 0) {
    if (o->n < 2) {
      *program_error = 1;
      *failure = dups("the op has no parent");
      return 1;
    }
    NODE(parent, 1);
    GTEXT_Markup_Node * node = NULL;
    if (o->n > 2) {
      node = argument_nodes(w, o, 2, failure);
      if (!node) {
        return 1;
      }
    }
    if (name[0] == 'r') {
      *failure = replace_all(parent->node, node, "replaceChildren");
    }
    else if (node) {
      ms = gtext_markup_insert_before(parent->node, node, name[0] == 'p' ? first_child_of(parent->node) : NULL);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception(name, ms);
      }
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "appendchild") == 0 || strcmp(name, "insertbefore") == 0) {
    const int ib = name[0] == 'i';
    NEEDN(ib ? 4 : 3);
    NODE(parent, 1);
    elem * child = find_elem(w, o->f[2]);
    if (!child) {
      *program_error = 1;
      *failure = fmt("the op %s names the node %s, which the program did not make", name, o->f[2]);
      return 1;
    }
    if (!child->node) {
      *failure = dups("TypeError: Failed to execute on 'Node': parameter 1 is not of type 'Node'.");
      return 1;
    }
    GTEXT_Markup_Node * ref = NULL;
    if (ib && strcmp(o->f[3], "N:") != 0) {
      elem * r = find_elem(w, o->f[3] + 2);
      if (!r) {
        *program_error = 1;
        *failure = dups("insertbefore names a reference the program did not make");
        return 1;
      }
      ref = r->node;
    }
    if (ref && gtext_markup_node_parent(ref) != parent->node) {
      *failure = dups("NotFoundError: insertBefore: the reference node is not a child of this node");
      return 1;
    }
    ms = gtext_markup_insert_before(parent->node, child->node, ref);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception(ib ? "insertBefore" : "appendChild", ms);
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "removechild") == 0) {
    NEEDN(3);
    NODE(parent, 1);
    NODE(child, 2);
    if (gtext_markup_node_parent(child->node) != parent->node) {
      *failure = dups("NotFoundError: removeChild: the node is not a child of this node");
      return 1;
    }
    gtext_markup_remove(child->node);
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "replacechild") == 0) {
    NEEDN(4);   /* replacechild PARENT NEW OLD */
    NODE(parent, 1);
    NODE(nw, 2);
    NODE(old, 3);
    if (gtext_markup_node_parent(old->node) != parent->node) {
      *failure = dups("NotFoundError: replaceChild: the node is not a child of this node");
      return 1;
    }
    ms = gtext_markup_replace(old->node, nw->node);
    if (ms != GTEXT_MARKUP_OK) {
      *failure = markup_exception("replaceChild", ms);
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "remove") == 0) {
    NEEDN(2);
    NODE(n, 1);
    gtext_markup_remove(n->node);
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "before") == 0 || strcmp(name, "after") == 0 || strcmp(name, "replacewith") == 0) {
    if (o->n < 2) {
      *program_error = 1;
      *failure = dups("the op has no node");
      return 1;
    }
    NODE(n, 1);
    GTEXT_Markup_Node * parent = gtext_markup_node_parent(n->node);
    if (!parent) {
      return 1;
    }
    /* the nodes of the arguments, to tell a viable sibling from one of them */
    GTEXT_Markup_Node * ref = NULL;
    const int is_arg = 0;
    (void)is_arg;
    if (name[0] == 'b') {
      ref = n->node;
      for (;;) {
        GTEXT_Markup_Node * prev = gtext_markup_node_previous_sibling(ref);
        int arg = 0;
        for (size_t i = 2; prev && i < o->n; i++) {
          if (o->l[i] >= 2 && o->f[i][0] == 'e') {
            elem * e = find_elem(w, o->f[i] + 2);
            arg |= e && e->node == prev;
          }
        }
        if (!arg) {
          break;
        }
        ref = prev;
      }
      ref = gtext_markup_node_previous_sibling(ref);
      ref = ref ? gtext_markup_node_next_sibling(ref) : first_child_of(parent);
    }
    else if (name[0] == 'a') {
      ref = gtext_markup_node_next_sibling(n->node);
      for (;;) {
        int arg = 0;
        for (size_t i = 2; ref && i < o->n; i++) {
          if (o->l[i] >= 2 && o->f[i][0] == 'e') {
            elem * e = find_elem(w, o->f[i] + 2);
            arg |= e && e->node == ref;
          }
        }
        if (!arg) {
          break;
        }
        ref = gtext_markup_node_next_sibling(ref);
      }
    }
    GTEXT_Markup_Node * node = NULL;
    if (o->n > 2) {
      node = argument_nodes(w, o, 2, failure);
      if (!node) {
        return 1;
      }
    }
    if (name[0] == 'r') {
      if (node) {
        ms = gtext_markup_replace(n->node, node);
        if (ms != GTEXT_MARKUP_OK) {
          *failure = markup_exception("replaceWith", ms);
        }
      }
      else {
        gtext_markup_remove(n->node);
      }
    }
    else if (node) {
      ms = gtext_markup_insert_before(parent, node, ref);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception(name, ms);
      }
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "tcget") == 0) {
    NEEDN(3);
    NODE(n, 2);
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n->node);
    if (k == GTEXT_MARKUP_KIND_DOCUMENT || k == GTEXT_MARKUP_KIND_DOCTYPE) {
      put_reg(w, o->f[1], val('N', "", 0));
    }
    else if (is_character_data(n->node)) {
      size_t len = 0;
      const char * d = gtext_markup_node_data(n->node, &len);
      put_reg(w, o->f[1], val('s', d ? d : "", d ? len : 0));
    }
    else {
      char * text = NULL;
      size_t len = 0;
      ms = gtext_markup_node_text_content(n->node, NULL, &text, &len);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception("textContent", ms);
        return 1;
      }
      put_reg(w, o->f[1], val('s', text, len));
      gtext_markup_string_free(NULL, text);
    }
    return 1;
  }
  if (strcmp(name, "tcset") == 0 || strcmp(name, "dset") == 0) {
    NEEDN(3);
    NODE(n, 1);
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n->node);
    if (name[0] == 'd' && !is_character_data(n->node)) {
      return 1;   /* `data` is an ordinary property of any other node: an expando of the script object */
    }
    if (is_character_data(n->node)) {
      ms = gtext_markup_node_set_data(n->node, o->f[2], o->l[2]);
      if (ms != GTEXT_MARKUP_OK) {
        *failure = markup_exception("textContent", ms);
      }
    }
    else if (k == GTEXT_MARKUP_KIND_ELEMENT || k == GTEXT_MARKUP_KIND_FRAGMENT) {
      GTEXT_Markup_Node * text = NULL;
      if (o->l[2]) {
        ms = gtext_markup_create_text(w->markup, o->f[2], o->l[2], &text);
        if (ms != GTEXT_MARKUP_OK) {
          *failure = markup_exception("textContent", ms);
          return 1;
        }
      }
      *failure = replace_all(n->node, text, "textContent");
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "ihget") == 0 || strcmp(name, "ohget") == 0) {
    NEEDN(3);
    NODE(n, 2);
    *failure = serialize_to_reg(w, o->f[1], n->node, name[0] == 'i' ? GTEXT_HTML_SERIALIZE_CHILDREN : GTEXT_HTML_SERIALIZE_OUTER);
    return 1;
  }
  if (strcmp(name, "ihset") == 0 || strcmp(name, "ihsetu") == 0) {
    NEEDN(3);   /* ihset NODE TEXT: innerHTML = TEXT; ihsetu: setHTMLUnsafe(TEXT), which also reads declarative shadow roots */
    NODE(n, 1);
    *failure = set_inner_html(w, n->node, o->f[2], o->l[2]);
    if (!*failure && strcmp(name, "ihsetu") == 0) {
      GTEXT_Markup_Node * tc = is_element(n->node) && local_is(n->node, "template") ? gtext_markup_element_template_content(n->node) : NULL;
      shadow_adopt_declarative(w, tc ? tc : n->node);
    }
    sync_world(w);
    return 1;
  }
  if (strcmp(name, "nread") == 0) {
    NEEDN(4);   /* nread R NODE WHAT */
    NODE(n, 2);
    const char * what = o->f[3];
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n->node);
    if (strcmp(what, "nodeType") == 0) {
      put_num(w, o->f[1], (size_t)node_type_of(n->node));
    }
    else if (strcmp(what, "nodeName") == 0) {
      char * owned;
      const char * s = node_name_of(n->node, &owned);
      put_reg(w, o->f[1], val('s', s, strlen(s)));
      free(owned);
    }
    else if (strcmp(what, "tagName") == 0 || strcmp(what, "localName") == 0 || strcmp(what, "namespaceURI") == 0) {
      /* undefined on a node that is not an element (localName and namespaceURI are null on other nodes, tagName is not there) */
      if (k != GTEXT_MARKUP_KIND_ELEMENT) {
        put_reg(w, o->f[1], what[0] == 't' ? val('u', "", 0) : val('N', "", 0));
      }
      else if (what[0] == 't') {
        char * owned;
        const char * s = node_name_of(n->node, &owned);
        put_reg(w, o->f[1], val('s', s, strlen(s)));
        free(owned);
      }
      else if (what[0] == 'l') {
        const char * s = gtext_markup_element_local_name(n->node);
        put_reg(w, o->f[1], val('s', s, strlen(s)));
      }
      else {
        const char * s = gtext_markup_element_namespace(n->node);
        put_reg(w, o->f[1], s ? val('s', s, strlen(s)) : val('N', "", 0));
      }
    }
    else if (strcmp(what, "nodeValue") == 0 || strcmp(what, "data") == 0) {
      if (is_character_data(n->node)) {
        size_t len = 0;
        const char * d = gtext_markup_node_data(n->node, &len);
        put_reg(w, o->f[1], val('s', d ? d : "", d ? len : 0));
      }
      else {
        put_reg(w, o->f[1], what[0] == 'n' ? val('N', "", 0) : val('u', "", 0));
      }
    }
    else if (strcmp(what, "isConnected") == 0) {
      put_bool(w, o->f[1], node_connected(w, n->node));
    }
    else if (strcmp(what, "hasChildNodes") == 0) {
      put_bool(w, o->f[1], first_child_of(n->node) != NULL);
    }
    else if (strcmp(what, "childElementCount") == 0) {
      size_t total = 0;
      child_at(n->node, 1, (size_t)-1, &total);
      put_num(w, o->f[1], total);
    }
    else {
      *program_error = 1;
      *failure = fmt("nread %s is not known", what);
    }
    return 1;
  }
  if (strcmp(name, "contains") == 0) {
    NEEDN(4);   /* contains R A B (B may be null: false) */
    NODE(a, 2);
    elem * b = find_elem(w, o->f[3]);
    if (!b) {
      *program_error = 1;
      *failure = dups("contains names a node the program did not make");
      return 1;
    }
    put_bool(w, o->f[1], b->node && node_is_inclusive_ancestor(a->node, b->node));
    return 1;
  }
  if (strcmp(name, "matches") == 0 || strcmp(name, "closest") == 0) {
    NEEDN(4);   /* matches R E SEL; closest E2 E SEL */
    NODE(e, 2);
    if (!is_element(e->node)) {
      *program_error = 1;
      *failure = dups("matches on a node that is not an element");
      return 1;
    }
    bool matched = false;
    GTEXT_Markup_Node * found = NULL;
    if (name[0] == 'm') {
      ms = gtext_markup_matches(e->node, o->f[3], o->l[3], NULL, &matched, NULL);
    }
    else {
      ms = gtext_markup_closest(e->node, o->f[3], o->l[3], NULL, &found, NULL);
    }
    if (ms == GTEXT_MARKUP_E_SELECTOR) {
      *failure = dups("SyntaxError: the selector is not valid");
      return 1;
    }
    if (ms != GTEXT_MARKUP_OK) {
      *failure = fmt("%s failed: %d", name, (int)ms);
      return 1;
    }
    if (name[0] == 'm') {
      put_bool(w, o->f[1], matched);
    }
    else {
      put_elem(w, o->f[1], found);
    }
    return 1;
  }
  if (strcmp(name, "qsa") == 0) {
    NEEDN(4);   /* qsa L ROOT SELECTOR: a static NodeList */
    const GTEXT_Markup_Node * scope = gtext_markup_document_root(w->markup);
    if (strcmp(o->f[2], "doc") != 0) {
      NODE(r, 2);
      scope = r->node;
    }
    GTEXT_Markup_Node ** nodes = NULL;
    size_t count = 0;
    ms = gtext_markup_query_selector_all(scope, o->f[3], o->l[3], NULL, &nodes, &count, NULL);
    if (ms == GTEXT_MARKUP_E_SELECTOR) {
      *failure = dups("SyntaxError: the selector is not valid");
      return 1;
    }
    if (ms != GTEXT_MARKUP_OK) {
      *failure = fmt("querySelectorAll failed: %d", (int)ms);
      return 1;
    }
    GTEXT_Markup_Node ** own = malloc((count ? count : 1) * sizeof *own);
    memcpy(own, nodes, count * sizeof *own);
    gtext_markup_query_free(NULL, nodes);
    put_list(w, o->f[1], own, count);
    return 1;
  }
  if (strcmp(name, "llen") == 0 || strcmp(name, "lget") == 0) {
    NEEDN(name[1] == 'l' ? 3 : 4);   /* llen R L; lget E L INDEX */
    nodelist * l = find_list(w, o->f[2]);
    if (!l) {
      *program_error = 1;
      *failure = fmt("the op %s names the list %s, which the program did not make", name, o->f[2]);
      return 1;
    }
    if (name[1] == 'l') {
      put_num(w, o->f[1], l->n);
    }
    else {
      const size_t idx = (size_t)strtoull(o->f[3], NULL, 10);
      put_elem(w, o->f[1], idx < l->n ? l->nodes[idx] : NULL);
    }
    return 1;
  }
  if (strcmp(name, "qlen") == 0) {
    NEEDN(4);   /* qlen R ROOT SELECTOR */
    const GTEXT_Markup_Node * scope = gtext_markup_document_root(w->markup);
    if (strcmp(o->f[2], "doc") != 0) {
      NODE(r, 2);
      scope = r->node;
    }
    GTEXT_Markup_Node ** nodes = NULL;
    size_t count = 0;
    ms = gtext_markup_query_selector_all(scope, o->f[3], o->l[3], NULL, &nodes, &count, NULL);
    if (ms == GTEXT_MARKUP_E_SELECTOR) {
      *failure = dups("SyntaxError: the selector is not valid");
      return 1;
    }
    if (ms != GTEXT_MARKUP_OK) {
      *failure = fmt("querySelectorAll failed: %d", (int)ms);
      return 1;
    }
    gtext_markup_query_free(NULL, nodes);
    put_num(w, o->f[1], count);
    return 1;
  }
  if (strcmp(name, "child") == 0 || strcmp(name, "nchildren") == 0) {
    /* child E PARENT elements|nodes INDEX; nchildren R PARENT elements|nodes */
    const int len_op = name[0] == 'n';
    NEEDN(len_op ? 4 : 5);
    NODE(p, 2);
    const int only_elements = strcmp(o->f[3], "elements") == 0;
    size_t total = 0;
    if (len_op) {
      child_at(p->node, only_elements, (size_t)-1, &total);
      put_num(w, o->f[1], total);
    }
    else {
      put_elem(w, o->f[1], child_at(p->node, only_elements, (size_t)strtoull(o->f[4], NULL, 10), &total));
    }
    return 1;
  }
  return 0;
#undef NEEDN
#undef NODE
}

/* ---- the style sheet ops: sheets, rules, rule lists and media lists as objects of the page ------------------------------------------------------- */

enum { OBJ_SHEET = 1, OBJ_RULE, OBJ_MEDIA, OBJ_MQL };
enum { ST_OBJECT = 0, ST_NULL, ST_UNDEFINED };

static void obj_release(object * o) {
  if (o->state == ST_OBJECT && o->ptr) {
    if (o->kind == OBJ_SHEET) {
      gtext_cssom_sheet_release(o->ptr);
    }
    else if (o->kind == OBJ_RULE) {
      gtext_cssom_rule_release(o->ptr);
    }
    else if (o->kind == OBJ_MEDIA) {
      gtext_cssom_media_release(o->ptr);
    }
    else if (o->kind == OBJ_MQL) {
      gtext_cssom_media_query_list_release(o->ptr);
    }
  }
  o->ptr = NULL;
}

static object * find_obj(world * w, const char * name) {
  for (size_t i = w->nobjs; i-- > 0;) {
    if (strcmp(w->objs[i].name, name) == 0) {
      return &w->objs[i];
    }
  }
  return NULL;
}

/* takes the reference the caller holds (ptr may be NULL for null and undefined) */
static void put_obj(world * w, const char * name, int kind, int state, void * ptr) {
  object * o = find_obj(w, name);
  if (o) {
    obj_release(o);
  }
  else {
    if (w->nobjs == w->capobjs) {
      w->capobjs = w->capobjs ? w->capobjs * 2 : 16;
      w->objs = realloc(w->objs, w->capobjs * sizeof *w->objs);
    }
    o = &w->objs[w->nobjs++];
    o->name = dups(name);
  }
  o->kind = kind;
  o->state = ptr ? ST_OBJECT : state;
  o->ptr = ptr;
}

static void put_found(world * w, const char * name, int kind, void * ptr, int absent) {
  put_obj(w, name, kind, ptr ? ST_OBJECT : absent, ptr);
}

/* the exception a status stands for, as the page's script would see it: "Name: what" */
static char * css_exception(const char * what, GTEXT_CSS_Status st) {
  const char * n = gtext_cssom_exception_name(st);
  if (n) {
    return fmt("%s: %s", n, what);
  }
  return fmt("%s: %s", what, gtext_css_status_string(st));
}

static GTEXT_CSSOM_Scope * world_scope(world * w) {
  return gtext_cssom_document_scope(w->doc);
}

/* a sheet's or rule's list of rules; NULL when the object has none (a rule that is not a grouping rule) */
static GTEXT_CSS_Status rule_list_of(object * o, const GTEXT_CSSOM_Rule_List ** list) {
  *list = NULL;
  if (o->kind == OBJ_SHEET) {
    return gtext_cssom_sheet_rules(o->ptr, list);
  }
  *list = gtext_cssom_rule_css_rules(o->ptr);
  return GTEXT_CSS_OK;
}

static int is_style_or_link(const GTEXT_Markup_Node * n) {
  const char * l = gtext_markup_element_local_name(n);
  const char * ns = gtext_markup_element_namespace(n);
  return l && ns && strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0 && (strcmp(l, "style") == 0 || strcmp(l, "link") == 0);
}

/* The sheet ops. Returns 1 when the op was one of these (its failure, if any, in *failure). */
static int sheet_op(world * w, const op * o, int * program_error, char ** failure) {
  const char * name = o->f[0];
  *failure = NULL;
#define NEEDS(n_) do { if (o->n != (n_)) { *program_error = 1; *failure = fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); return 1; } } while (0)
#define OBJ(var, fld, k_) object * var = find_obj(w, o->f[fld]); \
  if (!var) { *program_error = 1; *failure = fmt("the op %s names the object %s, which the program did not make", name, o->f[fld]); return 1; } \
  if (var->state != ST_OBJECT) { *failure = fmt("TypeError: %s of %s", name, var->state == ST_NULL ? "null" : "undefined"); return 1; } \
  if (var->kind != (k_) && (k_) != 0) { *failure = fmt("TypeError: %s of an object of another kind", name); return 1; }
  GTEXT_CSS_Status st;
  GTEXT_CSSOM_Outcome oc;
  const int absent_default = ST_UNDEFINED;
  (void)absent_default;
  if (strcmp(name, "sheetof") == 0) {
    NEEDS(3);   /* sheetof O E: element.sheet: null for a style or link element without one, undefined on any other element */
    elem * e = find_elem(w, o->f[2]);
    if (!e) {
      *program_error = 1;
      *failure = dups("sheetof names an element the program did not make");
      return 1;
    }
    if (!e->node) {
      *failure = dups("TypeError: the element is null");
      return 1;
    }
    sync_world(w);
    if (!is_element(e->node) || !is_style_or_link(e->node)) {
      put_obj(w, o->f[1], OBJ_SHEET, ST_UNDEFINED, NULL);
    }
    else {
      const shadow * home = shadow_of_node(w, e->node);
      put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_markup_sheet_of(home ? home->binding : w->binding, e->node), ST_NULL);
    }
    return 1;
  }
  if (strcmp(name, "nsheets") == 0) {
    NEEDS(2);
    sync_world(w);
    put_num(w, o->f[1], gtext_cssom_scope_sheet_count(world_scope(w)));
    return 1;
  }
  if (strcmp(name, "sheetat") == 0) {
    NEEDS(4);   /* sheetat O INDEX null|undefined */
    sync_world(w);
    put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_scope_sheet_at(world_scope(w), (size_t)strtoull(o->f[2], NULL, 10)), o->f[3][0] == 'n' ? ST_NULL : ST_UNDEFINED);
    return 1;
  }
  if (strcmp(name, "newsheet") == 0) {
    NEEDS(4);   /* newsheet O MEDIA DISABLED: new CSSStyleSheet({media, disabled}) */
    GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
    if (o->f[2][0] == 's') {
      so.media = o->f[2] + 2;
    }
    so.disabled = strcmp(o->f[3], "b:true") == 0;
    GTEXT_CSSOM_Sheet * sh = NULL;
    st = gtext_cssom_sheet_new_constructed(w->doc, &so, &sh);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("new CSSStyleSheet", st);
      return 1;
    }
    put_obj(w, o->f[1], OBJ_SHEET, ST_OBJECT, sh);
    return 1;
  }
  if (strcmp(name, "replacesync") == 0) {
    NEEDS(3);
    OBJ(sh, 1, OBJ_SHEET);
    st = gtext_cssom_sheet_replace_sync(sh->ptr, o->f[2], o->l[2], &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("replaceSync", st);
    }
    return 1;
  }
  if (strcmp(name, "rlen") == 0) {
    NEEDS(3);   /* rlen R OBJ: cssRules.length */
    OBJ(ob, 2, 0);
    if (ob->kind == OBJ_MEDIA) {
      *failure = dups("TypeError: cssRules of a media list");
      return 1;
    }
    const GTEXT_CSSOM_Rule_List * list;
    st = rule_list_of(ob, &list);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("cssRules", st);
    }
    else if (!list) {
      *failure = dups("TypeError: cssRules is undefined for this rule");
    }
    else {
      put_num(w, o->f[1], gtext_cssom_rule_list_length(list));
    }
    return 1;
  }
  if (strcmp(name, "ruleat") == 0) {
    NEEDS(5);   /* ruleat O OBJ INDEX null|undefined */
    OBJ(ob, 2, 0);
    const GTEXT_CSSOM_Rule_List * list;
    if (ob->kind == OBJ_MEDIA) {
      *failure = dups("TypeError: cssRules of a media list");
      return 1;
    }
    st = rule_list_of(ob, &list);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("cssRules", st);
    }
    else if (!list) {
      *failure = dups("TypeError: cssRules is undefined for this rule");
    }
    else {
      put_found(w, o->f[1], OBJ_RULE, gtext_cssom_rule_list_at(list, (size_t)strtoull(o->f[3], NULL, 10)), o->f[4][0] == 'n' ? ST_NULL : ST_UNDEFINED);
    }
    return 1;
  }
  if (strcmp(name, "klen") == 0 || strcmp(name, "kat") == 0) {
    /* rule.length and rule[i]: the indexed getter of CSSKeyframesRule; undefined on any other rule. klen R RULE; kat O RULE INDEX */
    NEEDS(name[1] == 'l' ? 3 : 4);
    OBJ(ru, 2, OBJ_RULE);
    if (strcmp(gtext_cssom_rule_interface_name(ru->ptr), "CSSKeyframesRule") != 0) {
      if (name[1] == 'l') {
        put_reg(w, o->f[1], val('u', "", 0));
      }
      else {
        put_obj(w, o->f[1], OBJ_RULE, ST_UNDEFINED, NULL);
      }
    }
    else if (name[1] == 'l') {
      put_num(w, o->f[1], gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(ru->ptr)));
    }
    else {
      put_found(w, o->f[1], OBJ_RULE, gtext_cssom_rule_list_at(gtext_cssom_rule_css_rules(ru->ptr), (size_t)strtoull(o->f[3], NULL, 10)), ST_UNDEFINED);
    }
    return 1;
  }
  if (strcmp(name, "rparent") == 0 || strcmp(name, "rsheet") == 0) {
    NEEDS(3);   /* rparent O RULE: parentRule; rsheet O RULE: parentStyleSheet */
    OBJ(ru, 2, OBJ_RULE);
    if (name[1] == 'p') {
      put_found(w, o->f[1], OBJ_RULE, gtext_cssom_rule_parent_rule(ru->ptr), ST_NULL);
    }
    else {
      put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_rule_parent_sheet(ru->ptr), ST_NULL);
    }
    return 1;
  }
  if (strcmp(name, "rstyle") == 0) {
    NEEDS(3);   /* rstyle S RULE: rule.style */
    OBJ(ru, 2, OBJ_RULE);
    GTEXT_CSSOM_Decl * d = NULL;
    st = gtext_cssom_rule_style(ru->ptr, &d);
    if (st != GTEXT_CSS_OK) {
      *failure = fmt("TypeError: this rule has no style (%s)", gtext_css_status_string(st));
      return 1;
    }
    put_style(w, o->f[1], d, 0, NULL, "", "");
    return 1;
  }
  if (strcmp(name, "omedia") == 0) {
    NEEDS(3);   /* omedia M OBJ: media */
    OBJ(ob, 2, 0);
    GTEXT_CSSOM_Media_List * m = NULL;
    st = ob->kind == OBJ_RULE ? gtext_cssom_rule_media(ob->ptr, &m) : ob->kind == OBJ_SHEET ? gtext_cssom_sheet_media(ob->ptr, &m) : GTEXT_CSS_E_INVALID;
    if (st != GTEXT_CSS_OK) {
      put_obj(w, o->f[1], OBJ_MEDIA, ST_UNDEFINED, NULL);
    }
    else {
      put_obj(w, o->f[1], OBJ_MEDIA, ST_OBJECT, m);
    }
    return 1;
  }
  if (strcmp(name, "kfind") == 0) {
    NEEDS(4);
    OBJ(ru, 2, OBJ_RULE);
    GTEXT_CSSOM_Rule * found = NULL;
    st = gtext_cssom_keyframes_find_rule(ru->ptr, o->f[3], o->l[3], &found);
    if (st != GTEXT_CSS_OK) {
      *failure = dups("TypeError: findRule of a rule that is not @keyframes");
      return 1;
    }
    put_found(w, o->f[1], OBJ_RULE, found, ST_NULL);
    return 1;
  }
  if (strcmp(name, "ownernode") == 0) {
    NEEDS(3);   /* ownernode E SHEET: a node, or null */
    OBJ(sh, 2, OBJ_SHEET);
    put_elem(w, o->f[1], (GTEXT_Markup_Node *)gtext_cssom_sheet_owner_node(sh->ptr));
    return 1;
  }
  if (strcmp(name, "ownerrule") == 0) {
    NEEDS(3);
    OBJ(sh, 2, OBJ_SHEET);
    GTEXT_CSSOM_Rule * r = NULL;
    gtext_cssom_sheet_owner_rule(sh->ptr, &r);
    put_found(w, o->f[1], OBJ_RULE, r, ST_NULL);
    return 1;
  }
  if (strcmp(name, "rattr") == 0) {
    NEEDS(4);   /* rattr R RULE NAME */
    OBJ(ru, 2, OBJ_RULE);
    char * text = NULL;
    size_t n = 0;
    bool is_null = false;
    if (strcmp(o->f[3], "selectorText") == 0) {
      st = gtext_cssom_rule_selector_text(ru->ptr, &text, &n);
    }
    else if (strcmp(o->f[3], "keyText") == 0) {
      st = gtext_cssom_rule_key_text(ru->ptr, &text, &n);
    }
    else if (strcmp(o->f[3], "cssText") == 0) {
      st = gtext_cssom_rule_css_text(ru->ptr, &text, &n);
    }
    else {
      st = gtext_cssom_rule_get_attribute(ru->ptr, o->f[3], strlen(o->f[3]), &text, &n, &is_null);
    }
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      put_reg(w, o->f[1], val('u', "", 0));   /* a member the interface does not have */
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception(o->f[3], st);
    }
    else if (is_null) {
      put_reg(w, o->f[1], val('N', "", 0));
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), text);
    }
    else {
      put_reg(w, o->f[1], take_string(w, text, n));
    }
    return 1;
  }
  if (strcmp(name, "rtype") == 0) {
    NEEDS(3);
    OBJ(ru, 2, OBJ_RULE);
    put_num(w, o->f[1], gtext_cssom_rule_type(ru->ptr));
    return 1;
  }
  if (strcmp(name, "shattr") == 0) {
    NEEDS(4);   /* shattr R SHEET NAME */
    OBJ(sh, 2, OBJ_SHEET);
    char * text = NULL;
    size_t n = 0;
    const char * a = o->f[3];
    if (strcmp(a, "href") == 0) {
      st = gtext_cssom_sheet_href(sh->ptr, &text, &n);
    }
    else if (strcmp(a, "title") == 0) {
      st = gtext_cssom_sheet_title(sh->ptr, &text, &n);
    }
    else if (strcmp(a, "type") == 0) {
      const char * t = gtext_cssom_sheet_type(sh->ptr);
      put_reg(w, o->f[1], val('s', t, strlen(t)));
      return 1;
    }
    else if (strcmp(a, "disabled") == 0) {
      put_bool(w, o->f[1], gtext_cssom_sheet_disabled(sh->ptr));
      return 1;
    }
    else {
      *program_error = 1;
      *failure = fmt("shattr %s is not known", a);
      return 1;
    }
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(a, st);
    }
    else if (!text) {
      put_reg(w, o->f[1], val('N', "", 0));   /* href and title are null for a sheet that has none */
    }
    else {
      put_reg(w, o->f[1], take_string(w, text, n));
    }
    return 1;
  }
  if (strcmp(name, "insertrule") == 0) {
    NEEDS(5);   /* insertrule R OBJ TEXT INDEX (u: for no index argument) */
    OBJ(ob, 2, 0);
    size_t index = o->f[4][0] == 'u' ? 0 : (size_t)strtoull(o->f[4] + 2, NULL, 10);
    if (o->f[4][0] == 'r') {
      /* the index is a number the program read earlier (`sheet.cssRules.length`) */
      int index_ok;
      value iv = operand(w, o->f[4], o->l[4], &index_ok);
      if (!index_ok || iv.kind != 'n') {
        value_free(&iv);
        *program_error = 1;
        *failure = dups("insertrule: the index is not a number the program made");
        return 1;
      }
      index = (size_t)strtoull(iv.s, NULL, 10);
      value_free(&iv);
    }
    GTEXT_CSSOM_Rule * made = NULL;
    if (ob->kind == OBJ_SHEET) {
      st = gtext_cssom_sheet_insert_rule(ob->ptr, o->f[3], o->l[3], index, &made, &oc);
    }
    else if (ob->kind == OBJ_RULE) {
      st = gtext_cssom_rule_insert_rule(ob->ptr, o->f[3], o->l[3], index, &made, &oc);
    }
    else {
      st = GTEXT_CSS_E_INVALID;
    }
    gtext_cssom_rule_release(made);
    sync_world(w);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *failure = dups("TypeError: insertRule is not a function");
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception("insertRule", st);
    }
    else {
      put_num(w, o->f[1], index);
    }
    return 1;
  }
  if (strcmp(name, "deleterule") == 0) {
    NEEDS(3);   /* deleterule OBJ ARG: an index, or for @keyframes the text of a key */
    OBJ(ob, 1, 0);
    if (ob->kind == OBJ_RULE && strcmp(gtext_cssom_rule_interface_name(ob->ptr), "CSSKeyframesRule") == 0) {
      st = gtext_cssom_keyframes_delete_rule(ob->ptr, o->f[2] + 2, o->l[2] - 2, &oc);
    }
    else {
      const size_t index = (size_t)strtoull(o->f[2] + 2, NULL, 10);
      st = ob->kind == OBJ_SHEET ? gtext_cssom_sheet_delete_rule(ob->ptr, index, &oc) : ob->kind == OBJ_RULE ? gtext_cssom_rule_delete_rule(ob->ptr, index, &oc) : GTEXT_CSS_E_INVALID;
    }
    sync_world(w);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *failure = dups("TypeError: deleteRule is not a function");
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception("deleteRule", st);
    }
    return 1;
  }
  if (strcmp(name, "addrule") == 0 || strcmp(name, "removerule") == 0) {
    if (name[0] == 'a') {
      NEEDS(6);   /* addrule R SHEET SELECTOR STYLE INDEX */
    }
    else {
      NEEDS(3);   /* removerule SHEET INDEX */
    }
    OBJ(sh, name[0] == 'a' ? 2 : 1, OBJ_SHEET);
    if (name[0] == 'a') {
      const bool has_index = o->f[5][0] != 'u';
      const size_t index = has_index ? (size_t)strtoull(o->f[5] + 2, NULL, 10) : 0;
      st = gtext_cssom_sheet_add_rule(sh->ptr, o->f[3], o->l[3], o->f[4], o->l[4], has_index, index, &oc);
      if (st == GTEXT_CSS_OK) {
        put_reg(w, o->f[1], val('n', "-1", 2));   /* the binding returns -1 */
      }
    }
    else {
      st = gtext_cssom_sheet_remove_rule(sh->ptr, (size_t)strtoull(o->f[2] + 2, NULL, 10), &oc);
    }
    sync_world(w);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(name[0] == 'a' ? "addRule" : "removeRule", st);
    }
    return 1;
  }
  if (strcmp(name, "appendrule") == 0) {
    NEEDS(3);
    OBJ(ru, 1, OBJ_RULE);
    st = gtext_cssom_keyframes_append_rule(ru->ptr, o->f[2], o->l[2], NULL, &oc);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *failure = dups("TypeError: appendRule is not a function");
    }
    else if (st != GTEXT_CSS_OK) {
      *failure = css_exception("appendRule", st);
    }
    return 1;
  }
  if (strcmp(name, "rset") == 0) {
    NEEDS(4);   /* rset RULE NAME VALUE */
    OBJ(ru, 1, OBJ_RULE);
    st = GTEXT_CSS_OK;
    if (strcmp(o->f[2], "selectorText") == 0) {
      st = gtext_cssom_rule_set_selector_text(ru->ptr, o->f[3], o->l[3], &oc);
    }
    else if (strcmp(o->f[2], "keyText") == 0) {
      st = gtext_cssom_rule_set_key_text(ru->ptr, o->f[3], o->l[3], &oc);
    }
    else if (strcmp(o->f[2], "cssText") != 0) {
      st = gtext_cssom_rule_set_attribute(ru->ptr, o->f[2], strlen(o->f[2]), o->f[3], o->l[3], &oc);
    }
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      st = GTEXT_CSS_OK;   /* a member the object does not have: script makes a property of the object, and nothing else happens */
    }
    sync_world(w);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(o->f[2], st);
    }
    return 1;
  }
  if (strcmp(name, "sdisabled") == 0) {
    NEEDS(3);
    OBJ(sh, 1, OBJ_SHEET);
    st = gtext_cssom_sheet_set_disabled(sh->ptr, strcmp(o->f[2], "true") == 0, &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("disabled", st);
    }
    return 1;
  }
  if (strcmp(name, "mtext") == 0 || strcmp(name, "mlen") == 0 || strcmp(name, "mitem") == 0) {
    NEEDS(name[1] == 'i' ? 5 : 3);   /* mtext R M; mlen R M; mitem R M INDEX null|undefined */
    OBJ(m, 2, OBJ_MEDIA);
    if (name[1] == 't') {
      char * text = NULL;
      size_t n = 0;
      st = gtext_cssom_media_text(m->ptr, &text, &n);
      if (st != GTEXT_CSS_OK) {
        *failure = css_exception("mediaText", st);
      }
      else {
        put_reg(w, o->f[1], take_string(w, text, n));
      }
    }
    else if (name[1] == 'l') {
      size_t n = 0;
      gtext_cssom_media_length(m->ptr, &n);
      put_num(w, o->f[1], n);
    }
    else {
      char * text = NULL;
      size_t n = 0;
      bool is_null = false;
      size_t len = 0;
      gtext_cssom_media_length(m->ptr, &len);
      const size_t index = (size_t)strtoull(o->f[3], NULL, 10);
      if (index >= len) {
        put_reg(w, o->f[1], o->f[4][0] == 'n' ? val('N', "", 0) : val('u', "", 0));
        return 1;
      }
      st = gtext_cssom_media_item(m->ptr, index, &text, &n, &is_null);
      if (st != GTEXT_CSS_OK || is_null) {
        put_reg(w, o->f[1], val('N', "", 0));
      }
      else {
        put_reg(w, o->f[1], take_string(w, text, n));
      }
    }
    return 1;
  }
  if (strcmp(name, "mset") == 0 || strcmp(name, "mappend") == 0 || strcmp(name, "mdelete") == 0) {
    NEEDS(3);
    OBJ(m, 1, OBJ_MEDIA);
    st = name[1] == 's' ? gtext_cssom_media_set_text(m->ptr, o->f[2], o->l[2], &oc) : name[1] == 'a' ? gtext_cssom_media_append(m->ptr, o->f[2], o->l[2], &oc) :
                                                                                                          gtext_cssom_media_delete(m->ptr, o->f[2], o->l[2], &oc);
    sync_world(w);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception(name[1] == 's' ? "mediaText" : name[1] == 'a' ? "appendMedium" : "deleteMedium", st);
    }
    return 1;
  }
  if (strcmp(name, "rinst") == 0) {
    NEEDS(4);   /* rinst R OBJ CLASS: x instanceof CLASS, for the names of the interfaces of rules and the sheet */
    object * ob = find_obj(w, o->f[2]);
    if (!ob) {
      *program_error = 1;
      *failure = dups("rinst names an object the program did not make");
      return 1;
    }
    int yes = 0;
    if (ob->state == ST_OBJECT) {
      if (ob->kind == OBJ_SHEET) {
        yes = strcmp(o->f[3], "CSSStyleSheet") == 0 || strcmp(o->f[3], "StyleSheet") == 0;
      }
      else if (ob->kind == OBJ_RULE) {
        yes = strcmp(o->f[3], "CSSRule") == 0 || strcmp(o->f[3], gtext_cssom_rule_interface_name(ob->ptr)) == 0;
      }
      else {
        yes = strcmp(o->f[3], "MediaList") == 0;
      }
    }
    put_bool(w, o->f[1], yes);
    return 1;
  }
  if (strcmp(name, "mql") == 0) {
    NEEDS(3);   /* mql O TEXT: window.matchMedia(text) */
    GTEXT_CSSOM_Media_Query_List * list = NULL;
    st = gtext_cssom_match_media(w->doc, o->f[2], o->l[2], &list);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("matchMedia", st);
      return 1;
    }
    put_obj(w, o->f[1], OBJ_MQL, ST_OBJECT, list);
    return 1;
  }
  if (strcmp(name, "mqread") == 0) {
    NEEDS(4);   /* mqread R O media|matches */
    OBJ(ob, 2, OBJ_MQL);
    if (strcmp(o->f[3], "media") == 0) {
      char * text = NULL;
      size_t n = 0;
      st = gtext_cssom_media_query_list_media(ob->ptr, &text, &n);
      if (st != GTEXT_CSS_OK) {
        *failure = css_exception("media", st);
        return 1;
      }
      put_reg(w, o->f[1], take_string(w, text, n));
    }
    else {
      bool matches = false;
      st = gtext_cssom_media_query_list_matches(ob->ptr, &matches);
      if (st != GTEXT_CSS_OK) {
        *failure = status_failure("matches", st);
        return 1;
      }
      put_bool(w, o->f[1], matches);
    }
    return 1;
  }
  if (strcmp(name, "mqcss") == 0) {
    /* mqcss R TEXT: matchmedia-utils.js query_is_css_parseable(): `@media <text>{}` inserted into an empty sheet is one rule and its media text is not `not all`; the sheet is the
       helper's style element's, here a constructed one (the same insertRule), and nothing is left in the page */
    NEEDS(3);
    GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
    GTEXT_CSSOM_Sheet * sh = NULL;
    st = gtext_cssom_sheet_new_constructed(w->doc, &so, &sh);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("new CSSStyleSheet", st);
      return 1;
    }
    char * rule_text = fmt("@media %s{}", o->f[2]);
    GTEXT_CSSOM_Rule * made = NULL;
    st = gtext_cssom_sheet_insert_rule(sh, rule_text, strlen(rule_text), 0, &made, &oc);
    free(rule_text);
    if (st != GTEXT_CSS_OK) {
      gtext_cssom_sheet_release(sh);
      *failure = css_exception("insertRule", st);
      return 1;
    }
    int result = 0;
    const GTEXT_CSSOM_Rule_List * list = NULL;
    if (gtext_cssom_sheet_rules(sh, &list) == GTEXT_CSS_OK && list && gtext_cssom_rule_list_length(list) == 1) {
      GTEXT_CSSOM_Media_List * m = NULL;
      if (gtext_cssom_rule_media(made, &m) == GTEXT_CSS_OK) {
        char * text = NULL;
        size_t n = 0;
        if (gtext_cssom_media_text(m, &text, &n) == GTEXT_CSS_OK) {
          result = !(n == 7 && memcmp(text, "not all", 7) == 0);
          gtext_cssom_buffer_free(gtext_cssom_document_allocator(w->doc), text);
        }
        gtext_cssom_media_release(m);
      }
    }
    gtext_cssom_rule_release(made);
    gtext_cssom_sheet_release(sh);
    put_bool(w, o->f[1], result);
    return 1;
  }
  if (strncmp(name, "r", 1) == 0 && (strcmp(name, "rnsheets") == 0 || strcmp(name, "rsheetat") == 0 || strcmp(name, "radoptset") == 0 || strcmp(name, "radoptpush") == 0 ||
                                      strcmp(name, "radoptn") == 0 || strcmp(name, "radoptat") == 0)) {
    /* the sheets of a shadow root's scope: rnsheets R ROOT; rsheetat O ROOT INDEX null|undefined; radoptset ROOT SHEET...; radoptpush ROOT SHEET; radoptn R ROOT; radoptat O ROOT INDEX */
    const int root_field = (strcmp(name, "radoptset") == 0 || strcmp(name, "radoptpush") == 0) ? 1 : 2;
    if ((size_t)root_field >= o->n) {
      *program_error = 1;
      *failure = fmt("the op %s has no root", name);
      return 1;
    }
    elem * re = find_elem(w, o->f[root_field]);
    shadow * sr = re && re->node ? shadow_by_root(w, re->node) : NULL;
    if (!sr) {
      *program_error = 1;
      *failure = fmt("the op %s names %s, which is not a shadow root the program made", name, o->f[root_field]);
      return 1;
    }
    sync_world(w);
    if (strcmp(name, "rnsheets") == 0) {
      NEEDS(3);
      put_num(w, o->f[1], gtext_cssom_scope_sheet_count(sr->scope));
    }
    else if (strcmp(name, "rsheetat") == 0) {
      NEEDS(5);
      put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_scope_sheet_at(sr->scope, (size_t)strtoull(o->f[3], NULL, 10)), o->f[4][0] == 'n' ? ST_NULL : ST_UNDEFINED);
    }
    else if (strcmp(name, "radoptn") == 0) {
      NEEDS(3);
      put_num(w, o->f[1], gtext_cssom_scope_adopted_count(sr->scope));
    }
    else if (strcmp(name, "radoptat") == 0) {
      NEEDS(4);
      put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_scope_adopted_at(sr->scope, (size_t)strtoull(o->f[3], NULL, 10)), ST_UNDEFINED);
    }
    else if (strcmp(name, "radoptpush") == 0) {
      NEEDS(3);
      OBJ(sh, 2, OBJ_SHEET);
      st = gtext_cssom_scope_adopted_insert(sr->scope, sh->ptr, gtext_cssom_scope_adopted_count(sr->scope), &oc);
      if (st != GTEXT_CSS_OK) {
        *failure = css_exception("adoptedStyleSheets", st);
      }
    }
    else {
      GTEXT_CSSOM_Sheet * list[64];
      if (o->n > 65) {
        *program_error = 1;
        *failure = dups("radoptset has too many sheets");
        return 1;
      }
      for (size_t i = 2; i < o->n; i++) {
        object * ob = find_obj(w, o->f[i]);
        if (!ob) {
          *program_error = 1;
          *failure = dups("radoptset names an object the program did not make");
          return 1;
        }
        if (ob->state != ST_OBJECT || ob->kind != OBJ_SHEET) {
          *failure = dups("TypeError: adoptedStyleSheets takes sheets");
          return 1;
        }
        list[i - 2] = ob->ptr;
      }
      st = gtext_cssom_scope_adopted_set(sr->scope, list, o->n - 2, &oc);
      if (st != GTEXT_CSS_OK) {
        *failure = css_exception("adoptedStyleSheets", st);
      }
    }
    return 1;
  }
  if (strcmp(name, "adoptset") == 0) {
    /* document.adoptedStyleSheets = [sheets]: the checks of the setter are the library's */
    GTEXT_CSSOM_Sheet * list[64];
    if (o->n > 65) {
      *program_error = 1;
      *failure = dups("adoptset has too many sheets");
      return 1;
    }
    for (size_t i = 1; i < o->n; i++) {
      object * ob = find_obj(w, o->f[i]);
      if (!ob) {
        *program_error = 1;
        *failure = dups("adoptset names an object the program did not make");
        return 1;
      }
      if (ob->state != ST_OBJECT || ob->kind != OBJ_SHEET) {
        *failure = dups("TypeError: adoptedStyleSheets takes sheets");
        return 1;
      }
      list[i - 1] = ob->ptr;
    }
    st = gtext_cssom_scope_adopted_set(world_scope(w), list, o->n - 1, &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("adoptedStyleSheets", st);
    }
    return 1;
  }
  if (strcmp(name, "adoptpush") == 0) {
    NEEDS(2);
    OBJ(sh, 1, OBJ_SHEET);
    st = gtext_cssom_scope_adopted_insert(world_scope(w), sh->ptr, gtext_cssom_scope_adopted_count(world_scope(w)), &oc);
    if (st != GTEXT_CSS_OK) {
      *failure = css_exception("adoptedStyleSheets", st);
    }
    return 1;
  }
  if (strcmp(name, "adoptn") == 0) {
    NEEDS(2);
    put_num(w, o->f[1], gtext_cssom_scope_adopted_count(world_scope(w)));
    return 1;
  }
  if (strcmp(name, "adoptat") == 0) {
    NEEDS(3);
    put_found(w, o->f[1], OBJ_SHEET, gtext_cssom_scope_adopted_at(world_scope(w), (size_t)strtoull(o->f[2], NULL, 10)), ST_UNDEFINED);
    return 1;
  }
  return 0;
#undef NEEDS
#undef OBJ
}

/* ---- running one op --------------------------------------------------------------------------------------------------------------------- */

/// Returns NULL, or the message of a failure of the test (an assertion, or the exception the page's script would have thrown).
/* ---- time, frames and events ---------------------------------------------------------------------------------------------------------------------- */

/* The document timeline advances by frames of 16 ms, and only when an op says time passes (`frame`, `advance`, `ewait`, `ewaito`): what the page does in between is instantaneous, as script
   is. One frame is the steps of "update the rendering" that matter here, in the order they have: a style recalc for the changes the page made since the last (which begins the transitions
   at the time of the last frame, so that a transition the page begins is a frame old at the first frame that reports it), the time moves on, and the events the transition tracker owes at
   the new time are queued, to be dispatched to the listeners (and a requestAnimationFrame callback, which is the ops after `frame`, runs after them). */
#define FRAME_MS 16.0
#define FRAME_CAP 625   /* ten seconds of frames: the time the harness gives a test */

static void qevent_free(qevent * e) {
  free(e->property);
  free(e->pseudo);
  e->property = e->pseudo = NULL;
}

static void queue_clear(world * w) {
  for (size_t i = w->headqueue; i < w->nqueue; i++) {
    qevent_free(&w->queue[i]);
  }
  w->nqueue = w->headqueue = 0;
}

static void listeners_clear(world * w) {
  for (size_t i = 0; i < w->nlisteners; i++) {
    free(w->listeners[i].type);
    free(w->listeners[i].message);
  }
  w->nlisteners = 0;
  for (size_t i = 0; i < w->nwatchers; i++) {
    free(w->watchers[i].name);
    for (size_t k = 0; k < w->watchers[i].nwait; k++) {
      free(w->watchers[i].wait_types[k]);
    }
    free(w->watchers[i].wait_types);
    for (size_t k = 0; k < w->watchers[i].nrec; k++) {
      qevent_free(&w->watchers[i].rec[k]);
    }
  }
  w->nwatchers = 0;
}

static void events_free(world * w) {
  for (size_t i = 0; i < w->nskipped; i++) {
    free(w->skipped[i]);
  }
  free(w->skipped);
  w->skipped = NULL;
  w->nskipped = 0;
  queue_clear(w);
  free(w->queue);
  listeners_clear(w);
  free(w->listeners);
  free(w->watchers);
  free(w->tracked);
  free(w->known);
  w->known = NULL;
  w->queue = NULL;
  w->listeners = NULL;
  w->watchers = NULL;
  w->tracked = NULL;
}

/* A test begins: the listeners and the events of the one before are not its. The timeline goes on. */
static void events_test_begin(world * w) {
  for (size_t i = 0; i < w->nskipped; i++) {
    free(w->skipped[i]);
  }
  w->nskipped = 0;
  queue_clear(w);
  listeners_clear(w);
  w->test_frames = 0;
}

static void collect_elements(GTEXT_Markup_Node * n, GTEXT_Markup_Node *** list, size_t * count, size_t * cap) {
  for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c; c = gtext_markup_node_next_sibling(c)) {
    if (!is_element(c)) {
      continue;
    }
    if (*count == *cap) {
      *cap = *cap ? *cap * 2 : 32;
      *list = realloc(*list, *cap * sizeof **list);
      if (!*list) {
        abort();
      }
    }
    (*list)[(*count)++] = c;
    collect_elements(c, list, count, cap);
  }
}

/* "Update the rendering", for the part that starts and stops transitions: a style change event for every element and pseudo-element of the document (when the page has made changes since the
   last; otherwise nothing needs doing), and the elements that left the document are forgotten ("If an element is no longer in the document, implementations must cancel any running transitions
   on it"). */
static char * style_recalc(world * w) {
  if (!w->tracker) {
    return NULL;
  }
  const uint64_t mg = gtext_markup_document_generation(w->markup);
  const uint64_t cg = gtext_cssom_document_generation(w->doc);
  if (w->recalc_done && mg == w->recalc_markup && cg == w->recalc_cssom) {
    return NULL;
  }
  sync_world(w);
  GTEXT_Markup_Node ** list = NULL;
  size_t count = 0, cap = 0;
  collect_elements((GTEXT_Markup_Node *)gtext_markup_document_root(w->markup), &list, &count, &cap);
  for (size_t i = 0; i < w->ntracked; i++) {
    int still = 0;
    for (size_t k = 0; k < count && !still; k++) {
      still = list[k] == w->tracked[i];
    }
    if (!still) {
      size_t gone = 0;
      const GTEXT_CSS_Status st = gtext_cssom_transition_tracker_forget(w->tracker, gtext_css_markup_handle(w->tracked[i]), &gone);
      if (st != GTEXT_CSS_OK) {
        free(list);
        return status_failure("forgetting an element", st);
      }
    }
  }
  static const char * const pseudos[] = {NULL, "::before", "::after", "::marker"};
  for (size_t i = 0; i < count; i++) {
    for (size_t k = 0; k < sizeof pseudos / sizeof *pseudos; k++) {
      const GTEXT_CSS_Status st = gtext_cssom_transition_tracker_flush(w->tracker, gtext_css_markup_handle(list[i]), pseudos[k], pseudos[k] ? strlen(pseudos[k]) : 0);
      if (st != GTEXT_CSS_OK) {
        free(list);
        return status_failure("style change event", st);
      }
    }
  }
  for (size_t i = 0; i < count; i++) {
    int seen = 0;
    for (size_t k = 0; k < w->nknown && !seen; k++) {
      seen = w->known[k] == list[i];
    }
    if (!seen) {
      if (w->nknown == w->capknown) {
        w->capknown = w->capknown ? w->capknown * 2 : 32;
        w->known = realloc(w->known, w->capknown * sizeof *w->known);
        if (!w->known) {
          abort();
        }
      }
      w->known[w->nknown++] = list[i];
    }
  }
  free(w->tracked);
  w->tracked = list;
  w->ntracked = count;
  w->captracked = cap;
  w->recalc_done = 1;
  w->recalc_markup = gtext_markup_document_generation(w->markup);
  w->recalc_cssom = gtext_cssom_document_generation(w->doc);
  return NULL;
}

static size_t tracked_index(const world * w, const GTEXT_Markup_Node * n) {
  for (size_t i = 0; i < w->ntracked; i++) {
    if (w->tracked[i] == n) {
      return i;
    }
  }
  return (size_t)-1;
}

static void queue_push(world * w, qevent e) {
  if (w->nqueue == w->capqueue) {
    w->capqueue = w->capqueue ? w->capqueue * 2 : 32;
    w->queue = realloc(w->queue, w->capqueue * sizeof *w->queue);
    if (!w->queue) {
      abort();
    }
  }
  w->queue[w->nqueue++] = e;
}

/* One frame: the style recalc, the time, and the events of the tracker queued in tree order of their targets (the tracker's order is the order it first saw the elements). */
static char * run_frame(world * w) {
  char * failure = style_recalc(w);
  if (failure) {
    return failure;
  }
  w->now_ms += FRAME_MS;
  w->test_frames++;
  if (!w->tracker) {
    return NULL;
  }
  gtext_cssom_transition_tracker_set_time(w->tracker, w->now_ms);
  if (w->headqueue == w->nqueue) {   /* everything queued has been dispatched: start again from the front, so that the events of this frame are the ones from `first` on */
    w->headqueue = w->nqueue = 0;
  }
  GTEXT_Anim_Events * events = NULL;
  const GTEXT_Anim_Status st = gtext_anim_world_update(w->anim, &events);
  if (st != GTEXT_ANIM_OK) {
    return fmt("the events of a frame could not be made (status %d)", (int)st);
  }
  static const char * const types[] = {"animationstart", "animationiteration", "animationend", "animationcancel", "transitionrun", "transitionstart", "transitionend", "transitioncancel"};
  const size_t n = gtext_anim_events_count(events);
  const size_t first = w->nqueue;
  for (size_t i = 0; i < n; i++) {
    GTEXT_Anim_Event e;
    if (!gtext_anim_events_at(events, i, &e)) {
      break;
    }
    if (e.type >= GTEXT_ANIM_EVENT_FINISH) {
      continue;   /* an AnimationPlaybackEvent is dispatched at the Animation, not at an element: no page of the program listens for one */
    }
    qevent q;
    memset(&q, 0, sizeof q);
    for (size_t k = 0; k < w->nknown; k++) {
      if (gtext_css_markup_handle(w->known[k]) == e.element) {
        q.target = w->known[k];
      }
    }
    q.type = types[e.type];
    q.property = dupn(e.name.data ? e.name.data : "", e.name.length);
    q.pseudo = dupn(e.pseudo_element.data ? e.pseudo_element.data : "", e.pseudo_element.length);
    q.elapsed = e.elapsed_time;
    q.resolved = e.scheduled_time.resolved;
    q.scheduled = e.scheduled_time.ms;
    queue_push(w, q);
  }
  gtext_anim_events_free(events);
  /* the end of the frame's task is a microtask checkpoint: the finish notifications of the Animations the program holds run */
  if (gtext_anim_world_checkpoint(w->anim) != GTEXT_ANIM_OK) {
    return dups("the microtask checkpoint of the Animations failed");
  }
  /* "Perform a stable sort of the animation events ... Sort the events by their scheduled event time ... Within events with equal scheduled event times, sort by their composite order": the library's list is in
     that order for the events of one element and of one frame of the tracker; across elements the composite order is the tree order of the owning elements (the tracker's order is the order it first saw them in),
     so this frame's events are sorted again, stably, by the time and then the tree order of the targets */
  for (size_t i = first + 1; i < w->nqueue; i++) {
    qevent x = w->queue[i];
    const size_t key = x.target ? tracked_index(w, x.target) : (size_t)-1;
    size_t k = i;
    while (k > first) {
      const qevent * prev = &w->queue[k - 1];
      const size_t pk = prev->target ? tracked_index(w, prev->target) : (size_t)-1;
      const int by_time = prev->resolved != x.resolved ? (prev->resolved ? 1 : -1) : (prev->scheduled > x.scheduled ? 1 : prev->scheduled < x.scheduled ? -1 : 0);
      if (by_time < 0 || (by_time == 0 && pk <= key)) {
        break;
      }
      w->queue[k] = w->queue[k - 1];
      k--;
    }
    w->queue[k] = x;
  }
  return NULL;
}

static watcher * find_watcher(world * w, const char * name) {
  for (size_t i = 0; i < w->nwatchers; i++) {
    if (strcmp(w->watchers[i].name, name) == 0) {
      return &w->watchers[i];
    }
  }
  return NULL;
}

static void add_listener(world * w, listener l) {
  if (w->nlisteners == w->caplisteners) {
    w->caplisteners = w->caplisteners ? w->caplisteners * 2 : 16;
    w->listeners = realloc(w->listeners, w->caplisteners * sizeof *w->listeners);
    if (!w->listeners) {
      abort();
    }
  }
  w->listeners[w->nlisteners++] = l;
}

/* The event reaches the listeners at its target and above it, in the order they were added. A forbidden event fails the test with the message its handler's assertion has; an EventWatcher's
   handler is testharness.js's: it asserts that a wait_for() is outstanding and that the event is the next type it waits for, and when the last type has come the wait is finished. */
static char * dispatch_event(world * w, const qevent * e) {
  for (size_t i = 0; i < w->nlisteners; i++) {
    const listener * l = &w->listeners[i];
    if (strcmp(l->type, e->type) != 0 || !e->target || !node_is_inclusive_ancestor(l->node, e->target)) {
      continue;
    }
    if (l->kind == 1) {
      return dups(l->message);
    }
    watcher * wt = &w->watchers[l->watcher];
    if (!wt->active) {
      continue;
    }
    if (!wt->waiting) {
      return fmt("assert_true: Not expecting event, but got %s event expected true got false", e->type);
    }
    const char * want = wt->wait_types[wt->pos];
    if (strcmp(want, e->type) != 0) {
      return fmt("assert_equals: Expected %s event, but got %s event instead expected \"%s\" but got \"%s\"", want, e->type, want, e->type);
    }
    if (wt->nrec == EVENT_RECORD) {
      return dups("the runner records at most 64 events of one wait_for()");
    }
    qevent * r = &wt->rec[wt->nrec++];
    *r = *e;
    r->property = dups(e->property);
    r->pseudo = dups(e->pseudo);
    if (++wt->pos == wt->nwait) {
      wt->waiting = 0;
      wt->finished = 1;
    }
  }
  return NULL;
}

/* The events that are queued, dispatched (a failure stops at the event that caused it). */
static char * dispatch_queue(world * w) {
  while (w->headqueue < w->nqueue) {
    qevent * e = &w->queue[w->headqueue++];
    char * failure = dispatch_event(w, e);
    qevent_free(e);
    if (failure) {
      return failure;
    }
  }
  return NULL;
}

static void put_event_regs(world * w, const char * const * names, const qevent * e) {
  char num[64];
  put_reg(w, names[0], val('s', e->type, strlen(e->type)));
  put_reg(w, names[1], val('s', e->property ? e->property : "", e->property ? strlen(e->property) : 0));
  snprintf(num, sizeof num, "%.17g", e->elapsed);
  put_reg(w, names[2], val('n', num, strlen(num)));
  put_reg(w, names[3], val('s', e->pseudo ? e->pseudo : "", e->pseudo ? strlen(e->pseudo) : 0));
}

/* ewait/ewaito: let time pass until the events a wait_for() waits for have come (or the one event a once listener waits for), dispatching what is queued first. */
static char * events_wait(world * w, const op * o, int * program_error) {
  const int once = strcmp(o->f[0], "ewaito") == 0;
  watcher * wt = NULL;
  GTEXT_Markup_Node * once_node = NULL;
  const char * once_type = NULL;
  size_t budget = 0, n = 1, first_reg = 0;
  if (once) {
    if (o->n != 7) {
      *program_error = 1;
      return dups("ewaito has the wrong number of fields");
    }
    elem * e = find_elem(w, o->f[1]);
    if (!e || !e->node) {
      *program_error = 1;
      return dups("ewaito names an element the program did not make");
    }
    once_node = e->node;
    once_type = o->f[2];
    first_reg = 3;
  }
  else {
    if (o->n < 5) {
      *program_error = 1;
      return dups("ewait has too few fields");
    }
    wt = find_watcher(w, o->f[1]);
    budget = (size_t)strtoull(o->f[2], NULL, 10);
    n = (size_t)strtoull(o->f[4], NULL, 10);
    if (!wt || n == 0 || o->n != 5 + n + 4 * n) {
      *program_error = 1;
      return dups("ewait names a watcher the program did not make, or has the wrong number of fields");
    }
    for (size_t i = 0; i < wt->nwait; i++) {
      free(wt->wait_types[i]);
    }
    free(wt->wait_types);
    wt->wait_types = calloc(n, sizeof *wt->wait_types);
    if (!wt->wait_types) {
      abort();
    }
    for (size_t i = 0; i < n; i++) {
      wt->wait_types[i] = dups(o->f[5 + i]);
    }
    wt->nwait = n;
    wt->pos = 0;
    wt->waiting = 1;
    wt->finished = 0;
    for (size_t k = 0; k < wt->nrec; k++) {
      qevent_free(&wt->rec[k]);
    }
    wt->nrec = 0;
    first_reg = 5 + n;
  }
  size_t frames_used = 0;
  qevent got;
  memset(&got, 0, sizeof got);
  char * failure = NULL;
  for (;;) {
    int done = 0;
    while (!failure && w->headqueue < w->nqueue) {
      qevent * e = &w->queue[w->headqueue++];
      failure = dispatch_event(w, e);
      if (!failure && once && strcmp(e->type, once_type) == 0 && e->target && node_is_inclusive_ancestor(once_node, e->target)) {
        done = 1;
        got = *e;
        got.property = dups(e->property);
        got.pseudo = dups(e->pseudo);
      }
      qevent_free(e);
      if (failure || done || (wt && wt->finished)) {
        break;
      }
    }
    if (failure || done || (wt && wt->finished)) {
      break;
    }
    if (budget && frames_used >= budget) {
      char left[256] = "";
      size_t used = 0;
      for (size_t i = wt->pos; i < wt->nwait && used + 1 < sizeof left; i++) {
        const int put = snprintf(left + used, sizeof left - used, "%s%s", i > wt->pos ? ", " : "", wt->wait_types[i]);
        if (put < 0 || (size_t)put >= sizeof left - used) {
          break;
        }
        used += (size_t)put;
      }
      failure = fmt("assert_true: Timed out waiting for %s expected true got false", left);
      break;
    }
    if (w->test_frames >= FRAME_CAP) {
      failure = dups("the test timed out: the page waited for an event that did not come");
      break;
    }
    failure = run_frame(w);
    frames_used++;
    if (failure) {
      break;
    }
  }
  if (wt) {
    wt->waiting = 0;
  }
  if (!failure) {
    if (once) {
      put_event_regs(w, (const char * const *)&o->f[first_reg], &got);
    }
    else {
      for (size_t i = 0; i < n; i++) {
        put_event_regs(w, (const char * const *)&o->f[first_reg + 4 * i], &wt->rec[i < wt->nrec ? i : wt->nrec - 1]);
      }
    }
  }
  if (once) {
    qevent_free(&got);
  }
  return failure;
}

/* ebatch s:PSEUDO N NAMES(N) TIMES(N) REGS(4N): the events (type, propertyName, elapsedTime, pseudoElement of each) and the expectations are each sorted by property name and compared. */
static char * events_batch(world * w, const op * o, int * program_error) {
  if (o->n < 3) {
    *program_error = 1;
    return dups("ebatch has too few fields");
  }
  const size_t n = (size_t)strtoull(o->f[2], NULL, 10);
  if (n == 0 || n > 64 || o->n != 3 + 2 * n + 4 * n || o->l[1] < 2) {
    *program_error = 1;
    return dups("ebatch has the wrong number of fields");
  }
  const char * pseudo = o->f[1] + 2;
  size_t ev[64], ex[64];
  const char * prop[64];
  for (size_t i = 0; i < n; i++) {
    const reg * r = find_reg(w, o->f[3 + 2 * n + 4 * i + 1]);
    if (!r) {
      *program_error = 1;
      return dups("ebatch names a register the program did not make");
    }
    prop[i] = r->v.s;
    ev[i] = i;
    ex[i] = i;
  }
  for (size_t i = 1; i < n; i++) {   /* stable insertion sorts */
    size_t x = ev[i], k = i;
    while (k > 0 && strcmp(prop[ev[k - 1]], prop[x]) > 0) {
      ev[k] = ev[k - 1];
      k--;
    }
    ev[k] = x;
    x = ex[i];
    k = i;
    while (k > 0 && strcmp(o->f[3 + ex[k - 1]], o->f[3 + x]) > 0) {
      ex[k] = ex[k - 1];
      k--;
    }
    ex[k] = x;
  }
  for (size_t i = 0; i < n; i++) {
    const size_t base = 3 + 2 * n + 4 * ev[i];
    const reg * rp = find_reg(w, o->f[base + 1]);
    const reg * re = find_reg(w, o->f[base + 2]);
    const reg * rs = find_reg(w, o->f[base + 3]);
    if (!rp || !re || !rs) {
      *program_error = 1;
      return dups("ebatch names a register the program did not make");
    }
    value expected = val('s', o->f[3 + ex[i]], o->l[3 + ex[i]]);
    if (!same_as(&rp->v, &expected, 1)) {
      char * m = mismatch("assert_equals", "", &expected, &rp->v);
      value_free(&expected);
      return m;
    }
    value_free(&expected);
    const double want = strtod(o->f[3 + n + ex[i]], NULL), got = strtod(re->v.s, NULL);
    if (!(fabs(got - want) <= 0.0005)) {
      return fmt("assert_approx_equals: %s differs from %s by more than 0.0005", re->v.s, o->f[3 + n + ex[i]]);
    }
    value p = val('s', pseudo, strlen(pseudo));
    if (!same_as(&rs->v, &p, 1)) {
      char * m = mismatch("assert_equals", "", &p, &rs->v);
      value_free(&p);
      return m;
    }
    value_free(&p);
  }
  return NULL;
}

/* "If an element is no longer in the document, implementations must cancel any running transitions on it": the DOM removes a node from its parent before it inserts it somewhere else, so an
   element that is moved or removed is forgotten by the op (the tracker reports its transitioncancel at the next frame), and one that is put back has no before-change style. The elements are
   collected before the op (it may take the children away) and forgotten after it, and only if it did not throw (an insertion that is refused moves nothing). */
typedef struct {
  GTEXT_Markup_Node ** nodes;
  size_t n, cap;
} forgetlist;

static void forget_collect(GTEXT_Markup_Node * n, forgetlist * l) {
  if (!n || !is_element(n)) {
    return;
  }
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 16;
    l->nodes = realloc(l->nodes, l->cap * sizeof *l->nodes);
    if (!l->nodes) {
      abort();
    }
  }
  l->nodes[l->n++] = n;
  for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c; c = gtext_markup_node_next_sibling(c)) {
    forget_collect(c, l);
  }
}

static void forget_named(world * w, const char * name, int children_only, forgetlist * l) {
  if (name[0] == 'e' && name[1] == ':') {
    name += 2;
  }
  elem * e = find_elem(w, name);
  if (!e || !e->node) {
    return;
  }
  if (children_only) {
    for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(e->node); c; c = gtext_markup_node_next_sibling(c)) {
      forget_collect(c, l);
    }
  }
  else {
    forget_collect(e->node, l);
  }
}

static void forget_plan(world * w, const op * o, forgetlist * l) {
  const char * n = o->f[0];
  if (!w->tracker || !w->async || o->n < 2) {
    return;
  }
  if (strcmp(n, "remove") == 0) {
    forget_named(w, o->f[1], 0, l);
  }
  else if (strcmp(n, "removechild") == 0 || strcmp(n, "appendchild") == 0 || strcmp(n, "insertbefore") == 0) {
    if (o->n > 2) {
      forget_named(w, o->f[2], 0, l);
    }
  }
  else if (strcmp(n, "replacechild") == 0) {
    for (size_t i = 2; i < o->n && i < 4; i++) {
      forget_named(w, o->f[i], 0, l);
    }
  }
  else if (strcmp(n, "append") == 0 || strcmp(n, "prepend") == 0 || strcmp(n, "before") == 0 || strcmp(n, "after") == 0 || strcmp(n, "replacewith") == 0 || strcmp(n, "replacechildren") == 0) {
    for (size_t i = 2; i < o->n; i++) {
      if (o->f[i][0] == 'e' && o->f[i][1] == ':') {
        forget_named(w, o->f[i], 0, l);
      }
    }
    if (strcmp(n, "replacewith") == 0) {
      forget_named(w, o->f[1], 0, l);
    }
    if (strcmp(n, "replacechildren") == 0) {
      forget_named(w, o->f[1], 1, l);
    }
  }
  else if (strcmp(n, "ihset") == 0 || strcmp(n, "ihsetu") == 0 || strcmp(n, "tcset") == 0 || strcmp(n, "dset") == 0) {
    forget_named(w, o->f[1], 1, l);
  }
}

static void forget_apply(world * w, forgetlist * l, int did) {
  for (size_t i = 0; did && i < l->n; i++) {
    size_t gone = 0;
    (void)gtext_cssom_transition_tracker_forget(w->tracker, gtext_css_markup_handle(l->nodes[i]), &gone);
  }
  free(l->nodes);
  l->nodes = NULL;
  l->n = l->cap = 0;
}

static char * events_op(world * w, const op * o, int * program_error) {
  const char * name = o->f[0];
  char num[64];
  if (strcmp(name, "ptest") == 0) {
    w->async = 1;
    return NULL;
  }
  if (strcmp(name, "tlnow") == 0) {
    if (o->n != 2) {
      *program_error = 1;
      return dups("tlnow has the wrong number of fields");
    }
    snprintf(num, sizeof num, "%.17g", w->now_ms);
    put_reg(w, o->f[1], val('n', num, strlen(num)));
    return NULL;
  }
  if (strcmp(name, "frame") == 0 || strcmp(name, "advance") == 0) {
    if (o->n != 2) {
      *program_error = 1;
      return dups("frame has the wrong number of fields");
    }
    const int advance = strcmp(name, "advance") == 0;
    const double until = advance ? w->now_ms + strtod(o->f[1], NULL) : 0;
    char * failure = dispatch_queue(w);
    if (failure) {
      return failure;
    }
    while (!advance || w->now_ms + 1e-9 < until) {   /* a frame is one step of time; a timer of 0 ms is a task between frames and lets none pass */
      if (w->test_frames >= FRAME_CAP) {
        return dups("the test timed out: the page waited for more time than the harness gives a test");
      }
      failure = run_frame(w);
      if (!failure) {
        failure = dispatch_queue(w);
      }
      if (failure) {
        return failure;
      }
      if (!advance) {
        break;
      }
    }
    if (!advance) {
      snprintf(num, sizeof num, "%.17g", w->now_ms);
      put_reg(w, o->f[1], val('n', num, strlen(num)));
    }
    return NULL;
  }
  if (strcmp(name, "lw") == 0 || strcmp(name, "lf") == 0) {
    const int watch = strcmp(name, "lw") == 0;
    if (o->n < (watch ? 4u : 4u)) {
      *program_error = 1;
      return dups("the listener op has too few fields");
    }
    elem * e = find_elem(w, o->f[watch ? 2 : 1]);
    if (!e || !e->node) {
      *program_error = 1;
      return fmt("the op %s names an element the program did not make", name);
    }
    if (watch) {
      if (w->nwatchers == w->capwatchers) {
        w->capwatchers = w->capwatchers ? w->capwatchers * 2 : 8;
        w->watchers = realloc(w->watchers, w->capwatchers * sizeof *w->watchers);
        if (!w->watchers) {
          abort();
        }
      }
      const size_t wi = w->nwatchers++;
      memset(&w->watchers[wi], 0, sizeof w->watchers[wi]);
      w->watchers[wi].name = dups(o->f[1]);
      w->watchers[wi].active = 1;
      for (size_t i = 3; i < o->n; i++) {
        listener l = {0, e->node, dups(o->f[i]), wi, NULL};
        add_listener(w, l);
      }
    }
    else {
      listener l = {1, e->node, dups(o->f[2]), 0, dups(o->f[3])};
      add_listener(w, l);
    }
    return NULL;
  }
  if (strcmp(name, "lstop") == 0) {
    watcher * wt = o->n == 2 ? find_watcher(w, o->f[1]) : NULL;
    if (!wt) {
      *program_error = 1;
      return dups("lstop names a watcher the program did not make");
    }
    wt->active = 0;
    return NULL;
  }
  if (strcmp(name, "ewait") == 0 || strcmp(name, "ewaito") == 0) {
    return events_wait(w, o, program_error);
  }
  if (strcmp(name, "ebatch") == 0) {
    return events_batch(w, o, program_error);
  }
  *program_error = 1;
  return fmt("the op %s is not one the runner knows", name);
}

static int is_events_op(const char * name) {
  static const char * const names[] = {"ptest", "frame", "advance", "ewait", "ewaito", "lw", "lf", "lstop", "ebatch", "tlnow"};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (strcmp(name, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/* ---- animations ------------------------------------------------------------------------------------------------------------------------------- */

static int is_anim_op(const char * name) {
  static const char * const names[] = {"ga", "alen", "aat", "aget", "aset", "tget", "pgen", "pwait", "am", "asetv", "calc", "pfloat", "anew", "animnew", "enew", "ecopy", "aeff", "aseteff", "asettl", "eget", "eset",
                                       "etarget", "esettarget", "ekf", "kflen", "kfget", "kfcmp", "kfhas", "esetkf", "eupdate"};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (strcmp(name, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static animh * find_ah(world * w, const char * name) {
  for (size_t i = w->nahs; i-- > 0;) {
    if (strcmp(w->ahs[i].name, name) == 0) {
      return &w->ahs[i];
    }
  }
  return NULL;
}

static animlist * find_alist(world * w, const char * name) {
  for (size_t i = w->nalists; i-- > 0;) {
    if (strcmp(w->alists[i].name, name) == 0) {
      return &w->alists[i];
    }
  }
  return NULL;
}

static void animh_free(animh * h) {
  free(h->name);
  free(h->pname);
  free(h->easing);
  free(h->idtext);
  memset(h, 0, sizeof *h);
}

static animh animh_copy(const animh * h, const char * name) {
  animh c = *h;
  c.name = dups(name);
  c.pname = dups(h->pname ? h->pname : "");
  c.easing = dups(h->easing ? h->easing : "");
  c.idtext = dups(h->idtext ? h->idtext : "");
  return c;
}

static value number_value(double v) {
  char buf[64];
  if (v != v) {
    return val('n', "NaN", 3);
  }
  if (v > 1e308) {
    return val('n', "Infinity", 8);
  }
  if (v < -1e308) {
    return val('n', "-Infinity", 9);
  }
  snprintf(buf, sizeof buf, "%.17g", v);
  return val('n', buf, strlen(buf));
}

static value time_value(GTEXT_Anim_Time t) {
  return t.resolved ? number_value(t.ms) : val('N', "", 0);
}

static const char * fill_name(GTEXT_Anim_Fill f) {
  switch (f) {
    case GTEXT_ANIM_FILL_NONE: return "none";
    case GTEXT_ANIM_FILL_FORWARDS: return "forwards";
    case GTEXT_ANIM_FILL_BACKWARDS: return "backwards";
    case GTEXT_ANIM_FILL_BOTH: return "both";
    default: return "auto";
  }
}

static const char * direction_name(GTEXT_Anim_Direction d) {
  switch (d) {
    case GTEXT_ANIM_DIRECTION_REVERSE: return "reverse";
    case GTEXT_ANIM_DIRECTION_ALTERNATE: return "alternate";
    case GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE: return "alternate-reverse";
    default: return "normal";
  }
}


/* ---- the Animation of the model ------------------------------------------------------------------------------------------------------------- */

static ctl * ctl_find(world * w, GTEXT_Anim_Kind kind, uint64_t id) {
  for (size_t i = 0; i < w->nctls; i++) {
    if (w->ctls[i].kind == kind && w->ctls[i].id == id) {
      return &w->ctls[i];
    }
  }
  return NULL;
}

/* The Animation that holds an animation the program touches, made now if it is the first touch (an animation the program found with getAnimations() is touched at once: `aat`). An animation that has no
 * record to hold (it is not in the tracker, which a handle the runner made before the model could hold it can be) has an Animation of the runner's own, idle. */
static ctl * ctl_get(world * w, const animh * h) {
  ctl * c = ctl_find(w, h->kind, h->id);
  if (c) {
    return c;
  }
  GTEXT_Anim_Animation * a = NULL;
  int orphan = 0;
  if (!w->anim || gtext_anim_world_control(w->anim, h->kind, h->id, &a) != GTEXT_ANIM_OK) {
    if (gtext_anim_animation_new(NULL, GTEXT_ANIM_TIMELINE_DOCUMENT, &a) != GTEXT_ANIM_OK) {
      return NULL;
    }
    orphan = 1;
  }
  if (w->nctls == w->capctls) {
    w->capctls = w->capctls ? w->capctls * 2 : 8;
    w->ctls = realloc(w->ctls, w->capctls * sizeof *w->ctls);
    if (!w->ctls) {
      abort();
    }
  }
  c = &w->ctls[w->nctls++];
  memset(c, 0, sizeof *c);
  c->kind = h->kind;
  c->id = h->id;
  c->a = a;
  c->orphan = orphan;
  return c;
}

static void ctl_settle(ctl * c, int which, uint32_t gen, int state) {
  for (size_t i = 0; i < c->np[which]; i++) {
    if (c->p[which][i].gen == gen) {
      c->p[which][i].state = state;
      return;
    }
  }
  if (c->np[which] == c->cap[which]) {
    c->cap[which] = c->cap[which] ? c->cap[which] * 2 : 8;
    c->p[which] = realloc(c->p[which], c->cap[which] * sizeof *c->p[which]);
    if (!c->p[which]) {
      abort();
    }
  }
  c->p[which][c->np[which]++] = (psettle){gen, state};
}

/* The promises the Animation settled since the last look, in the order the model made them. */
static void ctl_drain(ctl * c) {
  for (size_t i = 0; i < gtext_anim_animation_note_count(c->a); i++) {
    GTEXT_Anim_Note n;
    if (!gtext_anim_animation_note_at(c->a, i, &n)) {
      break;
    }
    const int which = n.kind == GTEXT_ANIM_NOTE_READY_RESOLVE || n.kind == GTEXT_ANIM_NOTE_READY_REJECT ? 0 : 1;
    ctl_settle(c, which, n.generation, n.kind == GTEXT_ANIM_NOTE_READY_REJECT || n.kind == GTEXT_ANIM_NOTE_FINISHED_REJECT ? 2 : 1);
  }
  gtext_anim_animation_notes_clear(c->a);
}

/* A microtask checkpoint, and then every promise that settled is in the tables. */
static char * ctl_drain_all(world * w) {
  if (w->anim && gtext_anim_world_checkpoint(w->anim) != GTEXT_ANIM_OK) {
    return dups("the microtask checkpoint of the Animations failed");
  }
  for (size_t i = 0; i < w->nctls; i++) {
    if (w->ctls[i].orphan && gtext_anim_animation_checkpoint(w->ctls[i].a, (GTEXT_Anim_Time){true, w->now_ms}) != GTEXT_ANIM_OK) {
      return dups("the microtask checkpoint of an Animation failed");   /* the world's checkpoint does not reach the runner's own */
    }
    ctl_drain(&w->ctls[i]);
  }
  return NULL;
}

/* The state of the promise of a generation: 0 pending, 1 fulfilled, 2 rejected. The current generation is the Animation's; an older one settled in a note (one with none was the initial ready promise,
 * which is fulfilled). */
static int ctl_promise_state(world * w, ctl * c, int which, uint32_t gen) {
  GTEXT_Anim_Animation_State st;
  (void)gtext_anim_animation_state(c->a, (GTEXT_Anim_Time){true, w->now_ms}, &st);
  const uint32_t current = which == 0 ? st.ready_generation : st.finished_generation;
  if (gen == current) {
    const GTEXT_Anim_Promise_State ps = which == 0 ? st.ready : st.finished;
    return ps == GTEXT_ANIM_PROMISE_PENDING ? 0 : ps == GTEXT_ANIM_PROMISE_RESOLVED ? 1 : 2;
  }
  for (size_t i = 0; i < c->np[which]; i++) {
    if (c->p[which][i].gen == gen) {
      return c->p[which][i].state;
    }
  }
  /* a generation with no note: the initial ready promise (fulfilled, made so); a finished promise the model replaced was rejected by the cancel that replaced it, and a note says so */
  return which == 0 ? 1 : 2;
}

/* "Name: message", the failure the DOMException or TypeError of the IDL is for a status of the model; a status that is the caller's mistake or an allocation is a fault of the program. */
static char * anim_status_failure(const char * what, GTEXT_Anim_Status st, int * program_error) {
  switch (st) {
    case GTEXT_ANIM_TYPE_ERROR: return fmt("TypeError: %s", what);
    case GTEXT_ANIM_RANGE_ERROR: return fmt("RangeError: %s", what);
    case GTEXT_ANIM_INVALID_STATE: return fmt("InvalidStateError: %s", what);
    case GTEXT_ANIM_NOT_SUPPORTED: return fmt("NotSupportedError: %s", what);
    case GTEXT_ANIM_NO_MODIFICATION_ALLOWED: return fmt("NoModificationAllowedError: %s", what);
    case GTEXT_ANIM_ABORT: return fmt("AbortError: %s", what);
    default:
      *program_error = 1;
      return fmt("%s failed with status %d", what, (int)st);
  }
}

/* one frame of the document: a step of time for a wait of the page (the same steps as the `frame` op) */
static char * step_frame(world * w) {
  if (w->test_frames >= FRAME_CAP) {
    return dups("the test timed out: the page waited for more time than the harness gives a test");
  }
  char * failure = run_frame(w);
  if (!failure) {
    failure = dispatch_queue(w);
  }
  return failure;
}

/* parseFloat (ECMAScript, "parseFloat ( string )"): the longest prefix of the string, after white space, that is a StrDecimalLiteral (a sign, `Infinity`, or digits with a point and an exponent); NaN if
 * there is none. "0px" is 0 and "0x10" is 0: the prefix is decimal. */
static double js_parse_float(const char * s) {
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') {
    s++;
  }
  const char * p = s;
  int negative = 0;
  if (*p == '+' || *p == '-') {
    negative = *p == '-';
    p++;
  }
  if (strncmp(p, "Infinity", 8) == 0) {
    return negative ? -INFINITY : INFINITY;
  }
  const char * q = p;
  size_t digits = 0;
  while (isdigit((unsigned char)*q)) {
    q++;
    digits++;
  }
  if (*q == '.') {
    q++;
    while (isdigit((unsigned char)*q)) {
      q++;
      digits++;
    }
  }
  if (!digits) {
    return NAN;
  }
  if (*q == 'e' || *q == 'E') {
    const char * e = q + 1;
    if (*e == '+' || *e == '-') {
      e++;
    }
    if (isdigit((unsigned char)*e)) {
      while (isdigit((unsigned char)*e)) {
        e++;
      }
      q = e;
    }
  }
  char buf[400];
  const size_t n = (size_t)(q - p) < sizeof buf - 1 ? (size_t)(q - p) : sizeof buf - 1;
  memcpy(buf, p, n);
  buf[n] = '\0';
  const double v = strtod(buf, NULL);
  return negative ? -v : v;
}

static double to_double(const value * v) {
  return v->kind == 'n' ? strtod(v->s, NULL) : (v->kind == 'b' ? (strcmp(v->s, "true") == 0 ? 1.0 : 0.0) : 0.0);
}

/* the ops of the Animation of the model; NULL and *handled 0 for an op that is not one of them */
static char * ctl_op(world * w, const op * o, int * program_error, int * handled) {
  const char * name = o->f[0];
  *handled = 1;
  int ok;
  if (strcmp(name, "calc") == 0) {
    /* calc R OP A B: arithmetic on the numbers a page does on the times of the timeline */
    if (o->n != 5) {
      *program_error = 1;
      return dups("calc has the wrong number of fields");
    }
    value a = operand(w, o->f[3], o->l[3], &ok), b;
    if (!ok || (a.kind != 'n')) {
      value_free(&a);
      *program_error = 1;
      return dups("calc: an operand that is not a number the program made");
    }
    b = operand(w, o->f[4], o->l[4], &ok);
    if (!ok || b.kind != 'n') {
      value_free(&a);
      value_free(&b);
      *program_error = 1;
      return dups("calc: an operand that is not a number the program made");
    }
    const double x = to_double(&a), y = to_double(&b);
    const char c = o->f[2][0];
    const double r = c == '+' ? x + y : c == '-' ? x - y : c == '*' ? x * y : x / y;
    put_reg(w, o->f[1], number_value(r));
    value_free(&a);
    value_free(&b);
    return NULL;
  }
  if (strcmp(name, "pfloat") == 0) {
    /* pfloat R S: parseFloat of a string (or of null, which is "null": NaN) */
    if (o->n != 3) {
      *program_error = 1;
      return dups("pfloat has the wrong number of fields");
    }
    value a = operand(w, o->f[2], o->l[2], &ok);
    if (!ok) {
      *program_error = 1;
      return dups("pfloat: an operand the program did not make");
    }
    double r = NAN;
    if (a.kind == 's' || a.kind == 'n') {
      r = js_parse_float(a.s);
    }
    put_reg(w, o->f[1], number_value(r));
    value_free(&a);
    return NULL;
  }
  *handled = 0;
  if (strcmp(name, "pgen") != 0 && strcmp(name, "pwait") != 0 && strcmp(name, "am") != 0 && strcmp(name, "asetv") != 0) {
    return NULL;
  }
  *handled = 1;
  if (o->n < 3) {
    *program_error = 1;
    return fmt("%s has too few fields", name);
  }
  /* pgen R A which; pwait A which GEN EXPECT [R]; am A method [ARG]; asetv A member VALUE: the animation is the second field of pgen and the first of the others */
  const char * an = strcmp(name, "pgen") == 0 ? o->f[2] : o->f[1];
  animh * h = find_ah(w, an);
  if (!h) {
    *program_error = 1;
    return fmt("%s names the animation %s, which the program did not make", name, an);
  }
  if (h->missing) {
    return dups("TypeError: Cannot read properties of undefined");
  }
  if (!w->anim) {
    return dups("an Animation was touched in a run with no animation world");
  }
  /* what a page reads of an animation or does to it is of the animation after the style change events that the page's changes to the style have made: a browser brings the style of the document up to
   * date first (the page of a CSS animation changes `animation-play-state` and asks for its `ready` promise in the next statement) */
  {
    char * recalc_failure = style_recalc(w);
    if (recalc_failure) {
      return recalc_failure;
    }
  }
  ctl * c = ctl_get(w, h);
  if (!c) {
    return dups("the Animation of the animation could not be made");
  }
  const GTEXT_Anim_Time now = {true, w->now_ms};
  if (strcmp(name, "pgen") == 0) {
    if (o->n != 4) {
      *program_error = 1;
      return dups("pgen has the wrong number of fields");
    }
    GTEXT_Anim_Animation_State st;
    (void)gtext_anim_animation_state(c->a, now, &st);
    put_reg(w, o->f[1], number_value((double)(strcmp(o->f[3], "ready") == 0 ? st.ready_generation : st.finished_generation)));
    return NULL;
  }
  if (strcmp(name, "pwait") == 0) {
    if (o->n != 5 && o->n != 6) {
      *program_error = 1;
      return dups("pwait has the wrong number of fields");
    }
    const int which = strcmp(o->f[2], "ready") == 0 ? 0 : 1;
    value g = operand(w, o->f[3], o->l[3], &ok);
    if (!ok || g.kind != 'n') {
      value_free(&g);
      *program_error = 1;
      return dups("pwait: the generation is not a number the program made");
    }
    const uint32_t gen = (uint32_t)to_double(&g);
    value_free(&g);
    char * failure = dispatch_queue(w);
    if (failure) {
      return failure;
    }
    int state;
    for (;;) {
      failure = ctl_drain_all(w);
      if (failure) {
        return failure;
      }
      state = ctl_promise_state(w, c, which, gen);
      if (state != 0) {
        break;
      }
      failure = step_frame(w);
      if (failure) {
        return failure;
      }
    }
    const char * expect = o->f[4];
    if (strncmp(expect, "dom:", 4) == 0) {
      if (state != 2) {
        return fmt("promise_rejects_dom: the promise was fulfilled, expected a rejection with %s", expect + 4);
      }
      if (strcmp(expect + 4, "AbortError") != 0) {
        return fmt("promise_rejects_dom: the promise was rejected with AbortError, expected %s", expect + 4);   /* the model rejects with an AbortError and nothing else */
      }
      return NULL;
    }
    if (strcmp(expect, "rej") == 0) {
      if (state != 2) {
        return dups("assert_unreached: the promise was fulfilled, and the test expected a rejection");
      }
      if (o->n == 6) {
        put_reg(w, o->f[5], val('s', "AbortError", 10));
      }
      return NULL;
    }
    if (state == 2) {
      return dups("AbortError: the promise was rejected");   /* the await of a rejected promise throws */
    }
    return NULL;
  }
  /* no microtask checkpoint here: a browser runs microtasks when the JavaScript stack is empty, which is where the page awaits or ends its task (a `pwait`, a frame), and not between the statements of one
   * task (`a.currentTime = <the end>; a.currentTime = 0` finishes nothing: the notification of the first finds the second's state) */
  GTEXT_Anim_Status st = GTEXT_ANIM_OK;
  const char * what = name;
  if (strcmp(name, "am") == 0) {
    if (o->n < 3 || o->n > 4) {
      *program_error = 1;
      return dups("am has the wrong number of fields");
    }
    const char * m = o->f[2];
    what = m;
    double arg = 0;
    if (o->n == 4) {
      value v = operand(w, o->f[3], o->l[3], &ok);
      if (!ok || v.kind != 'n') {
        value_free(&v);
        *program_error = 1;
        return fmt("am: the argument of %s is not a number the program made", m);
      }
      arg = to_double(&v);
      value_free(&v);
    }
    if (strcmp(m, "play") == 0) {
      st = gtext_anim_animation_play(c->a, now);
    }
    else if (strcmp(m, "pause") == 0) {
      st = gtext_anim_animation_pause(c->a, now);
    }
    else if (strcmp(m, "finish") == 0) {
      st = gtext_anim_animation_finish(c->a, now);
    }
    else if (strcmp(m, "cancel") == 0) {
      st = gtext_anim_animation_cancel(c->a, now);
    }
    else if (strcmp(m, "reverse") == 0) {
      st = gtext_anim_animation_reverse(c->a, now);
    }
    else if (strcmp(m, "persist") == 0) {
      st = gtext_anim_animation_persist(c->a);
    }
    else if (strcmp(m, "commitStyles") == 0) {
      /* "commit computed styles": the declarations are written to the inline style of the target, which flushes a style change event */
      GTEXT_Anim_Style_Values * list = NULL;
      st = gtext_anim_world_commit_styles(w->anim, c->a, &list);
      if (st == GTEXT_ANIM_OK && gtext_anim_style_values_count(list)) {
        const void * handle = gtext_anim_style_values_element(list);
        GTEXT_Markup_Node * node = node_of_handle(w, handle);
        GTEXT_CSSOM_Decl * d = NULL;
        sync_world(w);
        const GTEXT_CSS_Status is = node ? gtext_cssom_inline_style(w->doc, node, &w->attr, &d) : GTEXT_CSS_E_INVALID;
        if (is != GTEXT_CSS_OK) {
          gtext_anim_style_values_free(list);
          *program_error = 1;
          return dups("commitStyles: the inline style of the target could not be made");
        }
        for (size_t i = 0; i < gtext_anim_style_values_count(list); i++) {
          GTEXT_CSS_String pn, pv;
          (void)gtext_anim_style_values_at(list, i, &pn, &pv);
          (void)gtext_cssom_decl_set_property(d, pn.data, pn.length, pv.data, pv.length, "", 0, NULL);
        }
        gtext_cssom_decl_release(d);
      }
      gtext_anim_style_values_free(list);
    }
    else if (strcmp(m, "updatePlaybackRate") == 0 && o->n == 4) {
      st = gtext_anim_animation_update_playback_rate(c->a, arg, now);
    }
    else {
      *program_error = 1;
      return fmt("am does not know the method %s", m);
    }
  }
  else {
    /* asetv A member VALUE */
    if (o->n != 4) {
      *program_error = 1;
      return dups("asetv has the wrong number of fields");
    }
    value v = operand(w, o->f[3], o->l[3], &ok);
    if (!ok) {
      *program_error = 1;
      return dups("asetv: an operand the program did not make");
    }
    what = o->f[2];
    GTEXT_Anim_Time t = {v.kind == 'n', v.kind == 'n' ? to_double(&v) : 0.0};
    if (v.kind != 'n' && v.kind != 'N') {
      value_free(&v);
      *program_error = 1;
      return dups("asetv: the value is neither a number nor null");
    }
    if (strcmp(what, "currentTime") == 0) {
      st = gtext_anim_animation_set_current_time(c->a, t, now);
    }
    else if (strcmp(what, "startTime") == 0) {
      st = gtext_anim_animation_set_start_time(c->a, t, now);
    }
    else if (strcmp(what, "playbackRate") == 0) {
      if (!t.resolved) {
        value_free(&v);
        *program_error = 1;   /* `playbackRate = null` is 0 in JavaScript (a `double`), which the evaluator does not write: it refuses a value that is not a number */
        return dups("asetv: a null playbackRate is not a program the evaluator makes");
      }
      st = gtext_anim_animation_set_playback_rate(c->a, t.ms, now);
    }
    else {
      value_free(&v);
      *program_error = 1;
      return fmt("asetv does not know the member %s", what);
    }
    value_free(&v);
  }
  if (!c->orphan) {
    (void)gtext_anim_world_touch(w->anim);   /* what the Animation does reaches the style the page reads next */
  }
  if (st != GTEXT_ANIM_OK) {
    return anim_status_failure(what, st, program_error);
  }
  return NULL;
}

/* The member of getTiming() (`computed` false) or getComputedTiming() of a timing, an easing text and the computed timing: 0 for a member it does not have. */
static int timing_member(const char * member, int computed, const GTEXT_Anim_Effect_Timing * t, const char * easing, const GTEXT_Anim_Computed_Timing * ct, value * out) {
  value v;
  int known = 1;
  if (strcmp(member, "delay") == 0) {
    v = number_value(t->delay);
  }
  else if (strcmp(member, "endDelay") == 0) {
    v = number_value(t->end_delay);
  }
  else if (strcmp(member, "fill") == 0) {
    const char * s = fill_name(computed ? ct->timing.fill : t->fill);
    v = val('s', s, strlen(s));
  }
  else if (strcmp(member, "iterationStart") == 0) {
    v = number_value(t->iteration_start);
  }
  else if (strcmp(member, "iterations") == 0) {
    v = number_value(t->iterations);
  }
  else if (strcmp(member, "duration") == 0) {
    v = !computed && t->duration_auto ? val('s', "auto", 4) : number_value(computed ? ct->timing.duration : t->duration);
  }
  else if (strcmp(member, "direction") == 0) {
    const char * s = direction_name(t->direction);
    v = val('s', s, strlen(s));
  }
  else if (strcmp(member, "easing") == 0) {
    v = val('s', easing, strlen(easing));
  }
  else if (computed && strcmp(member, "endTime") == 0) {
    v = number_value(ct->end_time);
  }
  else if (computed && strcmp(member, "activeDuration") == 0) {
    v = number_value(ct->active_duration);
  }
  else if (computed && strcmp(member, "localTime") == 0) {
    v = time_value(ct->local_time);
  }
  else if (computed && strcmp(member, "progress") == 0) {
    v = ct->has_progress ? number_value(ct->progress) : val('N', "", 0);
  }
  else if (computed && strcmp(member, "currentIteration") == 0) {
    v = ct->has_current_iteration ? number_value(ct->current_iteration) : val('N', "", 0);
  }
  else {
    known = 0;
    v = val('u', "", 0);
  }
  *out = v;
  return known;
}

/* ---- script animations and keyframe effects (animation/effect.h) ------------------------------------------------------------------------------- */

/* The JSON the evaluator writes for the arguments of animate(), the constructor of KeyframeEffect and updateTiming(): null, true, false, numbers (Python writes Infinity and NaN as bare words), strings, arrays and
 * objects. */
typedef struct jv {
  char kind;               /* N null, b boolean, n number, s string, a array, o object */
  double num;
  int boolean;
  char * s;
  size_t slen;
  struct jv * items;
  char ** keys;
  size_t n;
} jv;

static void jv_free(jv * v) {
  free(v->s);
  for (size_t i = 0; i < v->n; i++) {
    jv_free(&v->items[i]);
    if (v->keys) {
      free(v->keys[i]);
    }
  }
  free(v->items);
  free(v->keys);
  memset(v, 0, sizeof *v);
}

static void jv_skip(const char ** p) {
  while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
    (*p)++;
  }
}

static int jv_hex4(const char * p, unsigned * out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    const char c = p[i];
    v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return 0;
    }
  }
  *out = v;
  return 1;
}

static size_t jv_utf8(unsigned cp, char * out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static int jv_string(const char ** p, char ** out, size_t * len) {
  if (**p != '"') {
    return 0;
  }
  (*p)++;
  size_t cap = 16, n = 0;
  char * buf = malloc(cap);
  while (**p && **p != '"') {
    if (n + 8 > cap) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
    if (**p == '\\') {
      (*p)++;
      const char c = **p;
      if (c == '\0') {
        free(buf);
        return 0;
      }
      if (c == 'u') {
        unsigned cp = 0;
        if (!jv_hex4(*p + 1, &cp)) {
          free(buf);
          return 0;
        }
        *p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && (*p)[1] == '\\' && (*p)[2] == 'u') {
          unsigned lo = 0;
          if (jv_hex4(*p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            *p += 6;
          }
        }
        n += jv_utf8(cp, buf + n);
      }
      else {
        buf[n++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
      }
      (*p)++;
    }
    else {
      buf[n++] = *(*p)++;
    }
  }
  if (**p != '"') {
    free(buf);
    return 0;
  }
  (*p)++;
  buf[n] = '\0';
  *out = buf;
  *len = n;
  return 1;
}

static int jv_parse_value(const char ** p, jv * out, int depth) {
  memset(out, 0, sizeof *out);
  jv_skip(p);
  if (depth > 40) {
    return 0;
  }
  if (**p == '"') {
    out->kind = 's';
    return jv_string(p, &out->s, &out->slen);
  }
  if (**p == '[' || **p == '{') {
    const int object = **p == '{';
    out->kind = object ? 'o' : 'a';
    (*p)++;
    jv_skip(p);
    size_t cap = 0;
    if (**p == (object ? '}' : ']')) {
      (*p)++;
      return 1;
    }
    for (;;) {
      if (out->n == cap) {
        cap = cap ? cap * 2 : 4;
        out->items = realloc(out->items, cap * sizeof *out->items);
        if (object) {
          out->keys = realloc(out->keys, cap * sizeof *out->keys);
        }
      }
      char * key = NULL;
      if (object) {
        size_t kl;
        jv_skip(p);
        if (!jv_string(p, &key, &kl)) {
          return 0;
        }
        jv_skip(p);
        if (*(*p)++ != ':') {
          free(key);
          return 0;
        }
      }
      if (!jv_parse_value(p, &out->items[out->n], depth + 1)) {
        jv_free(&out->items[out->n]);   /* what the child had made before it failed */
        free(key);
        return 0;
      }
      if (object) {
        out->keys[out->n] = key;
      }
      out->n++;
      jv_skip(p);
      if (**p == ',') {
        (*p)++;
        continue;
      }
      if (**p == (object ? '}' : ']')) {
        (*p)++;
        return 1;
      }
      return 0;
    }
  }
  if (strncmp(*p, "null", 4) == 0) {
    out->kind = 'N';
    *p += 4;
    return 1;
  }
  if (strncmp(*p, "true", 4) == 0 || strncmp(*p, "false", 5) == 0) {
    out->kind = 'b';
    out->boolean = **p == 't';
    *p += out->boolean ? 4 : 5;
    return 1;
  }
  char * end = NULL;
  const double d = strncmp(*p, "Infinity", 8) == 0 ? INFINITY : strncmp(*p, "-Infinity", 9) == 0 ? -INFINITY : strncmp(*p, "NaN", 3) == 0 ? NAN : strtod(*p, &end);
  if (strncmp(*p, "Infinity", 8) == 0) {
    end = (char *)*p + 8;
  }
  else if (strncmp(*p, "-Infinity", 9) == 0) {
    end = (char *)*p + 9;
  }
  else if (strncmp(*p, "NaN", 3) == 0) {
    end = (char *)*p + 3;
  }
  if (!end || end == *p) {
    return 0;
  }
  out->kind = 'n';
  out->num = d;
  *p = end;
  return 1;
}

static int jv_parse(const char * text, jv * out) {
  const char * p = text;
  if (!jv_parse_value(&p, out, 0)) {
    jv_free(out);
    return 0;
  }
  jv_skip(&p);
  return *p == '\0';
}

static jv * jv_get(jv * o, const char * key) {
  if (o->kind != 'o') {
    return NULL;
  }
  for (size_t i = 0; i < o->n; i++) {
    if (strcmp(o->keys[i], key) == 0) {
      return &o->items[i];
    }
  }
  return NULL;
}

/* The storage the arguments the library takes point into. */
typedef struct {
  GTEXT_Anim_Keyframes_Argument arg;
  GTEXT_Anim_Keyframe_Like * frames;
  GTEXT_Anim_Keyframe_Property_Input ** props;
  GTEXT_Anim_Property_Indexed idx;
  GTEXT_Anim_Offset * offsets;
  GTEXT_Anim_Text * easings;
  GTEXT_Anim_Keyframe_Composite * composites;
  GTEXT_Anim_Property_List_Input * lists;
  GTEXT_Anim_Text ** values;
  size_t nframes;
} kfstore;

static void kfstore_free(kfstore * k) {
  if (k->props) {
    for (size_t i = 0; i < k->nframes; i++) {
      free(k->props[i]);
    }
  }
  free(k->frames);
  free(k->props);
  free(k->offsets);
  free(k->easings);
  free(k->composites);
  if (k->values) {
    for (size_t i = 0; i < k->idx.property_count; i++) {
      free(k->values[i]);
    }
  }
  free(k->values);
  free(k->lists);
  memset(k, 0, sizeof *k);
}

static int composite_enum(const char * s, int allow_auto, GTEXT_Anim_Keyframe_Composite * out) {
  if (strcmp(s, "replace") == 0) {
    *out = GTEXT_ANIM_KF_COMPOSITE_REPLACE;
  }
  else if (strcmp(s, "add") == 0) {
    *out = GTEXT_ANIM_KF_COMPOSITE_ADD;
  }
  else if (strcmp(s, "accumulate") == 0) {
    *out = GTEXT_ANIM_KF_COMPOSITE_ACCUMULATE;
  }
  else if (allow_auto && strcmp(s, "auto") == 0) {
    *out = GTEXT_ANIM_KF_COMPOSITE_AUTO;
  }
  else {
    return 0;
  }
  return 1;
}

static GTEXT_Anim_Offset jv_offset(const jv * v) {
  GTEXT_Anim_Offset o = {0, 0};
  o.is_null = v->kind != 'n';
  o.value = v->kind == 'n' ? v->num : 0;
  return o;
}

/* "Process a keyframes argument" is the library's; this is the reading of what the evaluator wrote into the structs it takes. An enumeration that is not one of its values is a TypeError of the conversion of
 * the IDL, before the library is called. NULL, or the failure. */
static char * kf_build(jv * v, kfstore * k) {
  memset(k, 0, sizeof *k);
  jv * kind = jv_get(v, "k");
  if (!kind || kind->kind != 's') {
    return dups("the keyframes argument is not one the evaluator writes");
  }
  if (strcmp(kind->s, "null") == 0) {
    k->arg.kind = GTEXT_ANIM_KEYFRAMES_NULL;
    return NULL;
  }
  if (strcmp(kind->s, "array") == 0) {
    jv * frames = jv_get(v, "frames");
    if (!frames || frames->kind != 'a') {
      return dups("the keyframes argument of the op has no frames");
    }
    k->arg.kind = GTEXT_ANIM_KEYFRAMES_ARRAY;
    k->nframes = frames->n;
    k->frames = calloc(frames->n ? frames->n : 1, sizeof *k->frames);
    k->props = calloc(frames->n ? frames->n : 1, sizeof *k->props);
    for (size_t i = 0; i < frames->n; i++) {
      jv * f = &frames->items[i];
      GTEXT_Anim_Keyframe_Like * out = &k->frames[i];
      if (f->kind == 'N') {
        out->null_entry = true;
        out->offset.is_null = true;
        continue;
      }
      out->offset.is_null = true;
      jv * off = jv_get(f, "offset");
      if (off) {
        out->offset = jv_offset(off);
      }
      jv * ease = jv_get(f, "easing");
      if (ease) {
        out->easing = (GTEXT_Anim_Text){ease->s, ease->slen};
      }
      jv * comp = jv_get(f, "composite");
      if (comp && !composite_enum(comp->s, 1, &out->composite)) {
        kfstore_free(k);
        return fmt("TypeError: '%s' is not a valid value for the enumeration CompositeOperationOrAuto", comp->s);
      }
      jv * props = jv_get(f, "props");
      out->property_count = props->n;
      k->props[i] = calloc(props->n ? props->n : 1, sizeof **k->props);
      for (size_t j = 0; j < props->n; j++) {
        jv * pair = &props->items[j];
        k->props[i][j].name = (GTEXT_Anim_Text){pair->items[0].s, pair->items[0].slen};
        k->props[i][j].value = (GTEXT_Anim_Text){pair->items[1].s, pair->items[1].slen};
      }
      out->properties = k->props[i];
    }
    k->arg.array = k->frames;
    k->arg.array_count = frames->n;
    return NULL;
  }
  /* indexed */
  jv * offs = jv_get(v, "offset");
  jv * eas = jv_get(v, "easing");
  jv * cmp = jv_get(v, "composite");
  jv * props = jv_get(v, "props");
  if (!offs || !eas || !cmp || !props || offs->kind != 'a' || eas->kind != 'a' || cmp->kind != 'a' || props->kind != 'a') {
    return dups("the keyframes argument of the op is not the property-indexed form the evaluator writes");
  }
  k->arg.kind = GTEXT_ANIM_KEYFRAMES_INDEXED;
  k->offsets = calloc(offs->n ? offs->n : 1, sizeof *k->offsets);
  for (size_t i = 0; i < offs->n; i++) {
    k->offsets[i] = jv_offset(&offs->items[i]);
  }
  k->easings = calloc(eas->n ? eas->n : 1, sizeof *k->easings);
  for (size_t i = 0; i < eas->n; i++) {
    k->easings[i] = (GTEXT_Anim_Text){eas->items[i].s, eas->items[i].slen};
  }
  k->composites = calloc(cmp->n ? cmp->n : 1, sizeof *k->composites);
  for (size_t i = 0; i < cmp->n; i++) {
    if (!composite_enum(cmp->items[i].s, 1, &k->composites[i])) {
      char * m = fmt("TypeError: '%s' is not a valid value for the enumeration CompositeOperationOrAuto", cmp->items[i].s);
      kfstore_free(k);
      return m;
    }
  }
  k->lists = calloc(props->n ? props->n : 1, sizeof *k->lists);
  k->values = calloc(props->n ? props->n : 1, sizeof *k->values);
  k->idx.property_count = props->n;
  for (size_t i = 0; i < props->n; i++) {
    jv * pair = &props->items[i];
    jv * vals = &pair->items[1];
    k->values[i] = calloc(vals->n ? vals->n : 1, sizeof **k->values);
    for (size_t j = 0; j < vals->n; j++) {
      k->values[i][j] = (GTEXT_Anim_Text){vals->items[j].s, vals->items[j].slen};
    }
    k->lists[i].name = (GTEXT_Anim_Text){pair->items[0].s, pair->items[0].slen};
    k->lists[i].values = k->values[i];
    k->lists[i].value_count = vals->n;
  }
  k->idx.offsets = k->offsets;
  k->idx.offset_count = offs->n;
  k->idx.easings = k->easings;
  k->idx.easing_count = eas->n;
  k->idx.composites = k->composites;
  k->idx.composite_count = cmp->n;
  k->idx.properties = k->lists;
  k->arg.indexed = &k->idx;
  return NULL;
}

static int fill_enum(const char * s, GTEXT_Anim_Fill * out) {
  static const char * const names[] = {"auto", "none", "forwards", "backwards", "both"};
  for (int i = 0; i < 5; i++) {
    if (strcmp(s, names[i]) == 0) {
      *out = (GTEXT_Anim_Fill)i;
      return 1;
    }
  }
  return 0;
}

static int direction_enum(const char * s, GTEXT_Anim_Direction * out) {
  static const char * const names[] = {"normal", "reverse", "alternate", "alternate-reverse"};
  for (int i = 0; i < 4; i++) {
    if (strcmp(s, names[i]) == 0) {
      *out = (GTEXT_Anim_Direction)i;
      return 1;
    }
  }
  return 0;
}

/* The members of an `EffectTiming` the dictionary `t` has: the conversions of the IDL (an enumeration that is not one of its values, a `duration` string that is not `auto`) are TypeErrors. */
static char * timing_build(jv * t, GTEXT_Anim_Optional_Timing * out, GTEXT_Anim_Text * easing) {
  memset(out, 0, sizeof *out);
  out->values = gtext_anim_timing_default();
  for (size_t i = 0; t && i < t->n; i++) {
    const char * k = t->keys[i];
    jv * v = &t->items[i];
    if (strcmp(k, "delay") == 0) {
      out->present |= GTEXT_ANIM_TIMING_DELAY;
      out->values.delay = v->num;
    }
    else if (strcmp(k, "endDelay") == 0) {
      out->present |= GTEXT_ANIM_TIMING_END_DELAY;
      out->values.end_delay = v->num;
    }
    else if (strcmp(k, "iterationStart") == 0) {
      out->present |= GTEXT_ANIM_TIMING_ITERATION_START;
      out->values.iteration_start = v->num;
    }
    else if (strcmp(k, "iterations") == 0) {
      out->present |= GTEXT_ANIM_TIMING_ITERATIONS;
      out->values.iterations = v->num;
    }
    else if (strcmp(k, "duration") == 0) {
      out->present |= GTEXT_ANIM_TIMING_DURATION;
      if (v->kind == 's') {
        if (strcmp(v->s, "auto") != 0) {
          return fmt("TypeError: duration must be a number or \"auto\", not \"%s\"", v->s);
        }
        out->values.duration_auto = true;
        out->values.duration = 0;
      }
      else {
        out->values.duration_auto = false;
        out->values.duration = v->num;
      }
    }
    else if (strcmp(k, "fill") == 0) {
      out->present |= GTEXT_ANIM_TIMING_FILL;
      if (!fill_enum(v->s, &out->values.fill)) {
        return fmt("TypeError: '%s' is not a valid value for the enumeration FillMode", v->s);
      }
    }
    else if (strcmp(k, "direction") == 0) {
      out->present |= GTEXT_ANIM_TIMING_DIRECTION;
      if (!direction_enum(v->s, &out->values.direction)) {
        return fmt("TypeError: '%s' is not a valid value for the enumeration PlaybackDirection", v->s);
      }
    }
    else if (strcmp(k, "easing") == 0) {
      out->present |= GTEXT_ANIM_TIMING_EASING;
      *easing = (GTEXT_Anim_Text){v->s, v->slen};
    }
  }
  return NULL;
}

static char * options_build(jv * v, GTEXT_Anim_Effect_Options * out) {
  memset(out, 0, sizeof *out);
  GTEXT_Anim_Text easing = {NULL, 0};
  char * failure = timing_build(jv_get(v, "timing"), &out->timing, &easing);
  if (failure) {
    return failure;
  }
  out->easing = easing;
  jv * c = jv_get(v, "composite");
  if (c) {
    GTEXT_Anim_Keyframe_Composite kc;
    if (!composite_enum(c->s, 0, &kc)) {
      return fmt("TypeError: '%s' is not a valid value for the enumeration CompositeOperation", c->s);
    }
    out->has_composite = true;
    out->composite = (GTEXT_Anim_Composite)(kc - GTEXT_ANIM_KF_COMPOSITE_REPLACE);
  }
  c = jv_get(v, "iterationComposite");
  if (c) {
    if (strcmp(c->s, "replace") != 0 && strcmp(c->s, "accumulate") != 0) {
      return fmt("TypeError: '%s' is not a valid value for the enumeration IterationCompositeOperation", c->s);
    }
    out->has_iteration_composite = true;
    out->iteration_composite = strcmp(c->s, "replace") == 0 ? GTEXT_ANIM_ITERATION_REPLACE : GTEXT_ANIM_ITERATION_ACCUMULATE;
  }
  c = jv_get(v, "pseudoElement");
  if (c) {
    out->has_pseudo = true;
    if (c->kind == 's') {
      out->pseudo = (GTEXT_Anim_Text){c->s, c->slen};
    }
  }
  return NULL;
}

/* The effects and keyframe lists the program holds. */
static effh * find_eh(world * w, const char * name) {
  for (size_t i = w->neffs; i-- > 0;) {
    if (strcmp(w->effs[i].name, name) == 0) {
      return &w->effs[i];
    }
  }
  return NULL;
}

static void put_eh(world * w, const char * name, GTEXT_Anim_Effect * e) {
  effh * old = find_eh(w, name);
  if (old) {
    old->e = e;
    return;
  }
  if (w->neffs == w->capeffs) {
    w->capeffs = w->capeffs ? w->capeffs * 2 : 8;
    w->effs = realloc(w->effs, w->capeffs * sizeof *w->effs);
    if (!w->effs) {
      abort();
    }
  }
  w->effs[w->neffs].name = dups(name);
  w->effs[w->neffs].e = e;
  w->neffs++;
}

static kfl * find_kfl(world * w, const char * name) {
  for (size_t i = w->nkfls; i-- > 0;) {
    if (strcmp(w->kfls[i].name, name) == 0) {
      return &w->kfls[i];
    }
  }
  return NULL;
}

static void put_kfl(world * w, const char * name, GTEXT_Anim_Keyframes * k) {
  kfl * old = find_kfl(w, name);
  if (old) {
    gtext_anim_keyframes_free(old->k);
    old->k = k;
    return;
  }
  if (w->nkfls == w->capkfls) {
    w->capkfls = w->capkfls ? w->capkfls * 2 : 8;
    w->kfls = realloc(w->kfls, w->capkfls * sizeof *w->kfls);
    if (!w->kfls) {
      abort();
    }
  }
  w->kfls[w->nkfls].name = dups(name);
  w->kfls[w->nkfls].k = k;
  w->nkfls++;
}

static void put_ah(world * w, const char * name, GTEXT_Anim_Kind kind, uint64_t id) {
  animh h;
  memset(&h, 0, sizeof h);
  h.name = dups(name);
  h.kind = kind;
  h.id = id;
  h.pname = dups("");
  h.easing = dups("");
  h.idtext = dups("");
  animh * old = find_ah(w, name);
  if (old) {
    animh_free(old);
    *old = h;
    return;
  }
  if (w->nahs == w->capahs) {
    w->capahs = w->capahs ? w->capahs * 2 : 8;
    w->ahs = realloc(w->ahs, w->capahs * sizeof *w->ahs);
    if (!w->ahs) {
      abort();
    }
  }
  w->ahs[w->nahs++] = h;
}

/* An effect operand: `f:NAME` (an effect handle the program holds, or an animation's current effect) or `N:`. -1: a program fault; else 1 with `*e` the effect (NULL for null). */
static int effect_operand(world * w, const char * f, GTEXT_Anim_Effect ** e) {
  *e = NULL;
  if (strncmp(f, "N:", 2) == 0) {
    return 1;
  }
  if (strncmp(f, "f:", 2) != 0) {
    return -1;
  }
  effh * h = find_eh(w, f + 2);
  if (!h) {
    return -1;
  }
  *e = h->e;
  return 1;
}

/* The element an operand names (`e:NAME`) or null (`N:`): the tree's handle. */
static int element_operand(world * w, const char * f, const void ** handle, char ** failure) {
  *handle = NULL;
  *failure = NULL;
  if (strncmp(f, "N:", 2) == 0) {
    return 1;
  }
  if (strncmp(f, "e:", 2) != 0) {
    return -1;
  }
  elem * e = find_elem(w, f + 2);
  if (!e) {
    return -1;
  }
  if (!e->node || !is_element(e->node)) {
    *failure = dups("TypeError: the target is not an element");
    return 1;
  }
  *handle = gtext_css_markup_handle(e->node);
  return 1;
}

/* The node of an element handle the library gave back (an effect's target), found among the elements the runner knows. */
static GTEXT_Markup_Node * node_of_handle(world * w, const void * handle) {
  for (size_t i = 0; i < w->nelems; i++) {
    if (w->elems[i].node && gtext_css_markup_handle(w->elems[i].node) == handle) {
      return w->elems[i].node;
    }
  }
  for (size_t i = 0; i < w->nknown; i++) {
    if (w->known[i] && gtext_css_markup_handle(w->known[i]) == handle) {
      return w->known[i];
    }
  }
  for (size_t i = 0; i < w->ntracked; i++) {
    if (w->tracked[i] && gtext_css_markup_handle(w->tracked[i]) == handle) {
      return w->tracked[i];
    }
  }
  return NULL;
}

static const char * script_status_name(GTEXT_Anim_Status st) {
  return st == GTEXT_ANIM_SYNTAX_ERROR ? "SyntaxError" : NULL;
}

/* "Name: message" for a status of an effect call; a SyntaxError of a pseudoElement is the DOMException of that name. */
static char * effect_failure(const char * what, GTEXT_Anim_Status st, int * program_error) {
  const char * special = script_status_name(st);
  if (special) {
    return fmt("%s: %s", special, what);
  }
  return anim_status_failure(what, st, program_error);
}

static const char * composite_name(GTEXT_Anim_Composite c) {
  return c == GTEXT_ANIM_COMPOSITE_ADD ? "add" : c == GTEXT_ANIM_COMPOSITE_ACCUMULATE ? "accumulate" : "replace";
}

static const char * kf_composite_name(GTEXT_Anim_Keyframe_Composite c) {
  return c == GTEXT_ANIM_KF_COMPOSITE_ADD ? "add" : c == GTEXT_ANIM_KF_COMPOSITE_ACCUMULATE ? "accumulate" : c == GTEXT_ANIM_KF_COMPOSITE_REPLACE ? "replace" : "auto";
}

static int cmp_str(const void * a, const void * b) {
  return strcmp(*(char * const *)a, *(char * const *)b);
}

static int is_script_op(const char * name) {
  static const char * const names[] = {"anew", "animnew", "enew", "ecopy", "aeff", "aseteff", "asettl", "eget", "eset", "etarget", "esettarget", "ekf", "kflen", "kfget", "kfcmp", "kfhas", "esetkf", "eupdate"};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (strcmp(name, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static char * script_op(world * w, const op * o, int * program_error) {
  const char * name = o->f[0];
  *program_error = 0;
  if (!w->anim) {
    *program_error = 1;
    return dups("an animation op in a run with no animation world");
  }
#define NEEDN(n_) do { if (o->n != (n_)) { *program_error = 1; return fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); } } while (0)
  if (strcmp(name, "anew") == 0 || strcmp(name, "enew") == 0) {
    /* anew A E KF OPT | enew F E KF OPT */
    NEEDN(5);
    const void * target = NULL;
    char * failure = NULL;
    if (element_operand(w, o->f[2], &target, &failure) < 0) {
      *program_error = 1;
      return fmt("%s names the element %s, which the program did not make", name, o->f[2]);
    }
    if (failure) {
      return failure;
    }
    jv kjson, ojson;
    if (!jv_parse(o->f[3], &kjson) || !jv_parse(o->f[4], &ojson)) {
      *program_error = 1;
      return dups("the keyframes or the options of the op are not JSON");
    }
    kfstore ks;
    GTEXT_Anim_Effect_Options opt;
    failure = kf_build(&kjson, &ks);
    if (!failure) {
      failure = options_build(&ojson, &opt);
      if (failure) {
        kfstore_free(&ks);
      }
    }
    if (failure) {
      jv_free(&kjson);
      jv_free(&ojson);
      return failure;
    }
    sync_world(w);
    GTEXT_Anim_Effect * e = NULL;
    GTEXT_Anim_Status st;
    if (strcmp(name, "anew") == 0) {
      if (!target) {
        jv_free(&kjson);
        jv_free(&ojson);
        kfstore_free(&ks);
        return dups("TypeError: animate() was called on null");
      }
      GTEXT_Anim_Animation * a = NULL;
      uint64_t id = 0;
      st = gtext_anim_world_animate(w->anim, target, &ks.arg, &opt, &e, &id, &a);
      if (st == GTEXT_ANIM_OK) {
        put_ah(w, o->f[1], GTEXT_ANIM_KIND_SCRIPT, id);
        (void)ctl_get(w, find_ah(w, o->f[1]));
      }
    }
    else {
      st = gtext_anim_world_effect_new(w->anim, target, &ks.arg, &opt, &e);
      if (st == GTEXT_ANIM_OK) {
        put_eh(w, o->f[1], e);
      }
    }
    jv_free(&kjson);
    jv_free(&ojson);
    kfstore_free(&ks);
    if (st != GTEXT_ANIM_OK) {
      return effect_failure(strcmp(name, "anew") == 0 ? "animate() failed" : "the KeyframeEffect could not be made", st, program_error);
    }
    return NULL;
  }
  if (strcmp(name, "ecopy") == 0) {
    NEEDN(3);
    GTEXT_Anim_Effect * src = NULL;
    if (effect_operand(w, o->f[2], &src) < 0 || !src) {
      *program_error = 1;
      return dups("ecopy names an effect the program did not make");
    }
    GTEXT_Anim_Effect * e = NULL;
    const GTEXT_Anim_Status st = gtext_anim_world_effect_copy(w->anim, src, &e);
    if (st != GTEXT_ANIM_OK) {
      return effect_failure("the KeyframeEffect could not be copied", st, program_error);
    }
    put_eh(w, o->f[1], e);
    return NULL;
  }
  if (strcmp(name, "animnew") == 0) {
    /* animnew A F TL */
    NEEDN(4);
    GTEXT_Anim_Effect * e = NULL;
    if (effect_operand(w, o->f[2], &e) < 0) {
      *program_error = 1;
      return dups("animnew names an effect the program did not make");
    }
    GTEXT_Anim_Animation * a = NULL;
    uint64_t id = 0;
    const GTEXT_Anim_Status st = gtext_anim_world_animation_new(w->anim, e, o->f[3][0] == 'n' ? GTEXT_ANIM_TIMELINE_NONE : GTEXT_ANIM_TIMELINE_DOCUMENT, &id, &a);
    if (st != GTEXT_ANIM_OK) {
      return effect_failure("the Animation could not be made", st, program_error);
    }
    put_ah(w, o->f[1], GTEXT_ANIM_KIND_SCRIPT, id);
    (void)ctl_get(w, find_ah(w, o->f[1]));
    return NULL;
  }
  if (strcmp(name, "asettl") == 0) {
    /* asettl A d|n: animation.timeline = document.timeline | null ("Setting the timeline of an animation") */
    NEEDN(3);
    animh * h = find_ah(w, o->f[1]);
    if (!h) {
      *program_error = 1;
      return fmt("asettl names the animation %s, which the program did not make", o->f[1]);
    }
    if (h->missing) {
      return dups("TypeError: Cannot set properties of undefined");
    }
    ctl * c = ctl_get(w, h);
    if (!c) {
      return dups("the Animation could not be held");
    }
    const GTEXT_Anim_Status st = gtext_anim_animation_set_timeline(c->a, o->f[2][0] == 'n' ? GTEXT_ANIM_TIMELINE_NONE : GTEXT_ANIM_TIMELINE_DOCUMENT, (GTEXT_Anim_Time){true, w->now_ms});
    (void)gtext_anim_world_touch(w->anim);
    return st == GTEXT_ANIM_OK ? NULL : effect_failure("timeline could not be set", st, program_error);
  }
  if (strcmp(name, "aeff") == 0 || strcmp(name, "aseteff") == 0) {
    /* aeff F A | aseteff A F */
    NEEDN(3);
    animh * h = find_ah(w, o->f[strcmp(name, "aeff") == 0 ? 2 : 1]);
    if (!h) {
      *program_error = 1;
      return fmt("%s names the animation %s, which the program did not make", name, o->f[strcmp(name, "aeff") == 0 ? 2 : 1]);
    }
    if (h->missing) {
      return dups("TypeError: Cannot read properties of undefined");
    }
    ctl * c = ctl_get(w, h);
    if (!c) {
      return dups("the Animation could not be held");
    }
    GTEXT_Anim_Effect * e = NULL;
    GTEXT_Anim_Status st;
    if (strcmp(name, "aeff") == 0) {
      st = gtext_anim_world_effect_of(w->anim, c->a, &e);
      if (st != GTEXT_ANIM_OK) {
        return effect_failure("the effect could not be read", st, program_error);
      }
      put_eh(w, o->f[1], e);
      return NULL;
    }
    if (effect_operand(w, o->f[2], &e) < 0) {
      *program_error = 1;
      return dups("aseteff names an effect the program did not make");
    }
    if (h->kind != GTEXT_ANIM_KIND_SCRIPT) {
      return dups("NotSupportedError: replacing the effect of a CSS animation or transition is not built");
    }
    st = gtext_anim_animation_set_effect_object(c->a, e, (GTEXT_Anim_Time){true, w->now_ms});
    (void)gtext_anim_world_touch(w->anim);
    if (st != GTEXT_ANIM_OK) {
      return effect_failure("the effect could not be set", st, program_error);
    }
    return NULL;
  }
  /* the rest name an effect first (the second field, or the first for a write) */
  const int write = strcmp(name, "eset") == 0 || strcmp(name, "esettarget") == 0 || strcmp(name, "esetkf") == 0 || strcmp(name, "eupdate") == 0;
  if (strcmp(name, "kflen") == 0 || strcmp(name, "kfget") == 0 || strcmp(name, "kfcmp") == 0 || strcmp(name, "kfhas") == 0) {
    NEEDN(strcmp(name, "kflen") == 0 ? 3 : 5);
    const char * listname = strcmp(name, "kfcmp") == 0 ? o->f[1] : o->f[2];
    kfl * l = find_kfl(w, listname);
    if (!l) {
      *program_error = 1;
      return fmt("%s names the list %s, which the program did not make", name, listname);
    }
    if (strcmp(name, "kflen") == 0) {
      NEEDN(3);
      put_reg(w, o->f[1], number_value((double)gtext_anim_keyframes_count(l->k)));
      return NULL;
    }
    const size_t index = (size_t)strtoull(strcmp(name, "kfcmp") == 0 ? o->f[2] : o->f[3], NULL, 10);
    GTEXT_Anim_Keyframe_Info ki;
    if (strcmp(name, "kfhas") == 0) {
      /* kfhas R L I NAME: the keyframe has the property as an own property of the object getKeyframes() made of it */
      NEEDN(5);
      if (!gtext_anim_keyframes_at(l->k, index, &ki)) {
        return dups("TypeError: Cannot read properties of undefined");
      }
      static const char * const own[] = {"offset", "computedOffset", "easing", "composite"};
      int has = 0;
      for (size_t i = 0; i < 4; i++) {
        has = has || strcmp(o->f[4], own[i]) == 0;
      }
      for (size_t i = 0; i < ki.property_count && !has; i++) {
        GTEXT_CSS_String pn, idl, pv;
        (void)gtext_anim_keyframes_property_at(l->k, index, i, &pn, &idl, &pv);
        has = idl.length == strlen(o->f[4]) && memcmp(idl.data, o->f[4], idl.length) == 0;
      }
      put_reg(w, o->f[1], val('b', has ? "true" : "false", has ? 4 : 5));
      return NULL;
    }
    if (strcmp(name, "kfget") == 0) {
      NEEDN(5);
      if (!gtext_anim_keyframes_at(l->k, index, &ki)) {
        return dups("TypeError: Cannot read properties of undefined");
      }
      const char * member = o->f[4];
      value v;
      if (strcmp(member, "offset") == 0) {
        v = ki.offset_null ? val('N', "", 0) : number_value(ki.offset);
      }
      else if (strcmp(member, "computedOffset") == 0) {
        v = number_value(ki.computed_offset);
      }
      else if (strcmp(member, "easing") == 0) {
        v = val('s', ki.easing.data, ki.easing.length);
      }
      else if (strcmp(member, "composite") == 0) {
        const char * s = kf_composite_name(ki.composite);
        v = val('s', s, strlen(s));
      }
      else {
        v = val('u', "", 0);
        for (size_t i = 0; i < ki.property_count; i++) {
          GTEXT_CSS_String pn, idl, pv;
          (void)gtext_anim_keyframes_property_at(l->k, index, i, &pn, &idl, &pv);
          if (idl.length == strlen(member) && memcmp(idl.data, member, idl.length) == 0) {
            value_free(&v);
            v = val('s', pv.data, pv.length);
            break;
          }
        }
      }
      put_reg(w, o->f[1], v);
      return NULL;
    }
    /* kfcmp L I JSON NAME: assert_frames_equal(actual, expected, name) */
    NEEDN(5);
    if (!gtext_anim_keyframes_at(l->k, index, &ki)) {
      return dups("TypeError: Cannot convert undefined or null to object");
    }
    jv want;
    if (!jv_parse(o->f[3], &want) || want.kind != 'o') {
      *program_error = 1;
      jv_free(&want);
      return dups("kfcmp: the expected frame is not an object");
    }
    /* the keys of the keyframe: offset, computedOffset, easing, composite and its properties (by IDL name) */
    size_t nkeys = 4 + ki.property_count;
    char ** keys = calloc(nkeys, sizeof *keys);
    keys[0] = dups("composite");
    keys[1] = dups("computedOffset");
    keys[2] = dups("easing");
    keys[3] = dups("offset");
    for (size_t i = 0; i < ki.property_count; i++) {
      GTEXT_CSS_String pn, idl, pv;
      (void)gtext_anim_keyframes_property_at(l->k, index, i, &pn, &idl, &pv);
      keys[4 + i] = dupn(idl.data, idl.length);
    }
    char ** wkeys = calloc(want.n ? want.n : 1, sizeof *wkeys);
    for (size_t i = 0; i < want.n; i++) {
      wkeys[i] = dups(want.keys[i]);
    }
    qsort(keys, nkeys, sizeof *keys, cmp_str);
    qsort(wkeys, want.n, sizeof *wkeys, cmp_str);
    char * failure = NULL;
    if (nkeys != want.n) {
      failure = fmt("assert_array_equals: properties on %s should match: lengths differ, expected %zu got %zu", o->f[4], want.n, nkeys);
    }
    for (size_t i = 0; !failure && i < nkeys; i++) {
      if (strcmp(keys[i], wkeys[i]) != 0) {
        failure = fmt("assert_array_equals: properties on %s should match: expected \"%s\" but got \"%s\"", o->f[4], wkeys[i], keys[i]);
      }
    }
    for (size_t i = 0; !failure && i < nkeys; i++) {
      const char * key = keys[i];
      jv * ev = jv_get(&want, key);
      value got;
      if (strcmp(key, "offset") == 0) {
        got = ki.offset_null ? val('N', "", 0) : number_value(ki.offset);
      }
      else if (strcmp(key, "computedOffset") == 0) {
        got = number_value(ki.computed_offset);
      }
      else if (strcmp(key, "easing") == 0) {
        got = val('s', ki.easing.data, ki.easing.length);
      }
      else if (strcmp(key, "composite") == 0) {
        const char * s = kf_composite_name(ki.composite);
        got = val('s', s, strlen(s));
      }
      else {
        got = val('u', "", 0);
        for (size_t j = 0; j < ki.property_count; j++) {
          GTEXT_CSS_String pn, idl, pv;
          (void)gtext_anim_keyframes_property_at(l->k, index, j, &pn, &idl, &pv);
          if (idl.length == strlen(key) && memcmp(idl.data, key, idl.length) == 0) {
            value_free(&got);
            got = val('s', pv.data, pv.length);
            break;
          }
        }
      }
      value expected = ev->kind == 'n' ? number_value(ev->num) : ev->kind == 's' ? val('s', ev->s, ev->slen) : val('N', "", 0);
      const int approx = ((strcmp(key, "offset") == 0 && got.kind == 'n') || strcmp(key, "computedOffset") == 0) && expected.kind == 'n' && got.kind == 'n';
      int ok = approx ? fabs(strtod(got.s, NULL) - strtod(expected.s, NULL)) <= 0.00001 : same_as(&got, &expected, 1);
      if (!ok) {
        char m[300];
        snprintf(m, sizeof m, "value for '%s' on %s should match", key, o->f[4]);
        failure = mismatch("assert_equals", m, &expected, &got);
      }
      value_free(&got);
      value_free(&expected);
    }
    for (size_t i = 0; i < nkeys; i++) {
      free(keys[i]);
    }
    for (size_t i = 0; i < want.n; i++) {
      free(wkeys[i]);
    }
    free(keys);
    free(wkeys);
    jv_free(&want);
    return failure;
  }
  /* eget R F member | etarget N F | ekf L F | eset F member VALUE | esettarget F E | esetkf F KF | eupdate F JSON */
  const size_t at = write ? 1 : 2;
  if (o->n <= at) {
    *program_error = 1;
    return fmt("%s has too few fields", name);
  }
  GTEXT_Anim_Effect * e = NULL;
  effh * eh = find_eh(w, o->f[at]);
  if (!eh) {
    *program_error = 1;
    return fmt("%s names the effect %s, which the program did not make", name, o->f[at]);
  }
  e = eh->e;
  if (!e) {
    return dups("TypeError: Cannot read properties of null");
  }
  GTEXT_Anim_Status st;
  if (strcmp(name, "eget") == 0) {
    NEEDN(4);
    const char * member = o->f[3];
    value v;
    if (strcmp(member, "composite") == 0) {
      const char * s = composite_name(gtext_anim_effect_get_composite(e));
      v = val('s', s, strlen(s));
    }
    else if (strcmp(member, "iterationComposite") == 0) {
      const char * s = gtext_anim_effect_get_iteration_composite(e) == GTEXT_ANIM_ITERATION_ACCUMULATE ? "accumulate" : "replace";
      v = val('s', s, strlen(s));
    }
    else if (strcmp(member, "pseudoElement") == 0) {
      const void * el;
      GTEXT_CSS_String ps;
      (void)gtext_anim_effect_get_target(e, &el, &ps);
      v = ps.length ? val('s', ps.data, ps.length) : val('N', "", 0);
    }
    else {
      *program_error = 1;
      return fmt("eget does not know the member %s", member);
    }
    put_reg(w, o->f[1], v);
    return NULL;
  }
  if (strcmp(name, "etarget") == 0) {
    NEEDN(3);
    const void * el = NULL;
    GTEXT_CSS_String ps;
    (void)gtext_anim_effect_get_target(e, &el, &ps);
    put_elem(w, o->f[1], el ? node_of_handle(w, el) : NULL);
    return NULL;
  }
  if (strcmp(name, "ekf") == 0) {
    NEEDN(3);
    GTEXT_Anim_Keyframes * k = NULL;
    st = gtext_anim_effect_get_keyframes(e, &k);
    if (st != GTEXT_ANIM_OK) {
      return effect_failure("getKeyframes() failed", st, program_error);
    }
    put_kfl(w, o->f[1], k);
    return NULL;
  }
  if (strcmp(name, "eset") == 0) {
    NEEDN(4);
    const char * member = o->f[2];
    const char * v = o->f[3];
    const int is_null = strncmp(v, "N:", 2) == 0;
    const char * text = is_null ? NULL : v + 2;
    if (strcmp(member, "composite") == 0 && text) {
      GTEXT_Anim_Keyframe_Composite kc;
      if (composite_enum(text, 0, &kc)) {
        st = gtext_anim_effect_set_composite(e, (GTEXT_Anim_Composite)(kc - GTEXT_ANIM_KF_COMPOSITE_REPLACE));
        (void)gtext_anim_world_touch(w->anim);
        return st == GTEXT_ANIM_OK ? NULL : effect_failure("composite could not be set", st, program_error);
      }
      return NULL;   /* an attribute set to a string that is not one of its enumeration values is ignored */
    }
    if (strcmp(member, "iterationComposite") == 0 && text) {
      if (strcmp(text, "replace") == 0 || strcmp(text, "accumulate") == 0) {
        st = gtext_anim_effect_set_iteration_composite(e, strcmp(text, "replace") == 0 ? GTEXT_ANIM_ITERATION_REPLACE : GTEXT_ANIM_ITERATION_ACCUMULATE);
        (void)gtext_anim_world_touch(w->anim);
        return st == GTEXT_ANIM_OK ? NULL : effect_failure("iterationComposite could not be set", st, program_error);
      }
      return NULL;
    }
    if (strcmp(member, "pseudoElement") == 0) {
      st = gtext_anim_effect_set_pseudo(e, text, text ? strlen(text) : 0);
      (void)gtext_anim_world_touch(w->anim);
      return st == GTEXT_ANIM_OK ? NULL : effect_failure("pseudoElement could not be set", st, program_error);
    }
    *program_error = 1;
    return fmt("eset does not know the member %s", member);
  }
  if (strcmp(name, "esettarget") == 0) {
    NEEDN(3);
    const void * target = NULL;
    char * failure = NULL;
    if (element_operand(w, o->f[2], &target, &failure) < 0) {
      *program_error = 1;
      return dups("esettarget names an element the program did not make");
    }
    if (failure) {
      return failure;
    }
    sync_world(w);
    st = gtext_anim_effect_set_target(e, target);
    (void)gtext_anim_world_touch(w->anim);
    return st == GTEXT_ANIM_OK ? NULL : effect_failure("target could not be set", st, program_error);
  }
  if (strcmp(name, "esetkf") == 0) {
    NEEDN(3);
    jv kjson;
    if (!jv_parse(o->f[2], &kjson)) {
      *program_error = 1;
      return dups("the keyframes of the op are not JSON");
    }
    kfstore ks;
    char * failure = kf_build(&kjson, &ks);
    if (failure) {
      jv_free(&kjson);
      return failure;
    }
    st = gtext_anim_effect_set_keyframes(e, &ks.arg);
    jv_free(&kjson);
    kfstore_free(&ks);
    (void)gtext_anim_world_touch(w->anim);
    return st == GTEXT_ANIM_OK ? NULL : effect_failure("setKeyframes() failed", st, program_error);
  }
  if (strcmp(name, "eupdate") == 0) {
    NEEDN(3);
    jv tjson;
    if (!jv_parse(o->f[2], &tjson)) {
      *program_error = 1;
      return dups("the timing of the op is not JSON");
    }
    GTEXT_Anim_Optional_Timing in;
    GTEXT_Anim_Text easing = {NULL, 0};
    char * failure = timing_build(jv_get(&tjson, "timing"), &in, &easing);
    if (failure) {
      jv_free(&tjson);
      return failure;
    }
    st = gtext_anim_effect_update_timing(e, &in, &easing, (GTEXT_Anim_Time){true, w->now_ms});
    jv_free(&tjson);
    (void)gtext_anim_world_touch(w->anim);
    return st == GTEXT_ANIM_OK ? NULL : effect_failure("updateTiming() failed", st, program_error);
  }
  *program_error = 1;
  return fmt("the op %s is not one the runner knows", name);
#undef NEEDN
}

static char * anim_op(world * w, const op * o, int * program_error) {
  const char * name = o->f[0];
  *program_error = 0;
  if (is_script_op(name)) {
    return script_op(w, o, program_error);
  }
  {
    int handled = 0;
    char * result = ctl_op(w, o, program_error, &handled);
    if (handled) {
      return result;
    }
  }
  if (strcmp(name, "ga") == 0) {
    if (o->n != 5) {
      *program_error = 1;
      return dups("ga has the wrong number of fields");
    }
    sync_world(w);
    GTEXT_Markup_Node * node = NULL;
    if (strcmp(o->f[2], "doc") != 0) {
      elem * e = find_elem(w, o->f[2]);
      if (!e) {
        *program_error = 1;
        return fmt("ga names the element %s, which the program did not make", o->f[2]);
      }
      if (!e->node || !is_element(e->node)) {
        return dups("TypeError: the element is null");
      }
      node = e->node;
    }
    /* "Element.getAnimations()": the style of the document is brought up to date first, as a style change event for every element, whether or not the page has waited */
    char * recalc_failure = style_recalc(w);
    if (recalc_failure) {
      return recalc_failure;
    }
    if (!w->anim) {
      /* no world to ask: an empty list here would let a test that asserts the absence of animations pass with nothing behind it */
      return dups("getAnimations() was asked in a run with no animation world");
    }
    animlist l;
    memset(&l, 0, sizeof l);
    l.name = dups(o->f[1]);
    {
      GTEXT_Anim_List * list = NULL;
      const GTEXT_Anim_Status st = gtext_anim_world_get_animations(w->anim, node ? gtext_css_markup_handle(node) : NULL, o->f[3], o->l[3], strcmp(o->f[4], "true") == 0, &list);
      if (st != GTEXT_ANIM_OK) {
        free(l.name);
        return fmt("getAnimations() failed (status %d)", (int)st);
      }
      l.n = gtext_anim_list_count(list);
      l.items = calloc(l.n ? l.n : 1, sizeof *l.items);
      for (size_t i = 0; i < l.n; i++) {
        GTEXT_Anim_Info a;
        (void)gtext_anim_list_at(list, i, &a);
        animh * h = &l.items[i];
        h->kind = a.kind;
        h->id = a.id;
        h->pname = dupn(a.name.data ? a.name.data : "", a.name.length);
        h->easing = dupn(a.easing.data ? a.easing.data : "", a.easing.length);
        h->timing = a.timing;
        h->timing.easing = NULL;
        h->idtext = dups("");
      }
      gtext_anim_list_free(list);
    }
    animlist * old = find_alist(w, o->f[1]);
    if (old) {
      for (size_t i = 0; i < old->n; i++) {
        animh_free(&old->items[i]);
      }
      free(old->items);
      free(old->name);
      *old = l;
      return NULL;
    }
    if (w->nalists == w->capalists) {
      w->capalists = w->capalists ? w->capalists * 2 : 8;
      w->alists = realloc(w->alists, w->capalists * sizeof *w->alists);
      if (!w->alists) {
        abort();
      }
    }
    w->alists[w->nalists++] = l;
    return NULL;
  }
  if (strcmp(name, "alen") == 0) {
    if (o->n != 3) {
      *program_error = 1;
      return dups("alen has the wrong number of fields");
    }
    animlist * l = find_alist(w, o->f[2]);
    if (!l) {
      *program_error = 1;
      return fmt("alen names the list %s, which the program did not make", o->f[2]);
    }
    put_reg(w, o->f[1], number_value((double)l->n));
    return NULL;
  }
  if (strcmp(name, "aat") == 0) {
    if (o->n != 4) {
      *program_error = 1;
      return dups("aat has the wrong number of fields");
    }
    animlist * l = find_alist(w, o->f[2]);
    if (!l) {
      *program_error = 1;
      return fmt("aat names the list %s, which the program did not make", o->f[2]);
    }
    const size_t index = (size_t)strtoull(o->f[3], NULL, 10);
    animh h;
    memset(&h, 0, sizeof h);
    if (index < l->n) {
      h = animh_copy(&l->items[index], o->f[1]);
    }
    else {
      h.name = dups(o->f[1]);
      h.missing = 1;
      h.pname = dups("");
      h.easing = dups("");
      h.idtext = dups("");
    }
    if (!h.missing && w->anim) {
      /* the object exists from the moment getAnimations() returned it, and it is the Animation of the model that keeps what the animation does from then on (a transition that completes before the page asks
         it anything is finished, and not a record that is gone) */
      (void)ctl_get(w, &h);
    }
    animh * old = find_ah(w, o->f[1]);
    if (old) {
      animh_free(old);
      *old = h;
      return NULL;
    }
    if (w->nahs == w->capahs) {
      w->capahs = w->capahs ? w->capahs * 2 : 8;
      w->ahs = realloc(w->ahs, w->capahs * sizeof *w->ahs);
      if (!w->ahs) {
        abort();
      }
    }
    w->ahs[w->nahs++] = h;
    return NULL;
  }
  /* aget, aset and tget name an animation; a tget may name an effect (an effect the program holds, or the one of an animation it read) */
  const size_t at = strcmp(name, "aset") == 0 ? 1 : 2;
  if (o->n <= at) {
    *program_error = 1;
    return fmt("%s has too few fields", name);
  }
  if (strcmp(name, "tget") == 0 && find_eh(w, o->f[at])) {
    if (o->n != 5) {
      *program_error = 1;
      return dups("tget has the wrong number of fields");
    }
    GTEXT_Anim_Effect * e = find_eh(w, o->f[at])->e;
    if (!e) {
      return dups("TypeError: Cannot read properties of null");
    }
    char * failure = style_recalc(w);   /* what a page reads of an effect is of the animation after the style change events its changes made */
    if (failure) {
      return failure;
    }
    GTEXT_Anim_Effect_Timing t;
    GTEXT_CSS_String easing = {NULL, 0};
    GTEXT_Anim_Computed_Timing ct;
    const int computed = strcmp(o->f[3], "computed") == 0;
    GTEXT_Anim_Status ts = gtext_anim_effect_get_timing(e, &t, &easing);
    char * easing_text = ts == GTEXT_ANIM_OK ? dupn(easing.data ? easing.data : "linear", easing.data ? easing.length : 6) : NULL;   /* before the next call, which may replace it */
    if (ts == GTEXT_ANIM_OK) {
      ts = gtext_anim_effect_get_computed_timing(e, (GTEXT_Anim_Time){true, w->now_ms}, &ct);
    }
    if (ts != GTEXT_ANIM_OK) {
      free(easing_text);
      return fmt("getTiming() failed (status %d)", (int)ts);
    }
    value v;
    const int known = timing_member(o->f[4], computed, &t, easing_text, &ct, &v);
    free(easing_text);
    if (!known) {
      *program_error = 1;
      return fmt("tget does not know the member %s", o->f[4]);
    }
    put_reg(w, o->f[1], v);
    return NULL;
  }
  animh * h = find_ah(w, o->f[at]);
  if (!h) {
    *program_error = 1;
    return fmt("%s names the animation %s, which the program did not make", name, o->f[at]);
  }
  if (h->missing) {
    return dups("TypeError: Cannot read properties of undefined");
  }
  if (strcmp(name, "aset") == 0) {
    if (o->n != 4 || strcmp(o->f[2], "id") != 0) {
      *program_error = 1;
      return dups("aset sets only the id");
    }
    free(h->idtext);
    h->idtext = dups(o->f[3]);
    return NULL;
  }
  GTEXT_Anim_Info info;
  memset(&info, 0, sizeof info);
  bool found = false;
  if (!w->anim) {
    return dups("an animation was read in a run with no animation world");
  }
  if (strcmp(name, "aget") == 0 && o->n == 4 && strcmp(o->f[3], "className") != 0 && strcmp(o->f[3], "id") != 0 && strcmp(o->f[3], "animationName") != 0 && strcmp(o->f[3], "transitionProperty") != 0) {
    char * recalc_failure = style_recalc(w);   /* as for the other reads of an animation (ctl_op) */
    if (recalc_failure) {
      return recalc_failure;
    }
  }
  if (gtext_anim_world_find(w->anim, h->kind, h->id, &found, &info) != GTEXT_ANIM_OK) {
    return dups("the animation could not be read");
  }
  const char * member;
  if (strcmp(name, "aget") == 0) {
    if (o->n != 4) {
      *program_error = 1;
      return dups("aget has the wrong number of fields");
    }
    member = o->f[3];
    value v;
    if (strcmp(member, "animationName") == 0) {
      v = h->kind == GTEXT_ANIM_KIND_CSS_ANIMATION ? val('s', h->pname, strlen(h->pname)) : val('u', "", 0);
    }
    else if (strcmp(member, "transitionProperty") == 0) {
      v = h->kind == GTEXT_ANIM_KIND_CSS_TRANSITION ? val('s', h->pname, strlen(h->pname)) : val('u', "", 0);
    }
    else if (strcmp(member, "className") == 0) {
      const char * s = h->kind == GTEXT_ANIM_KIND_CSS_TRANSITION ? "CSSTransition" : h->kind == GTEXT_ANIM_KIND_SCRIPT ? "Animation" : "CSSAnimation";
      v = val('s', s, strlen(s));
    }
    else if (ctl_find(w, h->kind, h->id) && (strcmp(member, "playState") == 0 || strcmp(member, "pending") == 0 || strcmp(member, "currentTime") == 0 || strcmp(member, "playbackRate") == 0 ||
                                              strcmp(member, "replaceState") == 0)) {
      /* the Animation of the model holds the animation once the program has touched it: it is the one that says, also when the animation has left the document's (a transition that completed) */
      ctl * c = ctl_find(w, h->kind, h->id);
      GTEXT_Anim_Animation_State as;
      (void)gtext_anim_animation_state(c->a, (GTEXT_Anim_Time){true, w->now_ms}, &as);
      if (strcmp(member, "playState") == 0) {
        static const char * const states[] = {"idle", "running", "paused", "finished"};
        v = val('s', states[as.play_state], strlen(states[as.play_state]));
      }
      else if (strcmp(member, "pending") == 0) {
        v = val('b', as.pending ? "true" : "false", as.pending ? 4 : 5);
      }
      else if (strcmp(member, "currentTime") == 0) {
        v = time_value(as.current_time);
      }
      else if (strcmp(member, "playbackRate") == 0) {
        v = number_value(as.playback_rate);
      }
      else {
        static const char * const replace[] = {"active", "removed", "persisted"};
        v = val('s', replace[as.replace_state], strlen(replace[as.replace_state]));
      }
    }
    else if (strcmp(member, "playbackRate") == 0) {
      v = number_value(1.0);   /* an animation the program has not touched has the rate of 1: the style does not change it */
    }
    else if (strcmp(member, "replaceState") == 0) {
      v = val('s', "active", 6);
    }
    else if (strcmp(member, "playState") == 0) {
      static const char * const states[] = {"idle", "running", "paused", "finished"};
      const char * s = found ? states[info.play_state] : "idle";
      v = val('s', s, strlen(s));
    }
    else if (strcmp(member, "pending") == 0) {
      v = val('b', found && info.pending ? "true" : "false", found && info.pending ? 4 : 5);
    }
    else if (strcmp(member, "currentTime") == 0) {
      v = found ? time_value(info.current_time) : val('N', "", 0);
    }
    else if (strcmp(member, "id") == 0) {
      v = val('s', h->idtext, strlen(h->idtext));
    }
    else {
      *program_error = 1;
      return fmt("aget does not know the member %s", member);
    }
    put_reg(w, o->f[1], v);
    return NULL;
  }
  /* tget R A timing|computed MEMBER */
  if (o->n != 5) {
    *program_error = 1;
    return dups("tget has the wrong number of fields");
  }
  const int computed = strcmp(o->f[3], "computed") == 0;
  member = o->f[4];
  const GTEXT_Anim_Effect_Timing * t = found ? &info.timing : &h->timing;
  const char * easing = found ? NULL : h->easing;
  char * easing_copy = NULL;
  if (found) {
    easing_copy = dupn(info.easing.data ? info.easing.data : "", info.easing.length);
    easing = easing_copy;
  }
  /* the computed timing of an animation that is not found: its timing with no local time */
  GTEXT_Anim_Computed_Timing ct;
  if (found) {
    ct = info.computed;
  }
  else {
    (void)gtext_anim_computed_timing(t, (GTEXT_Anim_Time){false, 0.0}, 1.0, false, &ct);
  }
  value v;
  const int known = timing_member(member, computed, t, easing, &ct, &v);
  free(easing_copy);
  if (!known) {
    *program_error = 1;
    return fmt("tget does not know the member %s", member);
  }
  put_reg(w, o->f[1], v);
  return NULL;
}

static char * run_op(world * w, const op * o, testrun * t, int * program_error) {
  const char * name = o->f[0];
  *program_error = 0;
  int ok;
  {
    static const char * const family[] = {"anew", "enew", "animnew", "ecopy", "asettl", "aeff", "aseteff", "am", "pgen", "pwait", "asetv", "frame", "advance", "eupdate", "esetkf", "esettarget"};
    for (size_t fi_ = 0; fi_ < sizeof family / sizeof family[0]; fi_++) {
      if (strcmp(name, family[fi_]) == 0) {
        w->animates = 1;
        break;
      }
    }
  }
#define NEED(n_) do { if (o->n != (n_)) { *program_error = 1; return fmt("the op %s has %zu fields, not %d", name, o->n, (n_)); } } while (0)
#define ELEM(var, fld) elem * var = find_elem(w, o->f[fld]); if (!var) { *program_error = 1; return fmt("the op %s names the element %s, which the program did not make", name, o->f[fld]); } \
  if (!var->node) { return dups("TypeError: the element is null"); } \
  if (!is_element(var->node)) { return dups("TypeError: the node is not an element"); }
#define STYLE(var, fld) style * var = find_style(w, o->f[fld]); if (!var) { *program_error = 1; return fmt("the op %s names the style %s, which the program did not make", name, o->f[fld]); } \
  if (var->computed && w->tracker) { sync_world(w); if (w->async || w->animates) { char * recalc_failure = style_recalc(w); if (recalc_failure) { return recalc_failure; } } const GTEXT_CSS_Status flushed = gtext_cssom_transition_tracker_flush(w->tracker, gtext_css_markup_handle(var->node), var->pseudo, strlen(var->pseudo)); if (flushed != GTEXT_CSS_OK) { return status_failure("style change event", flushed); } \
    if (!*var->pseudo) { /* a read of a computed style is a style update of the document, and the pseudo-elements of the element are part of it: the transitions of its ::before, ::after and ::marker start at the same event */ \
      static const char * const pe_[] = {"::before", "::after", "::marker"}; \
      for (size_t pi_ = 0; pi_ < 3; pi_++) { const GTEXT_CSS_Status pf_ = gtext_cssom_transition_tracker_flush(w->tracker, gtext_css_markup_handle(var->node), pe_[pi_], strlen(pe_[pi_])); if (pf_ != GTEXT_CSS_OK) { return status_failure("style change event", pf_); } } } }

  {
    char * failure = NULL;
    forgetlist forgotten = {NULL, 0, 0};
    forget_plan(w, o, &forgotten);
    if (dom_op(w, o, program_error, &failure) || sheet_op(w, o, program_error, &failure)) {
      forget_apply(w, &forgotten, failure == NULL && !*program_error);
      return failure;
    }
    forget_apply(w, &forgotten, 0);
  }
  if (strcmp(name, "elem") == 0) {
    if (o->n < 4) {
      *program_error = 1;
      return dups("elem has too few fields");
    }
    const char * how = o->f[2];
    GTEXT_Markup_Node * found = NULL;
    if (strcmp(how, "byid") == 0) {
      found = find_by_id(w, o->f[3], o->l[3]);
    }
    else if (strcmp(how, "body") == 0) {
      found = find_by_local(w, "body");
    }
    else if (strcmp(how, "head") == 0) {
      found = find_by_local(w, "head");
    }
    else if (strcmp(how, "html") == 0) {
      found = root_element(w);
    }
    else if (strcmp(how, "sel") == 0 || strcmp(how, "qsa") == 0) {
      const GTEXT_Markup_Node * scope = gtext_markup_document_root(w->markup);
      if (strcmp(o->f[3], "doc") != 0) {
        elem * r = find_elem(w, o->f[3]);
        if (!r) {
          *program_error = 1;
          return fmt("the op elem names the element %s, which the program did not make", o->f[3]);
        }
        if (!r->node) {
          return dups("TypeError: the element is null");
        }
        scope = r->node;
      }
      if (o->n < 5) {
        *program_error = 1;
        return dups("elem sel has too few fields");
      }
      GTEXT_Markup_Node ** nodes = NULL;
      size_t count = 0;
      GTEXT_Markup_Status ms;
      if (strcmp(how, "sel") == 0) {
        GTEXT_Markup_Node * one = NULL;
        ms = gtext_markup_query_selector(scope, o->f[4], o->l[4], NULL, &one, NULL);
        if (ms != GTEXT_MARKUP_OK && ms != GTEXT_MARKUP_E_SELECTOR) {
          return fmt("querySelector failed: %d", (int)ms);
        }
        if (ms == GTEXT_MARKUP_E_SELECTOR) {
          return dups("SyntaxError: the selector is not valid");
        }
        found = one;
      }
      else {
        ms = gtext_markup_query_selector_all(scope, o->f[4], o->l[4], NULL, &nodes, &count, NULL);
        if (ms == GTEXT_MARKUP_E_SELECTOR) {
          return dups("SyntaxError: the selector is not valid");
        }
        if (ms != GTEXT_MARKUP_OK) {
          return fmt("querySelectorAll failed: %d", (int)ms);
        }
        const size_t idx = o->n > 5 ? (size_t)strtoull(o->f[5], NULL, 10) : 0;
        found = idx < count ? nodes[idx] : NULL;
        gtext_markup_query_free(NULL, nodes);
      }
    }
    else if (strcmp(how, "byidin") == 0) {
      /* elem R byidin ROOT ID: getElementById of a shadow root (a DocumentFragment): the first element of its tree with the id */
      elem * r = o->n > 4 ? find_elem(w, o->f[3]) : NULL;
      if (!r || !r->node) {
        *program_error = 1;
        return dups("elem byidin names a root the program did not make");
      }
      for (GTEXT_Markup_Node * x = gtext_markup_node_next_in_order(r->node, r->node, 0); x && !found; x = gtext_markup_node_next_in_order(r->node, x, 0)) {
        const char * v;
        size_t vl;
        if (o->l[4] > 0 && is_element(x) && attr_get(x, "id", &v, &vl) && vl == o->l[4] && memcmp(v, o->f[4], vl) == 0) {
          found = x;
        }
      }
    }
    else if (strcmp(how, "list") == 0) {
      nodelist * l = o->n > 4 ? find_list(w, o->f[3]) : NULL;
      if (!l) {
        *program_error = 1;
        return fmt("elem list names the list %s, which the program did not make", o->n > 3 ? o->f[3] : "");
      }
      const size_t idx = (size_t)strtoull(o->f[4], NULL, 10);
      found = idx < l->n ? l->nodes[idx] : NULL;
    }
    else if (strcmp(how, "nav") == 0) {
      elem * from = find_elem(w, o->f[3]);
      if (!from) {
        for (size_t k = 0; w->async && k < w->nskipped; k++) {
          if (strcmp(w->skipped[k], o->f[3]) == 0) {
            put_elem(w, o->f[1], NULL);   /* the element was never made (the test failed before): there is none to go from, as for a page */
            return NULL;
          }
        }
        *program_error = 1;
        return fmt("the op elem names the element %s, which the program did not make", o->f[3]);
      }
      if (!from->node) {
        return dups("TypeError: the element is null");
      }
      const char * what = o->n > 4 ? o->f[4] : "";
      GTEXT_Markup_Node * n = from->node;
      if (strcmp(what, "parentElement") == 0) {
        GTEXT_Markup_Node * p = gtext_markup_node_parent(n);
        found = is_element(p) ? p : NULL;
      }
      else if (strcmp(what, "firstElementChild") == 0) {
        for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c && !found; c = gtext_markup_node_next_sibling(c)) {
          found = is_element(c) ? c : NULL;
        }
      }
      else if (strcmp(what, "lastElementChild") == 0) {
        for (GTEXT_Markup_Node * c = gtext_markup_node_last_child(n); c && !found; c = gtext_markup_node_previous_sibling(c)) {
          found = is_element(c) ? c : NULL;
        }
      }
      else if (strcmp(what, "firstChild") == 0) {
        found = gtext_markup_node_first_child(n);
      }
      else if (strcmp(what, "lastChild") == 0) {
        found = gtext_markup_node_last_child(n);
      }
      else if (strcmp(what, "nextSibling") == 0) {
        found = gtext_markup_node_next_sibling(n);
      }
      else if (strcmp(what, "previousSibling") == 0) {
        found = gtext_markup_node_previous_sibling(n);
      }
      else if (strcmp(what, "parentNode") == 0) {
        found = gtext_markup_node_parent(n);
      }
      else if (strcmp(what, "shadowRoot") == 0) {
        const shadow * sh = is_element(n) ? shadow_by_host(w, n) : NULL;
        found = sh && sh->open ? sh->root : NULL;
      }
      else if (strcmp(what, "host") == 0) {
        const shadow * sh = shadow_by_root(w, n);
        if (!sh) {
          *program_error = 1;
          return dups("elem nav host on a node that is not a shadow root");
        }
        found = sh->host;
      }
      else if (strcmp(what, "assignedSlot") == 0) {
        GTEXT_Markup_Node * slot = assigned_slot_of(w, n);
        const shadow * sh = slot ? shadow_by_host(w, gtext_markup_node_parent(n)) : NULL;
        found = sh && sh->open ? slot : NULL;
      }
      else if (strcmp(what, "nextElementSibling") == 0) {
        found = element_sibling(n, 1);
      }
      else if (strcmp(what, "previousElementSibling") == 0) {
        found = element_sibling(n, 0);
      }
      else {
        *program_error = 1;
        return fmt("elem nav %s is not known", what);
      }
    }
    else {
      *program_error = 1;
      return fmt("elem %s is not known", how);
    }
    put_elem(w, o->f[1], found);
    return NULL;
  }
  if (strcmp(name, "gcs") == 0) {
    NEED(4);
    ELEM(e, 2);
    sync_world(w);
    GTEXT_CSSOM_Decl * d = NULL;
    GTEXT_CSS_Status st = gtext_cssom_computed_style(w->doc, gtext_css_markup_handle(e->node), o->f[3], o->l[3], &d);
    if (st != GTEXT_CSS_OK) {
      return status_failure("getComputedStyle", st);
    }
    put_style(w, o->f[1], d, 1, e->node, o->f[2], o->f[3]);
    return NULL;
  }
  if (strcmp(name, "style") == 0) {
    NEED(3);
    ELEM(e, 2);
    GTEXT_CSSOM_Decl * d = NULL;
    GTEXT_CSS_Status st = gtext_cssom_inline_style(w->doc, e->node, &w->attr, &d);
    if (st != GTEXT_CSS_OK) {
      return status_failure("element.style", st);
    }
    put_style(w, o->f[1], d, 0, e->node, o->f[2], "");
    return NULL;
  }
  if (strcmp(name, "sget") == 0 || strcmp(name, "sprio") == 0 || strcmp(name, "sattr") == 0 || strcmp(name, "sremove") == 0) {
    /* `sattr R S NAME flush` is a read whose value the page drops (`getComputedStyle(e).paddingLeft;`, to bring the style up to date): it is a style change event and nothing is asserted on it, so it needs
       no layout whatever the property is */
    const int flush_only = strcmp(name, "sattr") == 0 && o->n == 5 && strcmp(o->f[4], "flush") == 0;
    if (!flush_only) {
      NEED(4);
    }
    STYLE(s, 2);
    GTEXT_CSS_Status st;
    if (strcmp(name, "sget") == 0) {
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_get_property_value(s->decl, o->f[3], o->l[3], &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("getPropertyValue", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
      note_layout(w, t, s, o->f[3], o->f[1]);
    }
    else if (strcmp(name, "sprio") == 0) {
      const char * p = NULL;
      st = gtext_cssom_decl_get_property_priority(s->decl, o->f[3], o->l[3], &p);
      if (st != GTEXT_CSS_OK) {
        return status_failure("getPropertyPriority", st);
      }
      put_reg(w, o->f[1], val('s', p ? p : "", p ? strlen(p) : 0));
    }
    else if (strcmp(name, "sremove") == 0) {
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_remove_property(s->decl, o->f[3], o->l[3], &out, &n, NULL);
      if (st != GTEXT_CSS_OK) {
        return status_failure("removeProperty", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
    }
    else {
      if (!gtext_cssom_decl_has_attribute(s->decl, o->f[3], o->l[3])) {
        put_reg(w, o->f[1], val('u', "", 0));
        return NULL;
      }
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_idl_get(s->decl, o->f[3], o->l[3], &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("style attribute", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
      char * css = idl_to_css(o->f[3], o->l[3]);
      if (!flush_only) {
        note_layout(w, t, s, css, o->f[1]);
      }
      free(css);
    }
    return NULL;
  }
  if (strcmp(name, "slen") == 0 || strcmp(name, "scss") == 0 || strcmp(name, "sitem") == 0) {
    STYLE(s, 2);
    GTEXT_CSS_Status st;
    if (strcmp(name, "slen") == 0) {
      size_t n = 0;
      st = gtext_cssom_decl_length(s->decl, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("length", st);
      }
      char num[32];
      snprintf(num, sizeof num, "%zu", n);
      put_reg(w, o->f[1], val('n', num, strlen(num)));
    }
    else if (strcmp(name, "scss") == 0) {
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_css_text(s->decl, &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("cssText", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
    }
    else {
      size_t len = 0;
      st = gtext_cssom_decl_length(s->decl, &len);
      const size_t index = (size_t)strtoull(o->f[3], NULL, 10);
      if (st != GTEXT_CSS_OK) {
        return status_failure("length", st);
      }
      if (index >= len) {
        put_reg(w, o->f[1], val('s', "", 0));
        return NULL;
      }
      char * out = NULL;
      size_t n = 0;
      st = gtext_cssom_decl_item(s->decl, index, &out, &n);
      if (st != GTEXT_CSS_OK) {
        return status_failure("item", st);
      }
      put_reg(w, o->f[1], take_string(w, out, n));
    }
    return NULL;
  }
  if (strcmp(name, "sset") == 0) {
    NEED(5);
    STYLE(s, 1);
    GTEXT_CSS_Status st = gtext_cssom_decl_set_property(s->decl, o->f[2], o->l[2], o->f[3], o->l[3], o->f[4], o->l[4], NULL);
    return st == GTEXT_CSS_OK ? NULL : status_failure("setProperty", st);
  }
  if (strcmp(name, "sattrset") == 0) {
    NEED(4);
    STYLE(s, 1);
    if (!gtext_cssom_decl_has_attribute(s->decl, o->f[2], o->l[2])) {
      return NULL;   /* an expando of the script object: nothing happens to the declaration */
    }
    GTEXT_CSS_Status st = gtext_cssom_decl_idl_set(s->decl, o->f[2], o->l[2], o->f[3], o->l[3], NULL);
    return st == GTEXT_CSS_OK ? NULL : status_failure("style attribute", st);
  }
  if (strcmp(name, "scsstext") == 0) {
    NEED(3);
    STYLE(s, 1);
    GTEXT_CSS_Status st = gtext_cssom_decl_set_css_text(s->decl, o->f[2], o->l[2], NULL);
    return st == GTEXT_CSS_OK ? NULL : status_failure("cssText", st);
  }
  if (strcmp(name, "setattr") == 0) {
    NEED(4);
    ELEM(e, 1);
    char * lower = dupn(o->f[2], o->l[2]);
    for (char * p = lower; *p; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    GTEXT_Markup_Status ms = gtext_markup_element_set_attribute(e->node, NULL, NULL, lower, o->f[3], o->l[3]);
    free(lower);
    sync_world(w);
    return ms == GTEXT_MARKUP_OK ? NULL : fmt("setAttribute failed: %d", (int)ms);
  }
  if (strcmp(name, "rmattr") == 0) {
    NEED(3);
    ELEM(e, 1);
    char * lower = dupn(o->f[2], o->l[2]);
    for (char * p = lower; *p; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    gtext_markup_element_remove_attribute(e->node, NULL, lower);
    free(lower);
    sync_world(w);
    return NULL;
  }
  if (strcmp(name, "getattr") == 0 || strcmp(name, "hasattr") == 0) {
    NEED(4);
    ELEM(e, 2);
    char * lower = dupn(o->f[3], o->l[3]);
    for (char * p = lower; *p; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    const char * v;
    size_t vl;
    const int has = attr_get(e->node, lower, &v, &vl);
    free(lower);
    if (strcmp(name, "hasattr") == 0) {
      put_reg(w, o->f[1], val('b', has ? "true" : "false", has ? 4 : 5));
    }
    else {
      put_reg(w, o->f[1], has ? val('s', v, vl) : val('N', "", 0));
    }
    return NULL;
  }
  if (strncmp(name, "class", 5) == 0) {
    const int add = strcmp(name, "classadd") == 0, rem = strcmp(name, "classremove") == 0, tog = strcmp(name, "classtoggle") == 0, has = strcmp(name, "classhas") == 0;
    if (!(add || rem || tog || has)) {
      *program_error = 1;
      return fmt("the op %s is not known", name);
    }
    const size_t first = (tog || has) ? 3 : 2;
    ELEM(e, (tog || has) ? 2 : 1);
    const char * cur = "";
    size_t cl = 0;
    attr_get(e->node, "class", &cur, &cl);
    char * list = dupn(cur, cl);
    /* the tokens of the class attribute */
    char * tokens[256];
    size_t nt = 0;
    for (char * p = strtok(list, " \t\n\f\r"); p && nt < 256; p = strtok(NULL, " \t\n\f\r")) {
      tokens[nt++] = p;
    }
    int result = 0, changed = 0;
    for (size_t k = first; k < o->n; k++) {
      size_t at = nt;
      for (size_t i = 0; i < nt; i++) {
        if (strcmp(tokens[i], o->f[k]) == 0) {
          at = i;
          break;
        }
      }
      if (has) {
        result = at < nt;
      }
      else if (add || (tog && at == nt)) {
        if (at == nt && nt < 256) {
          tokens[nt++] = o->f[k];
          changed = 1;
        }
        result = 1;
      }
      else if (rem || (tog && at < nt)) {
        if (at < nt) {
          for (size_t i = at; i + 1 < nt; i++) {
            tokens[i] = tokens[i + 1];
          }
          nt--;
          changed = 1;
        }
        result = 0;
      }
    }
    if (changed) {
      size_t total = 1;
      for (size_t i = 0; i < nt; i++) {
        total += strlen(tokens[i]) + 1;
      }
      char * joined = malloc(total);
      size_t jo = 0;
      for (size_t i = 0; i < nt; i++) {
        if (i) {
          joined[jo++] = ' ';
        }
        jo += (size_t)sprintf(joined + jo, "%s", tokens[i]);
      }
      joined[jo] = '\0';
      gtext_markup_element_set_attribute(e->node, NULL, NULL, "class", joined, jo);
      free(joined);
      sync_world(w);
    }
    free(list);
    if (tog || has) {
      put_reg(w, o->f[1], val('b', result ? "true" : "false", result ? 4 : 5));
    }
    return NULL;
  }
  if (strcmp(name, "shas") == 0) {
    NEED(4);
    STYLE(s, 2);
    const bool has = gtext_cssom_decl_has_attribute(s->decl, o->f[3], o->l[3]);
    put_reg(w, o->f[1], val('b', has ? "true" : "false", has ? 4 : 5));
    return NULL;
  }
  if (strcmp(name, "script") == 0) {
    return NULL;   /* where a script ends in the page: the replay of the oracle puts the ops of a script back where the script was */
  }
  if (strcmp(name, "csssupports") == 0) {
    /* CSS.supports(conditionText) with one argument, CSS.supports(property, value) with two (the extra arguments of a call are ignored by the binding) */
    if (o->n != 3 && o->n != 4) {
      *program_error = 1;
      return dups("csssupports has the wrong number of fields");
    }
    bool result = false;
    GTEXT_CSS_Status st;
    if (o->n == 3) {
      st = gtext_css_conditions_supports_text(w->conditions, o->f[2], o->l[2], &result);
    }
    else {
      st = gtext_css_conditions_supports(w->conditions, o->f[2], o->f[3], &result);
    }
    if (st != GTEXT_CSS_OK) {
      return status_failure("CSS.supports", st);
    }
    put_reg(w, o->f[1], val('b', result ? "true" : "false", result ? 4 : 5));
    return NULL;
  }
  if (strcmp(name, "cssescape") == 0) {
    NEED(3);
    char * out = NULL;
    size_t n = 0;
    GTEXT_CSS_Status st = gtext_cssom_escape_buffer(o->f[2], o->l[2], GTEXT_CSSOM_TEXT_WTF8, NULL, &out, &n);
    if (st != GTEXT_CSS_OK) {
      return status_failure("CSS.escape", st);
    }
    put_reg(w, o->f[1], take_string(w, out, n));
    return NULL;
  }
  if (strcmp(name, "cmp") == 0) {
    NEED(5);
    value a = operand(w, o->f[3], o->l[3], &ok), b;
    if (!ok) {
      *program_error = 1;
      return dups("cmp: an operand the program did not make");
    }
    b = operand(w, o->f[4], o->l[4], &ok);
    if (!ok) {
      value_free(&a);
      *program_error = 1;
      return dups("cmp: an operand the program did not make");
    }
    int eq = same(&a, &b);
    if ((o->f[2][0] == 'l') && !eq) {
      /* loose equality: a number and a string compare as numbers; null and undefined are equal */
      if ((a.kind == 'N' || a.kind == 'u') && (b.kind == 'N' || b.kind == 'u')) {
        eq = 1;
      }
      else if ((a.kind == 'n' && b.kind == 's') || (a.kind == 's' && b.kind == 'n')) {
        eq = strtod(a.s, NULL) == strtod(b.s, NULL);
      }
    }
    const int negate = strcmp(o->f[2], "ne") == 0 || strcmp(o->f[2], "lne") == 0;
    const int r = negate ? !eq : eq;
    put_reg(w, o->f[1], val('b', r ? "true" : "false", r ? 4 : 5));
    inherit_taint(w, o->f[1], o->f[3], o->f[4]);
    value_free(&a);
    value_free(&b);
    return NULL;
  }
  if (strcmp(name, "not") == 0) {
    NEED(3);
    value a = operand(w, o->f[2], o->l[2], &ok);
    if (!ok) {
      *program_error = 1;
      return dups("not: an operand the program did not make");
    }
    const int truthy = a.kind == 'b' ? strcmp(a.s, "true") == 0 : a.kind == 's' ? a.n > 0 : a.kind == 'n' ? strtod(a.s, NULL) != 0 : 0;
    put_reg(w, o->f[1], val('b', truthy ? "false" : "true", truthy ? 5 : 4));
    inherit_taint(w, o->f[1], o->f[2], NULL);
    value_free(&a);
    return NULL;
  }
  if (strcmp(name, "fail") == 0) {
    return dups(o->n > 1 ? o->f[1] : "failed");
  }
  if (is_events_op(name)) {
    return events_op(w, o, program_error);
  }
  if (is_anim_op(name)) {
    return anim_op(w, o, program_error);
  }
  if (strncmp(name, "assert_", 7) == 0) {
    value vals[MAX_FIELDS];
    size_t nv = 0;
    const int has_msg = 1;
    for (size_t i = 1; i + (has_msg ? 1 : 0) < o->n; i++) {
      vals[nv] = operand(w, o->f[i], o->l[i], &ok);
      if (!ok) {
        for (size_t k = 0; k < nv; k++) {
          value_free(&vals[k]);
        }
        *program_error = 1;
        return fmt("%s: an operand the program did not make", name);
      }
      nv++;
    }
    for (size_t i = 1; i + 1 < o->n; i++) {
      note_taint(w, t, o->f[i]);
    }
    const char * msg = o->n > 1 ? o->f[o->n - 1] : "";
    char * result = NULL;
    if (strcmp(name, "assert_equals") == 0 && nv == 2) {
      if (!same_as(&vals[0], &vals[1], 1)) {
        result = mismatch(name, msg, &vals[1], &vals[0]);
      }
    }
    else if (strcmp(name, "assert_not_equals") == 0 && nv == 2) {
      if (same_as(&vals[0], &vals[1], 1)) {
        char g[400];
        fmt_value(&vals[0], g, sizeof g);
        result = fmt("assert_not_equals: %sgot disallowed value %s", msg, g);
      }
    }
    else if ((strcmp(name, "assert_true") == 0 || strcmp(name, "assert_false") == 0) && nv == 1) {
      const int want = strcmp(name, "assert_true") == 0;
      if (!(vals[0].kind == 'b' && (strcmp(vals[0].s, "true") == 0) == want)) {
        char g[400];
        fmt_value(&vals[0], g, sizeof g);
        result = fmt("%s: %sexpected %s got %s", name, msg, want ? "true" : "false", g);
      }
    }
    else if (strcmp(name, "assert_in_array") == 0 && nv >= 2) {
      int found = 0;
      for (size_t k = 1; k < nv; k++) {
        found |= same(&vals[0], &vals[k]);
      }
      if (!found) {
        char g[400];
        fmt_value(&vals[0], g, sizeof g);
        result = fmt("assert_in_array: %svalue %s not in array", msg, g);
      }
    }
    else if (strcmp(name, "assert_approx_equals") == 0 && nv == 3) {
      if (vals[0].kind != 'n' || vals[1].kind != 'n' || vals[2].kind != 'n' || fabs(strtod(vals[0].s, NULL) - strtod(vals[1].s, NULL)) > strtod(vals[2].s, NULL)) {
        result = fmt("assert_approx_equals: %s%s differs from %s by more than %s", msg, vals[0].s, vals[1].s, vals[2].s);
      }
    }
    else if (nv == 2 && (strcmp(name, "assert_greater_than") == 0 || strcmp(name, "assert_less_than") == 0 || strcmp(name, "assert_greater_than_equal") == 0 ||
                         strcmp(name, "assert_less_than_equal") == 0)) {
      int good = 0;
      if (vals[0].kind == 'n' && vals[1].kind == 'n') {
        const double x = strtod(vals[0].s, NULL), y = strtod(vals[1].s, NULL);
        good = strcmp(name, "assert_greater_than") == 0 ? x > y : strcmp(name, "assert_less_than") == 0 ? x < y : strcmp(name, "assert_greater_than_equal") == 0 ? x >= y : x <= y;
      }
      if (!good) {
        result = fmt("%s: %sfailed for %s, %s", name, msg, vals[0].s, vals[1].s);
      }
    }
    else {
      *program_error = 1;
      result = fmt("the op %s has %zu operands", name, nv);
    }
    for (size_t k = 0; k < nv; k++) {
      value_free(&vals[k]);
    }
    return result;
  }
  *program_error = 1;
  return fmt("the op %s is not one the runner knows", name);
#undef NEED
#undef ELEM
#undef STYLE
}

/* ---- the programs -------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * file;
  char * html;       /* the page's markup, all of it */
  size_t html_len;
  world w;
  int open;
  int in_test;
  int in_cleanup;
  int index;
  testrun t;
  int errors;
  /* `iftrue X` / `iffalse X` ... `else` ... `endif` (X an operand): the ops between are run when the value is true in JavaScript's sense (for a node, when there is one), and skipped otherwise; the blocks nest */
  int gdepth;
  char gparent[32], gcond[32], gact[32];
  /* `throws KIND NAME` ... `endthrows`: the ops between are the function of assert_throws_dom() or assert_throws_js(); the first failure among them is the exception, and the rest is skipped */
  int exp_active, exp_ignore;
  char * exp_kind, * exp_name, * exp_msg;
} program;

/* ---- the document as a script sees it -------------------------------------------------------------------------------------------------- */

/* A script runs when the parser reaches its end tag, so it sees the markup up to there (`doc N` gives N, in bytes). The document is parsed again from that prefix; the elements, the
 * declarations and the registers the program holds are carried over, an element by its place in the tree (the index of each node among its siblings from the root), a declaration by
 * the element and the pseudo-element it was made for. The evaluator emits `doc` only while the program has changed nothing, so the new tree is the old one grown. */
static size_t * node_path(const GTEXT_Markup_Node * n, size_t * depth) {
  size_t cap = 16, d = 0;
  size_t * path = malloc(cap * sizeof *path);
  for (const GTEXT_Markup_Node * c = n; c && gtext_markup_node_parent(c); c = gtext_markup_node_parent(c)) {
    size_t index = 0;
    for (GTEXT_Markup_Node * sib = gtext_markup_node_previous_sibling(c); sib; sib = gtext_markup_node_previous_sibling(sib)) {
      index++;
    }
    if (d == cap) {
      cap *= 2;
      path = realloc(path, cap * sizeof *path);
    }
    path[d++] = index;
  }
  *depth = d;
  return path;
}

static GTEXT_Markup_Node * node_from(GTEXT_Markup_Node * start, const size_t * path, size_t depth) {
  GTEXT_Markup_Node * n = start;
  while (depth-- > 0 && n) {
    n = gtext_markup_node_first_child(n);
    for (size_t i = path[depth]; n && i > 0; i--) {
      n = gtext_markup_node_next_sibling(n);
    }
  }
  return n;
}

static GTEXT_Markup_Node * node_at(GTEXT_Markup_Document * m, const size_t * path, size_t depth) {
  return node_from(gtext_markup_document_root(m), path, depth);
}

/* The node of the new world that stands where `n` stood in the old one: by its path from the document's root, or, for a node of a shadow tree, by the path of its host and then its path in the
   root (the shadow roots of a world that has not been mutated are the declarative ones, which the new world has made again). *fail is set for a node that is in neither. */
static GTEXT_Markup_Node * carry_node(world * ow, world * nw, GTEXT_Markup_Node * n, int * fail) {
  GTEXT_Markup_Node * top = tree_top(n);
  size_t depth;
  size_t * path = node_path(n, &depth);
  GTEXT_Markup_Node * out = NULL;
  if (top == gtext_markup_document_root(ow->markup)) {
    out = node_at(nw->markup, path, depth);
  }
  else {
    const shadow * sh = shadow_by_root(ow, top);
    if (!sh || tree_top(sh->host) != gtext_markup_document_root(ow->markup)) {
      *fail = 1;   /* a detached node, or a shadow tree inside another: not found again by a path */
    }
    else {
      size_t hdepth;
      size_t * hpath = node_path(sh->host, &hdepth);
      GTEXT_Markup_Node * host = node_at(nw->markup, hpath, hdepth);
      free(hpath);
      const shadow * nsh = host ? shadow_by_host(nw, host) : NULL;
      out = nsh ? node_from(nsh->root, path, depth) : NULL;
    }
  }
  free(path);
  return out;
}

static int program_open(program * p, size_t extent) {
  if (extent > p->html_len) {
    extent = p->html_len;
  }
  /* The world is opened where it stays: the callbacks of its compute context and of its conditions hold its address (`cenv.user`, `container.user`) and so do its match options, so a
     world opened in a local and copied into the program would leave them pointing at a frame that has returned. The old world is moved aside first; its data is read, never its
     callbacks. */
  world old = p->w;
  const int had = p->open;
  world * nw = &p->w;
  if (!world_open(nw, p->html, extent)) {
    world_close(nw);
    p->w = old;
    return 0;
  }
  if (had) {
    world * ow = &old;
    int carry_failed = 0;
    for (size_t i = 0; i < ow->nelems; i++) {
      put_elem(nw, ow->elems[i].name, ow->elems[i].node ? carry_node(ow, nw, ow->elems[i].node, &carry_failed) : NULL);
    }
    for (size_t i = 0; i < ow->nlists; i++) {
      GTEXT_Markup_Node ** nodes = malloc((ow->lists[i].n ? ow->lists[i].n : 1) * sizeof *nodes);
      for (size_t k = 0; k < ow->lists[i].n; k++) {
        nodes[k] = carry_node(ow, nw, ow->lists[i].nodes[k], &carry_failed);
      }
      put_list(nw, ow->lists[i].name, nodes, ow->lists[i].n);
    }
    if (carry_failed) {
      /* the program has to say so, not be given another node */
      world_close(nw);
      p->w = old;
      return 0;
    }
    for (size_t i = 0; i < ow->nregs; i++) {
      put_reg(nw, ow->regs[i].name, val(ow->regs[i].v.kind, ow->regs[i].v.s, ow->regs[i].v.n));
      nw->regs[nw->nregs - 1].layout = ow->regs[i].layout ? dups(ow->regs[i].layout) : NULL;
      nw->regs[nw->nregs - 1].timeline = ow->regs[i].timeline ? dups(ow->regs[i].timeline) : NULL;
    }
    g_doc = nw->doc;
    for (size_t i = 0; i < ow->nstyles; i++) {
      style * s = &ow->styles[i];
      elem * e = find_elem(nw, s->elem_name);
      GTEXT_CSSOM_Decl * d = NULL;
      if (e && e->node) {
        if (s->computed) {
          gtext_cssom_computed_style(nw->doc, gtext_css_markup_handle(e->node), s->pseudo, strlen(s->pseudo), &d);
        }
        else {
          gtext_cssom_inline_style(nw->doc, e->node, &nw->attr, &d);
        }
      }
      put_style(nw, s->name, d, s->computed, e ? e->node : NULL, s->elem_name, s->pseudo);
    }
    world_close(ow);
  }
  p->open = 1;
  g_doc = p->w.doc;
  return 1;
}

static void print_escaped(const char * s) {
  for (; *s; s++) {
    if (*s == '\n') {
      fputs("\\n", stdout);
    }
    else if (*s == '\t') {
      fputs("\\t", stdout);
    }
    else if (*s == '\r') {
      fputs("\\r", stdout);
    }
    else {
      fputc(*s, stdout);
    }
  }
}

static void end_test(program * p) {
  if (p->in_test) {
    if (p->t.layout) {
      printf("R\t%s\t%d\tLAYOUT\t", p->file, p->index);
      print_escaped(p->t.layout_property ? p->t.layout_property : "");
      fputc('\n', stdout);
    }
    else if (p->t.timeline) {
      printf("R\t%s\t%d\tTIMELINE\t", p->file, p->index);
      print_escaped(p->t.timeline_property ? p->t.timeline_property : "");
      fputc('\n', stdout);
    }
    else if (p->t.failed) {
      printf("R\t%s\t%d\tFAIL\t", p->file, p->index);
      print_escaped(p->t.message ? p->t.message : "");
      fputc('\n', stdout);
    }
    else {
      printf("R\t%s\t%d\tPASS\n", p->file, p->index);
    }
  }
  free(p->t.message);
  free(p->t.layout_property);
  free(p->t.timeline_property);
  memset(&p->t, 0, sizeof p->t);
  p->in_test = 0;
  p->in_cleanup = 0;
}

static void end_program(program * p) {
  end_test(p);
  p->gdepth = 0;
  p->exp_active = 0;
  free(p->exp_kind);
  free(p->exp_name);
  free(p->exp_msg);
  p->exp_kind = p->exp_name = p->exp_msg = NULL;
  if (p->open) {
    world_close(&p->w);
    p->open = 0;
  }
  free(p->file);
  free(p->html);
  p->file = NULL;
  p->html = NULL;
}

int main(void) {
  const char * tr = getenv("CSSOM_RUNNER_TRANSITIONS");
  if (tr && strcmp(tr, "none") == 0) {
    g_transitions = 0;
  }
  const char * tl = getenv("CSSOM_RUNNER_TIMELINE");
  if (tl && strcmp(tl, "none") == 0) {
    g_timeline = 0;
  }
  const char * taint = getenv("CSSOM_RUNNER_TAINT");
  if (taint && strcmp(taint, "none") == 0) {
    g_taint = 0;
  }
  const char * ua = getenv("CSSOM_RUNNER_UA");
  if (ua && strcmp(ua, "none") == 0) {
    g_ua_len = 0;
    g_palette = 0;
  }
  else if (ua && strcmp(ua, "full") == 0) {
    g_ua_sheet = UA_SHEET_FULL;
    g_ua_len = sizeof UA_SHEET_FULL - 1;
  }
  program p;
  memset(&p, 0, sizeof p);
  char * line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    if (len && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (line[0] == 'P' && line[1] == '\t') {
      end_program(&p);
      op o;
      if (!parse_op(line + 2, &o) || o.n != 3) {
        printf("E\t?\tbad program line\n");
        continue;
      }
      p.file = dups(o.f[0]);
    }
    else if (line[0] == 'H' && line[1] == '\t' && p.file) {
      op o;
      if (!parse_op(line + 2, &o) || o.n != 1) {
        printf("E\t%s\tbad document line\n", p.file);
        continue;
      }
      /* the document is parsed when the first op needs it: a program that says `doc N` first never builds the whole page */
      p.html = dupn(o.f[0], o.l[0]);
      p.html_len = o.l[0];
    }
    else if (line[0] == 'O' && line[1] == '\t' && p.file) {
      op o;
      if (!parse_op(line + 2, &o)) {
        printf("E\t%s\ttoo many fields\n", p.file);
        continue;
      }
      if (strcmp(o.f[0], "test") == 0 && o.n >= 2) {
        end_test(&p);
        p.in_test = 1;
        p.index = atoi(o.f[1]);
        if (p.open) {
          events_test_begin(&p.w);
        }
        continue;
      }
      if (strcmp(o.f[0], "cleanup") == 0 && o.n == 1) {
        p.in_cleanup = 1;
        continue;
      }
      if (strcmp(o.f[0], "end") == 0 && o.n == 1) {
        end_test(&p);
        continue;
      }
      if (strcmp(o.f[0], "doc") == 0 && o.n == 2 && p.html) {
        if (!program_open(&p, (size_t)strtoull(o.f[1], NULL, 10))) {
          printf("E\t%s\tcould not open the document\n", p.file);
        }
        continue;
      }
      if (!p.open && p.html && !program_open(&p, p.html_len)) {
        printf("E\t%s\tcould not open the document\n", p.file);
      }
      if (strcmp(o.f[0], "iftrue") == 0 || strcmp(o.f[0], "iffalse") == 0 || strcmp(o.f[0], "else") == 0 || strcmp(o.f[0], "endif") == 0) {
        if (o.f[0][0] == 'e' && o.f[0][1] == 'n') {
          if (p.gdepth > 0) {
            p.gdepth--;
          }
        }
        else if (o.f[0][0] == 'e') {
          if (p.gdepth > 0) {
            p.gact[p.gdepth - 1] = p.gparent[p.gdepth - 1] && !p.gcond[p.gdepth - 1];
          }
        }
        else if (p.gdepth < 32 && o.n == 2) {
          const int parent = p.gdepth == 0 ? 1 : p.gact[p.gdepth - 1];
          int cond = 0;
          if (parent) {
            int okay = 0;
            value v = operand(&p.w, o.f[1], o.l[1], &okay);
            /* JavaScript's truthiness of the value: a node is true when there is one */
            const int truth = !okay ? 0 : v.kind == 'b' ? strcmp(v.s, "true") == 0 : v.kind == 's' ? v.n > 0 : v.kind == 'n' ? (strtod(v.s, NULL) != 0 && strtod(v.s, NULL) == strtod(v.s, NULL)) : v.kind == 'e';
            value_free(&v);
            cond = o.f[0][2] == 't' ? truth : !truth;
          }
          p.gparent[p.gdepth] = (char)parent;
          p.gcond[p.gdepth] = (char)cond;
          p.gact[p.gdepth] = (char)(parent && cond);
          p.gdepth++;
        }
        continue;
      }
      if (p.gdepth > 0 && !p.gact[p.gdepth - 1]) {
        continue;
      }
      if ((strcmp(o.f[0], "throws") == 0 && o.n == 3) || (strcmp(o.f[0], "try") == 0 && o.n == 1)) {
        p.exp_active = 1;
        p.exp_ignore = !p.open || (p.in_test && p.t.failed && !p.in_cleanup);
        free(p.exp_kind);
        free(p.exp_name);
        free(p.exp_msg);
        p.exp_kind = dups(o.n == 3 ? o.f[1] : "any");
        p.exp_name = dups(o.n == 3 ? o.f[2] : "");
        p.exp_msg = NULL;
        continue;
      }
      if (strcmp(o.f[0], "endthrows") == 0 || strcmp(o.f[0], "endtry") == 0) {
        if (p.exp_active && !p.exp_ignore && strcmp(p.exp_kind, "any") != 0) {
          char * result = NULL;
          const size_t nl = strlen(p.exp_name);
          if (!p.exp_msg) {
            result = fmt("assert_throws_%s: function did not throw", p.exp_kind);
          }
          else if (strncmp(p.exp_msg, p.exp_name, nl) != 0 || p.exp_msg[nl] != ':') {
            result = fmt("assert_throws_%s: function threw \"%.80s\", expected %s", p.exp_kind, p.exp_msg, p.exp_name);
          }
          if (result && p.in_test && !p.t.failed) {
            p.t.failed = 1;
            p.t.message = result;
          }
          else {
            free(result);
          }
        }
        p.exp_active = 0;
        free(p.exp_msg);
        p.exp_msg = NULL;
        continue;
      }
      if (p.exp_active && (p.exp_ignore || p.exp_msg)) {
        continue;
      }
      if (!p.open || (p.in_test && p.t.failed && !p.in_cleanup)) {
        if (p.open && p.w.async && o.n > 1 && (strcmp(o.f[0], "create") == 0 || strcmp(o.f[0], "cfrag") == 0 || strcmp(o.f[0], "clone") == 0 || strcmp(o.f[0], "elem") == 0)) {
          if (p.w.nskipped == p.w.capskipped) {
            p.w.capskipped = p.w.capskipped ? p.w.capskipped * 2 : 8;
            p.w.skipped = realloc(p.w.skipped, p.w.capskipped * sizeof *p.w.skipped);
            if (!p.w.skipped) {
              abort();
            }
          }
          p.w.skipped[p.w.nskipped++] = dups(o.f[1]);
        }
        if (p.open && (strcmp(o.f[0], "elem") == 0 || strcmp(o.f[0], "gcs") == 0 || strcmp(o.f[0], "sget") == 0 || strcmp(o.f[0], "sattr") == 0)) {
          /* the exception of a failed assertion ended the test body; the reads after it are still looked at, for the one thing a read tells whatever the assertion did: that it needs a layout */
          int ignored = 0;
          free(run_op(&p.w, &o, &p.t, &ignored));
        }
        continue;
      }
      int program_error = 0;
      char * msg = run_op(&p.w, &o, &p.t, &program_error);
      if (msg && !p.t.layout) {
        /* a read that failed for want of a font metric, a container's size, scroll state or anchored fallback is a read that needs a layout, whatever the page asserts */
        const char * at = strstr(msg, "unsupported, needs ");
        if (at) {
          static const char * const layout_callbacks[] = {"cq", "ch", "ex", "ic", "cap", "lh", "rlh", "rch", "rex", "ric", "rcap", "container.size", "container.scroll_state", "container.anchored_fallback"};
          at += strlen("unsupported, needs ");
          for (size_t k = 0; k < sizeof layout_callbacks / sizeof *layout_callbacks; k++) {
            const size_t n = strlen(layout_callbacks[k]);
            if (strncmp(at, layout_callbacks[k], n) == 0 && (at[n] == '\0' || k == 0)) {
              p.t.layout = 1;
              p.t.layout_property = fmt("needs the %s of a layout", at);
            }
          }
        }
      }
      if (msg) {
        if (program_error) {
          printf("E\t%s\t", p.file);
          print_escaped(msg);
          fputc('\n', stdout);
          free(msg);
        }
        else if (p.exp_active) {
          p.exp_msg = msg;
        }
        else if (p.in_test) {
          if (!p.t.failed) {
            p.t.failed = 1;
            p.t.message = msg;
          }
          else {
            free(msg);
          }
        }
        else {
          printf("E\t%s\t", p.file);   /* a statement of the page's own script outside any test threw */
          print_escaped(msg);
          fputc('\n', stdout);
          free(msg);
        }
      }
    }
  }
  end_program(&p);
  free(line);
  return 0;
}
