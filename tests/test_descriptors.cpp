#include "testutil.h"

#include <set>

using namespace rt_util;

TEST(descriptors_ids_are_unique_and_well_formed)
{
    std::set<std::string_view> ids;
    for (const ModuleDesc *d : all_modules())
    {
        CHECK(ids.insert(d->id).second);
        CHECK(!d->name.empty() && !d->category.empty());
        CHECK(d->width_hp > 0);
        CHECK(d->state_size > 0 && d->state_align > 0
            && (d->state_align & (d->state_align - 1)) == 0);
        CHECK(d->init && d->process);
        CHECK(!d->ports.empty());
        CHECK(d->has_steps == (d->apply_step != nullptr));
        std::set<std::string_view> port_ids;
        for (const PortDesc &p : d->ports)
            CHECK(port_ids.insert(p.id).second && !p.name.empty());
        std::set<std::string_view> param_ids;
        for (const ParamDesc &p : d->params)
        {
            CHECK(param_ids.insert(p.id).second && !p.name.empty() && p.apply);
            CHECK(p.min < p.max);
            CHECK(p.def >= p.min && p.def <= p.max);
            if (p.scale == Scale::Log)
                CHECK(p.min > 0);
            if (p.scale == Scale::Int || p.scale == Scale::Enum || p.scale == Scale::Toggle)
                CHECK(p.min == std::round(p.min) && p.max == std::round(p.max)
                    && p.def == std::round(p.def));
            if (p.scale == Scale::Enum)
                CHECK(p.min == 0 && p.labels.size() == (size_t)p.max + 1);
            if (p.scale == Scale::Toggle)
                CHECK(p.min == 0 && p.max == 1);
        }
    }
    CHECK(ids.size() == 18);
}

// The wrappers index their jacks by slot number; the descriptors list them by name. They must
// agree.
TEST(descriptors_slot_counts_match_the_wrappers)
{
    struct Expect
    {
        const char *id;
        size_t      sig_in, sig_out, evt_in, evt_out;
    };
    const Expect table[] = {
        { "osc", 3, 2, 1, 0 },
        { "noise", 0, 1, 0, 0 },
        { "sah", 1, 1, 1, 0 },
        { "lfo", 1, 1, 0, 0 },
        { "adsr", 1, 1, 1, 0 },
        { "vca", 2, 1, 0, 0 },
        { "svf", 2, 3, 0, 0 },
        { "biquad", 2, 1, 0, 0 },
        { "clock", 0, 0, 0, 1 },
        { "arp", 0, 0, 1, 1 },
        { "seq", 0, 0, 1, 1 },
        { "keyboard", 0, 0, 0, 1 },
        { "out", 2, 0, 0, 0 },
        { "mixer", 4, 1, 0, 0 },
        { "attenuverter", 1, 1, 0, 0 },
        { "note_to_gate", 0, 1, 1, 0 },
        { "merge", 0, 0, 2, 1 },
        { "scope", 2, 0, 0, 0 },
    };
    for (const Expect &x : table)
    {
        const ModuleDesc *d = find_module(x.id);
        CHECK(d != nullptr);
        if (!d)
            continue;
        const PortSlots n = count_slots(*d);
        CHECK(n.sig_in == x.sig_in && n.sig_out == x.sig_out && n.evt_in == x.evt_in
            && n.evt_out == x.evt_out);
        // slot numbers run 0..n-1 within each kind and direction, in listed order
        std::set<std::tuple<int, int, size_t>> seen;
        for (size_t i = 0; i < d->ports.size(); i++)
            CHECK(seen.insert({ (int)d->ports[i].kind, (int)d->ports[i].is_output, slot_of(*d, i) })
                    .second);
    }
    CHECK(sizeof(table) / sizeof(table[0]) == all_modules().size());
}

TEST(descriptors_every_module_initialises_at_common_sample_rates)
{
    for (uint32_t rate : { 8000u, 44100u, 48000u, 96000u, 192000u })
        for (const ModuleDesc *d : all_modules())
        {
            std::vector<float> p;
            for (const ParamDesc &pd : d->params)
                p.push_back(pd.def);
            alignas(std::max_align_t) static unsigned char mem[1 << 16];
            CHECK(d->state_size <= sizeof mem && d->state_align <= alignof(std::max_align_t));
            std::memset(mem, 0, sizeof mem);
            CHECK(d->init(mem, NodeContext { rate }, p.data()));
        }
}

// Every control must accept its minimum, default and maximum (applied in list order, as a patch
// file does).
TEST(descriptors_every_param_applies_at_min_def_and_max)
{
    for (uint32_t rate : { 44100u, 48000u, 96000u })
        for (const ModuleDesc *d : all_modules())
            for (int which = 0; which < 3; which++)
            {
                std::vector<float> p;
                for (const ParamDesc &pd : d->params)
                    p.push_back(pd.def);
                alignas(std::max_align_t) static unsigned char mem[1 << 16];
                std::memset(mem, 0, sizeof mem);
                CHECK(d->init(mem, NodeContext { rate }, p.data()));
                for (const ParamDesc &pd : d->params)
                {
                    const float v = which == 0 ? pd.min : which == 1 ? pd.def : pd.max;
                    const bool  applied = pd.apply(mem, v);
                    if (!applied)
                        std::printf("    %s.%s refused %g at %u Hz\n", std::string(d->id).c_str(),
                            std::string(pd.id).c_str(), v, rate);
                    CHECK(applied);
                }
            }
}

TEST(descriptors_event_capacity_rules)
{
    for (const ModuleDesc *d : all_modules())
    {
        if (!d->evt_out_cap)
            continue;
        for (size_t in : { (size_t)0, (size_t)1, (size_t)64, (size_t)128, (size_t)4096 })
        {
            const size_t c = d->evt_out_cap(in);
            CHECK(c >= 64 && c <= 16384);
        }
    }
}

TEST(clamp_param_rules)
{
    const ParamDesc &wave = find_module("osc")->params[0];  // Enum 0..3
    const ParamDesc &freq = find_module("osc")->params[1];  // Log 20..8000, default 220
    const ParamDesc &run = find_module("clock")->params[2]; // Toggle
    CHECK(clamp_param(wave, 1.4f) == 1.0f && clamp_param(wave, 1.6f) == 2.0f
        && clamp_param(wave, -3) == 0 && clamp_param(wave, 99) == 3);
    CHECK(clamp_param(freq, 1) == 20 && clamp_param(freq, 1e9f) == 8000
        && clamp_param(freq, 440.5f) == 440.5f);
    CHECK(clamp_param(freq, NAN) == 220);
    CHECK(clamp_param(run, 0.4f) == 0 && clamp_param(run, 0.6f) == 1
        && clamp_param(run, INFINITY) == 1);
}
