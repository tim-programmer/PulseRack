// The rack's geometry without a window (src/ui/geometry.h): panel layouts for the whole catalogue,
// the knob mapping, value text, and placement.
#include "testutil.h"

#include "engine/model/patchfile.h"
#include "ui/geometry.h"

#include <cmath>
#include <fstream>
#include <sstream>

using namespace rt_util;
using namespace rack::ui;

TEST(geometry_every_panel_fits_and_nothing_overlaps)
{
    for (const ModuleDesc *d : all_modules())
    {
        const PanelLayout &L = panel_layout(*d);
        CHECK(!d->description.empty());
        CHECK(L.width == d->width_hp * kHp && L.height == kPanelHeight);
        CHECK(L.params.size() == d->params.size() && L.ports.size() == d->ports.size());
        CHECK(L.fits()); // controls end above the jacks
        const Rect        panel { 0, kTitleHeight, L.width, L.height - kTitleHeight };
        std::vector<Rect> all = L.params;
        all.insert(all.end(), L.ports.begin(), L.ports.end());
        for (size_t i = 0; i < all.size(); i++)
        {
            const Rect &r = all[i];
            CHECK(r.w > 0 && r.h > 0);
            CHECK(r.x >= kPad - 0.01f && r.right() <= L.width - kPad + 0.01f);
            CHECK(r.y >= panel.y && r.bottom() <= L.height - kPad + 0.01f);
            for (size_t j = 0; j < i; j++)
                CHECK(!r.overlaps(all[j]));
        }
        for (size_t i = 0; i < d->ports.size(); i++)
        { // the jack is inside its cell
            const Rect &c = L.ports[i], &j = L.jacks[i];
            CHECK(j.x >= c.x && j.right() <= c.right() && j.y >= c.y && j.bottom() <= c.bottom());
        }
        if (!L.fits())
            std::printf("  (%.*s does not fit: controls end at %.0f, jacks start at %.0f)\n",
                (int)d->id.size(), d->id.data(), (double)L.controls_bottom, (double)L.jacks_top);
    }
}

TEST(geometry_selectors_take_a_row_and_rows_are_in_descriptor_order)
{
    for (const ModuleDesc *d : all_modules())
    {
        const PanelLayout &L = panel_layout(*d);
        for (size_t i = 0; i < d->params.size(); i++)
        {
            if (d->params[i].control == Control::Selector)
                CHECK(std::fabs(L.params[i].w - (L.width - 2 * kPad)) < 0.01f);
            if (i > 0) // reading order: same row to the right, or a lower row
                CHECK(L.params[i].y > L.params[i - 1].y
                    || (L.params[i].y == L.params[i - 1].y && L.params[i].x > L.params[i - 1].x));
        }
    }
}

TEST(geometry_travel_round_trips_and_ends_are_exact)
{
    for (const ModuleDesc *d : all_modules())
        for (const ParamDesc &p : d->params)
        {
            CHECK(from_travel(p, 0) == clamp_param(p, p.min));
            CHECK(from_travel(p, 1) == clamp_param(p, p.max));
            CHECK(to_travel(p, p.min) == 0 && to_travel(p, p.max) == 1);
            CHECK(
                from_travel(p, -5) == from_travel(p, 0) && from_travel(p, 7) == from_travel(p, 1));
            CHECK(to_travel(p, NAN) == 0);
            if (is_discrete(p))
                continue;
            for (float t = 0; t <= 1.0001f; t += 0.05f) // continuous: travel -> value -> travel
                CHECK(std::fabs(to_travel(p, from_travel(p, t)) - std::min(t, 1.f)) < 1e-4f);
            float prev = -INFINITY; // and monotonic
            for (float t = 0; t <= 1.0001f; t += 0.01f)
            {
                const float v = from_travel(p, t);
                CHECK(v >= prev);
                prev = v;
            }
        }
}

TEST(geometry_log_knob_is_log)
{
    const ModuleDesc *svf = find_module("svf");
    const ParamDesc  &cut = svf->params[(size_t)find_param(*svf, "cutoff")];
    CHECK(cut.scale == Scale::Log);
    // halfway is the geometric mean: sqrt(20 * 20000) = 632 Hz
    CHECK(std::fabs(from_travel(cut, 0.5f) - std::sqrt(cut.min * cut.max)) < 0.5f);
}

TEST(geometry_drag_and_wheel)
{
    const ModuleDesc *vca = find_module("vca");
    const ParamDesc  &g = vca->params[0]; // 0 .. 1.9, linear
    float             t = 0;
    float             v = drag_value(g, 0, kDragPixels, false, &t); // the whole travel up
    CHECK(t == 1 && v == g.max);
    v = drag_value(g, 1, -kDragPixels / 2, false, &t);
    CHECK(std::fabs(t - 0.5f) < 1e-6f && std::fabs(v - g.max / 2) < 1e-5f);
    v = drag_value(g, 0.5f, kDragPixels / 2, true, &t); // Shift: a tenth as far
    CHECK(std::fabs(t - 0.55f) < 1e-6f);
    drag_value(g, 0.9f, 10 * kDragPixels, false, &t); // past the end stops there
    CHECK(t == 1);

    // discrete controls accumulate travel, so slow drags still get there
    const ModuleDesc *clk = find_module("clock");
    const ParamDesc  &ppqn = clk->params[(size_t)find_param(*clk, "ppqn")];
    CHECK(ppqn.scale == Scale::Int);
    float tt = to_travel(ppqn, ppqn.def), before = ppqn.def;
    for (int i = 0; i < 400; i++)
        before = drag_value(ppqn, tt, 1, false, &tt);
    CHECK(before == ppqn.max);

    // the wheel: one step for discrete controls, 1 % (0.1 % with Shift) of the travel otherwise
    const ModuleDesc *osc = find_module("osc");
    const ParamDesc  &wave = osc->params[(size_t)find_param(*osc, "waveform")];
    CHECK(step_value(wave, 2, 1, false) == 3 && step_value(wave, 3, 1, false) == 3);
    CHECK(step_value(wave, 0, -1, false) == 0);
    const float a = step_value(g, 1, 1, false), b = step_value(g, 1, 1, true);
    CHECK(std::fabs((a - 1) - 0.019f) < 1e-5f && std::fabs((b - 1) - 0.0019f) < 1e-5f);
}

TEST(geometry_value_text)
{
    ParamDesc hz { "f", "F", "Hz", 20, 20000, 440, Scale::Log, { }, Control::Knob, nullptr };
    CHECK(format_value(hz, 440) == "440 Hz");
    CHECK(format_value(hz, 1200) == "1.20 kHz");
    CHECK(format_value(hz, 15000) == "15.0 kHz");
    CHECK(format_value(hz, 2.5f) == "2.50 Hz");
    ParamDesc ms { "t", "T", "ms", 1, 10000, 10, Scale::Log, { }, Control::Knob, nullptr };
    CHECK(format_value(ms, 350) == "350 ms" && format_value(ms, 2500) == "2.50 s");
    ParamDesc plain { "x", "X", "", -1, 1, 0, Scale::Linear, { }, Control::Knob, nullptr };
    CHECK(format_value(plain, 0.7f) == "0.70" && format_value(plain, -0.25f) == "-0.25");
    ParamDesc tog { "x", "X", "", 0, 1, 0, Scale::Toggle, { }, Control::Switch, nullptr };
    CHECK(format_value(tog, 1) == "on" && format_value(tog, 0) == "off");
    ParamDesc n { "x", "X", "bpm", 1, 300, 120, Scale::Int, { }, Control::Knob, nullptr };
    CHECK(format_value(n, 120) == "120 bpm");
    const ModuleDesc *osc = find_module("osc");
    CHECK(format_value(osc->params[(size_t)find_param(*osc, "waveform")], 2) == "saw");
    // every control of every module can be written at its ends and default
    for (const ModuleDesc *d : all_modules())
        for (const ParamDesc &p : d->params)
            CHECK(!format_value(p, p.min).empty() && !format_value(p, p.def).empty()
                && !format_value(p, p.max).empty());
}

TEST(geometry_collisions_and_free_slots)
{
    Patch             p;
    const ModuleDesc *osc = find_module("osc"), *vca = find_module("vca");
    p.add_node(*osc, 0, 0);                    // 0..8
    const uint32_t b = p.add_node(*vca, 8, 0); // 8..12, touching is fine
    CHECK(!collides(p, *vca, 12, 0));
    CHECK(collides(p, *vca, 7, 0));
    CHECK(collides(p, *vca, 11, 0));
    CHECK(collides(p, *vca, 8, 0));
    CHECK(!collides(p, *vca, 8, 0, b)); // itself does not count
    CHECK(collides(p, *vca, 0, kPanelHeightHp - 1));
    CHECK(!collides(p, *vca, 0, kPanelHeightHp));
    CHECK(collides(p, *vca, -1, 30) && collides(p, *vca, 30, -1));

    Slot s = free_slot(p, *osc, 0, 0, 40);
    CHECK(s.x == 12 && s.y == 0);
    s = free_slot(p, *osc, 0, 0, 14); // no room in the first row within 14 HP: the second row
    CHECK(s.x == 0 && s.y == kPanelHeightHp);
    s = free_slot(p, *osc, 0, 0, 14, 1); // one row only: further right on it
    CHECK(s.x == 12 && s.y == 0);
    p.add_node(*osc, (float)s.x, (float)s.y);
    CHECK(!collides(p, *vca, 20, 0) && collides(p, *vca, 19, 0));
}

TEST(geometry_resolve_overlaps)
{
    Patch             p;
    const ModuleDesc *seq = find_module("seq"), *osc = find_module("osc");
    p.add_node(*seq, 8, 0);          // 8..24
    p.add_node(*osc, 18, 0);         // overlaps the sequencer
    p.add_node(*osc, -3, 2.4f);      // off the rack, and not on the grid
    CHECK(resolve_overlaps(p) == 1); // only the overlap counts as a move; snapping does not
    CHECK(p.find(2)->x == 24 && p.find(2)->y == 0);
    CHECK(p.find(3)->x == 0 && p.find(3)->y == 2); // snapped, then it fits at x = 0
    CHECK(resolve_overlaps(p) == 0);
    for (const Node &n : p.nodes())
        CHECK(!collides(p, *n.desc, (int)n.x, (int)n.y, n.id));
}

TEST(geometry_example_patches_need_no_moving)
{
    for (const char *name : { "sequence", "arpeggio", "filter_sweep" })
    {
        std::ifstream     f(std::string(RACK_SOURCE_DIR) + "/examples/" + name + ".rack");
        std::stringstream ss;
        ss << f.rdbuf();
        Patch p;
        CHECK(load_patch(ss.str(), p).ok);
        CHECK(!p.nodes().empty());
        CHECK(resolve_overlaps(p) == 0);
    }
}
