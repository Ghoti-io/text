@page format_web_animations Web Animations: timing, the CSS animations of an element and the animations of a document

# The Web Animations model

Headers: `animation/timing.h`, `animation/animation.h`, `animation/events.h`, `animation/css_animations.h`, `animation/world.h` (all through `animation.h`), `css_cssom_computed.h` (the tracker).

Web Animations 1, CSS Animations 2 and CSS Transitions 2 (pinned by `tools/css/DRAFTS_SHA256`) are built here as a model that owns **no clock, no promise and no event loop**: every call that depends on a
time takes it, an unresolved time is a value (`GTEXT_Anim_Time`, never a NaN), the events a frame owes are returned in the order the specification gives and the host dispatches them, and an exception
of the IDL is a status (`GTEXT_Anim_Status`) that a binding throws. What is built is the part that needs only the style change events: the timing of an effect, the CSS Animations and CSS Transitions of an
element, and `getAnimations()`, and the Animation state machine as a value of its own (`animation/animation.h`). `KeyframeEffect` and the effect stack, `Element.animate()`, scroll and view
timelines, and the use of the state machine by the CSS animations and transitions of the tracker (a `CSSAnimation` that script plays, pauses or seeks) are not here.

# Timing (`animation/timing.h`)

`GTEXT_Anim_Effect_Timing` is `EffectTiming` (delay, endDelay, fill, iterationStart, iterations, duration, direction, easing) with an infinity for `Infinity`, `GTEXT_Anim_Optional_Timing` is
`OptionalEffectTiming`, and `GTEXT_Anim_Computed_Timing` is `ComputedEffectTiming` with the intermediate results of the model (the phase, the active time, the overall and simple progress, the iteration index, the
directed progress, the before flag, the transformed progress). `gtext_anim_computed_timing()` is the whole of "Timing model" at a local time, a playback rate (which decides the *animation direction* of the phase)
and the endpoint-inclusive flag. `gtext_anim_timing_validate()` and `gtext_anim_timing_update()` are the checks and the delta update of "Updating the timing of an AnimationEffect" (a `TypeError` for a negative
`iterationStart`, a negative or NaN `iterations` or `duration`, an easing that is not one `<easing-function>`; all or nothing), and `gtext_anim_easing_parse()` reads the easing member. The older
`gtext_css_anim_timing()` (a playback rate of 1, an iteration start of 0) is the sampler's fast path, and a test holds the two to the same answer on a grid of more than 20,000 inputs.

# The Animation state machine (`animation/animation.h`)

`GTEXT_Anim_Animation` is an `Animation` with an associated effect that is its timing only: the **play state**, the pending play and pause tasks, the start time, the hold time, the playback rate and the
pending playback rate, the replace state, and the `ready` and `finished` promises. Each procedure of "Animation model" is one function that names the section it is (`play()` is "play an animation" with
auto-rewind, `updatePlaybackRate()` is "seamlessly update the playback rate", `finish()` is "finish an animation", `cancel()`, `reverse()`, `persist()`, `currentTime = x` is "set the current time",
`startTime = x` is "set the start time", `playbackRate = x` is "set the playback rate"). Every call takes `now`, the current time of the animation's timeline (unresolved for an inactive one), and the
animation never reads a time. What the specification does "as soon as the animation is ready" is a flag that `gtext_anim_animation_run_pending()` runs at the first resolved tick the host gives it, and what it
does in a microtask (the finish notification steps) is a flag that `gtext_anim_animation_checkpoint()` runs. A promise is a **generation** and a state: `ready_generation` and `finished_generation` go up when the
specification makes a new promise (a pause or play that finds no pending task, a cancel, an animation that leaves the finished state), and the *notes* (`gtext_anim_animation_note_at()`) say which generation
settles when and in what order, so a binding keeps one promise for each generation and settles it on the note. The `finish`, `cancel` and `remove` events are appended to the event list of a frame
(`gtext_anim_animation_take_events()`, with `currentTime`, `timelineTime` and the scheduled event time as the specification computes them), in the same list as the events of CSS animations and transitions.
A call that makes an exception changes nothing; one that cannot reserve room for its notes and events fails whole with `GTEXT_ANIM_OOM`. Not in it: a timeline that is not monotonic and is changed to or from
(Web Animations 2 changes "Setting the timeline of an animation": `GTEXT_ANIM_NOT_SUPPORTED`), an effect that is more than its timing, and the decision that an animation is replaced (which needs the effect stack).

# The CSS animations of an element (`animation/css_animations.h`)

A `GTEXT_Anim_Css_Set` holds the `CSSAnimation` objects of one element or pseudo-element as values. A style change event (`gtext_anim_css_set_update()`) gives it the effects of the after-change style and the
`@keyframes` that exist: an effect with a `@keyframes` of its name that uses the document timeline is an animation (a new one is **pending**, with its current time held at 0, until the next frame), the n-th
effect of a name is the n-th animation of it, a change of the properties updates its timing (except the members `updateTiming()` has taken over: "any subsequent change to a corresponding animation property will
not be reflected"), a newly `paused` or `running` `animation-play-state` holds the current time or plays again (the start time is then the time of the style change event less the time held), and an animation no
effect matches is cancelled. A frame (`gtext_anim_css_set_frame()`) makes the pending ready and reports the events of the table of CSS Animations 2, "Event dispatch": `animationstart`, `animationiteration` (one
for several boundaries, with the elapsed time of the boundary), `animationend` and, for a cancellation, `animationcancel` with "the active time ... calculated using a fill mode of both". The snapshot
(`gtext_anim_css_set_info()`) is `animationName`, `playState`, `pending`, `startTime`, `currentTime`, `getTiming()`, `getComputedTiming()` and whether the animation is *relevant* (its effect is current or in effect:
what `getAnimations()` returns). The start of an animation is the time of the style change event that played it, where the specification makes it the time of the frame that made it ready; the same simplification the
transitions of the library make (a probe of both browsers answers alike for the pages the model is scored on).

# The animations of a document (`animation/world.h`)

The tracker holds one set of transitions and one of CSS animations for each element and pseudo-element that has been through a style change event, and the time (`gtext_cssom_transition_tracker_set_time()`: the host's
tick, the only clock). `gtext_cssom_transition_tracker_animation_time()` is the layout interface's `animation_time`, so a style computed at the tracker's time samples each animation at its current time.
`GTEXT_Anim_World` is a view over the tracker. `gtext_anim_world_get_animations()` is `Element.getAnimations({subtree})` and `Document.getAnimations()`: the relevant animations in the composite order (CSS
Transitions before CSS Animations, each by the tree order of its owning element, the pseudo-elements of one element as the specifications list them, then a transition's generation and property name or an animation's
place in `animation-name`). The tree order is the element interface's `compare_order`, which the Markup adapter provides; without it the order the tracker first saw the elements in.
`gtext_anim_world_update()` is "update animations and send events": the events of every transition and animation, in one list, by a stable sort on the scheduled event time, with transitions before animations between
equal times (an animation with no start time has an unresolved scheduled time, which sorts first). It is all or nothing: a frame that cannot be made whole is made, whole, by the next call.

# Checks

`tests/test-anim-timing.cpp`, `test-anim-animation.cpp`, `test-anim-css.cpp` and `test-anim-world.cpp` (each rule a test that quotes its text, an allocation sweep, planted defects of every function in
`tools/conformance/CSS_ANIMATION_PLANTS` and `CSS_CSSOM_PLANTS`, each seen to fail); the pages of web-platform-tests that read `getAnimations()` and the effect's timing are scored by the CSSOM script gate against
the recorded answers of both browsers.
