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
    constexpr float kPanelBorder = 1; // absolute children are placed inside the panel's border

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
            .child(div()
                    .flex_row()
                    .items_center()
                    .gap(6)
                    .child(div().w(8).h(8).rounded_full().bg(st::kLedOff))
                    .child(label("no audio yet", 12, st::kDim)));
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
            .border_left(3)
            .border_color(cc.chip)
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
                { self.ed.get(cx)->hover(cx, self.node, Editor::kHoverPort - (int)i, inside); });
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

// =====================================================================================================
// The rack: a scrolling surface with the panels at their places; drag empty space to pan.

struct RackView
{
    Entity<Editor> ed;
    Scroll         scroll;
    bool           panning = false;
    gp_point       pan_p0 { }, pan_off0 { };
    uint32_t       menu_node = 0; // the panel whose menu is open

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
                          if (self.menu_node)
                          {
                              self.menu_node = 0;
                              cx.notify();
                          }
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
            c.notify();
        });
}

// =====================================================================================================
// The window

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
            .child(e->hint_line.cached());
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
            + "      cables arrive in milestone 3";
    }
    return { };
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
    refresh_rack(cx);
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
    refresh_rack(cx);
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
    if (!n)
        return { -1, -1 };
    const RackViewport vp = viewport_of(cx, *e);
    const gp_point     o = rack_origin();
    const Rect        &j = panel_layout(*n->desc).jacks[port];
    return { o.x + n->x * kHp - vp.x + kPanelBorder + j.x + j.w / 2,
        o.y + n->y * kHp - vp.y + kPanelBorder + j.y + j.h / 2 };
}

} // namespace rack::ui
