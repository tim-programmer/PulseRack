#include "ui/rack_ui.h"

#include "engine/model/patchfile.h"
#include "engine/model/registry.h"
#include "ui/geometry.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace rack::ui
{

using namespace gpuic;
namespace st = style;

namespace
{

    Element label(std::string s, float size, Color c)
    {
        return text(std::move(s)).text_size(size).text_color(c);
    }

    Color with_alpha(Color c, float a)
    {
        c.a = a;
        return c;
    }

    std::string lower(std::string s)
    {
        for (char &ch : s)
            ch = (char)std::tolower((unsigned char)ch);
        return s;
    }

    // What the hint line says about a control: its module, name and value, and how to use it.
    std::string describe(const ModuleDesc &d, const ParamDesc &p, float value)
    {
        std::string how;
        switch (p.control)
        {
        case Control::Knob:
        case Control::Slider:
            how = "drag up or down (Shift: fine), wheel, double-click: default";
            break;
        case Control::Selector:
            how = "click < or >, or use the wheel";
            break;
        case Control::Switch:
        case Control::Button:
            how = "click to switch";
            break;
        }
        return std::string(d.name) + "  ·  " + std::string(p.name) + ": " + format_value(p, value)
            + "      " + how;
    }
} // namespace

// =====================================================================================================
// The hint line and the toolbar

struct HintLine
{
    Entity<Editor> ed;
    Element        render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        return div()
            .w_full()
            .h(st::kHintH)
            .flex_row()
            .items_center()
            .px(12)
            .bg(st::kSurface)
            .border_top(1)
            .border_color(st::kBorder)
            .child(label(e ? e->hint() : std::string(), 12, st::kSubtext));
    }
};

struct Toolbar
{
    Entity<Editor> ed;
    Element        render(Ctx cx)
    {
        Editor      *e = ed.get(cx);
        const size_t n = e ? e->engine.patch().nodes().size() : 0;
        return div()
            .w_full()
            .h(st::kToolbarH)
            .flex_row()
            .items_center()
            .gap(14)
            .px(12)
            .bg(st::kSurface)
            .border_bottom(1)
            .border_color(st::kBorder)
            .child(label("PulseRack", 13, st::kText).font_bold())
            .child(label(e ? e->patch_name : std::string(), 13, st::kSubtext))
            .child(label(std::to_string(n) + (n == 1 ? " module" : " modules"), 12, st::kDim))
            .child(div().flex_1())
            .child(label(e ? e->audio_status() : std::string(), 12, st::kDim))
            .child(div()
                    .h(24)
                    .px(10)
                    .gap(6)
                    .rounded(4)
                    .flex_row()
                    .items_center()
                    .cursor_pointer()
                    .bg(st::kSurfaceHi)
                    .hover_bg(st::kBorder)
                    .border(1)
                    .border_color(st::kBorder)
                    .child(div().w(8).h(8).rounded_full().bg(
                        e && e->audio_running() ? st::kLedOn : st::kLedOff))
                    .child(label("Audio", 12, st::kText))
                    .on_click(
                        [](Toolbar &self, Ctx cx)
                        {
                            if (Editor *ed = self.ed.get(cx))
                            {
                                if (ed->audio_running())
                                    ed->stop_audio(cx);
                                else
                                    ed->start_audio(cx);
                            }
                        }));
    }
};

// =====================================================================================================
// The browser: a search box, category chips, and a card per module type with an Add button.

struct Browser
{
    Entity<Editor> ed;
    TextInput      search;
    std::string    category; // empty: all

    // `value` points into the descriptors' static strings (a handler may capture only what needs no
    // destructor)
    Element chip(std::string_view name, std::string_view value, Color accent)
    {
        const bool on = category == value;
        return div()
            .h(22)
            .px(9)
            .rounded(11)
            .flex_row()
            .items_center()
            .cursor_pointer()
            .bg(on ? with_alpha(accent, 0.22f) : st::kSurfaceHi)
            .border(1)
            .border_color(on ? accent : st::kBorder)
            .hover_bg(on ? with_alpha(accent, 0.3f) : st::kSurface)
            .child(label(std::string(name), 11, on ? accent : st::kSubtext))
            .on_click(
                [value](Browser &self, Ctx cx)
                {
                    self.category = std::string(value);
                    cx.notify();
                });
    }

    Element card(const ModuleDesc *d)
    {
        const auto cc = st::category(d->category);
        return div()
            .h(st::kCardH)
            .flex_shrink_0()
            .w_full()
            .p(8)
            .gap(3)
            .flex_col()
            .rounded(5)
            .bg(st::kSurfaceHi)
            .hover_bg(st::kSurface)
            .border_left(3)
            .border_color(cc.chip)
            // drag the card onto the rack to add the module there (the Add button adds it near the
            // view)
            .on_mouse_down(
                [d](Browser &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->module_drag_begin(cx, *d, ev.position);
                })
            .on_drag([](Browser &self, Ctx cx, const gp_mouse_event &ev)
                { self.ed.get(cx)->module_drag_move(cx, ev.position); })
            .on_mouse_up([](Browser &self, Ctx cx, const gp_mouse_event &ev)
                { self.ed.get(cx)->module_drag_end(cx, ev.position); })
            .child(div()
                    .flex_row()
                    .items_center()
                    .child(div()
                            .flex_col()
                            .gap(1)
                            .child(label(std::string(d->name), 13, st::kText).font_bold())
                            .child(label(std::string(d->category) + "  ·  "
                                    + std::to_string(d->width_hp) + " HP",
                                10, cc.chip)))
                    .child(div().flex_1())
                    .child(div()
                            .w(st::kAddW)
                            .h(st::kAddH)
                            .rounded(4)
                            .flex_shrink_0()
                            .flex_row()
                            .items_center()
                            .justify_center()
                            .cursor_pointer()
                            .bg(st::kAccent)
                            .hover_bg(st::kAccentHi)
                            .child(label("Add", 12, st::kBase).font_bold())
                            .on_click(
                                [d](Browser &self, Ctx cx)
                                {
                                    if (Editor *e = self.ed.get(cx))
                                        e->add_module(cx, *d);
                                })))
            .child(label(std::string(d->description), 11, st::kSubtext));
    }

    Element render(Ctx cx)
    {
        const std::string q = search ? lower(search.text(cx.app())) : std::string();
        Element           chips = div().flex_row().flex_wrap().gap(6).h(st::kChipsH).pt(8);
        chips.child(chip("All", "", st::kText));
        std::vector<std::string_view> cats;
        for (const ModuleDesc *d : all_modules())
            if (std::find(cats.begin(), cats.end(), d->category) == cats.end())
                cats.push_back(d->category);
        for (std::string_view c : cats)
            chips.child(chip(c, c, st::category(c).chip));

        Element list
            = div().flex_col().gap(st::kCardGap).flex_1().overflow_y_scroll().scrollbar_autohide();
        int shown = 0;
        for (const ModuleDesc *d : all_modules())
        {
            if (!category.empty() && d->category != category)
                continue;
            if (!q.empty() && lower(std::string(d->name)).find(q) == std::string::npos
                && lower(std::string(d->description)).find(q) == std::string::npos
                && lower(std::string(d->category)).find(q) == std::string::npos)
                continue;
            list.child(card(d));
            shown++;
        }
        if (!shown)
            list.child(label("No module matches.", 12, st::kDim));

        return div()
            .w(st::kBrowserW)
            .h_full()
            .flex_col()
            .flex_shrink_0()
            .p(st::kBrowserPad)
            .bg(st::kBase)
            .border_right(1)
            .border_color(st::kBorder)
            .child(div().h(st::kSearchH).flex_shrink_0().child(search.element()))
            .child(std::move(chips))
            .child(std::move(list));
    }
};

void open_menu(Ctx cx, Editor &e, uint32_t node); // the panel's right-click menu (in the rack view)

// =====================================================================================================
// A panel: one module, a cached view of its own, so turning a knob rebuilds this panel only.

struct PanelView
{
    Entity<Editor> ed;
    uint32_t       node = 0;

    int   active = -1; // the control being dragged
    float travel = 0, last_y = 0;

    Editor::Led        led_;       // copied from the editor at render: what the LED shows
    std::array<int, 2> meter_ { }; // and the output level (Audio Output panels)

    bool     moving = false; // the title bar is being dragged
    gp_point grab { };
    int      x0 = 0, y0 = 0;

    const Node *find(Ctx cx) const
    {
        Editor *e = ed.get(cx);
        return e ? e->engine.patch().find(node) : nullptr;
    }

    // ---- handlers
    void press(Ctx cx, size_t i, const gp_mouse_event &ev)
    {
        const Node *n = find(cx);
        Editor     *e = ed.get(cx);
        if (!n || ev.button != GP_MOUSE_LEFT)
            return;
        const ParamDesc &p = n->desc->params[i];
        if (ev.click_count >= 2)
        {
            active = -1;
            e->set_param(cx, node, i, p.def);
            return;
        }
        active = (int)i;
        travel = to_travel(p, n->params[i]);
        last_y = ev.position.y;
        e->say(cx, describe(*n->desc, p, n->params[i]));
        cx.notify();
    }
    void drag(Ctx cx, size_t i, const gp_mouse_event &ev)
    {
        const Node *n = find(cx);
        if (!n || active != (int)i)
            return;
        const ParamDesc &p = n->desc->params[i];
        const float      v
            = drag_value(p, travel, last_y - ev.position.y, (ev.mods & GP_MOD_SHIFT) != 0, &travel);
        last_y = ev.position.y;
        if (v != n->params[i])
            ed.get(cx)->set_param(cx, node, i, v);
    }
    void release(Ctx cx)
    {
        active = -1;
        cx.notify();
    }
    void wheel(Ctx cx, size_t i, const gp_wheel_event &ev)
    {
        const Node *n = find(cx);
        if (!n)
            return;
        // gpuic gives +40 per notch down; Shift+wheel arrives as dx, and is the fine adjust
        const bool  fine = ev.dy == 0 && ev.dx != 0;
        const float d = fine ? ev.dx : ev.dy;
        int         steps = -(int)std::lround(d / 40.f);
        if (steps == 0 && d != 0)
            steps = d > 0 ? -1 : 1;
        const ParamDesc &p = n->desc->params[i];
        ed.get(cx)->set_param(cx, node, i, step_value(p, n->params[i], steps, fine));
    }
    void click(Ctx cx, size_t i, const gp_mouse_event &ev, float cell_w)
    {
        const Node *n = find(cx);
        if (!n || ev.button != GP_MOUSE_LEFT)
            return;
        const ParamDesc &p = n->desc->params[i];
        float            v;
        if (p.control == Control::Selector)
            v = step_value(p, n->params[i], ev.local.x < cell_w / 2 ? -1 : 1, false);
        else
            v = n->params[i] >= 0.5f ? p.min : p.max;
        ed.get(cx)->set_param(cx, node, i, v);
    }

    // ---- elements
    // Lit by what the module's connected outputs carry (telemetry, while audio runs); red if it had
    // to drop events.
    Element activity_led() const
    {
        static const float kAlpha[4] = { 0, 0.35f, 0.7f, 1.f };
        return div()
            .absolute()
            .left(2)
            .top(kTitleHeight + 2)
            .w(6)
            .h(6)
            .rounded_full()
            .border(1)
            .border_color(st::kKnobEdge)
            .bg(led_.dropped     ? st::kDanger
                    : led_.level ? with_alpha(st::kLedOn, kAlpha[led_.level])
                                 : st::kLedOff);
    }
    Element meters(const PanelLayout &L) const
    {
        const float top = L.controls_bottom + 12, h = std::max(20.f, L.jacks_top - top - 12);
        const float bw = 8, x0 = L.width / 2 - bw - 2;
        Element     m = div().absolute().left(x0).top(top).w(2 * bw + 4).h(h);
        for (int ch = 0; ch < 2; ch++)
        {
            const float frac = (float)meter_[(size_t)ch] / 40.f;
            const Color c = frac >= 1.f ? st::kDanger : frac > 0.9f ? rgb(0xf9e2af) : st::kLedOn;
            m.child(div()
                    .absolute()
                    .left((float)ch * (bw + 4))
                    .top(0)
                    .w(bw)
                    .h(h)
                    .rounded(2)
                    .bg(st::kWell)
                    .child(div()
                            .absolute()
                            .left(0)
                            .top(h * (1 - frac))
                            .w(bw)
                            .h(h * frac)
                            .rounded(2)
                            .bg(c)));
        }
        return m;
    }

    Element knob(float t) const
    {
        return div()
            .w(kKnobSize)
            .h(kKnobSize)
            .flex_shrink_0()
            .rounded_full()
            .bg_gradient(180, { stop(rgb(0x343743), 0), stop(st::kKnobFace, 1) })
            .border(2)
            .border_color(st::kKnobEdge)
            .shadow(0, 2, 4, with_alpha(rgb(0x000000), 0.35f))
            // a full-size overlay turned about the face's centre carries the pointer
            .child(div()
                    .absolute()
                    .left(-2)
                    .top(-2)
                    .w(kKnobSize)
                    .h(kKnobSize)
                    .rotate(-135 + 270 * t)
                    .child(div()
                            .absolute()
                            .left(kKnobSize / 2 - 1.5f)
                            .top(4)
                            .w(3)
                            .h(9)
                            .rounded(1.5f)
                            .bg(st::kKnobPointer)));
    }
    Element slider(float t, Color accent) const
    {
        constexpr float kThumb = 10, kTrackW = 6, kW = 22;
        return div()
            .w(kW)
            .h(kSliderTrack)
            .flex_shrink_0()
            .child(div()
                    .absolute()
                    .left((kW - kTrackW) / 2)
                    .top(0)
                    .w(kTrackW)
                    .h(kSliderTrack)
                    .rounded(3)
                    .bg(st::kWell)
                    .child(div()
                            .absolute()
                            .left(0)
                            .top(kSliderTrack * (1 - t))
                            .w(kTrackW)
                            .h(kSliderTrack * t)
                            .rounded(3)
                            .bg(with_alpha(accent, 0.7f))))
            .child(div()
                    .absolute()
                    .left(0)
                    .top((1 - t) * (kSliderTrack - kThumb))
                    .w(kW)
                    .h(kThumb)
                    .rounded(2)
                    .bg(st::kKnobPointer)
                    .border(1)
                    .border_color(st::kKnobEdge));
    }
    Element led(bool on, Color accent) const
    {
        return div()
            .w(8)
            .h(8)
            .flex_shrink_0()
            .rounded_full()
            .bg(on ? accent : st::kLedOff)
            .border(1)
            .border_color(st::kKnobEdge);
    }

    Element control(const ModuleDesc &d, size_t i, float value, const Rect &r, Color accent) const
    {
        const ParamDesc &p = d.params[i];
        const float      t = to_travel(p, value);
        Element          cell
            = div().absolute().left(r.x).top(r.y).w(r.w).h(r.h).flex_col().items_center().gap(2);
        Element name = div().w(r.w).flex_row().justify_center().child(
            label(std::string(p.name), 9, st::kText).text_center());
        Element val = label(format_value(p, value), 9, st::kSubtext);
        switch (p.control)
        {
        case Control::Knob:
        case Control::Slider:
            cell.child(p.control == Control::Knob ? knob(t) : slider(t, accent))
                .child(std::move(name))
                .child(std::move(val))
                .on_mouse_down([i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.press(cx, i, ev); })
                .on_drag([i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.drag(cx, i, ev); })
                .on_mouse_up([](PanelView &self, Ctx cx) { self.release(cx); })
                .on_wheel([i](PanelView &self, Ctx cx, const gp_wheel_event &ev)
                    { self.wheel(cx, i, ev); });
            break;
        case Control::Selector:
            cell.child(std::move(name))
                .child(div()
                        .w(r.w)
                        .h(22)
                        .rounded(3)
                        .bg(st::kWell)
                        .flex_row()
                        .items_center()
                        .justify_between()
                        .px(6)
                        .child(label("<", 12, st::kSubtext))
                        .child(label(format_value(p, value), 11, st::kText))
                        .child(label(">", 12, st::kSubtext)))
                .cursor_pointer()
                .on_click([i, w = r.w](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.click(cx, i, ev, w); })
                .on_wheel([i](PanelView &self, Ctx cx, const gp_wheel_event &ev)
                    { self.wheel(cx, i, ev); });
            break;
        case Control::Switch:
        case Control::Button:
        {
            const bool on = value >= 0.5f;
            Element    body = p.control == Control::Switch
                ? div()
                      .w(26)
                      .h(14)
                      .rounded(7)
                      .bg(on ? with_alpha(accent, 0.75f) : st::kWell)
                      .border(1)
                      .border_color(st::kKnobEdge)
                      .child(div()
                              .absolute()
                              .left(on ? 12 : 0)
                              .top(0)
                              .w(12)
                              .h(12)
                              .rounded_full()
                              .bg(st::kKnobPointer))
                : div()
                      .w(24)
                      .h(16)
                      .rounded(3)
                      .bg_gradient(180, { stop(rgb(0x3a3d4a), 0), stop(st::kKnobFace, 1) })
                      .border(1)
                      .border_color(st::kKnobEdge)
                      .active_bg(st::kWell);
            cell.child(div()
                           .flex_row()
                           .items_center()
                           .gap(5)
                           .h(18)
                           .child(led(on, st::kLedOn))
                           .child(std::move(body)))
                .child(std::move(name))
                .cursor_pointer()
                .on_click([i, w = r.w](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.click(cx, i, ev, w); });
            break;
        }
        }
        cell.on_hover([i](PanelView &self, Ctx cx, bool inside)
            { self.ed.get(cx)->hover(cx, self.node, (int)i, inside); });
        return cell;
    }

    Element jack(const ModuleDesc &d, size_t i, const Rect &c, const Rect &j) const
    {
        const PortDesc &p = d.ports[i];
        const bool      event = p.kind == Kind::Event;
        Element         cell = div().absolute().left(c.x).top(c.y).w(c.w).h(c.h).rounded(3);
        if (p.is_output)
            cell.bg(with_alpha(rgb(0x000000), 0.3f));
        cell.child(div()
                       .absolute()
                       .left(j.x - c.x)
                       .top(j.y - c.y)
                       .w(j.w)
                       .h(j.h)
                       .rounded(event ? 3 : j.w / 2)
                       .bg_gradient(180, { stop(rgb(0xd5d8e2), 0), stop(rgb(0x9a9eab), 1) })
                       .border(1)
                       .border_color(st::kKnobEdge)
                       .flex_row()
                       .items_center()
                       .justify_center()
                       .child(div().w(8).h(8).rounded(event ? 1 : 4).bg(st::kWell)))
            .child(div()
                    .absolute()
                    .left(0)
                    .top(j.bottom() - c.y + 2)
                    .w(c.w)
                    .flex_row()
                    .justify_center()
                    .child(label(std::string(p.name), 8, st::kText).whitespace_pre()))
            .on_hover([i](PanelView &self, Ctx cx, bool inside)
                { self.ed.get(cx)->hover(cx, self.node, Editor::kHoverPort - (int)i, inside); })
            .cursor_pointer()
            .on_mouse_down(
                [i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->jack_press(
                            cx, PortRef { self.node, (uint16_t)i }, ev.position);
                })
            .on_drag([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                { self.ed.get(cx)->jack_drag(cx, ev.position); })
            .on_mouse_up([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                { self.ed.get(cx)->jack_release(cx, ev.position); });
        return cell;
    }

    Element render(Ctx cx)
    {
        const Node *n = find(cx);
        if (!n)
            return div();
        const ModuleDesc  &d = *n->desc;
        const PanelLayout &L = panel_layout(d);
        const auto         cc = st::category(d.category);
        {
            Editor *e = ed.get(cx);
            auto    it = e->leds.find(node);
            led_ = it == e->leds.end() ? Editor::Led { } : it->second;
            meter_ = e->meter;
        }

        Element p
            = div()
                  .w(L.width)
                  .h(L.height)
                  .rounded(4)
                  .bg_gradient(180, { stop(cc.top, 0), stop(cc.bottom, 1) })
                  .border(kPanelBorder)
                  .border_color(st::kKnobEdge)
                  .shadow(0, 3, 8, with_alpha(rgb(0x000000), 0.4f))
                  .on_mouse_down([](PanelView &) { }); // a press on the panel does not pan the rack

        // the title bar moves the panel; a right-click opens its menu
        p.child(div()
                .absolute()
                .left(0)
                .top(0)
                .w(L.width - 2 * kPanelBorder)
                .h(kTitleHeight)
                .rounded_top(3)
                .bg(cc.title)
                .flex_row()
                .items_center()
                .justify_center()
                .overflow_hidden()
                .child(label(std::string(d.name), L.width < 100 ? 9 : 11, st::kText).font_bold())

                .on_mouse_down(
                    [](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    {
                        const Node *n = self.find(cx);
                        Editor     *e = self.ed.get(cx);
                        if (!n)
                            return;
                        if (ev.button == GP_MOUSE_RIGHT)
                        {
                            open_menu(cx, *e, self.node);
                            return;
                        }
                        self.moving = ev.button == GP_MOUSE_LEFT;
                        self.grab = ev.position;
                        self.x0 = (int)n->x;
                        self.y0 = (int)n->y;
                    })
                .on_drag(
                    [](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    {
                        if (!self.moving)
                            return;
                        const int dx = (int)std::lround((ev.position.x - self.grab.x) / kHp);
                        const int dy = (int)std::lround((ev.position.y - self.grab.y) / kHp);
                        self.ed.get(cx)->move_module(cx, self.node, self.x0 + dx, self.y0 + dy);
                    })
                .on_mouse_up([](PanelView &self) { self.moving = false; })
                .on_hover([](PanelView &self, Ctx cx, bool inside)
                    { self.ed.get(cx)->hover(cx, self.node, Editor::kHoverTitle, inside); }));

        for (size_t i = 0; i < d.params.size(); i++)
            p.child(control(d, i, n->params[i], L.params[i], cc.chip));
        for (size_t i = 0; i < d.ports.size(); i++)
            p.child(jack(d, i, L.ports[i], L.jacks[i]));
        p.child(activity_led());
        if (d.id == "out") // the output level, between its controls and its jacks
            p.child(meters(L));

        if (active >= 0 && (size_t)active < d.params.size())
        { // the value while dragging
            const Rect &r = L.params[(size_t)active];
            const float bw = 84;
            p.child(div()
                    .absolute()
                    .left(std::clamp(r.x + r.w / 2 - bw / 2, 2.f, std::max(2.f, L.width - bw - 2)))
                    .top(std::max(r.y - 20, 2.f))
                    .w(bw)
                    .h(18)
                    .rounded(9)
                    .bg(with_alpha(rgb(0x000000), 0.85f))
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .child(label(format_value(d.params[(size_t)active], n->params[(size_t)active]),
                        10, st::kText)));
        }
        return p;
    }
};

void open_cable_menu(Ctx cx, Editor &e, PortRef to, Pt at); // a cable's right-click menu

// =====================================================================================================
// Cables: each a cached view of its own (moving one rebuilds that view only), drawn over the panels
// in rack pixels computed from the model, with a plug over each jack. The cable being dragged is
// DragView.

namespace
{
    Element ring(Pt c, float r, Color col, float width)
    {
        return div()
            .absolute()
            .left(c.x - r)
            .top(c.y - r)
            .w(2 * r)
            .h(2 * r)
            .rounded_full()
            .border(width)
            .border_color(col);
    }
    Element plug(Pt c, Color col)
    {
        return div()
            .absolute()
            .left(c.x - 7)
            .top(c.y - 7)
            .w(14)
            .h(14)
            .rounded_full()
            .bg(col)
            .border(2)
            .border_color(st::kKnobEdge);
    }
    Element cable_curve_element(const std::array<Pt, 4> &p, Color col, float width)
    {
        return curve(xy(p[0].x, p[0].y), xy(p[1].x, p[1].y), xy(p[2].x, p[2].y), xy(p[3].x, p[3].y))
            .stroke_width(width)
            .stroke_color(col);
    }
} // namespace

struct CableView
{
    Entity<Editor> ed;
    PortRef        to; // the cable into this input

    Element render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        if (!e)
            return div();
        const Patch &p = e->engine.patch();
        const Cable *c = p.cable_into(to);
        if (!c || (e->drag.active && e->drag.picked && e->drag.picked->to == to))
            return div(); // gone, or lifted off and drawn by the drag view
        const Pt    a = jack_center(*p.find(c->from.node), c->from.port);
        const Pt    b = jack_center(*p.find(c->to.node), c->to.port);
        const bool  sel = e->selected && *e->selected == to;
        const auto  it = e->cable_colour.find({ to.node, to.port });
        const Color col = st::cable_colour(it == e->cable_colour.end() ? 0 : it->second);
        // a press near either end is a press on that jack (the plug covers it); elsewhere it
        // selects the cable, and a right-click opens its menu
        return div()
            .absolute()
            .left(0)
            .top(0)
            .child(cable_curve_element(
                cable_curve(a, b), with_alpha(col, sel ? 1.f : 0.85f), sel ? 7.f : 4.5f)
                    .hit_tolerance(4)
                    .hover_bg(with_alpha(col, 1.f))
                    .cursor_pointer()
                    .on_mouse_down(
                        [](CableView &self, Ctx cx, const gp_mouse_event &ev)
                        {
                            Editor  *e = self.ed.get(cx);
                            const Pt q = e->to_rack(cx, ev.position);
                            cx.blur();
                            if (ev.button == GP_MOUSE_RIGHT)
                                open_cable_menu(cx, *e, self.to, q);
                            else if (auto j = jack_at(e->engine.patch(), q.x, q.y, 0))
                                e->jack_press(cx, *j, ev.position);
                            else if (ev.button == GP_MOUSE_LEFT)
                                e->select_cable(cx, self.to);
                        })
                    .on_drag([](CableView &self, Ctx cx, const gp_mouse_event &ev)
                        { self.ed.get(cx)->jack_drag(cx, ev.position); })
                    .on_mouse_up([](CableView &self, Ctx cx, const gp_mouse_event &ev)
                        { self.ed.get(cx)->jack_release(cx, ev.position); })
                    .on_hover(
                        [](CableView &self, Ctx cx, bool inside)
                        {
                            self.ed.get(cx)->hover(
                                cx, self.to.node, Editor::kHoverCable - self.to.port, inside);
                        }))
            .child(plug(a, col))
            .child(plug(b, col));
    }
};

struct DragView
{
    Entity<Editor> ed;

    Element render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        if (!e)
            return div();
        Element w = div().absolute().left(0).top(0);
        if (const auto &m = e->mdrag; m.active && m.moved && m.over_rack)
        { // where a module dragged from the browser would land
            const Rect  r = node_rect(*m.desc, (float)m.x, (float)m.y);
            const Color c = m.fits ? st::kLedOn : st::kDanger;
            w.child(div()
                    .absolute()
                    .left(r.x)
                    .top(r.y)
                    .w(r.w)
                    .h(r.h)
                    .rounded(4)
                    .bg(with_alpha(st::category(m.desc->category).top, m.fits ? 0.45f : 0.25f))
                    .border(2)
                    .border_color(c)
                    .flex_col()
                    .items_center()
                    .pt(3)
                    .child(label(std::string(m.desc->name), 11, st::kText).font_bold()));
        }
        if (!e->drag.active)
            return w;
        const auto  &d = e->drag;
        const Patch &p = e->engine.patch();
        for (const PortRef &t : d.targets) // every jack it fits
            if (const Node *n = p.find(t.node))
                w.child(ring(jack_center(*n, t.port), 13, with_alpha(st::kLedOn, 0.5f), 2));
        if (d.over)
            if (const Node *n = p.find(d.over->node))
                w.child(ring(jack_center(*n, d.over->port), 15,
                    d.verdict == Err::Ok ? st::kLedOn : st::kDanger, 3));
        const Node *fn = p.find(d.fixed.node);
        if (!fn)
            return w;
        int colour = e->next_colour;
        if (d.picked)
        {
            const auto it = e->cable_colour.find({ d.picked->to.node, d.picked->to.port });
            if (it != e->cable_colour.end())
                colour = it->second;
        }
        const Color col = st::cable_colour(colour);
        const Pt    a = jack_center(*fn, d.fixed.port);
        w.child(cable_curve_element(cable_curve(a, d.loose), with_alpha(col, 0.9f), 4.5f))
            .child(plug(a, col))
            .child(plug(d.loose, col));
        return w;
    }
};

// =====================================================================================================
// The rack: a scrolling surface with the panels at their places; drag empty space to pan.

struct RackView
{
    Entity<Editor>         ed;
    Scroll                 scroll;
    bool                   panning = false;
    gp_point               pan_p0 { }, pan_off0 { };
    uint32_t               menu_node = 0; // the panel whose menu is open
    std::optional<PortRef> menu_cable; // or the cable whose menu is open, at menu_at (rack pixels)
    Pt                     menu_at;

    template <class F> static Element menu_item(std::string text, Color c, F &&f)
    {
        return div()
            .h(26)
            .px(8)
            .rounded(3)
            .flex_row()
            .items_center()
            .cursor_pointer()
            .hover_bg(st::kSurface)
            .child(label(std::move(text), 12, c))
            .on_click(std::forward<F>(f));
    }

    Element render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        if (!e)
            return div();
        const Patch &patch = e->engine.patch();
        float        right = 0, bottom = 0;
        for (const Node &n : patch.nodes())
        {
            const Rect r = node_rect(*n.desc, n.x, n.y);
            right = std::max(right, r.right());
            bottom = std::max(bottom, r.bottom());
        }
        // room to move panels into: a screen beyond the last one, never less than three of them
        const float cw = std::max(right + st::kWindowW, 3 * st::kWindowW);
        const float ch = std::max(bottom + kPanelHeight, 3 * kPanelHeight);

        Element content
            = div()
                  .w(cw)
                  .h(ch)
                  .bg(st::kRack)
                  .on_mouse_down(
                      [](RackView &self, Ctx cx, const gp_mouse_event &ev)
                      {
                          if (self.menu_node || self.menu_cable)
                          {
                              self.menu_node = 0;
                              self.menu_cable.reset();
                              cx.notify();
                          }
                          cx.blur(); // the search box lets go of the keyboard (Delete is the
                                     // rack's)
                          if (Editor *e = self.ed.get(cx); e && e->selected)
                              e->select_cable(cx, std::nullopt);
                          self.panning = ev.button == GP_MOUSE_LEFT;
                          self.pan_p0 = ev.position;
                          self.pan_off0 = self.scroll.offset(cx);
                      })
                  .on_drag(
                      [](RackView &self, Ctx cx, const gp_mouse_event &ev)
                      {
                          if (self.panning)
                              self.scroll.to(cx, self.pan_off0.x - (ev.position.x - self.pan_p0.x),
                                  self.pan_off0.y - (ev.position.y - self.pan_p0.y));
                      })
                  .on_mouse_up([](RackView &self) { self.panning = false; });
        for (float y = kPanelHeight; y < ch;
            y += kPanelHeight) // a faint line between rows of panels
            content.child(div().absolute().left(0).top(y).w(cw).h(1).bg(st::kRackLine));

        for (auto &[id, view] : e->panels)
            if (const Node *n = patch.find(id))
                content.child(div()
                        .absolute()
                        .left(n->x * kHp)
                        .top(n->y * kHp)
                        .key_id(id)
                        .child(view.cached()));
        for (auto &[key, view] : e->cable_views) // over the panels
            content.child(div()
                    .absolute()
                    .left(0)
                    .top(0)
                    .key_id((uint64_t)1 << 40 | (uint64_t)key.first << 16 | key.second)
                    .child(view.cached()));
        content.child(
            div().absolute().left(0).top(0).key_id((uint64_t)1 << 41).child(e->drag_view.cached()));

        if (menu_cable && patch.cable_into(*menu_cable))
        {
            const PortRef to = *menu_cable;
            content.child(div()
                    .absolute()
                    .left(menu_at.x + 4)
                    .top(menu_at.y + 4)
                    .w(180)
                    .p(4)
                    .rounded(5)
                    .bg(st::kSurfaceHi)
                    .border(1)
                    .border_color(st::kBorder)
                    .shadow(0, 6, 16, with_alpha(rgb(0x000000), 0.5f))
                    .on_mouse_down([](RackView &) { })
                    .child(menu_item("Delete cable", st::kDanger,
                        [to](RackView &self, Ctx cx)
                        {
                            self.menu_cable.reset();
                            cx.notify();
                            if (Editor *ed = self.ed.get(cx))
                                ed->disconnect(cx, to);
                        }))
                    .child(menu_item("Cancel", st::kText,
                        [](RackView &self, Ctx cx)
                        {
                            self.menu_cable.reset();
                            cx.notify();
                        })));
        }
        if (const Node *n = menu_node ? patch.find(menu_node) : nullptr)
        {
            const uint32_t id = menu_node;
            content.child(div()
                    .absolute()
                    .left(n->x * kHp + 6)
                    .top(n->y * kHp + kTitleHeight + 2)
                    .w(180)
                    .p(4)
                    .rounded(5)
                    .bg(st::kSurfaceHi)
                    .border(1)
                    .border_color(st::kBorder)
                    .shadow(0, 6, 16, with_alpha(rgb(0x000000), 0.5f))
                    .on_mouse_down([](RackView &) { })
                    .child(menu_item("Delete " + std::string(n->desc->name), st::kDanger,
                        [id](RackView &self, Ctx cx)
                        {
                            self.menu_node = 0;
                            cx.notify();
                            if (Editor *ed = self.ed.get(cx))
                                ed->remove_module(cx, id);
                        }))
                    .child(menu_item("Cancel", st::kText,
                        [](RackView &self, Ctx cx)
                        {
                            self.menu_node = 0;
                            cx.notify();
                        })));
        }
        return div().size_full().overflow_scroll().track_scroll(scroll).bg(st::kRack).child(
            std::move(content));
    }
};

void open_menu(Ctx cx, Editor &e, uint32_t node)
{
    e.rack.update(cx,
        [node](RackView &r, Ctx c)
        {
            r.menu_node = node;
            r.menu_cable.reset();
            c.notify();
        });
}

void open_cable_menu(Ctx cx, Editor &e, PortRef to, Pt at)
{
    e.select_cable(cx, to);
    e.rack.update(cx,
        [to, at](RackView &r, Ctx c)
        {
            r.menu_node = 0;
            r.menu_cable = to;
            r.menu_at = at;
            c.notify();
        });
}

// =====================================================================================================
// The window

// The name of the module being dragged from the browser, under the pointer (window pixels).
struct PointerChip
{
    Entity<Editor> ed;
    Element        render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        if (!e || !e->mdrag.active || !e->mdrag.moved)
            return div();
        const auto &m = e->mdrag;
        // the pill needs room to size itself to its text, so while it is shown it lays out a
        // window-sized area (no handlers: the pointer goes through it, though the wheel would not
        // reach the rack, which does not matter during the drag); otherwise the view is empty
        return div().absolute().left(0).top(0).w(8000).h(8000).child(div()
                .absolute()
                .left(m.at.x + 14)
                .top(m.at.y + 10)
                .px(8)
                .h(22)
                .rounded(11)
                .flex_row()
                .items_center()
                .bg(with_alpha(st::category(m.desc->category).title, 0.95f))
                .border(1)
                .border_color(st::category(m.desc->category).chip)
                .flex_shrink_0()
                .child(
                    label(std::string(m.desc->name), 11, st::kText).font_bold().whitespace_pre()));
    }
};

struct Root
{
    Entity<Editor> ed;
    Element        render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        return div()
            .size_full()
            .flex_col()
            .bg(st::kBase)
            .text_color(st::kText)
            .child(e->toolbar.cached())
            .child(div()
                    .flex_row()
                    .flex_1()
                    .min_h(0)
                    .child(e->browser.cached())
                    .child(div().flex_1().h_full().min_w(0).child(e->rack.cached())))
            .child(e->hint_line.cached())
            .child(div().absolute().left(0).top(0).child(e->chip.cached()))
            // the computer keyboard as a piano: keys reach the root when nothing focused takes them
            .on_key_down(
                [](Root &self, Ctx cx, const gp_key_event &ev)
                {
                    if (!self.ed.get(cx)->piano(cx, ev, true))
                        cx.propagate();
                })
            .on_key_up(
                [](Root &self, Ctx cx, const gp_key_event &ev)
                {
                    if (!self.ed.get(cx)->piano(cx, ev, false))
                        cx.propagate();
                });
    }
};

// =====================================================================================================
// The editor

namespace
{
    RackViewport viewport_of(Ctx cx, Editor &e)
    {
        RackView *r = e.rack.get(cx);
        if (!r)
            return { };
        const gp_point off = r->scroll.offset(cx);
        gp_size        vp = r->scroll.viewport(cx);
        if (vp.w <= 0 || vp.h <= 0) // before the first layout: the window minus the chrome
            vp = { st::kWindowW - st::kBrowserW, st::kWindowH - st::kToolbarH - st::kHintH };
        return { off.x, off.y, vp.w, vp.h };
    }
} // namespace

void Editor::say(Ctx cx, std::string text)
{
    status = std::move(text);
    hover_text.clear(); // what an action did shows at once; the next element the pointer enters
                        // describes itself again
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
}

std::string Editor::describe_item(uint32_t node, int item) const
{
    const Node *n = engine.patch().find(node);
    if (!n)
        return { };
    const ModuleDesc &d = *n->desc;
    if (item >= 0 && (size_t)item < d.params.size())
        return describe(d, d.params[(size_t)item], n->params[(size_t)item]);
    if (item == kHoverTitle)
        return std::string(d.name) + "  ·  " + std::string(d.description)
            + "      drag the title to move it; right-click it to delete";
    const int port = kHoverPort - item;
    if (port >= 0 && (size_t)port < d.ports.size())
    {
        const PortDesc &p = d.ports[(size_t)port];
        return std::string(d.name) + "  ·  " + std::string(p.name) + ": "
            + (p.kind == Kind::Event ? "event " : "signal ") + (p.is_output ? "output" : "input")
            + (p.is_output ? "      drag from it to an input to connect"
                           : "      drag from it to an output to connect");
    }
    const int in_port = kHoverCable - item;
    if (in_port >= 0 && (size_t)in_port < d.ports.size())
        if (const Cable *c = engine.patch().cable_into(PortRef { node, (uint16_t)in_port }))
            return "Cable  " + port_name(c->from) + "  ->  " + port_name(c->to)
                + "      click to select (then Delete); right-click for its menu; drag a plug to "
                  "move it";
    return { };
}

std::string Editor::port_name(PortRef r) const
{
    const Node *n = engine.patch().find(r.node);
    if (!n || r.port >= n->desc->ports.size())
        return "?";
    return std::string(n->desc->name) + " " + std::string(n->desc->ports[r.port].name);
}

Pt Editor::to_rack(Ctx cx, gp_point w)
{
    const RackViewport vp = viewport_of(cx, *this);
    const gp_point     o = rack_origin();
    return { w.x - o.x + vp.x, w.y - o.y + vp.y };
}

// ---- sound

bool Editor::start_audio(Ctx cx)
{
    if (!audio)
        audio = std::make_unique<audio::AudioOut>(engine);
    std::string err;
    const bool  ok = audio->start(&err);
    audio_error = ok ? std::string() : err;
    if (ok)
    {
        meter_task.cancel();
        meter_task = cx.app().ctx(self).every(1.0 / 30, [](Editor &e, Ctx c) { e.tick(c); });
        say(cx, "Audio on: " + audio_status() + ".");
    }
    else
        say(cx, audio_error + ".");
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    return ok;
}

void Editor::stop_audio(Ctx cx)
{
    meter_task.cancel();
    if (audio)
        audio->stop();
    load_pct = -1;
    tick(cx); // the LEDs and meters go dark
    for (auto &[id, l] : leds)
        if (l.level || l.dropped)
        {
            l = Led { };
            if (auto it = panels.find(id); it != panels.end())
                it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
        }
    say(cx, "Audio off.");
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
}

std::string Editor::audio_status() const
{
    if (!audio_running())
        return audio_error.empty() ? "audio off" : audio_error;
    const audio::DeviceInfo &d = audio->info();
    char                     buf[128];
    std::snprintf(buf, sizeof buf, "%s  ·  %.1f kHz  ·  %.1f ms  ·  CPU %d %%", d.driver.c_str(),
        d.rate / 1000.0, d.latency_ms(), std::max(0, load_pct));
    return buf;
}

void Editor::tick(Ctx cx)
{
    Telemetry &t = engine.telemetry();
    for (auto &[id, view] : panels)
    {
        const int   slot = engine.telemetry_slot(id);
        const float a = t.take_activity(slot);
        const Led   now { a < 0.005f ? 0
                : a < 0.1f           ? 1
                : a < 0.4f           ? 2
                                     : 3,
            slot >= 0 && t.dropped[(size_t)slot].load(std::memory_order_relaxed) > 0 };
        Led &shown = leds[id];
        if (now.level != shown.level || now.dropped != shown.dropped)
        {
            shown = now;
            view.update(cx, [](PanelView &, Ctx c) { c.notify(); });
        }
    }
    bool meters_changed = false;
    for (int ch = 0; ch < 2; ch++)
    { // -60 .. 0 dB on 40 steps
        const float p = t.take_peak(ch);
        const float db = p > 1e-6f ? 20.f * std::log10(p) : -120.f;
        const int   m
            = p >= 0.999f ? 40 : std::clamp((int)std::lround((db + 60.f) / 60.f * 39.f), 0, 39);
        if (m != meter[(size_t)ch])
        {
            meter[(size_t)ch] = m;
            meters_changed = true;
        }
    }
    if (meters_changed)
        for (const Node &n : engine.patch().nodes())
            if (n.desc->id == "out")
                if (auto it = panels.find(n.id); it != panels.end())
                    it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    const int load
        = audio_running() ? (int)std::lround(100 * t.load.load(std::memory_order_relaxed)) : -1;
    if (load != load_pct)
    {
        load_pct = load;
        toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    }
}

namespace
{
    std::string note_name(int n)
    {
        static const char *names[12]
            = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
    }
} // namespace

bool Editor::piano(Ctx cx, const gp_key_event &ev, bool down)
{
    if (ev.mods & (GP_MOD_CTRL | GP_MOD_ALT | GP_MOD_SUPER))
        return false;
    if (Browser *b = browser.get(cx);
        b && b->search && cx.is_focused(Focus(b->search.focus(cx.app()))))
        return false; // typing in the search box
    // A W S E D F T G Y H U J K O L P ; : the white keys on the home row, the black ones above
    static const std::pair<const char *, int> keys[] = { { "a", 0 }, { "w", 1 }, { "s", 2 },
        { "e", 3 }, { "d", 4 }, { "f", 5 }, { "t", 6 }, { "g", 7 }, { "y", 8 }, { "h", 9 },
        { "u", 10 }, { "j", 11 }, { "k", 12 }, { "o", 13 }, { "l", 14 }, { "p", 15 }, { ";", 16 } };
    const std::string                         k = ev.key ? ev.key : "";
    if (k == "z" || k == "x")
    {
        if (down && !ev.repeat)
        {
            octave = std::clamp(octave + (k == "x" ? 1 : -1), -4, 4);
            say(cx, "Computer keyboard: the A key plays " + note_name(60 + 12 * octave) + ".");
        }
        return true;
    }
    int semi = -1;
    for (const auto &[name, s] : keys)
        if (k == name)
            semi = s;
    if (semi < 0)
        return false;
    uint8_t note;
    if (down)
    {
        if (ev.repeat || held.count(k))
            return true; // auto-repeat: the note is already on
        note = (uint8_t)std::clamp(60 + 12 * octave + semi, 0, 127);
        held[k] = note;
    }
    else
    {
        auto it = held.find(k);
        if (it == held.end())
            return true;
        note = it->second;
        held.erase(it);
    }
    int sent = 0;
    for (const Node &n : engine.patch().nodes())
        if (n.desc->id == "keyboard" && engine.key(n.id, down, note, 100) == Err::Ok)
            sent++;
    if (down)
        say(cx,
            sent ? "Note " + note_name(note) + (audio_running() ? "" : "  (audio is off)")
                 : "No Keyboard module: add one to play notes from the computer keyboard.");
    return true;
}

// ---- modules dragged from the browser

Slot Editor::drop_slot(Ctx cx, const ModuleDesc &d, gp_point window)
{ // the pointer holds the panel by the middle of its title
    const Pt q = to_rack(cx, window);
    return { (int)std::lround((q.x - d.width_hp * kHp / 2) / kHp),
        (int)std::lround((q.y - kTitleHeight / 2) / kHp) };
}

void Editor::module_drag_begin(Ctx, const ModuleDesc &d, gp_point window)
{
    mdrag = ModuleDrag { };
    mdrag.active = true;
    mdrag.desc = &d;
    mdrag.start = mdrag.at = window;
}

void Editor::module_drag_move(Ctx cx, gp_point window)
{
    if (!mdrag.active)
        return;
    mdrag.at = window;
    if (!mdrag.moved && std::hypot(window.x - mdrag.start.x, window.y - mdrag.start.y) < 4)
        return; // a click, so far
    mdrag.moved = true;
    const gp_point     o = rack_origin();
    const RackViewport vp = viewport_of(cx, *this);
    mdrag.over_rack
        = window.x >= o.x && window.y >= o.y && window.x < o.x + vp.w && window.y < o.y + vp.h;
    const std::string name(mdrag.desc->name);
    if (mdrag.over_rack)
    {
        const Slot s = drop_slot(cx, *mdrag.desc, window);
        mdrag.x = s.x;
        mdrag.y = s.y;
        mdrag.fits = !collides(engine.patch(), *mdrag.desc, s.x, s.y);
        status = mdrag.fits ? "Release to add " + name + " here."
                            : "No room here for " + name + ": it would overlap another panel.";
    }
    else
        status = "Drop " + name + " on the rack to add it.";
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
    chip.update(cx, [](PointerChip &, Ctx c) { c.notify(); });
}

void Editor::module_drag_end(Ctx cx, gp_point window)
{
    if (!mdrag.active)
        return;
    module_drag_move(cx, window);
    const ModuleDrag m = mdrag;
    mdrag = ModuleDrag { };
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
    chip.update(cx, [](PointerChip &, Ctx c) { c.notify(); });
    if (!m.moved)
        return; // a click: the card does nothing, its Add button does
    if (m.over_rack && m.fits)
        add_module_at(cx, *m.desc, m.x, m.y);
    else if (m.over_rack)
        say(cx, "Not added: no room there for " + std::string(m.desc->name) + ".");
    else
        say(cx, "Not added: drop a module on the rack.");
}

// ---- cables

void Editor::sync_cables(Ctx cx)
{
    App          app = cx.app();
    const Patch &patch = engine.patch();
    for (auto it = cable_views.begin(); it != cable_views.end();)
    {
        if (patch.cable_into(PortRef { it->first.first, it->first.second }))
            ++it;
        else
        {
            app.release(it->second);
            cable_colour.erase(it->first);
            it = cable_views.erase(it);
        }
    }
    for (const Cable &c : patch.cables())
    {
        const CableKey key { c.to.node, c.to.port };
        if (cable_views.count(key))
            continue;
        auto v = app.new_view<CableView>();
        app.get(v)->ed = self;
        app.get(v)->to = c.to;
        cable_views[key] = v;
        if (!cable_colour.count(key))
            cable_colour[key] = next_colour++;
    }
    if (selected && !patch.cable_into(*selected))
        selected.reset();
    refresh_rack(cx);
}

void Editor::refresh_cables_of(Ctx cx, uint32_t node)
{
    for (const Cable &c : engine.patch().cables())
        if (c.from.node == node || c.to.node == node)
            if (auto it = cable_views.find({ c.to.node, c.to.port }); it != cable_views.end())
                it->second.update(cx, [](CableView &, Ctx v) { v.notify(); });
}

Err Editor::connect(Ctx cx, PortRef a, PortRef b)
{
    Patch &patch = engine.patch();
    const auto [from, to] = as_cable(patch, a, b);
    const Err e = patch.connect(from, to);
    if (e != Err::Ok)
    {
        say(cx, "Not connected: " + std::string(describe(e)) + ".");
        return e;
    }
    if (engine.compile() != Err::Ok)
    {
        patch.disconnect(to);
        engine.compile();
        say(cx, "Not connected: the engine could not build the patch with it.");
        return Err::BadPatch;
    }
    sync_cables(cx);
    say(cx, "Connected " + port_name(from) + "  ->  " + port_name(to) + ".");
    return Err::Ok;
}

void Editor::disconnect(Ctx cx, PortRef to)
{
    const Cable *c = engine.patch().cable_into(to);
    if (!c)
        return;
    const std::string what = port_name(c->from) + "  ->  " + port_name(c->to);
    engine.patch().disconnect(to);
    engine.compile();
    sync_cables(cx);
    say(cx, "Removed the cable " + what + ".");
}

void Editor::select_cable(Ctx cx, std::optional<PortRef> to)
{
    const std::optional<PortRef> old = selected;
    selected = to && engine.patch().cable_into(*to) ? to : std::nullopt;
    for (const auto &k : { old, selected })
        if (k)
            if (auto it = cable_views.find({ k->node, k->port }); it != cable_views.end())
                it->second.update(cx, [](CableView &, Ctx v) { v.notify(); });
    if (selected)
    {
        const Cable *c = engine.patch().cable_into(*selected);
        say(cx,
            "Selected the cable " + port_name(c->from) + "  ->  " + port_name(c->to)
                + ".      Delete or Backspace removes it.");
    }
}

void Editor::delete_selected(Ctx cx)
{
    if (selected)
        disconnect(cx, *selected);
}

namespace
{
    // The patch as it would be without the cable being moved, to judge where it may go.
    Patch without(const Patch &p, const std::optional<Cable> &c)
    {
        Patch q = p;
        if (c)
            q.disconnect(c->to);
        return q;
    }
} // namespace

void Editor::jack_press(Ctx cx, PortRef jack, gp_point window)
{
    const Patch &p = engine.patch();
    const Node  *n = p.find(jack.node);
    if (!n || jack.port >= n->desc->ports.size())
        return;
    const bool output = n->desc->ports[jack.port].is_output;
    drag = CableDrag { };
    drag.active = true;
    if (!output)
        if (const Cable *c = p.cable_into(jack))
            drag.picked = *c; // lift the cable off this input; it hangs from its output
    drag.fixed = drag.picked ? drag.picked->from : jack;
    drag.loose = to_rack(cx, window);
    const Patch probe = without(p, drag.picked);
    for (const Node &m : probe.nodes())
        for (uint16_t k = 0; k < m.desc->ports.size(); k++)
        {
            const PortRef q { m.id, k };
            const auto [f, t] = as_cable(probe, drag.fixed, q);
            if (!(q == drag.fixed) && probe.can_connect(f, t) == Err::Ok)
                drag.targets.push_back(q);
        }
    if (selected)
        select_cable(cx, std::nullopt);
    if (drag.picked)
        if (auto it = cable_views.find({ drag.picked->to.node, drag.picked->to.port });
            it != cable_views.end())
            it->second.update(cx, [](CableView &, Ctx v) { v.notify(); });
    status = drag.picked
        ? "Move the plug to another input, or drop it on empty rack to remove the cable."
        : std::string("Drag to ") + (output ? "an input" : "an output")
            + " to connect; the jacks it fits are lit (" + std::to_string(drag.targets.size())
            + ").";
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
}

void Editor::jack_drag(Ctx cx, gp_point window)
{
    if (!drag.active)
        return;
    drag.loose = to_rack(cx, window);
    const Patch &p = engine.patch();
    drag.over = jack_at(p, drag.loose.x, drag.loose.y);
    if (drag.over && *drag.over == drag.fixed)
        drag.over.reset();
    if (drag.over)
    {
        const Patch probe = without(p, drag.picked);
        const auto [f, t] = as_cable(probe, drag.fixed, *drag.over);
        drag.verdict = probe.can_connect(f, t);
        status = drag.verdict == Err::Ok
            ? "Release to connect " + port_name(f) + "  ->  " + port_name(t) + "."
            : "Cannot connect here: " + std::string(describe(drag.verdict)) + ".";
    }
    else
        status = drag.picked ? "Release here to remove the cable."
                             : "Release on a lit jack to connect; anywhere else cancels.";
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
}

void Editor::jack_release(Ctx cx, gp_point window)
{
    if (!drag.active)
        return;
    jack_drag(cx, window); // where it ends
    const CableDrag d = drag;
    drag = CableDrag { };
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
    if (d.picked)
        if (auto it = cable_views.find({ d.picked->to.node, d.picked->to.port });
            it != cable_views.end())
            it->second.update(cx, [](CableView &, Ctx v) { v.notify(); });

    if (d.over && d.verdict == Err::Ok)
    {
        const auto [f, t] = as_cable(engine.patch(), d.fixed, *d.over);
        if (d.picked && d.picked->to == t)
        {
            say(cx, "The cable is where it was.");
            return;
        }
        if (d.picked)
        { // moved to another input: same cable, same colour
            const auto it = cable_colour.find({ d.picked->to.node, d.picked->to.port });
            if (it != cable_colour.end())
                cable_colour[{ t.node, t.port }] = it->second;
            engine.patch().disconnect(d.picked->to);
        }
        else
            cable_colour[{ t.node, t.port }] = next_colour++;
        if (connect(cx, f, t) != Err::Ok
            && d.picked) // cannot happen after can_connect; put it back
            connect(cx, d.picked->from, d.picked->to);
    }
    else if (d.picked && !d.over)
        disconnect(cx, d.picked->to);
    else if (d.over)
        say(cx, "Not connected: " + std::string(describe(d.verdict)) + ".");
    else
        say(cx, "No cable: release on a lit jack to connect.");
}

void Editor::hover(Ctx cx, uint32_t node, int item, bool inside)
{
    if (inside)
    {
        hover_node = node;
        hover_item = item;
        hover_text = describe_item(node, item);
    }
    else if (hover_node == node
        && hover_item == item) // a late leave of something else changes nothing
    {
        hover_node = 0;
        hover_text.clear();
    }
    else
        return;
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
}

void Editor::refresh_rack(Ctx cx)
{
    rack.update(cx, [](RackView &, Ctx c) { c.notify(); });
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
}

void Editor::sync_panels(Ctx cx)
{
    App          app = cx.app();
    const Patch &patch = engine.patch();
    for (auto it = panels.begin(); it != panels.end();)
    {
        if (patch.find(it->first))
            ++it;
        else
        {
            app.release(it->second);
            it = panels.erase(it);
        }
    }
    for (const Node &n : patch.nodes())
        if (!panels.count(n.id))
        {
            auto v = app.new_view<PanelView>();
            app.get(v)->ed = self;
            app.get(v)->node = n.id;
            panels[n.id] = v;
        }
}

uint32_t Editor::add_module_at(Ctx cx, const ModuleDesc &d, int x, int y)
{
    Patch &patch = engine.patch();
    if (collides(patch, d, x, y))
        return 0;
    const uint32_t id = patch.add_node(d, (float)x, (float)y);
    if (engine.compile() != Err::Ok)
    {
        patch.remove_node(id);
        engine.compile();
        say(cx, std::string(d.name) + ": the library refused its default settings; not added");
        return 0;
    }
    sync_panels(cx);
    refresh_rack(cx);
    say(cx,
        "Added " + std::string(d.name)
            + ".      Drag its title to move it; right-click the title to delete it.");
    return id;
}

uint32_t Editor::add_module(Ctx cx, const ModuleDesc &d)
{
    const RackViewport vp = viewport_of(cx, *this);
    const int          x0 = (int)std::ceil(vp.x / kHp), y0 = (int)std::lround(vp.y / kHp);
    const int          rows = std::max(1, (int)(vp.h / kPanelHeight));
    const Slot     s = free_slot(engine.patch(), d, x0, y0, std::max(1, (int)(vp.w / kHp)), rows);
    const uint32_t id = add_module_at(cx, d, s.x, s.y);
    if (id)
    { // keep the new panel in sight
        const Rect r = node_rect(d, (float)s.x, (float)s.y);
        if (r.right() > vp.x + vp.w || r.bottom() > vp.y + vp.h)
            if (RackView *rv = rack.get(cx))
                rv->scroll.to(cx, std::max(0.f, r.right() - vp.w + kHp), std::max(0.f, r.y - kHp));
    }
    return id;
}

void Editor::remove_module(Ctx cx, uint32_t node)
{
    const Node *n = engine.patch().find(node);
    if (!n)
        return;
    const std::string name(n->desc->name);
    engine.patch().remove_node(node);
    if (hover_node == node)
    {
        hover_node = 0;
        hover_text.clear();
    }
    engine.compile();
    sync_panels(cx);
    sync_cables(cx); // its cables went with it
    say(cx, "Deleted " + name + ".");
}

bool Editor::move_module(Ctx cx, uint32_t node, int x, int y)
{
    Node *n = engine.patch().find(node);
    if (!n)
        return false;
    if ((float)x == n->x && (float)y == n->y)
        return true;
    if (collides(engine.patch(), *n->desc, x, y, node))
        return false;
    n->x = (float)x;
    n->y = (float)y;
    refresh_rack(cx);
    refresh_cables_of(cx, node); // its cables follow
    return true;
}

void Editor::set_param(Ctx cx, uint32_t node, size_t param, float value)
{
    const Node *n = engine.patch().find(node);
    if (!n || param >= n->params.size())
        return;
    engine.set_param(node, param, value);
    auto it = panels.find(node);
    if (it != panels.end())
        it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    say(cx, describe(*n->desc, n->desc->params[param], n->params[param]));
    if (hover_node == node
        && hover_item == (int)param) // the control under the pointer: its new value
        hover_text = describe_item(node, hover_item);
}

bool Editor::load_file(Ctx cx, const std::string &path, std::string *error)
{
    std::ifstream f(path);
    if (!f)
    {
        *error = path + ": cannot be read";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const LoadResult r = load_patch(ss.str(), engine.patch());
    if (!r.ok)
    {
        *error = path + ": " + r.error;
        return false;
    }
    const int moved = resolve_overlaps(engine.patch());
    if (engine.compile() != Err::Ok)
    {
        *error
            = path + ": module " + std::to_string(engine.failed_node()) + " refused its settings";
        return false;
    }
    const size_t slash = path.find_last_of('/');
    patch_name = slash == std::string::npos ? path : path.substr(slash + 1);
    sync_panels(cx);
    sync_cables(cx);
    std::string msg = "Opened " + patch_name + ".";
    if (moved)
        msg += "  " + std::to_string(moved) + " panel(s) moved apart: they overlapped.";
    if (!r.warnings.empty())
        msg += "  " + std::to_string(r.warnings.size())
            + " warning(s), the first: " + r.warnings[0];
    say(cx, msg);
    return true;
}

// =====================================================================================================

Entity<Editor> open_rack(App app, const Options &opt, std::string *error)
{
    auto    ed = app.new_entity<Editor>();
    Editor *e = app.get(ed);
    e->self = ed;
    e->status = "Add modules from the list on the left.";

    e->hint_line = app.new_view<HintLine>();
    app.get(e->hint_line)->ed = ed;
    e->toolbar = app.new_view<Toolbar>();
    app.get(e->toolbar)->ed = ed;
    e->rack = app.new_view<RackView>();
    app.get(e->rack)->ed = ed;
    app.get(e->rack)->scroll = app.new_scroll();
    e->drag_view = app.new_view<DragView>();
    app.get(e->drag_view)->ed = ed;
    e->chip = app.new_view<PointerChip>();
    app.get(e->chip)->ed = ed;
    // Delete or Backspace removes the selected cable, unless something focused (the search box)
    // takes the key
    app.bind_key("delete", "rack::delete_cable");
    app.bind_key("backspace", "rack::delete_cable");
    app.on_global_action("rack::delete_cable",
        [ed](Ctx cx)
        {
            if (Editor *e2 = ed.get(cx))
                e2->delete_selected(cx);
        });
    e->browser = app.new_view<Browser>();
    Browser *b = app.get(e->browser);
    b->ed = ed;
    b->search = app.text_input()
                    .owner(e->browser)
                    .placeholder("Search modules")
                    .width(st::kBrowserW - 2 * st::kBrowserPad)
                    .font_size(13)
                    .bg(st::kSurfaceHi)
                    .border_color(st::kBorder)
                    .focus_border_color(st::kAccent)
                    .text_color(st::kText)
                    .placeholder_color(st::kDim)
                    .on_change([](Browser &, Ctx cx) { cx.notify(); })
                    .build();

    if (!opt.patch_path.empty() && !e->load_file(app.ctx(), opt.patch_path, error))
        return { };

    auto root = app.new_view<Root>();
    app.get(root)->ed = ed;
    gp_window_options w { };
    w.title = "PulseRack";
    w.width = (int)st::kWindowW;
    w.height = (int)st::kWindowH;
    app.open_window(w, root);
    if (opt.audio) // a failure is shown in the toolbar and the hint line; the window works without
        e->start_audio(app.ctx(ed));
    return ed;
}

gp_point rack_origin() { return { st::kBrowserW, st::kToolbarH }; }

RackViewport rack_viewport(Ctx cx, Entity<Editor> ed)
{
    Editor *e = ed.get(cx);
    return e ? viewport_of(cx, *e) : RackViewport { };
}

gp_point control_center(Ctx cx, Entity<Editor> ed, uint32_t node, int param)
{
    Editor     *e = ed.get(cx);
    const Node *n = e ? e->engine.patch().find(node) : nullptr;
    if (!n)
        return { -1, -1 };
    const RackViewport vp = viewport_of(cx, *e);
    const gp_point     o = rack_origin();
    const float        px = o.x + n->x * kHp - vp.x, py = o.y + n->y * kHp - vp.y;
    if (param < 0)
        return { px + n->desc->width_hp * kHp / 2, py + kTitleHeight / 2 };
    const PanelLayout &L = panel_layout(*n->desc);
    const Rect        &r = L.params[(size_t)param];
    if (n->desc->params[(size_t)param].control == Control::Knob)
        return { px + kPanelBorder + r.x + r.w / 2, py + kPanelBorder + r.y + kKnobSize / 2 };
    return { px + kPanelBorder + r.x + r.w / 2, py + kPanelBorder + r.y + r.h / 2 };
}

gp_point port_center(Ctx cx, Entity<Editor> ed, uint32_t node, size_t port)
{
    Editor     *e = ed.get(cx);
    const Node *n = e ? e->engine.patch().find(node) : nullptr;
    if (!n || port >= n->desc->ports.size())
        return { -1, -1 };
    const RackViewport vp = viewport_of(cx, *e);
    const gp_point     o = rack_origin();
    const Rect        &j = panel_layout(*n->desc).jacks[port];
    return { o.x + n->x * kHp - vp.x + kPanelBorder + j.x + j.w / 2,
        o.y + n->y * kHp - vp.y + kPanelBorder + j.y + j.h / 2 };
}

} // namespace rack::ui
