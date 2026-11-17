# PulseRack and REAPER

Status: *Level 1* uses only what the rack already has (MIDI In, MIDI Clock, MIDI Out, SDL3 audio, `rack-render`). Level 2 is
a proposal. *Level 3* is built (2026-11-18): `pulserack.clap` with two plugins, **PulseRack** (an instrument) and **PulseRack FX**
(an effect), the rack's window as their editor, and `PulseRack.vst3` wrapping the same (*As built (the CLAP plugin)*, at the
end). Where
something has not been tried with a real REAPER it says so. REAPER's own menus are named as they are in REAPER 7 on Linux.

## What the two programs would exchange

| What | Direction | Today |
|------|-----------|-------|
| Notes (a MIDI track playing the rack) | REAPER → rack | yes, through a MIDI port into a **MIDI In** |
| Tempo and transport (start, stop, continue) | REAPER → rack | yes, MIDI clock into a **MIDI Clock**; no song position |
| Notes the rack plays (sequencer, arp) | rack → REAPER | yes, a **MIDI Out** into a port REAPER records from |
| The rack's sound | rack → REAPER | yes, through PipeWire's graph (the rack is an ordinary audio client) |
| REAPER's sound into the rack (a track to filter, to sample) | REAPER → rack | no: the rack has no audio input (*Audio In* is on the work list) |
| Knob automation | REAPER → rack | partly: CC from a MIDI track turns a learned knob (MIDI learn, or a **MIDI CC** module) |
| The patch saved with the REAPER project | both | no: only as a separate `.rack` file beside the project |

## Level 1: two programs side by side (works today)

PulseRack runs as its own program and REAPER talks to it the way it would talk to a hardware synth: MIDI out, audio back in.

```
 REAPER ── MIDI notes + clock ──► virtual port ──► MIDI In / MIDI Clock ─┐
   ▲                                                                     │ the rack
   └──── audio (PipeWire) ◄─── Audio Output ◄────────────────────────────┘
   └──── MIDI (recorded) ◄──── virtual port ◄─── MIDI Out
```

### A MIDI connection between them

The rack opens **existing** ports by name (libremidi's ALSA sequencer back-end; `midi.md`, *As built (steps 1 to 3)*); it does
not make a port of its own. So something must provide a port both programs see:

* **`snd-virmidi`** (`sudo modprobe snd-virmidi midi_devs=1`) makes a virtual card with both a raw MIDI device and an ALSA
  sequencer port. REAPER lists the card under *Preferences > Audio > MIDI Devices* (raw MIDI), the rack lists the sequencer
  port (`build/src/pulserack-midi --list`). This is the safest bridge, because it does not depend on which ALSA interface
  REAPER uses for MIDI. *Not yet tried with REAPER.*
* **JACK MIDI under PipeWire**: when REAPER runs on the JACK audio system (*Preferences > Audio > Device*, `pipewire-jack`),
  its MIDI ports are PipeWire MIDI ports, and PipeWire bridges those to the ALSA sequencer, so the rack sees them. Connect them
  in `qpwgraph` or Helvum. *Not yet tried.*
* *Midi Through Port-0* also works one way (it is how step 9 was checked by hand with `aseqsend` and `aseqdump`), but it is a
  single shared port, so notes going to the rack and notes coming back would mix. Use it only for a quick test.

In the rack: switch the port on in the device menu, choose it in the **Device** box of the MIDI In (and MIDI Clock), and on
REAPER's side enable the port for output, set the track's MIDI hardware output to it, and set the channel the MIDI In listens on.

### Tempo and transport

1. In REAPER, *Preferences > Audio > MIDI Devices*, the output port's *Configure*: tick **Send clock/SPP**.
2. In the rack, feed a **MIDI Clock** (Device: that port, Pulses per beat 24, Start/stop on) into the Clock's place, i.e. into
   the sequencer or arpeggiator's clock input.
3. Play in REAPER: its *start* sends the sequencer to its first step, *stop* stops it and releases the held notes, the tempo
   follows REAPER's tempo map tick by tick.

Limits: **song position is ignored** (`midi.md`, *Not done*), so starting REAPER from the middle of the song sends *continue*
(or *start*) without telling the rack where it is: a sequencer carries on from its current step, not from the bar REAPER is at.
Start from a bar line that is a multiple of the pattern's length, or from the beginning. The rack's own Clock does not follow
MIDI (by design), so a patch driven by REAPER must use MIDI Clock.

### The rack's sound into REAPER

The rack opens its output with SDL3, which on this machine is a PipeWire stream. With REAPER on JACK (`pipewire-jack`), both are
nodes in one graph: in `qpwgraph`, disconnect the rack's stream from the speakers and connect it to REAPER's inputs, then record
on a track whose input is those two channels (monitoring on, to hear it through REAPER's effects).

* The rack only has **one stereo output** (one SDL stream), so a multitrack recording of separate voices takes several runs, or
  *rack-render* (below).
* Latency: the rack's device buffer is 256 frames (5.3 ms, `architecture.md`, *Audio*), and MIDI in waits one device call
  (`midi.md`, *As built (step 8)*), plus REAPER's own buffers. The recorded take therefore lands late by a fixed amount. Measure
  it once (a sequencer step into a click, recorded beside REAPER's metronome) and set it as the track's *recording offset*
  (*Track recording settings > Track input offset*). *Not yet measured.*
* The two programs run on separate audio clocks only if they use separate devices. On PipeWire they share the graph's clock,
  so there is no drift.

### MIDI from the rack into REAPER

A **MIDI Out** (Device: the second virtual port, a channel) after the sequencer or arpeggiator; in REAPER, a track with that
port as its MIDI input, armed, records the notes. Then the part can be edited in REAPER and played back into the rack (above).
MIDI Out leaves at the same fixed latency as the audio (`midi.md`, *MIDI Out, Timing*), so the same recording offset applies.
It sends no controls or pitch bend (*Not done*).

### Offline: rack-render into a REAPER project

`rack-render patch.rack out.wav seconds` renders a patch with the same engine and no sound card. It has no MIDI input, so it
suits a patch that plays itself (its own Clock and sequencer), at the patch's tempo. Set REAPER's project tempo to the patch's
`bpm` and drag the WAV in at bar 1: it lines up exactly, with no latency, and the render is bit-exact from run to run.

A REAPER action could do it from inside the project: a ReaScript (Lua) that runs `rack-render` with `reaper.ExecProcess` and
inserts the result with `reaper.InsertMedia`. *Not written.*

## Level 2: small additions to the rack

Each is useful without the others; none needs anything from REAPER beyond what it already does.

| Addition | What it gives | Cost |
|----------|---------------|------|
| **Ports of its own** | a "PulseRack in" and "PulseRack out" ALSA sequencer port the rack creates, so no `snd-virmidi` and no Midi Through. libremidi can open virtual ports on ALSA. | small: a setting, two slots in `midi::Inputs` / `midi::Outputs`, the Device menus list them |
| **Song position** (`F2`) | REAPER started mid-song puts the sequencer on the right step. MIDI Clock would pass on a position; the sequencer would need a "go to step" event, which `pcl_event_t` does not have (PulseCoreLib change, or done in the glue). | medium |
| **Audio In** (on the work list) | REAPER's tracks into the rack: a send to the rack's input, through filters and the delay, back out. Also the planned Sampler's recording. | medium; already wanted |
| **Several outputs** | separate stereo pairs per Audio Output module, so REAPER records voices on their own tracks. SDL3 can open more channels on one stream; PipeWire would show them as ports. | medium |
| **rack-render options** | `--tempo`, `--stems` (one WAV per Audio Output), `--midi file.mid` (a REAPER item exported as MIDI, played into the MIDI In modules offline) | small to medium; the MIDI file reader is new code |
| **OSC** | REAPER's OSC control surface sends transport and track values over UDP; the rack could listen for `/pulserack/<node>/<control>`. | medium; a new dependency or a small parser |

Recommended order if Level 1 is kept: ports of its own first (it removes the setup step that most often goes wrong), then
song position, then Audio In.

## Level 3: PulseRack as a plugin inside REAPER

The rack as an instrument (and effect) plugin: REAPER hosts it on a track, sends it notes, transport and automation with
sample offsets, records its output on that track, and saves the patch in the `.rpp`. No ports, no graph wiring, no latency to
compensate, and REAPER's render renders the rack too (faster than real time, bit-exact). This is the real answer if the rack is
to be used with a DAW often.

### Which format

* **CLAP first.** REAPER has hosted CLAP since 6.71. The API is a plain C header (MIT), small enough to read whole, made for
  exactly this (sample-accurate events, a thread model that is spelled out, state as a byte stream). Fetched with
  `FetchContent` at a release tag, like libremidi.
* **VST3 second**, through a CLAP-to-VST3 wrapper (`clap-wrapper`), for hosts without CLAP. The VST3 SDK is now MIT as well
  (Steinberg relicensed it in 2025; to be checked against the version fetched).
* Not LV2: REAPER hosts it, but it would be a third set of code for one platform.

### How the engine maps onto a plugin

The engine already has the shape a plugin needs (`architecture.md`, *Threads*):

| Plugin | Engine |
|--------|--------|
| `activate(sample_rate, min, max frames)` | a new sample rate: new states and plan on the main thread, as changing the output device does today |
| `process()` on the host's audio thread | `Engine::render()`, attached as the audio thread; frames in blocks of at most 256 |
| the host's main thread | the UI thread: compile, set_param, undo, the patch file |
| note events with sample offsets | a new **host inbox** beside the MIDI device inboxes: a MIDI In with Device *Host* hears them at their own sample (step 8 already places messages on samples) |
| transport (tempo, playing, beat position) | a new **Host Clock** module, distinct from Clock and MIDI Clock (a module of its own, not a mode of either): pulses at its ppqn locked to the host's beat, START/STOP from the play state, and it can jump the sequencer to the host's position |
| automation | a fixed set of **macro parameters** (Macro 1 to 32) the plugin declares once; any knob is assigned to one by a right-click (*Assign to host macro N*), stored in the patch like a per-patch `map` record. CLAP wants a fixed parameter list, the rack's knobs change with the patch, so macros sit between them. |
| state save/load | the patch file text as the state stream (it already round-trips exactly and refuses a bad file whole) |
| audio outputs | the Audio Output modules: the first is the main stereo bus, further ones extra ports (REAPER shows them as extra pin pairs) |
| audio inputs (as an effect) | the Audio In module, fed by the plugin's input buffers instead of a sound card |

### What is hard

1. **The window.** gpuic opens its own SDL3 window and runs its own loop (`run()`). A plugin must draw into a window the host
   gives it (an X11 window id on Linux, HWND on Windows) and must not own the event loop. Either gpuic learns to attach to a
   parent window and be driven by the host's timer (a gpuic change: the first thing to ask for, with a prompt like
   `gpuic-prompt-4.md`), or the plugin opens the rack as a **floating window of its own** (CLAP allows non-embedded GUIs), which
   needs only that gpuic can run its loop off the host's timer. The floating window is the smaller first step.
2. **Two plugins at once.** Everything that is process-wide today must become per instance: the settings file, SDL
   initialisation (shared, counted), the MIDI devices (in a plugin the rack would not open devices at all; the host provides
   MIDI).
3. **No SDL audio.** The plugin build leaves out `src/audio/` entirely; `Engine` never included it, so this is a build target,
   not a rewrite.
4. **Float only.** A plugin is built with `PULSERACK_SAMPLE_TYPE=FLOAT`; the Q15/Q31 builds stay for the standalone and tests.
5. **Real-time rules in the host's thread.** The audio thread rules (no lock, no allocation) already hold in `render()`. The
   plugin's event conversion must keep them.

### A possible order

1. `pulserack-clap` target with no GUI: the engine, a fixed patch from the state, host notes into MIDI In, one stereo output.
   Check in REAPER that a sequenced example plays and renders bit-exact twice.
2. Host Clock module, transport and position.
3. Macro parameters and *Assign to host macro*.
4. The rack's window, floating, then (with gpuic's help) embedded.
5. Extra outputs, the effect variant with Audio In, the VST3 wrapper.

## As built (the CLAP plugin)

**Using it.** The build makes `build/src/pulserack.clap` (option `PULSERACK_CLAP`, on by default; float builds only: a Q15 or
Q31 build skips it with a message), and with `-DPULSERACK_VST3=ON` also `build/Release/PulseRack.vst3` (copy or link it into
`~/.vst3`, or add its folder to *Preferences > Plug-ins > VST*). Copy or link it into `~/.clap` (Windows: `%LOCALAPPDATA%\Programs\Common\CLAP`), then in
REAPER *Options > Preferences > Plug-ins > CLAP*: *Re-scan*. Insert **CLAPi: PulseRack (PulseRack)** on a track as its
instrument. A new instance has the MIDI voice example in it, so a MIDI item on the track plays it at once. The FX window shows
the rack: add modules, cable them and turn knobs as in the standalone rack.

**What it does.**

* **Notes.** The track's MIDI (CLAP note events or MIDI 1.0 on its one note port) reaches every MIDI In and MIDI CC module at
  its own sample, through `Engine::render(frames, l, r, host)` (`midi::Timed`: a message and its offset in the call). In the
  plugin every device a patch names is the host (slot 0), so a patch made for the KeyStep plays from the track without changes;
  the Device box shows *Any device* or the name, and its menu has nothing else to offer.
* **Sound.** Eight stereo outputs. An Audio Output module has an **Output** knob, 1 to 8: Output 1 is the main one (the
  track), 2 to 8 are the plugin's further outputs, which REAPER shows as pins 3/4 to 15/16 (route them to tracks of their own
  for separate recording or effects). Where there are none (the standalone rack, a host that leaves them out) every output
  plays on the main one. Each output gets the same clamp to ±1.
* **PulseRack FX** is the same rack as an effect: the track's audio comes in through the new **Audio In** module (L, R and a
  Level knob; silent in the standalone rack, which has no input yet). A new instance has a delay on each side (350 and 370 ms).
* **Tempo and transport.** The new **Host Clock** module (a module of its own, beside Clock and MIDI Clock) sends clock
  pulses on the host's beat grid at *Pulses per beat* (a pulse on every beat of the song, wherever playing starts), START
  when the host starts playing or jumps back (a loop, or the cursor moved back), STOP when it stops; nothing while it is
  stopped, and nothing in the standalone rack. Tempo changes are followed call by call. The position is not passed on: a
  sequencer starts at its first step wherever playing started, so start from a bar the pattern begins on.
* **Automation: Macro 1 to 32.** The plugin has 32 automatable parameters. Right-click a control in the window, *Automate
  with a host macro*: it gets the lowest free macro (`macro <n> <node> <control>` in the patch, an undo step), and REAPER's
  parameter list names it (*Macro 3: Cutoff (State-variable filter 4)*). A macro's value is the knob's travel (0..1, its
  own scale: a log knob moves as on screen); a change from REAPER sets the control at the start of the engine block it falls
  in (at most 256 samples late, 5 ms at 48 kHz), and the model, the panel and the saved patch follow. *Take off host macro N* frees it.
  Controls the library fixes when a module is made (a reverb's size) cannot be on a macro. Turning a macro's knob in the
  window tells REAPER its new value (rescan), but is not recorded as automation.
* **MIDI Out to the track.** A MIDI Out module sends to the plugin's MIDI output (any device it names, or none, is the
  host), each message at the sample it was made: REAPER can record it or send it on to another instrument.
* **MIDI learn from the track's CCs.** The track's control changes reach MIDI learn as a device's do: with the window open,
  both kinds of learn work (the global one is kept in the rack's settings file, shared with the standalone rack); with the
  window closed, the patch's own mappings (*MIDI learn for this knob only*) still turn their knobs (the plugin's 60 Hz timer).
  In the plugin a mapping answers the track whatever device it was learned from, so a patch mapped to the KeyStep's knobs in
  the standalone rack answers the same CCs from REAPER.
* **The patch is the plugin's state**, written as the patch file's text: REAPER keeps it in the `.rpp` (base64 in the CLAP
  block's `<STATE>`), and a state decoded from there is a `.rack` file the standalone rack opens. A state that does not load is
  refused, the patch before it is kept, and the error goes to the host's log. Opening a patch (*Open*, a factory patch, the
  host's state) makes every module again from it, as in the standalone rack.
* **The window** (`clap.gui` and `clap.timer-support`): `open_rack` with `Options::engine` (the plugin's engine, which the
  window edits but does not own: it outlives every window), `parent` (REAPER's X11 window, or an HWND), the size, the host's
  name and `on_change`. gpuic's app is made when REAPER shows the editor and freed when it hides it; REAPER's 16 ms timer steps
  it. The toolbar says *played by REAPER · 48.0 kHz* in place of the device menu and the Audio button; no sound card or MIDI
  device is opened, and no recovery file or settings file is written. Every change (`Editor::touch`, a patch opened) calls
  `mark_dirty`, so REAPER asks to save the project. The window can be resized (900 x 500 at least).
* **Threads.** init, activate, the state and the window on REAPER's main thread (the engine's UI thread); `process` on its
  audio thread, attached between activate and deactivate. `process` neither allocates nor locks (the host's messages go into a
  vector reserved at init; past 4096 in one call they are dropped and counted, and the count is logged at deactivate).

**Checked.**

* `clap_plugin` (ctest): `tests/clap_host.cpp` loads the built `.clap` with `dlopen` / `LoadLibrary` as a host does: both
  descriptors and their ports; silent until a note; a note sounds from its own sample (within the envelope's control step), from CLAP
  note events and from MIDI bytes alike; the host's call size (64 or 1000 frames) changes nothing in the output; the state is
  the patch text, written and read through streams that take a few bytes at a time, and another instance that loads it plays
  the same notes bit for bit; a bad state is refused and the old patch kept; `examples/sequence.rack` as the state plays with
  no notes; a CC from the track turns the knob a patch maps it to with no window open (a mapping made with another device
  too, another channel does nothing); a MIDI Out sends the track's notes back at exactly their samples on its channel; the Host
  Clock gives START and pulses on the 120 bpm grid from wherever playing began (900 samples to the next pulse, then every
  1000), START again on a jump back and STOP on stop; the macros are named after their controls, a host change sets the
  control (get_value, the saved patch and value_to_text follow), a flush does the same, a macro on nothing keeps its value;
  an Audio Output on Output 3 plays there and not on the main output; PulseRack FX passes its input through Audio In exactly.
* `ui_hosted` (ctest, `ui_smoke --hosted`): the window over an engine it does not own shows that engine's patch (panels and
  cables), keeps the plugin's MIDI slot function, reports a knob turned and a module added through `on_change`, puts a
  control on the lowest free macro (one undo step), shows a macro's change on the panel, takes it off again, opens a patch
  into it, and the engine and its patch outlive the window.
* In REAPER 7 on Linux (run from a script, with a separate resource folder and `CLAP_PATH`): the plugin is found and inserted;
  a MIDI item of four notes renders with each note where it is (and the release after it); the `.rpp` holds the patch text; a
  project whose state was replaced by `examples/sequence.rack` renders that sequence with no MIDI; the editor opens inside the
  FX window with the patch on it (screenshot), and REAPER closes cleanly with it open. Then: at 90 bpm a Host Clock patch
  (one sequencer step a beat into Note to Gate) renders its gates exactly every 32000 samples from 0; REAPER lists the 32
  macros with Macro 1 named after its control and its value as the knob's travel; PulseRack FX on an audio item gives the
  click dry and its echoes at 350 ms (left) and 370 ms (right); `PulseRack.vst3` is found as *VST3i: PulseRack (16 out)* and
  *PulseRack FX (2->16ch)* and renders a MIDI item (REAPER shows 2131 parameters for it: clap-wrapper adds the MIDI CCs of
  every channel as hidden parameters, VST3 having no CC events).
* Also built with `-DPULSERACK_MIDI=OFF` (the plugin and both tests pass), and a Q15 configuration skips the plugin.

**Not yet checked by hand:** playing it live from the KeyStep through REAPER; working in the editor with the real mouse inside
REAPER (zoom, drags, the file dialogs, keys: REAPER may keep some keys for itself); two instances with their windows open at
once; a project saved, closed and opened again with the window open; automation drawn in an envelope and played back; the
extra outputs routed to tracks; the VST3's window; Windows (the code is there, CI builds the CLAP).

**Not done:** song position (a sequencer cannot be put on a given step: `pcl_event_t` has no such event); a macro change
inside an engine block (it waits for the block's start); recording a knob turned in the window as automation (CLAP's
gesture and param-out events); MIDI Out sends notes and clock only (no CCs or pitch bend); the VST3 on Windows and macOS is
not built in CI.

**Building for other machines.** The `.clap` links SDL3, FreeType, HarfBuzz and FriBidi the way the rack does: with
`PULSERACK_GPUIC_DEPENDENCIES=FETCH` they are static and stay inside the plugin (`--exclude-libs`); with SYSTEM or AUTO (this
machine's `build/`) it needs the system's copies at run time.

## Questions for you

* Is REAPER the only DAW in view? If so, Level 1 with *ports of its own* may be enough for a long while; if other DAWs matter,
  Level 3 (CLAP + wrapper) covers them all with one piece of work.
* For Level 3: is a floating window acceptable at first, or must it be embedded before it is worth having?
* Should a patch saved inside a REAPER project also be written out as a `.rack` file (so it opens in the standalone rack)?
