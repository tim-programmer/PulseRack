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
    return ParamDesc { id, name, unit, mn, mx, def, scale, labels, control, PanelPos { }, apply };
}

template <class T> inline T *as(void *s) { return static_cast<T *>(s); }
inline bool                  ok(pcl_status_t s) { return s == PCL_OK; }

// ---- the glue modules' extra entry points (used by the engine and the tests)
// Schedules a key event for a Keyboard node at an absolute sample position (engine time). False if
// its queue is full.
bool keyboard_push(void *state, uint64_t at, uint8_t type, uint8_t note, uint8_t velocity);
// How many events the sequencer, arpeggiator or clock dropped for lack of room (telemetry for the
// panel's LED).
uint32_t dropped_events(const ModuleDesc &desc, const void *state);

// ---- every module descriptor (defined in modules_lib.cpp and modules_glue.cpp)
extern const ModuleDesc kOscDesc, kNoiseDesc, kSahDesc, kLfoDesc, kAdsrDesc, kVcaDesc, kSvfDesc,
    kBiquadDesc, kClockDesc, kArpDesc, kSeqDesc;
extern const ModuleDesc kKeyboardDesc, kAudioOutDesc, kMixerDesc, kAttenuverterDesc,
    kNoteToGateDesc, kMergeDesc;

} // namespace rack
