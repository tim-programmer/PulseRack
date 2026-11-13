// The drum modules in real patches. The Kick: silent until a hit, which starts on its own sample;
// the pitch falls from the sweep to the tune; the decay, the accent and the DECAY input do what
// their names say; a second hit starts the sound over.
#include "testutil.h"

using namespace rt_util;

namespace
{
// A Kick hit by a scripted key at each of `times` (seconds) with `velocity`, into the output.
struct KickRig
{
    Rig      r;
    uint32_t kb, kick, out;
    KickRig(std::initializer_list<double> times, uint8_t velocity = 127)
    {
        kb = r.add("keyboard");
        kick = r.add("kick");
        out = r.add("out");
        r.param(out, "level", 1);
        CHECK(r.cable(kb, "notes", kick, "note") == Err::Ok
            && r.cable(kick, "out", out, "l") == Err::Ok);
        for (double t : times)
        {
            r.key(kb, t, true, 36, velocity);
            r.key(kb, t + 0.01, false, 36);
        }
    }
    void param(const char *id, float v) { r.param(kick, id, v); }
    void run(double seconds)
    {
        CHECK(r.compile());
        r.render((size_t)(seconds * kRate));
    }
    const std::vector<float> &L() const { return r.L; }
};
size_t at(double seconds) { return (size_t)(seconds * kRate); }
} // namespace

TEST(kick_is_silent_until_hit_and_starts_on_the_hit_sample)
{
    KickRig k({ 0.1 });
    k.run(0.5);
    const size_t hit = at(0.1);
    CHECK(peak(k.L(), 0, hit) == 0.f);
    CHECK(peak(k.L(), hit, hit + at(0.01)) > 0.5f); // loud at once
    // the first audible sample is within a couple of samples of the hit (the body's one-sample
    // latency)
    size_t first = hit;
    while (first < k.L().size() && std::fabs(k.L()[first]) < 1e-4f)
        first++;
    CHECK(first <= hit + 3);
    CHECK(k.r.e.apply_failures() == 0 && k.r.e.nonfinite_samples() == 0);
}

TEST(kick_pitch_falls_from_the_sweep_to_the_tune)
{
    KickRig k({ 0.05 });
    k.param("tune", 50);
    k.param("sweep", 2);        // starts two octaves up: 200 Hz
    k.param("sweep_time", 20);
    k.param("punch", 0);        // the body alone
    k.param("drive", 0);
    k.param("decay", 2000);
    k.run(1.0);
    // the first cycle (the sine starts at phase 0, so it ends at the first rising zero crossing)
    size_t end = at(0.05) + 4;
    while (end < k.L().size() && !(k.L()[end - 1] < 0 && k.L()[end] >= 0))
        end++;
    const double early = (double)kRate / (double)(end - at(0.05)),
                 late = frequency(k.L(), at(0.3), at(0.9));
    CHECK(early > 120 && early < 200); // well above the tune at first
    CHECK(late > 48 && late < 52);  // and settled on it
}

TEST(kick_decay_sets_the_length)
{
    float tail[2] = { };
    int   i = 0;
    for (float decay : { 100.f, 1000.f })
    {
        KickRig k({ 0.05 });
        k.param("decay", decay);
        k.run(1.0);
        tail[i++] = peak(k.L(), at(0.4), at(0.5));
    }
    CHECK(tail[0] < 1e-3f);         // 100 ms: long gone at 0.35 s after the hit
    CHECK(tail[1] > 0.05f);         // 1 s: still sounding
}

TEST(kick_accent_follows_velocity_as_much_as_the_knob_says)
{
    auto level = [](uint8_t velocity, float accent)
    {
        KickRig k({ 0.05 }, velocity);
        k.param("accent", accent);
        k.param("drive", 0);
        k.run(0.3);
        return peak(k.L(), at(0.05), at(0.1));
    };
    const float loud = level(127, 1), soft = level(32, 1);
    CHECK(loud > 0.7f && soft < loud * 0.4f);
    CHECK(std::fabs(level(127, 0) - level(32, 0)) < 1e-3f); // accent 0: velocity does nothing
}

TEST(kick_a_second_hit_starts_it_over)
{
    KickRig k({ 0.05, 0.25 });
    k.param("decay", 150); // the first is gone before the second
    k.param("punch", 0);   // (the click is noise: different each time)
    k.run(0.5);
    const float first = peak(k.L(), at(0.05), at(0.06)), second = peak(k.L(), at(0.25), at(0.26));
    CHECK(std::fabs(first - second) < 0.05f * first);
    // and the same samples, since the phase is put back to 0: the two hits' first 20 ms agree
    double diff = 0;
    for (size_t i = 0; i < at(0.02); i++)
        diff = std::max(diff, (double)std::fabs(k.L()[at(0.05) + i] - k.L()[at(0.25) + i]));
    CHECK(diff < 0.01);
}

TEST(kick_trig_input_and_decay_input)
{
    // a gate into TRIG hits on its rising edge only; DECAY at +1 makes the tail four times as long
    auto tail = [](float decay_cv)
    {
        Rig            r;
        const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate"), kick = r.add("kick"),
                       a = r.add("attenuverter"), out = r.add("out");
        r.param(a, "scale", 0);
        r.param(a, "offset", decay_cv);
        r.param(kick, "decay", 100);
        r.param(out, "level", 1);
        CHECK(r.cable(kb, "notes", g, "notes") == Err::Ok
            && r.cable(g, "gate", kick, "trig") == Err::Ok
            && r.cable(a, "out", kick, "decay") == Err::Ok
            && r.cable(kick, "out", out, "l") == Err::Ok);
        r.key(kb, 0.05, true, 60);
        r.key(kb, 0.5, false, 60); // a long gate: one hit, not one per block
        CHECK(r.compile());
        r.render(kRate);
        CHECK(peak(r.L, 0, at(0.05)) == 0.f && peak(r.L, at(0.05), at(0.06)) > 0.5f);
        return peak(r.L, at(0.25), at(0.3));
    };
    CHECK(tail(0) < 1e-3f);  // 100 ms
    CHECK(tail(1) > 0.01f);  // 400 ms
}

// ---- Snare / Clap and Metal
namespace
{
// A drum of `type` hit by notes (note, seconds), into the output; returns the left channel.
struct DrumRig
{
    Rig      r;
    uint32_t kb, drum, out;
    explicit DrumRig(const char *type)
    {
        kb = r.add("keyboard");
        drum = r.add(type);
        out = r.add("out");
        r.param(out, "level", 1);
        CHECK(r.cable(kb, "notes", drum, "note") == Err::Ok
            && r.cable(drum, "out", out, "l") == Err::Ok);
    }
    void hit(double t, uint8_t note = 38, uint8_t vel = 127)
    {
        r.key(kb, t, true, note, vel);
        r.key(kb, t + 0.005, false, note);
    }
    const std::vector<float> &run(double seconds)
    {
        CHECK(r.compile());
        r.render((size_t)(seconds * kRate));
        CHECK(r.e.nonfinite_samples() == 0 && r.e.apply_failures() == 0);
        return r.L;
    }
};
// The level in 1 ms windows from `from` for `ms` windows.
std::vector<float> envelope(const std::vector<float> &v, size_t from, int ms)
{
    std::vector<float> e;
    const size_t       w = kRate / 1000;
    for (int i = 0; i < ms; i++)
        e.push_back(peak(v, from + (size_t)i * w, from + (size_t)(i + 1) * w));
    return e;
}
} // namespace

TEST(snare_is_silent_until_hit_and_its_decay_sets_its_length)
{
    float tail[2];
    int   k = 0;
    for (float decay : { 60.f, 600.f })
    {
        DrumRig d("snare");
        d.r.param(d.drum, "decay", decay);
        d.r.param(d.drum, "body", 40);
        d.hit(0.05);
        const auto &L = d.run(0.6);
        CHECK(peak(L, 0, at(0.05)) == 0.f && peak(L, at(0.05), at(0.06)) > 0.4f);
        tail[k++] = peak(L, at(0.3), at(0.4));
    }
    CHECK(tail[0] < 1e-3f && tail[1] > 0.02f);
}

TEST(snare_body_is_tuned)
{
    DrumRig d("snare");
    d.r.param(d.drum, "tune", 200);
    d.r.param(d.drum, "snap", 0); // the body alone
    d.r.param(d.drum, "body", 400);
    d.hit(0.05);
    const auto  &L = d.run(0.5);
    const double f = frequency(L, at(0.1), at(0.3));
    CHECK(f > 195 && f < 205);
}

TEST(clap_is_three_bursts_spread_apart_then_a_tail)
{
    DrumRig d("snare");
    d.r.param(d.drum, "clap", 1);
    d.r.param(d.drum, "spread", 12);
    d.r.param(d.drum, "decay", 150);
    d.hit(0.05);
    const auto &L = d.run(0.4);
    // the level rises again at 12 and 24 ms after the hit, having fallen in between
    const auto e = envelope(L, at(0.05), 40);
    CHECK(e[1] > 0.3f);
    CHECK(e[10] < e[1] * 0.7f && e[13] > e[10] * 1.5f);  // the second burst
    CHECK(e[22] < e[13] * 0.7f && e[25] > e[22] * 1.5f); // the third
    CHECK(e[39] > 0.05f);                                 // then the tail goes on
    // and no body: with snap 0 a clap is silent
    DrumRig quiet("snare");
    quiet.r.param(quiet.drum, "clap", 1);
    quiet.r.param(quiet.drum, "snap", 0);
    quiet.hit(0.05);
    CHECK(peak(quiet.run(0.2)) < 1e-3f);
}

TEST(metal_closed_is_short_open_is_long_and_a_closed_hit_chokes_an_open_one)
{
    { // GM 42 (closed) against 46 (open)
        DrumRig a("metal"), b("metal");
        a.hit(0.05, 42);
        b.hit(0.05, 46);
        const float closed = peak(a.run(0.6), at(0.3), at(0.4)),
                    open = peak(b.run(0.6), at(0.3), at(0.4));
        CHECK(peak(a.r.L, at(0.05), at(0.06)) > 0.2f);
        CHECK(closed < 1e-3f && open > 0.02f);
    }
    { // an open hit, then a closed one 100 ms later: the long tail is cut
        DrumRig d("metal");
        d.hit(0.05, 46);
        d.hit(0.15, 42);
        CHECK(peak(d.run(0.6), at(0.4), at(0.5)) < 1e-3f);
    }
}

TEST(metal_choke_input_and_accent)
{
    // OPEN from a gate, CHOKE from another 100 ms later: silent soon after the choke
    Rig            r;
    const uint32_t kb = r.add("keyboard"), g1 = r.add("note_to_gate"), kb2 = r.add("keyboard"),
                   g2 = r.add("note_to_gate"), m = r.add("metal"), out = r.add("out");
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", g1, "notes") == Err::Ok && r.cable(g1, "gate", m, "open") == Err::Ok
        && r.cable(kb2, "notes", g2, "notes") == Err::Ok && r.cable(g2, "gate", m, "choke") == Err::Ok
        && r.cable(m, "out", out, "l") == Err::Ok);
    r.key(kb, 0.05, true, 60);
    r.key(kb, 0.5, false, 60);
    r.key(kb2, 0.15, true, 60);
    r.key(kb2, 0.5, false, 60);
    CHECK(r.compile());
    r.render(kRate / 2);
    CHECK(peak(r.L, at(0.1), at(0.15)) > 0.05f); // ringing (open) until the choke
    CHECK(peak(r.L, at(0.2), at(0.3)) < 1e-3f);  // gone within 50 ms of it
    // accent: a soft note is quieter, as much as the knob says
    DrumRig loud("metal"), soft("metal");
    loud.r.param(loud.drum, "accent", 1);
    soft.r.param(soft.drum, "accent", 1);
    loud.hit(0.05, 42, 127);
    soft.hit(0.05, 42, 30);
    CHECK(peak(soft.run(0.2)) < 0.4f * peak(loud.run(0.2)));
}
