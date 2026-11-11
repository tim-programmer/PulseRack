# GUI

Status: **milestones 2 and 3 are built** (see the *As built* sections at the end; where the two differ, it and the code win). Reference for the look and the workflow: Voltage Modular (a browser on the left,
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
   Not needed for now. *(Asked for after milestone 3, and built: see* As built: dragging modules from the browser*.)*
6. **A second window** (scope, a larger sequencer): not planned.

## As built (milestone 2)

**Code:** `src/ui/geometry.{h,cpp}` (panel layout, knob mapping, value text, placement: no window, so it is tested headless),
`src/ui/rack_ui.{h,cpp}` (the views and the `Editor` that every edit goes through), `src/ui/style.h` (colours and chrome sizes),
`src/main.cpp`. **Run:** `build/src/pulserack [patch.rack]`. gpuic is fetched like PulseCoreLib (`FetchContent`, branch `main`,
the commit printed at configure; built against `771626e`, then `da89032`, which answered the requests below), with `GPUIC_DEPENDENCIES=SYSTEM` by default.

**What works.** The toolbar (patch name, module count, *no audio yet*); the browser (search over name, category and description,
category chips, a card per module with its description and *Add*); the rack (a scrolling surface: wheel, Shift+wheel, or drag empty
space to pan); a panel per module, each a cached view with a `gp_key` (the node id); **knobs and sliders** (drag 200 px for the whole
travel, Shift held during the drag for a tenth, double-click for the default, the wheel 1 % per notch and Shift+wheel 0.1 %, one step for
whole-number controls, the value in a bubble while dragging); **selectors** (click either half, or the wheel); **switches and the
Run button** (click, with an LED); jacks drawn (round for signals, square for events, outputs on a dark plate) and inert; moving a
panel by its title (snapped to the grid, refused where it would overlap); right-click on the title for *Delete*; the hint line, which describes the control, jack or title under the pointer (with the
value, kept current while it turns) and otherwise what the last action did.
Every change goes through `Editor`, which changes the patch and the running module together (`Engine::set_param`, `compile`).

**Tests.** `rack_tests` gains 9 geometry cases (every panel of the catalogue fits with nothing overlapping, rows in reading order,
travel round trips and exact ends, the log mapping, drag and wheel steps, value text, collisions, free places, moving overlapping
panels apart, the example patches) and an engine case (below). `ui_smoke` drives the real window with gpuic's synthetic input
(71 checks: *Add* from the browser and after a search, knob drag, Shift pressed and released during a drag, double-click,
wheel, the hint line over a knob, a jack, a title and empty rack, selector halves, switch,
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
5. **Shift for fine adjustment is read on every move**, so it can be pressed or let go during a drag (gpuic `da89032`; until then
   it had to be held at the press, see below).
6. **The hint line follows the pointer** (`on_hover` on every control, jack and title bar) and falls back to the status of the last
   action. Before gpuic `da89032` it could only describe the last control pressed or turned.
7. Selectors stop at their ends (no wrap). The Clock's *Run* (a `Button`) latches. **No dead buttons**: New/Open/Save and Undo/Redo
   are absent until milestones 5 and 6.
8. The browser is a plain scrolling column, not `gp_list`: 17 cards do not need virtualising.

**Engine fix found while building:** `Engine::compile` kept a running module's state by node id alone, so a patch replaced
wholesale (a file loaded over another) could hand a clock the oscillator's state memory under the same id: heap corruption (the new
test aborts with `malloc(): unaligned tcache chunk detected` without the fix). It now also compares the type. Editing could not
trigger it (`Patch::add_node` never reuses an id); loading a file over a patch (milestone 5) would have.

**gpuic findings** (sent as `gpuic-prompt-2.md`; **all answered in gpuic `da89032`** and checked, `spike.md`, last follow-up):
* **Drag events carried no modifiers** (`app.c` fired the captured `on_drag` with `mods = 0`), so Shift pressed *during* a drag was
  not seen. Fixed: every drag and release event has the modifiers held at that moment. PulseRack now uses them.
* **No hover callback.** Fixed: `gp_on_hover` / C++ `.on_hover`, with mouseenter/mouseleave semantics. PulseRack's hint line uses it.
* **A drag could be handed to another element** when the pressed one moved among its siblings (new in `771626e`). Fixed: a drag
  keeps the pressed element's handlers until the release. The panels keep their `gp_key` anyway.
* Two warning cleanups (a GCC-only pragma under clang; `-Wmaybe-uninitialized` in gpuic's own tests). Fixed: both builds are quiet.

**Not done** (milestone 2 stops here): cables and the jacks' behaviour (milestone 3); sound, meters, LEDs from telemetry and the
computer keyboard as a piano (4); the sequencer's **step grid** (its 64 steps are kept in the patch but not editable in the window
yet); the on-screen keyboard strip; arrow keys on a focused knob; undo; a Delete key; zoom (decided: not in the first version).

## As built (milestone 3: cables)

**What works.** Press on a jack and drag: a cable hangs from it and follows the pointer, **every jack it could connect to is lit**
(a faint green ring), and the jack under the pointer gets a green ring, or a red one with the reason on the hint line ("a signal
cannot go to an event input...", "this input already has a cable", "this would make a loop"). Release on a lit jack connects;
anywhere else does not. **Press on a plugged input** lifts its cable off: drop it on another input to move it (same cable, same
colour), on empty rack to remove it. A cable is **selected by a click on its stroke** (gpuic's stroke-only hit test, 4 px of
tolerance) and removed with **Delete or Backspace**, or from its **right-click menu**. Cables are cubic Beziers that sag more the
longer they are, take a colour each from a palette of eight in turn, have a plug drawn over each jack, and **follow a panel while
it is dragged** (their ends come from the model: panel position + `panel_layout()`). Deleting a module takes its cables with it.
Every edit compiles the engine at once (`Editor::connect` / `disconnect` -> `Engine::compile`); the smoke test renders the engine
before and after a cable and hears the difference.

**Code.** `Patch::can_connect` (what `connect` would say, changing nothing; `connect` calls it), in the engine. In
`src/ui/geometry`: `jack_center`, `jack_at`, `cable_curve`, `as_cable`. In `src/ui/rack_ui`: `CableView` (one cached view per cable,
keyed by its input, drawn over the panels), `DragView` (the cable being dragged, the lit jacks and the ring under the pointer), and
the `Editor`'s cable operations.

**Tests.** `rack_tests`: `can_connect` agrees with `connect` on every ordered pair of jacks in a patch, in a random order, and
changes nothing (1,461 checks); jacks are found where they are drawn; cables sag and are oriented output -> input. `ui_smoke`
(105 checks) adds: a drag from an output with the lit jacks checked (its own inputs are not lit: a loop), a red verdict over an event
input, a release on a VCA's input and the engine's output going from silence to sound; refusals (event input, loop, empty release);
lifting a plug onto another input (same colour) and onto empty rack (removed); a drag from an empty input to an output; selecting by
a click, Backspace going to the focused search box and not to the cable, Delete removing it; the menu's *Delete cable*; a cable
following its panel; and the module's cables going when it is deleted. `ui_smoke --pose PATCH NODE PORT X Y` holds a cable in
mid-drag for a screenshot. Float, Q15 and Q31 pass. Under clang with ASan and UBSan the window tests first stopped on one report
inside gpuic (below); since gpuic `7d2cb8d` all of them pass with `halt_on_error=1`.

**gpuic finding** (sent as `gpuic-prompt-3.md`; **fixed in gpuic `7d2cb8d`** and checked): `src/paint.c:983` copies a cached view's paint chunk with
`memcpy(vv->chunk, pc->scene->items + start, count * sizeof *vv->chunk)`; when the view painted nothing (`count == 0`) and has never
had a chunk, `vv->chunk` is NULL, and `memcpy(NULL, ..., 0)` is undefined behaviour in C (UBSan: "null pointer passed as argument 1,
which is declared to never be null"). Harmless in practice; it stops a UBSan run. Cables brought the first cached views that can be
empty (`DragView` when no cable is being dragged, a `CableView` while its cable is lifted). gpuic now skips the copy when there is
nothing to copy, fixed the same pattern in two more places it found (a list splice and the font cache), added `emptyview_test`, and
has a `GPUIC_SANITIZE` option that makes any UBSan report fail its tests.

**Decisions taken while building (mine, for your review).**
1. **A lifted cable stays in the patch until it is released**, so the sound goes on during the drag; where it may go is judged on a
   copy of the patch without it. A release on an incompatible jack leaves it where it was.
2. **A cable can also start at an empty input** and end on an output (the design only had outputs and connected inputs).
3. **All the jacks a cable fits are lit for the whole drag**, not only the one under the pointer.
4. **A cable over a panel takes the press**, title bars included; a press on a plug is a press on its jack. Cables are drawn at 85 %
   opacity (the selected one opaque and thicker): about twice the cost of opaque cables (`spike.md`), well inside a frame.
5. **Delete and Backspace** remove the selected cable unless a text field has the keyboard; a click in the rack takes it away from
   the search box.
6. **The hint line shows what an action did at once** (it used to wait for the pointer to leave the element it was over).
7. **Cable colours are not saved**: they are handed out in turn, in file order when a patch is opened.

**Not done** (milestone 3 stops here): the rack does not scroll by itself when a cable is dragged to its edge (scroll first, then
drag); an output's cables cannot be moved by their output end (a press on an output always starts a new cable); no undo; no
colour choice; sound (milestone 4).

## As built: dragging modules from the browser (after milestone 3)

Asked for after milestone 3 ("dragging them feels more natural"), alongside the *Add* button.

**What works.** Press on a browser card (anywhere but its *Add* button) and move: past 4 px it becomes a drag. A pill with the
module's name follows the pointer; over the rack, an outline of the panel shows where it will land, held by the middle of its
title and snapped to the grid, **green** where it fits and **red** where it would overlap another panel; the hint line says which.
Release on green adds the module there (`Editor::add_module_at`, the same path as *Add*); release on red, or off the rack, adds
nothing and says why. A click on a card (no movement) does nothing, so the *Add* button works as before.

**Decisions (mine, for your review).**
1. **An overlapping drop is refused**, as a panel move onto another panel is; it does not push panels aside or pick the nearest
   free place, either of which could put the module somewhere you did not aim.
2. **The panel is held by the middle of its title**, as when it is moved.
3. The rack does not scroll by itself when the pointer reaches its edge (as with cables).

**Code and tests.** `Editor::module_drag_begin/move/end` and `drop_slot`; the outline is drawn by `DragView`, the pill by a
`PointerChip` view at window level. `ui_smoke` (113 checks) adds: a click on a card adds nothing; a drag to a free place (the
outline and the hint checked mid-drag, the module added there and running in the engine); a drop onto a panel (refused, with the
reason); a drop off the rack (nothing). `ui_smoke --pose-module PATCH CARD X Y` holds a card mid-drag for a screenshot.

**Found while building:** an overlay the size of the window with no handlers lets clicks through but **not the wheel** (gpuic
scrolls the innermost scroller under the pointer, and the overlay is not inside the rack's): checked, the rack's scroll stayed at 0
under the wheel with such an overlay. The pill's view therefore lays out its large area only while a card is dragged. *Possible
request for gpuic:* let the wheel pass through elements with no handlers, as clicks do. `port_center` now checks its port index (a harness run against a half-built rack
indexed a jack with -1).

## As built (milestone 4: sound)

**What works.** The toolbar's **Audio** button switches the sound card on and off (on at start; `pulserack --no-audio` starts
without), and next to it the driver, the rate, the latency and the CPU load (`pipewire · 48.0 kHz · 5.3 ms · CPU 1 %`), or why
there is no sound. Each panel has an **activity LED** in its top-left corner, under the title: green, brighter with the level of
what its connected outputs carry (a gate, a clock, audio), red if the module has dropped events. The **Audio Output** panels show
the output level on two bars (-60 to 0 dB; amber near the top, red at full scale). The **computer keyboard is a piano** for every
Keyboard module in the patch: A W S E D F T G Y H U J K O L P ; play C to E an octave up from C4 (white keys on the home row, black
ones above), Z and X move the octave (-4 to +4), auto-repeat is ignored, and a note is released by its own key even after an octave
change. While the search box has the keyboard, keys are its own. Everything the window changes reaches the running modules through
the engine's command ring (`architecture.md`, *As built (milestone 4)*).

**Decisions (mine, for your review).**
1. **Audio starts on** when the window opens; a missing device is shown, not an error, and the window works without sound.
2. **The LED shows connected outputs only** (a module whose outputs go nowhere stays dark): it says "signal leaves here".
3. **The piano plays every Keyboard module**, at velocity 100; with none, the hint line says to add one.
4. The meters show the **master output** on every Audio Output panel (the engine sums all of them into one output).

**Not done** (milestone 4 stops here): the on-screen keyboard strip on the Keyboard panel; the sequencer's step grid and its
playhead; a choice of device; underruns are not measured or shown.

## As built: moving panels (asked for after milestone 4)

You found moving a panel clunky: (1) a panel already on the rack should drag freely, with the green/red outline of the browser
drag, and it was hard to move from where it was; (2) it should be draggable from any unused part of its face, not only the title.

**The cause.** On every pointer move the panel jumped to the grid place under the pointer and **stayed put whenever that place was
taken** (`Editor::move_module` refused it), so it stuck against its neighbours, could not be carried across them, and gave no sign
of where it would land. Only the 20 px title bar started a move; the face took the press and did nothing (so as not to pan the
rack).

**Now.** A press on the title **or anywhere on the face that is not a control, a jack or a cable** starts a move once the pointer
has gone 4 px (a click does nothing). The panel **follows the pointer freely**, unsnapped, drawn over the other panels; its cables
follow it (its model position is the free one meanwhile, and their ends come from the model). The **outline** of the browser drag
shows the snapped place it would land on, green if free and red if it would overlap, with the reason on the hint line. **The
release** puts it there if green, and **back where it was** if red ("Not moved: ..."). A right-click on the face opens the panel's
menu, as on the title. Selectors and switches now take their own press, so a press on one never moves the panel.

**Code and tests.** `Editor::panel_drag_begin/move/end` and `PanelDrag` (the old per-panel move state is gone; `move_module`
stays for code that places a panel directly). `ui_smoke` (123 checks) adds: a click on the face does nothing; a panel pressed on
its face and carried over a neighbour (red mid-drag, the panel and its cable following) to a free place beyond (green), where the
release leaves it; released on another panel it goes back; a knob on the face still turns and the panel stays; a move by the
title. The earlier tests (a refused move onto a neighbour, an accepted one to the next row) pass unchanged.
`ui_smoke --pose-panel PATCH NODE X Y` holds a panel mid-move for a screenshot.

## As built (milestone 5: save and load)

**What works.** The toolbar has **New**, **Open**, **Save** and **Save as** (and Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S), then the
patch's name, marked with ` *` while it has **unsaved changes** (every edit marks it: modules added, moved or deleted, a control
turned, a cable made or removed). **Open** is a menu: *Open a file...* (the system's file dialog, through SDL3), the **factory
patches** (the four in `examples/`, compiled into the program, each with its description), and *Recover the unsaved patch* when
there is one. **New**, **Open** and an example ask first when there are unsaved changes: *Save* (to its file, or where you choose
if it has none, then goes on), *Don't save*, or *Cancel*; the question covers the window until it is answered, and Escape closes
it or the menu. Save on an untitled patch asks where; a name without an extension gets `.rack`. An example opens **untitled**, so
Save never overwrites it. A file is written to a temporary file and renamed, so a failed save leaves the old file as it was, and
says why. A file that does not load changes nothing and says why; a module whose settings the library refuses puts the old patch
back. The view forgets what it held about the old patch (selection, drags, menus, scroll, cable colours, which are given again in
file order).

**Closing the window with unsaved changes.** gpuic has no way to ask before a window closes, so the patch is **kept** instead:
when the window closes, the editor writes it to `$XDG_STATE_HOME/pulserack/unsaved.rack` (or `~/.local/state/pulserack/`); a
crash does not get that far (no autosave while working). The next start
says so on the hint line, and *Open > Recover the unsaved patch* brings it back (unsaved, untitled), after which the kept copy is
removed.

**Code.** `src/ui/files.{h,cpp}` (reading, the atomic write, the state directory; no window) and the generated
`factory_patches.cpp` (from `examples/*.rack` at configure time; a changed example reconfigures); `src/ui/dialog.cpp` (SDL's
dialogs; SDL may answer on another thread, so the answer is passed to the main thread with `gp_post`, gpuic's one thread-safe call);
in `rack_ui`: the `Overlay` view (the menu and the question), and the `Editor`'s `request` / `answer` / `perform`, `open_text`,
`save_to`, `touch`, `write_recovery`. The chooser is a `std::function` on the editor, so the tests can answer it.

**Tests.** `rack_tests` (4 more cases): every factory patch is the file in `examples/`, loads without a warning, compiles and has no
overlapping panels; the atomic write round-trips, replaces, leaves nothing behind, and on failure keeps the old file; the state
directory follows `XDG_STATE_HOME`. `ui_files` (32 checks) drives the toolbar and the menu with the test's own chooser: Ctrl+S on an
untitled patch, the Save button, an example from the menu, the question answered Cancel, Don't save and Save (with and without a
file), Save as cancelled, recovery through the menu, a damaged file, a failed save, Escape, and, after the window has really
closed, the kept patch on disk. `ui_smoke --pose-ask PATCH` shows the question for a screenshot.

**Decisions (mine, for your review).**
1. **Unsaved changes are kept at close, not asked about** (gpuic cannot intercept the close; see below).
2. **Examples open untitled**; **New** is an empty patch (no template).
3. Cable colours are not saved (as in milestone 3); module positions are, as whole rack units.
4. No list of recent files.

**Requests for gpuic** (not sent; for a later prompt): a way to **set a window's title** (the patch name and its unsaved mark belong
there), and a **close request** a program can answer ("save changes?"), so that keeping the patch is the fallback rather than the
only answer.

**Not done:** recent files; a choice of folder for the examples (they are in the program); autosave while working; undo (milestone 6).

## As built (milestone 6, part 1: undo)

**What works.** **Undo** and **Redo** in the toolbar (dimmed when there is nothing to undo or redo), and Ctrl+Z, Ctrl+Shift+Z and
Ctrl+Y (not while the search box has the keyboard: its text is its own). Every edit is a step: a module added (from the browser,
by *Add* or by dragging it in), deleted, moved; a cable made, moved to another input, or removed; a control changed. **Consecutive
changes of one control are one step** (a knob drag, then wheel notches on the same knob; another control starts a new step), as
designed. The hint line says what was undone or redone ("Undone: Oscillator Frequency."). Undoing back to the version that was
saved clears the unsaved mark; a new edit after an undo drops what could have been redone; opening a file or an example starts a new
history. At most 200 steps are kept.

**How.** Each step keeps a copy of the patch model (a few kilobytes) from before the action. Undo and redo swap it back in the way
opening a file does, but the modules whose id and type are still there keep running (their state is not made again, so an LFO's
phase or a held note goes on), and every parameter or sequencer step that differs is sent to its running module through the
engine's command ring: without that, the model would be undone and the sound not. A module that comes back after a deletion is
made from its settings. An action made of others (a cable moved from one input to another: a removal and a connection) is one step.

**Tests.** `ui_undo` (29 checks): nothing to undo at first; three steps for two modules and a dragged cable; a knob drag and two
wheel notches on it are one step, another knob a second; undo by key and by button; **undo reaching the running module** (the
output's level to 0 is silence; undone, the engine is heard again); a move by the face undone; a deleted module coming back with its
cable and its sound, redone and undone; all the way back to the empty patch (not unsaved) and forward with both redo keys; a new edit
dropping the redo list; the saved mark following undo and redo across a save; Ctrl+Z in the search box; a new history on opening.

## As built (milestone 6, part 2: the sequencer's step grid)

**What works.** The Sequencer's panel has a **step grid** between its controls and its jacks: **four pages of 16 steps** (tabs
1-16 ... 49-64 above the columns) instead of the 16-column scroller the design had (tabs are easier to hit in a 16 HP panel). Each
column is one step: a **note bar** (its height is the note, C1 to C8; drag it, 4 px a semitone; the wheel, a semitone a notch;
Shift+wheel, an octave; a double-click turns the step on or to a rest), an **on/rest pad** and a **tie pad** below it. A step that
is a rest shows its note faintly; steps past the sequence's *Length* are dimmed. While audio runs, the **step being played is lit**
(from telemetry: the library's `pcl_seq_position`, read at 30 Hz). The hint line says what a step is ("step 3: F#3, on, tied to the
next"). Edits go through `Editor::set_step`: **one undo step per step edited** (a drag and the wheel on one step are one), and the
running sequencer gets them through the command ring.

**Not done:** a step's own gate length (each step uses the sequence's *Gate*), its velocity (on is 100), and copying or clearing
pages.

**Code and tests.** Telemetry: `position` per module and the descriptor-side `module_position`. Geometry: `step_grid()` (the tabs and
each column's bar and pads) and `note_fill()`, with a test that the grid fits between the controls and the jacks with nothing
overlapping. `ui_steps` (18 checks): the on pad, heard through the engine (a clock, the sequencer, Note to Gate, the output: silent
with every step a rest, a gate once step 1 is on); the tie pad; a double-click; a drag of 20 px is 5 semitones and one undo step;
the wheel and Shift+wheel joining that step; undo of the lot; the second page's first column is step 17. `ui_sound` checks that the
playhead moves through several steps while the clock runs.

## As built (milestone 6, part 3: the on-screen keyboard)

**What works.** The Keyboard panel's face is a **two-octave piano on end** (low notes at the bottom, the Cs named), starting at
the note the computer keyboard's A key plays (C4, moved by Z and X), so the two show the same range. Pressing a key plays **that**
Keyboard module; dragging across the keys **glides** (each note released before the next sounds); releasing ends the note. Keys
light while held, by the mouse or by the computer keyboard. Notes are not undoable (they are not edits).

**Code and tests.** Geometry: `keyboard_strip()` (the keys, black over white, and which key is under a point), with a test (every
key found at its middle, higher notes higher up, the corners). `ui_steps` adds: a held key heard through Note to Gate and the output,
silence after the release; a glide from C4 to E4 ending on E4 and leaving nothing held; the computer keyboard playing the same
module.

