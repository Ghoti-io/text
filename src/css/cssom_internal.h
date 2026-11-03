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
 * What the CSSOM sources share (css_cssom_core.h): the document core, the base every object starts with and its two counts, the internal emit seam and the
 * reentrancy flag, and the declaration object. Private.
 *
 * **The base.** Every CSSOM object begins with a @ref css_obj, so a pointer to any of them is a pointer to its base (the public calls that take any object
 * take a `void *`). The object has an internal count (owning edges) and an external count (the callers'); an external retain of a child adds one external
 * hold on its parent, up the chain; the object is freed, with its finalizer run once, when both are zero. Freeing never recurses: a dying object's children
 * are queued on the core and freed by the loop that is already running.
 *
 * **Mutating.** Every mutator begins with css_cssom_enter() (the reentrancy and generation checks), does the work that can fail, and only then commits:
 * css_cssom_commit() bumps the generation and delivers the one event. A mutator that changes nothing commits nothing.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_cssom.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct css_core css_core;
typedef struct css_obj css_obj;
typedef struct css_cssom_event css_cssom_event;
struct css_text_cache;
struct css_cmps;
struct css_comp;
struct css_qevent;
struct GTEXT_CSSOM_Scope;
struct GTEXT_CSSOM_Media_Query_List;

/// A compiled sheet the caller registered for the user-agent or user origin.
typedef struct {
  GTEXT_CSS_Sheet * sheet;             ///< Retained.
  GTEXT_CSS_Origin origin;
} css_registered_sheet;

/// The base of every object.
struct css_obj {
  uint8_t kind;                        ///< GTEXT_CSSOM_Object_Kind.
  bool finalized;
  bool queued;                         ///< On the queue of objects being freed.
  bool uncounted;                      ///< The document's own scope: it takes no id and is no live object of the counters, so that a document with nothing in it has no objects.
  uint64_t internal;                   ///< Owning edges.
  uint64_t external;                   ///< The callers' references and the holds of held children.
  css_obj * parent;                    ///< Weak: the object a held child keeps alive; NULL when detached or when there is none.
  css_core * core;
  void * user;
  GTEXT_CSSOM_Finalizer finalizer;
  uint64_t id;
  css_obj * next_dead;                 ///< Intrusive link of the queue of objects being freed.
  void (*destroy)(css_obj * object);   ///< Releases what the object holds (its children's internal counts) and frees it; the base is freed by the caller after.
  size_t bytes;                        ///< What the object accounts against `max_live_bytes`.
};

/// An element-owned declaration's entry in the weak map.
typedef struct {
  const void * element;
  struct GTEXT_CSSOM_Decl * decl;
} css_inline_entry;

/// The test tap and, from C5c3, the observers: the one function every event goes through.
typedef GTEXT_CSS_Status (*css_cssom_tap_fn)(void * user, const css_cssom_event * event);

/// A registered observer (cssom_events.c).
typedef struct {
  uint64_t handle;
  GTEXT_CSSOM_Observer_Fn fn;
  void * user;
  uint64_t since;                      ///< The generation when it was registered: it is called for the events after that.
  bool dead;                           ///< Removed during a delivery; taken out when the outermost delivery ends.
} css_observer;

/// The document core: reference counted by the objects and the document handle, not atomic.
struct css_core {
  size_t refs;
  GTEXT_Allocator alloc;
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSSOM_Config config;           ///< The limits, with the defaults filled in.
  uint64_t generation;
  uint64_t next_id;
  uint64_t stamp_counter;              ///< The stamps of sheets' changes (css_sheet_bump()): monotonic, never reused, independent of the generation.
  uint64_t tree_epoch;                 ///< gtext_cssom_document_tree_changed() advances it.
  struct GTEXT_CSSOM_Document * document;   ///< NULL once the handle was released.
  unsigned in_callback;                ///< Inside a notification: nesting depth.
  unsigned in_finalizer;               ///< Inside a finalizer: nesting depth.
  css_obj * dead_head;                 ///< The queue of objects being freed.
  bool freeing;                        ///< The loop that frees the queue is running.
  GTEXT_CSSOM_Counters counters;
  css_inline_entry * inline_map;       ///< Open addressing over the element handle; an entry with no element is empty, one with `decl` NULL is a tombstone.
  size_t inline_cap, inline_used, inline_live;
  struct css_text_cache * cache_head, * cache_tail;   ///< The cached `cssText` of leaf rules, the entry added first at the head.
  size_t cache_bytes;
  css_cssom_tap_fn tap;                ///< A test's hook: called at the mutation, never deferred by a batch. The observers are the registry below.
  void * tap_user;
  /* observers, batches (cssom_events.c) */
  css_observer * observers;
  size_t nobservers, capobservers;     ///< Entries, dead ones included.
  size_t live_observers;
  uint64_t next_observer;
  unsigned delivering;                 ///< Deliveries running (nesting depth, for the compaction).
  unsigned batch_depth;
  struct css_qevent * queue;           ///< The events of the open batch, in the order they were made.
  size_t qcount, qcap, qdone;
  size_t queue_bytes;                  ///< What the open batch's queue holds (the array and the copies in its events); it counts against `max_live_bytes` with the live objects.
  /* the document (cssom_document.c, cssom_scope.c, cssom_index.c, cssom_mql.c) */
  GTEXT_CSS_Conditions * conditions;   ///< Retained; never NULL while the core lives.
  uint64_t conditions_epoch;           ///< Advances when the conditions object is replaced by another.
  GTEXT_CSS_Selector_Match_Options match;
  bool has_match;
  struct GTEXT_CSSOM_Scope ** scopes;          ///< The registered scopes in the order they were made; the document's own is first. Each is held by an internal count.
  size_t nscopes, capscopes;
  struct GTEXT_CSSOM_Scope * doc_scope;
  uint32_t next_context;               ///< The context the next scope that asks for none gets.
  uint64_t structure_version;          ///< Advances when a scope, a `styleSheets` list or an adopted list changed.
  uint64_t registration_version;       ///< Advances when a user-agent or user sheet was registered or taken out.
  css_registered_sheet * regs;
  size_t nregs, capregs;
  GTEXT_CSS_Rule_Index * index;        ///< The index last built (one owner held), or NULL.
  bool index_valid;
  uint64_t index_stamp, index_structure, index_registration, index_epoch;   ///< The tuple it was built for.
  struct GTEXT_CSSOM_Media_Query_List * mql_head, * mql_tail;   ///< The live media query lists in creation order (weak).
  size_t nmql;
  unsigned import_depth;               ///< Import callbacks running above this point.
  /* getComputedStyle() (cssom_computed.c) */
  struct css_cmps * cs;                  ///< The session, made on first use.
  GTEXT_CSSOM_Layout_Interface layout; ///< The caller's layout interface, copied (`has_layout`).
  bool has_layout;
};

/// A notification (css_cssom_core.h names the kinds). The pointers are borrowed for the call.
struct css_cssom_event {
  GTEXT_CSSOM_Event_Kind kind;
  uint64_t sheet_id, rule_id;
  void * declaration;                  ///< The declaration object, or NULL.
  void * sheet;                        ///< The sheet, or NULL.
  void * scope;                        ///< The tree scope of a sheet-list or adopted-list event, or NULL.
  void * rule;                         ///< The rule, or NULL.
  bool silent;                         ///< The mutation changed an object that no sheet holds: no generation, no event.
  /* the rule's place when a rule was taken out (it is not in its list any more): the rule it was in (NULL: the sheet) and the index */
  bool has_position;
  const void * position_parent;        ///< The rule the removed rule was in, or NULL for the sheet's own list.
  size_t position_index;
  /* a replace: the rule's selector list before the change (owned by the event: the commit takes it), and whether making it failed */
  void * selectors_before;
  bool before_failed;
  const void * element;                ///< The owner element of an inline style.
  const GTEXT_CSS_Property_Id * properties;   ///< The longhands whose declarations changed, ascending, without repeats.
  size_t property_count;
  bool all_changed;                    ///< `all` is among them.
  const GTEXT_CSS_String * custom_names;      ///< The custom properties that changed, ascending by bytes.
  size_t custom_count;
  bool attribute_written;
  uint64_t generation;                 ///< The document's generation after the mutation.
};

/* ---- objects (cssom_object.c) -------------------------------------------------------------------------------------------------------------------- */

/// Initialize a freshly allocated object: counts as the caller says, the core retained. `bytes` is charged to the core (the caller checked the limits).
void css_obj_init(css_obj * o, css_core * core, GTEXT_CSSOM_Object_Kind kind, uint64_t internal, uint64_t external, size_t bytes, void (*destroy)(css_obj *));

/// Whether one more object of `bytes` is within the limits of the core.
GTEXT_CSS_Status css_core_room(css_core * core, size_t bytes);

/// Whether `bytes` more are within the live-bytes limit (what a syntax tree and the text it was read from take; they are not objects).
GTEXT_CSS_Status css_core_room_bytes(const css_core * core, size_t bytes);

/// The same as css_obj_init(), with the id given and the document's counter left alone (an object that shares the id of its owner).
void css_obj_init_id(css_obj * o, css_core * core, GTEXT_CSSOM_Object_Kind kind, uint64_t internal, uint64_t external, size_t bytes, void (*destroy)(css_obj *), uint64_t id);

/// Charge or credit bytes to a live object (a string it keeps): false if the limit would be passed (nothing charged).
bool css_obj_charge(css_obj * o, size_t bytes);
void css_obj_credit(css_obj * o, size_t bytes);

void css_obj_retain_external(css_obj * o);
void css_obj_release_external(css_obj * o);
void css_obj_retain_internal(css_obj * o);
void css_obj_release_internal(css_obj * o);

/// Make `parent` the weak parent of `o` (NULL: detach): the hold an externally held `o` keeps on its parent moves with it.
void css_obj_set_parent(css_obj * o, css_obj * parent);

/// A reference of the core (an object, or the document handle). The last release frees the core.
void css_core_retain(css_core * core);
void css_core_release(css_core * core);

/* ---- the core (cssom_core.c) --------------------------------------------------------------------------------------------------------------------- */

/// The core of a document handle, and whether the handle is still there.
css_core * css_document_core(GTEXT_CSSOM_Document * document);
bool css_document_alive(const css_core * core);

/// The start of every mutator: GTEXT_CSS_E_REENTRANT inside a notification or a finalizer, GTEXT_CSS_E_LIMIT_CSSOM when the generation counter is spent.
GTEXT_CSS_Status css_cssom_enter(css_core * core);

/// After a mutation is complete and consistent: advance the generation, deliver the event, and report the first failure of an observer. `outcome` may be NULL.
void css_cssom_commit(css_core * core, css_cssom_event * event, GTEXT_CSSOM_Outcome * outcome);

/// Deliver the event to the observers, or queue it in the open batch (cssom_events.c). Takes `event->selectors_before`.
void css_events_emit(css_core * core, const css_cssom_event * event, GTEXT_CSSOM_Outcome * outcome);

/// Make room for `n` more events in the open batch (nothing when there is none, or no observer): GTEXT_CSS_E_OOM, GTEXT_CSS_E_LIMIT_CSSOM. A mutator that sends more than one event calls it
/// before it changes anything; css_cssom_enter() reserves one.
GTEXT_CSS_Status css_cssom_reserve_events(css_core * core, size_t n);

/// Whether an observer is registered: the mutators that need to capture state for the summary (a replace's selector list) ask. The test tap reads none of it.
bool css_events_wanted(const css_core * core);

/// The end of the document handle: the batch is dropped undelivered and the observers go.
void css_events_teardown(css_core * core);

/// Set the tap (NULL to clear). For tests: it sees every event at the mutation, a batch notwithstanding.
void css_cssom_set_tap(css_core * core, css_cssom_tap_fn fn, void * user);

/// The core of any object.
css_core * css_obj_core(const void * object);

/// A new buffer of `n + 1` bytes from the core's allocator holding a copy of `s` and a NUL; NULL on failure.
char * css_core_dup(css_core * core, const char * s, size_t n);

void css_outcome_reset(GTEXT_CSSOM_Outcome * o, const css_core * core);

/// The tree version the caller's hook gives (0 without one), and the epoch of gtext_cssom_document_tree_changed().
uint64_t css_core_tree_version(const css_core * core, bool * has_hook);

/* ---- declarations (cssom_decl.c, cssom_inline.c) --------------------------------------------------------------------------------------------------- */

struct GTEXT_CSSOM_Decl;

/// What a rule (or any owner) does around a mutation of its declarations. C5b1 fills it for rules; a test fills it with a double.
typedef struct {
  /// Before the mutation: may refuse it (the sheet may not be modified); nothing has changed yet.
  GTEXT_CSS_Status (*prepare)(css_obj * owner, struct GTEXT_CSSOM_Decl * decl);
  /// After the mutation is complete: infallible. Fills the event's sheet and rule ids; bumps the owner's version.
  void (*commit)(css_obj * owner, struct GTEXT_CSSOM_Decl * decl, css_cssom_event * event);
} css_decl_owner_ops;

/// One descriptor of a typed rule's block (a `src` of `@font-face`, the `size` of `@page`): its name (lower case), its value as the typed model writes it, its importance.
typedef struct {
  char * name;
  size_t name_length;
  char * text;
  size_t text_length;
  bool important;
} css_desc;

struct GTEXT_CSSOM_Decl {
  css_obj obj;
  GTEXT_CSS_Declaration_Block * block;
  GTEXT_CSSOM_Decl_Owner owner_kind;
  bool readonly;
  uint64_t version;
  /* rule-owned */
  const css_decl_owner_ops * ops;
  css_obj * owner;                     ///< The rule (weak: the rule owns the declaration).
  /* element-owned */
  GTEXT_CSSOM_Attribute_Binding binding;
  const void * element;
  bool detached;                       ///< The element was released: the binding is not called any more.
  char * remembered;                   ///< The attribute string last parsed or written.
  size_t remembered_length;
  bool remembered_present;
  bool has_version;                    ///< `binding.attribute_version` answered for the remembered string.
  uint64_t remembered_version;
  bool validated;                      ///< Validated at least once.
  uint64_t validated_epoch, validated_tree;
  bool stale;                          ///< gtext_cssom_inline_style_attribute_changed() was called.
  /* the block of a typed rule (cssom_typed.c): a keyframe, a page or margin rule, a font-face, a position-try ... The declarations that are properties are in `block`; the
     descriptors that are not are in `descs`, after them in every listing. Every mutation is made on a copy and accepted only if the rule, read again from the text it makes,
     still holds what was set. */
  bool typed;
  unsigned typed_bypass;               ///< Greater than 0 while the typed code calls the block code of the untyped declaration.
  css_desc * descs;
  size_t ndescs, capdescs;
  /* computed (cssom_computed.c) */
  struct css_comp * comp;
};

/// Make a declaration object of an owner; the block is empty. Counts as css_obj_init() says.
/// `id` 0 takes the next id of the document's counter; another value is the owner's id, shared (a rule's declaration block).
GTEXT_CSS_Status css_decl_create(css_core * core, GTEXT_CSSOM_Decl_Owner owner_kind, uint64_t internal, uint64_t external, uint64_t id, GTEXT_CSSOM_Decl ** out);

/// Build a block from declarations the parser made (a rule's, a nested-declarations run), as a declaration list is read: an invalid declaration is dropped, one the library cannot
/// judge is kept as written, the later of two of a property replaces the earlier. `count` is checked against the document's `max_work`.
GTEXT_CSS_Status css_decl_block_from_declarations(css_core * core, const GTEXT_CSS_Declaration * declarations, size_t count, bool quirks, GTEXT_CSS_Declaration_Block ** out);

/// Validate an element-owned declaration against its attribute (the start of every call that reads or edits it); a no-op for the others.
GTEXT_CSS_Status css_decl_validate(GTEXT_CSSOM_Decl * decl);

/// Parse a declaration list into a new block (the `cssText` setter, the reload of an attribute). `work` is the declarations looked at.
GTEXT_CSS_Status css_decl_parse_block(css_core * core, const char * text, size_t length, bool quirks, GTEXT_CSS_Declaration_Block ** out);

/// Release the inline map entry of an element-owned declaration that is being freed.
void css_inline_forget(css_core * core, struct GTEXT_CSSOM_Decl * decl);
void css_inline_map_free(css_core * core);

/// Write the block back to the element: set the attribute to `text` and remember it. The block swap is the caller's.
GTEXT_CSS_Status css_decl_write_attribute(GTEXT_CSSOM_Decl * decl, const char * text, size_t length, bool * written);

/* ---- the IDL names (cssom_decl_idl.c) -------------------------------------------------------------------------------------------------------------- */

bool css_idl_lookup(const char * attribute, size_t length, const char ** property, size_t * property_length);

/* ---- utilities ---------------------------------------------------------------------------------------------------------------------------------------- */

/// "Serialize an identifier" with the text read as UTF-8 or WTF-8 (css_serialize.c).
GTEXT_CSS_Status css_serialize_identifier_text(const char * text, size_t length, bool wtf8, const GTEXT_Markup_Sink * sink);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_INTERNAL_H
