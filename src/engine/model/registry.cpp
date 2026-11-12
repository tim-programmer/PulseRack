#include "engine/model/registry.h"

#include "engine/rt/modules_common.h"

#include <array>

namespace rack
{

std::span<const ModuleDesc *const> all_modules()
{
    static const std::array<const ModuleDesc *, 19> table = {
        &kOscDesc,
        &kNoiseDesc,
        &kSahDesc,
        &kLfoDesc,
        &kAdsrDesc,
        &kVcaDesc,
        &kSvfDesc,
        &kBiquadDesc,
        &kClockDesc,
        &kArpDesc,
        &kSeqDesc,
        &kKeyboardDesc,
        &kMidiInDesc,
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
