# PulseRack

A virtual modular rack built on [PulseCoreLib](../PulseCoreLib) (the DSP) and
[gpuic](../gpuic) (the GUI). Modules sit in a rack as panels with knobs, sliders and jacks; you
connect them with cables and hear the result.

The reference for the look and the workflow is Voltage Modular: a module browser on the left
(search, filter, an *Add* button per module), a rack of panels on the right, cables between
jacks, and a toolbar for new/save/open and the audio device.

**Status: the gpuic spike (milestone 0), the headless engine (milestone 1) and the window without cables (milestone 2) are
built. No cables and no sound yet** (milestones 3 and 4). The name is a placeholder.

## Documents

| Document | Contents |
|----------|----------|
| [docs/architecture.md](docs/architecture.md) | repository layout, how the two libraries are pulled in, audio, threads, sample types, testing |
| [docs/patch-engine.md](docs/patch-engine.md) | the headless part: module descriptors, ports, cables, processing order, the patch file |
| [docs/gui.md](docs/gui.md) | the window: browser, panels, knobs, cable drawing, what gpuic gives us and what is unverified |
| [docs/gpuic-prompt.md](docs/gpuic-prompt.md) | a self-contained prompt for a gpuic session: a stroked-curve element for cables |

## Try it

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build        # float; add -DPULSERACK_SAMPLE_TYPE=Q15 or Q31
build/src/pulserack examples/sequence.rack                                  # the window: browse, add, move, turn knobs
build/src/rack-render examples/sequence.rack bass.wav 8                     # render a patch to a WAV file
```

`examples/` has three patches (a sequenced bass line, an arpeggio, a filter sweep). PulseCoreLib and gpuic are fetched from GitHub
(branch `main`); to build against local checkouts, add `-DFETCHCONTENT_SOURCE_DIR_PULSECORELIB=/path/to/PulseCoreLib` and
`-DFETCHCONTENT_SOURCE_DIR_GPUIC=/path/to/gpuic`. gpuic's own dependencies (SDL3, FreeType, HarfBuzz, FriBidi) must be installed
(`-DPULSERACK_GPUIC_DEPENDENCIES=AUTO` lets gpuic download what is missing); `-DPULSERACK_BUILD_GUI=OFF` builds the engine alone.
The window tests need a display and are skipped without one.

## Decided

* A **separate repository** that pulls in gpuic and PulseCoreLib; PulseCoreLib is not changed by it.
* **SDL3 audio** for sound output (SDL3 is already a gpuic dependency, so nothing new).
* Design documents first; nothing is built until they are reviewed.
* **Language: C++20.** **The patch engine lives in PulseRack**, not in PulseCoreLib. **A cable that would close a loop is
  refused.** **An input takes one cable** (a Mixer module sums).
* **Libraries**: both are fetched with `FetchContent` from GitHub, branch `main` for now (pin later by changing `GIT_TAG`);
  both are Apache-2.0. A local checkout is used through CMake's `FETCHCONTENT_SOURCE_DIR_*`.
* **GUI** (answered in `gui.md`): a restrained look; free panel placement; no zoom and no drag-from-browser in the first
  version; cables are drawn by a **new primitive added to gpuic** (`gp_curve`, commit `bc442de`, requested in
  `docs/gpuic-prompt.md`; landed and checked, and the spike re-checked against gpuic `771626e`, see `docs/spike.md`).

## Open questions

Nothing blocks the next step. The smaller questions that remain are at the end of `docs/patch-engine.md` (the ADSR gate
jacks, where the Mixer, Attenuverter and Merge live, polyphony), each with a recommendation.
