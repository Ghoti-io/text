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
