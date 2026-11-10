#include "testutil.h"

#include <random>

using namespace rt_util;

namespace
{

// clock -> sequencer -> oscillator + ADSR -> VCA -> output, with an LFO on a filter: events, audio
// and modulation
void build_voice(Rig &r)
{
    const uint32_t clk = r.add("clock"), seq = r.add("seq"), osc = r.add("osc"),
                   adsr = r.add("adsr"), vca = r.add("vca"), svf = r.add("svf"), lfo = r.add("lfo"),
                   out = r.add("out");
    r.param(clk, "bpm", 150);
    r.param(seq, "length", 5);
    r.param(seq, "direction", 2);
    for (size_t i = 0; i < 5; i++)
        CHECK(r.e.set_step(seq, i, StepData { (uint8_t)(43 + 3 * i), 100, 0, (uint8_t)(i == 2) })
            == Err::Ok);
    r.param(osc, "waveform", 2);
    r.param(adsr, "attack", 4);
    r.param(adsr, "decay", 80);
    r.param(adsr, "sustain", 0.4f);
    r.param(adsr, "release", 70);
    r.param(lfo, "frequency", 0.7f);
    r.param(svf, "cutoff", 900);
    r.param(svf, "resonance", 0.5f);
    CHECK(r.cable(clk, "clock", seq, "in") == Err::Ok && r.cable(seq, "out", osc, "note") == Err::Ok
        && r.cable(seq, "out", adsr, "note") == Err::Ok);
    CHECK(r.cable(osc, "out", svf, "in") == Err::Ok && r.cable(lfo, "out", svf, "cutoff") == Err::Ok
        && r.cable(svf, "lp", vca, "in") == Err::Ok);
    CHECK(r.cable(adsr, "out", vca, "gain") == Err::Ok && r.cable(vca, "out", out, "l") == Err::Ok);
}

} // namespace

TEST(engine_block_size_does_not_change_the_samples)
{
    Rig ref;
    build_voice(ref);
    CHECK(ref.compile());
    ref.render(3 * kRate);
    CHECK(rms(ref.L) > 0.01);
    for (size_t chunk : { (size_t)1, (size_t)7, (size_t)64, (size_t)255, (size_t)256, (size_t)257,
             (size_t)1000, (size_t)4801 })
    {
        Rig r;
        build_voice(r);
        CHECK(r.compile());
        for (size_t done = 0; done < 3 * kRate; done += chunk)
            r.render(std::min(chunk, 3 * kRate - done));
        CHECK(same_bits(r.L, ref.L) && same_bits(r.R, ref.R));
    }
}

TEST(engine_reset_and_second_instance_are_deterministic)
{
    Rig a, b;
    build_voice(a);
    build_voice(b);
    CHECK(a.compile() && b.compile());
    a.render(20000);
    b.render(20000);
    CHECK(same_bits(a.L, b.L));
    a.e.reset();
    std::vector<float> l(20000), r(20000);
    a.e.render(20000, l.data(), r.data());
    CHECK(same_bits(l, b.L)); // reset: bit-identical to a fresh run
    CHECK(a.e.time() == 20000);
}

TEST(engine_edits_keep_running_modules_running)
{
    Rig ref;
    build_voice(ref);
    CHECK(ref.compile());
    ref.render(8000);

    Rig r;
    build_voice(r);
    CHECK(r.compile());
    r.render(3000);
    const uint32_t extra = r.add("noise"); // an unrelated module appears mid-render
    CHECK(r.compile());
    r.render(2000);
    CHECK(r.e.patch().remove_node(extra) == Err::Ok); // and goes away again
    CHECK(r.compile());
    r.render(3000);
    CHECK(same_bits(
        r.L, ref.L)); // the voice never noticed: phases, filter state and envelope carried on
}

TEST(engine_live_parameter_changes_take_effect_and_are_remembered)
{
    Rig            r;
    const uint32_t osc = r.add("osc"), out = r.add("out");
    r.param(osc, "waveform", 0);
    r.param(osc, "frequency", 200);
    r.param(out, "level", 1);
    CHECK(r.cable(osc, "out", out, "l") == Err::Ok && r.compile());
    r.render(kRate / 2);
    r.param(osc, "frequency", 800);
    r.render(kRate / 2);
    const double f1 = frequency(r.L, 2000, kRate / 2 - 200),
                 f2 = frequency(r.L, kRate / 2 + 5000, kRate);
    CHECK(f1 > 195 && f1 < 205 && f2 > 790 && f2 < 810);
    CHECK(r.e.patch().find(osc)->params[(size_t)r.param_index(osc, "frequency")] == 800);
    CHECK(r.e.apply_failures() == 0);
}

TEST(engine_other_sample_rates)
{
    for (uint32_t rate : { 22050u, 44100u, 96000u })
    {
        Rig r(rate);
        build_voice(r);
        CHECK(r.compile());
        r.render(rate);
        CHECK(rms(r.L) > 0.01 && r.e.nonfinite_samples() == 0);
    }
}

TEST(engine_random_patches_with_extreme_settings_stay_finite)
{
    std::mt19937 rng(2024);
    for (int round = 0; round < 80; round++)
    {
        Rig                   r(round % 5 == 0 ? 44100u : kRate);
        std::vector<uint32_t> ids;
        for (const ModuleDesc *d : all_modules())
            ids.push_back(r.e.patch().add_node(*d));
        for (uint32_t id : ids)
        {
            Node *n = r.e.patch().find(id);
            for (size_t i = 0; i < n->desc->params.size(); i++)
            {
                const ParamDesc &pd = n->desc->params[i];
                const int        pick = (int)(rng() % 4);
                (void)r.e.patch().set_param(id, i,
                    pick == 0       ? pd.min
                        : pick == 1 ? pd.max
                                    : pd.min + (float)(rng() % 1001) / 1000.f * (pd.max - pd.min));
            }
            if (n->desc == &kKeyboardDesc)
                for (int k = 0; k < 20; k++)
                {
                    const double  at = (double)(rng() % 900) / 1000.0;
                    const uint8_t note = (uint8_t)(rng() % 128);
                    const uint8_t vel = (uint8_t)(1 + rng() % 127);
                    r.key(id, at, k % 3 != 2, note, vel);
                }
        }
        for (int k = 0; k < 150; k++)
        {
            const Node  *a = r.e.patch().find(ids[rng() % ids.size()]);
            const Node  *b = r.e.patch().find(ids[rng() % ids.size()]);
            const size_t pa = rng()
                % a->desc->ports
                      .size(); // drawn first: the order of evaluation of arguments is unspecified
            const size_t pb = rng() % b->desc->ports.size();
            (void)r.e.patch().connect({ a->id, (uint16_t)pa }, { b->id, (uint16_t)pb });
        }
        const bool compiled = r.compile();
        if (!compiled)
            std::printf("    round %d: %s refused its settings\n", round,
                std::string(r.e.patch().find(r.e.failed_node())->desc->id).c_str());
        CHECK(compiled);
        if (!compiled)
            continue;
        for (int block = 0; block < 8; block++) // and change settings while it runs
        {
            r.render(6000);
            const uint32_t id = ids[rng() % ids.size()];
            const Node    *n = r.e.patch().find(id);
            if (n->desc->params.empty())
                continue;
            const size_t     pi = rng() % n->desc->params.size();
            const ParamDesc &pd = n->desc->params[pi];
            CHECK(r.e.set_param(id, pi, (rng() & 1) ? pd.min : pd.max) == Err::Ok);
        }
        CHECK(r.e.nonfinite_samples() == 0);
        CHECK(r.e.apply_failures() == 0);
        CHECK(peak(r.L) <= 1.0f && peak(r.R) <= 1.0f);
    }
}

TEST(engine_reports_the_module_that_refuses_its_settings)
{
    Rig r(
        1000); // below the library's lowest sample rate: every library module refuses to initialise
    const uint32_t kb = r.add("keyboard"), osc = r.add("osc");
    CHECK(r.e.compile() == Err::InitFailed);
    CHECK(r.e.failed_node() == kb
        || r.e.failed_node()
            == osc); // the glue module has no sample-rate limit; the oscillator is the one
    CHECK(r.e.failed_node() == osc);
    std::vector<float> l(500, 1.0f), rr(500, 1.0f);
    r.e.render(500, l.data(), rr.data()); // without a plan it is silent, and does not crash
    CHECK(peak(l) == 0 && peak(rr) == 0);
}

TEST(engine_ignores_key_scripts_for_modules_that_take_no_keys)
{
    Rig a, b;
    build_voice(a);
    build_voice(b);
    for (const Node &n :
        b.e.patch().nodes()) // a stray key record for every module that is not a keyboard
        b.key(n.id, 0.05, true, 60);
    b.key(9999, 0.05, true, 60); // and one for a module that does not exist
    CHECK(a.compile() && b.compile());
    a.render(20000);
    b.render(20000);
    CHECK(same_bits(a.L, b.L));
}

namespace
{
// clock -> sequencer -> gate -> output: a gate that follows the steps
void build_gate_sequence(Rig &r, bool set_steps_before_compile, bool *ok)
{
    const uint32_t clk = r.add("clock"), seq = r.add("seq"), g = r.add("note_to_gate"),
                   out = r.add("out");
    r.param(clk, "bpm", 300);
    r.param(seq, "length", 8);
    r.param(seq, "division", 6);
    r.param(seq, "gate", 2);
    r.param(out, "level", 1);
    *ok = r.cable(clk, "clock", seq, "in") == Err::Ok && r.cable(seq, "out", g, "notes") == Err::Ok
        && r.cable(g, "gate", out, "l") == Err::Ok;
    auto steps = [&]
    {
        for (size_t i = 0; i < 8; i++)
            *ok = *ok
                && r.e.set_step(seq, i,
                       StepData { (uint8_t)(40 + i), (uint8_t)(i % 3 == 0 ? 0 : 100),
                           (uint8_t)(i % 2 ? 3 : 0), 0 })
                    == Err::Ok;
    };
    if (set_steps_before_compile)
        steps();
    *ok = *ok && r.compile();
    if (!set_steps_before_compile)
        steps(); // the live module must hear them
}
} // namespace

TEST(engine_live_step_changes_reach_the_running_sequencer)
{
    bool ok_a = false, ok_b = false;
    Rig  a, b;
    build_gate_sequence(a, true, &ok_a);
    build_gate_sequence(b, false, &ok_b);
    CHECK(ok_a && ok_b);
    a.render(2 * kRate);
    b.render(2 * kRate);
    CHECK(rms(a.L) > 0.1);
    CHECK(same_bits(a.L, b.L));
}

TEST(engine_does_not_replay_key_events_that_are_already_in_the_past)
{
    auto build = [](Rig &r)
    {
        const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate"), out = r.add("out");
        r.param(out, "level", 1);
        CHECK(
            r.cable(kb, "notes", g, "notes") == Err::Ok && r.cable(g, "gate", out, "l") == Err::Ok);
        r.key(kb, 100.0 / kRate, true, 60);
        r.key(kb, 200.0 / kRate, false, 60);
        r.key(kb, 5000.0 / kRate, true, 62);
        r.key(kb, 5200.0 / kRate, false, 62);
    };
    Rig ref;
    build(ref);
    CHECK(ref.compile());
    ref.render(8000);
    Rig r;
    build(r);
    CHECK(r.compile());
    r.render(3000); // the first pair has played
    r.add("noise"); // an edit: the script is read again, and what is already past must not be sent
                    // again
    CHECK(r.compile());
    r.render(5000);
    CHECK(same_bits(r.L, ref.L));
}

TEST(engine_delivers_a_script_longer_than_the_keyboard_queue)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), g = r.add("note_to_gate"), out = r.add("out");
    r.param(out, "level", 1);
    CHECK(r.cable(kb, "notes", g, "notes") == Err::Ok && r.cable(g, "gate", out, "l") == Err::Ok);
    for (int i = 0; i < 3000; i++)
        r.key(kb, (double)(30 * i) / kRate, i % 2 == 0, 60); // 3000 events, 1500 pulses
    CHECK(r.compile());
    r.render(30 * 3000 + 100);
    int  rising = 0;
    bool prev = false;
    for (float v : r.L)
    {
        rising += v > 0.5f && !prev;
        prev = v > 0.5f;
    }
    CHECK(rising == 1500);
}

TEST(engine_replacing_the_patch_does_not_reuse_a_state_of_another_type)
{
    // id 1 is a noise source first, then (a new patch loaded over it) an oscillator: the engine
    // must make a new oscillator, not keep the noise state under the same id
    const char *first
        = "pulserack 1\nnode 1 noise x=0 y=0\nnode 2 out x=6 y=0\ncable 1.out -> 2.l\n";
    const char *second
        = "pulserack 1\nnode 1 osc x=0 y=0\nnode 2 out x=8 y=0\ncable 1.out -> 2.l\n";
    Engine e(48000);
    CHECK(load_patch(first, e.patch()).ok && e.compile() == Err::Ok);
    std::vector<float> l(4000), r(4000);
    e.render(l.size(), l.data(), r.data());
    const void *noise_state = e.state(1);
    CHECK(load_patch(second, e.patch()).ok && e.compile() == Err::Ok);
    CHECK(e.state(1) != nullptr && e.state(1) != noise_state);
    e.reset();
    e.render(l.size(), l.data(), r.data());

    Engine fresh(48000); // the same patch in an engine that never saw the first one
    CHECK(load_patch(second, fresh.patch()).ok && fresh.compile() == Err::Ok);
    std::vector<float> fl(4000), fr(4000);
    fresh.render(fl.size(), fl.data(), fr.data());
    CHECK(same_bits(l, fl));
    CHECK(e.nonfinite_samples() == 0);
}
