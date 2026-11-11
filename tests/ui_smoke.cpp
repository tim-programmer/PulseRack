// The window, driven like a user would, through gpuic's synthetic input (it goes through the real
// event path: hit testing, capture, handlers). Needs a display; exits 77 (skipped) without one.
//   ui_smoke                  the interaction test
//   ui_smoke --check-ppm F    F is a screenshot that is not blank (several colours, not all one)
//   ui_smoke --pose PATCH NODE PORT X Y   opens PATCH, presses on that jack and holds a cable at
//                             window (X, Y), for a screenshot of a drag (GPUIC_SCREENSHOT,
//   ui_smoke --pose-module PATCH CARD X Y   the same with the browser's CARD-th card held at (X, Y)
//   ui_smoke --pose-panel PATCH NODE X Y   the same with a panel held by its face at (X, Y)
//   ui_smoke --pose-ask PATCH   opens PATCH, makes a change and asks New: the unsaved-changes
//   question
//                             GPUIC_WAIT_MS)
#include "engine/model/patchfile.h"
#include "engine/model/registry.h"
#include "ui/files.h"
#include "ui/geometry.h"
#include "ui/rack_ui.h"
#include "ui/style.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

using namespace rack;
using namespace rack::ui;
using namespace gpuic;

namespace
{
int checks = 0, failures = 0;
#define CHECK(c) check_((c), #c, __LINE__)
void check_(bool ok, const char *expr, int line)
{
    checks++;
    if (!ok)
    {
        failures++;
        std::printf("  FAIL ui_smoke.cpp:%d %s\n", line, expr);
    }
}
bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

int check_ppm(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    std::string   magic;
    int           w = 0, h = 0, maxv = 0;
    f >> magic >> w >> h >> maxv;
    f.get();
    if (magic != "P6" || w <= 0 || h <= 0)
    {
        std::printf("%s: not a PPM\n", path);
        return 1;
    }
    std::vector<unsigned char> px((size_t)w * h * 3);
    f.read((char *)px.data(), (std::streamsize)px.size());
    std::set<uint32_t> colours;
    for (size_t i = 0; i + 2 < px.size(); i += 3 * 7) // a sample of the pixels
        colours.insert((uint32_t)px[i] << 16 | (uint32_t)px[i + 1] << 8 | px[i + 2]);
    std::printf("%s: %d x %d, %zu colours in the sample\n", path, w, h, colours.size());
    return colours.size() >= 50 ? 0 : 1;
}

struct Driver
{
    Co            &co;
    App            app;
    Entity<Editor> ed;

    Driver(Co &c, App a, Entity<Editor> e)
        : co(c)
        , app(a)
        , ed(e)
    {
    }

    void wait(double s = 0.06) { co.sleep(s); }
    void move(gp_point p, uint32_t mods = 0)
    {
        gp_simulate_mouse(app, GP_SIM_MOVE, p.x, p.y, 0, mods);
    }
    void press(gp_point p, int button = GP_MOUSE_LEFT, int clicks = 1, uint32_t mods = 0)
    {
        move(p, mods);
        gp_simulate_mouse_button(app, GP_SIM_DOWN, p.x, p.y, button, clicks, mods);
        wait();
    }
    void release(gp_point p, int button = GP_MOUSE_LEFT, uint32_t mods = 0)
    {
        gp_simulate_mouse_button(app, GP_SIM_UP, p.x, p.y, button, 1, mods);
        wait();
    }
    void click(gp_point p, int button = GP_MOUSE_LEFT, int clicks = 1)
    {
        press(p, button, clicks);
        release(p, button);
    }
    // press at `a`, move to `b` in `steps` steps, release
    void drag(gp_point a, gp_point b, int steps = 10, uint32_t mods = 0)
    {
        press(a, GP_MOUSE_LEFT, 1, mods);
        for (int i = 1; i <= steps; i++)
        {
            move({ a.x + (b.x - a.x) * i / steps, a.y + (b.y - a.y) * i / steps }, mods);
            wait(0.02);
        }
        wait();
        release(b, GP_MOUSE_LEFT, mods);
    }
    Editor     &e() { return *app.get(ed); }
    Ctx         cx() { return app.ctx(ed); }
    const Node *node(uint32_t id) { return e().engine.patch().find(id); }
    float       param(uint32_t id, const char *name)
    {
        const Node *n = node(id);
        return n->params[(size_t)find_param(*n->desc, name)];
    }
    gp_point control(uint32_t id, const char *name)
    {
        return control_center(cx(), ed, id, find_param(*node(id)->desc, name));
    }
    // the n-th card's Add button in the browser (as the list shows it now)
    gp_point add_button(int card)
    {
        const float top = style::kToolbarH + style::kBrowserPad + style::kSearchH + style::kChipsH
            + card * (style::kCardH + style::kCardGap);
        return { style::kBrowserW - style::kBrowserPad - 8 - style::kAddW / 2, top + 8 + 14 };
    }
    uint32_t newest()
    {
        uint32_t id = 0;
        for (const Node &n : e().engine.patch().nodes())
            id = std::max(id, n.id);
        return id;
    }

    PortRef P(uint32_t n, const char *id)
    {
        return PortRef { n, (uint16_t)find_port(*node(n)->desc, id) };
    }
    gp_point     J(uint32_t n, const char *id) { return port_center(cx(), ed, n, P(n, id).port); }
    const Cable *into(uint32_t n, const char *id)
    {
        return e().engine.patch().cable_into(P(n, id));
    }
    bool  hint_has(const char *s) { return e().hint().find(s) != std::string::npos; }
    float peak(size_t frames = 4800) // the engine's output, rendered here (no audio device)
    {
        std::vector<float> l(frames), r(frames);
        e().engine.render(l.size(), l.data(), r.data());
        float m = 0;
        for (float v : l)
            m = std::max(m, std::fabs(v));
        return m;
    }
    // the window position of the middle of the cable into `to`
    gp_point cable_middle(PortRef to)
    {
        const Patch &p = e().engine.patch();
        const Cable *c = p.cable_into(to);
        const auto   q = cable_curve(jack_center(*p.find(c->from.node), c->from.port),
            jack_center(*p.find(c->to.node), c->to.port));
        const float  x = 0.125f * q[0].x + 0.375f * q[1].x + 0.375f * q[2].x + 0.125f * q[3].x;
        const float  y = 0.125f * q[0].y + 0.375f * q[1].y + 0.375f * q[2].y + 0.125f * q[3].y;
        const RackViewport vp = rack_viewport(cx(), ed);
        return { rack_origin().x + x - vp.x, rack_origin().y + y - vp.y };
    }

    void cables(uint32_t osc, uint32_t vca, uint32_t adsr)
    {
        const Patch   &patch = e().engine.patch();
        const uint32_t out = e().add_module(cx(), *find_module("out"));
        wait();
        CHECK(out != 0);
        CHECK(e().connect(cx(), P(vca, "out"), P(out, "l")) == Err::Ok);
        CHECK(peak() == 0); // nothing into the VCA yet

        // ---- a cable from an output to an input, with what the drag shows on the way
        const gp_point a = J(osc, "out");
        press(a);
        move({ a.x + 30, a.y + 40 });
        wait();
        CHECK(e().drag.active);
        auto lit = [&](PortRef q)
        {
            return std::find(e().drag.targets.begin(), e().drag.targets.end(), q)
                != e().drag.targets.end();
        };
        CHECK(lit(P(vca, "in")) && lit(P(adsr, "gate"))
            && lit(P(osc, "pitch")) == false); // its own input: a loop
        CHECK(!lit(P(adsr, "note")) && !lit(P(vca, "out"))
            && !lit(P(out, "l"))); // event, an output, in use
        move(J(adsr, "note"));
        wait();
        CHECK(e().drag.over && e().drag.verdict == Err::KindMismatch
            && hint_has("Cannot connect here"));
        move(J(vca, "in"));
        wait();
        CHECK(e().drag.over && e().drag.verdict == Err::Ok && hint_has("Release to connect"));
        release(J(vca, "in"));
        CHECK(!e().drag.active);
        CHECK(into(vca, "in") && into(vca, "in")->from == P(osc, "out"));
        CHECK(peak() > 0.05f); // the engine took the cable at once: osc -> VCA -> out
        CHECK(e().cable_views.size() == patch.cables().size());

        // ---- refused: an event input, a loop, an empty release
        drag(a, J(adsr, "note"));
        CHECK(!into(adsr, "note") && hint_has("Not connected"));
        drag(J(vca, "out"), J(osc, "pitch"));
        CHECK(!into(osc, "pitch") && hint_has("loop"));
        const size_t n_cables = patch.cables().size();
        drag(a, { a.x + 150, a.y + 160 });
        CHECK(patch.cables().size() == n_cables && hint_has("No cable"));

        // ---- lift a plug off an input and put it on another: the same cable, the same colour
        const int colour = e().cable_colour.at({ vca, P(vca, "in").port });
        drag(J(vca, "in"), J(adsr, "gate"));
        CHECK(!into(vca, "in") && into(adsr, "gate") && into(adsr, "gate")->from == P(osc, "out"));
        CHECK(e().cable_colour.at({ adsr, P(adsr, "gate").port }) == colour);
        CHECK(peak() == 0); // the VCA lost its input
        // ... and drop it on empty rack: removed
        drag(J(adsr, "gate"), { o().x + 900, o().y + 700 });
        CHECK(!into(adsr, "gate") && patch.cables().size() == n_cables - 1);

        // ---- from an empty input to an output works too
        drag(J(vca, "in"), a);
        CHECK(into(vca, "in") && into(vca, "in")->from == P(osc, "out"));

        // ---- select by clicking the cable; Delete removes it, but not while the search box has
        // the keys
        click(cable_middle(P(vca, "in")));
        CHECK(e().selected && *e().selected == P(vca, "in") && hint_has("Selected the cable"));
        click({ style::kBrowserPad + 40,
            style::kToolbarH + style::kBrowserPad + style::kSearchH / 2 });
        gp_simulate_keystrokes(app, "backspace");
        wait(0.1);
        CHECK(into(vca, "in") != nullptr); // the key went to the search box
        click(cable_middle(P(vca, "in")));
        gp_simulate_keystrokes(app, "delete");
        wait(0.1);
        CHECK(!into(vca, "in") && !e().selected);

        // ---- or from its menu
        CHECK(e().connect(cx(), P(osc, "out"), P(vca, "in")) == Err::Ok);
        wait();
        const gp_point m = cable_middle(P(vca, "in"));
        click(m, GP_MOUSE_RIGHT);
        click({ m.x + 4 + 90, m.y + 4 + 4 + 13 });
        CHECK(!into(vca, "in"));

        // ---- a cable follows its panel
        CHECK(e().connect(cx(), P(osc, "out"), P(vca, "in")) == Err::Ok);
        wait();
        const gp_point before = cable_middle(P(vca, "in"));
        // (by the left end of its title: the VCA OUT -> Audio Output cable crosses the middle, and
        // a cable over a panel takes the press, as designed)
        const gp_point title = control_center(cx(), ed, vca, -1);
        const gp_point grab { title.x - 2 * kHp + 4, title.y };
        drag(grab, { grab.x + 3 * kHp, grab.y });
        CHECK(node(vca)->x == 11);
        const gp_point after = cable_middle(P(vca, "in"));
        CHECK(after.x > before.x + 10);
        click(after); // the cable is drawn where the model says
        CHECK(e().selected && *e().selected == P(vca, "in"));
        click({ o().x + 900, o().y + 700 }); // and a click on empty rack lets go of it
        CHECK(!e().selected);
        CHECK(e().engine.apply_failures() == 0 && e().engine.nonfinite_samples() == 0);
    }
    gp_point o() { return rack_origin(); }

    // A panel moved by its face: it follows the pointer freely (over its neighbours, its cable with
    // it), the outline says where it would land, and the release puts it there or back.
    gp_point face(uint32_t n) // a point on the panel's face between its controls and its jacks
    {
        const Node        *nd = node(n);
        const PanelLayout &L = panel_layout(*nd->desc);
        const RackViewport v = rack_viewport(cx(), ed);
        return { o().x + nd->x * kHp + L.width / 2 - v.x,
            o().y + nd->y * kHp + (L.controls_bottom + L.jacks_top) / 2 - v.y };
    }
    void carry(uint32_t osc, uint32_t adsr)
    {
        const Patch &patch = e().engine.patch();
        CHECK(node(osc)->x == 0 && node(osc)->y == 0 && into(vca_of(), "in"));
        float free_x = 0; // the first free place right of everything on the first row
        for (const Node &n : patch.nodes())
            if (n.id != osc && n.y == 0)
                free_x = std::max(free_x, n.x + n.desc->width_hp);
        const gp_point f = face(osc), cable0 = cable_middle(P(vca_of(), "in"));
        click(f); // a click on the face does nothing
        CHECK(node(osc)->x == 0 && !e().pdrag.active);
        press(f);
        const float over_adsr = (node(adsr)->x + 1) * kHp;
        for (int i = 1; i <= 10; i++)
        {
            move({ f.x + over_adsr * i / 10, f.y });
            wait(0.02);
        }
        wait();
        CHECK(e().pdrag.moved && !e().pdrag.fits && hint_has("No room")); // red over the ADSR
        CHECK(node(osc)->x > node(adsr)->x - 1);                          // carried over it, freely
        CHECK(cable_middle(P(vca_of(), "in")).x > cable0.x + 50);         // its cable with it
        for (int i = 1; i <= 10; i++)
        {
            move({ f.x + over_adsr + (free_x * kHp - over_adsr) * i / 10, f.y });
            wait(0.02);
        }
        wait();
        CHECK(e().pdrag.fits && e().pdrag.x == (int)free_x && hint_has("Release to put"));
        release({ f.x + free_x * kHp, f.y });
        CHECK(node(osc)->x == free_x && node(osc)->y == 0 && !e().pdrag.active);
        // released on another panel: it goes back
        const gp_point f2 = face(osc);
        drag(f2, { f2.x - (free_x - node(adsr)->x) * kHp, f2.y }, 12);
        CHECK(node(osc)->x == free_x && hint_has("Not moved"));
        // a knob on the face still turns its knob, and the panel stays
        const float    fr = param(osc, "frequency");
        const gp_point k = control(osc, "frequency");
        drag(k, { k.x, k.y - 30 });
        CHECK(param(osc, "frequency") != fr && node(osc)->x == free_x);
        click(k, GP_MOUSE_LEFT, 2); // its default again
        // and back to where it started, for what follows (by its title this time)
        const gp_point t = control_center(cx(), ed, osc, -1);
        drag({ t.x - 20, t.y }, { t.x - 20 - free_x * kHp, t.y }, 12);
        CHECK(node(osc)->x == 0 && node(osc)->y == 0);
    }
    uint32_t vca_ = 0;

    // ---- the smaller pieces of milestone 6: selection and Delete, the arrow keys, a cable by its
    // output end, the rack scrolling under a drag held at its edge
    void gaps()
    {
        const Patch &patch = e().engine.patch();
        wait(0.3);
        const uint32_t osc = e().add_module(cx(), *find_module("osc"));
        const uint32_t vca = e().add_module(cx(), *find_module("vca"));
        const uint32_t out = e().add_module(cx(), *find_module("out"));
        const uint32_t lfo = e().add_module(cx(), *find_module("lfo"));
        wait();

        // a click on a panel selects it; Delete removes it; undo brings it back
        click(face(vca));
        CHECK(e().selected_node == vca && hint_has("Selected VCA"));
        gp_simulate_keystrokes(app, "delete");
        wait(0.1);
        CHECK(!node(vca) && e().selected_node == 0);
        click(undo_btn());
        CHECK(node(vca) != nullptr);
        // a cable selected unselects the module, and the other way round
        CHECK(e().connect(cx(), P(osc, "out"), P(vca, "in")) == Err::Ok);
        click(face(osc));
        CHECK(e().selected_node == osc);
        e().select_cable(cx(), P(vca, "in"));
        CHECK(e().selected_node == 0 && e().selected);
        click(face(osc));
        CHECK(e().selected_node == osc && !e().selected);
        click({ o().x + 900, o().y + 700 }); // empty rack: nothing selected
        CHECK(e().selected_node == 0);

        // the arrow keys turn the control under the pointer
        const size_t     fi = (size_t)find_param(*node(osc)->desc, "frequency");
        const ParamDesc &fd = node(osc)->desc->params[fi];
        const float      f0 = param(osc, "frequency");
        move(control(osc, "frequency"));
        wait(0.1);
        gp_simulate_keystrokes(app, "up");
        wait(0.1);
        CHECK(param(osc, "frequency") == step_value(fd, f0, 1, false));
        const float f1 = param(osc, "frequency");
        gp_simulate_keystrokes(app, "shift-up"); // fine
        wait(0.1);
        CHECK(param(osc, "frequency") == step_value(fd, f1, 1, true));
        move(control(osc, "waveform"));
        wait(0.1);
        gp_simulate_keystrokes(app, "left"); // a selector: the previous choice
        wait(0.1);
        CHECK(param(osc, "waveform") == 1);
        // away from every control: the last one touched
        move({ o().x + 900, o().y + 700 });
        wait(0.1);
        gp_simulate_keystrokes(app, "right");
        wait(0.1);
        CHECK(param(osc, "waveform") == 2);

        // Shift+drag on a plugged output moves its newest cable by the output end
        CHECK(e().connect(cx(), P(osc, "out"), P(out, "l")) == Err::Ok); // the newest from osc.out
        const int colour = e().cable_colour.at({ out, P(out, "l").port });
        drag(J(osc, "out"), J(lfo, "out"), 12, GP_MOD_SHIFT);
        CHECK(into(out, "l") && into(out, "l")->from == P(lfo, "out"));   // moved
        CHECK(into(vca, "in") && into(vca, "in")->from == P(osc, "out")); // the other one stayed
        CHECK(e().cable_colour.at({ out, P(out, "l").port }) == colour);
        CHECK(patch.cables().size() == 2);
        // and to empty rack: removed
        drag(J(lfo, "out"), { o().x + 900, o().y + 700 }, 12, GP_MOD_SHIFT);
        CHECK(!into(out, "l") && patch.cables().size() == 1);
        // without Shift an output still makes a new cable
        drag(J(osc, "out"), J(out, "r"));
        CHECK(into(out, "r") && into(vca, "in") && patch.cables().size() == 2);

        // a panel held at the rack's right edge: the rack scrolls, and the panel goes with it
        RackViewport v = rack_viewport(cx(), ed);
        CHECK(v.x == 0);
        const gp_point f = face(lfo);
        const float    lx0 = node(lfo)->x;
        const gp_point edge { o().x + v.w - 10, f.y };
        press(f);
        for (int i = 1; i <= 10; i++)
        {
            move({ f.x + (edge.x - f.x) * i / 10, f.y });
            wait(0.02);
        }
        wait(0.8); // held there
        v = rack_viewport(cx(), ed);
        CHECK(v.x > 100);
        CHECK(e().pdrag.moved && node(lfo)->x * kHp > (lx0 * kHp) + (edge.x - f.x) + 100);
        release(edge);
        CHECK(node(lfo)->x
            > lx0 + (edge.x - f.x) / kHp + 5); // put down further right than the pointer went
        // a cable held at the bottom edge scrolls the rack down
        const float    vy0 = rack_viewport(cx(), ed).y;
        const gp_point j = J(lfo, "rate"); // (in view: the oscillator has scrolled out of it)
        press(j);
        move({ j.x, o().y + v.h - 8 });
        wait(0.6);
        CHECK(rack_viewport(cx(), ed).y > vy0 + 50 && e().drag.active);
        release({ j.x, o().y + v.h - 8 });
        CHECK(!e().drag.active && !into(lfo, "rate"));
        CHECK(e().engine.apply_failures() == 0);
    }

    // ---- the sequencer's step grid
    gp_point in_panel(
        uint32_t n, const Rect &r) // the window position of a rectangle's centre in a panel
    {
        const Node        *nd = node(n);
        const RackViewport v = rack_viewport(cx(), ed);
        return { o().x + nd->x * kHp + kPanelBorder + r.x + r.w / 2 - v.x,
            o().y + nd->y * kHp + kPanelBorder + r.y + r.h / 2 - v.y };
    }
    StepGrid grid(uint32_t n) { return step_grid(panel_layout(*node(n)->desc)); }
    StepData step(uint32_t n, size_t i) { return node(n)->steps[i]; }
    void     steps()
    {
        wait(0.3);
        const uint32_t clk = e().add_module(cx(), *find_module("clock"));
        const uint32_t seq = e().add_module(cx(), *find_module("seq"));
        const uint32_t gate = e().add_module(cx(), *find_module("note_to_gate"));
        const uint32_t out = e().add_module(cx(), *find_module("out"));
        CHECK(e().connect(cx(), P(clk, "clock"), P(seq, "in")) == Err::Ok);
        CHECK(e().connect(cx(), P(seq, "out"), P(gate, "notes")) == Err::Ok);
        CHECK(e().connect(cx(), P(gate, "gate"), P(out, "l")) == Err::Ok);
        wait();
        CHECK(peak() == 0); // every step a rest
        const StepGrid g = grid(seq);

        // the on pad of step 1, and the engine hears it (the running sequencer got the step)
        size_t u = undos();
        click(in_panel(seq, g.on_pad(0)));
        CHECK(step(seq, 0).velocity == 100 && undos() == u + 1);
        CHECK(
            peak(57600) > 0.5f); // the gate, through the output (1.2 s: the loop of 8 steps is 1 s)
        // the tie pad; a double-click on the bar: on / rest
        click(in_panel(seq, g.tie_pad(0)));
        CHECK(step(seq, 0).tie == 1);
        click(in_panel(seq, g.note_bar(1)), GP_MOUSE_LEFT, 2);
        CHECK(step(seq, 1).velocity == 100);
        click(in_panel(seq, g.note_bar(1)), GP_MOUSE_LEFT, 2);
        CHECK(step(seq, 1).velocity == 0);

        // the note bar: 20 px up is 5 semitones, and one undo step however many moves made it
        const gp_point b = in_panel(seq, g.note_bar(2));
        u = undos();
        drag(b, { b.x, b.y - 20 }, 10);
        CHECK(step(seq, 2).note == 65 && undos() == u + 1);
        move(b);
        gp_simulate_wheel(app, b.x, b.y, 0, 1, 0); // a notch up: a semitone
        wait();
        CHECK(step(seq, 2).note == 66 && undos() == u + 1);   // the same step, the same undo step
        gp_simulate_wheel(app, b.x, b.y, 1, 0, GP_MOD_SHIFT); // Shift: an octave (down)
        wait();
        CHECK(step(seq, 2).note == 54);
        CHECK(hint_has("step 3: F#3"));
        click(undo_btn());
        CHECK(step(seq, 2).note == 60); // all of step 3's changes, in one undo
        // the pages: the second page's first column is step 17
        click(in_panel(seq, g.tab(1)));
        click(in_panel(seq, g.on_pad(0)));
        CHECK(step(seq, 16).velocity == 100 && step(seq, 0).velocity == 100);
        click(undo_btn());
        CHECK(step(seq, 16).velocity == 0);
        CHECK(e().engine.apply_failures() == 0);

        // ---- the Keyboard panel's piano: a held key is a held note (heard through Note to Gate)
        const uint32_t kb = e().add_module(cx(), *find_module("keyboard"));
        const uint32_t g2 = e().add_module(cx(), *find_module("note_to_gate"));
        CHECK(e().connect(cx(), P(kb, "notes"), P(g2, "notes")) == Err::Ok);
        CHECK(e().connect(cx(), P(g2, "gate"), P(out, "r")) == Err::Ok);
        e().disconnect(cx(), P(out, "l")); // (the sequencer's gate: only the keyboard is heard now)
        wait();
        CHECK(peak() == 0);
        const KeyStrip k = keyboard_strip(panel_layout(*node(kb)->desc));
        auto           key_at = [&](int s)
        {
            const Rect r = k.key(s);
            return in_panel(kb,
                k.is_black(s) ? r : Rect { r.right() - 8, r.y, 6, r.h }); // right of the black keys
        };
        move(key_at(0)); // over C4 first, as a hand would be (the hover comes before the press)
        wait();
        press(key_at(0));
        CHECK(hint_has("Note C4") && peak() > 0.5f);
        release(key_at(0));
        CHECK(peak() == 0);
        // a glide from C4 up to E4: every key on the way, each released before the next
        press(key_at(0));
        for (int s = 1; s <= 4; s++)
        {
            move(key_at(s));
            wait(0.03);
        }
        wait();
        CHECK(hint_has("Note E4") && peak() > 0.5f);
        release(key_at(4));
        CHECK(peak() == 0); // nothing left held
        // the computer keyboard's notes light the piano too (and play this module)
        click({ o().x + 900, o().y + 700 });
        gp_simulate_key(app, "a", true);
        wait(0.1);
        CHECK(e().held.count("a") && peak() > 0.5f);
        gp_simulate_key(app, "a", false);
        wait(0.1);
        CHECK(peak() == 0);
    }

    // ---- undo: the toolbar's buttons and Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y
    size_t   undos() { return e().undo_stack.size(); }
    gp_point undo_btn() { return btn(style::kUndoX, style::kUndoW); }
    gp_point redo_btn() { return btn(style::kRedoX, style::kRedoW); }
    void     undo_redo()
    {
        const Patch &patch = e().engine.patch();
        wait(0.3);
        click(undo_btn());
        CHECK(hint_has("Nothing to undo") && undos() == 0);

        // a module from the browser, another, and a cable dragged between them: three steps
        click(add_button(0)); // the oscillator
        const uint32_t osc = newest();
        const uint32_t out = e().add_module(cx(), *find_module("out"));
        wait();
        drag(J(osc, "out"), J(out, "l"));
        CHECK(into(out, "l") && undos() == 3 && e().dirty);
        CHECK(peak() > 0.1f);

        // a knob dragged (many changes) is one step; the wheel on the same knob joins it; another
        // knob is a step of its own
        const size_t     fi = (size_t)find_param(*node(osc)->desc, "frequency");
        const ParamDesc &fd = node(osc)->desc->params[fi];
        const float      f0 = param(osc, "frequency");
        const gp_point   k = control(osc, "frequency");
        drag(k, { k.x, k.y - 60 }, 12);
        CHECK(undos() == 4 && param(osc, "frequency") != f0);
        move(k);
        gp_simulate_wheel(app, k.x, k.y, 0, 1, 0);
        gp_simulate_wheel(app, k.x, k.y, 0, 1, 0);
        wait();
        CHECK(undos() == 4);
        e().set_param(cx(), osc, (size_t)find_param(*node(osc)->desc, "pulse_width"), 0.3f);
        CHECK(undos() == 5);
        // one undo takes the pulse width back, the next the whole of the frequency's changes
        gp_simulate_keystrokes(app, "ctrl-z");
        wait(0.1);
        CHECK(param(osc, "pulse_width")
            == node(osc)->desc->params[(size_t)find_param(*node(osc)->desc, "pulse_width")].def);
        click(undo_btn());
        CHECK(param(osc, "frequency") == f0 && hint_has("Undone: Oscillator Frequency"));
        (void)fd;

        // undo reaches the running module: the output's level to 0 is silence, undone is sound
        // again
        e().set_param(cx(), out, (size_t)find_param(*node(out)->desc, "level"), 0);
        CHECK(peak() == 0);
        gp_simulate_keystrokes(app, "ctrl-z");
        wait(0.1);
        CHECK(param(out, "level") > 0 && peak() > 0.1f);

        // a move by the face, undone: back where it was
        const float    x0 = node(osc)->x;
        const gp_point f = face(osc);
        drag(f, { f.x, f.y + kPanelHeightHp * kHp }, 12);
        CHECK(node(osc)->y == kPanelHeightHp);
        click(undo_btn());
        CHECK(node(osc)->x == x0 && node(osc)->y == 0);

        // a module deleted with its cable, undone: back, cable and sound included
        const size_t before = undos();
        e().remove_module(cx(), osc);
        CHECK(!node(osc) && !into(out, "l") && peak() == 0 && undos() == before + 1);
        click(undo_btn());
        CHECK(
            node(osc) && into(out, "l") && into(out, "l")->from == P(osc, "out") && peak() > 0.1f);
        CHECK(e().panels.count(osc) && e().cable_views.size() == 1);
        // ... and redone
        click(redo_btn());
        CHECK(!node(osc) && patch.cables().empty());
        gp_simulate_keystrokes(app, "ctrl-z"); // (back)
        wait(0.1);
        CHECK(node(osc) != nullptr);

        // all the way back: the empty patch, as it was opened, so not unsaved; then all forward
        while (!e().undo_stack.empty())
            click(undo_btn());
        CHECK(patch.nodes().empty() && !e().dirty);
        // the redo list holds the two modules, the cable, and (on top of them) the deletion that
        // was redone and undone again above
        CHECK(e().redo_stack.size() == 4);
        for (int i = 0; i < 3; i++)
        {
            gp_simulate_keystrokes(app, i % 2 ? "ctrl-y" : "ctrl-shift-z");
            wait(0.1);
        }
        CHECK(
            patch.nodes().size() == 2 && into(out, "l") && e().redo_stack.size() == 1 && e().dirty);
        CHECK(peak() > 0.1f);
        // a new edit after an undo drops what could be redone
        e().add_module(cx(), *find_module("noise"));
        CHECK(e().redo_stack.empty());

        // the saved version: undo after a save is unsaved again, redo is saved again
        CHECK(e().save_to(
                  cx(), std::filesystem::temp_directory_path().string() + "/pulserack_undo.rack")
            && !e().dirty);
        click(undo_btn());
        CHECK(e().dirty);
        click(redo_btn());
        CHECK(!e().dirty);
        std::filesystem::remove(std::filesystem::temp_directory_path() / "pulserack_undo.rack");

        // Ctrl+Z in the search box is the search box's
        const size_t n = patch.nodes().size();
        click({ style::kBrowserPad + 40,
            style::kToolbarH + style::kBrowserPad + style::kSearchH / 2 });
        gp_simulate_text(app, "x");
        gp_simulate_keystrokes(app, "ctrl-z");
        wait(0.1);
        CHECK(patch.nodes().size() == n);
        click({ o().x + 900, o().y + 700 });
        // opening a patch starts a new history
        e().perform(cx(), Editor::Then::Factory, factory_index("sequence"));
        CHECK(e().undo_stack.empty() && e().redo_stack.empty());
        CHECK(e().engine.apply_failures() == 0 && e().engine.nonfinite_samples() == 0);
    }

    // ---- files: the toolbar's buttons, the Open menu, the unsaved-changes question, recovery. The
    // chooser is this test's: it answers with `next` (a path, or nothing for a cancelled dialog).
    std::optional<std::string> next;
    int                        asked_open = 0, asked_save = 0;
    gp_point                   btn(float x, float w) { return { x + w / 2, style::kToolbarH / 2 }; }
    // the Open menu's rows: 0 "Open a file", then Recover if shown, then the examples
    gp_point menu_row(int row, bool recover_shown)
    {
        float       y = style::kToolbarH - 2 + 1 + 4;
        const float x = style::kOpenX + 1 + 4 + 60;
        if (row == 0)
            return { x, y + style::kFileItemH / 2 };
        y += style::kFileItemH;
        if (recover_shown)
        {
            if (row == 1)
                return { x, y + style::kFileExampleH / 2 };
            y += style::kFileExampleH;
            row--;
        }
        y += style::kFileHeaderH;
        return { x, y + (row - 1) * style::kFileExampleH + style::kFileExampleH / 2 };
    }
    gp_point ask_button(int which) // 0 Save, 1 Don't save, 2 Cancel
    {
        const float l = (style::kWindowW - style::kAskW) / 2,
                    t = (style::kWindowH - style::kAskH) / 2;
        const float y = t + style::kAskH - 1 - style::kAskPad - style::kAskBtnH / 2;
        const float save_x = l + style::kAskW - 1 - style::kAskPad - style::kAskBtnW / 2;
        return { save_x - which * (style::kAskBtnW + style::kAskGap), y };
    }
    int factory_index(const char *name)
    {
        const auto f = factory_patches();
        for (size_t i = 0; i < f.size(); i++)
            if (f[i].name == name)
                return (int)i;
        return -1;
    }
    void files(const std::string &dir)
    {
        namespace fs = std::filesystem;
        e().chooser = [this](Ctx c, bool save, const std::string &)
        {
            (save ? asked_save : asked_open)++;
            e().file_chosen(c, save, next);
        };
        const Patch &patch = e().engine.patch();
        wait(0.3);
        CHECK(e().patch_name == "untitled" && !e().dirty && e().file_path.empty());

        // an edit makes the patch unsaved; Ctrl+S on an untitled patch asks where (".rack" added)
        const uint32_t osc = e().add_module(cx(), *find_module("osc"));
        CHECK(e().dirty);
        next = dir + "/one";
        gp_simulate_keystrokes(app, "ctrl-s");
        wait(0.15);
        CHECK(asked_save == 1 && !e().dirty && e().file_path == dir + "/one.rack"
            && e().patch_name == "one.rack");
        std::string text, err;
        CHECK(read_file(dir + "/one.rack", &text, &err) && text == save_patch(patch));
        // a knob turned, then the Save button: the same file, without asking
        click(control(osc, "frequency"), GP_MOUSE_LEFT, 2); // (its default: no change)
        e().set_param(cx(), osc, (size_t)find_param(*node(osc)->desc, "frequency"), 440);
        CHECK(e().dirty);
        click(btn(style::kSaveX, style::kSaveW));
        CHECK(asked_save == 1 && !e().dirty && read_file(dir + "/one.rack", &text, &err)
            && text.find("param " + std::to_string(osc) + " frequency 440") != std::string::npos);

        // the Open menu: an example, opened at once (nothing unsaved), untitled
        click(btn(style::kOpenX, style::kOpenW));
        CHECK(e().file_menu_open);
        click(menu_row(1 + factory_index("sequence"), false));
        CHECK(
            !e().file_menu_open && e().patch_name == "Sequence (example)" && e().file_path.empty());
        CHECK(patch.nodes().size() == 8 && patch.cables().size() == 8 && !e().dirty);
        CHECK(e().cable_views.size() == 8 && e().panels.size() == 8);

        // unsaved changes, then Open a file: the question; Cancel keeps everything
        e().set_param(cx(), patch.nodes()[0].id, 0, patch.nodes()[0].desc->params[0].max);
        CHECK(e().dirty);
        next = dir + "/one.rack";
        click(btn(style::kOpenX, style::kOpenW));
        click(menu_row(0, false));
        CHECK(e().ask.shown && hint_has("Unsaved changes"));
        click(ask_button(2));
        CHECK(!e().ask.shown && asked_open == 0 && patch.nodes().size() == 8 && e().dirty);
        // ... and "Don't save" goes on: the chooser, then the file
        click(btn(style::kOpenX, style::kOpenW));
        click(menu_row(0, false));
        click(ask_button(1));
        CHECK(asked_open == 1 && e().patch_name == "one.rack" && patch.nodes().size() == 1
            && !e().dirty);
        CHECK(patch.nodes()[0].params[(size_t)find_param(*patch.nodes()[0].desc, "frequency")]
            == 440);

        // a change, then Ctrl+N: "Save" writes the file first, then a new patch
        e().set_param(cx(), patch.nodes()[0].id,
            (size_t)find_param(*patch.nodes()[0].desc, "frequency"), 880);
        gp_simulate_keystrokes(app, "ctrl-n");
        wait(0.15);
        CHECK(e().ask.shown);
        click(ask_button(0));
        CHECK(patch.nodes().empty() && e().patch_name == "untitled" && !e().dirty
            && e().file_path.empty());
        CHECK(read_file(dir + "/one.rack", &text, &err)
            && text.find("frequency 880") != std::string::npos);
        // an untitled patch with changes, then New and "Save": where to, then new
        e().add_module(cx(), *find_module("vca"));
        next = dir + "/two.rack";
        click(btn(style::kNewX, style::kNewW));
        click(ask_button(0));
        CHECK(asked_save == 2 && patch.nodes().empty() && fs::exists(dir + "/two.rack"));
        // Save as on a cancelled dialog: nothing changes
        e().add_module(cx(), *find_module("noise"));
        next.reset();
        click(btn(style::kSaveAsX, style::kSaveAsW));
        CHECK(asked_save == 3 && e().dirty && hint_has("Cancelled"));

        // what would be lost at close is kept, and offered back in the menu
        CHECK(!e().has_recovery());
        e().write_recovery(); // (what closing the window does: checked for real at the end)
        CHECK(e().has_recovery());
        e().new_patch(cx()); // (without asking)
        click(btn(style::kOpenX, style::kOpenW));
        click(menu_row(1, true)); // Recover
        CHECK(patch.nodes().size() == 1 && patch.nodes()[0].desc->id == "noise" && e().dirty);
        CHECK(!e().has_recovery() && e().patch_name == "recovered patch" && e().file_path.empty());

        // a damaged file does not open, and changes nothing
        CHECK(write_file_atomic(dir + "/bad.rack", "pulserack 1\nnode one two\n", &err));
        next = dir + "/bad.rack";
        e().perform(cx(), Editor::Then::OpenDialog, -1);
        CHECK(hint_has("Not opened") && patch.nodes().size() == 1 && e().dirty);
        // a save that fails says so, and the patch stays unsaved
        CHECK(
            !e().save_to(cx(), dir + "/no/such/dir/x.rack") && e().dirty && hint_has("Not saved"));
        // Escape closes the menu
        click(btn(style::kOpenX, style::kOpenW));
        CHECK(e().file_menu_open);
        gp_simulate_keystrokes(app, "escape");
        wait(0.1);
        CHECK(!e().file_menu_open);
        // left with unsaved changes: closing the window keeps them (checked after it closes)
    }
    uint32_t vca_of() { return vca_; }

    // With the sound card on (SDL's dummy driver in the tests): the engine runs on the audio
    // thread.
    int  led(uint32_t n) { return e().leds.count(n) ? e().leds.at(n).level : 0; }
    void key(const char *k, bool down)
    {
        gp_simulate_key(app, k, down);
        wait(0.25); // a few blocks and a few meter ticks
    }
    void sound()
    {
        wait(0.5);
        CHECK(e().audio_running() && e().engine.audio_attached());
        CHECK(e().audio_status().find("dummy") != std::string::npos);
        CHECK(e().engine.sample_rate() == e().audio->info().rate && e().audio->info().frames > 0);
        const uint64_t t0 = e().engine.time();
        wait(0.4);
        CHECK(e().engine.time() > t0 + e().engine.sample_rate() / 10); // the callback is rendering

        // an oscillator into the output: its LED and the meters light (telemetry at 30 Hz)
        const uint32_t osc = e().add_module(cx(), *find_module("osc"));
        const uint32_t out = e().add_module(cx(), *find_module("out"));
        CHECK(e().connect(cx(), P(osc, "out"), P(out, "l")) == Err::Ok);
        wait(0.4);
        CHECK(led(osc) > 0 && e().meter[0] > 20 && e().meter[1] > 20);
        CHECK(led(out) == 0); // no outputs of its own
        // a knob turned while the audio thread runs: the output's level to 0, and the meters fall
        e().set_param(cx(), out, (size_t)find_param(*node(out)->desc, "level"), 0);
        wait(0.4);
        CHECK(e().meter[0] == 0 && e().meter[1] == 0);

        // the computer keyboard as a piano: a Keyboard module -> Note to Gate, whose LED is the
        // gate
        const uint32_t kb = e().add_module(cx(), *find_module("keyboard"));
        const uint32_t gate = e().add_module(cx(), *find_module("note_to_gate"));
        const uint32_t vca = e().add_module(cx(), *find_module("vca"));
        CHECK(e().connect(cx(), P(kb, "notes"), P(gate, "notes")) == Err::Ok);
        CHECK(e().connect(cx(), P(gate, "gate"), P(vca, "gain")) == Err::Ok); // connected: measured
        wait(0.3);
        CHECK(led(gate) == 0);
        key("a", true);
        CHECK(led(gate) > 0 && e().held.count("a") && e().held.at("a") == 60);
        CHECK(hint_has("Note C4"));
        key("a", false);
        CHECK(led(gate) == 0 && e().held.empty());
        key("x", true); // an octave up
        key("x", false);
        key("k", true); // the C above the A key's
        CHECK(e().held.count("k") && e().held.at("k") == 84 && led(gate) > 0);
        key("k", false);
        // not while the search box has the keyboard
        click({ style::kBrowserPad + 40,
            style::kToolbarH + style::kBrowserPad + style::kSearchH / 2 });
        key("a", true);
        CHECK(e().held.empty() && led(gate) == 0);
        key("a", false);
        click({ o().x + 900, o().y + 700 }); // the rack takes the keyboard back

        // a sequencer's playhead moves with the music (telemetry at 30 Hz)
        const uint32_t clk = e().add_module(cx(), *find_module("clock"));
        const uint32_t seq = e().add_module(cx(), *find_module("seq"));
        CHECK(e().connect(cx(), P(clk, "clock"), P(seq, "in")) == Err::Ok);
        e().set_param(cx(), clk, (size_t)find_param(*node(clk)->desc, "bpm"), 300);
        std::set<int> seen;
        for (int i = 0; i < 20; i++)
        {
            wait(0.1);
            if (e().playhead.count(seq))
                seen.insert(e().playhead.at(seq));
        }
        CHECK(seen.size() >= 3); // several steps went by
        for (int pos : seen)
            CHECK(pos >= -1 && pos < 8); // within the default length

        // the toolbar's Audio button: off (the engine is detached, edits apply at once) and on
        // again
        const gp_point button { style::kWindowW - 12 - 33, style::kToolbarH / 2 };
        click(button);
        CHECK(!e().audio_running() && !e().engine.audio_attached() && hint_has("Audio off"));
        CHECK(led(osc) == 0 && e().meter[0] == 0);
        click(button);
        CHECK(e().audio_running() && e().engine.audio_attached());
        CHECK(e().engine.apply_failures() == 0 && e().engine.nonfinite_samples() == 0);
        CHECK(e().engine.telemetry().commands_lost.load() == 0);
    }

    void run()
    {
        wait(0.4); // the first layout
        const Patch &patch = e().engine.patch();
        CHECK(patch.nodes().empty());

        // ---- the browser: Add on the first card (the oscillator), then search and add the VCA
        click(add_button(0));
        CHECK(patch.nodes().size() == 1);
        const uint32_t osc = newest();
        CHECK(node(osc) && node(osc)->desc->id == "osc" && node(osc)->x == 0 && node(osc)->y == 0);
        CHECK(e().engine.state(osc) != nullptr); // the engine made the module

        click({ style::kBrowserPad + 40,
            style::kToolbarH + style::kBrowserPad + style::kSearchH / 2 });
        gp_simulate_text(app, "vca");
        wait(0.15);
        click(add_button(0));
        const uint32_t vca = newest();
        CHECK(patch.nodes().size() == 2 && node(vca)->desc->id == "vca");
        CHECK(node(vca)->x == 8 && node(vca)->y == 0); // next to the oscillator

        const uint32_t adsr = e().add_module(cx(), *find_module("adsr"));
        wait();
        CHECK(adsr && node(adsr)->x == 12);

        // ---- a knob: drag up 100 px is half the travel; Shift is a tenth of that
        const ParamDesc &freq
            = node(osc)->desc->params[(size_t)find_param(*node(osc)->desc, "frequency")];
        const float t0 = to_travel(freq, param(osc, "frequency"));
        gp_point    k = control(osc, "frequency");
        drag(k, { k.x, k.y - 100 });
        CHECK(near(to_travel(freq, param(osc, "frequency")), t0 + 0.5f, 1e-3f));
        const float t1 = to_travel(freq, param(osc, "frequency"));
        // Shift pressed during the drag: fine from that move on; released: full speed again
        press(k);
        for (int i = 1; i <= 10; i++) // 100 px down with Shift: a tenth of half the travel
        {
            move({ k.x, k.y + 10.f * i }, GP_MOD_SHIFT);
            wait(0.02);
        }
        wait();
        CHECK(near(to_travel(freq, param(osc, "frequency")), t1 - 0.05f, 1e-3f));
        for (int i = 1; i <= 4; i++) // then 40 px further down without it: a fifth of the travel
        {
            move({ k.x, k.y + 100 + 10.f * i });
            wait(0.02);
        }
        wait();
        release({ k.x, k.y + 140 });
        CHECK(near(to_travel(freq, param(osc, "frequency")), t1 - 0.05f - 0.2f, 1e-3f));
        // the engine's model is what the knob changed
        CHECK(
            e().engine.patch().find(osc)->params[(size_t)find_param(*node(osc)->desc, "frequency")]
            == param(osc, "frequency"));

        // double-click: the default; a wheel notch up: 1 % of the travel
        click(k, GP_MOUSE_LEFT, 2);
        CHECK(param(osc, "frequency") == freq.def);
        move(k);
        gp_simulate_wheel(app, k.x, k.y, 0, 1, 0);
        wait();
        CHECK(param(osc, "frequency") == step_value(freq, freq.def, 1, false));

        // ---- the hint line follows the pointer: a knob, a jack, a title, then empty rack
        move(control(osc, "pitch_depth"));
        wait(0.1);
        CHECK(e().hint().find("Pitch depth") != std::string::npos);
        move(port_center(cx(), ed, osc, (size_t)find_port(*node(osc)->desc, "pwm")));
        wait(0.1);
        CHECK(e().hint().find("PWM: signal input") != std::string::npos);
        move(control_center(cx(), ed, osc, -1));
        wait(0.1);
        CHECK(e().hint().find(std::string(node(osc)->desc->description)) != std::string::npos);
        move({ rack_origin().x + 900, rack_origin().y + 600 });
        wait(0.1);
        CHECK(e().hover_text.empty() && e().hint() == e().status);
        // the wheel over a knob under the pointer: the hint shows the new value
        k = control(osc, "frequency");
        move(k);
        wait(0.1);
        gp_simulate_wheel(app, k.x, k.y, 0, -1, 0);
        wait();
        CHECK(e().hint().find(format_value(freq, param(osc, "frequency"))) != std::string::npos);
        click(k, GP_MOUSE_LEFT, 2); // back to the default for what follows

        // ---- a selector: the right half is the next choice, the left half the previous
        gp_point sel = control(osc, "waveform");
        CHECK(param(osc, "waveform") == 2); // saw
        click({ sel.x + 30, sel.y + 8 });
        CHECK(param(osc, "waveform") == 3);
        click({ sel.x - 30, sel.y + 8 });
        click({ sel.x - 30, sel.y + 8 });
        CHECK(param(osc, "waveform") == 1);

        // ---- a switch and a slider
        click(control(adsr, "run_through"));
        CHECK(param(adsr, "run_through") == 1);
        click(control(adsr, "run_through"));
        CHECK(param(adsr, "run_through") == 0);
        const float sus0 = param(adsr, "sustain");
        gp_point    s = control(adsr, "sustain");
        drag(s, { s.x, s.y + 40 });
        CHECK(near(param(adsr, "sustain"), sus0 - 0.2f, 1e-3f));

        // ---- moving panels: onto a neighbour is refused, into the next row is fine
        gp_point title = control_center(cx(), ed, vca, -1);
        drag(title, { title.x + 5 * kHp, title.y });
        CHECK(node(vca)->x == 8 && node(vca)->y == 0);
        drag(title, { title.x, title.y + kPanelHeightHp * kHp });
        CHECK(node(vca)->x == 8 && node(vca)->y == kPanelHeightHp);

        const gp_point o = rack_origin();
        vca_ = vca;
        cables(osc, vca, adsr);
        carry(osc, adsr);

        // ---- panning: drag empty space
        drag({ o.x + 900, o.y + 300 }, { o.x + 700, o.y + 250 });
        RackViewport vp = rack_viewport(cx(), ed);
        CHECK(near(vp.x, 200, 1) && near(vp.y, 50, 1));
        drag({ o.x + 700, o.y + 250 }, { o.x + 900, o.y + 300 });
        vp = rack_viewport(cx(), ed);
        CHECK(near(vp.x, 0, 1) && near(vp.y, 0, 1));

        // ---- the panel's menu: right-click the title, then Delete
        title = control_center(cx(), ed, osc, -1);
        click(title, GP_MOUSE_RIGHT);
        const Node *n = node(osc);
        CHECK(node(osc) != nullptr);
        const gp_point item { o.x + n->x * kHp + 6 + 90,
            o.y + n->y * kHp + kTitleHeight + 2 + 4 + 13 };
        click(item);
        CHECK(node(osc) == nullptr && e().engine.state(osc) == nullptr);
        CHECK(patch.nodes().size() == 3);     // the VCA, the ADSR and the audio output
        for (const Cable &c : patch.cables()) // its cables went with it
            CHECK(c.from.node != osc && c.to.node != osc);
        CHECK(e().cable_views.size() == patch.cables().size());
        CHECK(e().engine.apply_failures() == 0);

        // ---- the rest of the catalogue: every module can be added and shows a panel
        for (const ModuleDesc *d : all_modules())
            CHECK(e().add_module(cx(), *d) != 0);
        wait(0.2);
        CHECK(patch.nodes().size() == 3 + all_modules().size());
        CHECK(e().panels.size() == patch.nodes().size());
        for (const Node &a : patch.nodes())
            CHECK(!collides(patch, *a.desc, (int)a.x, (int)a.y, a.id));

        // ---- drag a module from the browser onto the rack
        click({ style::kBrowserPad + 40,
            style::kToolbarH + style::kBrowserPad + style::kSearchH / 2 });
        gp_simulate_keystrokes(
            app, "ctrl-a backspace"); // every card again: the first is the oscillator
        wait(0.15);
        // the two visible rows are full by now: scroll down to the empty third (the wheel over a
        // title)
        const gp_point over_title = control_center(cx(), ed, newest(), -1);
        move(over_title);
        gp_simulate_wheel(app, over_title.x, over_title.y, 0, -20, 0);
        wait(0.15);
        const ModuleDesc  &oscd = *find_module("osc");
        const RackViewport v = rack_viewport(cx(), ed);
        const Slot         spot = free_slot(patch, oscd, (int)std::ceil(v.x / kHp),
            (int)std::lround(v.y / kHp), (int)(v.w / kHp), std::max(1, (int)(v.h / kPanelHeight)));
        const gp_point     to { rack_origin().x + spot.x * kHp + oscd.width_hp * kHp / 2 - v.x,
            rack_origin().y + spot.y * kHp + kTitleHeight / 2 - v.y };
        const gp_point     card { style::kBrowserPad + 40,
            add_button(0).y + 32 }; // the card's text, not its button
        size_t             count = patch.nodes().size();
        click(card); // a click on a card adds nothing (its Add button does)
        CHECK(patch.nodes().size() == count && !e().mdrag.active);
        press(card);
        for (int i = 1; i <= 12; i++)
        {
            move({ card.x + (to.x - card.x) * i / 12, card.y + (to.y - card.y) * i / 12 });
            wait(0.02);
        }
        wait();
        CHECK(e().mdrag.moved && e().mdrag.over_rack && e().mdrag.fits
            && hint_has("Release to add Oscillator"));
        CHECK(e().mdrag.x == spot.x && e().mdrag.y == spot.y);
        release(to);
        CHECK(patch.nodes().size() == count + 1 && !e().mdrag.active);
        const uint32_t dropped = newest();
        CHECK(node(dropped)->desc == &oscd && node(dropped)->x == spot.x
            && node(dropped)->y == spot.y);
        CHECK(e().engine.state(dropped) != nullptr && e().panels.count(dropped));
        // onto a panel: refused, with the reason
        drag(card, control_center(cx(), ed, dropped, -1), 12);
        CHECK(patch.nodes().size() == count + 1 && hint_has("no room"));
        // off the rack (onto the toolbar): nothing
        drag(card, { 600, style::kToolbarH / 2 }, 12);
        CHECK(patch.nodes().size() == count + 1 && hint_has("drop a module on the rack"));
    }
};
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "--check-ppm"))
        return check_ppm(argv[2]);
    if (argc == 3 && !std::strcmp(argv[1], "--pose-ask"))
    {
        static Options opt;
        opt.patch_path = argv[2];
        opt.audio = false;
        return gpuic::run(
            [](App app)
            {
                std::string error;
                auto        ed = open_rack(app, opt, &error);
                app.ctx(ed).spawn(
                    [app, ed](Co &co)
                    {
                        co.sleep(0.2);
                        Editor     *e = app.get(ed);
                        const Node &n = e->engine.patch().nodes()[0];
                        e->set_param(app.ctx(ed), n.id, 0, n.desc->params[0].max);
                        e->request(app.ctx(ed), Editor::Then::New);
                    });
            });
    }
    if (argc == 6 && !std::strcmp(argv[1], "--pose-panel"))
    {
        static Options  opt;
        static uint32_t which;
        static gp_point to;
        opt.patch_path = argv[2];
        opt.audio = false;
        which = (uint32_t)std::atoi(argv[3]);
        to = { (float)std::atof(argv[4]), (float)std::atof(argv[5]) };
        setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
        return gpuic::run(
            [](App app)
            {
                std::string error;
                auto        ed = open_rack(app, opt, &error);
                app.ctx(ed).spawn(
                    [app, ed](Co &co)
                    {
                        co.sleep(0.2);
                        Driver         d { co, app, ed };
                        const gp_point a = d.face(which);
                        d.press(a);
                        for (int i = 1; i <= 8; i++)
                        {
                            d.move({ a.x + (to.x - a.x) * i / 8, a.y + (to.y - a.y) * i / 8 });
                            d.wait(0.02);
                        }
                    });
            });
    }
    if (argc == 6 && !std::strcmp(argv[1], "--pose-module"))
    {
        static Options  opt;
        static int      card;
        static gp_point to;
        opt.patch_path = argv[2];
        opt.audio = false;
        card = std::atoi(argv[3]);
        to = { (float)std::atof(argv[4]), (float)std::atof(argv[5]) };
        setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
        return gpuic::run(
            [](App app)
            {
                std::string error;
                auto        ed = open_rack(app, opt, &error);
                app.ctx(ed).spawn(
                    [app, ed](Co &co)
                    {
                        co.sleep(0.2);
                        Driver         d { co, app, ed };
                        const gp_point a { style::kBrowserPad + 40, d.add_button(card).y + 32 };
                        d.press(a);
                        for (int i = 1; i <= 8; i++)
                        {
                            d.move({ a.x + (to.x - a.x) * i / 8, a.y + (to.y - a.y) * i / 8 });
                            d.wait(0.02);
                        }
                    });
            });
    }
    if (argc == 7 && !std::strcmp(argv[1], "--pose"))
    {
        static Options  opt;
        static PortRef  jack;
        static gp_point to;
        opt.patch_path = argv[2];
        opt.audio = false;
        jack = PortRef { (uint32_t)std::atoi(argv[3]), (uint16_t)std::atoi(argv[4]) };
        to = { (float)std::atof(argv[5]), (float)std::atof(argv[6]) };
        setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1);
        return gpuic::run(
            [](App app)
            {
                std::string error;
                auto        ed = open_rack(app, opt, &error);
                app.ctx(ed).spawn(
                    [app, ed](Co &co)
                    {
                        co.sleep(0.2);
                        Driver         d { co, app, ed };
                        const gp_point a = port_center(d.cx(), ed, jack.node, jack.port);
                        d.press(a);
                        for (int i = 1; i <= 8; i++)
                        {
                            d.move({ a.x + (to.x - a.x) * i / 8, a.y + (to.y - a.y) * i / 8 });
                            d.wait(0.02);
                        }
                    });
            });
    }
    static bool sound = false, files = false, undo = false, steps = false, gaps = false;
    sound = argc == 2 && !std::strcmp(argv[1], "--sound");
    files = argc == 2 && !std::strcmp(argv[1], "--files");
    undo = argc == 2 && !std::strcmp(argv[1], "--undo");
    steps = argc == 2 && !std::strcmp(argv[1], "--steps");
    gaps = argc == 2 && !std::strcmp(argv[1], "--gaps");
    static std::string dir;
    if (files)
    {
        namespace fs = std::filesystem;
        dir = (fs::temp_directory_path() / "pulserack_ui_files").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    if (sound)
        setenv("SDL_AUDIO_DRIVER", "dummy", 1); // never the real sound card in a test
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
    {
        std::printf("no display: skipped\n");
        return 77;
    }
    setenv("GPUIC_IGNORE_REAL_MOUSE", "1", 1); // only the synthetic pointer
    gpuic::run(
        [](App app)
        {
            std::string error;
            Options     opt;
            opt.audio = sound; // otherwise the test renders the engine itself
            if (files)
                opt.state_dir = dir + "/state";
            auto ed = open_rack(app, opt, &error);
            CHECK((bool)ed);
            app.ctx(ed).spawn(
                [app, ed](Co &co)
                {
                    if (files)
                        Driver { co, app, ed }.files(dir);
                    else if (undo)
                        Driver { co, app, ed }.undo_redo();
                    else if (steps)
                        Driver { co, app, ed }.steps();
                    else if (gaps)
                        Driver { co, app, ed }.gaps();
                    else if (sound)
                        Driver { co, app, ed }.sound();
                    else
                        Driver { co, app, ed }.run();
                    app.quit();
                });
        });
    if (files) // the window closed with unsaved changes: they were kept
    {
        std::string text, err;
        CHECK(read_file(recovery_path(dir + "/state"), &text, &err));
        CHECK(text.find("noise") != std::string::npos
            && text.find("# Kept when PulseRack closed") == 0);
        std::filesystem::remove_all(dir);
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
