#include "engine/model/registry.h"

#include "engine/rt/modules_common.h"

#include <algorithm>
#include <array>

namespace rack
{

std::span<const ModuleDesc *const> all_modules()
{
    static const std::array<const ModuleDesc *, 42> table = {
        &kOscDesc,
        &kUnisonDesc,
        &kSamplerDesc,
        &kNoiseDesc,
        &kKickDesc,
        &kSnareDesc,
        &kMetalDesc,
        &kSyncVoiceDesc,
        &kStackVoiceDesc,
        &kFmVoiceDesc,
        &kAirVoiceDesc,
        &kStringVoiceDesc,
        &kGrainVoiceDesc,
        &kChordVoiceDesc,
        &kMacroVoiceDesc,
        &kFmSynthDesc,
        &kSahDesc,
        &kLfoDesc,
        &kAdsrDesc,
        &kVcaDesc,
        &kSvfDesc,
        &kBiquadDesc,
        &kLadderDesc,
        &kDelayDesc,
        &kReverbDesc,
        &kClockDesc,
        &kArpDesc,
        &kSeqDesc,
        &kMelodyDesc,
        &kKeyboardDesc,
        &kMidiInDesc,
        &kMidiCcDesc,
        &kMidiClockDesc,
        &kMidiOutDesc,
        &kHostClockDesc,
        &kAudioInDesc,
        &kAudioOutDesc,
        &kMixerDesc,
        &kAttenuverterDesc,
        &kNoteToGateDesc,
        &kMergeDesc,
        &kScopeDesc,
    };
    return table;
}

const ModuleDesc *find_module(std::string_view id)
{
    for (const ModuleDesc *d : all_modules())
        if (d->id == id)
            return d;
    return nullptr;
}

int find_port(const ModuleDesc &d, std::string_view id)
{
    for (size_t i = 0; i < d.ports.size(); i++)
        if (d.ports[i].id == id)
            return (int)i;
    return -1;
}

int find_param(const ModuleDesc &d, std::string_view id)
{
    for (size_t i = 0; i < d.params.size(); i++)
        if (d.params[i].id == id)
            return (int)i;
    return -1;
}

int preset_param(const ModuleDesc &d)
{
    if (d.presets.empty())
        return -1;
    for (size_t i = 0; i < d.params.size(); i++)
        if (d.params[i].id == "preset")
            return (int)i;
    return -1;
}

std::vector<float> preset_values(const ModuleDesc &d, size_t index)
{
    std::vector<float> values;
    for (const ParamDesc &p : d.params)
        values.push_back(p.def);
    if (d.presets.empty())
        return values;
    index = std::min(index, d.presets.size() - 1);
    for (const PresetValue &v : d.presets[index].values)
        for (size_t i = 0; i < d.params.size(); i++)
            if (d.params[i].id == v.param)
                values[i] = std::clamp(v.value, d.params[i].min, d.params[i].max);
    if (const int p = preset_param(d); p >= 0)
        values[(size_t)p] = (float)index;
    return values;
}

PortSlots count_slots(const ModuleDesc &d)
{
    PortSlots n;
    for (const PortDesc &p : d.ports)
    {
        size_t &c = p.kind == Kind::Signal ? (p.is_output ? n.sig_out : n.sig_in)
                                           : (p.is_output ? n.evt_out : n.evt_in);
        c++;
    }
    return n;
}

size_t slot_of(const ModuleDesc &d, size_t port_index)
{
    const PortDesc &me = d.ports[port_index];
    size_t          slot = 0;
    for (size_t i = 0; i < port_index; i++)
        if (d.ports[i].kind == me.kind && d.ports[i].is_output == me.is_output)
            slot++;
    return slot;
}

} // namespace rack
