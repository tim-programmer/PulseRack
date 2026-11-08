# Patch engine

Status: **milestone 1 is built** (see *As built* at the end); the sections before it are the design it was built from, and
where the two differ, *As built* and the code win. The headless half of the rack: it knows nothing about windows or sound
devices. It can be driven by the GUI, by a patch file (`rack-render`), or by a test.

## Why this is not in PulseCoreLib

PulseCoreLib's rules (no heap, caller-owned state, no hidden globals) fit a library for microcontrollers.
A rack needs the opposite: nodes created and destroyed at run time, tables of descriptors, a graph that
changes while it runs. The engine therefore lives here and uses the library through its public API
only, with no change to the library. It also covers build-order step 7 ("patching layer ... for test
patches") of PulseCoreLib. **Decided:** the engine lives in PulseRack. A one-line note in PulseCoreLib's `CLAUDE.md` build order ("step 7 is
implemented in PulseRack") is suggested but **not made**; it is a change to PulseCoreLib, so it is yours.

## Vocabulary

| Term | Meaning |
|------|---------|
| **Module type** | one entry of the descriptor table: "Oscillator", "VCA", "Sequencer" |
| **Node** | an instance of a type in the patch: id, position on the rack, parameter values, state |
| **Port** | a jack: an input or an output of a node, of a *kind* (below) |
| **Cable** | one output port connected to one input port |
| **Patch** | the nodes and cables, the transport settings and the rack layout |
| **Plan** | the patch compiled for the audio thread: sorted steps, buffers, resolved pointers |

## Signal kinds

Two kinds, matching what the library actually exchanges:

* **SIGNAL**: a block of `pcl_sample_t`. Audio and control voltages are the same thing (the library
  calls control inputs "normalized [-1, 1]" and audio nominally [-1, 1]); the jack's *colour* is a hint,
  not a type. A gate is a signal that rises through 0.5, as the ADSR and the sample-and-hold already
  expect.
* **EVENT**: a sorted block of `pcl_event_t` (`NOTE_ON`, `NOTE_OFF`, `CLOCK`, `START`, `STOP`) with
  sample offsets, produced by the keyboard, the clock, the arpeggiator and the sequencer.

A cable joins an output and an input of the **same kind**; the GUI refuses anything else and says why.
There is deliberately no automatic conversion. Where a conversion is useful it is a module you can see
(see *Glue modules*). **An input takes one cable** (decided); fan-out from an output is free, because consumers
only read. Summing is done by a Mixer module.

## Descriptors: one table drives everything

Each module type is described once, as `constexpr` data (C++20, so the tables are checked at compile time and
live in flash-like read-only memory):

```cpp
enum class Kind : uint8_t { Signal, Event };
enum class Scale : uint8_t { Linear, Log, Exp, Steps, Enum, Toggle };
enum class Control : uint8_t { Knob, Slider, Switch, Button, Selector };

struct PortDesc  { std::string_view id, name; Kind kind; bool is_output; PanelPos pos; };

struct ParamDesc {
    std::string_view id, name, unit;           // "cutoff", "Cutoff", "Hz"
    float min, max, def;                       // natural units
    Scale scale;
    std::span<const std::string_view> labels;  // Enum and Steps: names of the choices
    Control control;
    PanelPos pos;
    ApplyFn apply;                             // natural-unit value -> the library's setter call
};

struct ModuleDesc {
    std::string_view id;                       // "osc", "adsr": the patch file's name, never changes
    std::string_view name, category;           // "Oscillator", "Source"
    uint8_t width_hp;                          // panel width in rack units; the height is fixed
    size_t  state_size, state_align;           // the library struct plus the wrapper's own state
    std::span<const PortDesc>  ports;
    std::span<const ParamDesc> params;
    InitFn init;  ResetFn reset;  ProcessFn process;   // plain function pointers: audio-thread safe
};
```

The browser, the panel renderer, the patch file's parameter names, the engine's clamping and the
tests all read this table. Adding a module is **one entry and one wrapper function**; no UI code.
`apply` is the only place a library setter is named, for example for the SVF cutoff:
`pcl_svf_set_cutoff(&n->svf, PCL_HZ(value))`. (`PCL_HZ`, `PCL_COEF`, `PCL_OCT` are the library's
conversion macros and give the right type in all three builds.) The model stores natural-unit
`float`s; a value from the file or the UI is clamped to `[min, max]` before it is sent, so a setter is
never expected to fail, and a failure is counted and ignored rather than trusted.

Knob mapping from a 0..1 position to a value is the descriptor's `scale`: LOG for frequency and times
(so one turn covers 20 Hz to 20 kHz usefully), LINEAR otherwise.

## Wrappers

A wrapper is the `process` function of one type: it receives the node, its resolved input and output
pointers and the block length, and calls the library.

```cpp
struct NodeIo {
    std::span<const pcl_sample_t *const> sig_in;   // an entry is nullptr when the input has no cable
    std::span<pcl_sample_t *const>       sig_out;  // nullptr when nothing is connected: "don't compute it"
    std::span<const EventBuf *const>     evt_in;   // sorted; empty when unconnected
    std::span<EventBuf *const>           evt_out;
    size_t frames;
};
```

The library's "NULL means unmodulated" convention maps straight onto an unconnected jack, so most
wrappers are a few lines. Three need more:

* **Oscillator NOTE input.** The oscillator is tuned by a frequency setter, not by a signal. The
  wrapper splits the block at each `NOTE_ON` offset: process up to the event, set the frequency
  (`440 * 2^((note-69)/12)`, through `pcl_math.h`), process on. This is what `tools/render.c`'s `events`
  patch already does, so it is a known quantity, and it makes a note sample-accurate. The PITCH
  signal input (`pitch_mod`, octaves per unit via the *Pitch depth* knob) is separate and is for vibrato.
* **Event modules.** The clock, arpeggiator and sequencer write into an event buffer that the plan sizes
  once: `pcl_clock_max_events(frames)` for the clock, and the capacity formulas in their design notes
  (`2 × clock events + key events` for the arp and sequencer). Dropped events are counted by the library; the
  wrapper copies that count into telemetry and the panel shows it as an LED.
* **Transport.** *Play/Stop* is a command from the UI to the clock (`pcl_clock_start/stop`), applied on
  the audio thread. The sequencer's and arpeggiator's `START`/`STOP` come from the clock's events, which is
  how the library designed them.

## Glue modules (written in this repository, not in the library)

The library has no mixer, no utility for physical input and no output; those are the rack's own:

| Module | Ports | Notes |
|--------|-------|-------|
| **Keyboard** | out: NOTE events | the computer keyboard as a piano plus an on-screen strip; octave and velocity buttons |
| **Audio Output** | in: L, R signals | the sink; mono (only L connected) plays on both channels; the only module that knows the device |
| **Mixer** | in: 4 signals + level knobs; out: 1 | saturating sum written with the `pcl_types.h` helpers, so it works in all three builds |
| **Attenuverter** | in, out; scale (−1..1) and offset | the usual CV helper; makes a modulation depth visible and patchable |
| **Note → Gate** | in: NOTE events; out: GATE signal | for modules that take a gate signal and not events (the ADSR takes both) |

The Mixer and the Attenuverter are small enough to be added to PulseCoreLib later if you want them
there; they are here because they have no design note yet.

## First catalogue (the modules that exist today)

Ports come from the library headers; parameters are the setters. "Mod" inputs are the library's
`*_mod` arguments.

| Module | Inputs | Outputs | Parameters |
|--------|--------|---------|------------|
| Oscillator | NOTE (event), PITCH, PWM, SYNC | OUT, SYNC OUT | waveform, frequency, pitch depth, pulse width, PWM depth |
| Noise | | OUT | colour (white/pink/brown), seed |
| Sample & Hold | TRIG (signal), CLOCK (event) | OUT | rate, slew, seed |
| LFO | RATE | OUT | shape, frequency, rate depth, pulse width, unipolar |
| ADSR | GATE (signal), NOTE (event) | OUT | attack, decay, sustain, release, three curves, run-through |
| VCA | IN, GAIN | OUT | gain |
| State-variable filter | IN, CUTOFF | LP, BP, HP | cutoff, resonance, mod depth, saturate |
| Biquad filter | IN, CUTOFF | OUT | type, cutoff, Q, gain, mod depth |
| Clock | | CLOCK (event) | bpm, ppqn, play/stop |
| Arpeggiator | IN (event: keys + clock) | OUT (event) | mode, octaves, division, gate, latch |
| Sequencer | IN (event: clock) | OUT (event) | length, division, gate, swing, direction, transpose, 64 steps |

Notes on the table:

* The arpeggiator and the sequencer each take **one** event input that carries keys *and* clock pulses (as
  the library does). A **Merge** utility (events, two inputs, sorted by offset and by the library's
  same-offset order) is needed to feed a clock and a keyboard into an arpeggiator. It is the engine's
  `merge_events`, which `tools/render.c` already contains; it joins the glue modules above.
* The SVF has three outputs but the library computes only the ones that are not NULL, so an unconnected output
  costs nothing.
* The sequencer's 64 steps (note, velocity, gate, tie) are not knobs; they are edited in a step grid and
  stored in the patch as a list (see *gui.md*).

## Compiling the plan

1. **Validate** the patch: every cable joins an output to an input of the same kind, no input has two cables,
   no cycle.
2. **Order**: a topological sort of the nodes; the tie-break is the node id so that the order is
   deterministic and the same patch always renders the same samples.
3. **Buffers**: one `PCL_MAX_BLOCK`-sample buffer per *connected* output. Unconnected outputs get no buffer
   (the wrapper passes NULL). Buffers whose last reader has run are reused, which keeps a patch of dozens of
   modules within a few kilobytes of working memory. Event outputs get an event buffer sized as above.
4. **Resolve** every input to a pointer into the right buffer, or NULL, and emit the step list.

The plan is immutable once published; an edit builds a new one (see the threads section of
*architecture.md*). Compiling is cheap and happens on the UI thread, never on the audio thread.

**Cycles.** A cable that would close a loop is **refused** at the moment it is dragged, with a message.
(Decided.) Feedback patches are musically useful (a filter in a delay loop, FM) but they need a
defined delay, and the honest choice for a first version is to not guess. The alternative is a dedicated
*Feedback* module that is a one-block delay and is the only legal way to close a loop; it is easy to add later
without changing anything else.

**Block size.** The engine renders in blocks of `min(PCL_MAX_BLOCK, what remains)`. The audio device's callback
size never reaches a module.

## The patch file

A text file, one record per line, ids are small integers, no third-party parser (so no new dependency):

```
pulserack 1
node 1 clock    x=12 y=0
param 1 bpm 120
node 2 seq      x=40 y=0
param 2 division 6
step 2 0 note=48 vel=100 gate=0 tie=0
node 3 osc      x=80 y=0
param 3 waveform saw
node 4 out      x=140 y=0
cable 1.clock -> 2.in
cable 2.out   -> 3.note
cable 3.out   -> 4.l
```

* The first line carries the format version; an unknown version is refused, an unknown type or port is a
  warning and the node/cable is skipped (a patch from a newer rack still opens).
* Names are the descriptors' `id`s and never change; positions are rack units.
* Parameter values are natural units, `printf("%.9g")` so that a round trip is exact.
* A file is rejected, not partly applied, if it has a syntax error. Limits are enforced while reading (maximum
  nodes, cables and line length) so a hostile file cannot make the loader allocate without bound.

## Tests

Listed in *architecture.md*. Specific to this document: every descriptor is checked for a valid range
(`min < max`, `def` inside it), unique ids and a `pos` inside the panel; every `apply` is called with `min`,
`def` and `max` and must return success; and **the whole catalogue is rendered with default parameters for a
second and checked for NaN/Inf and for silence where silence is wrong** (so a wrapper that forgets to connect
something is caught without a human listening).

## Open questions

*Questions 1 and 2 were answered (loops are refused; an input takes one cable). Questions 3 to 5 were settled while building,
with the recommended answers: two ADSR jacks, the glue modules in this repository, no polyphony; see *As built*, decisions 1 to 3.*

1. ~~Cycles~~: **refuse** (decided).
2. ~~One cable per input~~: **one** (decided).
3. **GATE as a signal and as events** both appear on the ADSR; is it clearer to show one jack (accepting either) or two?
   The engine can do either; two jacks keep the kind rule simple.
4. **Do you want the Mixer, Attenuverter and Merge in PulseCoreLib** (with design notes and tests in three builds), or in
   this repository only?
5. **Polyphony.** The library's voices are monophonic; one Keyboard→Oscillator chain plays one note. Polyphony would be
   several copies of a sub-patch with a voice allocator, which is a real feature of its own and **not** in this plan.

## As built (milestone 1)

**Code:** `src/engine/` (`model/descriptor.h`, `registry`, `patch`, `plan`, `patchfile`; `rt/modules_lib.cpp`, `rt/modules_glue.cpp`,
`rt/sample_ops.h`; `engine.h/.cpp`), `src/render_main.cpp` (`rack-render`), `tests/`, `examples/`. C++20, `-Wall -Wextra -Wpedantic -Werror`
with GCC and clang. PulseCoreLib is fetched with `FetchContent` (branch `main`); no gpuic is needed yet.

**What exists.** All 17 module types: the 11 from PulseCoreLib (oscillator, noise, sample & hold, LFO, ADSR, VCA, state-variable filter,
biquad, clock, arpeggiator, sequencer) and the 6 glue modules (keyboard, audio output, mixer, attenuverter, note to gate, merge).
`Patch` (the rules: kinds, one cable per input, no loops, clamped parameters), `Plan` (topological order with ties by id, reused signal
buffers, event buffers sized by each producer's rule), `Engine` (live module states that survive edits, `render` in any block size,
`reset`, live `set_param`/`set_step`/`key`, a key script), the patch file reader and writer, and `rack-render patch.rack out.wav [seconds]`.

**Tests** (`tests/`, 70 cases, 26,445 checks, the same in float, Q15 and Q31, with GCC and clang):
* descriptors: ids, ranges, every control accepts its minimum, default and maximum at three sample rates, the wrappers' jack indexes
  match the descriptors, capacity rules;
* the patch rules (every refusal, loops, boundaries, removal) and the plan (order, buffer reuse and lifetime, random graphs);
* **bit-exact reference tests**: five patches (osc-vca-out; noise-LFO-filter; clock-sequencer-ADSR-osc; keys-merge-arpeggiator-voice;
  S&H-attenuverter-osc-biquad) rendered by the engine equal, sample for sample, the same modules called directly through PulseCoreLib;
* the glue modules, sample-accurate gates and note tuning, key scripts across block edges, the keyboard queue (out-of-order and equal-time
  events, compaction, scripts longer than the queue, the per-block capacity), worst-case event capacity, NaN and infinity at the output;
* the engine: block-size independence (1, 7, 64, 255, 256, 257, 1000, 4801), reset, edits that keep running modules running, other
  sample rates, 80 random patches with extreme settings that must stay finite;
* the patch file: a golden text, round trips with identical audio, 30 kinds of bad file, limits, warnings, 3000 randomly damaged files;
* the three example patches rendered by the real `rack-render` binary.
Also clean under AddressSanitizer + UBSan in all three sample types, and a GCC Release build.

**Mutation testing** of the engine's own sources (`patch`, `plan`, `patchfile`, `engine`, both module files; 412 operator and statement mutants,
115 of which do not compile): the first pass left 64 survivors and showed gaps in the tests, which are now closed (live step changes, key
events already in the past, queue overflow and compaction, boundary indexes, error line numbers, release by velocity-0 note-on or STOP, a
cable-order golden, notes with pitch, PWM and sync together, and more). The second pass: 267 killed, 29 survived. Of those, 8 are
off-by-one memory writes that AddressSanitizer kills (checked: each of the 8 aborts the suite under ASan), 2 sites were then killed by a further test, and
**19 are equivalent**: comparator ties between unique keys, reads that stay inside a buffer, a guard against a loop that `Patch` makes
impossible, a cable limit that the 1,024-module limit makes unreachable, a toggle compared with 0.5 that is only ever 0 or 1, and clamps whose
boundary value maps to itself.

**Decisions taken while building (mine, for your review).** These are the small open questions at the end of this document; I used the
recommended answer for each and chose the rest. Each is cheap to change.
1. The ADSR has **two jacks**, GATE (signal) and NOTE (event). *(recommended)*
2. The mixer, attenuverter, merge, note to gate, keyboard and audio output are **in this repository**, not PulseCoreLib. *(recommended)*
3. **No polyphony.** *(recommended)*
4. **Key scripts in the patch file** (`key <node> <seconds> on|off <note> [velocity]`) and a Keyboard module that sends them at their sample.
   Not in the earlier design; without it a patch cannot play itself headless. A live key press (`Engine::key`) is the same queue, heard at
   offset 0 of the next block.
5. **The clock starts running** (`run` defaults to 1); the GUI's Play button will set it.
6. Defaults: audio output level 0.8; mixer levels 0.5 (four inputs); merge has two inputs and **A goes first at the same sample**;
   note to gate is monophonic, the last note wins, STOP releases it.
7. **Control ranges are narrower than the library's** where a GUI wants it (envelope times 1 to 10,000 ms, oscillator 20 to 8000 Hz, filter
   20 to 20,000 Hz, LFO 0.01 to 100 Hz) and **a little inside the library's limits** for pulse width (0.02 to 0.98) and PWM depth (0 to 0.48):
   a float like `0.99f` is a hair above the library's `0.99` once converted to Q30, and the fixed-point builds refuse it.
8. Arpeggiator and sequencer have **no seed control** (the library has no setter for it); random modes use the default seed.
9. **`Engine::reset` re-creates every module from the patch's settings** rather than calling the library's `reset`: a library reset keeps
   what was set since the module was made (an oscillator's frequency after notes), so it is not the same as a fresh start.
10. In the patch file an **enumerated choice is written by its name with a space as `_`** (`param 3 waveform saw`, `shape random_step`);
    numbers outside a control's range are clamped on loading, with a warning.
11. Unconnected **required outputs** are written to a scratch buffer and unconnected **required inputs** read zeros, so a module
    always runs and keeps its state; optional outputs stay NULL ("don't compute").
12. The engine **clamps the output to ±1 and silences and counts any NaN or infinity** (`nonfinite_samples()`, which the tests keep at 0).
13. Event output capacities: clock 64 (53 is the worst case at the fastest tempo and lowest sample rate); arpeggiator 32 per input event
    (a chord's releases and starts), capped at 4096; sequencer 2 per input event plus 2; merge the sum of its inputs.
14. **Single-threaded.** The command ring, the atomic plan swap and the telemetry of `architecture.md` arrive with the audio device
    (milestone 4). `PanelPos` exists in the descriptors but is zero until milestone 2.

**Findings in PulseCoreLib** (the first one has since been fixed there, uncommitted; the second is a note):
* **Signed overflow in the Q31 oscillator with a negative `sync_in`.** `pcl_osc_process` takes `sync_in` as "master wrap fractions" in
  [0, 1]; `osc_sync` passes the sample to `osc_comp(delta)`, which computes `INT32_MAX - x`, and a negative `x` overflows (UBSan:
  `pcl_osc.c:105`, `signed integer overflow: 2147483647 - -5422501`). The Q15 and float paths are not undefined behaviour but
  also get a meaningless fraction. It surfaced because the rack lets any signal (noise, an LFO) be patched into SYNC. The rack now clamps
  the input to [0, 1] in its oscillator wrapper (and has a regression test). **Fixed in the library afterwards**: `pcl_osc_process` now treats a
  negative or NaN `sync_in` sample as no event and a float above 1 as 1; its regression test (`test_sync_in_outside_the_contract`) shows the
  bug was wider than first found: the float build also had undefined behaviour (a negative float converted to unsigned, `pcl_osc.c:245`)
  and the Q15 build produced different audio. The rack's own clamp can go once that fix is on the library's `main`.
* A float constant such as `0.99f` or `0.49f` is outside a library range written as `0.99` or `0.49` once it is converted to Q30, so
  the same range check passes in float builds and fails in fixed-point builds (the cause of decision 7).

**A flaw in my own tests, found and fixed:** several randomised tests called the random generator twice inside one function call, and
C++ does not specify the order of evaluating arguments, so GCC and clang built different random patches. They now draw first, and every
build agrees on the check count.

**Not done** (milestone 1 stops here): the real-time protocol, any GUI, panel layout, undo, a `reset`-less "panic" for stuck notes, WAV
comparison of the three sample types (they are each exact against the library, not against each other), MSVC `/W4 /WX`.
