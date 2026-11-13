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
| **Oscilloscope** | in: A, B signals; no outputs | *(added later)* draws its inputs on its panel: a window of 5 ms to 1 s in 240 columns of lowest and highest value, triggered on A rising or falling through a level (or free), auto-triggered when no edge comes; frames reach the window through a lock-free triple buffer (`engine/model/scope.h`) |

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
| Unison oscillator *(added later)* | NOTE (event), PITCH, PWM, SYNC | L, R (L alone: the mono mix) | waveform, frequency, voices (1-8), detune (cents), stereo width, pitch depth, pulse width, PWM depth |
| Ladder filter *(added later)* | IN, CUTOFF | OUT | mode (LP 24/18/12/6, BP 12/24, HP 12/24), cutoff, resonance, drive, mod depth, saturate, keep bass |
| Delay *(added later)* | IN, TIME | OUT | time (up to 2 s), feedback, tone, mix, mod depth, glide, saturate |
| Reverb *(added later)* | IN | L, R (L alone: the mono sum) | algorithm and size (both remake it), decay, pre-delay, treble decay and where it starts, width, mix, modulation, freeze |
| Sampler *(added later)* | NOTE (event), PITCH | OUT | loop mode, root note, loop start/end (these remake it), start, gain, velocity, pitch depth, reverse; and its WAV file |

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
* `device <node> <name>` (after a MIDI In or MIDI CC node's parameters, and only if it has one): the MIDI input the
  module listens to, by name. The name is the rest of the line, so it may hold spaces and a `#` (`nanoKONTROL2 #2`): this
  record is recognised before a comment is cut off. See *midi.md*.
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
14. **Single-threaded** *(milestone 4 replaced this: see `architecture.md`, As built (milestone 4))*. The command ring, the atomic plan swap and the telemetry of `architecture.md` arrive with the audio device
    (milestone 4). *(Milestone 2 removed `PanelPos`: the panel layout is computed from the descriptor; see `gui.md`,
    *As built*.)*

**Findings in PulseCoreLib** (the first one has since been fixed there; the second is a note):
* **Signed overflow in the Q31 oscillator with a negative `sync_in`.** `pcl_osc_process` takes `sync_in` as "master wrap fractions" in
  [0, 1]; `osc_sync` passes the sample to `osc_comp(delta)`, which computes `INT32_MAX - x`, and a negative `x` overflows (UBSan:
  `pcl_osc.c:105`, `signed integer overflow: 2147483647 - -5422501`). The Q15 and float paths are not undefined behaviour but
  also get a meaningless fraction. It surfaced because the rack lets any signal (noise, an LFO) be patched into SYNC. The rack first
  clamped the input to [0, 1] in its oscillator wrapper (with a regression test). **Fixed in the library afterwards** (PulseCoreLib
  `fd4f2ec`): `pcl_osc_process` now treats a negative or NaN `sync_in` sample as no event and a float above 1 as 1; its regression test
  (`test_sync_in_outside_the_contract`) shows the bug was wider than first found: the float build also had undefined behaviour (a
  negative float converted to unsigned, `pcl_osc.c:245`) and the Q15 build produced different audio. With the fix on the library's
  `main`, the rack's clamp was removed (2026-11-12); its test `oscillator_sync_input_accepts_any_signal` now checks the library's
  handling.
* A float constant such as `0.99f` or `0.49f` is outside a library range written as `0.99` or `0.49` once it is converted to Q30, so
  the same range check passes in float builds and fails in fixed-point builds (the cause of decision 7).

**A flaw in my own tests, found and fixed:** several randomised tests called the random generator twice inside one function call, and
C++ does not specify the order of evaluating arguments, so GCC and clang built different random patches. They now draw first, and every
build agrees on the check count.

## As built (the later library modules: unison, ladder, delay, reverb, sampler)

* They are in `src/engine/rt/modules_fx.cpp`, one descriptor and one wrapper each, like the first set.
* **Memory.** The delay and the reverb run on caller memory: their `init` (on the UI thread, where `Engine::make_state` runs) sizes
  it for the sample rate with the library's `*_required_bytes` and allocates it, and the new `ModuleDesc::destroy` frees it when
  the state is let go (`Engine::StateDeleter`, also on the UI thread, after the audio thread has adopted a plan without the state).
  The delay's longest time is 2 s.
* **Settings fixed at init.** The reverb's size and algorithm, and the sampler's root note, loop mode and loop points (part of the
  library's sample description, which a sounding note reads), cannot be changed by a setter. Such a parameter has
  `ParamDesc::rebuild`: `Engine::set_param` makes the module again from the patch and swaps it in with the next plan (its sound
  starts over; a state that cannot be made leaves the old one playing and counts an apply failure).
* **The Sampler's file.** `Node::sample` is a path, saved as `sample <id> <path>` (the rest of the line, like `device`).
  `engine/model/wav.cpp` reads PCM at 8/16/24/32 bits and float at 32/64 (several channels mixed to one). The engine reads a file
  once into a cache (UI thread), hands the frames to the module after `init` (`sampler_load`, which copies them into memory the
  module owns), and `compile()` remakes a Sampler whose file changed (which covers undo). A file that does not load leaves the
  module silent but the patch opens; `Engine::sample_error` says why, and the panel's Sample box shows NOT LOADED.
* Tests: `tests/test_fx.cpp` (each module in a patch, the reverb's rebuild, WAV formats, the sampler's pitch, loop, one shot,
  missing file and patch record), `descriptors_every_rebuild_param_initialises_at_min_and_max`, and the Sample box in
  `ui_smoke --files`.

## As built (the drum modules: Kick, Snare / Clap, Metal)

The first of the "all-in-one" modules (worklist, *New modules*): each is its own module with a layout made for it, its sound made in
the rack (`src/engine/rt/modules_drums.cpp`) from PulseCoreLib's parts, in the new **Drum** category (peach).

* **Kick** (8 HP): Tune (30-150 Hz), Pitch sweep (0-4 octaves above the tune), Sweep time, Decay, Punch (the click), Drive, Accent
  (how much a hit's accent changes it). Inputs: TRIG (a hit when it rises through 0.5, Schmitt trigger at 0.5 / 0.25), NOTE (a hit
  per note-on; the velocity is the accent; the note number is not used), ACCENT (0..1, read at each hit, instead of the velocity),
  DECAY (-1..1, read at each hit: a quarter to four times the Decay). Output OUT.
* The body is a `pcl_osc` sine, **reset** at each hit (`pcl_osc_reset`: phase 0, and its one sample of latency cleared, so a hit does
  not start with the previous hit's last sample, and every hit starts alike). Its pitch input gets an exponential envelope (1 at the
  hit, 1/e after Sweep time) times Pitch sweep octaves. The click is `pcl_noise` through a `pcl_svf` band-pass at 3.5 kHz under a
  4 ms envelope. Drive is tanh, normalised so full scale stays full scale; it comes before the accent's level, so a soft hit stays
  soft (with it after, drive squashed the accents away).
* **The envelopes are the rack's own**, per-sample exponential decays restarted on the hit's sample: `pcl_adsr` applies a gate edge
  at its next control point and takes an off-and-on inside one control period as no edge, so it cannot retrigger a drum on the
  sample (a possible PulseCoreLib addition: a trigger input or a one-shot AD envelope). The envelope maths is float, in every sample
  type (the oscillator, noise and filter are the library's, in the build's type).
* Decay and Sweep time reach the hit that is sounding; Punch, Drive and Accent apply from the next hit.
* Tests: `tests/test_drums.cpp` (silent until hit and starting on the hit's sample, the pitch falling to the tune, decay length,
  accent from velocity and the Accent knob, a second hit identical to the first, TRIG's edge and the DECAY input). Example:
  `examples/kick_drum.rack` (rendered by `render_kick_drum`).

The three drums share their hit detection (`collect_hits`: the note-ons and any number of trigger inputs, each a Schmitt
trigger, in time order) and the accent (`accent_of`, `accent_level`).

* **Snare / Clap** (8 HP): Tune (100-400 Hz), Body (its decay), Tone (the noise's band-pass), Snap (the noise's level), Decay
  (the noise's), Clap, Spread, Accent; TRIG, NOTE, ACCENT, DECAY (the noise's decay) in. The body is a `pcl_osc` sine reset on
  each hit with a 0.6-octave drop over 8 ms; the noise is `pcl_noise` through a `pcl_svf` (its band-pass plus half its
  high-pass). **Clap** makes the noise three bursts Spread apart (each falling over 18 ms), the third going on into the tail,
  and takes the body away as it rises: 0 is a snare, 1 a clap, between them a snare with a clap's smear. A gentle tanh at the
  end glues the two.
* **Metal** (8 HP): Tune (a ratio, 0.5-2), Noise, Tone, Closed and Open decays, Accent; TRIG (closed), OPEN, CHOKE, NOTE,
  ACCENT in. Six `pcl_osc` square waves at the classic inharmonic ratios (205, 304, 370, 523, 540 and 800 Hz times Tune),
  running freely, blended with noise, through a band-pass at Tone and a high-pass at 0.6 Tone. A note hits closed, except the
  General MIDI open hat and cymbals (46, 49, 51, 52, 55, 57, 59), which hit open. Every hit starts the one envelope again, so
  a closed hit chokes an open one, as on a real hi-hat; CHOKE fades it out over 12 ms.
* Tests: the snare silent until hit, its decay and its body's pitch; the clap's three bursts at the Spread and its tail (and
  no body); Metal's closed and open by note, a closed hit choking an open one, CHOKE, accent. Example:
  `examples/drum_machine.rack` (rendered by `render_drum_machine`).

## As built (the voice modules)

Six voices in a **Voice** category (yellow), each its own module with only the controls its sound needs
(`src/engine/rt/modules_voices.cpp`). Each is a complete voice from a stream of notes: NOTE in, PITCH in (octaves), its own
envelope and amplifier, so it sounds with nothing else patched.

* **The core they share** (`VoiceCore`): monophonic, the last note held taking priority. A note played while another is held
  glides there without starting the envelope again; letting it go falls back to the note still held; a note after all were
  let go starts the envelope again on its own sample. The envelope is a linear attack to full, held while a note is, then an
  exponential release. Velocity sets the level (35 % to 100 %). STOP lets go.
* **Sync voice** (8 HP): a master `pcl_osc` at the note, never heard, hard-syncs a slave (saw or pulse; PWM in) at Ratio times
  it; the slave's pitch sweeps down from Sweep octaves above over Sweep time at each new note. Wave, Ratio, Sweep, Sweep time,
  Pulse width, Attack, Release.
* **Stack voice** (12 HP): `pcl_unison` (up to 8 saws, spread in stereo) through a driven `pcl_ladder` per side, opened by the
  envelope Contour octaves (CUTOFF in adds octaves). Voices, Detune, Spread, Cutoff, Resonance, Drive, Contour, Attack,
  Release; L and R out (L alone: the mono mix through one filter).
* **FM voice** (8 HP): two sine operators, the modulator at a Ratio of the note (x0.5 to x12: names, not numbers, for the
  patch file) bending the carrier's phase by Index, plus Env amount falling over Env decay at each new note, and Feedback on
  the modulator; INDEX in. In the rack's own float: the library's oscillators take their pitch at control rate, and phase
  modulation needs it at audio rate. From silence both phases start at 0, so every note sounds alike.
* **Air voice** (8 HP): `pcl_noise` (Colour) through a `pcl_svf` band-pass tuned to the note: high Resonance whistles, low is
  wind; the band's level is made up as it narrows. Breath adds the filter's high-pass for the air around a blown note.
* **String voice** (8 HP): Karplus-Strong, in the rack's own float. Each new note adds to a delay line a burst of noise one
  period long, softened by Brightness and combed at Position (where along the string it was plucked); the line feeds back
  through a low-pass blend (Damping) with the gain that rings for Decay seconds while the key is held and for Release after.
  Its length is fractional (linear interpolation, the filter's half sample counted), so it is in tune. Its 64 KB of line and
  burst are allocated by its init and freed by `destroy`.
* **Grain voice** (12 HP): clouds of grains of a WAV file, chosen in its Sample box as for the Sampler (`takes_sample`, now
  with `load_sample(desc, ...)` choosing the module's loader). Grains Size long under a raised-cosine window start Density
  times a second around Position (POS in adds), scattered by Spread, each at the note's pitch against the Root note, up to
  48 at once, the sum scaled by the overlap. Silent with no file. In the rack's own float: the library's sampler is one
  playhead.
* Tests (`tests/test_voices.cpp`): each voice silent until its note and then at the note's pitch (set up to sound plainly;
  pitch by autocorrelation, as a bright waveform crosses zero many times a period), the release, legato gliding without a
  restart and falling back to the held note, the string in tune within 1 % from 110 to 880 Hz and its decay, the grain
  voice playing a sine file an octave up and silent without one. Example: `examples/voices.rack` (the string, stack and FM
  voices together; rendered by `render_voices`).

**Not done** (milestone 1 stops here): the real-time protocol, any GUI, panel layout, undo, a `reset`-less "panic" for stuck notes, WAV
comparison of the three sample types (they are each exact against the library, not against each other), MSVC `/W4 /WX`.
