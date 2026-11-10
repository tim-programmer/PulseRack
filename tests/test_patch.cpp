#include "testutil.h"

using namespace rt_util;

TEST(patch_connect_rules)
{
    Rig            r;
    const uint32_t osc = r.add("osc"), vca = r.add("vca"), svf = r.add("svf"), seq = r.add("seq"),
                   kb = r.add("keyboard"), out = r.add("out");
    Patch         &p = r.e.patch();
    auto P = [&](uint32_t n, const char *id) { return PortRef { n, (uint16_t)r.port(n, id) }; };

    CHECK(p.connect(P(osc, "out"), P(vca, "in")) == Err::Ok);
    CHECK(p.connect(P(osc, "out"), P(svf, "in")) == Err::Ok);        // fan-out is free
    CHECK(p.connect(P(svf, "lp"), P(vca, "in")) == Err::InputInUse); // an input takes one cable
    CHECK(p.connect(P(svf, "lp"), P(vca, "gain")) == Err::Ok);
    CHECK(p.connect(P(osc, "out"), P(seq, "in"))
        == Err::KindMismatch); // a signal into an event input
    CHECK(p.connect(P(kb, "notes"), P(vca, "in"))
        == Err::KindMismatch); // an event into a signal input
    CHECK(p.connect(P(vca, "in"), P(svf, "cutoff")) == Err::NotAnOutput);   // from an input
    CHECK(p.connect(P(osc, "out"), P(osc, "sync_out")) == Err::NotAnInput); // to an output
    CHECK(p.connect(P(vca, "out"), P(out, "l")) == Err::Ok);
    CHECK(p.connect(P(kb, "notes"), P(osc, "note")) == Err::Ok);
    CHECK(p.connect(PortRef { 999, 0 }, P(out, "r")) == Err::NoSuchNode);
    CHECK(p.connect(PortRef { osc, 99 }, P(out, "r")) == Err::NoSuchPort);
    CHECK(p.cables().size() == 5);
    for (Err e : { Err::Ok, Err::WouldLoop, Err::KindMismatch, Err::InputInUse, Err::NotAnInput,
             Err::NotAnOutput, Err::NoSuchNode, Err::NoSuchPort })
        CHECK(std::strlen(describe(e)) >= 2);
}

TEST(patch_refuses_loops)
{
    Rig            r;
    const uint32_t a = r.add("vca"), b = r.add("vca"), c = r.add("vca");
    CHECK(r.cable(a, "out", b, "in") == Err::Ok);
    CHECK(r.cable(b, "out", c, "in") == Err::Ok);
    CHECK(r.cable(c, "out", a, "in") == Err::WouldLoop); // a -> b -> c -> a
    CHECK(r.cable(c, "out", a, "gain") == Err::WouldLoop);
    CHECK(r.cable(a, "out", a, "gain") == Err::WouldLoop); // a module into itself
    CHECK(r.cable(a, "out", c, "gain") == Err::Ok);        // a diamond is not a loop
    CHECK(r.e.patch().reaches(a, c) && !r.e.patch().reaches(c, a));
    CHECK(r.e.patch().cables().size() == 3);
}

TEST(patch_remove_and_disconnect)
{
    Rig            r;
    const uint32_t a = r.add("osc"), b = r.add("vca"), c = r.add("out"), kb = r.add("keyboard");
    CHECK(r.cable(a, "out", b, "in") == Err::Ok);
    CHECK(r.cable(b, "out", c, "l") == Err::Ok);
    r.key(kb, 0.5, true, 60);
    Patch &p = r.e.patch();
    CHECK(p.disconnect({ c, (uint16_t)r.port(c, "l") }) == Err::Ok);
    CHECK(p.disconnect({ c, (uint16_t)r.port(c, "l") }) == Err::NoSuchPort);
    CHECK(r.cable(b, "out", c, "l") == Err::Ok);
    CHECK(p.remove_node(b) == Err::Ok);
    CHECK(p.cables().empty() && p.find(b) == nullptr && p.nodes().size() == 3);
    CHECK(p.remove_node(b) == Err::NoSuchNode);
    CHECK(p.remove_node(kb) == Err::Ok && p.keys.empty());
    const uint32_t again = r.add(
        "vca"); // ids are not reused while smaller ones are free? they must at least be unique
    CHECK(p.find(again) != nullptr && again != a && again != c);
}

TEST(patch_params_and_steps_are_clamped)
{
    Rig            r;
    const uint32_t osc = r.add("osc"), seq = r.add("seq");
    Patch         &p = r.e.patch();
    float          stored = 0;
    CHECK(p.set_param(osc, (size_t)r.param_index(osc, "frequency"), 1e9f, &stored) == Err::Ok
        && stored == 8000);
    CHECK(p.set_param(osc, (size_t)r.param_index(osc, "waveform"), 2.7f, &stored) == Err::Ok
        && stored == 3);
    CHECK(p.set_param(osc, 99, 1) == Err::NoSuchParam);
    CHECK(p.set_param(999, 0, 1) == Err::NoSuchNode);
    CHECK(p.set_step(seq, 3, StepData { 200, 250, 250, 7 }) == Err::Ok);
    const StepData s = p.find(seq)->steps[3];
    CHECK(s.note == 127 && s.velocity == 127 && s.gate == 96 && s.tie == 1);
    CHECK(p.set_step(seq, 64, StepData { }) == Err::NoSuchStep);
    CHECK(p.set_step(osc, 0, StepData { }) == Err::NoSuchStep); // only the sequencer has steps
    CHECK(p.find(osc)->steps.empty() && p.find(seq)->steps.size() == 64);
}

TEST(patch_index_boundaries)
{
    Rig            r;
    const uint32_t osc = r.add("osc"), vca = r.add("vca");
    Patch         &p = r.e.patch();
    const uint16_t n_osc = (uint16_t)p.find(osc)->desc->ports.size(),
                   n_vca = (uint16_t)p.find(vca)->desc->ports.size();
    CHECK(p.connect({ osc, n_osc }, { vca, 0 }) == Err::NoSuchPort); // one past the last jack
    CHECK(p.connect({ osc, (uint16_t)(n_osc - 1) }, { vca, n_vca }) == Err::NoSuchPort);
    CHECK(p.connect({ osc, (uint16_t)r.port(osc, "out") }, { vca, (uint16_t)(n_vca - 1) })
        == Err::NotAnInput); // the last jack is real (an output)
    const size_t n_params = p.find(osc)->desc->params.size();
    CHECK(p.set_param(osc, n_params - 1, 0) == Err::Ok);      // the last control is real
    CHECK(p.set_param(osc, n_params, 0) == Err::NoSuchParam); // one past it is not
    CHECK(p.set_step(find_module("seq") ? p.add_node(*find_module("seq")) : 0, 63, StepData { })
        == Err::Ok);
}

TEST(patch_new_ids_skip_the_ones_a_file_already_used)
{
    Patch p;
    CHECK(load_patch("pulserack 1\nnode 1 noise\nnode 2 noise\nnode 5 noise\n", p).ok);
    CHECK(p.add_node(*find_module("vca")) == 3); // the first free id
    CHECK(p.add_node(*find_module("vca")) == 4);
    CHECK(p.add_node(*find_module("vca")) == 6); // 5 is taken
    CHECK(p.nodes().size() == 6);
}

TEST(patch_removing_a_module_keeps_the_other_keyboards_keys)
{
    Rig            r;
    const uint32_t k1 = r.add("keyboard"), k2 = r.add("keyboard");
    r.key(k1, 0.1, true, 60);
    r.key(k2, 0.2, true, 62);
    r.key(k1, 0.3, false, 60);
    CHECK(r.e.patch().remove_node(k1) == Err::Ok);
    CHECK(r.e.patch().keys.size() == 1 && r.e.patch().keys[0].node == k2
        && r.e.patch().keys[0].note == 62);
}

TEST(patch_can_connect_agrees_with_connect_and_changes_nothing)
{
    // every ordered pair of jacks in a small patch, tried in a random order: can_connect predicts
    // connect exactly, and leaves the patch as it was
    Rig            r;
    const uint32_t ids[] = { r.add("osc"), r.add("vca"), r.add("svf"), r.add("seq"),
        r.add("keyboard"), r.add("out"), r.add("adsr") };
    Patch         &p = r.e.patch();
    std::vector<PortRef> all;
    for (uint32_t id : ids)
        for (uint16_t k = 0; k < p.find(id)->desc->ports.size(); k++)
            all.push_back(PortRef { id, k });
    std::vector<std::pair<PortRef, PortRef>> pairs;
    for (const PortRef &a : all)
        for (const PortRef &b : all)
            pairs.push_back({ a, b });
    uint32_t seed = 7;
    for (size_t i = pairs.size() - 1; i > 0; i--)
    {
        seed = seed * 1664525u + 1013904223u;
        std::swap(pairs[i], pairs[(seed >> 8) % (i + 1)]);
    }
    int made = 0;
    for (const auto &[a, b] : pairs)
    {
        const size_t before = p.cables().size();
        const Err    predicted = p.can_connect(a, b);
        CHECK(p.cables().size() == before);
        const Err actual = p.connect(a, b);
        CHECK(predicted == actual);
        CHECK(p.cables().size() == before + (actual == Err::Ok));
        made += actual == Err::Ok;
    }
    CHECK(made > 5);
    CHECK(p.can_connect(PortRef { 999, 0 }, all[0]) == Err::NoSuchNode);
}
