# MIDI controllers

Status: **design, steps 1 to 9 built** (see the *As built* sections at the end). Where the two differ, *As built* and the code win.

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
 device B thread ──push──► ring B ─┼──► audio thread: render() drains every ring at the start of a device call
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
(SysEx, MIDI time code, song position) is dropped. Each message keeps the time it arrived, in nanoseconds on the
steady clock, for step 8.

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
5. **MIDI In module**, and the inputs in the device menu (a switch per device). *(built; the Device came with step 8)*
6. **MIDI CC module.** *(built; the Device came with step 8)*
7. **Global MIDI learn.** *(built)*
8. **Sample-accurate timing:** a message's timestamp turned into a sample offset with one block of fixed latency
   (a constant delay instead of up to a block of jitter: 5.3 ms at 256 frames and 48 kHz). *(built, with the modules'
   Device)*
9. **Later:** per-patch mappings, MIDI clock in to the clock and sequencer (`PCL_EVT_CLOCK/START/STOP`), MIDI out.
   *(built: the MIDI Clock and MIDI Out modules, and MIDI learn for one knob in the patch)*

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
* ~~No Device parameter yet.~~ Built with step 8 (*As built (step 8 and the modules' Device)*): it is not a parameter (those
  are numbers) but a name kept beside them in the patch.
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

## As built (step 6)

**MIDI CC** (`midi_cc`, 6 HP, in `modules_glue.cpp`): a **Channel** selector (Omni, 1 to 16) and a **Controller** knob
(0 to 119, 1 at first: the mod wheel); one output, 0 .. 1, smoothed over 3 ms. It starts at 0 (a controller's position is
not known until it moves) and keeps its value when the controller number or the channel is changed. Its Device came with
step 8, as MIDI In's did. No 14-bit pairs.

To find a control's number, run `pulserack-midi` and move the control: the second figure of its `control` line is the
number. (MIDI learn, step 7, will make this unnecessary for knobs on screen.)

**Engine.** The feed is no longer tied to one module type: `midi_sink(desc)` gives the function that takes MIDI for a
module type (MIDI In, MIDI CC) or null, and a plan lists every such module (`Snapshot::midi_ins`) with its function.

**Checked** (`rack_tests`): the default controller from any channel and device, the smoothing, other controllers and notes
ignored, a change of controller number keeping the value, the channel filter, a MIDI In beside it getting the same
messages, `reset()`. The panel was looked at in a screenshot. **Not yet checked:** a real controller's knob through it.

## As built (step 7)

**Using it.** Right-click a knob, slider, selector or switch: a small menu opens under it with **MIDI learn**, *Forget its
mapping (CC n)* if that place has one, and *Cancel* (a press elsewhere closes it too). *MIDI learn* selects the control's
module and the hint line asks for a controller to be moved (Escape cancels). The first control change that arrives is mapped to **that place** (control N of a module, in
the descriptor's order), not to that one knob: from then on the controller turns control N of **whichever module is
selected**. With no module selected, moving it says so. Learning a place again replaces its mapping, and a controller has one
place. *Forget the MIDI learn mappings* is in the device menu while there are any.

**How.** The audio thread, as it feeds the modules, also passes every control change on to the UI through a ring
(`Engine::take_control`; 1024 messages, more are dropped). With no audio thread the call feeds the modules itself, so learn
works with audio off. `Editor::pump_controls` takes them 60 times a second (only while MIDI is on) and `midi_control` either
finishes a learn or looks the controller up by **device name, channel and number**. The value goes through
`Editor::set_param`, so the model is the source of truth, the knob moves on screen and a sweep is one undo step (it has the
knob's own merge key). A knob, slider or selector takes the controller's 0 .. 127 as its travel (`from_travel`); a switch is
on from 64. Mappings are `learn` lines in the settings file (channel and control written from 1).

The learn messages are shown in the hint line even though the pointer is still on the control that was right-clicked
(`Editor::notice`): without that, the control's own hint hid them.

**Differences from the design:** none in what it does. Not done: per-patch mappings (step 9), soft takeover (a knob jumps to
the controller's position on its first move), and any mark on a control that has a mapping.

**Checked.** `rack_tests`: the settings' `learn` lines (round trip, names with spaces, bad lines skipped with a warning); the
engine passing control changes on, in order, with and without rendering, and dropping what the ring cannot hold. `ui_smoke`:
an unmapped controller does nothing; a right-click opens the control's menu and *MIDI learn* in it starts a learn and
selects the module (the menu's *Cancel*, its *Forget its mapping*, a press on empty rack, and the menu on a selector too); the controller that moves is
mapped without turning the knob; then 127, 0 and 64 give the ends and the middle of the travel in one undo step; another
controller or channel does nothing; with another module selected its control at that place turns and the first module's does
not; with none selected the hint says so; learning again replaces; Escape cancels; forgetting empties the list.
**Not yet checked:** a real controller's knob, and the menu by hand (it was not looked at in a screenshot).

## As built (step 8 and the modules' Device)

**Timing.** A message is stamped when it arrives, in the device's callback, with `steady_clock` (not libremidi's own
timestamp, whose "monotonic" is the raw clock where there is one: the engine could not compare it with its own reading).
`Engine::render()` feeds MIDI **once per device call** (`feed_midi(frames)`), not once per block: it reads the clock, and a
message that is `age` samples old is placed `frames - age` samples into the call (to the nearest sample). One that arrived
just now lands on the last sample, one that arrived a whole call ago or more on the first. Every message is therefore heard
**one device buffer after it arrived**, where it used to be heard at the start of the next block: the same latency at worst,
and no jitter within it. A message with no timestamp (`time_ns` 0, as the tests' are), and one taken outside a device call
(`take_control` with audio off), is heard at once.

Each MIDI In and MIDI CC module keeps the messages it wants in a queue of its own, sorted by sample (`MidiQueue`, 256
messages; filtered by device and channel as they are when the message arrives). In `process` it takes those that are due
in the block: a note becomes an event at its offset in the block, and the signals (bend, mod, pressure, velocity, the CC's
value) are made in stretches from one message to the next, so a change takes effect at its own sample. A device call longer
than `PCL_MAX_BLOCK` is made in several blocks, and a message waits for the one it falls in. `reset()` empties the queues.
Note offs the module makes itself between blocks (a change of channel or device) go out at the start of the next block.
`Engine::set_midi_clock()` gives the tests a clock of their own.

Not done: the device call's own length is taken as the latency. A sound card that calls back unevenly (two calls close
together, then a gap) moves every message of a call together, which is not heard as jitter between notes of one call but
is between calls; correcting for it needs the time the buffer will be played at, which SDL does not give.

**A module's Device.** `Node::device` is the name of the one input a MIDI In or MIDI CC module listens to; empty is every
input that is switched on. It is not a parameter (those are numbers) but is kept beside them: in the patch file as
`device <node> <name>` after the node's parameters, the name being the rest of the line (so it may hold spaces and a `#`:
the record is recognised before a comment is cut off). A `device` line for a module that takes no MIDI is skipped with a
warning.

The engine turns the name into a device slot through `Engine::midi_slot_of`, which the application sets:
`midi::Inputs::slot_for(name)` gives the name's slot and, if it has none, a free one, so a patch may name a device that is
not plugged in and hear it when it is. With no MIDI, or when all 8 slots are taken, the name has no slot and the module
hears nothing (never every device: a patch made for two keyboards must not play both parts from one).
`Engine::set_device(node, name)` changes it while running (`Cmd::Device`, applied on the audio thread like a parameter);
`init_state` applies it to a new module; `refresh_devices()` looks every name up again once the device manager has started.
A MIDI In whose device changes lets go of the notes it holds, as on a change of channel; a MIDI CC keeps its value.

**In the window.** Both panels have a **Device** box under their controls: *Any device* or the name. A click opens a menu
under it: *Any device*, the inputs present, those switched on in the settings but not plugged in, and the module's own
device if it is none of these (a patch from another machine). The one in use is in the accent colour. Choosing an input
that is not switched on **switches it on** (the settings file, as the device menu does), and the hint line says so; it is
one undo step (the patch's part of it: the machine setting stays). The box's caption turns amber and says *NOT CONNECTED*
or *SWITCHED OFF* when the module's input cannot be heard; it follows hot-plugging and the device menu's switches.
`Editor::set_midi_device`, `device_menu_names`; `RackView::dev_node` is the open menu.

**Checked.**
* `rack_tests`: a note 100 samples old in a call of 256 starts at sample 156 and ends at its own sample; older than the
  call, exactly a call old, just arrived, stamped after the clock was read, and unstamped; a call of 64; a note on and off
  40 samples apart in one call; two devices' messages in time order, with a *DeviceLost* between them; bend, the mod wheel,
  velocity and MIDI CC unchanged before their sample; a call of 1000 frames (the message lands in the third block);
  `reset()` with a message waiting. A module set to one device hears only that one, a change of device releases held notes,
  a name with no slot hears nothing from any of the 8, and no name hears all again; a device named in the patch before
  `compile()`, `refresh_devices()` after the slots changed, no `midi_slot_of` at all, a module that takes no MIDI. The patch
  file: the round trip, names with spaces and `#`, a comment that mentions a device, the refusals and the warning.
* `ui_smoke`: the box is on MIDI modules only and its hint says what it listens to; its menu lists *Any device* and the
  inputs; choosing one that is off sets the device, switches the input on, makes one undo step and marks the patch unsaved,
  and it is in the saved text; the same one again changes nothing; undo and redo; an unplugged, switched-off device stays in
  its module's menu until the module leaves it; *Any device*.
* By eye, in a screenshot: the box on `midi_voice.rack` with a device named in the file (*SWITCHED OFF* with an empty
  settings file and the KeyStep plugged in).

**Not yet checked:** with the real controller: that fast repeated notes sound even and not grouped by buffer, and the Device
menu by hand with it plugged in and unplugged. Two controllers at once (there is one here).

## As built (step 9: MIDI clock in, per-patch mappings, MIDI out)

**MIDI Clock** (`midi_clock`, 6 HP, Event, in `modules_glue.cpp`): another device's tempo as clock pulses. A module of its own
rather than a mode of the Clock: a patch chooses its source by which module feeds the sequencer, and the Clock stays the
library's. It has a **Device** box like MIDI In (the realtime messages have no channel), a **Pulses per beat** knob (1 to 96,
24 at first, as the Clock's) and a **Start/stop** switch.
* MIDI clock is 24 ticks a beat. At 24 a tick is a pulse on its own sample. Below 24, ticks are skipped evenly (12: every other
  one); above, a tick makes several pulses, the first on the tick and the others spread over the time the last tick took, so
  that 96 ppqn has four evenly spaced pulses a tick. A tick that comes before the pulses still owed (the tempo went up) sends
  them at once: none is lost, and the count stays exact. After a start the first tick always makes a pulse (the first of the
  beat), whatever the rate.
* With **Start/stop** on (the default), it sends pulses only between a *start* (which it passes on as `PCL_EVT_START`: the
  sequencer goes back to its first step) or a *continue* (no START: it carries on from where it was) and a *stop*
  (`PCL_EVT_STOP`, which also comes when its device goes away). Off, every tick is a pulse and the transport is ignored, for a
  device that sends its clock without them.
* The internal **Clock** is unchanged: it does not follow MIDI.

**Per-patch mappings.** A control's right-click menu now has two learns: **MIDI learn** (as in step 7: control N of whichever
module is selected, in the settings) and **MIDI learn for this knob only**, which maps the controller to that very control of
that module, in the patch (`Patch::mappings`, written as `map <node> <control> <channel 1..16> <cc> <device name>`, the name
being the rest of the line). Learning one is an undo step and marks the patch unsaved; a module's mappings go with it when it is
removed. A controller with a mapping in the patch turns its control **whatever is selected**, and before any global mapping of
the same controller. The menu offers *Forget this knob's mapping (CC n)* for it, beside *Forget its mapping* for the global one
of that place. The control menu is wider for the longer items.

**MIDI Out** (`midi_out`, 6 HP, Event): NOTES and CLOCK inputs, a **Channel** selector (*ch 1* to *ch 16*: a name that is a
number would be read back from a patch file as an index), and a Device box that chooses an **output** (*No output* at first:
it sends nothing until one is chosen; a name with no slot sends nothing either).
* Note on and off on its channel; on the CLOCK input, every pulse is a MIDI clock tick (feed it a 24 ppqn clock), START is
  `FA` and STOP `FC`. At one sample the two inputs are merged in the library's order (STOP, NOTE_OFF, START, CLOCK, NOTE_ON), so
  a start reaches the other side before the notes after it.
* It remembers the notes it holds and sends their note offs, to the output and channel they went to, on a change of channel or
  output, on reset and on a stop. A note off for a note it did not send is not passed on.
* **Timing.** The engine takes what each MIDI Out made after every block (`midi_source`, `Snapshot::midi_outs`) and puts it in
  the **outbox** (`midi/outbox.h`: one ring, its producer the rendering thread) stamped with the time it is due: the sample's
  place in the device call plus one call's length after the call began, the same fixed latency as MIDI in, so MIDI and audio
  leave together. With no audio thread it is due at once.
* **`midi::Outputs`** (`midi/outputs.cpp`, libremidi; the only file besides `inputs.cpp` that includes it): a name gets a slot
  (`Engine::midi_out_slot_of`) and its port is opened when present (also later, when it is plugged in: `poll()`, with the
  inputs' 4 Hz tick). A **sender thread** takes the outbox, keeps the messages in time order and sends each when it is due,
  checking every half millisecond, under a lock it shares with the UI thread's opening and closing of ports. Before it lets go
  of its outputs (at exit) it sends *all notes off* (CC 123) on all 16 channels to each, so nothing is left sounding.
* The Device box says *OUTPUT* or, amber, *NOT CONNECTED*; its menu lists *No output* and the outputs present (and the
  module's own if it is not). Choosing one is an undo step and switches nothing on in the settings (outputs are not switched on
  and off: a module naming one opens it).
* `has_midi_device()` replaces `midi_sink()` wherever "has a Device" is meant (the box, the patch file's `device` record,
  `Engine::set_device`, `refresh_devices`).

**Checked.**
* `rack_tests`: MIDI Clock alone, its events read with their samples: ticks passed on only between start and stop and each on its
  own sample across blocks, START before the first CLOCK, continue without START; 12 ppqn (every other tick, the first after a
  start included); 96 ppqn (four a tick, spread over the last tick's time across short blocks); a tempo jump (the owed pulses
  sent at the next tick, in order, none lost); Start/stop off; a lost device stopping it; notes, controls and another device's
  ticks ignored. In a patch, a start and 24 ticks through the engine's inboxes are four sequencer steps. MIDI Out: notes on its
  channel to its output's slot, unmatched note offs not sent, note offs on a change of channel and of output (to where the notes
  went), nothing with no output or an unknown one, clock ticks, START first and STOP with the held note let go, and a message due
  a device call after its sample (a fake clock, to the microsecond). The patch file's `map` record: the round trip, a name with
  spaces and `#`, no name, the refusals, a control that is not there, and removing a module.
* `ui_smoke`: *MIDI learn for this knob only* maps the knob in the patch as one undo step, marks it unsaved and is saved; the
  controller turns it with nothing selected and before a global mapping of the same controller; forgetting it, undo and redo.
  The MIDI Out's box: its hint, *No output* and the outputs in its menu, choosing one (an undo step, no setting changed, saved),
  an unplugged one kept in its menu.
* **By hand, through ALSA's *Midi Through*:** a patch whose key script plays three notes into a MIDI Out on channel 3 printed
  them in `aseqdump` (note on, note off, in order), and at exit *all notes off* on the 16 channels released the last one. A start,
  48 ticks and a stop sent with `aseqsend` into a MIDI Clock driving a 4-step sequencer (a step every 6 pulses) whose notes went
  back out through a MIDI Out gave exactly eight notes, the pattern twice from its first step, then the stop.
* A build with `-DPULSERACK_MIDI=OFF` (17 of 17), and the engine tests in Q15.

**Not yet checked:** a real synth on the end of MIDI Out (the timing heard against the rack's own sound), a real device's clock
(a drum machine or a DAW) into MIDI Clock, and a per-patch mapping with the real controller.

**Not done:** a MIDI Out's own clock rate (feed it 24 ppqn); song position; 14-bit controllers; MIDI Out of controls (CC) or
pitch bend; soft takeover and a mark on mapped controls (as before).
