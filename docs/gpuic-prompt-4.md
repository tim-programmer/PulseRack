# Prompt for a gpuic session: sharp text under a scale, and drags with any button

Status: **not sent yet.** Written to be pasted into a Claude Code session whose working directory is the gpuic repository,
against gpuic `0e0e23a`. It is self-contained: that session has not seen PulseRack. Everything inside the fence is the prompt;
the notes after it are for you.

---

```text
I'm building a virtual modular synthesizer ("PulseRack") with gpuic's C++ layer, against your commit 0e0e23a. Its rack of
panels can be zoomed with the wheel: the whole rack is one element under gp_scale(z) (transform_origin 0,0), z from 0.4 to 2.5.
Two things came out of that. They are in priority order; each says what I saw, where I think it comes from, and what I would
like. The code locations are from reading your source; confirm them before changing anything.

## 1. Text under a scale is blurry (the important one)

Zoomed in (z = 2, say), every label on the panels is soft: it looks like the text of z = 1 stretched to twice its size.
Shapes (rounded boxes, borders, gradients, curves) stay sharp at any zoom; only text does not.

Where I think it comes from: src/paint.c rasterizes a glyph at the font size times the WINDOW scale and takes no account of the
transform it is painted under:

    float scale = pc->scale;
    float px_size = n->ts.size * scale;                       /* around line 717 */
    ...
    gp_text_snap_x(pc->ts, glyphs[gi].font, px_size, (line_x + glyphs[gi].x) * scale, &pixel, &bin);
    gp_glyph_entry e = gp_text_glyph_lcd(pc->ts, glyphs[gi].font, glyphs[gi].glyph, px_size, bin, lcd);
    ...
    set4(inst.bounds, gx / scale, gy / scale, e.w / scale, e.h / scale);

so under pc->xf with a scale of 2 the glyph bitmap made for 11 px is drawn over 22 px. Zoomed out the same bitmaps are shrunk,
which looks acceptable but is not what the font's hinting would give at that size either. The same px_size is used for text on
a path (around line 562).

What I'd like: when the transform a text is painted under keeps the axes (no rotation or skew: xf.b and xf.c are 0, or near
enough), rasterize the glyphs at the size they will have on the screen, px_size = size * window scale * the transform's scale,
and place them so that the result is what the stretched text would have covered. In particular:

* Layout must not change: the text is still measured and wrapped at its own font size, so a label takes the same room in the
  element at every zoom and nothing reflows while zooming. Only the bitmaps and their placement change. Glyph advances at the
  larger size differ a little from the scaled advances of the small size (hinting); I would rather keep the layout's positions
  scaled (each glyph's origin where the transform puts it, then snapped to the pixel grid as now) than re-shape at the big size.
* A non-uniform scale (sx != sy): use whichever is simplest and say which (the geometric mean, as the stroke code near line 600
  does, would be fine); I only use uniform scales.
* Rotated or skewed text can stay as it is today.
* The glyph cache is keyed by pixel size: a continuous zoom makes many sizes. My zoom moves in steps of 10 %, so that is a few
  dozen sizes at most, but say if the atlas needs a size quantization (to a quarter pixel, for example) or eviction to stay
  bounded, and what you chose.
* Subpixel (LCD) rendering: today it is switched off under any transform ("the stripes of a rotated or scaled glyph are not
  the screen's"). Under a pure scale plus translation, with glyphs rasterized at the final size, the stripes are the screen's
  again, so it could stay on. Your call; grayscale at the right size is already a large improvement.
* Cached views: my panels are cached child views (gp_child_view_cached) painted under that scale. A cached view's kept paint
  must not be reused when the scale it was painted under changes (I think the signature at around line 926 already covers the
  transform's numbers; please confirm it also covers this case, since the glyph instances now depend on the scale).
* Text inputs and anything else that draws text through its own path: the same, or say that it is left out.

A test: the same text painted at scale 1 and under gp_scale(2); the second must use glyphs rasterized at twice the size (by
whatever the test can observe: the glyph entries asked for, or a rendered image that is not the upscaled first one), and the
layout box of the text element must be the same in both.

## 2. on_drag and on_mouse_up for the middle and right buttons

An element with on_drag or on_mouse_up captures the mouse only for the left button (src/app.c, gp_window_on_mouse_button:
`if (button == GP_MOUSE_LEFT && (hit.el->on_drag.fn || hit.el->on_mouse_up.fn))`), and gp_window_on_mouse_move fires the
captured handler with GP_MOUSE_LEFT hard-wired. on_mouse_down does arrive for every button. So a drag with the right or middle
button (I pan the rack with them) gets the press and nothing after it; I work around it by polling SDL_GetMouseState 120 times
a second until the button is up, which I would like to delete.

What I'd like: the capture starts for whichever button was pressed on an element that has on_drag or on_mouse_up, and lasts
until THAT button is released; gp_mouse_event.button in on_drag and on_mouse_up is the button that started it. While a capture
is active, a press of another button should not start a second capture or end the first (ignore it, or deliver only its
on_mouse_down, whichever is consistent with what you do now; say which). The left button's extra behaviour (focus, on_click,
scrollbar grabbing, the pressed style) stays the left button's. gp_simulate_mouse_button already takes a button, so a test can
press the right button, move, and release, and check on_drag and on_mouse_up arrive with button == GP_MOUSE_RIGHT, and that a
left-button drag is unchanged.

This changes behaviour for existing programs whose elements have on_drag and are right-pressed: they will now get drag events
they did not get before, and must look at the button. Mention that in the README and the commit message.

Keep all your tests passing, and say in the commit message what changed.
```

---

## Notes for you

* **Why.** Your note 3 ("when zoomed in the fonts on the modules are blurry"). The rack is one element under `gp_scale(zoom)`
  (`RackView::render`), which is what keeps hit tests and the model in rack pixels; gpuic draws each glyph at
  `font size x window scale` and lets the transform stretch it (`build/_deps/gpuic-src/src/paint.c`, `px_size`).
* **The alternative without gpuic** is to lay everything out at the zoomed size: every width, height, position and font size in
  `rack_ui.cpp` times the zoom, in place of the transform. Large, easy to get wrong, and the model's "rack pixels" would no
  longer be the screen's. Not recommended while gpuic can be changed.
* **When part 1 lands:** nothing changes in PulseRack but the gpuic commit it builds against; look at a zoomed rack by eye, and
  check the panels are repainted on a zoom step (they are cached views).
* **When part 2 lands:** `Editor::pan_probe`, `pan_task` and the 120 Hz polling go; the rack's and the panels' `on_drag` /
  `on_mouse_up` handlers then get the right and middle buttons and must look at `ev.button` (a panel's `on_drag` moves the panel:
  left button only). `ui_smoke`'s panning check uses its own `pan_probe` and would use `gp_simulate_mouse_button` with the button.
* Until it is sent and done, the text stays soft when zoomed in; 100 % is unaffected.
