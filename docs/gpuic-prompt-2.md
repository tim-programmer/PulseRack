# Prompt for a gpuic session: what milestone 2 found

Status: **done**: implemented in gpuic `da89032` and checked (`spike.md`, *Follow-up: gpuic `da89032`*). The original text follows,
as sent. Written to be pasted into a Claude Code session whose working directory is the gpuic
repository, against gpuic `771626e`. It is self-contained: that session has not seen PulseRack. Everything inside the fence is the
prompt; the notes after it are for you.

---

```text
I'm building a virtual modular synthesizer ("PulseRack") with gpuic's C++ layer: a rack of panels with knobs, sliders and
selectors, a module browser, and a hint line. The first window now works (against your commit 771626e), and building it, plus a
re-check of your previous commit, turned up five things. They are in priority order; each says what I saw, where I think it comes
from, and what I would like. The code locations are from reading your source; confirm them before changing anything.

## 1. Drag events carry no modifier keys (a bug)

A knob turns finely when Shift is held during the drag. Shift held at the press is seen (gp_mouse_event.mods on on_mouse_down
is right), but every on_drag event has mods == 0, with real input and with gp_simulate_mouse(..., mods) alike.

Where I think it comes from:
* src/app.c:253, gp_window_on_mouse_move fires the captured handler as
      fire(w, &w->capture.move, w->mouse, w->capture.origin, GP_MOUSE_LEFT, 0, 0);
  so the modifiers are hard-wired to 0;
* src/platform_sdl.c:257, SDL_EVENT_MOUSE_MOTION calls gp_window_on_mouse_move(w, x, y) with no modifiers at all, and
  gp_platform_inject_mouse smuggles mods into button events only (motion.which = SYNTH_MOUSE_ID).

What I'd like: on_drag (and on_mouse_up, which looks right already) get the modifiers held at that moment: SDL_GetModState()
for real motion, the injected mods for synthetic motion (smuggled the way button events do it). A test: press, then moves with
and without GP_MOD_SHIFT, and the drag handler sees each.

## 2. A drag can be handed to a different element (a regression in 771626e)

Since 771626e a drag in progress takes on_drag / on_mouse_up again from each new tree, finding the element by w->capture.path
(src/app.c:308 and refresh_capture at :414), which is its index among its siblings unless it has a gp_key. When the pressed
element moves among its siblings during the drag, the drag goes on with whatever element now has that path. My test: three
absolutely placed boxes; pressing one moves it to the end of its siblings (as a rack raises the panel being dragged); two moves
and a release. Without keys, on_drag and on_mouse_up went to the box that took its place (0/2/0 and 0/1/0 instead of 2/0/0 and
1/0/0); with gp_key_id they went to the right one. Before 771626e the drag kept the pressed element's own handlers, so this is new.

The reason for the change is sound (a C++ lambda's captures live in the frame's arena, so the old handlers' data could dangle),
but silently driving another element is worse than a dangling pointer is likely. What I'd like, in this order of preference:
* the drag keeps the handlers of the element that was pressed, with their data kept alive until the release (for example: the
  capture copies its handler data out of the frame arena, or the arena of the press frame is kept until the capture ends);
* or, at the least: refresh from the new tree only when the element at that path is the same element (an explicit key that
  matches; or the same key as at the press), and otherwise END the drag (no further on_drag, and on_mouse_up not delivered to
  another element). The README should then say that a dragged element whose siblings can change needs a gp_key.
A test with the three boxes above, with and without keys.

## 3. A hover callback

There are hover *styles*, but no callback when the pointer enters or leaves an element, so a hint line ("Cutoff: 1.20 kHz,
drag to change") or a tooltip cannot follow the pointer; mine describes the last control pressed instead. What I'd like:
    gp_node *gp_on_hover(gp_hover_fn fn);   gp_on_hover_data(fn, data);
    typedef void (*gp_hover_fn)(void *self, gp_ctx *cx, bool inside, void *data);
called when the pointer enters (inside = true) or leaves (false) the element's hit area: on pointer moves, and also when the
tree changes under a pointer that did not move (an element appears under it or goes away), with the same hit rules as the
hover style (transforms, clips, stroke-only for curves). C++: .on_hover([](Self&, Ctx, bool inside) { ... }), able to capture.
Leaving the window counts as leaving. A test with nested elements: entering the child does not make the parent "leave"
(as CSS's mouseenter/mouseleave, not mouseover/mouseout), unless you have a reason to prefer the other.

## 4. A clang warning in svg.c (small)

Building gpuic with clang prints
    src/svg.c:33:32: warning: unknown warning group '-Wstringop-truncation', ignored [-Wunknown-warning-option]
because that pragma is GCC-only; the -Wmaybe-uninitialized pragma a few lines below is already inside #if !defined(__clang__).
Moving -Wstringop-truncation under the same guard should do it. With -Werror in a consumer that compiles gpuic's sources (or a
clang -Werror build of gpuic itself) it would fail.

## 5. Your own tests warn (smallest)

A Release build of gpuic (GCC) prints -Wmaybe-uninitialized for `float ax, ay;` in the box_of helpers of tests/box_test.c:94,
image_test.c:130 and svg_test.c:127 (the same helper is copied into a dozen tests; only those three warn here). Initialising
them (or sharing one helper) would make the build quiet.

## How I checked

A consumer program (C++20, -Wall -Wextra -Wpedantic -Werror, your headers as ordinary includes), driven with gp_simulate_mouse,
gp_simulate_mouse_button and gp_simulate_wheel, on Linux with an NVIDIA GPU, SDL3 3.4.18, GCC and clang. Your 59 tests pass at
771626e. Please keep all of them passing, add a test for each change, and say in the README what changed. Where the
architecture makes something here impractical, stop and explain the trade-off before choosing, instead of quietly relaxing it.
```

---

## Notes for you

* **Priorities.** 1 and 2 matter for PulseRack: 1 makes Shift-drag work as `gui.md` says (today it must be held at the press),
  and 2 is the trap behind the "every dragged element needs a `gp_key`" rule in `spike.md`. Milestone 3 drags cables, and the
  panels are keyed already, so neither blocks it. 3 is what the hint line was designed for. 4 and 5 are cleanups.
* **Item 2 asks for a behaviour, not an implementation.** Both proposals have a cost (keeping a frame's arena, or copying handler
  data); the gpuic session can judge which fits its arena design. Ending the drag is the minimum, because it fails visibly.
* **The hover API is only a proposal.** gpuic owns its conventions; the CSS enter/leave semantics are the part I care about,
  since a hint line flickers otherwise.
* **What I read in gpuic.** `src/app.c` (pointer and capture handling), `src/platform_sdl.c` (event translation and injection),
  `src/svg.c` (the pragmas), the public headers. Not the renderer.
* **When it lands**, PulseRack changes in two places: `PanelView::fine` (`src/ui/rack_ui.cpp`) can go, and the hint line can
  follow the pointer. The `ui_smoke` test's Shift-drag can then press without Shift and add it during the drag.
