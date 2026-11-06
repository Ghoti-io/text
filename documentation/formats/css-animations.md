@page format_css_animations Animations: the effects of an element and the value of a property at a time

# CSS animations: the effects of an element

Headers: `css_animation.h`.

CSS Animations 1 and 2 and Web Animations 1 (the drafts are pinned by `tools/css/DRAFTS_SHA256` and fetched by `tools/css/fetch.sh`) give an element a list of **animation effects**. The
library reads that list from the computed style, and samples it at a time the caller supplies; it never runs an animation, fires an event or reads a clock.

## The effect list

`gtext_css_anim_list_from_style()` reads the `animation-*` longhands of a computed style into one list of `GTEXT_CSS_Anim_Effect`. `animation-name` is "the coordinating list base
property" of the group (Animations 1, "Declaring Animations"): each item of its list is one effect, and each of the other lists is repeated to the length of the names or truncated to it
(Values 4, "Linked Properties"). An item that is `none` is not an effect but keeps its place, so the others still coordinate; `GTEXT_CSS_Anim_Effect::index` is the place.

| Field | From |
| --- | --- |
| `name`, `name_is_string` | `animation-name` (an identifier or a string) |
| `duration_ms`, `duration_auto` | `animation-duration` (`auto` is 0 for the document timeline) |
| `easing` | `animation-timing-function`, the typed easing function (css_function.h) |
| `delay_start_ms`, `delay_end_ms` | `animation-delay-start`, `animation-delay-end` (Animations 2: the two halves of `animation-delay`) |
| `infinite`, `iteration_count` | `animation-iteration-count` |
| `direction`, `fill`, `composite`, `paused` | `animation-direction`, `animation-fill-mode`, `animation-composition`, `animation-play-state` |
| `timeline_auto` | `animation-timeline` is `auto`: the document timeline. Other timelines are not sampled. |

The effects are in list order, and the last has the highest composite order ("the animation closest to the end of the list of names wins"). The list owns the names and the easing functions;
it is released with `gtext_css_anim_list_free()`. Every allocation goes through the allocator given (or the style's), and a failure leaves nothing behind.

# Sampling the effects at a time

`GTEXT_CSS_Style_Options::animation` (a `GTEXT_CSS_Style_Animation`, css_style.h) asks `gtext_css_style_compute()` to sample. The library reads no clock and runs nothing: the caller gives the
`@keyframes` rules (a set, css_atrule_set.h) and, through `current_time`, the current time of each animation (Web Animations 1: the local time of its effect, in milliseconds). A paused
animation's time is the one it holds; a callback with no time for an animation, or no callback, leaves that animation unsampled, and the style says for which properties
(`gtext_css_computed_animation_untimed()`, `gtext_css_computed_animation_untimed_custom()`). Without the options the style is the underlying one, as before.

## What is computed

1. The cascade's winners are computed once without animations: the **base style**, "the computed value for that property in the absence of animations" (Web Animations 1, "Applying the
   composited result"), whose `animation-*` longhands are the effects (above).
2. For each effect whose keyframes exist, whose timeline is the document's and which has a time, `gtext_css_anim_timing()` is the timing model of Web Animations 1: the phase, the active time
   with the fill mode, the overall and simple iteration progress, the current iteration index and the directed progress. An effect with no resolved active time (before its delay with no
   backwards fill, after its end with no forwards fill) applies nothing.
3. Each property any keyframe sets (a shorthand sets its longhands; `all` sets none; a property that is not animatable, or does not apply to the pseudo-element, is left out) has its own list
   of keyframes ("an animation is run independently for each property"), with a keyframe made from the underlying value at `0%` and `100%` where there is none. The value of a keyframe is the
   property's **computed value in the element's own context with that keyframe's declarations in its cascade**, so `em`, `var()`, `inherit` and `sibling-index()` are the element's, and a
   value that is out of range (a negative `calc()` length) is clamped before it is interpolated. `revert`, `revert-layer` and `revert-rule` in a keyframe are the underlying value: a keyframe
   has no origin, layer or rule of its own to roll back to.
4. The two keyframes that bracket the directed progress are combined by the animation type of the property (`css_interp_text()`): numbers, integers (rounded), percentages and dimensions by
   arithmetic, a length with a percentage as a `calc()`, legacy colors premultiplied in sRGB and other colors in Oklab, lists, functions and blocks item by item, and every other pair as a
   discrete value that flips at 50 percent (a `letter-spacing` of `normal` is 0 and a `font-style` of `normal` is `oblique 0deg` when the other end is not a keyword; `currentcolor` is the
   element's color). The distance is passed through the easing function of the first keyframe (`animation-timing-function` in the keyframe, else the animation's),
   `gtext_css_easing_output()`: the cubic Bezier is extended along its tangents outside 0 to 1, the step functions use the before flag, `linear()` extends its end segments.
5. The effects of a property are composited in the order of the names (the last wins), each over the one before; `animation-composition` (or the keyframe's own) of `add` and `accumulate`
   combine the keyframe's value with the underlying value before the two are interpolated.
6. The result is declared **after** the cascade's winners, which is the animation origin: it overrides a normal declaration and not an important one. The element's style is computed again from
   that, so the children that inherit the property, a `var()` that reads an animated custom property and the `em` of an animated font size all see the animated value.

Every allocation goes through the context's allocator; the sweep over each of them is in `tests/test-css-animation-sample.cpp`.

# In a document

A `GTEXT_CSSOM_Document` (\ref format_css_cssom "css-cssom.md") samples the animations of every element and pseudo-element it computes: the `@keyframes` rules of its sheets are collected with the
rule index, and the time of each animation is asked of the layout interface (`animation_time`, with a `timeline_version` that keys the cached styles by the time as well as the tree). An animation
that is given no time is not sampled, and `gtext_cssom_computed_origin()` says `GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE` for the properties it targets, as it says `ABSENT_LAYOUT` for a used value
that needs a layout.

# What is not here

Running animations, the `animationstart` family of events, transitions (they need the style before the change), and scroll- or view-driven timelines: an effect whose `animation-timeline` is not
`auto` is not sampled. A property whose animation type is not by computed value (a transform list that does not match, the individual transform properties when one is `none`, a basic shape, a grid track list, a filter list) is combined by the generic rules above where its values have one shape, and is discrete where they do not.
