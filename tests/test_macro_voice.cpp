#include "testutil.h"

#include <array>

using namespace rt_util;

TEST(macro_voice_models_have_distinct_audio_and_aux_outputs)
{
    std::array<std::vector<float>, 4> main;
    for (size_t model = 0; model < main.size(); ++model)
    {
        Rig        r;
        const auto keys = r.add("keyboard");
        const auto voice = r.add("macro_voice");
        const auto output = r.add("out");
        CHECK(r.cable(keys, "notes", voice, "note") == Err::Ok);
        CHECK(r.cable(voice, "out", output, "l") == Err::Ok);
        CHECK(r.cable(voice, "aux", output, "r") == Err::Ok);
        r.param(voice, "model", static_cast<float>(model));
        r.param(voice, "decay", 100);
        r.key(keys, 0.01, true, 57);
        r.key(keys, 0.12, false, 57);
        CHECK(r.compile());
        r.render(kRate / 2);
        CHECK(peak(r.L, 0, 480) == 0);
        CHECK(peak(r.R, 0, 480) == 0);
        CHECK(rms(r.L, 1440, 4800) > 0.02);
        CHECK(rms(r.R, 1440, 4800) > 0.02);
        CHECK(!same_bits(r.L, r.R));
        CHECK(peak(r.L, 20000) < 0.001);
        for (size_t prev = 0; prev < model; ++prev)
            CHECK(!same_bits(r.L, main[prev]));
        main[model] = std::move(r.L);
    }
}

TEST(macro_voice_trigger_and_drone_can_sound_without_note_events)
{
    Rig        r;
    const auto keys = r.add("keyboard");
    const auto gate = r.add("note_to_gate");
    const auto voice = r.add("macro_voice");
    const auto output = r.add("out");
    CHECK(r.cable(keys, "notes", gate, "notes") == Err::Ok);
    CHECK(r.cable(gate, "gate", voice, "trig") == Err::Ok);
    CHECK(r.cable(voice, "out", output, "l") == Err::Ok);
    r.param(voice, "decay", 80);
    r.key(keys, 0.02, true, 60);
    r.key(keys, 0.4, false, 60);
    CHECK(r.compile());
    r.render(kRate / 2);
    CHECK(peak(r.L, 0, 960) == 0);
    CHECK(rms(r.L, 1080, 2000) > 0.01);
    CHECK(peak(r.L, 14400) < 0.001);
    r.param(voice, "drone", 1);
    r.render(kRate / 10);
    CHECK(rms(r.L, 24500) > 0.02);
}
