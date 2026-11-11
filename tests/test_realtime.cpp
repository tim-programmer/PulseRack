// The engine with an audio thread attached (docs/architecture.md, *Threads*): commands through the
// ring, plans swapped at block boundaries, states freed only after the audio thread let go, and
// telemetry. The stress test is the one to run under ThreadSanitizer and AddressSanitizer.
#include "testutil.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

using namespace rt_util;

namespace
{

// A small voice: keyboard -> oscillator + ADSR -> VCA -> filter -> output, an LFO on the cutoff.
struct Voice
{
    uint32_t kb, osc, adsr, vca, svf, lfo, out;
};
Voice build(Engine &e)
{
    Patch &p = e.patch();
    auto   add = [&](const char *t) { return p.add_node(*find_module(t)); };
    auto   P = [&](uint32_t n, const char *id)
    { return PortRef { n, (uint16_t)find_port(*p.find(n)->desc, id) }; };
    Voice v { add("keyboard"), add("osc"), add("adsr"), add("vca"), add("svf"), add("lfo"),
        add("out") };
    CHECK(p.connect(P(v.kb, "notes"), P(v.osc, "note")) == Err::Ok);
    CHECK(p.connect(P(v.kb, "notes"), P(v.adsr, "note")) == Err::Ok);
    CHECK(p.connect(P(v.osc, "out"), P(v.vca, "in")) == Err::Ok);
    CHECK(p.connect(P(v.adsr, "out"), P(v.vca, "gain")) == Err::Ok);
    CHECK(p.connect(P(v.vca, "out"), P(v.svf, "in")) == Err::Ok);
    CHECK(p.connect(P(v.lfo, "out"), P(v.svf, "cutoff")) == Err::Ok);
    CHECK(p.connect(P(v.svf, "lp"), P(v.out, "l")) == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    return v;
}
size_t pidx(Engine &e, uint32_t n, const char *id)
{
    return (size_t)find_param(*e.patch().find(n)->desc, id);
}

// The same edits between the same blocks, applied at once (detached) or by render() (attached): the
// samples must be identical, bit for bit.
std::vector<float> session(bool attached)
{
    Engine e(48000);
    if (attached)
        e.attach_audio(true);
    const Voice        v = build(e);
    std::vector<float> all, l(300), r(300);
    auto               block = [&](size_t n)
    {
        e.render(n, l.data(), r.data());
        all.insert(all.end(), l.begin(), l.begin() + (long)n);
    };
    block(300);
    CHECK(e.key(v.kb, true, 57) == Err::Ok);
    block(257);
    CHECK(e.set_param(v.svf, pidx(e, v.svf, "resonance"), 0.8f) == Err::Ok);
    CHECK(e.set_param(v.osc, pidx(e, v.osc, "waveform"), 3) == Err::Ok);
    block(100);
    CHECK(e.key(v.kb, true, 64, 90, (int64_t)e.time() + 37) == Err::Ok); // at a given sample
    block(300);
    // a module added and patched in, mid-render
    Patch         &p = e.patch();
    const uint32_t noise = p.add_node(*find_module("noise"));
    CHECK(p.connect({ noise, 0 }, { v.out, (uint16_t)find_port(*p.find(v.out)->desc, "r") })
        == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    block(211);
    CHECK(e.key(v.kb, false, 64) == Err::Ok);
    CHECK(e.set_param(v.adsr, pidx(e, v.adsr, "release"), 40) == Err::Ok);
    block(300);
    // and one removed (its cables go with it)
    CHECK(p.remove_node(v.lfo) == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    block(300);
    e.reset();
    CHECK(e.time() == (attached ? 1768u : 0u)); // attached: the reset waits for the next block
    block(300);
    CHECK(e.time() == 300);
    CHECK(e.apply_failures() == 0 && e.nonfinite_samples() == 0);
    if (attached)
        e.attach_audio(false);
    return all;
}

} // namespace

TEST(realtime_attached_render_equals_detached_bit_for_bit)
{
    const auto a = session(false), b = session(true);
    CHECK(a.size() == b.size());
    CHECK(same_bits(a, b));
    float peak = 0;
    for (float x : a)
        peak = std::max(peak, std::fabs(x));
    CHECK(peak > 0.05f); // it made sound
}

TEST(realtime_edits_wait_for_the_audio_thread)
{
    Engine e(48000);
    e.attach_audio(true);
    const Voice v = build(e);
    CHECK(e.plan().step_count() == 0); // published, not yet adopted
    std::vector<float> l(64), r(64);
    e.render(64, l.data(), r.data());
    CHECK(e.plan().step_count() == 7); // adopted at the block boundary
    // a removed module's state stays alive until the audio thread has a plan without it
    const void *lfo_state = e.state(v.lfo);
    CHECK(lfo_state != nullptr);
    CHECK(e.patch().remove_node(v.lfo) == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    CHECK(e.state(v.lfo) == nullptr);  // the model let go
    CHECK(e.plan().step_count() == 7); // the audio side still runs the old plan
    e.render(64, l.data(), r.data());  // ... until here (and the state is freed after)
    CHECK(e.plan().step_count() == 6);
    e.collect();
    // two plans published before the audio thread takes one: only the last runs
    CHECK(e.patch().remove_node(v.svf) == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    CHECK(e.patch().remove_node(v.vca) == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    e.render(64, l.data(), r.data());
    CHECK(e.plan().step_count() == 4);
    e.attach_audio(false);
}

TEST(realtime_a_full_ring_loses_commands_but_never_blocks)
{
    Engine e(48000);
    e.attach_audio(true);
    const Voice  v = build(e);
    const size_t gain = pidx(e, v.vca, "gain");
    for (int i = 0; i < 5000; i++) // nobody renders: the ring (4096) fills
        CHECK(e.set_param(v.vca, gain, 0.001f * (float)(i % 1000)) == Err::Ok);
    CHECK(e.telemetry().commands_lost.load() == 5000 - 4096);
    CHECK(e.patch().find(v.vca)->params[gain] == 0.001f * 999); // the model has the last value
    std::vector<float> l(64), r(64);
    e.render(64, l.data(), r.data());                 // drains what fitted
    CHECK(e.set_param(v.vca, gain, 0.5f) == Err::Ok); // and there is room again
    CHECK(e.telemetry().commands_lost.load() == 5000 - 4096);
    e.attach_audio(false);
}

TEST(realtime_telemetry_levels_and_activity)
{
    Engine         e(48000);
    Patch         &p = e.patch();
    const uint32_t osc = p.add_node(*find_module("osc")), out = p.add_node(*find_module("out"));
    const uint32_t idle = p.add_node(*find_module("lfo")); // its output goes nowhere
    CHECK(p.connect({ osc, (uint16_t)find_port(*p.find(osc)->desc, "out") },
              { out, (uint16_t)find_port(*p.find(out)->desc, "l") })
        == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    std::vector<float> l(4800), r(4800);
    e.render(l.size(), l.data(), r.data());
    float pk = 0;
    for (float x : l)
        pk = std::max(pk, std::fabs(x));
    Telemetry &t = e.telemetry();
    CHECK(pk > 0.1f && t.take_peak(0) == pk); // the largest |sample| since last taken
    CHECK(t.take_peak(0) == 0);               // taken
    CHECK(t.take_peak(1) == pk);              // mono: L alone plays on both
    const int so = e.telemetry_slot(osc), si = e.telemetry_slot(idle);
    CHECK(so >= 0 && si >= 0 && so != si && e.telemetry_slot(12345) == -1);
    CHECK(t.take_activity(so) > 0.5f); // the oscillator's connected output
    CHECK(t.take_activity(si) == 0.f); // an unconnected output is not measured
    CHECK(t.take_activity(so) == 0.f); // taken
    // slots are reused once a module is gone
    CHECK(p.remove_node(idle) == Err::Ok);
    CHECK(e.compile() == Err::Ok);
    const uint32_t again = p.add_node(*find_module("noise"));
    CHECK(e.compile() == Err::Ok);
    CHECK(e.telemetry_slot(again) == si);
}

TEST(realtime_a_new_sample_rate_is_a_fresh_start_at_that_rate)
{
    Engine a(44100);
    build(a);
    std::vector<float> l(2000), r(2000);
    a.render(l.size(), l.data(), r.data());
    a.set_sample_rate(48000);
    CHECK(a.sample_rate() == 48000 && a.time() == 0);
    std::vector<float> la(3000), ra(3000), lb(3000), rb(3000);
    a.render(la.size(), la.data(), ra.data());
    Engine b(48000);
    build(b);
    b.render(lb.size(), lb.data(), rb.data());
    CHECK(same_bits(la, lb));
}

TEST(realtime_stress_an_audio_thread_and_a_ui_thread)
{
    // A real audio thread renders all the time while this thread edits at random: parameters, keys,
    // modules and cables added and removed, resets. Run it under TSan (races) and ASan (a state
    // used after it was freed); in a plain build it checks that nothing breaks.
    Engine e(48000);
    e.attach_audio(true);
    std::atomic<bool>     stop { false };
    std::atomic<uint64_t> blocks { 0 };
    std::thread           audio(
        [&]
        {
            std::vector<float> l(512), r(512);
            std::mt19937       rng(1);
            while (!stop.load())
            {
                e.render(1 + rng() % 512, l.data(), r.data());
                blocks++;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

    std::mt19937 rng(7);
    auto         pick = [&](size_t n) { return (size_t)(rng() % n); };
    const auto   mods = all_modules();
    int          edits = 0;
    const auto   until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < until)
    {
        Patch &p = e.patch();
        switch (pick(10))
        {
        case 0:
        case 1:
            if (p.nodes().size() < 24)
            {
                p.add_node(*mods[pick(mods.size())]);
                CHECK(e.compile() == Err::Ok);
            }
            break;
        case 2:
            if (!p.nodes().empty())
            {
                CHECK(p.remove_node(p.nodes()[pick(p.nodes().size())].id) == Err::Ok);
                CHECK(e.compile() == Err::Ok);
            }
            break;
        case 3:
        case 4:
            if (p.nodes().size() > 1)
            { // a random cable, if the rules allow it
                const Node &a = p.nodes()[pick(p.nodes().size())],
                           &b = p.nodes()[pick(p.nodes().size())];
                const PortRef pa { a.id, (uint16_t)pick(a.desc->ports.size()) },
                    pb { b.id, (uint16_t)pick(b.desc->ports.size()) };
                if (p.connect(pa, pb) == Err::Ok)
                    CHECK(e.compile() == Err::Ok);
            }
            break;
        case 5:
            if (!p.cables().empty())
            {
                CHECK(p.disconnect(p.cables()[pick(p.cables().size())].to) == Err::Ok);
                CHECK(e.compile() == Err::Ok);
            }
            break;
        case 6:
        case 7:
        case 8:
            if (!p.nodes().empty())
            {
                const Node &n = p.nodes()[pick(p.nodes().size())];
                if (!n.desc->params.empty())
                {
                    const size_t     k = pick(n.desc->params.size());
                    const ParamDesc &d = n.desc->params[k];
                    CHECK(e.set_param(
                              n.id, k, d.min + (d.max - d.min) * (float)(rng() % 1000) / 999.f)
                        == Err::Ok);
                }
                if (n.desc->id == "keyboard")
                    CHECK(e.key(n.id, rng() % 2, (uint8_t)(36 + pick(48))) == Err::Ok);
            }
            break;
        case 9:
            if (pick(20) == 0)
                e.reset();
            break;
        }
        edits++;
        if (edits % 16 == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    stop.store(true);
    audio.join();
    e.attach_audio(false);
    std::printf(
        "  (%d edits while %llu blocks were rendered)\n", edits, (unsigned long long)blocks.load());
    CHECK(edits > 1000 && blocks.load() > 100);
    CHECK(e.nonfinite_samples() == 0);
    // after the audio thread is gone, everything it left is applied and freed
    std::vector<float> l(256), r(256);
    e.render(256, l.data(), r.data());
    for (float x : l)
        CHECK(std::isfinite(x) && std::fabs(x) <= 1);
}
