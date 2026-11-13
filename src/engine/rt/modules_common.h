// What every module file shares: descriptor builders and the glue modules' entry points.
#pragma once

#include "engine/model/descriptor.h"
#include "engine/rt/sample_ops.h"

namespace rack
{

using Labels = std::span<const std::string_view>;
using ApplyFn = bool (*)(void *, float);

inline ParamDesc param(std::string_view id, std::string_view name, std::string_view unit, float mn,
    float mx, float def, Scale scale, Control control, ApplyFn apply, Labels labels = { })
{
    return ParamDesc { id, name, unit, mn, mx, def, scale, labels, control, apply, false };
}
// A parameter that makes the module again when it changes (ParamDesc::rebuild).
inline ParamDesc rebuild_param(std::string_view id, std::string_view name, std::string_view unit,
    float mn, float mx, float def, Scale scale, Control control, Labels labels = { })
{
    return ParamDesc { id, name, unit, mn, mx, def, scale, labels, control,
        [](void *, float) { return true; }, true };
}

template <class T> inline T *as(void *s) { return static_cast<T *>(s); }

// One module type's descriptor (`destroy` only in RACK_DESC_D, for a module whose init allocates).
#define RACK_DESC(                                                                                 \
    var, id, name, cat, hp, T, ports, params, steps, init, reset, process, apply_step, cap, blurb) \
    const ModuleDesc var = { id, name, cat, hp, sizeof(T), alignof(T), ports, params, steps, init, \
        reset, process, apply_step, cap, blurb, nullptr }
#define RACK_DESC_D(var, id, name, cat, hp, T, ports, params, init, reset, process, destroy, blurb) \
    const ModuleDesc var = { id, name, cat, hp, sizeof(T), alignof(T), ports, params, false, init, \
        reset, process, nullptr, nullptr, blurb, destroy }
inline bool                  ok(pcl_status_t s) { return s == PCL_OK; }

// ---- the glue modules' extra entry points (used by the engine and the tests)
// Schedules a key event for a Keyboard node at an absolute sample position (engine time). False if
// its queue is full.
bool keyboard_push(void *state, uint64_t at, uint8_t type, uint8_t note, uint8_t velocity);
// A MIDI message for a MIDI In or MIDI CC node (the audio thread), with the sample (engine time) it
// takes effect at: it waits in the node's queue until then (docs/midi.md, *Modules* and step 8).
namespace midi
{
    struct Msg;
}
void     midi_in_push(void *state, const midi::Msg &m, uint64_t at);
void     midi_cc_push(void *state, const midi::Msg &m, uint64_t at);
uint32_t midi_in_dropped(const void *state); // notes it had no room for
// The function that takes MIDI messages for a module of this type (the engine gives every message
// to every such module), or null for a module that takes none.
using MidiPushFn = void (*)(void *state, const midi::Msg &m, uint64_t at);
MidiPushFn midi_sink(const ModuleDesc &desc);
// A message a MIDI Out module made in the block just processed: its sample there, the output's
// slot, the bytes.
struct MidiOutMsg
{
    uint16_t offset;
    uint8_t  slot, len;
    uint8_t  bytes[3];
};
// The function that hands over what a module of this type sent in its last block (the engine
// takes them after every block), or null for a module that sends none. Returns how many it wrote.
using MidiTakeFn = size_t (*)(void *state, MidiOutMsg *out, size_t cap);
MidiTakeFn midi_source(const ModuleDesc &desc);
// Has this module type a MIDI device (Node::device): an input it listens to, or an output it sends
// to (sends_midi)?
bool        has_midi_device(const ModuleDesc &desc);
inline bool sends_midi(const ModuleDesc &desc) { return midi_source(desc) != nullptr; }
// The device such a module listens to: a slot (0 ..), -1 for every device, -2 for none (its device
// is not known on this machine); for a MIDI Out, the output it sends to (-1 and -2: none). False
// for a module with no MIDI device. The audio thread.
inline constexpr int kMidiAnyDevice = -1, kMidiNoDevice = -2;
bool                 midi_set_device(const ModuleDesc &desc, void *state, int device);
// How many events the sequencer, arpeggiator or clock dropped for lack of room (telemetry for the
// panel's LED).
uint32_t dropped_events(const ModuleDesc &desc, const void *state);
// An oscilloscope's newest frame, if one came since the last call (the UI thread: the reader's side
// of its triple buffer); false for other modules.
struct ScopeFrame;
bool scope_take(const ModuleDesc &desc, void *state, ScopeFrame &out);
// The step a sequencer played last (telemetry for its step grid's playhead); -1 for other modules.
int32_t module_position(const ModuleDesc &desc, const void *state);

// A Sampler: does this module type play a sample file (Node::sample)? And the file's frames for
// one, after its init (the UI thread): the module keeps a copy; false if it cannot play them.
struct SampleData;
bool takes_sample(const ModuleDesc &desc);
bool load_sample(const ModuleDesc &desc, void *state, const SampleData &data);

// ---- every module descriptor (defined in modules_lib.cpp and modules_glue.cpp)
extern const ModuleDesc kOscDesc, kNoiseDesc, kSahDesc, kLfoDesc, kAdsrDesc, kVcaDesc, kSvfDesc,
    kBiquadDesc, kClockDesc, kArpDesc, kSeqDesc;
extern const ModuleDesc kLadderDesc, kUnisonDesc, kDelayDesc, kReverbDesc, kSamplerDesc;
extern const ModuleDesc kKickDesc, kSnareDesc, kMetalDesc;
extern const ModuleDesc kSyncVoiceDesc, kStackVoiceDesc, kFmVoiceDesc, kAirVoiceDesc,
    kStringVoiceDesc, kGrainVoiceDesc;
bool grain_load(void *state, const SampleData &data); // (modules_voices.cpp)
extern const ModuleDesc kKeyboardDesc, kAudioOutDesc, kMixerDesc, kAttenuverterDesc,
    kNoteToGateDesc, kMergeDesc, kScopeDesc, kMidiInDesc, kMidiCcDesc, kMidiClockDesc, kMidiOutDesc;

} // namespace rack
