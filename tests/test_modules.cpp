// The rack's own modules and the behaviours that need a real patch: constants, mixing, gates,
// notes, merging, key scripts.
#include "testutil.h"

using namespace rt_util;

namespace
{
// A constant signal: an attenuverter with nothing at its input, scale 0 and an offset.
uint32_t constant(Rig &r, float v)
{
    const uint32_t a = r.add("attenuverter");
    r.param(a, "scale", 0);
    r.param(a, "offset", v);
    return a;
}
constexpr float kTol = 2e-3f; // fixed point: a few LSB of Q15 through two products
} // namespace

TEST(attenuverter_scales_and_offsets)
{
    Rig            r;
    const uint32_t c = constant(r, 0.5f), a = r.add("attenuverter"), out = r.add("out");
    r.param(a, "scale", -0.5f);
    r.param(a, "offset", 0.125f);
    r.param(out, "level", 1);
    CHECK(r.cable(c, "out", a, "in") == Err::Ok && r.cable(a, "out", out, "l") == Err::Ok
        && r.compile());
    r.render(512);
    for (size_t i = 100; i < 512; i++)
        CHECK(std::fabs(r.L[i] - (0.5f * -0.5f + 0.125f)) < kTol); // -0.125
}

TEST(mixer_sums_levels_and_saturates)
{
    Rig            r;
    const uint32_t a = constant(r, 0.5f), b = constant(r, 0.25f), m = r.add("mixer"),
                   out = r.add("out");
    r.param(m, "level1", 0.5f);
    r.param(m, "level2", 1);
    r.param(out, "level", 1);
    CHECK(r.cable(a, "out", m, "in1") == Err::Ok && r.cable(b, "out", m, "in2") == Err::Ok
        && r.cable(m, "out", out, "l") == Err::Ok && r.compile());
    r.render(256);
    for (size_t i = 0; i < 256; i++)
        CHECK(std::fabs(r.L[i] - 0.5f) < kTol
            && std::fabs(r.R[i] - 0.5f) < kTol); // 0.25 + 0.25, on both channels

    Rig s; // 0.9 + 0.9 must not wrap in the fixed-point builds, and the output stage clips float at
           // 1
    const uint32_t c1 = constant(s, 0.9f), c2 = constant(s, 0.9f), ms = s.add("mixer"),
                   o2 = s.add("out");
    s.param(ms, "level1", 1);
    s.param(ms, "level2", 1);
    s.param(o2, "level", 1);
    CHECK(s.cable(c1, "out", ms, "in1") == Err::Ok && s.cable(c2, "out", ms, "in2") == Err::Ok
        && s.cable(ms, "out", o2, "l") == Err::Ok && s.compile());
    s.render(256);
    for (size_t i = 0; i < 256; i++)
        CHECK(s.L[i] > 0.99f);
    Rig            t;
    const uint32_t d1 = constant(t, 0.9f), d2 = constant(t, -0.9f), mt = t.add("mixer"),
                   o3 = t.add("out");
    t.param(mt, "level1", 1);
    t.param(mt, "level2", 1);
    t.param(o3, "level", 1);
    CHECK(t.cable(d1, "out", mt, "in1") == Err::Ok && t.cable(d2, "out", mt, "in2") == Err::Ok
        && t.cable(mt, "out", o3, "l") == Err::Ok && t.compile());
    t.render(256);
    for (size_t i = 0; i < 256; i++)
        CHECK(std::fabs(t.L[i]) < kTol);
}

TEST(audio_out_levels_mono_and_stereo)
{
    Rig            r;
    const uint32_t a = constant(r, 0.5f), b = constant(r, -0.25f), out = r.add("out");
    r.param(out, "level", 0.5f);
    CHECK(r.cable(a, "out", out, "l") == Err::Ok && r.compile());
    r.render(256);
    for (size_t i = 0; i < 256; i++)
        CHECK(
            std::fabs(r.L[i] - 0.25f) < kTol && r.L[i] == r.R[i]); // L alone plays on both channels
    CHECK(r.cable(b, "out", out, "r") == Err::Ok && r.compile());
    r.render(256);
    for (size_t i = 256; i < 512; i++)
        CHECK(std::fabs(r.L[i] - 0.25f) < kTol && std::fabs(r.R[i] + 0.125f) < kTol);
}

TEST(audio_out_adds_every_output_module_and_clips)
{
    Rig            r;
    const uint32_t a = constant(r, 0.9f), b = constant(r, 0.9f), o1 = r.add("out"),
                   o2 = r.add("out");
    r.param(o1, "level", 1);
    r.param(o2, "level", 1);
    CHECK(r.cable(a, "out", o1, "l") == Err::Ok && r.cable(b, "out", o2, "l") == Err::Ok
        && r.compile());
    r.render(256);
    for (size_t i = 0; i < 256; i++)
        CHECK(r.L[i] == 1.0f && r.R[i] == 1.0f);
}

TEST(note_to_gate_is_sample_accurate_and_last_note_wins)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate"), out = r.add("out");
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", g, "notes") == Err::Ok && r.cable(g, "gate", out, "l") == Err::Ok);
    r.e.patch().keys.push_back(KeyScript { kb, 480.0 / kRate, true, 60, 100 }); // sample 480
    r.e.patch().keys.push_back(
        KeyScript { kb, 700.0 / kRate, true, 64, 100 }); // a second note while the first sounds
    r.e.patch().keys.push_back(KeyScript {
        kb, 800.0 / kRate, false, 60, 0 }); // the first one's release: ignored, 64 is the last note
    r.e.patch().keys.push_back(
        KeyScript { kb, 960.0 / kRate, false, 64, 0 }); // sample 960: the gate ends
    CHECK(r.compile());
    r.render(1500);
    for (size_t i = 0; i < 480; i++)
        CHECK(r.L[i] == 0.0f);
    for (size_t i = 480; i < 960; i++)
        CHECK(r.L[i] > 0.99f);
    for (size_t i = 960; i < 1500; i++)
        CHECK(r.L[i] == 0.0f);
}

TEST(keyboard_events_cross_block_boundaries_at_the_right_offset)
{
    // a note on at sample 255, 256 and 257 (the block edge) and its release a block later
    for (int on : { 255, 256, 257, 511, 512 })
    {
        Rig            r;
        const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate"), out = r.add("out");
        r.param(out, "level", 1);
        CHECK(
            r.cable(kb, "notes", g, "notes") == Err::Ok && r.cable(g, "gate", out, "l") == Err::Ok);
        r.e.patch().keys.push_back(KeyScript { kb, (double)on / kRate, true, 60, 100 });
        r.e.patch().keys.push_back(KeyScript { kb, (double)(on + 300) / kRate, false, 60, 0 });
        CHECK(r.compile());
        r.render(1200);
        for (int i = 0; i < 1200; i++)
            CHECK((r.L[(size_t)i] > 0.99f) == (i >= on && i < on + 300));
    }
}

TEST(live_keys_are_heard_at_the_start_of_the_next_block)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate"), out = r.add("out");
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", g, "notes") == Err::Ok && r.cable(g, "gate", out, "l") == Err::Ok
        && r.compile());
    r.render(300);
    CHECK(r.e.key(kb, true, 60) == Err::Ok);
    r.render(100);
    CHECK(r.L[300] > 0.99f && r.L[299] == 0.0f);
    CHECK(r.e.key(kb, false, 60) == Err::Ok);
    r.render(100);
    CHECK(r.L[400] == 0.0f && r.L[399] > 0.99f);
    CHECK(r.e.key(r.e.patch().nodes()[0].id + 99, true, 60) == Err::NoSuchNode);
    CHECK(r.e.key(out, true, 60) == Err::NotAKeyboard);
}

TEST(keyboard_queue_is_ordered_and_bounded)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard");
    CHECK(r.compile());
    void *st = const_cast<void *>(r.e.state(kb));
    int   pushed = 0;
    while (
        keyboard_push(st, 1000 + (uint64_t)(pushed % 7), PCL_EVT_NOTE_ON, 60, 100) && pushed < 5000)
        pushed++;
    CHECK(pushed == 1024); // the queue is bounded, a full push says so
    CHECK(!keyboard_push(st, 5, PCL_EVT_NOTE_ON, 60, 100));
}

TEST(merge_orders_by_offset_and_puts_a_first_at_a_tie)
{
    // keys (A) and clock (B) at the same sample: the key must come first, so the arpeggiator hears
    // it before the pulse
    Rig            r;
    const uint32_t kb = r.add("keyboard"), clk = r.add("clock"), mg = r.add("merge"),
                   arp = r.add("arp"), osc = r.add("osc"), out = r.add("out");
    r.param(clk, "bpm", 120);
    r.param(clk, "ppqn", 24);
    r.param(arp, "division", 6);
    r.param(arp, "gate", 3);
    r.param(out, "level", 1);
    r.param(osc, "waveform", 0);
    CHECK(r.cable(kb, "notes", mg, "a") == Err::Ok && r.cable(clk, "clock", mg, "b") == Err::Ok
        && r.cable(mg, "out", arp, "in") == Err::Ok);
    CHECK(r.cable(arp, "out", osc, "note") == Err::Ok && r.cable(osc, "out", out, "l") == Err::Ok);
    r.key(kb, 0.0, true, 69); // at sample 0, the same sample as the clock's first pulse
    CHECK(r.compile());
    r.render(4000);
    // the first pulse (sample 0) already plays the key: the oscillator is tuned to 440 Hz from the
    // start
    CHECK(rms(r.L, 100, 1200) > 0.1);
    const double f = frequency(r.L, 200, 1400);
    CHECK(f > 430 && f < 450);
}

TEST(keyboard_to_oscillator_tunes_on_the_note_sample)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), osc = r.add("osc"), out = r.add("out");
    r.param(osc, "waveform", 0); // sine
    r.param(osc, "frequency", 220);
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", osc, "note") == Err::Ok && r.cable(osc, "out", out, "l") == Err::Ok);
    r.key(kb, 0.5, true, 81); // A5 = 880 Hz
    CHECK(r.compile());
    r.render(2 * kRate);
    const double before = frequency(r.L, 2000, (size_t)(0.49 * kRate)),
                 after = frequency(r.L, (size_t)(0.6 * kRate), 2 * kRate);
    CHECK(before > 215 && before < 225);
    CHECK(after > 870 && after < 890);
}

TEST(clock_seq_note_to_gate_counts_the_steps)
{
    Rig            r;
    const uint32_t clk = r.add("clock"), seq = r.add("seq"), g = r.add("note_to_gate"),
                   out = r.add("out");
    r.param(clk, "bpm", 120); // 24 ppqn: 20 samples per ... 120 bpm = 2 beats/s, 48 pulses/s, a
                              // step of 6 pulses = 8 steps/s
    r.param(seq, "length", 8);
    r.param(seq, "division", 6);
    r.param(seq, "gate", 3);
    for (size_t i = 0; i < 8; i++)
        CHECK(r.e.set_step(seq, i, StepData { (uint8_t)(48 + i), 100, 0, 0 }) == Err::Ok);
    r.param(out, "level", 1);
    CHECK(r.cable(clk, "clock", seq, "in") == Err::Ok && r.cable(seq, "out", g, "notes") == Err::Ok
        && r.cable(g, "gate", out, "l") == Err::Ok);
    CHECK(r.compile());
    r.render(2 * kRate);
    int    rising = 0;
    size_t high = 0;
    for (size_t i = 1; i < r.L.size(); i++)
    {
        if (r.L[i] > 0.5f && r.L[i - 1] <= 0.5f)
            rising++;
        high += r.L[i] > 0.5f;
    }
    CHECK(rising == 15); // 16 steps in 2 s; the first starts at sample 0 (no rising edge there)
    CHECK(r.L[0] > 0.99f);
    CHECK(std::abs((int)high - 16 * 3 * 1000)
        < 40); // each gate is 3 pulses = 3000 samples at 120 bpm, 24 ppqn, 48 kHz
}

TEST(sequencer_swing_never_exceeds_the_division)
{
    Rig            r;
    const uint32_t seq = r.add("seq");
    CHECK(r.compile());
    r.param(seq, "division", 4);
    r.param(
        seq, "swing", 90); // above division - 1: clamped by the wrapper, not refused by the library
    r.param(seq, "division", 96);
    r.param(seq, "swing", 95);
    CHECK(r.e.apply_failures() == 0);
}

TEST(event_capacity_is_enough_in_the_worst_case)
{
    // the fastest clock the library allows at the lowest sample rate, into a chord arpeggiator
    // holding 16 keys
    Rig            r(8000);
    const uint32_t kb = r.add("keyboard"), clk = r.add("clock"), mg = r.add("merge"),
                   arp = r.add("arp"), seq = r.add("seq");
    r.param(clk, "bpm", 999);
    r.param(clk, "ppqn", 96);
    r.param(arp, "mode", 6); // chord
    r.param(arp, "division", 1);
    r.param(arp, "gate", 1);
    CHECK(r.cable(kb, "notes", mg, "a") == Err::Ok && r.cable(clk, "clock", mg, "b") == Err::Ok
        && r.cable(mg, "out", arp, "in") == Err::Ok);
    CHECK(r.cable(clk, "clock", seq, "in") == Err::Ok);
    r.param(seq, "division", 1);
    r.param(seq, "gate", 1);
    for (size_t i = 0; i < 8; i++)
        CHECK(r.e.set_step(seq, i, StepData { (uint8_t)(40 + i), 100, 0, 0 }) == Err::Ok);
    for (int k = 0; k < 16; k++)
        r.key(kb, 0.001 * k, true, (uint8_t)(40 + 3 * k));
    CHECK(r.compile());
    r.render(8000);
    for (uint32_t id : { clk, arp, seq })
        CHECK(dropped_events(*r.e.patch().find(id)->desc, r.e.state(id)) == 0);
}

// The oscillator's SYNC input takes wrap fractions in [0, 1]; the rack lets any signal be patched
// in. Bipolar noise there must be harmless. It once overflowed a signed integer in the library's Q31
// build (UBSan found it); the rack clamped the input until PulseCoreLib fixed it (fd4f2ec), and now
// passes it straight through, so this test checks the library's handling.
TEST(oscillator_sync_input_accepts_any_signal)
{
    for (const char *src : { "noise", "lfo" })
    {
        Rig            r;
        const uint32_t s = r.add(src), osc = r.add("osc"), out = r.add("out");
        r.param(osc, "waveform", 2);
        r.param(out, "level", 1);
        if (std::string(src) == "noise")
            r.param(s, "colour", 0);
        CHECK(r.cable(s, "out", osc, "sync") == Err::Ok && r.cable(osc, "out", out, "l") == Err::Ok
            && r.compile());
        r.render(kRate);
        CHECK(r.e.nonfinite_samples() == 0 && rms(r.L) > 0.01);
    }
    // and a real master-slave pair still syncs: the slave restarts on every master wrap, so its
    // output repeats every master period (480 samples at 100 Hz), which a free-running 333 Hz slave
    // does not
    auto repeat_error = [](bool synced)
    {
        Rig            m;
        const uint32_t a = m.add("osc"), b = m.add("osc"), out = m.add("out");
        m.param(a, "waveform", 0);
        m.param(a, "frequency", 100);
        m.param(b, "waveform", 2);
        m.param(b, "frequency", 333);
        m.param(out, "level", 1);
        if (synced)
            CHECK(m.cable(a, "sync_out", b, "sync") == Err::Ok);
        CHECK(m.cable(b, "out", out, "l") == Err::Ok && m.compile());
        m.render(kRate / 2);
        double d = 0;
        size_t n = 0;
        for (size_t i = 6000; i + 480 < m.L.size(); i++, n++)
            d += (double)(m.L[i] - m.L[i + 480]) * (m.L[i] - m.L[i + 480]);
        return std::sqrt(d / (double)n) / rms(m.L, 6000);
    };
    CHECK(repeat_error(true) < 0.05);
    CHECK(repeat_error(false) > 0.3);
}

// ---- the Keyboard queue and the note-to-gate module, driven with raw events
// -------------------------------------------------------

namespace
{
struct KeyRig
{
    Rig      r;
    uint32_t kb, gate, out;
    KeyRig()
    {
        kb = r.add("keyboard");
        gate = r.add("note_to_gate");
        out = r.add("out");
        r.param(out, "level", 1);
        CHECK(r.cable(kb, "notes", gate, "notes") == Err::Ok
            && r.cable(gate, "gate", out, "l") == Err::Ok);
        CHECK(r.compile());
    }
    bool push(uint64_t at, uint8_t type, uint8_t note, uint8_t vel)
    {
        return keyboard_push(const_cast<void *>(r.e.state(kb)), at, type, note, vel);
    }
    bool high(size_t i) const { return r.L[i] > 0.5f; }
};
} // namespace

TEST(keyboard_events_scheduled_ahead_wait_for_their_block_and_may_arrive_out_of_order)
{
    KeyRig k;
    k.push(3500, PCL_EVT_NOTE_OFF, 60, 0); // pushed in the wrong order on purpose
    k.push(3000, PCL_EVT_NOTE_ON, 60, 100);
    k.push(3800, PCL_EVT_NOTE_OFF, 64, 0);
    k.push(3200, PCL_EVT_NOTE_ON, 64, 100);
    k.r.render(5000);
    for (size_t i = 0; i < 5000; i++)
        CHECK(k.high(i)
            == (i >= 3000
                && i < 3800)); // 60 on, 64 on (last note wins), the release of 60 ignored, 64 off
}

TEST(keyboard_events_at_one_sample_keep_the_order_they_were_given)
{
    for (int on_first = 0; on_first < 2; on_first++)
    {
        KeyRig k;
        if (on_first)
        {
            k.push(1000, PCL_EVT_NOTE_ON, 60, 100);
            k.push(1000, PCL_EVT_NOTE_OFF, 60, 0);
        }
        else
        {
            k.push(1000, PCL_EVT_NOTE_OFF, 60, 0);
            k.push(1000, PCL_EVT_NOTE_ON, 60, 100);
        }
        k.r.render(1500);
        CHECK(k.high(1100) == !on_first); // the last event decides
    }
}

TEST(keyboard_events_at_one_sample_survive_a_long_run_of_them)
{
    KeyRig k;
    for (int i = 0; i < 40; i++)
        k.push(1000, PCL_EVT_NOTE_ON, (uint8_t)(40 + i), 100);
    k.push(1000, PCL_EVT_NOTE_OFF, 79,
        0); // the last note's release, written last: the gate must be low
    k.r.render(1500);
    CHECK(!k.high(1100) && !k.high(1400));
}

TEST(keyboard_queue_compacts_when_full_and_keeps_every_event)
{
    KeyRig k;
    for (int i = 0; i < 1024; i++)
        CHECK(k.push(
            100u * (uint64_t)i, i % 2 ? PCL_EVT_NOTE_OFF : PCL_EVT_NOTE_ON, 60, i % 2 ? 0 : 100));
    CHECK(!k.push(5, PCL_EVT_NOTE_ON, 60, 100)); // full
    k.r.render(2560);            // about 26 events are sent, the queue is still full of the rest
    for (int j = 0; j < 20; j++) // so these only fit if the queue is compacted first
        CHECK(k.push(102400u + 100u * (uint64_t)j, j % 2 ? PCL_EVT_NOTE_OFF : PCL_EVT_NOTE_ON, 60,
            j % 2 ? 0 : 100));
    k.r.render(105000 - 2560);
    int  rising = 0;
    bool prev = false;
    for (size_t i = 0; i < k.r.L.size(); i++)
    {
        rising += k.high(i) && !prev;
        prev = k.high(i);
    }
    CHECK(rising == 512 + 10); // every note-on of both batches made a pulse
}

TEST(keyboard_sends_at_most_a_capacity_of_events_per_block)
{
    KeyRig k;
    for (int i = 0; i < 100; i++)
        k.push(1000, PCL_EVT_NOTE_ON, (uint8_t)(30 + i % 90), 100);
    k.push(1000, PCL_EVT_NOTE_OFF, 30,
        0); // far beyond the block's capacity of 64: dropped, never written past the buffer
    k.r.render(2000);
    CHECK(k.high(1500)); // the first events got through, the release at the end did not
}

TEST(keyboard_future_key_from_the_engine_waits_for_its_sample)
{
    KeyRig k;
    CHECK(k.r.e.key(k.kb, true, 60, 100, 3000) == Err::Ok);
    CHECK(k.r.e.key(k.kb, false, 60, 0, 3600) == Err::Ok);
    k.r.render(4000);
    for (size_t i = 0; i < 4000; i++)
        CHECK(k.high(i) == (i >= 3000 && i < 3600));
}

TEST(note_to_gate_obeys_every_kind_of_release_and_ignores_the_rest)
{
    KeyRig k;
    k.push(100, PCL_EVT_NOTE_ON, 60, 100);
    k.push(200, PCL_EVT_NOTE_OFF, 60, 64); // a note-off with a velocity is still a note-off
    k.push(300, PCL_EVT_NOTE_ON, 60, 100);
    k.push(400, PCL_EVT_NOTE_ON, 60, 0); // a note-on with velocity 0 is a note-off
    k.push(500, PCL_EVT_NOTE_ON, 60, 100);
    k.push(600, PCL_EVT_STOP, 0, 0); // a transport stop releases it
    k.push(700, PCL_EVT_NOTE_ON, 60, 100);
    k.push(800, PCL_EVT_CLOCK, 60,
        0); // clock and start do not touch it, even with the current note number and no velocity
    k.push(810, PCL_EVT_START, 60, 0);
    k.push(900, PCL_EVT_NOTE_OFF, 59, 0); // the release of a note that is not the current one
    k.push(1000, PCL_EVT_NOTE_OFF, 60, 0);
    k.r.render(1200);
    for (size_t i = 0; i < 1200; i++)
    {
        const bool expect = (i >= 100 && i < 200) || (i >= 300 && i < 400) || (i >= 500 && i < 600)
            || (i >= 700 && i < 1000);
        CHECK(k.high(i) == expect);
    }
}

TEST(oscillator_is_retuned_by_note_ons_only)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), osc = r.add("osc"), out = r.add("out");
    r.param(osc, "waveform", 0);
    r.param(osc, "frequency", 220);
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", osc, "note") == Err::Ok && r.cable(osc, "out", out, "l") == Err::Ok
        && r.compile());
    void *st = const_cast<void *>(r.e.state(kb));
    keyboard_push(st, 1000, PCL_EVT_NOTE_OFF, 81, 64); // a release with a velocity: no retune
    keyboard_push(
        st, 2000, PCL_EVT_NOTE_ON, 81, 0); // a note-on with velocity 0 is a release: no retune
    keyboard_push(st, 2500, PCL_EVT_CLOCK, 81, 100); // other event types: ignored
    r.render(20000);
    const double f = frequency(r.L, 4000, 20000);
    CHECK(f > 215 && f < 225);
}

TEST(dropped_events_are_reported_only_for_the_modules_that_count_them)
{
    for (const ModuleDesc *d : all_modules())
    {
        Rig r;
        r.e.patch().add_node(*d);
        CHECK(r.compile());
        r.render(3000);
        CHECK(dropped_events(*d, r.e.state(r.e.patch().nodes()[0].id)) == 0);
    }
}

TEST(nan_and_infinity_at_the_output_are_silenced_and_counted)
{
    for (float bad : { (float)NAN, (float)INFINITY, -(float)INFINITY })
    {
        Rig            r;
        const uint32_t c = constant(r, 0.5f), out = r.add("out");
        CHECK(r.cable(c, "out", out, "l") == Err::Ok && r.compile());
        kAudioOutDesc.params[0].apply(
            const_cast<void *>(r.e.state(out)), bad); // a level the control ranges never allow
        r.render(100);
        CHECK(r.e.nonfinite_samples() == 200); // both channels, every sample
        CHECK(peak(r.L) == 0 && peak(r.R) == 0);
    }
}

TEST(engine_key_script_keeps_the_written_order_at_one_sample)
{
    for (int on_last = 0; on_last < 2; on_last++)
    {
        Rig            r;
        const uint32_t kb = r.add("keyboard"), gate = r.add("note_to_gate"), out = r.add("out");
        r.param(out, "level", 1);
        CHECK(r.cable(kb, "notes", gate, "notes") == Err::Ok
            && r.cable(gate, "gate", out, "l") == Err::Ok);
        for (int i = 0; i < 40; i++)
            r.key(kb, 0.02, true, (uint8_t)(40 + i)); // forty notes at one sample...
        r.key(kb, 0.02, false, 79); // ...then the release of the last: the gate must end low
        if (on_last)
            r.key(kb, 0.02, true, 30); // or one more note after it: high
        CHECK(r.compile());
        r.render(2000);
        CHECK((r.L[1500] > 0.5f) == (on_last == 1));
    }
}
