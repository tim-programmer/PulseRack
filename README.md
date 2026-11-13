# PulseRack

A virtual modular rack built on [PulseCoreLib](../PulseCoreLib) (the DSP) and
[gpuic](../gpuic) (the GUI). Modules sit in a rack as panels with knobs, sliders and jacks; you
connect them with cables and hear the result.

The reference for the look and the workflow is Voltage Modular: a module browser on the left
(search, filter, an *Add* button per module), a rack of panels on the right, cables between
jacks, and a toolbar for new/save/open and the audio device.

**Status: milestones 0 to 6 are built**: the gpuic spike, the headless engine, the window, cables, sound (SDL3 output on its
own thread, the computer keyboard as a piano, LEDs and meters), saving and loading, and the polish of milestone 6 (undo, the
sequencer's step grid, the on-screen keyboard, the output device, dropouts). Every PulseCoreLib module is in the rack, the later
ones too (unison oscillator, ladder filter, delay, reverb, and a sampler that plays a WAV file). **MIDI** (docs/midi.md): steps 1 to 8 of 9 are built (the MIDI In and MIDI CC modules, each listening to every input or to one
chosen in its Device box; messages heard at their own sample; MIDI learn by a right-click on a control; inputs are
switched on in the device menu). The name is a placeholder.

## Documents

| Document | Contents |
|----------|----------|
| [docs/architecture.md](docs/architecture.md) | repository layout, how the two libraries are pulled in, audio, threads, sample types, testing |
| [docs/patch-engine.md](docs/patch-engine.md) | the headless part: module descriptors, ports, cables, processing order, the patch file |
| [docs/gui.md](docs/gui.md) | the window: browser, panels, knobs, cable drawing, what gpuic gives us and what is unverified |
| [docs/midi.md](docs/midi.md) | MIDI controllers: libremidi, devices and threads, the MIDI In and MIDI CC modules, MIDI learn, the settings file |
| [docs/gpuic-prompt-4.md](docs/gpuic-prompt-4.md) | a self-contained prompt for a gpuic session: sharp text under a scale (the rack's zoom), drags with any button (done) |

## Try it

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build        # float; add -DPULSERACK_SAMPLE_TYPE=Q15 or Q31
build/src/pulserack examples/sequence.rack                                  # the window, with sound (--no-audio: without)
build/src/rack-render examples/sequence.rack bass.wav 8                     # render a patch to a WAV file
build/src/pulserack-midi --list                                             # the MIDI inputs (a name as argument: print what it sends)
```

`examples/` has five patches (a sequenced bass line, an arpeggio, a filter sweep, a voice for the computer keyboard, a voice
for a MIDI keyboard); they are
built into the program too, under *Open > Examples*. PulseCoreLib and gpuic are fetched from GitHub
(branch `main`); to build against local checkouts, add `-DFETCHCONTENT_SOURCE_DIR_PULSECORELIB=/path/to/PulseCoreLib` and
`-DFETCHCONTENT_SOURCE_DIR_GPUIC=/path/to/gpuic`. gpuic's own dependencies (SDL3, FreeType, HarfBuzz, FriBidi) must be installed
(`-DPULSERACK_GPUIC_DEPENDENCIES=AUTO` lets gpuic download what is missing); `-DPULSERACK_BUILD_GUI=OFF` builds the engine alone.
libremidi is fetched at its release tag `v5.4.3` (`-DFETCHCONTENT_SOURCE_DIR_LIBREMIDI` for a local checkout; `-DPULSERACK_MIDI=OFF`
builds without MIDI). Machine settings (the output device, the MIDI inputs) are kept in `~/.config/pulserack/settings`.
The window tests need a display and are skipped without one.

## Decided

* A **separate repository** that pulls in gpuic and PulseCoreLib; PulseCoreLib is not changed by it.
* **SDL3 audio** for sound output (SDL3 is already a gpuic dependency, so nothing new).
* Design documents first; nothing is built until they are reviewed.
* **Language: C++20.** **The patch engine lives in PulseRack**, not in PulseCoreLib. **A cable that would close a loop is
  refused.** **An input takes one cable** (a Mixer module sums).
* **Libraries**: both are fetched with `FetchContent` from GitHub, branch `main` for now (pin later by changing `GIT_TAG`);
  both are Apache-2.0. A local checkout is used through CMake's `FETCHCONTENT_SOURCE_DIR_*`.
* **GUI** (answered in `gui.md`): a restrained look; free panel placement; no zoom in the first version; modules are added with
  *Add* or by dragging them from the browser onto the rack; cables are drawn by a **new primitive added to gpuic** (`gp_curve`, commit `bc442de`, requested in
  `docs/gpuic-prompt.md`; landed and checked, and the spike re-checked against gpuic `da89032`, see `docs/spike.md`).

## Open questions

Nothing blocks the next step. The smaller questions that remain are at the end of `docs/patch-engine.md` (the ADSR gate
jacks, where the Mixer, Attenuverter and Merge live, polyphony), each with a recommendation.
