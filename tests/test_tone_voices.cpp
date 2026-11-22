// Harmonic Voice, Cluster Voice and Bell Voice (they replace Macro Voice and its Engine switch).
#include "testutil.h"

#include <array>

using namespace rt_util;

namespace
{
struct ToneRig
{
    Rig      r;
    uint32_t keys, voice, out;
    ToneRig(const char *type, const char *a, const char *b)
        : keys(r.add("keyboard"))
        , voice(r.add(type))
        , out(r.add("out"))
    {
        CHECK(r.cable(keys, "notes", voice, "note") == Err::Ok);
        CHECK(r.cable(voice, a, out, "l") == Err::Ok);
        CHECK(r.cable(voice, b, out, "r") == Err::Ok);
    }
};
} // namespace

TEST(tone_voices_each_sound_on_both_outputs_and_differ)
{
    const char *types[][3] = { { "harmonic_voice", "out", "sub" }, { "cluster_voice", "l", "r" },
        { "bell_voice", "out", "metal" } };
    std::array<std::vector<float>, 3> heard;
    for (size_t t = 0; t < 3; t++)
    {
        ToneRig v(types[t][0], types[t][1], types[t][2]);
        v.r.param(v.voice, "decay", t == 2 ? 1000 : 100); // (a bell rings; letting go damps it)
        v.r.key(v.keys, 0.01, true, 57);
        v.r.key(v.keys, 0.12, false, 57);
        CHECK(v.r.compile());
        v.r.render(kRate / 2);
        CHECK(peak(v.r.L, 0, 480) == 0 && peak(v.r.R, 0, 480) == 0);
        CHECK(rms(v.r.L, 1440, 4800) > 0.02);
        CHECK(rms(v.r.R, 1440, 4800) > 0.005);
        CHECK(!same_bits(v.r.L, v.r.R));
        CHECK(peak(v.r.L, 20000) < 0.001);
        for (size_t before = 0; before < t; before++)
            CHECK(!same_bits(v.r.L, heard[before]));
        heard[t] = std::move(v.r.L);
    }
}

TEST(tone_voices_trigger_and_drone_sound_without_notes)
{
    for (const char *type : { "harmonic_voice", "cluster_voice", "bell_voice" })
    {
        Rig        r;
        const auto keys = r.add("keyboard");
        const auto gate = r.add("note_to_gate");
        const auto voice = r.add(type);
        const auto output = r.add("out");
        CHECK(r.cable(keys, "notes", gate, "notes") == Err::Ok);
        CHECK(r.cable(gate, "gate", voice, "trig") == Err::Ok);
        CHECK(r.cable(voice, std::string(type) == "cluster_voice" ? "l" : "out", output, "l")
            == Err::Ok);
        r.param(voice, "decay", 80);
        r.key(keys, 0.02, true, 60);
        r.key(keys, 0.4, false, 60);
        CHECK(r.compile());
        r.render(kRate / 2);
        CHECK(peak(r.L, 0, 960) == 0);
        CHECK(rms(r.L, 1080, 2000) > 0.01);
        CHECK(peak(r.L, 14400) < 0.001);
        if (std::string(type) == "bell_voice")
            continue; // (a bell has no Drone)
        r.param(voice, "drone", 1);
        r.render(kRate / 10);
        CHECK(rms(r.L, 24500) > 0.02);
    }
}

TEST(bell_voice_dies_away_while_held_metal_first_and_damps_when_let_go)
{
    auto ring = [](double release_at)
    {
        ToneRig v("bell_voice", "out", "metal");
        v.r.param(v.voice, "decay", 1500);
        v.r.key(v.keys, 0, true, 69);
        v.r.key(v.keys, release_at, false, 69);
        CHECK(v.r.compile());
        v.r.render(kRate);
        return std::make_pair(v.r.L, v.r.R);
    };
    const auto [body, metal] = ring(0.9); // held for most of the second
    const double body_early = rms(body, 480, 4800), body_late = rms(body, 24000, 28800);
    const double metal_early = rms(metal, 480, 4800), metal_late = rms(metal, 24000, 28800);
    CHECK(body_late < body_early * 0.5);                      // a held key does not hold it
    CHECK(metal_late / metal_early < body_late / body_early); // the metal goes first
    const auto [damped, ignored] = ring(0.1);
    CHECK(rms(damped, 24000, 28800) < body_late * 0.1); // let go: a hand on the bell
}
