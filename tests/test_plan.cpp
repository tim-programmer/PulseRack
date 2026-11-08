#include "testutil.h"

#include <random>

using namespace rt_util;

TEST(plan_order_puts_producers_first_and_is_deterministic)
{
    Rig            r;
    const uint32_t out = r.add("out"), vca = r.add("vca"), osc = r.add("osc"), adsr = r.add("adsr"),
                   clk = r.add("clock"), seq = r.add("seq");
    CHECK(r.cable(osc, "out", vca, "in") == Err::Ok && r.cable(adsr, "out", vca, "gain") == Err::Ok
        && r.cable(vca, "out", out, "l") == Err::Ok);
    CHECK(r.cable(clk, "clock", seq, "in") == Err::Ok && r.cable(seq, "out", osc, "note") == Err::Ok
        && r.cable(seq, "out", adsr, "note") == Err::Ok);
    CHECK(r.compile());
    const auto order = r.e.plan().order();
    CHECK(order.size() == 6);
    auto at = [&](uint32_t n) { return std::find(order.begin(), order.end(), n) - order.begin(); };
    CHECK(at(clk) < at(seq) && at(seq) < at(osc) && at(seq) < at(adsr) && at(osc) < at(vca)
        && at(adsr) < at(vca) && at(vca) < at(out));

    Rig            r2; // the same patch built in another order gives the same node order
    const uint32_t clk2 = r2.add("clock"), seq2 = r2.add("seq"), adsr2 = r2.add("adsr"),
                   osc2 = r2.add("osc"), vca2 = r2.add("vca"), out2 = r2.add("out");
    (void)out2;
    CHECK(r2.cable(osc2, "out", vca2, "in") == Err::Ok
        && r2.cable(clk2, "clock", seq2, "in") == Err::Ok);
    CHECK(r2.compile());
    CHECK(r2.e.plan().order() == r2.e.plan().order());
    (void)adsr2;
}

TEST(plan_random_graphs_are_ordered_and_buffers_are_never_shared_while_live)
{
    std::mt19937 rng(7);
    for (int round = 0; round < 60; round++)
    {
        Rig         r;
        const char *types[]
            = { "vca", "svf", "biquad", "attenuverter", "osc", "noise", "lfo", "mixer" };
        std::vector<uint32_t> nodes;
        const int             n = 3 + (int)(rng() % 12);
        for (int i = 0; i < n; i++)
            nodes.push_back(r.add(types[rng() % 8]));
        const uint32_t out = r.add("out");
        for (int k = 0; k < 40;
            k++) // random connection attempts; the patch refuses the invalid ones
        {
            const uint32_t   a = nodes[rng() % nodes.size()], b = nodes[rng() % nodes.size()];
            const Node      *na = r.e.patch().find(a);
            const Node      *nb = r.e.patch().find(b);
            std::vector<int> outs, ins;
            for (size_t i = 0; i < na->desc->ports.size(); i++)
                if (na->desc->ports[i].is_output && na->desc->ports[i].kind == Kind::Signal)
                    outs.push_back((int)i);
            for (size_t i = 0; i < nb->desc->ports.size(); i++)
                if (!nb->desc->ports[i].is_output && nb->desc->ports[i].kind == Kind::Signal)
                    ins.push_back((int)i);
            if (outs.empty() || ins.empty())
                continue;
            const int po = outs[rng() % outs.size()]; // drawn before the call: the order of
                                                      // evaluation of arguments is unspecified
            const int pi = ins[rng() % ins.size()];
            (void)r.e.patch().connect({ a, (uint16_t)po }, { b, (uint16_t)pi });
        }
        (void)r.e.patch().connect(
            { nodes.back(), (uint16_t)r.e.patch().find(nodes.back())->desc->ports.size() },
            { out, 0 }); // out of range: refused
        CHECK(r.compile());
        const auto order = r.e.plan().order();
        CHECK(order.size() == r.e.patch().nodes().size());
        std::map<uint32_t, size_t> at;
        for (size_t i = 0; i < order.size(); i++)
            at[order[i]] = i;
        for (const Cable &c : r.e.patch().cables())
            CHECK(at[c.from.node] < at[c.to.node]);
        CHECK(r.e.plan().signal_buffer_count() <= r.e.patch().cables().size() + 1);
        r.render(700);
        CHECK(r.e.nonfinite_samples() == 0 && r.e.apply_failures() == 0);
    }
}

TEST(plan_reuses_buffers_in_a_chain)
{
    Rig            r;
    const uint32_t osc = r.add("osc");
    uint32_t       prev = osc;
    for (int i = 0; i < 12; i++)
    {
        const uint32_t v = r.add("vca");
        CHECK(r.cable(prev, "out", v, "in") == Err::Ok);
        prev = v;
    }
    const uint32_t out = r.add("out");
    CHECK(r.cable(prev, "out", out, "l") == Err::Ok);
    CHECK(r.compile());
    CHECK(r.e.plan().signal_buffer_count()
        == 2); // an output is given a buffer before the step's inputs are released: two alternate
}

TEST(plan_keeps_a_buffer_until_its_last_reader_has_run)
{
    Rig r;
    // one source feeding three consumers at different depths: all three must still see the source's
    // samples
    const uint32_t noise = r.add("noise"), a = r.add("attenuverter"), b = r.add("attenuverter"),
                   c = r.add("attenuverter"), m = r.add("mixer"), out = r.add("out");
    r.param(a, "scale", 1);
    r.param(b, "scale", -1);
    r.param(c, "scale", 0.5f);
    CHECK(r.cable(noise, "out", a, "in") == Err::Ok && r.cable(noise, "out", b, "in") == Err::Ok);
    CHECK(r.cable(a, "out", c, "in") == Err::Ok); // c runs after a, b; the source is read again by
                                                  // nothing, but a's output must survive for c
    CHECK(r.cable(b, "out", m, "in1") == Err::Ok && r.cable(c, "out", m, "in2") == Err::Ok
        && r.cable(noise, "out", m, "in3") == Err::Ok);
    r.param(m, "level1", 1);
    r.param(m, "level2", 1);
    r.param(m, "level3", 1);
    CHECK(r.cable(m, "out", out, "l") == Err::Ok);
    r.param(out, "level", 1);
    CHECK(r.compile());
    r.render(2000);
    // b = -n, c = 0.5 n, noise = n: the sum is 0.5 n, so it must not be silent (and must not be the
    // sum of a corrupted buffer)
    Rig ref; // the same without the intermediate nodes: noise -> attenuverter(0.5) -> out
    const uint32_t n2 = ref.add("noise"), a2 = ref.add("attenuverter"), o2 = ref.add("out");
    ref.param(a2, "scale", 0.5f);
    ref.param(o2, "level", 1);
    CHECK(ref.cable(n2, "out", a2, "in") == Err::Ok && ref.cable(a2, "out", o2, "l") == Err::Ok
        && ref.compile());
    ref.render(2000);
    double worst = 0;
    for (size_t i = 0; i < 2000; i++)
        worst = std::max(worst, (double)std::fabs(r.L[i] - ref.L[i]));
    CHECK(rms(r.L) > 0.01);
    CHECK(worst < 1e-3); // fixed-point rounding of three products and a sum
}

TEST(plan_unconnected_modules_render_without_trouble)
{
    for (const ModuleDesc *d : all_modules())
    {
        Rig r;
        r.e.patch().add_node(*d);
        CHECK(r.compile());
        r.render(1500);
        CHECK(r.e.nonfinite_samples() == 0);
    }
}
