# Prompt for a gpuic session: a stroked-curve primitive

Status: **done**: implemented in gpuic `bc442de` and checked (`spike.md`, *Follow-up*). The original text follows. This is written to be pasted into a Claude Code session whose working directory is
the gpuic repository. It is self-contained: that session has not seen PulseRack. Everything inside the fence is the
prompt; the notes after it are for you.

---

```text
I'm building a virtual modular synthesizer ("PulseRack") on top of gpuic. The GUI is a rack of panels with knobs
and jacks, and cables between jacks drawn as smooth curves. gpuic has no way to draw a curve except SVG, which
is rasterised into a bitmap the size of the curve's bounding box: wasteful for a long diagonal cable, and the cable
being dragged would be re-rasterised every frame. I want a native stroked-curve element instead.

I built a throw-away spike against your repository (commit f6157d9) and measured the SVG route; the numbers are below, so you can
check the new element against them.

Read the README sections on SVG and paths, boxes, hit testing, transforms and cached views first, and look at
how src/render_gl.c batches boxes and how src/paint.c walks the tree, before designing anything.

## What I need

A new element, in the same style as the existing builders, that strokes a curve:

    gp_node *gp_curve(gp_point p0, gp_point c1, gp_point c2, gp_point p3, ...);   /* one cubic Bezier */
    gp_node *gp_polyline(const gp_point *pts, size_t n, ...);                     /* straight pieces; pts copied */

with modifiers:  gp_stroke_width(float), gp_stroke_color(gp_color),
gp_stroke_cap_round() / gp_stroke_cap_butt(), gp_stroke_join_round() / _miter() (polyline),
gp_hit_tolerance(float).

This is only a proposal. Use whatever names, argument order and coordinate convention fit gpuic best, and tell me
what you chose. The behaviours below are the requirements:

1. **Coordinates.** I will compute every cable's endpoints from my own model (panel position plus a jack's offset),
   not from layout. So the points must be in a coordinate space I can compute without a layout pass: either the
   parent's space or the element's own, as long as it is documented and the element can be placed absolutely
   (gp_absolute) over a whole canvas. The element's box is the bounding box of the points padded by half the
   stroke width plus the hit tolerance, and it takes no part in the layout of its siblings.
2. **Drawing.** Smooth, anti-aliased edges at least as good as the rounded boxes. Flatten the Bezier on the CPU
   to a tolerance of about 0.25 device pixels *at the current transform scale*, and draw it as triangles through the
   existing renderer. No bitmap, no texture upload, no rasterisation per frame. Premultiplied alpha must be right
   (translucent cables over panels). Round caps; the curve must have no visible gaps or notches at its joints.
3. **Hit testing.** A click, a press, a hover and a drag start must register **only on the stroke** (distance to the
   curve at most half the width plus gp_hit_tolerance), not on the whole bounding box. Everywhere else the
   pointer must reach what is underneath (a knob under a cable's bounding box must still work). **Measured today with SVG:**
   an svg_img without handlers is transparent to the pointer, which is good, but the moment it has an on_mouse_down
   handler it takes its whole bounding box: a press on a knob that lies under the cable's box went to the cable (1) and
   not to the knob (0). That is why I cannot make an SVG cable clickable. gp_on_click,
   gp_on_mouse_down, gp_on_drag and the hover/pressed style modifiers must work on it, and the cursor
   modifier too.
4. **Everything that already applies to elements must apply here**: an ancestor's overflow clip, its transforms
   (rotate, scale, translate; both the pixels and the pointer), opacity if gpuic has it, z-order by tree
   order, the dirty tracking and cached views (changing one curve's points must not rebuild or repaint more than
   it has to; say what it does), and being created from a per-frame arena (no allocation that outlives a frame).
5. **Performance, and no collateral invalidation.** I will draw about 100 curves, of which one or two change every frame while
   the user drags a cable, over 40 cached panel views. Report GPUIC_STATS=1 numbers for 100 static curves, and for 100 with
   one moving: build, layout, paint and GL-submit time per draw, and the view renders per draw.
   **Baseline with one SVG per cable today** (per draw, summed): 3.1 ms with 1 moving cable, 3.6 ms with 40 other cables,
   5.4 ms with 100, against 0.12 ms for a knob drag in the same scene; most of it is paint (the nanosvg raster of the moving
   cable) and its upload, and the churn caused 2-3 atlas resets in 3 s. **My target for the new element is at most 0.5 ms per draw
   for 100 curves with one moving.** Changing one curve's points must re-render only that curve's view: today
   gp_svg_release ends in gp_app_notify, which invalidates EVERY cached view (41 view renders per draw instead of 2 while I
   released one SVG per frame), so nothing the new element does may take that route. If you cannot reach the target, say where
   the time goes rather than hiding it.
6. **C++ layer.** Add the matching builders to include/gpuic/gpuic.hpp / the chain generator, as every other
   element has.

## Tests and docs

Follow the repository's own conventions (headless, gp_simulate_*, pixel readback as in box_test, lcd_test,
transform_test):

* pixels on and off the stroke for several widths, both caps, a straight line, a tight curve and a nearly
  degenerate one (all four points equal, p0 == p3, a zero-length polyline);
* anti-aliasing: an edge pixel is partially covered, a far pixel untouched;
* hit tests on and off the stroke, a click that reaches an element under the bounding box, hover and cursor;
* a rotated or scaled ancestor, an overflow clip, a translucent colour over a box;
* changing the points between frames (the dragged cable) repaints correctly and a static neighbour is untouched;
* ASan/UBSan and TSan builds still run clean.

Add a README section in the style of the others, listing what is supported and what is not done.

## Not needed (do not build)

Dashes, gradients along the stroke, arrowheads, filled shapes, text on a path, animation of the points
(I do that myself), and anything about SVG.

## Two small requests and an answer I would like confirmed

a. **A wheel handler for any element** (`gp_on_wheel(fn)`: the event with dx, dy and mods, a way to say it was consumed).
   Measured today: a wheel event reaches only a scroll container; an element with `gp_on_scroll` that is not a scroller
   gets nothing (0 callbacks). I want the wheel to adjust a knob. I have a workaround (a hidden scroller kept at its middle,
   with the offset delta read in `gp_on_scroll`; it gave the right net delta in a test), so this is a nicety, not a blocker.
   Please say whether you would add it and how.
b. **C++ and `-Wpedantic`** (optional). `gp_rgb`, `GP_ENTITY_NULL` and others are C compound literals, which GCC rejects under
   `-Wpedantic` in C++ ("ISO C++ forbids compound-literals"). I can mark your include directory SYSTEM, so it is not a blocker; a
   C++-friendly spelling in the `gpuic.hpp` paths would be nicer. Tell me if you would rather not.
c. **Confirm one thing I only inferred from reading the code:** `gp_platform_shutdown` calls `SDL_Quit()` after the entities are
   dropped, and I rely on that order to close my SDL3 audio device in an entity's destructor. If that order is not
   meant to be stable, say so.

Where the existing architecture makes any requirement above impractical, stop and explain the trade-off before
choosing, instead of quietly relaxing it.
```

---

## Notes for you

* **What it asks for and why.** The curve is a first-class element with stroke-only hit testing. Both the
  "click a cable to select it" interaction and cables over panels depend on it, and SVG cannot give either
  without a large invisible hit box.
* **The proposed API is deliberately only a proposal.** You and that session own gpuic's conventions.
* **The spike changed the prompt** (`spike.md`): the stroke-only hit test and the "no collateral invalidation" requirement are now
  backed by measurements, the performance target has a baseline, and the earlier questions about the redraw timer and
  SDL audio are gone because the spike answered them (`cx.every` works; audio coexists). What is left is a wheel handler
  (a nicety, with a working workaround), an optional C++ warning cleanup, and one confirmation about shutdown order.
* **What I did and did not look at.** I read gpuic's README and public header, and, for the spike, `app.c`, `views.c`, `svg.c` and
  `platform_sdl.c`. I did not read `render_gl.c` or `paint.c`, so "through the existing renderer" is an instruction to that
  session, not something I have checked is easy.
* **Until it lands**, PulseRack's cable code is written against a small interface of its own
  (`cable_draw(points)`), so the first version can use an SVG stand-in (option A in `gui.md`) and switch with one
  file changed.
