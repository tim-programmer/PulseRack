# Architecture

Status: **reviewed; milestones 0 to 5 are built** (the table at the end). Where something has not been checked against
the real code or library it says so.

## Repository layout

```
PulseRack/
  CMakeLists.txt
  README.md
  docs/                 these notes
  src/
    engine/
      rt/               audio-thread code: wrappers, the compiled plan, the rings (restricted subset, below)
      model/            UI-thread code: descriptors, patch model, plan compiler, patch file, undo
    audio/              SDL3 audio output and the telemetry
    ui/                 gpuic app: window, browser, rack view, panels, cables
    main.cpp            the application
    render_main.cpp     headless renderer: a patch file in, a WAV file out
  tests/                engine tests (no display needed), UI smoke tests (display needed)
```

`src/engine` depends on PulseCoreLib only (and the C++ standard library). `src/ui` depends on the engine and gpuic. `src/audio`
depends on the engine and SDL3. The engine never includes a gpuic or SDL header, so every engine
test runs headless.

## Dependencies

| Library | Used for | How it is found |
|---------|----------|-----------------|
| PulseCoreLib (`pcl::pcl`) | all DSP and the event modules | `FetchContent`, branch `main` (below) |
| gpuic (`gpuic::gpuic`) | window, layout, input, drawing | `FetchContent`, branch `main` (below) |
| SDL3 | audio output only (gpuic uses it for windowing) | `find_package(SDL3)` first, so gpuic reuses the same target |

* **No new third-party code.** SDL3, FreeType, HarfBuzz and FriBidi are gpuic's own dependencies, and this
  machine already has them (SDL3 3.4.18). gpuic's default policy downloads what is missing; this repository
  will pass `-DGPUIC_DEPENDENCIES=SYSTEM` by default so a build never touches the network unasked
  (an option switches it to `AUTO`).
* **Both libraries are fetched from GitHub with `FetchContent`, branch `main`** (decided, for now):
  `https://github.com/tim-programmer/PulseCoreLib` and `https://github.com/tim-programmer/gpuic`. Both are
  Apache-2.0. The consequences, accepted:
  * a configure can pick up a newer `main` of either library, so a PulseRack build is only as reproducible as
    the day it was made. The configure step prints the commit each library resolved to, so a build log says what
    it used; going to a pinned hash or tag later is a one-word change of `GIT_TAG` in one file;
  * `FETCHCONTENT_UPDATES_DISCONNECTED=ON` stops CMake re-checking the network on every configure once a tree is
    populated (it will be the default in the development preset);
  * **for working on all three repositories at once**, use CMake's own `FETCHCONTENT_SOURCE_DIR_PULSECORELIB` and
    `FETCHCONTENT_SOURCE_DIR_GPUIC` to point at local checkouts; no custom variables are needed;
  * a breaking change on `main` of either library breaks PulseRack's next fresh configure; the answer is the
    pin above, and it is yours to pull when that gets annoying.
  PulseCoreLib's `third_party/` SDK folder (ignored by its `.gitignore`) is never needed: the board firmware is
  not part of this build.
* PulseCoreLib is added as a subproject with its tests, tools and examples switched off (`PCL_BUILD_TESTS`, `_EXAMPLES`,
  `_TOOLS` = OFF). **Checked by the spike** (`spike.md`): it configures and builds that way through `FetchContent`, together
  with gpuic, in float, Q15 and Q31, and its flags do not leak into PulseRack's targets.
* **gpuic's headers and `-Wpedantic`.** gpuic `f6157d9` used C compound literals that GCC rejects under `-Wpedantic` in C++; gpuic
  `bc442de` wrote them with `GP_LIT`, and a consumer builds clean with `-Wall -Wextra -Wpedantic -Werror` and gpuic's headers as
  ordinary includes (checked). No SYSTEM workaround is needed.
* **Sample type.** `PULSERACK_SAMPLE_TYPE` = `FLOAT` | `Q15` | `Q31` is forwarded to `PCL_SAMPLE_TYPE`, and
  the whole rack is one build of one type (`pcl::pcl` exports the definition, so the engine sees the right
  `pcl_sample_t`). Float is the default. The Q15 and Q31 builds are kept working so that the fixed-point
  paths of every module can be *heard* and compared, which is a feature of the rack as well as a test.
* **Language: C++20** (decided). PulseRack is the one place in the three repositories where C++ is used; PulseCoreLib
  stays C99 and gpuic stays C with its optional header-only C++ layer (`gpuic.hpp`, which needs C++20 and
  gives lambdas for handlers). Flags: `-std=c++20 -Wall -Wextra -Wpedantic -Werror` (MSVC `/W4 /WX`).
  * **PulseCoreLib's headers work from C++.** Checked today: `#include "pcl/pcl.h"` plus a
    `pcl_svf_config_t` and `PCL_HZ(440.0)` compile cleanly under `g++ -std=c++20 -Wall -Wextra -Wpedantic
    -Werror` for FLOAT, Q15 and Q31 (the headers use `extern "C"` through `PCL_BEGIN_DECLS`). That covers the
    umbrella header and one macro; every wrapper will exercise the rest, and the first build will tell.
  * **gpuic's C++ layer**: since gpuic `771626e` the `on_drag` and `on_mouse_up` lambdas may capture (the knobs capture their
    parameter's index); `on_layout` and `on_ime` still cannot. Captures live in the frame's arena and are never destroyed, so they
    are numbers, pointers, handles or `string_view`s into static data, never a `std::string` (gpuic refuses those at compile time).
    A knob keeps its drag state in its panel's view state, which the handler receives.
  * **The audio thread runs a restricted subset** of C++ (`engine/rt/`): plain structs, function pointers, spans,
    `std::atomic`; no exceptions, no `new`/`delete`, no growing containers, no `std::function`, no locks,
    no virtual calls into UI code. `engine/model/` and `ui/` use the full language. The split is a directory
    boundary and a review rule, backed by the TSan test below; enforcing it with `-fno-exceptions` on
    `rt/` is a build detail for milestone 1.
* **Tests use a small in-repository header** (check macros and a runner, about 100 lines, like PulseCoreLib's
  `pcl_test.h`), not GoogleTest or Catch2: a new test dependency would need your approval, and nothing here
  needs more.

## Audio

* SDL3's audio-stream API opens the default playback device as 32-bit float, stereo, at whatever rate the
  device offers (48 kHz expected). The device rate becomes the engine's sample rate; changing device
  rebuilds the plan and re-initialises the modules at the new rate. **Checked by the spike:** `SDL_OpenAudioDeviceStream`
  with an `SDL_AudioStreamCallback`, `SDL_InitSubSystem(SDL_INIT_AUDIO)` after gpuic's video init, and a PulseCoreLib
  oscillator rendering from the callback all work on PipeWire at 48 kHz (3.4.18).
* The stream callback asks for a number of frames that is not ours to choose, so the engine renders in
  blocks of at most `PCL_MAX_BLOCK` (256) frames into a small ring and the callback drains it. A block that is
  not a whole multiple is fine: the library's modules accept any frame count from 1 up.
* The *Audio Output* module converts `pcl_sample_t` to float: identity in the float build, a divide by 2^15
  or 2^31 in the others, then a hard clamp to ±1. This is the only place the host format appears.
* `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=256` (the SDL hint, set by the application before opening the device) gives a
  **256-frame (5.3 ms) device buffer**, measured; 128 frames also opened. Without it the default was 1024 (21 ms). Underruns
  were not measured. The real latency is whatever the device grants and is shown in the toolbar.
* **Ownership at shutdown.** gpuic calls `SDL_Quit()` when `run()` finishes, after dropping the entities. So the audio device
  is owned by a gpuic **entity** and closed in its destructor (as the spike did); closing it after `run()` returns would be
  too late. gpuic `bc442de` **guarantees** that order and tests it (`shutdown_test`: SDL is still initialised in entity drop
  functions; do not touch windows or other entities there).
* **Offline rendering uses the same engine.** `rack-render patch.rack out.wav seconds` is the headless twin of the
  application, and the basis of the engine's golden tests.

## Threads

Two threads matter: the **UI thread** (gpuic's main loop) and the **audio thread** (SDL's callback).

1. **The audio thread owns every module.** PulseCoreLib setters are "control-thread calls" and are not
   safe against a concurrent `process`. So *all* calls into a module, setters and `process` alike,
   happen on the audio thread. The UI never touches a module's state.
2. **UI → audio: a fixed-size command ring** (single producer, single consumer, `std::atomic` indices, trivially copyable
   commands: set parameter, gate/note on and off from the on-screen or computer keyboard, reset, start/stop
   the transport). The audio thread drains it at the start of every block. Notes from the keyboard take effect
   at offset 0 of the next block (one block of latency; sample-accurate live input is not worth the complexity).
3. **Editing the graph: a compiled plan, swapped atomically.** The UI thread builds a new *plan* (the sorted
   list of steps and their buffers, see the engine document) from the patch model and publishes it with an
   atomic pointer exchange. The audio thread adopts it at a block boundary and hands the old one back through a
   retire slot; the UI thread frees it later. Module *state* belongs to the node in the model, not to the plan,
   so a plan swap keeps oscillator phases, filter memory and envelopes running. A node is destroyed only
   after a plan that does not reference it has been acknowledged.
4. **Audio → UI: telemetry.** Meter levels, gate LEDs, the current step of a sequencer and the clock LED are
   written by the audio thread into a small array of atomics (floats stored as `uint32_t` bit patterns)
   and read by the UI when it draws.
5. The audio thread never locks, allocates, calls `printf` or touches a gpuic object. This is the library's
   real-time rule applied to the rack's own code.

**Redraw.** gpuic wakes the loop only when something changes, and meters and LEDs need about 30 Hz while audio runs.
`ctx.every(1.0/30, ...)` does it: measured at 29.6 draws/s, 0.07 ms per draw, under 1% of a core, and 3 wakeups per
10 s when idle (`spike.md`).

## Testing

* **Engine tests, headless, in all three sample types** (the same rule as PulseCoreLib): topological order,
  cycle rejection, port-type checking, parameter clamping against the descriptors, patch file round-trip
  and rejection of damaged files.
* **Bit-exact reference tests.** A patch rendered through the engine must equal the same modules called
  directly through the library's API, sample for sample (this proves the wrappers add nothing and take
  nothing away, including block splitting at event offsets).
* **Protocol test under ThreadSanitizer**: a fake audio thread drains the ring and swaps plans while a UI thread
  hammers parameter changes and edits. (gpuic's checkout already has `build-tsan`/`build-asan` trees, so
  those sanitizers work with it.)
* **UI smoke tests** with gpuic's headless hooks (`GPUIC_SCREENSHOT`, `GPUIC_CLICK`, `GPUIC_DRAG`, `GPUIC_KEYS`):
  open a window, add a module, drag a knob, drag a cable, check the PPM is not blank and that the model
  changed. These need a display, like gpuic's own window tests, and are skipped without one.
* No generated audio is committed except small golden files, as in PulseCoreLib.

## As built (milestone 4: sound)

**Code:** `src/engine/engine.{h,cpp}` (the protocol), `src/engine/rt/spsc.h` (the ring), `Plan::run`'s activity output,
`src/audio/audio_out.{h,cpp}` (the SDL3 device), and the window's side in `src/ui/rack_ui.cpp`.

**As designed above**, with these choices:
1. **One engine, two modes.** With no audio thread attached (`rack-render`, the engine tests, the window with audio off) the UI
   thread is also the audio side: each edit drains the ring and adopts the new plan at once. So every existing test, the bit-exact
   reference tests included, runs the same code as live audio, and a test checks that an attached engine whose `render()` runs
   between the same edits gives the **same samples, bit for bit**.
2. **No library `init` on the audio thread.** A compile, a reset or a new sample rate makes new module states on the UI thread
   (on memory nobody else can see yet) and publishes them with the plan. A reset is therefore a plan swap that also sets time to 0,
   not a command.
3. **The order at the start of a block** is: take the pending plan, apply every command in the ring, adopt the plan, acknowledge
   its generation. A removed module's state is freed by the UI thread only after that acknowledgement; no command still to come
   can name it, because the UI sends none for a node that is gone. Two plans published before the audio thread takes one: the first
   is freed unseen. If the ring of plans handed back (256) is full, adopting waits a block.
4. **A full command ring loses the command**, counts it (`Telemetry::commands_lost`) and never blocks; the model keeps the value,
   so the next change of that control sends it again. The ring holds 4096.
5. **The engine runs at the device's own rate** (`set_sample_rate` makes every module again, time 0), so SDL never resamples.
   Measured here: PipeWire, 48 kHz, 256 frames (5.3 ms), as in the spike.
6. **Telemetry:** the output's peak, each module's activity (the largest sample of its *connected* signal outputs, or 1 if it sent
   an event), each event module's dropped-event count, and the callback's load (time spent over the time the frames cover,
   smoothed). The window takes them at 30 Hz and re-renders only the panels whose LED or meter changed.
7. **The device is closed through the lock SDL documents**: `SDL_SetAudioStreamGetCallback(stream, NULL, NULL)` returns only
   after a running callback has finished; then the stream is destroyed and the engine detached. The `Editor` (an entity) owns the
   device after the engine, so it closes first, and both go before gpuic's `SDL_Quit`.

**Tests:** `test_realtime.cpp` (6 cases): attached equals detached bit for bit (keys, parameters, a module added and one removed
mid-render, a reset); edits wait for the audio thread and states outlive the plan that used them; a full ring; telemetry; a new
sample rate equals a fresh engine at that rate; and a **stress test** in which a real audio thread renders while the UI thread makes
about 30,000 random edits (modules, cables, parameters, keys, resets). Clean under **ThreadSanitizer** and under **AddressSanitizer
+ UBSan** (leaks on). `ui_sound` (22 checks) runs the window with audio on through SDL's dummy driver: the callback renders at the
device's rate, LEDs and meters light and fall, the piano's notes reach a Keyboard module (and not while the search box has the
keyboard), the toolbar switches audio off and on. **Not measured:** underruns, latency end to end, other devices or platforms; the
real device was opened (PipeWire) but no sound was listened to in these tests.

## Milestones (proposed; one at a time, you say which is next)

| # | Item | Done when |
|---|------|-----------|
| 0 | **gpuic spike** (throw-away) | **done**, see [spike.md](spike.md): cable stand-in cost, cached-panel knob drag, wheel, 30 Hz redraw, 64-step grid, one CMake tree in three sample types, SDL audio next to the window |
| – | **gpuic curve primitive** (in the gpuic repository, from `gpuic-prompt.md`) | **done and checked** (gpuic `bc442de`, `spike.md` *Follow-up*; re-checked at `b5e2679`, `771626e` and `da89032`, no regressions); milestone 3 uses `gp_curve` directly |
| 1 | Engine + `rack-render` | **built**: the headless engine passes its tests in float, Q15 and Q31; all 11 library modules and 6 glue modules are wrapped; see `patch-engine.md`, *As built* |
| 2 | Window without cables | **built**: browser, rack with draggable panels, knobs, sliders, selectors, switches working on the engine; see `gui.md`, *As built* |
| 3 | Cables | **built**: jacks, dragging cables (lit targets, reasons for refusals), moving and deleting them, the engine updated live; see `gui.md`, *As built (milestone 3)* |
| 4 | Sound | **built**: SDL3 output on its own thread, the computer keyboard as a piano, activity LEDs and level meters; see *As built (milestone 4)* above and `gui.md` |
| 5 | Save and load | **built**: New, Open, Save, Save as, the factory patches, unsaved changes asked about and kept at close; see `gui.md`, *As built (milestone 5)* |
| 6 | Polish and the later modules | search/filter, undo, delay/sampler/reverb as PulseCoreLib gains them |

Milestones 1 and 0 are independent. If a spike result is bad (cables, mostly), it changes milestone 3
and nothing earlier.
