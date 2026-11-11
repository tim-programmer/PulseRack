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

TEST(geometry_jacks_are_found_where_they_are_drawn)
{
    Patch             p;
    const ModuleDesc *osc = find_module("osc"), *vca = find_module("vca");
    const uint32_t    a = p.add_node(*osc, 3, 2), b = p.add_node(*vca, 20, 2);
    for (const Node &n : p.nodes())
        for (size_t k = 0; k < n.desc->ports.size(); k++)
        {
            const Pt    c = jack_center(n, k);
            const Rect &j = panel_layout(*n.desc).jacks[k];
            CHECK(c.x == n.x * kHp + kPanelBorder + j.x + j.w / 2);
            auto hit = jack_at(p, c.x, c.y);
            CHECK(hit && hit->node == n.id && hit->port == k);
            hit = jack_at(p, c.x + kJackSize / 2 + 3, c.y); // within the slack
            CHECK(hit && hit->node == n.id && hit->port == k);
            CHECK(node_rect(*n.desc, n.x, n.y).contains(c.x, c.y));
        }
    const Node *na = p.find(a);
    const Pt    c0 = jack_center(*na, 0);
    CHECK(!jack_at(p, c0.x, c0.y - 40));                    // a control, not a jack
    CHECK(!jack_at(p, 1000, 1000) && !jack_at(p, -50, 40)); // empty rack
    (void)b;
}

TEST(geometry_cables_sag_and_orient)
{
    const auto c = cable_curve({ 100, 300 }, { 500, 280 });
    CHECK(c[0].x == 100 && c[0].y == 300 && c[3].x == 500 && c[3].y == 280);
    CHECK(c[1].y > 300 && c[2].y > 280); // the control points hang below both ends
    const auto shorter = cable_curve({ 100, 300 }, { 160, 300 });
    CHECK(shorter[1].y - 300 < c[1].y - 300);              // a short cable sags less
    CHECK(cable_curve({ 0, 0 }, { 5000, 0 })[1].y <= 240); // and a long one has a limit

    Patch             p;
    const ModuleDesc *osc = find_module("osc"), *vca = find_module("vca");
    const uint32_t    a = p.add_node(*osc, 0, 0), b = p.add_node(*vca, 8, 0);
    const PortRef     out { a, (uint16_t)find_port(*osc, "out") },
        in { b, (uint16_t)find_port(*vca, "in") };
    auto [f1, t1] = as_cable(p, out, in);
    auto [f2, t2] = as_cable(p, in, out); // a drag that started at the input
    CHECK(f1 == out && t1 == in && f2 == out && t2 == in);
    const PortRef out2 { b, (uint16_t)find_port(*vca, "out") };
    auto [f3, t3] = as_cable(p, out, out2); // two outputs: as given, for can_connect to refuse
    CHECK(f3 == out && t3 == out2 && p.can_connect(f3, t3) == Err::NotAnInput);
}

TEST(geometry_the_step_grid_fits_between_the_controls_and_the_jacks)
{
    for (const ModuleDesc *d : all_modules())
    {
        if (!d->has_steps)
            continue;
        const PanelLayout &L = panel_layout(*d);
        const StepGrid     g = step_grid(L);
        CHECK(g.area.y >= L.controls_bottom && g.area.bottom() <= L.jacks_top);
        CHECK(g.area.x >= kPad && g.area.right() <= L.width - kPad + 0.01f);
        std::vector<Rect> all;
        for (int p = 0; p < kGridPages; p++)
            all.push_back(g.tab(p));
        for (int c = 0; c < kGridColumns; c++)
        {
            all.push_back(g.note_bar(c));
            all.push_back(g.on_pad(c));
            all.push_back(g.tie_pad(c));
            CHECK(g.note_bar(c).w >= 10); // big enough to hit
            const Rect &b = g.note_bar(c);
            CHECK(g.column_at(b.x + b.w / 2) == c && g.column_at(b.x + 1) == c);
        }
        for (size_t i = 0; i < all.size(); i++)
        {
            CHECK(all[i].x >= g.area.x - 0.01f && all[i].right() <= g.area.right() + 0.01f);
            CHECK(all[i].y >= g.area.y && all[i].bottom() <= g.area.bottom());
            for (size_t j = 0; j < i; j++)
                CHECK(!all[i].overlaps(all[j]));
        }
        CHECK(g.column_at(g.area.x - 1) == -1 && g.column_at(g.area.right() + 1) == -1);
    }
    CHECK(note_fill(24) == 0 && note_fill(108) == 1 && note_fill(0) == 0 && note_fill(127) == 1);
    CHECK(std::fabs(note_fill(66) - 0.5f) < 1e-6f);
}

TEST(geometry_the_keyboard_strip)
{
    const PanelLayout &L = panel_layout(*find_module("keyboard"));
    const KeyStrip     k = keyboard_strip(L);
    CHECK(k.area.y >= L.controls_bottom && k.area.bottom() <= L.jacks_top);
    CHECK(k.area.x >= kPad && k.area.right() <= L.width - kPad);
    CHECK(k.area.h / kStripWhiteKeys >= 16); // a key big enough to press
    int whites = 0;
    for (int s = 0; s < kStripSemitones; s++)
    {
        const Rect r = k.key(s);
        CHECK(r.x >= k.area.x && r.right() <= k.area.right() + 0.01f);
        CHECK(r.y >= k.area.y - 0.01f && r.bottom() <= k.area.bottom() + 0.01f);
        // the middle of every key finds it (a white key's middle, right of the black keys)
        const float x = k.is_black(s) ? r.x + r.w / 2 : r.right() - 3;
        CHECK(k.key_at(x, r.y + r.h / 2) == s);
        whites += !k.is_black(s);
        if (s > 0) // higher notes are higher up
            CHECK(r.y + r.h / 2 < k.key(s - 1).y + k.key(s - 1).h / 2);
    }
    CHECK(whites == kStripWhiteKeys);
    CHECK(k.is_black(1) && k.is_black(13) && !k.is_black(4) && !k.is_black(5));
    CHECK(k.key_at(k.area.x - 1, k.area.y + 5) == -1
        && k.key_at(k.area.x + 2, k.area.bottom() + 1) == -1);
    // the bottom left corner is the lowest C; the top right the highest B
    CHECK(k.key_at(k.area.x + 1, k.area.bottom() - 1) == 0);
    CHECK(k.key_at(k.area.right() - 1, k.area.y + 1) == 23);
}
