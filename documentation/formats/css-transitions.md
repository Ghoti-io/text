@page format_css_transitions Transitions: the running transitions of an element and their current values

# CSS transitions: the running and the completed transitions of an element

Headers: `css_transition.h`, `css_style.h` (`GTEXT_CSS_Style_Transitions`), `css_cssom_computed.h` (the tracker).

CSS Transitions 1 and 2 (pinned by `tools/css/DRAFTS_SHA256`) say that "implementations must maintain a set of running transitions, each of which applies to a specific element and
non-shorthand property", and a set of completed ones, and that on a **style change event** they start transitions from the difference between the *before-change style* and the *after-change
style*. The library reads no clock and keeps nothing between calls, so the sets are values the caller owns and the style change event is a call.

# The calls

| Call | What it is |
|---|---|
| `gtext_css_transition_set_new()` | The sets of one element, or of one of its pseudo-elements. Empty. |
| `gtext_css_transitions_update()` | One style change event for one element: the set, the before-change style, the after-change style and the time. All or nothing. |
| `GTEXT_CSS_Style_Options::transitions` | The set and a time: the style gets the current value of each running transition. |
| `gtext_css_transition_current_value()` | The current value of one running transition at a time, as text. |
| `gtext_cssom_document_after_change_style()` | The after-change style of an element of a document: its style without transitions, inheriting from its parent's. |
| `gtext_cssom_transition_tracker_*()` | A tracker: the sets and before-change styles of the elements of a document, and the style change event over the chain from the root. |

The **after-change style** is the computed style with `transitions` NULL (and so inheriting from the parent's after-change style); the **before-change style** is the one the caller kept from the
previous event; the style that is shown is computed with the set. A caller that keeps its own structure calls `gtext_css_transitions_update()` per element top down; a document asks the tracker,
whose `gtext_cssom_transition_tracker_lookup()` and `..._version()` are the layout interface's `transitions` and `transition_version`, so that `getComputedStyle()` shows a transition part way
through and a style cached at one version of the transitions is not shown at another. A read is not an event: nothing starts until `..._flush()`.

# What the update does

For each property that is not a shorthand and not flow-relative (a flow-relative property shares its value with a physical one, and the physical one transitions), in this order:

1. Running transitions whose end time is at or before the event complete: they become completed transitions.
2. If there is no running transition, the before-change and after-change values differ and are *transitionable* (the property's animation type is neither "not animatable" nor "discrete", and the
   pair does not interpolate discretely; with `transition-behavior: allow-discrete` the discrete ones are), there is a matching `transition-property` value (the last one that matches: `all`,
   a name, a shorthand's name), the combined duration (the matching duration, at least 0s, plus the matching delay) is greater than 0s and there is no completed transition with the
   after-change value as its end value, a transition starts: start time = event time + delay, end time = start time + duration, start value = the before-change value.
3. A completed transition whose end value is not the after-change value is forgotten; a running or completed one with no matching `transition-property` value is cancelled or forgotten.
4. A running transition whose end value is not the after-change value is cancelled (when the current value equals the after-change value, or the two are not transitionable, or the combined
   duration is not greater than 0s), reversed (when its reversing-adjusted start value is the after-change value: the *reversing shortening factor* is the absolute value, clamped to 0..1, of the
   output of its timing function at the event times the old factor, plus 1 minus the old factor, and the new one takes that fraction of the duration) or replaced by one that starts from its
   current value.

A property that a CSS Animation of the element is applying (`gtext_css_computed_animation_applied()`, in either style) starts no transition, and a running transition is not in the cascade
while an animation applies its property. The current value is declared at the transition origin: it beats every other declaration, important ones included, and it is the value the descendants
inherit.

# Custom properties

"When referenced by animations and transitions, custom property values interpolate by computed value, in accordance with the type that they parsed as" (CSS Properties and Values API 1, "Animation
Behavior"), and a custom property that is registered with a syntax that has a computed value type has that as its animation type, where "the animation type is discrete" for one with no such type (the
universal syntax) or no registration (Web Animations 1, "Custom Properties"). So a `transition-property` item that names a custom property (`--w`: names are case-sensitive) starts a transition when the
property's computed value changes, which interpolates by the registered syntax: `<length>`, `<number>`, `<color>`, `<transform-function>` and the lists of them as the animation of the typed value says.
`transition-property: all` names the registered properties that have a type, and not the unregistered or universal ones. A pair of values that is discrete (an unregistered property, a universal one, a
`<custom-ident>`, a list of another length) transitions only with `transition-behavior: allow-discrete`, and then holds the start value until the middle of the transition and shows the end value from it.
A custom property nothing sets has the guaranteed-invalid value, which has no text (an empty one is `--x:;`): a transition from it or to it is discrete too, and its value is reported as `initial`, the declaration that gives it. A running transition of a custom property is in
`GTEXT_CSS_Transition_Info` with `property` 0 and the name in `custom_name`, and a completed one in `gtext_css_transition_set_completed_at()` the same way. The current value is declared at the transition
origin as a custom property declaration, so a `var()` that reads it, and every property that depends on it, sees the interpolated value; registering a property again between two style change events makes the
two styles not comparable, so no transition starts for that event (the tracker already treated a change of context so: "Registering (or changing the registration) of a custom property can change its computed
value, which can start or interrupt a CSS transition").

# Events and `CSSTransition`

The library owns no clock and no event loop, so the events of CSS Transitions 1 and 2 are a **frame** of a set: `gtext_css_transition_set_frame(set, time, events)` compares the phase each transition was
in the last time it was framed with its phase at `time`, and appends what Transitions 2, "Event dispatch", says to dispatch to a list (`gtext_css_transition_events_*`), in the order to dispatch it. The
phase is `idle` until a frame sees the transition, then `before` (the time is before the start time), `active` (from the start time to the end time) or `after` (from the end time on; a transition with no
duration is never active); the table of that section gives the events of a change (idle to before: `transitionrun`; idle to active: `transitionrun`, `transitionstart`; before to after:
`transitionstart`, `transitionend`; active to after: `transitionend`; after to active, before and the rest as the table says, for a caller whose clock moves backwards) and their times: `elapsedTime` is
in seconds, the *interval start* `max(min(-start delay, active duration), 0)` for `transitionrun` and `transitionstart` and the *interval end*, the active duration, for `transitionend`. A cancellation
is queued as it happens (a style change event that cancels, replaces or reverses a transition; `gtext_css_transition_set_cancel_all()` for an element that is no longer being rendered) and the next frame
reports it as a `transitioncancel` whose `elapsedTime` is "the active time of the animation at the moment it was canceled calculated using a fill mode of both" (clamped to the transition's active
interval, so 0 in the delay phase). A transition that was cancelled before any frame saw it reports nothing (idle to idle), and one that completes in a style change event before any frame saw it reports its
`transitionrun`, `transitionstart` and `transitionend` at the next. The events of one frame are in the order Web Animations 1 gives ("update animations and send events"): a stable sort by the **scheduled event time**
(`GTEXT_CSS_Transition_Event::scheduled_time_ms`, on the clock of the transitions' times: `transitionrun` when the style change event happened, `transitionstart` when the delay ends and not before the event,
`transitionend` at the end of the effect, `transitioncancel` at the moment of the cancellation), and between equal times the composite order of the transitions: by transition generation (the number of the
style change event that started it, for the set), then by the codepoints of the expanded transition property name (`GTEXT_CSS_Transition_Event::property_name`: the longhand's name, or the custom property's),
and a `transitioncancel` has the generation it had. The text this library is given says when an event is scheduled only for the playback events; the ideal time of a transition's event is the moment its
phase change happens, and an event of a time that moved backwards (which only a caller with the Animation model gives a frame) has the time of the frame. `gtext_css_transition_set_frame_bound()` and `gtext_css_transition_events_reserve()` let a caller that takes the frames of many sets into one list
reserve the room first, so that no frame can fail (a frame is all or nothing in any case). `gtext_css_transition_set_clear()` is the same as cancelling without the events: the set of something that is gone.
A **pending frame** (`gtext_css_transition_set_frame_pending()`, and `gtext_cssom_transition_tracker_set_frame_pending()` for the tracker) is a frame at which the transitions are not yet ready: the first frame that renders them has not completed, or the timeline is inactive, so the pending play task of a new transition has not run. A transition no frame has seen is *pending*, which the table treats as the phase before the effect, so such a frame reports `transitionrun` only and the next ordinary frame reports the `transitionstart` (and the `transitionend` when the time has passed it); a transition an ordinary frame has already seen is not moved by a pending one. Which frame is the first that is ready is the host's: the text says "as soon as" (Web Animations 1, "Waiting for the associated effect"), and the two browsers differ on a fresh page and agree afterwards.

`gtext_css_transition_set_animation_at()` is the **`CSSTransition`** of a running transition at a time, as a value (`GTEXT_CSS_Transition_Animation`): `transitionProperty`, `playState`, `pending`, `startTime`
(unresolved until the first frame after the transition was made) and `currentTime`, and the effect as `getTiming()` and `getComputedTiming()` read it (the delay, an `endDelay` of 0, the duration, the
easing, a fill of `backwards`, one iteration, and the `progress` that the timing function gives, null after the effect), with the start and the end value of the two keyframes. The library keeps no objects, so
the caller that has one holds it by the transition's `id`, which the events carry too (the `animation` of a `TransitionEvent`), and learns from the events that it finished (a `transitionend`) or was
cancelled (a `transitioncancel`). The methods of `Animation` that change a transition (`pause()`, `finish()`, `reverse()`, setting `currentTime`, `timeline = null`, the `ready` and `finished` promises)
belong to the Web Animations model, which the library does not implement.

The tracker (css-cssom.md) is the document's consumer of both: `gtext_cssom_transition_tracker_events()` is the frame of every element and pseudo-element at the tracker's time, with each event's element and
`pseudoElement`, and a flush of an element that is not being rendered (its after-change style has `display: none`, or that of an ancestor in the flushed chain, unless a running transition of `display` holds it) and `forget()` of an element that is no longer in the document cancel its transitions, as Transitions 2 says ("must also cancel any running
transitions on elements that are no longer being rendered"; "Transitions and animations should not occur on display:none elements" in the pinned `display-none-no-animations.html`). A flush of an element does not reach its descendants that are not in its chain.

The library has no pending play task, so a transition's first frame sees its real phase: one that is active at its first frame reports `transitionrun` and `transitionstart` together (the table's "idle -> active" row), where an implementation that holds a transition pending for a frame reports the run first (the pinned `event-dispatch.tentative.html` says "whether or not the transition is still pending depends on the implementation"). A set holds at most 4,096 events it owes the next frame (`gtext_css_transition_set_events_lost()` counts what a caller that never takes frames lost).

# What is not here

`@starting-style` is the style options' `starting_style` flag, and the tracker uses it: an element that has no before-change style at a style change event (its first, or the first after it was not rendered or left
the document) is compared with its **starting style** (`gtext_cssom_document_starting_style()`: the after-change style with the rules under `@starting-style` in addition, which "cascade as any other grouped style
rules without introducing any new ordering", so they do not necessarily win, and which it alone is made with, its ancestors being made without, as "starting style inherits from the parent's after-change style").
Only a document whose rule index has an `@starting-style` rule (`gtext_css_rule_index_has_starting_style()`) computes the second style. The current value of a transition at its start time is its start value
(Transitions 1: "at or before the start time"), so a transition from the starting style that has no delay reads the starting value until time passes, as the text says and both browsers do not for a timing function
that jumps at 0.
The specification builds the after-change style from the *before-change* `animation-*` values; this library asks the two styles which properties an animation applies, which agrees for every
property an animation applies in both and differs when an animation starts or ends in the same event as a change of the property it did not apply before.

A custom property that transitions from or to a value that an `attr()` made keeps the taint of Values 5, "attr() security": the transition remembers, for its start and its end value, whether the style's value was
tainted (`gtext_css_custom_value_tainted()`), `gtext_css_transition_current_tainted()` says whether the current value at a time is (the start value at or before the start time, the end value from the end time, and
a value made of both when either was), and the sampler declares the current value as a tainted declaration, which computes to a tainted custom value. A `url()` made of it (`image-set(var(--s))`) is invalid at
computed-value time, as it is for the value itself.
