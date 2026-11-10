# GUI

Status: **milestone 2 is built** (see *As built* at the end; where the two differ, it and the code win). Reference for the look and the workflow: Voltage Modular (a browser on the left,
a rack of coloured panels with knobs and sliders, cables between jacks, a toolbar above).

The GUI is a *view* of the patch engine (see *patch-engine.md*). It owns no audio state; it edits the
patch model and sends parameter commands.

## Window

```
+----------------------------------------------------------------------------+
| New  Open  Save   patch name        Undo Redo        (o) audio  48k 5.3ms  |  toolbar
+---------------------+------------------------------------------------------+
| [ search...      ]  |                                                      |
| All Source Mod ...  |   panels on a scrollable rack surface                 |
| ------------------  |   with cables drawn over them                         |
| Oscillator  [Add]   |                                                      |
| Noise       [Add]   |                                                      |
| ...                 |                                                      |
+---------------------+------------------------------------------------------+
| hint line: what the control under the pointer does, its value in units     |
+----------------------------------------------------------------------------+
```

* **Browser**: a virtualised list (gpuic's `gp_list`) of module cards: name, category, one-line description, an
  *Add* button. The search box is gpuic's text input; category chips filter. Everything comes from the
  descriptor table, so a new module appears without UI work.
* **Rack surface**: panels at free positions on a grid, no overlap (a move that would overlap is refused;
  a new module goes to the first free spot near the visible area). Pan by dragging empty space or with
  the wheel. **No zoom in the first version** (decided); every coordinate in the model is then a plain pixel
  offset, which keeps cables and hit tests simple. A later zoom would need the pointer to follow `gp_scale`.
* **Panel colour by category**, as in the reference: sources green, modulators red, filters blue, event modules teal, utilities
  grey. A title bar (drag to move; right-click for *Delete*), controls above, jacks along the bottom.
  Dark theme first, since that is how the reference looks and it shows the cables best. **The look is the
  restrained one** (decided): flat category colour with a subtle gradient, a clear knob pointer, no bevels or
  metal textures in the first version.
* **Toolbar and status**: patch name, audio on/off, the device's real rate and latency, CPU load from the
  telemetry (time spent in the callback over the time available), and the hint line.

## Controls

All come from the same data: a `param_desc_t` gives the kind, the position and the range, and one generic
renderer draws it.

| Control | Look | Interaction |
|---------|------|-------------|
| **Knob** | a rounded face, a rotated pointer, −135° to +135° | press and drag vertically (about 200 px for the full range); **Shift** = fine (×0.1); **double-click** = default; arrow keys when focused; a bubble shows the value in units while dragging |
| **Slider** | a track and a thumb, vertical or horizontal | same drag rules |
| **Selector** | `‹ saw ›` with the choice's name | click either side and the wheel (`gp_on_wheel`); for ENUM and STEPS parameters |
| **Switch / button** | latching or momentary, with an LED | click; Play, Latch, Retrigger, Unipolar |
| **Step grid** | the sequencer's steps | 16 columns visible, scrolls to 64; each column has a pitch slider, a rest/tie toggle and a gate bar; a lit column follows the playhead |
| **Meter / LED** | level bar, gate, clock, dropped-events | read-only, from telemetry |
| **Jack** | circle (signal) or square (event), with a label | see below |

Knob and slider values go through the descriptor's `scale` (LOG for Hz and times). A drag changes the model
and sends one parameter command per frame; consecutive changes of one control are **one undo step**.

**Verified in gpuic's headers**: mouse-down with capture so the drag continues outside the element, `local`
coordinates, `click_count` (double-click), `button` (right-click), `mods` (Shift, Ctrl), a rotated child whose hit-test
follows the transform, hover and pressed styles, gradients, borders, rounded corners. **Verified by the spike**
(`spike.md`): a knob press-drag-release works under injected input, also with an overlay above it.
**The wheel** did not reach a plain element in gpuic `f6157d9`; gpuic `bc442de` added `gp_on_wheel(fn)` for any element
(checked: +40 `dy` per notch, shift+wheel arrives as `dx`, the innermost handler consumes it unless it calls
`gp_propagate`). A knob uses it for the fine adjust; Shift+drag stays.

## Cables

* Press on an **output** jack (or on a *connected input*, which picks its cable up) and drag; compatible free
  inputs light up as the pointer passes over them; release on one to connect. Release on empty space cancels;
  release on an incompatible jack cancels and the hint line says why ("a signal output cannot go to an
  event input", "this input already has a cable", "this would make a loop").
* Cables are Bezier curves that sag under gravity, coloured in turn from a palette so that crossing cables
  can be told apart. A cable is selected by clicking it (the hit test is on the stroke only, with a few pixels of tolerance) and removed with Delete or
  from its context menu; dragging it off its jack also removes it.
* **Kinds are visible without colour**: signal jacks are round, event jacks are square. Cable colour is an
  aid, not a signal.
* Cable endpoints come from the **model**, not from layout: panel position + the jack's `pos` in the descriptor,
  (no zoom). Cables therefore never wait for a layout pass and move with the panel while it is dragged.

### How a cable is drawn: the main technical risk

gpuic has no immediate-mode canvas. What it has is boxes, text, images and **SVG**; its vector primitive
is `gp_svg_from_path` (a path in SVG syntax, with fill and stroke, rasterised by nanosvg into an image that is
then drawn, sharp at any size; README, *SVG and paths*). Three ways to draw cables with it:

| Option | How | Concern |
|--------|-----|---------|
| **A. One SVG per cable** | `gp_svg_from_path` of a cubic Bezier in an absolutely positioned layer | the image is the cable's bounding box, so a long diagonal cable is a large mostly empty bitmap; the dragged cable is re-rasterised every frame |
| **B. Segments** | each cable cut into ~16 short thin rotated rounded boxes (`gp_rotate` about each box's origin) | no rasteriser and no bitmap, but ~16 nodes per cable; joins and anti-aliasing of rotated boxes are unknown |
| **C. A polyline primitive in gpuic** | a new `gp_polyline(points, n, width, color)` drawn by `render_gl.c` | the cleanest result; **a change to gpuic**, which is your repository, so it is your decision |

**Decided: option C, and it has landed.** gpuic `bc442de` added `gp_curve` (cubic Bezier) and `gp_polyline`, with `gp_stroke_width`,
`gp_stroke_color`, round or butt caps, round or miter joins and `gp_hit_tolerance`; C++ spelling `curve(xy(..), xy(..), xy(..), xy(..))
.stroke_width(4).stroke_color(c).hit_tolerance(4)`. Its points are in the **parent's** space (as `gp_left`/`gp_top` use), so the rack's
cable layer is a full-size parent and every cable is computed from the model. The element is drawn by the GPU from a few instances (no
bitmap), hit-tested on the stroke only, and clipped, transformed and z-ordered like any element.

**Each cable is a cached view of its own** (`.cached()`), so moving one rebuilds and repaints that view only. Measured from a consumer
(`spike.md`, *Follow-up*): **0.20 ms per draw** with 100 cables and one moving, against 5.4 ms for the SVG stand-in and 0.12 ms for a
knob drag. The SVG stand-in is dropped, together with its two rules (batched releases; no handlers on SVGs).

## How the view is built (gpuic)

The UI is written with gpuic's C++ layer (`gpuic.hpp`, C++20; it only calls the public C functions, so the `gp_*` names
below are what it wraps). Handlers may capture (since gpuic `771626e` also `on_drag` and `on_mouse_up`; not `on_layout`);
a knob's drag state lives in its panel's view state.

* The rack is a view; **each panel is its own view entity** and is shown as a *cached* child
  (`gp_child_view_cached`), so dragging a knob rebuilds that one panel and not the whole rack
  (**measured:** a knob drag with 40 panels costs 0.12 ms per draw, 39 panels reused). Anything that calls
  `gp_app_notify` (including `gp_svg_release`) defeats this, so such calls are batched or avoided.
* The cable layer is a separate view above the panels, so moving a cable never rebuilds a panel.
* The browser, the toolbar and the hint line are plain views. Search is gpuic's text input.
* **Keyboard.** `gp_on_key_down`/`gp_on_key_up` (gpuic delivers key-up) drive the piano: the home row for
  white keys and the row above for black keys, Z/X for the octave. Auto-repeat is ignored. Key events go to
  the search box while it has focus.
* **Redraw.** gpuic draws only when something changed. Meters and LEDs need a steady 30 Hz while audio runs, taken from
  telemetry: `win.ctx().every(1.0/30, ...)` with a `notify` on the meter's entity. **Measured:** 29.6 draws/s at 0.07 ms
  each, about 0.9% of a core, and an idle window does 3 wakeups per 10 s.
* **Fonts.** gpuic finds system fonts itself; the first start scans them (about 0.3 s, cached afterwards).
* Only Linux has been built and run in gpuic, so this rack is Linux-first.

## Spike (milestone 0)

Done; the results, numbers and what they changed are in [spike.md](spike.md). Answered: cable cost with the SVG stand-in,
a knob drag with 40 cached panels, the wheel, a 30 Hz redraw, the 64-step grid, PulseCoreLib and gpuic in one CMake tree
(float, Q15, Q31), and SDL3 audio next to the window. Not measured: GPU time, underruns, other platforms.

## Review of the first window

Milestone 2 ends with a screenshot taken through gpuic's `GPUIC_SCREENSHOT` of three panels (oscillator, ADSR, VCA)
beside the browser, to compare with the reference before any more modules are drawn.

## Open questions

*All answered on 2026-11-09; your lines are kept under each question, and the sections above now follow them.*

1. **Look.** The reference is skeuomorphic (gradients, bevels, metal knobs). Recommendation: a restrained version of it
   (flat colour with a subtle gradient, a clear knob pointer), because a heavy style costs frame time and a
   screenshot is the better judge than a document.
   For the first version the restrained version is okay.
2. **Cable drawing**: A, B or C above, to be settled by the spike. **Option C changes gpuic**; do you want to
   own that?
   Advise a prompt for gpuic and I can get it integrated.
3. **Free panel placement** (recommended) or rows like Voltage Modular's racks?
   Free panel placement is okay.
4. **Zoom**: needed for the first version, or later?
   Zoom not needed for now.
5. **Dragging a module from the browser onto the rack** as well as the *Add* button: later (recommended).
   Not needed for now.
6. **A second window** (scope, a larger sequencer): not planned.

## As built (milestone 2)

**Code:** `src/ui/geometry.{h,cpp}` (panel layout, knob mapping, value text, placement: no window, so it is tested headless),
`src/ui/rack_ui.{h,cpp}` (the views and the `Editor` that every edit goes through), `src/ui/style.h` (colours and chrome sizes),
`src/main.cpp`. **Run:** `build/src/pulserack [patch.rack]`. gpuic is fetched like PulseCoreLib (`FetchContent`, branch `main`,
the commit printed at configure; `gpuic 771626e` when this was built), with `GPUIC_DEPENDENCIES=SYSTEM` by default.

**What works.** The toolbar (patch name, module count, *no audio yet*); the browser (search over name, category and description,
category chips, a card per module with its description and *Add*); the rack (a scrolling surface: wheel, Shift+wheel, or drag empty
space to pan); a panel per module, each a cached view with a `gp_key` (the node id); **knobs and sliders** (drag 200 px for the whole
travel, Shift at the press for a tenth, double-click for the default, the wheel 1 % per notch and Shift+wheel 0.1 %, one step for
whole-number controls, the value in a bubble while dragging); **selectors** (click either half, or the wheel); **switches and the
Run button** (click, with an LED); jacks drawn (round for signals, square for events, outputs on a dark plate) and inert; moving a
panel by its title (snapped to the grid, refused where it would overlap); right-click on the title for *Delete*; the hint line.
Every change goes through `Editor`, which changes the patch and the running module together (`Engine::set_param`, `compile`).

**Tests.** `rack_tests` gains 9 geometry cases (every panel of the catalogue fits with nothing overlapping, rows in reading order,
travel round trips and exact ends, the log mapping, drag and wheel steps, value text, collisions, free places, moving overlapping
panels apart, the example patches) and an engine case (below). `ui_smoke` drives the real window with gpuic's synthetic input
(65 checks: *Add* from the browser and after a search, knob drag, Shift-drag, double-click, wheel, selector halves, switch,
slider, a refused and an accepted panel move, panning, the menu's *Delete*, then every module added once); `ui_screenshot` opens
`examples/sequence.rack` and checks the picture is not blank. Both need a display and report *skipped* (77) without one. Float,
Q15 and Q31.

**Decisions taken while building (mine, for your review; each is cheap to change).**
1. **The panel layout is computed from the descriptor**, not placed by hand: controls flow in rows under the title in descriptor
   order (a selector takes a row), jacks flow from the bottom edge (inputs, then outputs). `PanelPos` is removed from the
   descriptors; cables (milestone 3) find a jack with `panel_layout()` from the model alone, as planned. A module still needs no UI
   code. A hand-placed override can come later if a panel looks wrong.
2. **Units:** a position is in rack units (HP) on both axes; **1 HP = 16 px**; **a panel is 26 HP (416 px) high**, so two rows fit
   the 1400 x 900 window. Knob cells are 52 x 72 px, so a 4 HP panel holds one knob per row.
3. **The example patches overlapped** (they were written before panels had a size): they are moved apart (only `x=`, `y=`; the
   audio is the same). A file whose panels overlap is opened with them moved right on their row, and the hint line says so.
4. **`ModuleDesc` gains `description`**, the browser card's line (17 written).
5. **Shift for fine adjustment is taken at the press** and holds for the drag: gpuic gives drag events no modifiers (below).
6. **The hint line describes the last control pressed or turned**, not the one under the pointer: gpuic has no hover callback.
7. Selectors stop at their ends (no wrap). The Clock's *Run* (a `Button`) latches. **No dead buttons**: New/Open/Save and Undo/Redo
   are absent until milestones 5 and 6.
8. The browser is a plain scrolling column, not `gp_list`: 17 cards do not need virtualising.

**Engine fix found while building:** `Engine::compile` kept a running module's state by node id alone, so a patch replaced
wholesale (a file loaded over another) could hand a clock the oscillator's state memory under the same id: heap corruption (the new
test aborts with `malloc(): unaligned tcache chunk detected` without the fix). It now also compares the type. Editing could not
trigger it (`Patch::add_node` never reuses an id); loading a file over a patch (milestone 5) would have.

**gpuic findings (requests for gpuic):**
* **Drag events carry no modifiers.** `app.c` fires the captured `on_drag` with `mods = 0` (and the platform layer only smuggles
  modifiers into button events), so Shift pressed *during* a drag is not seen, with real or synthetic input. Wanted: the current
  modifiers on every drag event.
* **No hover callback.** Hover *styles* exist; a callback when the pointer enters or leaves an element (or moves over it) does not,
  so a hint line or a tooltip cannot follow the pointer.

**Not done** (milestone 2 stops here): cables and the jacks' behaviour (milestone 3); sound, meters, LEDs from telemetry and the
computer keyboard as a piano (4); the sequencer's **step grid** (its 64 steps are kept in the patch but not editable in the window
yet); the on-screen keyboard strip; arrow keys on a focused knob; undo; a Delete key; zoom (decided: not in the first version).

