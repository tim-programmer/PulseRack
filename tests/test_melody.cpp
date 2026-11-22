#include "testutil.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

using namespace rt_util;

TEST(melody_locks_a_scale_quantized_loop_and_releases_on_stop)
{
    const auto                   &desc = kMelodyDesc;
    std::vector<std::max_align_t> state(
        (desc.state_size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    std::memset(state.data(), 0, desc.state_size);
    std::vector<float> params;
    for (const auto &p : desc.params)
        params.push_back(p.def);
    params[find_param(desc, "external")] = 1;
    params[find_param(desc, "division")] = 1;
    params[find_param(desc, "length")] = 4;
    params[find_param(desc, "mutation")] = 100;
    params[find_param(desc, "density")] = 100;
    params[find_param(desc, "lock")] = 1;
    CHECK(desc.init(state.data(), { kRate }, params.data()));

    std::array<pcl_event_t, 9> pulses { };
    for (size_t i = 0; i < 8; ++i)
        pulses[i] = { static_cast<uint16_t>(i * 3), PCL_EVT_CLOCK, 0, 0 };
    pulses.back() = { 24, PCL_EVT_STOP, 0, 0 };
    std::array<pcl_event_t, 32> notes { };
    EventBuf                    in { pulses.data(), pulses.size(), pulses.size() };
    EventBuf                    out { notes.data(), notes.size(), 0 };
    const EventBuf             *inputs[] = { &in };
    EventBuf                   *outputs[] = { &out };
    NodeIo                      io { };
    io.evt_in = inputs;
    io.evt_out = outputs;
    io.frames = 32;
    desc.process(state.data(), io);

    CHECK(out.count == 16);
    const bool minor[]
        = { true, false, true, true, false, true, false, true, true, false, true, false };
    for (size_t i = 0; i < 8; ++i)
    {
        const auto &on = notes[i * 2];
        const auto &off = notes[i * 2 + 1];
        CHECK(on.type == PCL_EVT_NOTE_ON && on.offset == i * 3 && minor[on.note % 12]);
        CHECK(off.type == PCL_EVT_NOTE_OFF && off.offset == (i + 1) * 3 && off.note == on.note);
        CHECK(on.note == notes[(i % 4) * 2].note);
    }

    desc.reset(state.data());
    desc.params[find_param(desc, "lock")].apply(state.data(), 0);
    desc.process(state.data(), io);
    CHECK(out.count == 16);
    // Every step moves when it changes, except the first, which stays on the root.
    CHECK(notes[0].note == 60 && notes[8].note == 60);
    for (size_t i = 1; i < 4; ++i)
        CHECK(notes[i * 2].note != notes[(i + 4) * 2].note);

    in.count = 1;
    desc.process(state.data(), io);
    CHECK(out.count == 1 && notes[0].type == PCL_EVT_NOTE_ON);
    const auto held = notes[0].note;
    desc.params[find_param(desc, "seed")].apply(state.data(), 42);
    in.count = 0;
    desc.process(state.data(), io);
    CHECK(out.count == 1 && notes[0].type == PCL_EVT_NOTE_OFF && notes[0].note == held);
}

// Plays one loop of 32 steps, one clock pulse per step, and returns the note-ons.
static std::vector<pcl_event_t> melody_loop(
    std::initializer_list<std::pair<const char *, float>> settings)
{
    const auto                   &desc = kMelodyDesc;
    std::vector<std::max_align_t> state(
        (desc.state_size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    std::memset(state.data(), 0, desc.state_size);
    std::vector<float> params;
    for (const auto &p : desc.params)
        params.push_back(p.def);
    params[find_param(desc, "external")] = 1;
    params[find_param(desc, "division")] = 1;
    params[find_param(desc, "length")] = 32;
    params[find_param(desc, "lock")] = 1;
    for (const auto &[id, value] : settings)
        params[find_param(desc, id)] = value;
    CHECK(desc.init(state.data(), { kRate }, params.data()));

    std::array<pcl_event_t, 32> pulses { };
    for (size_t i = 0; i < pulses.size(); ++i)
        pulses[i] = { static_cast<uint16_t>(i), PCL_EVT_CLOCK, 0, 0 };
    std::array<pcl_event_t, 80> events { };
    EventBuf                    in { pulses.data(), pulses.size(), pulses.size() };
    EventBuf                    out { events.data(), events.size(), 0 };
    const EventBuf             *inputs[] = { &in };
    EventBuf                   *outputs[] = { &out };
    NodeIo                      io { };
    io.evt_in = inputs;
    io.evt_out = outputs;
    io.frames = 32;
    desc.process(state.data(), io);
    std::vector<pcl_event_t> ons;
    for (size_t i = 0; i < out.count; ++i)
        if (events[i].type == PCL_EVT_NOTE_ON)
            ons.push_back(events[i]);
    return ons;
}

TEST(melody_walks_the_scale_with_rests_and_accents)
{
    int small = 0, total = 0;
    for (float seed = 1; seed <= 40; ++seed)
        for (float scale : { 0.f, 3.f })
        {
            const auto notes = melody_loop({ { "seed", seed }, { "scale", scale }, { "density", 100 } });
            CHECK(notes.size() == 32 && notes[0].note == 60);
            for (size_t i = 1; i < notes.size(); ++i)
            {
                const int interval = std::abs(notes[i].note - notes[i - 1].note);
                CHECK(interval <= 8); // never more than a minor sixth
                small += interval <= 3;
                ++total;
            }
        }
    CHECK(small * 10 > total * 6); // mostly steps and repeats

    // Density takes steps away in a fixed order, but the first step always plays.
    CHECK(melody_loop({ { "density", 0 } }).size() == 1);
    const auto half = melody_loop({ { "density", 50 } }).size();
    CHECK(half > 8 && half < 28);

    for (const auto &e : melody_loop({ { "accent", 0 }, { "density", 100 } }))
        CHECK(e.velocity < 100);
    for (const auto &e : melody_loop({ { "accent", 100 }, { "density", 100 } }))
        CHECK(e.velocity > 100);
}

TEST(chord_voice_strums_and_arpeggiates_from_a_single_note)
{
    auto render = [](bool arp, float strum)
    {
        Rig        r;
        const auto keys = r.add("keyboard");
        const auto voice = r.add("chord_voice");
        const auto output = r.add("out");
        CHECK(r.cable(keys, "notes", voice, "note") == Err::Ok);
        CHECK(r.cable(voice, "out", output, "l") == Err::Ok);
        r.param(voice, "arp", arp ? 1 : 0);
        r.param(voice, "strum", strum);
        r.param(voice, "rate", 10);
        // a near-pure tone to measure: the filter just above A4, no detune, no pluck
        r.param(voice, "color", 0.2f);
        r.param(voice, "detune", 0);
        r.param(voice, "pluck", 0);
        r.key(keys, 0, true, 69);
        r.key(keys, 0.4, false, 69);
        CHECK(r.compile());
        r.render(kRate);
        return r.L;
    };
    const auto chord = render(false, 0);
    const auto strum = render(false, 100);
    const auto arp = render(true, 0);
    CHECK(rms(chord, 14400, 18000) > 0.02); // (A4 to E5, through the filter at about 500 Hz)
    CHECK(std::abs(frequency(strum, 1440, 3840) - 440) < 2);
    CHECK(std::abs(frequency(arp, 1440, 3840) - 440) < 2);
    CHECK(std::abs(frequency(arp, 6240, 8640) - 523.25) < 2); // A minor: C major's chord on A
    CHECK(rms(arp, 43200, 48000) < 0.001);
}

TEST(chord_voice_builds_chords_in_key_and_leads_them_smoothly)
{
    auto in_scale = [](int note, int key, int scale)
    {
        const int semitone = ((note - key) % 12 + 12) % 12;
        for (int i = 0; i < kScaleSize[scale]; ++i)
            if (kScaleSteps[scale][i] == semitone)
                return true;
        return false;
    };
    uint8_t chord[4];

    // Root position without a chord before: C major's triads on C, on A and on B.
    CHECK(chord_notes(60, 0, 0, 0, nullptr, 0, chord) == 3);
    CHECK(chord[0] == 60 && chord[1] == 64 && chord[2] == 67);
    chord_notes(69, 0, 0, 0, nullptr, 0, chord);
    CHECK(chord[0] == 69 && chord[1] == 72 && chord[2] == 76);
    chord_notes(71, 0, 0, 1, nullptr, 0, chord); // B half-diminished seventh
    CHECK(chord[0] == 71 && chord[1] == 74 && chord[2] == 77 && chord[3] == 81);
    // A note out of the scale moves down to it: C# in C major is a C chord.
    chord_notes(61, 0, 0, 0, nullptr, 0, chord);
    CHECK(chord[0] == 60);
    // D minor, from a D below the octave of the key (negative degrees).
    chord_notes(38, 2, 1, 0, nullptr, 0, chord);
    CHECK(chord[0] == 38 && chord[1] == 41 && chord[2] == 45);

    // Every chord of every shape, key and scale is in key.
    for (int key = 0; key < 12; ++key)
        for (int scale = 0; scale < 4; ++scale)
            for (int shape = 0; shape < 6; ++shape)
                for (int note = 30; note < 90; ++note)
                {
                    const int count = chord_notes(note, key, scale, shape, nullptr, 0, chord);
                    for (int t = 0; t < count; ++t)
                        CHECK(in_scale(chord[t], key, scale));
                }

    // Led: C, F, G, C and Am, wherever the notes played jump, each voice moves at most a
    // fourth and the chords stay near where they started.
    const int played[] = { 60, 53, 79, 48, 69, 62, 55 };
    uint8_t   previous[4];
    int       count = chord_notes(played[0], 0, 0, 0, nullptr, 0, previous);
    for (const int note : played)
    {
        const int n = chord_notes(note, 0, 0, 0, previous, count, chord);
        CHECK(n == 3);
        for (int t = 0; t < n; ++t)
        {
            CHECK(in_scale(chord[t], 0, 0));
            CHECK(std::abs(chord[t] - previous[t]) <= 5);
            CHECK(chord[t] >= 48 && chord[t] <= 84);
        }
        std::copy_n(chord, n, previous);
        count = n;
    }
}

TEST(chord_voice_brightness_and_pluck_open_its_filter)
{
    // the upper harmonics, as the share of sign changes beyond what the notes alone make
    auto crossings = [](const std::vector<float> &v, size_t from, size_t to)
    {
        int c = 0;
        for (size_t i = from + 1; i < to; i++)
            c += (v[i - 1] < 0) != (v[i] < 0);
        return c;
    };
    auto render = [](float bright, float pluck)
    {
        Rig        r;
        const auto keys = r.add("keyboard");
        const auto voice = r.add("chord_voice");
        const auto output = r.add("out");
        CHECK(r.cable(keys, "notes", voice, "note") == Err::Ok);
        CHECK(r.cable(voice, "out", output, "l") == Err::Ok);
        r.param(voice, "color", bright);
        r.param(voice, "pluck", pluck);
        r.key(keys, 0, true, 45);
        CHECK(r.compile());
        r.render(kRate);
        return r.L;
    };
    const auto dark = render(0.1f, 0), bright = render(0.9f, 0), plucked = render(0.1f, 3);
    CHECK(crossings(bright, 24000, 33600) > crossings(dark, 24000, 33600) * 2);
    // Pluck: bright at the start, back to the dark sound once it has closed (500 ms)
    CHECK(crossings(plucked, 480, 4800) * 10 > crossings(dark, 480, 4800) * 14);
    CHECK(std::abs(crossings(plucked, 38400, 48000) - crossings(dark, 38400, 48000))
        <= crossings(dark, 38400, 48000) / 4 + 4);
}
