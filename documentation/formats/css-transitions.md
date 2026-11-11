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

# What is not here

The events (transitionrun, transitionstart, transitionend, transitioncancel) and the `CSSTransition` object: the update returns counts and the caller can read the set before and after. `@starting-style` is the style options' `starting_style` flag (compute the starting style, then the style, and give the first as the before-change style).
The specification builds the after-change style from the *before-change* `animation-*` values; this library asks the two styles which properties an animation applies, which agrees for every
property an animation applies in both and differs when an animation starts or ends in the same event as a change of the property it did not apply before.
