# Milestone 0: the gpuic spike

Status: **done; results for your review.** The throw-away program is `spike/` (one `spike.cpp` and a `CMakeLists.txt`);
nothing from it is meant to be kept, and the whole directory can be deleted. It is not committed.

* **Machine:** Linux, an NVIDIA RTX 3070 (OpenGL 4.6), SDL3 3.4.18, GCC, CMake 4.4.
* **Versions:** PulseCoreLib `fd9b8aa`, gpuic `f6157d9`, both fetched from GitHub `main` by `FetchContent`.
* **How the numbers were taken:** `GPUIC_SCREENSHOT=x.ppm GPUIC_WAIT_MS=3000 GPUIC_STATS=1 ./spike <mode>` runs a mode
  for 3 s and prints gpuic's own per-draw averages. In that mode the loop draws as fast as it can (about 200 draws/s), so
  the figures are **CPU time per draw** (`build + layout + paint + GL submit`, where submit is the CPU's time to hand the work to
  GL, not GPU time), not a frame rate. One run per row, no repeats, one machine: read them as orders of magnitude, not
  as precise values.

## Summary

| # | Question | Answer |
|---|----------|--------|
| 1 | What does the SVG cable stand-in cost? | **3.1 ms per draw** with one moving cable and no others, **3.6 ms** with 40 static ones, **5.4 ms** with 100, once its pitfall is avoided (see below). About 30× a knob drag. Usable at 60 Hz; wasteful. |
| 2 | Do cached panel views keep a knob drag cheap with 40 panels? | **Yes: 0.12 ms per draw**, about 2 view renders per draw, 39 of 40 panels reused. |
| 3 | Wheel on a plain element? | **No.** Only scroll containers get wheel events. A hidden-scroller workaround works. |
| 4 | A 30 Hz redraw for meters? | **Yes, `cx.every(1/30)`**: 29.6 draws/s at 0.07 ms each. An idle window does 3 wakeups per 10 s. |
| 5 | A 64-step grid? | **Cheap either way:** 0.71 ms (one view) or 0.13 ms (64 cached views) per change. |
| 6 | PulseCoreLib and gpuic in one CMake tree? | **Yes**, in float, Q15 and Q31. One C++ warning workaround needed. |
| 7 | SDL3 audio next to gpuic's window? | **Yes**: PipeWire, 48 kHz, 5.3 ms with a hint. One ownership rule for clean shutdown. |

## 1. Cables: the SVG stand-in (option A)

A cable is one `gp_svg_from_path` (a Bezier) shown with `svg_img`, in an overlay above 40 cached panels. One cable
is re-created every frame (the one being dragged) and the old SVG released.

| Variant | draws | view renders per draw | build | layout | paint | GL submit | **total** |
|---------|------:|----------------------:|------:|-------:|------:|----------:|----------:|
| Reference: knob drag, no cables (item 2) | 659 | 2.0 | 0.020 | 0.023 | 0.021 | 0.054 | **0.12 ms** |
| 40 static cables, none moving | 674 | 2.0 | 0.024 | 0.019 | 0.128 | 0.106 | **0.28 ms** |
| 0 static + 1 moving, SVG released every frame | 615 | **41.6** | 0.400 | 0.380 | 2.383 | 0.477 | **3.64 ms** |
| 40 static + 1 moving, released every frame | 585 | **41.8** | 0.419 | 0.388 | 2.569 | 0.643 | **4.02 ms** |
| 100 static + 1 moving, released every frame | 542 | **41.8** | 0.436 | 0.398 | 2.799 | 0.886 | **4.52 ms** |
| 0 static + 1 moving, **released in batches** | 606 | 2.3 | 0.012 | 0.015 | 2.290 | 0.743 | **3.06 ms** |
| 40 static + 1 moving, batches | 546 | 2.4 | 0.030 | 0.026 | 2.553 | 0.996 | **3.61 ms** |
| 100 static + 1 moving, batches | 439 | 2.3 | 0.064 | 0.044 | 3.784 | 1.480 | **5.37 ms** |

**Finding: `gp_svg_release` invalidates the whole app.** It ends with `gp_app_notify(app)`, which marks every cached view in
every window dirty. Releasing the dragged cable's SVG every frame therefore makes all 40 panels render again on
every draw (41.6 view renders per draw instead of 2), and the cache is worth nothing. Releasing the old SVGs 120
at a time (one invalidation every 2 s at 60 Hz) restores it. The stand-in must do that, or never release during a drag.

What remains is the cost of rasterising the moving cable (nanosvg, into a bitmap the size of the cable's bounding box,
every frame) and uploading it: paint 2.3-3.8 ms. The batches also caused 2-3 atlas resets in 3 s (churning
rasters fill gpuic's atlas, which then clears and repaints everything). So option A holds 60 Hz here, with
room, but it spends about 30× what a knob drag does, and it is the reason the native curve is worth asking for.

**Finding: elements without handlers are transparent to the pointer, SVGs included.** A press-drag-release on a knob
under the overlay and under the cables' bounding boxes reached the knob (down/drag/up = 1/2/1, identical to the
control without the overlay). A click on a cable box with no knob under it did nothing.

**Finding: an SVG cable with a handler is NOT transparent.** With `on_mouse_down` on the 40 static cables, the same
press on the knob went to a cable (cable down = 1, knob down = 0): the handler takes its whole bounding box. So the
stand-in **cannot** support clicking a cable to select it without blocking the controls beneath. This is the
strongest argument for the native primitive's *stroke-only* hit test. Until then, a cable is removed from its jack
(drag it off, or right-click the jack), not by clicking the cable.

## 2. Knob drag with 40 cached panels

One knob changes every frame (a state change and `notify` on that panel's entity): 659 draws, 1345 view renders (about
2 per draw: the changed panel and one other view, presumably its parent), 25,428 layouts skipped and 25,668 paints reused over the run. **0.12 ms per draw.**
Cached child views work as `gui.md` hoped. The knob drag handlers (`on_mouse_down`, `on_drag`, `on_mouse_up`, with the
drag state in the panel's state and the knob index captured by `on_mouse_down`) also worked under injected input.

## 3. Wheel events

* A plain element with `gp_on_scroll`: **0 callbacks** with the wheel over it. Wheel input goes only to scroll
  containers (the header says so: "scrolls the innermost container under the pointer that can still move").
* A scroll container reports the offset: 40 px per wheel unit (3 units gave 120 px). The callback also fires once after
  layout.
* **Workaround that works:** a knob contains a hidden scroller (`overflow_y_scroll`, `hide_scrollbar`) with a tall child, kept
  at its middle (offset 1000 of 2000). Each callback's offset difference is the knob's delta; the callback then
  calls `gp_scroll_to` to re-centre. Injected 3 ticks down and 2 up gave **+40.0 net**, as it should (3×40 − 2×40).
  It costs one extra scroll container per wheel-enabled control and a first-callback "prime" step.
* The clean fix is a `gp_on_wheel` for any element; it is now a request in `gpuic-prompt.md`.

## 4. A 30 Hz redraw source

`win.ctx().every(1.0/30, ...)` with a `notify` on a meter entity: **296 draws in 10 s (29.6 Hz)**, 0.073 ms GL submit each
(the rest negligible). Process CPU over 10 s in a real window: 0.37 s with the timer vs 0.28 s idle, so about
**0.09 s per 10 s (under 1% of a core)**. An idle window did **3 wakeups and 1 draw in 10 s**. (The screenshot mode
itself draws continuously, which is why this was measured in a real window with `SPIKE_QUIT_MS`.)

## 5. The 64-step grid

One step changes every frame. **One view with all 64 columns: 0.71 ms per draw** (build 0.165, layout 0.408, paint 0.090,
submit 0.044). **64 cached child views: 0.13 ms** (about 5× less; 43,281 layouts skipped). Both are well inside a
frame; the cached version is the one to use for the playhead moving at tempo.

## 6. One CMake tree

* `FetchContent` of both libraries from GitHub `main` configures and builds, with `-DGPUIC_DEPENDENCIES=SYSTEM` and
  SDL3 found first (gpuic reused it). The configure step prints each library's resolved commit, as proposed.
  `FETCHCONTENT_SOURCE_DIR_PULSECORELIB` / `_GPUIC` point it at local checkouts (used for the Q15 and Q31 builds).
* PulseCoreLib added as a subproject (tests, examples and tools off) caused no trouble; its own flags did not leak to
  the spike's target.
* **float, Q15 and Q31** all build and the audio mode runs in each (the sample type reaches the code through the
  `PCL_SAMPLE_TYPE` definition that `pcl::pcl` exports).
* **C++ warning finding:** gpuic's headers use C compound literals (`gp_rgb`, `GP_ENTITY_NULL`), which GCC rejects under
  `-Wpedantic` in C++. Marking gpuic's include directory SYSTEM for PulseRack's targets fixes it
  (`target_include_directories(... SYSTEM ...)`); the spike builds clean with `-Wall -Wextra -Wpedantic -Werror`. The other
  fix is in gpuic (an optional request in `gpuic-prompt.md`). PulseCoreLib's headers needed nothing.

## 7. SDL3 audio next to the window

* An `Audio` entity opens `SDL_OpenAudioDeviceStream` (float, stereo) in its constructor and plays a PulseCoreLib sine
  from the stream callback in blocks of at most `PCL_MAX_BLOCK`. Result: driver **pipewire**, **48000 Hz**, 140
  callbacks and 143,360 frames in 3 s (≈ 47,800 per second), clean exit.
* **Latency.** The default device buffer is **1024 frames (21 ms)**. With `SDL_AUDIO_DEVICE_SAMPLE_FRAMES` set, 256 frames
  (5.3 ms) and 128 frames (2.7 ms) both opened and ran; so the architecture document's "about 5 ms" is real, and the app
  will set that hint. **Not measured:** underruns, or latency end to end.
* **Ownership rule found in gpuic's code and confirmed:** gpuic calls `SDL_Init(SDL_INIT_VIDEO)` and, at shutdown,
  `SDL_Quit()` (all subsystems). It runs entity destructors *before* `SDL_Quit`, so an audio engine owned by an entity
  (device closed in its destructor) shuts down cleanly. Anything that closes the device after `run()` returns would
  be too late. The spike did it that way and exited 0.
* The test tone played on the default output device (a headset here) for about 3.5 s at amplitude 0.2, once, and for
  shorter times in the buffer-size runs.

## Follow-up: gpuic `bc442de` (the native curve), checked 2026-11-09

A gpuic session implemented `gpuic-prompt.md` (commit `bc442de`: `gp_curve`, `gp_polyline`, `gp_on_wheel`, ISO C++ headers, a
guaranteed shutdown order). Checked independently, not taken from its README:

* **Its own tests, fresh build, system dependencies: 49 of 49 pass** (including `curve_test`, `wheel_test`, `shutdown_test`,
  `cpp_pedantic`, and all the older ones).
* **Its benchmark** (`cables_bench`, 40 panels and 100 cables, each cable a cached view): **0.158 ms per draw with one cable
  moving**, 0.145 with a knob turning, 0.146 for a pure repaint. With the SVG stand-in the same scene cost 5.4 ms.
* **From a consumer, the way PulseRack will use it** (a separate C++20 program, `-Wall -Wextra -Wpedantic -Werror`, gpuic's
  headers as ordinary includes, no SYSTEM workaround): it builds clean. 100 cached cable views with one moving:
  **0.20 ms per draw** (build 0.013, layout 0.017, paint 0.063, GL submit 0.108), about 2.1 view renders per draw.
* **Hit testing on the stroke only:** with a curve passing over a box, a click on the stroke went to the curve (cable = 1) and a
  click inside the curve's bounding box but off the stroke went to the box (knob = 1). The SVG route could not do that.
* **`gp_on_wheel` on a plain box:** 2 injected notches gave 2 events, `dy` sum 80 (+40 per notch down). The hidden-scroller
  workaround is no longer needed.
* **Pixels:** one screenshot of 100 random cables: smooth, even width, round caps, no gaps or notches visible. Not checked: a
  translucent cable over a panel, hover colours, curves under a transform, other GPUs.
* The scratch consumer lives in the session's scratch directory, not in this repository.

**Result: the prompt's requirements are met** (stroke-only hit test, no collateral invalidation, target ≤ 0.5 ms for 100 curves at 0.16-0.20 ms,
C++ builders, tests). The SVG stand-in and its two rules (batch the releases, no handlers on cable SVGs) are no longer needed.

## What changes in the design

* **Cables stand-in:** batch the SVG releases; no click-to-select until the native primitive exists (remove a cable from its
  jack). Written into `gui.md`.
* **gpuic prompt:** the stroke-only hit test is now a measured need; the "do not invalidate unrelated cached views" requirement
  is explicit; the wheel question became a request (`gp_on_wheel`); the redraw and SDL questions are answered and removed;
  the C++ pedantic warning is an optional request; the performance target uses the baseline above.
* **Meters and LEDs:** `cx.every(1/30)`, as designed. **Step grid:** 64 cached child views.
* **Audio:** the hint `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=256`, and the audio owner is a gpuic entity (rule above).
* **Build:** `-DGPUIC_DEPENDENCIES=SYSTEM`; gpuic's include directory as SYSTEM; the commit printed at configure.

## Not done / not measured

* No GPU timing and no vsync-paced frame rate; no repeats; one machine; Linux only.
* The knob drag was exercised with injected input (one press-move-release), and its frame cost was measured with a
  state-driven change each frame, not with a real mouse.
* Cable drawing quality (anti-aliasing, widths) was looked at in one screenshot only.
* Q15 and Q31 builds were run, but the audio was not listened to or compared to float.
* gpuic on Wayland vs X11, other GPUs, and any non-Linux platform.
