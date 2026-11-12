# MIDI controllers

Status: **design, steps 1 to 5 built** (see the *As built* sections at the end). Where the two differ, *As built* and the code win.

MIDI keyboards and controllers playing and turning the rack: notes into a patch, controls (knobs, faders, the mod wheel)
into signals, and a controller's knobs turning the knobs on screen.

## Decisions

| | Decided |
|---|---|
| Library | **libremidi** (`celtera/libremidi`), fetched with `FetchContent` and pinned to a release tag (`v5.4.3`), like PulseCoreLib and gpuic but not on `main`: a third-party library does not change under us between builds. `FETCHCONTENT_SOURCE_DIR_LIBREMIDI` points at a local checkout. |
| Build switch | `PULSERACK_MIDI` (ON by default). Off, nothing of libremidi is fetched or built and the rack has no MIDI. |
| Where notes arrive | a new **MIDI In** module (not the Keyboard module) |
| Controller knobs | a **MIDI CC** module for modulation in the patch, and **MIDI learn** for knobs on screen |
| Learn mappings | **global** (in the settings file, not the patch): a mapping turns *knob N of the selected module*, so it works in every patch. Per-patch mappings that point at exact knobs are a later option in the settings. |
| Input devices | a **machine setting**, chosen by name, **several at once** |
| libremidi stays in `src/midi/` | nothing else includes its headers, so it could be swapped (RtMidi) by changing that folder only |

## Threads

```
 device A thread ──push──► ring A ─┐
 device B thread ──push──► ring B ─┼──► audio thread: render() drains every ring at the start of a block
 ...                               ┘
 UI thread: opens and closes devices, follows hot-plugging, MIDI learn (through Engine::set_param)
```

* libremidi opens one `midi_in` per port and may call each one back **on its own thread**. `SpscRing` has exactly one
  producer, so **every open device has its own ring** (at most `kMaxInputs` = 8 devices; 512 messages each). A
  multi-producer ring would also work but is more than this needs.
* A message carries the **slot** of its device (0 to 7). A slot belongs to one device name for the whole run: unplugging
  and plugging it back in gives it the same slot, so a MIDI In module set to that device keeps hearing it.
* Notes do **not** pass through the UI thread (that would add a frame of latency). `Engine::key()` cannot be used from a
  MIDI thread: its command ring has the UI thread as its one producer.
* **Hot-plugging** is followed by the UI thread: libremidi's observer only raises a flag, and the UI's next tick lists
  the ports again and opens or closes devices. Closing a `midi_in` stops its thread, so the UI thread may then push the
  device's last message itself: a **device lost** message, after which consumers release every note that device holds.
* Lost messages (a full ring) are counted per device.

## Messages

libremidi delivers whole messages (it handles running status). `decode()` turns one into a small, trivially copyable
`midi::Msg`: note on and off (a note on with velocity 0 is a note off), control change, pitch bend (14 bits, 8192 is the
middle), channel and key pressure, program change, and the realtime clock, start, continue and stop. Everything else
(SysEx, MIDI time code, song position) is dropped. Each message keeps libremidi's monotonic timestamp in nanoseconds for
step 8.

PulseCoreLib's `pcl_event_t` only has note on/off, clock, start and stop. Controls, pitch bend and pressure therefore
become **signals** in PulseRack, never library events.

## Device names

A device is chosen by name, because port handles last only as long as the MIDI system's session. The name is libremidi's
display name (its port name if that is empty). Two identical controllers would have the same name, so the second one
becomes `name #2`, the third `name #3`, in the order the system lists them.

## Settings file

`$XDG_CONFIG_HOME/pulserack/settings` (or `~/.config/pulserack/settings`): machine settings, not part of any patch.
Line-based, like a patch file:

```
pulserack-settings 1
output Built-in Audio Analog Stereo
midi-input Arturia KeyStep 37
midi-input nanoKONTROL2
```

* `output`: the sound card chosen in the toolbar (missing: the system's default). It used to be forgotten at exit.
* `midi-input`: an input that is switched on, one line each.
* `learn ...` lines will hold the global MIDI learn mappings (step 7).
* Unknown lines are kept as warnings and ignored, so an older PulseRack reads a newer file.
* Written through `write_file_atomic` whenever a setting changes. The tests use no settings file at all.

## Modules

**MIDI In** (event and signal outputs):

| Parameter | Values |
|---|---|
| Device | Any, or one input by name |
| Channel | Omni, 1 to 16 |

| Output | Kind |
|---|---|
| Notes | events: note on and off, with sample offsets |
| Pitch bend | signal, -1 .. 1 |
| Mod wheel | signal, 0 .. 1 (CC 1) |
| Pressure | signal, 0 .. 1 (channel pressure; key pressure of the newest note) |
| Velocity | signal, 0 .. 1, of the newest note |

On *device lost*, `reset()` and a change of Device or Channel it sends a note off for every note it holds.

**MIDI CC**: Device, Channel and CC number (0 to 119) parameters; one signal output, 0 .. 1, smoothed over a few
milliseconds so steps of 1/127 do not click. 14-bit pairs (CC n and n+32) are a possible later addition.

## MIDI learn

Right-click a knob, *Learn*, then move a control: the control is stored as **(device, channel, CC) → knob N of the
selected module**. Turning it goes through `Engine::set_param` on the UI thread, so the model stays the source of truth
and the knob moves on screen; a sweep becomes one undo step, like a knob drag. Mappings are saved in the settings file.

## Steps

1. **Build:** libremidi `v5.4.3` by `FetchContent` behind `PULSERACK_MIDI`. *(built)*
2. **Settings file:** load and save it; the output device choice moves into it. *(built)*
3. **`src/midi/`:** the device manager (list ports, hot-plug, open by name, one ring per device), `decode()`, the
   unique names, tests that need no hardware, and a `pulserack-midi` monitor. *(built)*
4. **Engine:** drain every device ring in `render()`; lost messages per device in the telemetry. *(built)*
5. **MIDI In module**, and the inputs in the device menu (a switch per device). *(built, without the Device parameter)*
6. **MIDI CC module.**
7. **Global MIDI learn.**
8. **Sample-accurate timing:** a message's timestamp turned into a sample offset with one block of fixed latency
   (a constant delay instead of up to a block of jitter: 5.3 ms at 256 frames and 48 kHz).
9. **Later:** per-patch mappings, MIDI clock in to the clock and sequencer (`PCL_EVT_CLOCK/START/STOP`), MIDI out.

## As built (steps 1 to 3)

**Build.** `CMakeLists.txt` fetches libremidi `v5.4.3` (shallow) behind `PULSERACK_MIDI`. Only its ALSA back-end is built
on Linux: the PipeWire back-end would fetch readerwriterqueue from its `master` branch, and the network back-end Boost, so
those are off along with JACK and the computer-keyboard back-end. PipeWire systems still see every device, because
PipeWire serves the ALSA sequencer. `-DPULSERACK_MIDI=OFF` builds and passes the tests without libremidi.

**Targets.**

| Target | Files | libremidi |
|---|---|---|
| `rack_midi_core` | `src/midi/message.cpp` (`decode()`), `src/midi/names.cpp` (`unique_names()`); `inbox.h` is header-only | no; always built, the tests link it |
| `rack_midi` | `src/midi/inputs.cpp` (`midi::Inputs`) | yes, the only file that includes it |
| `pulserack-midi` | `src/midi_monitor.cpp` | through `rack_midi` |
| `rack_geometry` | `src/ui/settings.cpp` added | no |

**`midi::Inputs`** (all calls from one thread): `start()` makes the observer (hardware and virtual ports, so software
senders and *Midi Through* are listed); `set_enabled()` takes the settings' names; `poll()` re-lists the ports only after
the observer's thread raised its flag, closes what went away or was switched off (pushing *DeviceLost* into the slot)
and opens what is wanted and present. Each device's callback decodes and pushes into its slot's `Inbox`; a full ring
counts into `Inbox::lost`. Messages carry libremidi's `SystemMonotonic` timestamp. libremidi's own announcement port on
ALSA (`libremidi-observe`) is left out of the list: it is not a device.

**Settings.** `ui/settings.h`: `Settings { output, midi_inputs }`, `load_settings` / `save_settings` in
`default_config_dir()` (`$XDG_CONFIG_HOME/pulserack`). `open_rack` loads them (warnings to stderr) and starts audio on the
saved output; choosing an output in the toolbar saves it. `Options::config_dir` is empty in the tests, so they read and
write no file. The `midi-input` lines are read and written but nothing uses them until step 5.

**Checked.**
* `rack_tests`: `decode()` for every message type and the refusals (SysEx, sensing, song position, wrong lengths, a data
  byte with the top bit set; `out` untouched), `unique_names()` (including a device really called `X #2`), an inbox
  overflowing (order kept, losses counted), one producer thread and one consumer through an inbox (100 000 messages), the
  settings text round trip, unknown lines, CRLF, refusal of a patch file, names with a newline, on disk, and the XDG path.
* By hand, through ALSA's *Midi Through* port: `aseqsend -p 14:0 90 3C 64 B2 01 7F E0 00 40 F8 80 3C 00 93 40 00` while
  `pulserack-midi "Midi Through Port-0"` listened printed note on, control 1 = 127 on channel 3, pitch bend 8192, clock,
  note off, and the note on with velocity 0 as a note off.

**Hot-plugging, found with a real controller (Arturia KeyStep 37):** unplugging it printed no *device lost* and, plugged back
in, it stayed silent. libremidi's ALSA observer reports a port's removal only if it reported that port before, and
`notify_in_constructor` was off, so a device that was there at the start was never reported as gone; the stale `midi_in` then
counted as "already open". Now `notify_in_constructor` is on and all four callbacks (inputs and outputs, added and removed) raise
the flag. Reproduced and checked with a virtual ALSA device that sends notes, goes away and comes back while `pulserack-midi`
listens: notes, *closed*, *device lost*, *open*, notes again.

`pulserack-midi` takes a name in part and in any case (`keystep` for `Arturia KeyStep 37 MIDI 1`), says so when a name fits
no input or several, and prints what it is listening to.

**Checked on the real controller after the fix:** notes print when it is played; unplugging gives *closed* and *device lost*,
and plugging it back in gives *open* and notes again.

## As built (steps 4 and 5)

**Engine.** `Engine::set_midi(Inboxes *)` (set while no audio thread is attached; the inboxes outlive the last `render()`).
At the start of every block, after the commands, `render()` empties every inbox and gives each message to every MIDI In
module of the current plan (`Snapshot::midi_ins`). With no MIDI In module the messages are still taken, so nothing piles up.
A message is heard at the first sample of the next block: up to a block late and with that much jitter until step 8.
`Engine::discard_midi()` empties the inboxes before audio starts, so that what was played while audio was off is not heard
all at once. Lost messages are counted in each `Inbox::lost` (the UI can read them; nothing shows them yet).

**MIDI In** (`midi_in`, 8 HP, in `modules_glue.cpp`): a **Channel** selector (Omni, 1 to 16); outputs NOTES (events),
BEND (-1 .. 1), MOD (CC 1, 0 .. 1), PRESS (channel pressure, or key pressure of the newest note) and VEL (the newest
note's velocity, held after the note ends). Bend, mod and pressure are smoothed over 3 ms. It keeps a bit per note per
device slot, and sends the note offs itself when a device is lost, on CC 120 or 123 from that device, and when its channel
changes. Up to 128 notes per block; more are dropped and counted (the panel's LED turns red, as for the sequencer).

**Differences from the design:**
* **No Device parameter yet.** A MIDI In module hears every input that is switched on. Choosing one device per module needs
  a parameter that holds a name, which the patch file and the descriptors do not have (parameters are numbers); a slot
  number would not do, because slots are given per run. Several devices at once work; telling them apart in a patch does not.
* No sustain pedal (CC 64), and the realtime messages (clock, start, stop) are ignored until step 9.

**The application.** `Editor` owns the inboxes and (with `PULSERACK_MIDI`) a `midi::Inputs`, both declared before the sound
card so that they outlive its thread. At start it opens the settings' `midi-input` names and polls for hot-plugging four
times a second; the hint line says when an input connects or goes. The **device menu** (the audio status in the toolbar)
lists the MIDI inputs under the outputs: a click switches one on or off (several may be on), saves the settings and opens or
closes the device; one that is switched on but not plugged in is listed as *not connected*. The toolbar shows `MIDI n` while
n inputs are open. `Options::midi` is off in the tests, which feed the engine's inboxes themselves. A build without
`PULSERACK_MIDI` still has the module and the menu, which then says MIDI is not built in.

`examples/midi_voice.rack` (also under *Open > Examples*): MIDI In to an oscillator and an envelope, pitch bend bending
two semitones and the mod wheel opening the filter.

**Checked.**
* `rack_tests`: notes through MIDI In and Note to Gate open and close a gate from the first sample of the next block, from
  any slot; the channel filter, and a channel change releasing held notes; *DeviceLost* releasing only that device's notes;
  CC 123; `reset()`; bend, mod, pressure and velocity values and the smoothing; messages taken with no MIDI In module;
  `discard_midi()`; 200 notes in one block (72 dropped and counted).
* By hand: the application with a settings file naming a virtual ALSA device that plays notes, `midi_voice.rack` and SDL's
  dummy sound driver: the toolbar showed `MIDI 1` and the output meter moved.

**Not yet checked:** the real controller playing through the speakers, the device menu's switches by hand, and how the
latency feels (a block of the device plus up to a block of jitter).
