#include "testutil.h"

#include <string>

using namespace rt_util;

namespace
{
// An FM Synth played from a Keyboard, both outputs to the Audio Output.
struct FmRig
{
    Rig      r;
    uint32_t keys = r.add("keyboard"), synth = r.add("fm_synth"), out = r.add("out");
    FmRig(size_t preset)
    {
        CHECK(r.cable(keys, "notes", synth, "note") == Err::Ok);
        CHECK(r.cable(synth, "l", out, "l") == Err::Ok);
        CHECK(r.cable(synth, "r", out, "r") == Err::Ok);
        const auto values = preset_values(kFmSynthDesc, preset);
        for (size_t i = 0; i < values.size(); i++)
            CHECK(r.e.set_param(synth, i, values[i]) == Err::Ok);
    }
    void dry() // no chorus or delay, so a note's pitch and end are its own
    {
        r.param(synth, "chorus", 0);
        r.param(synth, "delay_mix", 0);
    }
};
constexpr size_t kInit = 11, kArpPluck = 10;
} // namespace

TEST(fm_synth_presets_name_real_settings)
{
    CHECK(kFmSynthDesc.presets.size() == kFmSynthDesc.params[0].labels.size());
    CHECK(preset_param(kFmSynthDesc) == 0);
    for (size_t p = 0; p < kFmSynthDesc.presets.size(); p++)
    {
        CHECK(kFmSynthDesc.presets[p].name == kFmSynthDesc.params[0].labels[p]);
        for (const PresetValue &v : kFmSynthDesc.presets[p].values)
        {
            const int i = find_param(kFmSynthDesc, std::string(v.param).c_str());
            CHECK(i > 0);
            if (i > 0)
                CHECK(v.value >= kFmSynthDesc.params[(size_t)i].min
                    && v.value <= kFmSynthDesc.params[(size_t)i].max);
        }
        // every preset but the first (the defaults) sets all four operators
        if (p > 0)
            CHECK(kFmSynthDesc.presets[p].values.size() >= 24);
    }
    const auto bell = preset_values(kFmSynthDesc, 1);
    CHECK(bell[0] == 1);
    CHECK(bell[(size_t)find_param(kFmSynthDesc, "algorithm")] == 3);
    CHECK(bell[(size_t)find_param(kFmSynthDesc, "vibrato")] == 0); // not in the preset: default
    const auto defaults = preset_values(kFmSynthDesc, 0);
    for (size_t i = 0; i < defaults.size(); i++)
        CHECK(defaults[i] == kFmSynthDesc.params[i].def);
}

TEST(fm_synth_plays_in_tune_and_stops_after_its_release)
{
    FmRig f(kInit); // one sine, the first operator
    f.dry();
    f.r.key(f.keys, 0, true, 69);
    f.r.key(f.keys, 0.5, false, 69);
    CHECK(f.r.compile());
    f.r.render(kRate);
    CHECK(std::abs(frequency(f.r.L, 4800, 19200) - 440) < 1);
    CHECK(rms(f.r.L, 4800, 19200) > 0.05);
    CHECK(rms(f.r.L, 40000, 48000) < 1e-4); // 100 ms release, long gone
}

TEST(fm_synth_is_polyphonic_and_steals_the_oldest_voice)
{
    auto level = [](int notes)
    {
        FmRig f(kInit);
        f.dry();
        for (int k = 0; k < notes; k++)
            f.r.key(f.keys, 0.01 * k, true, (uint8_t)(48 + 3 * k));
        CHECK(f.r.compile());
        f.r.render(kRate / 2);
        return rms(f.r.L, 12000, 24000);
    };
    const double one = level(1), three = level(3), ten = level(10);
    CHECK(three > one * 1.4); // three notes sound together (not one replacing the other)
    CHECK(ten > 0.05 && ten < 1); // ten keys on eight voices: still sounds, and limited
}

TEST(fm_synth_brightness_adds_harmonics)
{
    // The E. piano at no brightness is its carriers' sines; at full it is much brighter.
    auto zero_crossings = [](float bright)
    {
        FmRig f(0);
        f.dry();
        f.r.param(f.synth, "brightness", bright);
        f.r.key(f.keys, 0, true, 57);
        CHECK(f.r.compile());
        f.r.render(kRate / 4);
        int crossings = 0;
        for (size_t i = 1201; i < 4800; i++)
            crossings += (f.r.L[i - 1] < 0) != (f.r.L[i] < 0);
        return crossings;
    };
    CHECK(zero_crossings(2) > zero_crossings(0) + 4);
}

TEST(fm_synth_arpeggiates_held_keys_on_its_clock)
{
    // Arp pluck: a held chord becomes single notes, a 16th apart at 120 bpm (6000 samples).
    auto onsets = [](bool external)
    {
        FmRig f(kArpPluck);
        f.dry();
        f.r.param(f.synth, "external", external ? 1.f : 0.f); // nothing on CLOCK: no pulses
        f.r.key(f.keys, 0, true, 60);
        f.r.key(f.keys, 0, true, 64);
        f.r.key(f.keys, 0, true, 67);
        CHECK(f.r.compile());
        f.r.render(kRate);
        int    count = 0;
        double before = 0;
        for (size_t w = 0; w + 600 <= f.r.L.size(); w += 600)
        {
            const double now = rms(f.r.L, w, w + 600);
            count += now > 0.02 && now > before * 2;
            before = now;
        }
        return std::make_pair(count, rms(f.r.L));
    };
    const auto internal = onsets(false);
    CHECK(internal.first >= 6); // eight steps in the second: most of them seen as a new note
    const auto external = onsets(true);
    CHECK(external.second < 1e-4); // the keys alone play nothing while it waits for a clock
}
