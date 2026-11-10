// The window, driven like a user would, through gpuic's synthetic input (it goes through the real
// event path: hit testing, capture, handlers). Needs a display; exits 77 (skipped) without one.
//   ui_smoke                  the interaction test
//   ui_smoke --check-ppm F    F is a screenshot that is not blank (several colours, not all one)
//   ui_smoke --pose PATCH NODE PORT X Y   opens PATCH, presses on that jack and holds a cable at
//                             window (X, Y), for a screenshot of a drag (GPUIC_SCREENSHOT,
//                             GPUIC_WAIT_MS)
#include "engine/model/registry.h"
#include "ui/geometry.h"
#include "ui/rack_ui.h"
#include "ui/style.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
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
    float peak() // the engine's output, rendered here (there is no audio device yet)
    {
        std::vector<float> l(4800), r(4800);
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
        cables(osc, vca, adsr);

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
    }
};
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "--check-ppm"))
        return check_ppm(argv[2]);
    if (argc == 7 && !std::strcmp(argv[1], "--pose"))
    {
        static Options  opt;
        static PortRef  jack;
        static gp_point to;
        opt.patch_path = argv[2];
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
            auto        ed = open_rack(app, Options { }, &error);
            CHECK((bool)ed);
            app.ctx(ed).spawn(
                [app, ed](Co &co)
                {
                    Driver { co, app, ed }.run();
                    app.quit();
                });
        });
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
