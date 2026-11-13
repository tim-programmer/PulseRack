#include "ui/rack_ui.h"

#include "engine/model/patchfile.h"
#include "engine/model/registry.h"
#include "engine/rt/modules_common.h"
#include "ui/dialog.h"
#include "ui/files.h"
#include "ui/geometry.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <filesystem>
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

    const Color kBlack = rgb(0x000000), kWhite = rgb(0xffffff);

    // The Device box of a MIDI In or MIDI CC panel: under its controls, in the panel's pixels
    // (inside its border). A click on it opens the device menu (RackView).
    Rect device_box(const PanelLayout &L)
    {
        return { kPad, L.controls_bottom + 8, L.width - 2 * kPanelBorder - 2 * kPad, 32 };
    }

    // A light that is on glows: a soft halo of its own colour.
    Element glow(Element e, Color c, float blur = 6, float alpha = 0.75f)
    {
        return std::move(e).shadow(0, 0, blur, with_alpha(c, alpha));
    }

    std::string note_name(int n)
    {
        static const char *names[12]
            = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
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
            how = "drag up or down (Shift: fine), wheel, double-click: default, right-click: menu";
            break;
        case Control::Selector:
            how = "click < or >, or use the wheel, right-click: menu";
            break;
        case Control::Switch:
        case Control::Button:
            how = "click to switch, right-click: menu";
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
    Entity<Editor>                    ed;
    template <class F> static Element button(const char *text, float w, F &&f, bool enabled = true)
    {
        return div()
            .w(w)
            .h(st::kToolBtnH)
            .flex_shrink_0()
            .rounded(4)
            .flex_row()
            .items_center()
            .justify_center()
            .cursor_pointer()
            .bg_gradient(180, { stop(rgb(0x343846), 0), stop(st::kSurfaceHi, 1) })
            .hover_bg(st::kBorder)
            .active_bg(st::kWell)
            .border(1)
            .border_color(st::kBorder)
            .child(label(text, 12, enabled ? st::kText : st::kDim))
            .on_click(std::forward<F>(f));
    }
    Element render(Ctx cx)
    {
        Editor      *e = ed.get(cx);
        const size_t n = e ? e->engine.patch().nodes().size() : 0;
        const bool   dirty = e && e->dirty;
        return div()
            .w_full()
            .h(st::kToolbarH)
            .flex_row()
            .items_center()
            .gap(st::kToolGap)
            .px(12)
            .bg_gradient(180, { stop(rgb(0x282b35), 0), stop(st::kSurface, 1) })
            .border_bottom(1)
            .border_color(st::kBorder)
            // the mark: three bars of a pulse, then the name (together kAppNameW wide)
            .child(div()
                    .w(st::kAppNameW)
                    .flex_shrink_0()
                    .flex_row()
                    .items_center()
                    .gap(1.5f)
                    .child(div().w(2).h(6).rounded(1).bg(st::kAccent))
                    .child(div().w(2).h(13).rounded(1).bg(st::kAccentHi))
                    .child(div().w(2).h(8).rounded(1).bg(st::kAccent))
                    .child(div().w(2))
                    .child(label("PulseRack", 13, st::kText).font_bold()))
            .child(button("New", st::kNewW,
                [](Toolbar &self, Ctx cx) { self.ed.get(cx)->request(cx, Editor::Then::New); }))
            .child(button("Open  v", st::kOpenW,
                [](Toolbar &self, Ctx cx)
                {
                    Editor *e = self.ed.get(cx);
                    e->file_menu_open = !e->file_menu_open;
                    e->overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
                }))
            .child(button(
                "Save", st::kSaveW, [](Toolbar &self, Ctx cx) { self.ed.get(cx)->save(cx); }))
            .child(button("Save as", st::kSaveAsW,
                [](Toolbar &self, Ctx cx) { self.ed.get(cx)->save_as(cx); }))
            .child(div().w(8 - st::kToolGap))
            .child(button(
                "Undo", st::kUndoW, [](Toolbar &self, Ctx cx) { self.ed.get(cx)->undo(cx); },
                e && !e->undo_stack.empty()))
            .child(button(
                "Redo", st::kRedoW, [](Toolbar &self, Ctx cx) { self.ed.get(cx)->redo(cx); },
                e && !e->redo_stack.empty()))
            .child(div().w(8))
            // the name, with a mark while there are unsaved changes
            .child(dirty
                    ? glow(div().w(6).h(6).rounded_full().bg(st::kAccent), st::kAccent, 5, 0.6f)
                    : div())
            .child(label(e ? e->patch_name : std::string(), 13, dirty ? st::kText : st::kSubtext))
            .child(label(std::to_string(n) + (n == 1 ? " module" : " modules"), 12, st::kDim))
            .child(div().flex_1())
            .child(e && e->shown_dropouts
                    ? label(std::to_string(e->shown_dropouts)
                              + (e->shown_dropouts == 1 ? " dropout" : " dropouts"),
                          12, st::kDanger)
                    : div())
            .child(div()
                    .flex_row()
                    .items_center()
                    .h(st::kToolBtnH)
                    .px(6)
                    .rounded(4)
                    .cursor_pointer()
                    .hover_bg(st::kSurfaceHi)
                    .child(label((e ? e->audio_status() + e->midi_status() : std::string()) + "  v",
                        12, st::kDim))
                    .on_click([](Toolbar &self, Ctx cx) { self.ed.get(cx)->open_device_menu(cx); }))
            .child(div()
                    .w(st::kAudioW)
                    .justify_center()
                    .h(24)
                    .gap(6)
                    .rounded(4)
                    .flex_row()
                    .items_center()
                    .cursor_pointer()
                    .bg_gradient(180, { stop(rgb(0x343846), 0), stop(st::kSurfaceHi, 1) })
                    .hover_bg(st::kBorder)
                    .active_bg(st::kWell)
                    .border(1)
                    .border_color(
                        e && e->audio_running() ? with_alpha(st::kLedOn, 0.45f) : st::kBorder)
                    .child(e && e->audio_running()
                            ? glow(div().w(8).h(8).rounded_full().bg(st::kLedOn), st::kLedOn)
                            : div()
                                  .w(8)
                                  .h(8)
                                  .rounded_full()
                                  .bg(st::kLedOff)
                                  .border(1)
                                  .border_color(st::kBorder))
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
            .bg_gradient(90, { stop(with_alpha(cc.chip, 0.10f), 0), stop(st::kSurfaceHi, 0.45f) })
            .hover_bg(rgb(0x343846))
            .border_left(3)
            .border_color(cc.chip)
            .shadow(0, 1, 3, with_alpha(kBlack, 0.3f))
            // drag the card onto the rack to add the module there (the Add button adds it near the
            // view)
            .on_mouse_down(
                [d](Browser &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->module_drag_begin(cx, *d, ev.position);
                })
            // (gpuic sends drags and releases for every button: these are the left one's)
            .on_drag(
                [](Browser &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->module_drag_move(cx, ev.position);
                })
            .on_mouse_up(
                [](Browser &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->module_drag_end(cx, ev.position);
                })
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
                            .bg_gradient(180, { stop(st::kAccentHi, 0), stop(st::kAccent, 1) })
                            .hover_bg(st::kAccentHi)
                            .active_bg(rgb(0x6f9be6))
                            .shadow(0, 1, 2, with_alpha(kBlack, 0.35f))
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
void open_control_menu(Ctx cx, Editor &e, uint32_t node, size_t param); // a control's (MIDI learn)
void open_device_menu_of(Ctx cx, Editor &e, uint32_t node); // a MIDI module's Device box

// =====================================================================================================
// A panel: one module, a cached view of its own, so turning a knob rebuilds this panel only.

struct PanelView
{
    Entity<Editor> ed;
    uint32_t       node = 0;

    int   active = -1; // the control being dragged
    float travel = 0, last_y = 0;

    int                page = 0;         // the step grid's page (16 of the 64 steps)
    int                step_active = -1; // the step whose note bar is being dragged
    int                step_note0 = 0;
    float              step_y0 = 0;
    int                playhead_ = -1;    // copied from the editor at render
    bool               selected_ = false; // copied at render: the selected module
    int                strip_note = -1;   // the piano key held down with the mouse (a note), or -1
    int                strip_base = 60;   // copied at render: the note of the piano's lowest key
    std::vector<int>   strip_held_;       // copied at render: the notes the computer keys hold
    Editor::Led        led_;              // copied from the editor at render: what the LED shows
    std::array<int, 2> meter_ { };        // and the output level (Audio Output panels)

    // a press on the title or the face: the menu (right button) or a move
    void press_face(Ctx cx, const gp_mouse_event &ev)
    {
        Editor *e = ed.get(cx);
        if (!e || !find(cx))
            return;
        if (ev.button == GP_MOUSE_RIGHT)
            open_menu(cx, *e, node);
        else if (ev.button == GP_MOUSE_LEFT)
            e->panel_drag_begin(cx, node, ev.position);
        else if (ev.button == GP_MOUSE_MIDDLE) // the middle button drags the rack, over a panel too
            e->pan_begin(cx, ev.position, ev.button);
    }
    // the moves and the release after it: the left button's move the panel, the middle one's the
    // rack (gpuic sends them for every button, each with its own capture)
    void drag_face(Ctx cx, const gp_mouse_event &ev)
    {
        Editor *e = ed.get(cx);
        if (e && ev.button == GP_MOUSE_LEFT)
            e->panel_drag_move(cx, ev.position);
        else if (e)
            e->pan_move(cx, ev.position, ev.button);
    }
    void release_face(Ctx cx, const gp_mouse_event &ev)
    {
        Editor *e = ed.get(cx);
        if (e && ev.button == GP_MOUSE_LEFT)
            e->panel_drag_end(cx, ev.position);
        else if (e)
            e->pan_end(ev.button);
    }

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
        if (n && ev.button == GP_MOUSE_RIGHT)
            open_control_menu(cx, *e, node, i);
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
        if (!n || active != (int)i || ev.button != GP_MOUSE_LEFT)
            return;
        const ParamDesc &p = n->desc->params[i];
        const float      v
            = drag_value(p, travel, last_y - ev.position.y, (ev.mods & GP_MOD_SHIFT) != 0, &travel);
        last_y = ev.position.y;
        if (v != n->params[i])
            ed.get(cx)->set_param(cx, node, i, v);
    }
    void release(Ctx cx, const gp_mouse_event &ev)
    {
        if (ev.button != GP_MOUSE_LEFT)
            return;
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
            .shadow(0, 0, 5,
                led_.dropped ? with_alpha(st::kDanger, 0.8f)
                             : with_alpha(st::kLedOn, 0.7f * kAlpha[led_.level]))
            .bg(led_.dropped     ? st::kDanger
                    : led_.level ? with_alpha(st::kLedOn, kAlpha[led_.level])
                                 : st::kLedOff);
    }
    // ---- the oscilloscope's screen
    Element scope_element(
        const Node &n, const PanelLayout &L, const ScopeFrame *f, bool running) const
    {
        const Rect  r = scope_display(L);
        const Color green = rgb(0x8ee59a), amber = rgb(0xf9c86b), faint = rgb(0x8ee59a);
        Element     w = div().absolute().left(0).top(0);
        w.child(div()
                .absolute()
                .left(r.x)
                .top(r.y)
                .w(r.w)
                .h(r.h)
                .rounded(3)
                .bg(rgb(0x0b110d))
                .border(1)
                .border_color(st::kKnobEdge));
        const float cy = r.y + r.h / 2, half = r.h / 2 - 3;
        auto        y_of = [&](float v) { return cy - std::clamp(v, -1.04f, 1.04f) * half; };
        // the graticule: a centre line, half-scale lines, ten divisions across
        w.child(
            div().absolute().left(r.x + 1).top(cy).w(r.w - 2).h(1).bg(with_alpha(faint, 0.22f)));
        for (float v : { -0.5f, 0.5f })
            w.child(div().absolute().left(r.x + 1).top(y_of(v)).w(r.w - 2).h(1).bg(
                with_alpha(faint, 0.08f)));
        for (int k = 1; k < 10; k++)
            w.child(div()
                    .absolute()
                    .left(r.x + r.w * (float)k / 10)
                    .top(r.y + 1)
                    .w(1)
                    .h(r.h - 2)
                    .bg(with_alpha(faint, 0.08f)));
        const ModuleDesc &d = *n.desc;
        auto param_of = [&](const char *id) { return n.params[(size_t)find_param(d, id)]; };
        if ((int)std::lround(param_of("trigger")) != 0) // the trigger level, on A's scale
            w.child(div()
                    .absolute()
                    .left(r.x + 1)
                    .top(y_of(param_of("level") * param_of("scale_a")) - 1)
                    .w(8)
                    .h(2)
                    .bg(amber));
        std::string corner;
        if (f && f->seq)
        {
            for (int ch = 1; ch >= 0; ch--) // A over B
            {
                if (!f->has[ch])
                    continue;
                std::vector<gp_point> pts; // through each column's lowest and highest value
                pts.reserve(2 * kScopeColumns);
                for (int c = 0; c < kScopeColumns; c++)
                {
                    const float x = r.x + 2 + (r.w - 4) * ((float)c + 0.5f) / kScopeColumns;
                    const float a = y_of(f->lo[ch][c]), b = y_of(f->hi[ch][c]);
                    pts.push_back({ x, c % 2 ? a : b });
                    pts.push_back({ x, c % 2 ? b : a });
                }
                w.child(polyline(pts).stroke_width(1.5f).stroke_color(ch ? amber : green));
            }
            char buf[48];
            std::snprintf(buf, sizeof buf, f->window_ms >= 100 ? "%.0f ms%s" : "%.1f ms%s",
                (double)f->window_ms, f->triggered ? "" : "  auto");
            corner = buf;
        }
        else
            corner = running ? "waiting" : "audio off";
        w.child(div()
                .absolute()
                .left(r.x)
                .top(r.bottom() - 15)
                .w(r.w - 6)
                .flex_row()
                .justify_end()
                .child(label(corner, 9, with_alpha(faint, 0.7f))));
        return w;
    }

    // ---- the Keyboard panel's piano
    int strip_note_at(const gp_mouse_event &ev, const PanelLayout &L) const
    { // ev.local is relative to the strip's area
        const KeyStrip k = keyboard_strip(L);
        const int      s = k.key_at(k.area.x + ev.local.x, k.area.y + ev.local.y);
        return s < 0 ? -1 : strip_base + s;
    }
    void strip_press(Ctx cx, const gp_mouse_event &ev)
    {
        const Node *n = find(cx);
        if (!n || ev.button != GP_MOUSE_LEFT)
            return;
        strip_note = strip_note_at(ev, panel_layout(*n->desc));
        if (strip_note >= 0)
            ed.get(cx)->strip_key(cx, node, strip_note, true);
        cx.notify();
    }
    void strip_drag(Ctx cx, const gp_mouse_event &ev)
    { // gliding across the keys: each note is released before the next sounds
        const Node *n = find(cx);
        if (!n || ev.button != GP_MOUSE_LEFT)
            return;
        const int now = strip_note_at(ev, panel_layout(*n->desc));
        if (now == strip_note)
            return;
        if (strip_note >= 0)
            ed.get(cx)->strip_key(cx, node, strip_note, false);
        strip_note = now;
        if (strip_note >= 0)
            ed.get(cx)->strip_key(cx, node, strip_note, true);
        cx.notify();
    }
    void strip_release(Ctx cx, const gp_mouse_event &ev)
    {
        if (ev.button != GP_MOUSE_LEFT)
            return;
        if (strip_note >= 0)
            ed.get(cx)->strip_key(cx, node, strip_note, false);
        strip_note = -1;
        cx.notify();
    }
    Element strip_element(const PanelLayout &L, Color accent) const
    {
        const KeyStrip k = keyboard_strip(L);
        Element        w = div().absolute().left(0).top(0);
        auto           lit = [&](int note)
        {
            return note == strip_note
                || std::find(strip_held_.begin(), strip_held_.end(), note) != strip_held_.end();
        };
        for (int pass = 0; pass < 2; pass++) // the white keys, then the black ones over them
            for (int s = 0; s < kStripSemitones; s++)
            {
                if (k.is_black(s) != (pass == 1))
                    continue;
                const Rect r = k.key(s);
                const int  note = strip_base + s;
                Element    key = div()
                                     .absolute()
                                     .left(r.x)
                                     .top(r.y)
                                     .w(r.w)
                                     .h(r.h)
                                     .rounded(2)
                                     .border(1)
                                     .border_color(st::kKnobEdge)
                                     .bg(lit(note)  ? accent
                                             : pass ? rgb(0x202228)
                                                    : rgb(0xe6e7ee));
                if (s % 12 == 0) // the Cs are named
                    key.flex_row().items_center().justify_end().pr(4).child(
                        label(note_name(note), 8, rgb(0x55596a)));
                w.child(std::move(key));
            }
        // one element over the keys takes the press, the glide and the release
        w.child(div()
                .absolute()
                .left(k.area.x)
                .top(k.area.y)
                .w(k.area.w)
                .h(k.area.h)
                .cursor_pointer()
                .on_mouse_down([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.strip_press(cx, ev); })
                .on_drag([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.strip_drag(cx, ev); })
                .on_mouse_up([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.strip_release(cx, ev); })
                .on_hover([](PanelView &self, Ctx cx, bool inside)
                    { self.ed.get(cx)->hover(cx, self.node, Editor::kHoverStrip, inside); }));
        return w;
    }

    // ---- the step grid
    StepData step_of(Ctx cx, size_t i) const
    {
        const Node *n = find(cx);
        return n && i < n->steps.size() ? n->steps[i] : StepData { };
    }
    void    step_set(Ctx cx, size_t i, StepData s) { ed.get(cx)->set_step(cx, node, i, s); }
    Element step_grid_element(const Node &n, const PanelLayout &L, Color accent) const
    {
        const StepGrid g = step_grid(L);
        const int      li = find_param(*n.desc, "length");
        const int      length
            = li >= 0 ? (int)std::lround(n.params[(size_t)li]) : kGridColumns * kGridPages;
        Element w = div().absolute().left(0).top(0);
        for (int p = 0; p < kGridPages; p++)
        {
            const Rect r = g.tab(p);
            const bool on = p == page;
            w.child(div()
                    .absolute()
                    .left(r.x)
                    .top(r.y)
                    .w(r.w)
                    .h(r.h)
                    .rounded(3)
                    .bg(on ? with_alpha(accent, 0.35f) : st::kWell)
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .cursor_pointer()
                    .child(label(std::to_string(p * kGridColumns + 1) + "-"
                            + std::to_string((p + 1) * kGridColumns),
                        9, on ? st::kText : st::kSubtext))
                    .on_mouse_down([](PanelView &) { })
                    .on_click(
                        [p](PanelView &self, Ctx cx)
                        {
                            self.page = p;
                            cx.notify();
                        }));
        }
        for (int c = 0; c < kGridColumns; c++)
        {
            const size_t    i = (size_t)(page * kGridColumns + c);
            const StepData &s = n.steps[i];
            const bool      in_loop = (int)i < length;
            const Rect      bar = g.note_bar(c), on = g.on_pad(c), tie = g.tie_pad(c);
            if ((int)i == playhead_) // the step being played
                w.child(div()
                        .absolute()
                        .left(bar.x - 1)
                        .top(bar.y - 2)
                        .w(bar.w + 2)
                        .h(tie.bottom() - bar.y + 4)
                        .rounded(3)
                        .bg(with_alpha(st::kKnobPointer, 0.25f)));
            const float fill = note_fill(s.note) * bar.h;
            const float a = in_loop ? 1.f : 0.35f; // steps past the sequence's length are dimmed
            w.child(div()
                    .absolute()
                    .left(bar.x)
                    .top(bar.y)
                    .w(bar.w)
                    .h(bar.h)
                    .rounded(2)
                    .bg(with_alpha(st::kWell, a))
                    .child(div()
                            .absolute()
                            .left(0)
                            .top(bar.h - fill)
                            .w(bar.w)
                            .h(fill)
                            .rounded(2)
                            .bg(with_alpha(accent, (s.velocity ? 0.8f : 0.2f) * a)))
                    .child(div()
                            .absolute()
                            .left(0)
                            .top(std::max(0.f, bar.h - fill - 1))
                            .w(bar.w)
                            .h(2)
                            .bg(with_alpha(st::kKnobPointer, s.velocity ? a : 0.4f * a)))
                    .cursor_pointer()
                    .on_mouse_down(
                        [i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                        {
                            if (ev.button != GP_MOUSE_LEFT)
                                return;
                            const StepData st = self.step_of(cx, i);
                            if (ev.click_count >= 2) // double-click: on / rest
                            {
                                StepData t = st;
                                t.velocity = st.velocity ? 0 : 100;
                                self.step_set(cx, i, t);
                                return;
                            }
                            self.step_active = (int)i;
                            self.step_note0 = st.note;
                            self.step_y0 = ev.position.y;
                        })
                    .on_drag(
                        [i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                        {
                            if (self.step_active != (int)i || ev.button != GP_MOUSE_LEFT)
                                return;
                            StepData t = self.step_of(cx, i);
                            t.note = (uint8_t)std::clamp(self.step_note0
                                    + (int)std::lround(
                                        (self.step_y0 - ev.position.y) / kGridPxPerSemitone),
                                0, 127);
                            self.step_set(cx, i, t);
                        })
                    .on_mouse_up(
                        [](PanelView &self, Ctx, const gp_mouse_event &ev)
                        {
                            if (ev.button == GP_MOUSE_LEFT)
                                self.step_active = -1;
                        })
                    .on_wheel(
                        [i](PanelView &self, Ctx cx, const gp_wheel_event &ev)
                        {
                            const bool octave
                                = ev.dy == 0 && ev.dx != 0; // Shift+wheel arrives as dx
                            const float dv = octave ? ev.dx : ev.dy;
                            int         by = dv > 0 ? -1 : dv < 0 ? 1 : 0;
                            StepData    t = self.step_of(cx, i);
                            t.note
                                = (uint8_t)std::clamp((int)t.note + by * (octave ? 12 : 1), 0, 127);
                            self.step_set(cx, i, t);
                        })
                    .on_hover(
                        [i](PanelView &self, Ctx cx, bool inside)
                        {
                            self.ed.get(cx)->hover(
                                cx, self.node, Editor::kHoverStep - (int)i, inside);
                        }));
            auto pad = [&](const Rect &r, bool lit, Color col)
            {
                return div()
                    .absolute()
                    .left(r.x)
                    .top(r.y)
                    .w(r.w)
                    .h(r.h)
                    .rounded(2)
                    .bg(lit ? with_alpha(col, a) : with_alpha(st::kWell, a))
                    .border(1)
                    .border_color(st::kKnobEdge)
                    .cursor_pointer()
                    .on_mouse_down([](PanelView &) { })
                    .on_hover(
                        [i](PanelView &self, Ctx cx, bool inside)
                        {
                            self.ed.get(cx)->hover(
                                cx, self.node, Editor::kHoverStep - (int)i, inside);
                        });
            };
            w.child(pad(on, s.velocity != 0, st::kLedOn)
                    .on_click(
                        [i](PanelView &self, Ctx cx)
                        {
                            StepData t = self.step_of(cx, i);
                            t.velocity = t.velocity ? 0 : 100;
                            self.step_set(cx, i, t);
                        }));
            w.child(pad(tie, s.tie != 0, rgb(0xf9e2af))
                    .on_click(
                        [i](PanelView &self, Ctx cx)
                        {
                            StepData t = self.step_of(cx, i);
                            t.tie = t.tie ? 0 : 1;
                            self.step_set(cx, i, t);
                        }));
        }
        return w;
    }

    // A MIDI module's Device box: the input it listens to; a click opens the menu to choose one.
    // Its caption says when that input cannot be heard (it is not plugged in, or is switched off
    // in the device menu).
    Element device_element(Ctx cx, const Node &n, const PanelLayout &L, Color chip)
    {
        const Rect  r = device_box(L);
        Editor     *e = ed.get(cx);
        const char *caption = "DEVICE";
        bool        heard = true;
        if (e && !n.device.empty())
        {
            const auto in = [&](const std::vector<std::string> &v)
            { return std::find(v.begin(), v.end(), n.device) != v.end(); };
            if (!in(e->midi_available))
                caption = "NOT CONNECTED", heard = false;
            else if (!in(e->settings.midi_inputs))
                caption = "SWITCHED OFF", heard = false;
        }
        return div()
            .absolute()
            .left(r.x)
            .top(r.y)
            .w(r.w)
            .h(r.h)
            .rounded(3)
            .bg(st::kWell)
            .border(1)
            .border_color(with_alpha(chip, 0.5f))
            .hover_bg(rgb(0x1a1c22))
            .px(5)
            .flex_col()
            .justify_center()
            .gap(1)
            .overflow_hidden()
            .cursor_pointer()
            .child(label(caption, 8, heard ? st::kDim : rgb(0xf9e2af)).whitespace_pre())
            .child(label(n.device.empty() ? std::string("Any device") : n.device, 10,
                heard ? st::kText : st::kSubtext)
                    .whitespace_pre())
            // the box's press, not the panel's
            .on_mouse_down(
                [](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                {
                    Editor *e = self.ed.get(cx);
                    if (e && ev.button != GP_MOUSE_MIDDLE) // (the middle button: nothing, as on a
                        open_device_menu_of(cx, *e, self.node); // control)
                })
            .on_hover([](PanelView &self, Ctx cx, bool inside)
                { self.ed.get(cx)->hover(cx, self.node, Editor::kHoverDevice, inside); });
    }

    // The output level as two columns of segments: green, amber near the top, red at the top.
    Element meters(const PanelLayout &L) const
    {
        constexpr int kSegs = 20;
        const float   top = L.controls_bottom + 12, h = std::max(20.f, L.jacks_top - top - 12);
        const float   bw = 8, x0 = L.width / 2 - bw - 2;
        const float   pitch = h / kSegs, sh = std::max(1.f, pitch - 1.5f);
        Element       m = div().absolute().left(x0).top(top).w(2 * bw + 4).h(h);
        for (int ch = 0; ch < 2; ch++)
        {
            const float frac = (float)meter_[(size_t)ch] / 40.f;
            const int   lit = (int)std::lround(frac * kSegs);
            Element     col = div()
                                  .absolute()
                                  .left((float)ch * (bw + 4) - 1)
                                  .top(-1)
                                  .w(bw + 2)
                                  .h(h + 1)
                                  .rounded(2)
                                  .bg(st::kWell);
            for (int k = 0; k < kSegs; k++) // k = 0 is the lowest segment
            {
                const Color c = k >= kSegs - 1 ? st::kDanger
                    : k >= kSegs - 4           ? rgb(0xf9e2af)
                                               : st::kLedOn;
                col.child(div()
                        .absolute()
                        .left(1)
                        .top(1 + h - (float)(k + 1) * pitch)
                        .w(bw)
                        .h(sh)
                        .rounded(1)
                        .bg(k < lit ? c : with_alpha(c, 0.10f)));
            }
            m.child(std::move(col));
        }
        return m;
    }

    // A knob: a ring that shows the value as an arc (from `t0`, the travel of zero for a knob that
    // goes both ways, else from the start), a face inside it, and a pointer turned about the
    // centre.
    Element knob(float t, float t0, Color accent) const
    {
        constexpr float kRing = 3; // the arc's width
        const Color     track = with_alpha(kBlack, 0.45f), none = with_alpha(kBlack, 0);
        // the conic gradient starts at 7 o'clock and the knob's travel is three quarters of a turn
        const float a = 0.75f * std::min(t, t0), b = std::max(0.75f * std::max(t, t0), a + 0.004f);
        const float face = kKnobSize - 2 * kRing;
        return div()
            .w(kKnobSize)
            .h(kKnobSize)
            .flex_shrink_0()
            .rounded_full()
            .bg_conic(0.5f, 0.5f, 225,
                { stop(track, 0), stop(track, a), stop(accent, a), stop(accent, b), stop(track, b),
                    stop(track, 0.75f), stop(none, 0.75f), stop(none, 1) })
            .child(div()
                    .absolute()
                    .left(kRing)
                    .top(kRing)
                    .w(face)
                    .h(face)
                    .rounded_full()
                    .bg_gradient(180, { stop(rgb(0x4a4e5e), 0), stop(st::kKnobFace, 0.75f) })
                    .border(1)
                    .border_color(st::kKnobEdge)
                    .shadow(0, 2, 4, with_alpha(kBlack, 0.45f)))
            // a full-size overlay turned about the face's centre carries the pointer
            .child(div()
                    .absolute()
                    .left(0)
                    .top(0)
                    .w(kKnobSize)
                    .h(kKnobSize)
                    .rotate(-135 + 270 * t)
                    .child(div()
                            .absolute()
                            .left(kKnobSize / 2 - 1.25f)
                            .top(kRing + 2)
                            .w(2.5f)
                            .h(8)
                            .rounded(1.25f)
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
                    .bg_gradient(180, { stop(kWhite, 0), stop(rgb(0xb9bcc9), 1) })
                    .border(1)
                    .border_color(st::kKnobEdge)
                    .shadow(0, 1, 3, with_alpha(kBlack, 0.5f))
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .child(div().w(kW - 8).h(1).bg(with_alpha(kBlack, 0.45f))));
    }
    Element led(bool on, Color accent) const
    {
        Element e = div()
                        .w(8)
                        .h(8)
                        .flex_shrink_0()
                        .rounded_full()
                        .bg(on ? accent : st::kLedOff)
                        .border(1)
                        .border_color(st::kKnobEdge);
        return on ? glow(std::move(e), accent) : e;
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
            cell.child(p.control == Control::Knob
                        ? knob(t, p.min < 0 && p.max > 0 ? to_travel(p, 0) : 0, accent)
                        : slider(t, accent))
                .child(std::move(name))
                .child(std::move(val))
                .on_mouse_down([i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.press(cx, i, ev); })
                .on_drag([i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.drag(cx, i, ev); })
                .on_mouse_up(
                    [](PanelView &self, Ctx cx, const gp_mouse_event &ev) { self.release(cx, ev); })
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
                // a control's press, not the panel's (the right button: MIDI learn)
                .on_mouse_down(
                    [i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    {
                        if (ev.button == GP_MOUSE_RIGHT)
                            open_control_menu(cx, *self.ed.get(cx), self.node, i);
                    })
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
                              .bg_gradient(180, { stop(kWhite, 0), stop(rgb(0xc4c7d3), 1) })
                              .shadow(0, 1, 2, with_alpha(kBlack, 0.5f)))
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
                // a control's press, not the panel's (the right button: MIDI learn)
                .on_mouse_down(
                    [i](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    {
                        if (ev.button == GP_MOUSE_RIGHT)
                            open_control_menu(cx, *self.ed.get(cx), self.node, i);
                    })
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
            cell.bg(with_alpha(kBlack, 0.3f));
        cell.hover_bg(p.is_output ? with_alpha(kBlack, 0.45f) : with_alpha(kWhite, 0.07f));
        cell.child(
                div()
                    .absolute()
                    .left(j.x - c.x)
                    .top(j.y - c.y)
                    .w(j.w)
                    .h(j.h)
                    .rounded(event ? 3 : j.w / 2)
                    .bg_gradient(180, { stop(rgb(0xe3e5ee), 0), stop(rgb(0x8b8f9d), 1) })
                    .border(1)
                    .border_color(st::kKnobEdge)
                    .shadow(0, 1, 2, with_alpha(kBlack, 0.4f))
                    .flex_row()
                    .items_center()
                    .justify_center()
                    .child(div()
                            .w(8)
                            .h(8)
                            .rounded(event ? 1 : 4)
                            .bg_gradient(180, { stop(rgb(0x050506), 0), stop(rgb(0x1d1f26), 1) })))
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
                            cx, PortRef { self.node, (uint16_t)i }, ev.position, ev.mods);
                })
            .on_drag(
                [](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->jack_drag(cx, ev.position);
                })
            .on_mouse_up(
                [](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                {
                    if (ev.button == GP_MOUSE_LEFT)
                        self.ed.get(cx)->jack_release(cx, ev.position);
                });
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
            selected_ = e->selected_node == node;
            auto ph = e->playhead.find(node);
            playhead_ = ph == e->playhead.end() || !e->audio_running() ? -1 : ph->second;
            strip_base = 60 + 12 * e->octave;
            strip_held_.clear();
            for (const auto &[k, v] : e->held)
                strip_held_.push_back(v);
        }

        Element p = div()
                        .w(L.width)
                        .h(L.height)
                        .rounded(4)
                        .bg_gradient(180, { stop(cc.top, 0), stop(cc.bottom, 1) })
                        .border(kPanelBorder)
                        .border_color(selected_ ? st::kAccent : st::kKnobEdge)
                        .shadow(0, selected_ ? 0 : 3, selected_ ? 12 : 8,
                            selected_ ? with_alpha(st::kAccent, 0.55f) : with_alpha(kBlack, 0.4f))
                        // the face moves the panel (a press on it does not pan the rack); controls,
                        // jacks and cables take their own presses first
                        .on_mouse_down([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                            { self.press_face(cx, ev); })
                        .on_drag([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                            { self.drag_face(cx, ev); })
                        .on_mouse_up([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                            { self.release_face(cx, ev); });

        // a sheen across the face (under everything else), as light falls on a metal panel
        p.child(div()
                .absolute()
                .left(0)
                .top(kTitleHeight)
                .w(L.width - 2 * kPanelBorder)
                .h(L.height - 2 * kPanelBorder - kTitleHeight)
                .rounded_bottom(3)
                .bg_gradient(115,
                    { stop(with_alpha(kWhite, 0.07f), 0), stop(with_alpha(kWhite, 0), 0.45f),
                        stop(with_alpha(kBlack, 0.10f), 1) }));
        // the title bar moves the panel; a right-click opens its menu
        p.child(div()
                .absolute()
                .left(0)
                .top(0)
                .w(L.width - 2 * kPanelBorder)
                .h(kTitleHeight)
                .rounded_top(3)
                .bg_gradient(180, { stop(cc.top, 0), stop(cc.title, 0.6f) })
                .border_bottom(1)
                .border_color(with_alpha(kBlack, 0.35f))
                .flex_row()
                .items_center()
                .justify_center()
                .overflow_hidden()
                .child(label(std::string(d.name), L.width < 100 ? 9 : 11, st::kText).font_bold())

                .on_mouse_down([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.press_face(cx, ev); })
                .on_drag([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.drag_face(cx, ev); })
                .on_mouse_up([](PanelView &self, Ctx cx, const gp_mouse_event &ev)
                    { self.release_face(cx, ev); })
                .on_hover([](PanelView &self, Ctx cx, bool inside)
                    { self.ed.get(cx)->hover(cx, self.node, Editor::kHoverTitle, inside); }));

        for (size_t i = 0; i < d.params.size(); i++)
            p.child(control(d, i, n->params[i], L.params[i], cc.chip));
        for (size_t i = 0; i < d.ports.size(); i++)
            p.child(jack(d, i, L.ports[i], L.jacks[i]));
        p.child(activity_led());
        if (d.has_steps)
            p.child(step_grid_element(*n, L, cc.chip));
        if (d.id == "keyboard")
            p.child(strip_element(L, cc.chip));
        if (d.id == "scope")
        {
            Editor *e = ed.get(cx);
            auto    it = e->scopes.find(node);
            p.child(scope_element(
                *n, L, it == e->scopes.end() ? nullptr : &it->second, e->audio_running()));
        }
        if (d.id == "out") // the output level, between its controls and its jacks
            p.child(meters(L));
        if (midi_sink(d)) // the MIDI input it listens to, under its controls
            p.child(device_element(cx, *n, L, cc.chip));

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
                    .bg(with_alpha(kBlack, 0.88f))
                    .border(1)
                    .border_color(with_alpha(cc.chip, 0.7f))
                    .shadow(0, 2, 6, with_alpha(kBlack, 0.5f))
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
    constexpr float kRailH = 12; // a rack rail's height

    // A control's right-click menu: under the control, its left edge at the control's (rack
    // pixels).
    constexpr float kControlMenuW = 190, kControlMenuPad = 4, kMenuItemH = 26;
    Pt              control_menu_origin(const Node &n, size_t param)
    {
        const Rect &r = panel_layout(*n.desc).params[param];
        return { n.x * kHp + kPanelBorder + r.x, n.y * kHp + kPanelBorder + r.y + r.h + 2 };
    }

    // A MIDI module's device menu: under its Device box.
    constexpr float kDeviceMenuW = 250;
    Pt              device_menu_origin(const Node &n)
    {
        const Rect r = device_box(panel_layout(*n.desc));
        return { n.x * kHp + kPanelBorder + r.x, n.y * kHp + kPanelBorder + r.y + r.h + 2 };
    }

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
            .bg_radial(0.4f, 0.35f, { stop(kWhite, 0), stop(col, 0.45f) })
            .border(2)
            .border_color(st::kKnobEdge)
            .shadow(0, 2, 4, with_alpha(kBlack, 0.5f))
            .flex_row()
            .items_center()
            .justify_center()
            .child(div().w(4).h(4).rounded_full().bg(with_alpha(kBlack, 0.55f)));
    }
    // The cable is see-through for its first and last stretch (about as far as a jack's name
    // reaches), so the name under a plugged jack can still be read.
    Element faded_ends(Element stroke, const std::array<Pt, 4> &p, Color col)
    {
        const float len = 1.15f * std::hypot(p[3].x - p[0].x, p[3].y - p[0].y);
        const float f = std::clamp(30.f / std::max(len, 1.f), 0.02f, 0.3f);
        const Color thin = with_alpha(col, col.a * 0.3f);
        return std::move(stroke).stroke_gradient({ stop(thin, 0), stop(thin, 0.6f * f),
            stop(col, f), stop(col, 1 - f), stop(thin, 1 - 0.6f * f), stop(thin, 1) });
    }
    Element cable_curve_element(const std::array<Pt, 4> &p, Color col, float width)
    {
        return faded_ends(
            curve(xy(p[0].x, p[0].y), xy(p[1].x, p[1].y), xy(p[2].x, p[2].y), xy(p[3].x, p[3].y))
                .stroke_width(width)
                .stroke_color(col),
            p, col);
    }
    // What a cable casts on the panels under it: the same curve, lower, dark and soft. Drawn before
    // the cable, with no handlers of its own.
    Element cable_shadow_element(const std::array<Pt, 4> &p, float width)
    {
        const Color col = with_alpha(kBlack, 0.28f);
        return faded_ends(curve(xy(p[0].x, p[0].y + 3), xy(p[1].x, p[1].y + 5),
                              xy(p[2].x, p[2].y + 5), xy(p[3].x, p[3].y + 3))
                              .stroke_width(width + 1.5f)
                              .stroke_color(col),
            p, col);
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
            .child(cable_shadow_element(cable_curve(a, b), sel ? 7.f : 4.5f))
            .child(cable_curve_element(
                cable_curve(a, b), with_alpha(col, sel ? 1.f : 0.9f), sel ? 7.f : 4.5f)
                    .hit_tolerance(4)
                    .cursor_pointer()
                    .on_mouse_down(
                        [](CableView &self, Ctx cx, const gp_mouse_event &ev)
                        {
                            Editor  *e = self.ed.get(cx);
                            const Pt q = e->to_rack(cx, ev.position);
                            cx.blur();
                            if (ev.button == GP_MOUSE_RIGHT)
                                open_cable_menu(cx, *e, self.to, q);
                            else if (ev.button != GP_MOUSE_LEFT)
                                return;
                            else if (auto j = jack_at(e->engine.patch(), q.x, q.y, 0))
                                e->jack_press(cx, *j, ev.position, ev.mods);
                            else
                                e->select_cable(cx, self.to);
                        })
                    .on_drag(
                        [](CableView &self, Ctx cx, const gp_mouse_event &ev)
                        {
                            if (ev.button == GP_MOUSE_LEFT)
                                self.ed.get(cx)->jack_drag(cx, ev.position);
                        })
                    .on_mouse_up(
                        [](CableView &self, Ctx cx, const gp_mouse_event &ev)
                        {
                            if (ev.button == GP_MOUSE_LEFT)
                                self.ed.get(cx)->jack_release(cx, ev.position);
                        })
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
        Element           w = div().absolute().left(0).top(0);
        const ModuleDesc *od = nullptr;   // where a module dragged in, or a panel moved, would land
        float             ox = 0, oy = 0; // in HP
        bool              ofits = false;
        if (const auto &m = e->mdrag; m.active && m.moved && m.over_rack)
        {
            od = m.desc;
            ox = (float)m.x;
            oy = (float)m.y;
            ofits = m.fits;
        }
        else if (const auto &pd = e->pdrag; pd.active && pd.moved)
            if (const Node *n = e->engine.patch().find(pd.node))
            {
                // A panel being moved follows the pointer exactly, and its rectangle (green: it
                // fits where it would land, red: it does not) is drawn around the panel itself.
                // Drawn at the grid place it would land on, it stepped a whole HP at a time while
                // the panel moved smoothly, and so seemed to lag behind it.
                od = n->desc;
                ox = n->x;
                oy = n->y;
                ofits = pd.fits;
            }
        if (od)
        {
            const Rect  r = node_rect(*od, ox, oy);
            const Color c = ofits ? st::kLedOn : st::kDanger;
            w.child(div()
                    .absolute()
                    .left(r.x)
                    .top(r.y)
                    .w(r.w)
                    .h(r.h)
                    .rounded(4)
                    .bg(with_alpha(st::category(od->category).top, ofits ? 0.45f : 0.25f))
                    .border(2)
                    .border_color(c));
            if (e->mdrag
                    .active) // a module from the browser: its name (a moved panel shows its own)
                w.child(div()
                        .absolute()
                        .left(r.x)
                        .top(r.y + 3)
                        .w(r.w)
                        .flex_row()
                        .justify_center()
                        .child(label(std::string(od->name), 11, st::kText).font_bold()));
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
        w.child(cable_shadow_element(cable_curve(a, d.loose), 4.5f))
            .child(cable_curve_element(cable_curve(a, d.loose), with_alpha(col, 0.95f), 4.5f))
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
    uint32_t               menu_node = 0; // the panel whose menu is open
    uint32_t               ctrl_node = 0; // or the control whose menu is open: its node and which
    size_t                 ctrl_param = 0;
    std::optional<PortRef> menu_cable; // or the cable whose menu is open, at menu_at (rack pixels)
    Pt                     menu_at;
    uint32_t               dev_node = 0; // or the MIDI module whose device menu is open

    // At most one menu is open. Closes it; true if one was.
    bool close_menus()
    {
        const bool any = menu_node || ctrl_node || menu_cable || dev_node;
        menu_node = ctrl_node = dev_node = 0;
        menu_cable.reset();
        return any;
    }

    template <class F> static Element menu_item(std::string text, Color c, F &&f)
    {
        return div()
            .h(kMenuItemH)
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
        // (zoomed out, the view shows more than that: the surface always fills it)
        const float   z = e->zoom;
        const gp_size view = scroll.viewport(cx);
        const float   cw
            = std::max({ right + st::kWindowW, 3 * st::kWindowW, view.w / z + st::kWindowW / 2 });
        const float ch = std::max({ bottom + kPanelHeight, 3 * kPanelHeight,
            (std::floor(view.h / z / kPanelHeight) + 1) * kPanelHeight });

        Element content
            = div()
                  .w(cw)
                  .h(ch)
                  .bg(st::kRack)
                  .on_mouse_down(
                      [](RackView &self, Ctx cx, const gp_mouse_event &ev)
                      {
                          if (self.close_menus())
                              cx.notify();
                          cx.blur(); // the search box lets go of the keyboard (Delete is the
                                     // rack's)
                          if (Editor *e = self.ed.get(cx); e && e->selected)
                              e->select_cable(cx, std::nullopt);
                          if (Editor *e = self.ed.get(cx); e && e->selected_node)
                              e->select_node(cx, 0);
                          // any button drags the rack about (left, middle or right)
                          if (Editor *e = self.ed.get(cx))
                              e->pan_begin(cx, ev.position, ev.button);
                      })
                  .on_drag([](RackView &self, Ctx cx, const gp_mouse_event &ev)
                      { self.ed.get(cx)->pan_move(cx, ev.position, ev.button); })
                  .on_mouse_up([](RackView &self, Ctx cx, const gp_mouse_event &ev)
                      { self.ed.get(cx)->pan_end(ev.button); })
                  // The wheel over the rack zooms about the pointer (a control under the pointer
                  // takes the wheel first, as before). Ctrl+wheel and Shift+wheel are passed on to
                  // the scroller: up and down, and sideways.
                  .on_wheel(
                      [](RackView &self, Ctx cx, const gp_wheel_event &ev)
                      {
                          Editor *e = self.ed.get(cx);
                          if (!e || ev.dy == 0 || (ev.mods & GP_MOD_CTRL))
                          {
                              cx.propagate();
                              return;
                          }
                          e->set_zoom(cx, e->zoom * std::pow(1.1f, -ev.dy / 40.f), ev.position);
                      });
        // Each row of the rack has a rail along its top and its bottom, with a mounting slot per HP
        // (the slots are one repeating gradient: a period is one HP of the content's width).
        const float hp = kHp / cw;
        auto        rail = [&](float y)
        {
            const Color slot = rgb(0x0c0d10), none = with_alpha(slot, 0);
            content.child(div()
                    .absolute()
                    .left(0)
                    .top(y)
                    .w(cw)
                    .h(kRailH)
                    .bg_gradient(180, { stop(rgb(0x2b2d37), 0), stop(rgb(0x1f2027), 1) })
                    .border_y(1)
                    .border_color(rgb(0x121317))
                    .child(div()
                            .absolute()
                            .left(0)
                            .top(kRailH / 2 - 3)
                            .w(cw)
                            .h(4)
                            .bg_repeating(90,
                                { stop(none, 0), stop(none, 0.3f * hp), stop(slot, 0.3f * hp),
                                    stop(slot, 0.7f * hp), stop(none, 0.7f * hp),
                                    stop(none, hp) })));
        };
        for (float y = 0; y < ch; y += kPanelHeight)
        {
            rail(y);
            rail(y + kPanelHeight - kRailH);
        }
        if (patch.nodes().empty()) // an empty rack says how to begin
            content.child(div()
                    .absolute()
                    .left(48)
                    .top(kPanelHeight / 2 - 40)
                    .flex_col()
                    .gap(6)
                    .child(label("An empty rack", 22, st::kSubtext).font_bold())
                    .child(label(
                        "Add a module from the list on the left, or drag one here.", 13, st::kDim))
                    .child(label("Open > Examples has patches to start from.", 13, st::kDim)));

        const uint32_t lifted = e->pdrag.active && e->pdrag.moved ? e->pdrag.node : 0;
        auto           place = [&](uint32_t id, Entity<PanelView> &pv)
        {
            if (const Node *n = patch.find(id))
                content.child(div()
                        .absolute()
                        .left(n->x * kHp)
                        .top(n->y * kHp)
                        .key_id(id)
                        .child(pv.cached()));
        };
        for (auto &[id, panel] : e->panels)
            if (id != lifted)
                place(id, panel);
        if (auto it = e->panels.find(lifted); it != e->panels.end()) // over the others
            place(lifted, it->second);
        for (auto &[key, cable] : e->cable_views) // over the panels
            content.child(div()
                    .absolute()
                    .left(0)
                    .top(0)
                    .key_id((uint64_t)1 << 40 | (uint64_t)key.first << 16 | key.second)
                    .child(cable.cached()));
        content.child(
            div().absolute().left(0).top(0).key_id((uint64_t)1 << 41).child(e->drag_view.cached()));

        // While a menu is open, the next press anywhere else only closes it: one transparent sheet
        // over the panels and cables and under the menu takes that press (any button) and uses it
        // up, so a knob under the pointer does not move. It passes the wheel on: an element with
        // no wheel handler would swallow it (docs/gui.md).
        if (menu_node || ctrl_node || dev_node || menu_cable)
            content.child(div()
                    .absolute()
                    .left(0)
                    .top(0)
                    .w(cw)
                    .h(ch)
                    .key_id((uint64_t)1 << 42)
                    .on_mouse_down(
                        [](RackView &self, Ctx cx, const gp_mouse_event &)
                        {
                            if (self.close_menus())
                                cx.notify();
                        })
                    .on_wheel([](RackView &, Ctx cx, const gp_wheel_event &) { cx.propagate(); }));
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
        // a control's menu, under the control: MIDI learn
        if (const Node *n = ctrl_node ? patch.find(ctrl_node) : nullptr;
            n && ctrl_param < n->desc->params.size())
        {
            const uint32_t id = ctrl_node;
            const size_t   pi = ctrl_param;
            const Pt       at = control_menu_origin(*n, pi);
            const auto     it = std::find_if(e->settings.learn.begin(), e->settings.learn.end(),
                [&](const Learn &l) { return l.control == (int)pi; });
            Element        menu = div()
                                      .absolute()
                                      .left(at.x)
                                      .top(at.y)
                                      .w(kControlMenuW)
                                      .p(kControlMenuPad)
                                      .rounded(5)
                                      .bg(st::kSurfaceHi)
                                      .border(1)
                                      .border_color(st::kBorder)
                                      .shadow(0, 6, 16, with_alpha(rgb(0x000000), 0.5f))
                                      .on_mouse_down([](RackView &) { });
            menu.child(menu_item("MIDI learn", st::kText,
                [id, pi](RackView &self, Ctx cx)
                {
                    self.ctrl_node = 0;
                    cx.notify();
                    if (Editor *ed = self.ed.get(cx))
                        ed->learn_begin(cx, id, pi);
                }));
            if (it != e->settings.learn.end())
                menu.child(menu_item("Forget its mapping  (CC " + std::to_string(it->cc) + ")",
                    st::kDanger,
                    [pi](RackView &self, Ctx cx)
                    {
                        self.ctrl_node = 0;
                        cx.notify();
                        if (Editor *ed = self.ed.get(cx))
                            ed->forget_learned_at(cx, (int)pi);
                    }));
            menu.child(menu_item("Cancel", st::kText,
                [](RackView &self, Ctx cx)
                {
                    self.ctrl_node = 0;
                    cx.notify();
                }));
            content.child(std::move(menu));
        }
        // a MIDI module's device menu, under its Device box: every input, or one of them
        if (const Node *n = dev_node ? patch.find(dev_node) : nullptr; n && midi_sink(*n->desc))
        {
            const uint32_t id = dev_node;
            const Pt       at = device_menu_origin(*n);
            Element        menu = div()
                                      .absolute()
                                      .left(at.x)
                                      .top(at.y)
                                      .w(kDeviceMenuW)
                                      .p(kControlMenuPad)
                                      .rounded(5)
                                      .bg(st::kSurfaceHi)
                                      .border(1)
                                      .border_color(st::kBorder)
                                      .shadow(0, 6, 16, with_alpha(rgb(0x000000), 0.5f))
                                      .on_mouse_down([](RackView &) { });
            // the one it has now is in the accent colour
            menu.child(menu_item("Any device", n->device.empty() ? st::kAccent : st::kText,
                [id](RackView &self, Ctx cx)
                {
                    self.dev_node = 0;
                    cx.notify();
                    if (Editor *ed = self.ed.get(cx))
                        ed->set_midi_device(cx, id, std::string());
                }));
            const std::vector<std::string> names = e->device_menu_names(id);
            for (size_t k = 0; k < names.size(); k++)
            {
                const bool here
                    = std::find(e->midi_available.begin(), e->midi_available.end(), names[k])
                    != e->midi_available.end();
                menu.child(menu_item(names[k] + (here ? "" : "   (not connected)"),
                    names[k] == n->device ? st::kAccent : here ? st::kText : st::kSubtext,
                    [id, k](RackView &self, Ctx cx)
                    { // (a handler keeps no string: the name is looked up again by its place)
                        self.dev_node = 0;
                        cx.notify();
                        Editor *ed = self.ed.get(cx);
                        if (!ed)
                            return;
                        const std::vector<std::string> now = ed->device_menu_names(id);
                        if (k < now.size())
                            ed->set_midi_device(cx, id, now[k]);
                    }));
            }
            content.child(std::move(menu));
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
        // the content is scaled about its top left corner inside a box of its size on the screen
        // (a transform does not change what an element takes up in the layout)
        return div().size_full().overflow_scroll().track_scroll(scroll).bg(st::kRack).child(
            div().w(cw * z).h(ch * z).child(
                std::move(content).absolute().left(0).top(0).transform_origin(0, 0).scale(z)));
    }
};

void open_menu(Ctx cx, Editor &e, uint32_t node)
{
    e.rack.update(cx,
        [node](RackView &r, Ctx c)
        {
            r.close_menus();
            r.menu_node = node;
            c.notify();
        });
}

void open_device_menu_of(Ctx cx, Editor &e, uint32_t node)
{
#if PULSERACK_MIDI
    if (e.midi) // what is plugged in now
        e.midi_available = e.midi->available();
#endif
    e.rack.update(cx,
        [node](RackView &r, Ctx c)
        {
            r.close_menus();
            r.dev_node = node;
            c.notify();
        });
}

void open_control_menu(Ctx cx, Editor &e, uint32_t node, size_t param)
{
    e.rack.update(cx,
        [node, param](RackView &r, Ctx c)
        {
            r.close_menus();
            r.ctrl_node = node;
            r.ctrl_param = param;
            c.notify();
        });
}

void open_cable_menu(Ctx cx, Editor &e, PortRef to, Pt at)
{
    e.select_cable(cx, to);
    e.rack.update(cx,
        [to, at](RackView &r, Ctx c)
        {
            r.close_menus();
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

// Over the whole window, only while something is shown: the Open menu (a click outside closes it)
// and the unsaved-changes question (modal: nothing under it can be reached until it is answered).
struct Overlay
{
    Entity<Editor> ed;

    template <class F> static Element item(std::string title, std::string about, F &&f)
    {
        Element it
            = div()
                  .h(about.empty() ? st::kFileItemH : st::kFileExampleH)
                  .flex_shrink_0()
                  .overflow_hidden() // a description longer than two lines is cut, never overlaps
                  .px(10)
                  .justify_center()
                  .rounded(4)
                  .flex_col()
                  .gap(2)
                  .cursor_pointer()
                  .hover_bg(st::kSurface)
                  .child(label(std::move(title), 12, st::kText));
        if (!about.empty())
            it.child(label(std::move(about), 10, st::kSubtext));
        it.on_click(std::forward<F>(f));
        return it;
    }
    template <class F> static Element choice(const char *text, Color bg, Color fg, F &&f)
    {
        return div()
            .w(st::kAskBtnW)
            .h(st::kAskBtnH)
            .rounded(4)
            .flex_row()
            .items_center()
            .justify_center()
            .cursor_pointer()
            .bg(bg)
            .hover_bg(st::kBorder)
            .child(label(text, 12, fg))
            .on_click(std::forward<F>(f));
    }

    Element render(Ctx cx)
    {
        Editor *e = ed.get(cx);
        if (!e || (!e->file_menu_open && !e->ask.shown && !e->device_menu_open))
            return div();
        Element layer = div().absolute().left(0).top(0).w(8000).h(8000);
        if (e->ask.shown)
        {
            layer.bg(with_alpha(rgb(0x000000), 0.45f));
            layer.on_mouse_down([](Overlay &) { });
            layer.child(div()
                    .absolute()
                    .left((st::kWindowW - st::kAskW) / 2)
                    .top((st::kWindowH - st::kAskH) / 2)
                    .w(st::kAskW)
                    .h(st::kAskH)
                    .p(st::kAskPad)
                    .gap(10)
                    .flex_col()
                    .rounded(6)
                    .bg(st::kSurfaceHi)
                    .border(1)
                    .border_color(st::kBorder)
                    .shadow(0, 10, 30, with_alpha(rgb(0x000000), 0.6f))
                    .child(label("Unsaved changes to " + e->patch_name, 14, st::kText).font_bold())
                    .child(label("Save them first?", 12, st::kSubtext))
                    .child(div().flex_1())
                    .child(div()
                            .flex_row()
                            .gap(st::kAskGap)
                            .justify_end()
                            .child(choice("Cancel", st::kSurface, st::kText,
                                [](Overlay &self, Ctx c) { self.ed.get(c)->answer(c, 2); }))
                            .child(choice("Don't save", st::kSurface, st::kDanger,
                                [](Overlay &self, Ctx c) { self.ed.get(c)->answer(c, 1); }))
                            .child(choice("Save", st::kAccent, st::kBase,
                                [](Overlay &self, Ctx c) { self.ed.get(c)->answer(c, 0); }))));
            return layer;
        }
        layer.on_mouse_down([](Overlay &self, Ctx c) { self.ed.get(c)->close_overlays(c); });
        if (e->device_menu_open)
        { // the output devices, under the audio status
            Element menu = div()
                               .absolute()
                               .left(st::kWindowW - 12 - st::kAudioW - st::kDeviceMenuW)
                               .top(st::kToolbarH - 2)
                               .w(st::kDeviceMenuW)
                               .p(4)
                               .flex_col()
                               .rounded(5)
                               .bg(st::kSurfaceHi)
                               .border(1)
                               .border_color(st::kBorder)
                               .shadow(0, 8, 20, with_alpha(rgb(0x000000), 0.5f));
            menu.on_mouse_down([](Overlay &) { });
            auto mark = [&](const std::string &name)
            { return std::string(e->audio_device == name ? "*  " : "   "); };
            menu.child(item(mark("") + "System default", "",
                [](Overlay &self, Ctx c) { self.ed.get(c)->choose_device(c, ""); }));
            for (size_t k = 0; k < e->devices.size(); k++)
                menu.child(item(mark(e->devices[k].name) + e->devices[k].name, "",
                    [k](Overlay &self, Ctx c)
                    {
                        Editor *ed2 = self.ed.get(c);
                        if (k < ed2->devices
                                .size()) // (a copy: the list is read again on the next opening)
                            ed2->choose_device(c, std::string(ed2->devices[k].name));
                    }));
            // the MIDI inputs: each is switched on or off (several may be on); the menu stays open
            menu.child(div()
                    .h(st::kFileHeaderH)
                    .flex_shrink_0()
                    .px(10)
                    .flex_row()
                    .items_end()
                    .pb(3)
                    .child(label("MIDI inputs  (click to switch on or off)", 10, st::kDim)));
            const std::vector<std::string> names = e->midi_menu_names();
            for (size_t k = 0; k < names.size(); k++)
            {
                const std::string &n = names[k];
                const bool         on
                    = std::find(e->settings.midi_inputs.begin(), e->settings.midi_inputs.end(), n)
                    != e->settings.midi_inputs.end();
                const bool here = std::find(e->midi_available.begin(), e->midi_available.end(), n)
                    != e->midi_available.end();
                menu.child(item(std::string(on ? "*  " : "   ") + n
                        + (here ? (on && !e->midi_input_open(n) ? "   (could not be opened)" : "")
                                : "   (not connected)"),
                    "",
                    [k](Overlay &self, Ctx c)
                    { // (a handler keeps no string: the name is looked up again by its place)
                        Editor                        *ed2 = self.ed.get(c);
                        const std::vector<std::string> now = ed2->midi_menu_names();
                        if (k < now.size())
                            ed2->toggle_midi_input(c, now[k]);
                    }));
            }
            if (names.empty())
                menu.child(div()
                        .h(st::kFileItemH)
                        .px(10)
                        .flex_row()
                        .items_center()
                        .child(
                            label(e->midi_error.empty() ? std::string("No MIDI input is connected.")
                                                        : e->midi_error,
                                12, st::kSubtext)));
            if (!e->settings.learn.empty())
                menu.child(item("Forget the MIDI learn mappings  ("
                        + std::to_string(e->settings.learn.size()) + ")",
                    "", [](Overlay &self, Ctx c) { self.ed.get(c)->forget_learned(c); }));
            layer.child(std::move(menu));
            return layer;
        }
        // the Open menu, under its button
        Element menu = div()
                           .absolute()
                           .left(st::kOpenX)
                           .top(st::kToolbarH - 2)
                           .w(st::kFileMenuW)
                           .p(4)
                           .flex_col()
                           .rounded(5)
                           .bg(st::kSurfaceHi)
                           .border(1)
                           .border_color(st::kBorder)
                           .shadow(0, 8, 20, with_alpha(rgb(0x000000), 0.5f));
        menu.on_mouse_down([](Overlay &) { }); // a press in the menu does not close it
        menu.child(item("Open a file...      Ctrl+O", "",
            [](Overlay &self, Ctx c) { self.ed.get(c)->request(c, Editor::Then::OpenDialog); }));
        if (e->has_recovery())
            menu.child(item("Recover the unsaved patch",
                "kept when PulseRack last closed with unsaved changes",
                [](Overlay &self, Ctx c) { self.ed.get(c)->request(c, Editor::Then::Recover); }));
        menu.child(div()
                .h(st::kFileHeaderH)
                .flex_shrink_0()
                .px(10)
                .flex_row()
                .items_end()
                .pb(3)
                .child(label("Examples", 10, st::kDim)));
        const auto f = factory_patches();
        for (size_t i = 0; i < f.size(); i++)
            menu.child(
                item(std::string(f[i].title), std::string(f[i].about), [i](Overlay &self, Ctx c)
                    { self.ed.get(c)->request(c, Editor::Then::Factory, (int)i); }));
        layer.child(std::move(menu));
        return layer;
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
            .child(div().absolute().left(0).top(0).child(e->overlay.cached()))
            // the computer keyboard as a piano: keys reach the root when nothing focused takes them
            .on_key_down(
                [](Root &self, Ctx cx, const gp_key_event &ev)
                {
                    Editor *e = self.ed.get(cx);
                    if (e->piano(cx, ev, true))
                        return;
                    const std::string k = ev.key ? ev.key : "";
                    const int         dir = k == "up" || k == "right" ? 1
                        : k == "down" || k == "left"                  ? -1
                                                                      : 0;
                    if (dir && !e->search_focused(cx) && !(ev.mods & (GP_MOD_CTRL | GP_MOD_ALT))
                        && e->nudge(cx, dir, (ev.mods & GP_MOD_SHIFT) != 0))
                        return;
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
        const float z = e.zoom;
        return { off.x / z, off.y / z, vp.w / z, vp.h / z, z };
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
            + (p.is_output ? "      drag from it to an input to connect; Shift+drag moves a cable "
                             "plugged here"
                           : "      drag from it to an output to connect");
    }
    if (item == kHoverDevice)
        return std::string(d.name) + "  ·  listens to "
            + (n->device.empty() ? std::string("every MIDI input that is switched on") : n->device)
            + "      click to choose one MIDI input, or all of them";
    if (item == kHoverStrip)
        return std::string(d.name)
            + "  ·  press the keys to play, drag across them to glide; the computer keys "
              "A W S E D F ... play every Keyboard too (Z and X: octave)";
    const int step = kHoverStep - item;
    if (d.has_steps && step >= 0 && (size_t)step < n->steps.size())
    {
        const StepData    &s = n->steps[(size_t)step];
        static const char *names[12]
            = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const std::string note = std::string(names[s.note % 12]) + std::to_string(s.note / 12 - 1);
        return std::string(d.name) + "  ·  step " + std::to_string(step + 1) + ": " + note
            + (s.velocity ? ", on" : ", rest") + (s.tie ? ", tied to the next" : "")
            + "      drag the bar (or the wheel; Shift: octaves) for the note; the pads: on/rest, "
              "tie";
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
    return { (w.x - o.x) / vp.zoom + vp.x, (w.y - o.y) / vp.zoom + vp.y };
}

void Editor::set_zoom(Ctx cx, float z, gp_point about)
{
    z = std::clamp(z, kZoomMin, kZoomMax);
    if ((zoom - 1) * (z - 1) < 0)
        z = 1; // passing 100 % stops there
    RackView *rv = rack.get(cx);
    if (!rv || z == zoom)
        return;
    // the rack point under `about` stays under it
    const Pt       q = to_rack(cx, about);
    const gp_point o = rack_origin();
    zoom = z;
    rv->scroll.to(
        cx, std::max(0.f, q.x * z - (about.x - o.x)), std::max(0.f, q.y * z - (about.y - o.y)));
    say(cx,
        "Zoom " + std::to_string((int)std::lround(z * 100))
            + " %.      Wheel: zoom, Ctrl+wheel: up and down, Shift+wheel: sideways, Ctrl+0: 100 "
              "%");
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    refresh_rack(cx);
}

// ---- sound

bool Editor::start_audio(Ctx cx)
{
    if (!audio)
        audio = std::make_unique<audio::AudioOut>(engine);
    std::string err;
    engine.discard_midi(); // what was played while audio was off is not heard all at once
    const bool ok = audio->start(&err, audio_device);
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

// ---- MIDI

#if PULSERACK_MIDI
void Editor::start_midi(Ctx cx)
{
    midi = std::make_unique<midi::Inputs>(*midi_boxes);
    std::string err;
    if (!midi->start(&err))
    {
        midi_error = "MIDI: " + err;
        midi.reset();
        return;
    }
    std::vector<std::string> errors;
    midi->set_enabled(settings.midi_inputs, &errors);
    for (const std::string &e : errors)
        say(cx, "MIDI: " + e + ".");
    midi_available = midi->available();
    if (settings.midi_inputs.empty() && !midi_available.empty() && status.empty())
        status = "MIDI inputs are connected: switch them on in the device menu (top right, beside "
                 "Audio).";
    midi_task = cx.app().ctx(self).every(0.25, [](Editor &e, Ctx c) { e.poll_midi(c); });
    control_task = cx.app().ctx(self).every(1.0 / 60, [](Editor &e, Ctx c) { e.pump_controls(c); });
}

void Editor::poll_midi(Ctx cx)
{
    if (!midi)
        return;
    std::vector<std::string> before;
    for (const std::string &n : midi->enabled())
        if (midi->is_open(n))
            before.push_back(n);
    std::vector<std::string> errors;
    if (!midi->poll(&errors) && errors.empty())
        return;
    for (const std::string &n : midi->enabled())
    {
        const bool was = std::find(before.begin(), before.end(), n) != before.end();
        if (midi->is_open(n) != was)
            say(cx, was ? "MIDI input gone: " + n + "." : "MIDI input connected: " + n + ".");
    }
    for (const std::string &e : errors)
        say(cx, "MIDI: " + e + ".");
    midi_available = midi->available();
    refresh_midi_panels(cx);
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
}

bool Editor::midi_input_open(const std::string &name) const { return midi && midi->is_open(name); }

int Editor::midi_open_count() const
{
    int n = 0;
    if (midi)
        for (const std::string &name : midi->enabled())
            n += midi->is_open(name);
    return n;
}
#else
void Editor::start_midi(Ctx) { midi_error = "This PulseRack was built without MIDI."; }
void Editor::poll_midi(Ctx) { }
bool Editor::midi_input_open(const std::string &) const { return false; }
int  Editor::midi_open_count() const { return 0; }
#endif

void Editor::toggle_midi_input(Ctx cx, const std::string &name)
{
    auto      &on = settings.midi_inputs;
    const auto it = std::find(on.begin(), on.end(), name);
    const bool now = it == on.end();
    if (now)
        on.push_back(name);
    else
        on.erase(it);
    if (std::string err; !save_settings(config_dir, settings, &err))
        say(cx, err + ".");
#if PULSERACK_MIDI
    if (midi)
    {
        std::vector<std::string> errors;
        midi->set_enabled(on, &errors);
        say(cx,
            errors.empty() ? "MIDI input " + name + (now ? ": on." : ": off.")
                           : "MIDI: " + errors[0] + ".");
    }
#endif
    refresh_midi_panels(cx); // their Device boxes say whether their input is switched on
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
}

// ---- a module's own MIDI input

void Editor::refresh_midi_panels(Ctx cx)
{
    for (const Node &n : engine.patch().nodes())
        if (midi_sink(*n.desc))
            if (auto it = panels.find(n.id); it != panels.end())
                it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
}

std::vector<std::string> Editor::device_menu_names(uint32_t node) const
{
    std::vector<std::string> names = midi_menu_names();
    const Node              *n = engine.patch().find(node);
    if (n && !n->device.empty() && std::find(names.begin(), names.end(), n->device) == names.end())
        names.push_back(n->device); // from a patch made on another machine, perhaps
    return names;
}

void Editor::set_midi_device(Ctx cx, uint32_t node, const std::string &name)
{
    const Node *n = engine.patch().find(node);
    if (!n || !midi_sink(*n->desc) || n->device == name)
        return;
    checkpoint(cx, std::string(n->desc->name) + " device");
    // an input that is chosen is wanted: it is switched on if it was not (a machine setting, which
    // undo leaves alone)
    const bool was_on = name.empty()
        || std::find(settings.midi_inputs.begin(), settings.midi_inputs.end(), name)
            != settings.midi_inputs.end();
    if (!was_on)
        toggle_midi_input(cx, name);
    engine.set_device(node, name);
    touch(cx);
    if (auto it = panels.find(node); it != panels.end())
        it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    const bool here
        = std::find(midi_available.begin(), midi_available.end(), name) != midi_available.end();
    say(cx,
        std::string(n->desc->name)
            + (name.empty() ? " listens to every MIDI input that is switched on."
                            : " listens to " + name
                        + (!here         ? "  (not connected: it will be heard when it is)."
                                : was_on ? "."
                                         : ", which is now switched on.")));
}

// ---- MIDI learn

void Editor::learn_begin(Ctx cx, uint32_t node, size_t param)
{
    const Node *n = engine.patch().find(node);
    if (!n || param >= n->desc->params.size())
        return;
    select_node(cx, node);
    learn_control = (int)param;
    notice = true; // (the pointer is on the control: its own hint would hide this)
    notice_node = node;
    notice_item = (int)param;
    say(cx,
        "MIDI learn: move a knob or fader on your controller to map it to "
            + std::string(n->desc->params[param].name) + " (control " + std::to_string(param + 1)
            + " of the selected module).      Escape cancels.");
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
}

void Editor::learn_cancel(Ctx cx)
{
    if (learn_control < 0)
        return;
    learn_control = -1;
    notice = true;
    say(cx, "MIDI learn cancelled.");
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
}

void Editor::forget_learned_at(Ctx cx, int control)
{
    std::erase_if(settings.learn, [&](const Learn &l) { return l.control == control; });
    if (std::string err; !save_settings(config_dir, settings, &err))
        say(cx, err + ".");
    else
        say(cx, "The MIDI mapping of control " + std::to_string(control + 1) + " is forgotten.");
    notice = true;
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
}

void Editor::forget_learned(Ctx cx)
{
    settings.learn.clear();
    if (std::string err; !save_settings(config_dir, settings, &err))
        say(cx, err + ".");
    else
        say(cx, "The MIDI learn mappings are forgotten.");
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
}

void Editor::pump_controls(Ctx cx)
{
    midi::Msg m;
    while (engine.take_control(m))
        midi_control(cx, m);
}

void Editor::midi_control(Ctx cx, const midi::Msg &m)
{
    if (m.data >= 120) // channel mode messages (all notes off...), not controls
        return;
    std::string device;
#if PULSERACK_MIDI
    if (midi)
        device = midi->name_of(m.slot);
#endif
    auto same = [&](const Learn &l)
    { return l.channel == m.channel && l.cc == m.data && l.device == device; };
    const std::string from
        = "CC " + std::to_string(m.data) + (device.empty() ? std::string() : " of " + device);
    if (learn_control >= 0)
    { // one controller per place, one place per controller
        const int control = learn_control;
        learn_control = -1;
        notice = true;
        std::erase_if(
            settings.learn, [&](const Learn &l) { return same(l) || l.control == control; });
        settings.learn.push_back(Learn { m.channel, m.data, control, device });
        if (std::string err; !save_settings(config_dir, settings, &err))
            say(cx, err + ".");
        else
            say(cx,
                from + " now turns control " + std::to_string(control + 1)
                    + " of whichever module is selected.");
        hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
        return;
    }
    const auto it = std::find_if(settings.learn.begin(), settings.learn.end(), same);
    if (it == settings.learn.end())
        return;
    const Node *n = engine.patch().find(selected_node);
    if (!n)
    {
        notice = true;
        say(cx,
            from + " turns control " + std::to_string(it->control + 1)
                + " of the selected module: click a module to select it.");
        hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
        return;
    }
    if ((size_t)it->control >= n->desc->params.size())
        return; // this module has fewer controls
    const ParamDesc &p = n->desc->params[(size_t)it->control];
    const float      t = (float)std::min<int>(m.value, 127) / 127.0f;
    const float      v = p.control == Control::Switch || p.control == Control::Button
        ? (t >= 0.5f ? p.max : p.min)
        : from_travel(p, t);
    if (v != n->params[(size_t)it->control])
    {
        set_param(cx, selected_node, (size_t)it->control, v); // (a sweep is one undo step)
        hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    }
}

std::vector<std::string> Editor::midi_menu_names() const
{
    std::vector<std::string> names = midi_available;
    for (const std::string &n : settings.midi_inputs) // switched on, but not plugged in
        if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    return names;
}

std::string Editor::midi_status() const
{
    const int n = midi_open_count();
    return n ? "  ·  MIDI " + std::to_string(n) : std::string();
}

void Editor::open_device_menu(Ctx cx)
{
    std::string err;
#if PULSERACK_MIDI
    if (midi)
        midi_available = midi->available();
#endif
    devices = audio::AudioOut::outputs(&err);
    device_menu_open = true;
    file_menu_open = false;
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
    if (!err.empty())
        say(cx, err + ".");
}

void Editor::choose_device(Ctx cx, const std::string &device)
{
    device_menu_open = false;
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
    audio_device = device;
    settings.output = device;
    if (std::string err; !save_settings(config_dir, settings, &err))
        say(cx, err + ".");
    if (audio_running())
        stop_audio(cx);
    start_audio(cx);
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
    std::snprintf(buf, sizeof buf, "%s (%s)  ·  %.1f kHz  ·  %.1f ms  ·  CPU %d %%", d.name.c_str(),
        d.driver.c_str(), d.rate / 1000.0, d.latency_ms(), std::max(0, load_pct));
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
    for (const Node &n : engine.patch().nodes()) // the sequencers' playheads
        if (n.desc->has_steps)
        {
            const int slot = engine.telemetry_slot(n.id);
            const int pos
                = slot < 0 ? -1 : t.position[(size_t)slot].load(std::memory_order_relaxed);
            auto it = playhead.find(n.id);
            if (it == playhead.end() || it->second != pos)
            {
                playhead[n.id] = pos;
                if (auto v = panels.find(n.id); v != panels.end())
                    v->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
            }
        }
    for (const Node &n : engine.patch().nodes()) // the oscilloscopes' newest frames
        if (n.desc->id == "scope")
        {
            ScopeFrame f;
            if (engine.scope_frame(n.id, f))
            {
                scopes[n.id] = f;
                if (auto v = panels.find(n.id); v != panels.end())
                    v->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
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
    const uint32_t drops = audio_running() ? audio->dropouts() : shown_dropouts;
    if (drops != shown_dropouts)
    {
        shown_dropouts = drops;
        toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    }
    const int load
        = audio_running() ? (int)std::lround(100 * t.load.load(std::memory_order_relaxed)) : -1;
    if (load != load_pct)
    {
        load_pct = load;
        toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    }
}

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
            refresh_keyboards(cx); // their range follows
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
    refresh_keyboards(cx); // the pianos light the key
    if (down)
        say(cx,
            sent ? "Note " + note_name(note) + (audio_running() ? "" : "  (audio is off)")
                 : "No Keyboard module: add one to play notes from the computer keyboard.");
    return true;
}

// ---- the rack scrolls under a drag held near its edge

void Editor::edge_scroll_start(Ctx cx, gp_point window)
{
    last_pointer = window;
    if (!edge_task.active())
        edge_task = cx.app().ctx(self).every(1.0 / 60, [](Editor &e, Ctx c) { e.edge_tick(c); });
}

void Editor::edge_tick(Ctx cx)
{
    const bool dragging
        = drag.active || (pdrag.active && pdrag.moved) || (mdrag.active && mdrag.moved);
    if (!drag.active && !pdrag.active && !mdrag.active)
    {
        edge_task.cancel();
        return;
    }
    RackView *rv = rack.get(cx);
    if (!dragging || !rv)
        return;
    const RackViewport vp = viewport_of(cx, *this);
    const gp_point     o = rack_origin(), p = last_pointer;
    const float        ww = vp.w * vp.zoom, wh = vp.h * vp.zoom; // the view in window pixels
    if (p.x < o.x || p.y < o.y || p.x > o.x + ww || p.y > o.y + wh)
        return; // only inside the rack (a module still over the browser does not scroll it)
    constexpr float band = 32;
    auto speed = [&](float depth) { return depth <= 0 ? 0.f : 1 + std::min(12.f, depth * 0.4f); };
    const float sx = speed(p.x - (o.x + ww - band)) - speed(o.x + band - p.x);
    const float sy = speed(p.y - (o.y + wh - band)) - speed(o.y + band - p.y);
    if (sx == 0 && sy == 0)
        return;
    rv->scroll.by(cx, sx, sy);
    // what is dragged follows: the same pointer is now over another part of the rack
    if (drag.active)
        jack_drag(cx, p);
    else if (pdrag.active)
        panel_drag_move(cx, p);
    else if (mdrag.active)
        module_drag_move(cx, p);
}

// ---- panels moved on the rack

void Editor::panel_drag_begin(Ctx cx, uint32_t node, gp_point window)
{
    const Node *n = engine.patch().find(node);
    if (!n)
        return;
    pdrag = PanelDrag { };
    pdrag.active = true;
    pdrag.node = node;
    pdrag.start = window;
    pdrag.x0 = n->x;
    pdrag.y0 = n->y;
    const Pt q = to_rack(cx, window);
    pdrag.grab = { q.x - n->x * kHp, q.y - n->y * kHp };
    edge_scroll_start(cx, window);
}

// ---- dragging the rack about: any button on empty rack, the middle button on a panel too

void Editor::pan_begin(Ctx cx, gp_point window, int button)
{
    RackView *rv = rack.get(cx);
    if (!rv)
        return;
    pan_active = true; // (a second button pressed meanwhile takes the drag over)
    pan_button = button;
    pan_p0 = window;
    pan_off0 = rv->scroll.offset(cx);
}

// The moves and the release come from gpuic to the element that was pressed, for whichever button
// that was; only those of the button that began the drag count.
void Editor::pan_move(Ctx cx, gp_point window, int button)
{
    if (!pan_active || button != pan_button)
        return;
    if (RackView *rv = rack.get(cx))
        rv->scroll.to(cx, pan_off0.x - (window.x - pan_p0.x), pan_off0.y - (window.y - pan_p0.y));
}

void Editor::pan_end(int button)
{
    if (button == pan_button)
        pan_active = false;
}

void Editor::panel_drag_move(Ctx cx, gp_point window)
{
    if (!pdrag.active)
        return;
    Node *n = engine.patch().find(pdrag.node);
    if (!n)
    {
        pdrag = PanelDrag { };
        return;
    }
    last_pointer = window;
    if (!pdrag.moved && std::hypot(window.x - pdrag.start.x, window.y - pdrag.start.y) < 4)
        return; // a click, so far
    pdrag.moved = true;
    const Pt q = to_rack(cx, window);
    // the panel goes where the pointer takes it (its cables with it: their ends come from the
    // model)
    n->x = std::max(0.f, (q.x - pdrag.grab.x) / kHp);
    n->y = std::max(0.f, (q.y - pdrag.grab.y) / kHp);
    pdrag.x = (int)std::lround(n->x);
    pdrag.y = (int)std::lround(n->y);
    pdrag.fits = !collides(engine.patch(), *n->desc, pdrag.x, pdrag.y, pdrag.node);
    const std::string name(n->desc->name);
    status = pdrag.fits ? "Release to put " + name + " here."
                        : "No room here for " + name
            + ": it would overlap another panel; released here, it goes back.";
    refresh_rack(cx);
    refresh_cables_of(cx, pdrag.node);
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
}

void Editor::panel_drag_end(Ctx cx, gp_point window)
{
    if (!pdrag.active)
        return;
    panel_drag_move(cx, window);
    const PanelDrag d = pdrag;
    pdrag = PanelDrag { };
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
    Node *n = engine.patch().find(d.node);
    if (!n)
        return;
    if (!d.moved)
    { // a click on the panel: it is selected
        select_node(cx, d.node);
        return;
    }
    const std::string name(n->desc->name);
    if (d.fits)
    {
        if ((float)d.x != d.x0 || (float)d.y != d.y0)
        { // the step holds where it was, not where the drag took it meanwhile
            n->x = d.x0;
            n->y = d.y0;
            checkpoint(cx, "Move " + name);
            n = engine.patch().find(d.node);
            touch(cx);
        }
        n->x = (float)d.x;
        n->y = (float)d.y;
        say(cx, "Moved " + name + ".");
    }
    else
    {
        n->x = d.x0;
        n->y = d.y0;
        say(cx, "Not moved: " + name + " would overlap another panel there.");
    }
    refresh_rack(cx);
    refresh_cables_of(cx, d.node);
}

// ---- modules dragged from the browser

Slot Editor::drop_slot(Ctx cx, const ModuleDesc &d, gp_point window)
{ // the pointer holds the panel by the middle of its title
    const Pt q = to_rack(cx, window);
    return { (int)std::lround((q.x - d.width_hp * kHp / 2) / kHp),
        (int)std::lround((q.y - kTitleHeight / 2) / kHp) };
}

void Editor::module_drag_begin(Ctx cx, const ModuleDesc &d, gp_point window)
{
    mdrag = ModuleDrag { };
    mdrag.active = true;
    mdrag.desc = &d;
    mdrag.start = mdrag.at = window;
    edge_scroll_start(cx, window);
}

void Editor::module_drag_move(Ctx cx, gp_point window)
{
    if (!mdrag.active)
        return;
    mdrag.at = window;
    last_pointer = window;
    if (!mdrag.moved && std::hypot(window.x - mdrag.start.x, window.y - mdrag.start.y) < 4)
        return; // a click, so far
    mdrag.moved = true;
    const gp_point     o = rack_origin();
    const RackViewport vp = viewport_of(cx, *this);
    mdrag.over_rack = window.x >= o.x && window.y >= o.y && window.x < o.x + vp.w * vp.zoom
        && window.y < o.y + vp.h * vp.zoom;
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
    if (patch.can_connect(from, to) == Err::Ok)
        checkpoint(cx, "Connect " + port_name(from) + " -> " + port_name(to));
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
    touch(cx);
    say(cx, "Connected " + port_name(from) + "  ->  " + port_name(to) + ".");
    return Err::Ok;
}

void Editor::disconnect(Ctx cx, PortRef to)
{
    const Cable *c = engine.patch().cable_into(to);
    if (!c)
        return;
    const std::string what = port_name(c->from) + "  ->  " + port_name(c->to);
    checkpoint(cx, "Remove the cable " + port_name(c->from) + " -> " + port_name(c->to));
    engine.patch().disconnect(to);
    engine.compile();
    sync_cables(cx);
    touch(cx);
    say(cx, "Removed the cable " + what + ".");
}

void Editor::select_node(Ctx cx, uint32_t node)
{
    const uint32_t old = selected_node;
    selected_node = engine.patch().find(node) ? node : 0;
    if (selected_node && selected)
        select_cable(cx, std::nullopt); // one or the other
    for (uint32_t k : { old, selected_node })
        if (auto it = panels.find(k); k && it != panels.end())
            it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    if (selected_node)
        say(cx,
            "Selected " + std::string(engine.patch().find(selected_node)->desc->name)
                + ".      Delete or Backspace removes it.");
}

bool Editor::nudge(Ctx cx, int steps, bool fine)
{
    uint32_t n = 0;
    int      i = -1;
    if (hover_item >= 0 && engine.patch().find(hover_node)) // the control under the pointer
    {
        n = hover_node;
        i = hover_item;
    }
    else if (engine.patch().find(last_ctrl_node))
    {
        n = last_ctrl_node;
        i = last_ctrl_param;
    }
    const Node *nd = engine.patch().find(n);
    if (!nd || i < 0 || (size_t)i >= nd->params.size())
        return false;
    set_param(cx, n, (size_t)i,
        step_value(nd->desc->params[(size_t)i], nd->params[(size_t)i], steps, fine));
    return true;
}

void Editor::select_cable(Ctx cx, std::optional<PortRef> to)
{
    const std::optional<PortRef> old = selected;
    if (to && selected_node)
    { // one or the other
        const uint32_t was = selected_node;
        selected_node = 0;
        if (auto it = panels.find(was); it != panels.end())
            it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    }
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
    else if (selected_node)
        remove_module(cx, selected_node);
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

void Editor::jack_press(Ctx cx, PortRef jack, gp_point window, uint32_t mods)
{
    const Patch &p = engine.patch();
    const Node  *n = p.find(jack.node);
    if (!n || jack.port >= n->desc->ports.size())
        return;
    const bool output = n->desc->ports[jack.port].is_output;
    drag = CableDrag { };
    drag.active = true;
    if (!output)
    {
        if (const Cable *c = p.cable_into(jack))
            drag.picked = *c; // lift the cable off this input; it hangs from its output
    }
    else if (mods & GP_MOD_SHIFT)
        for (const Cable &c : p.cables()) // the newest cable from this output, by its output end
            if (c.from == jack)
            {
                drag.picked = c;
                drag.lift_output = true;
            }
    drag.fixed = drag.picked ? (drag.lift_output ? drag.picked->to : drag.picked->from) : jack;
    drag.loose = to_rack(cx, window);
    edge_scroll_start(cx, window);
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
        ? std::string("Move the plug to another ") + (drag.lift_output ? "output" : "input")
            + ", or drop it on empty rack to remove the cable."
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
    last_pointer = window;
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
        if (d.picked && d.picked->to == t && d.picked->from == f)
        {
            say(cx, "The cable is where it was.");
            return;
        }
        checkpoint(
            cx, d.picked ? "Move a cable" : "Connect " + port_name(f) + " -> " + port_name(t));
        ++batch; // the disconnect and connect below are this one step
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
        --batch;
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
    // the pointer has moved on to something else (the same control entered again, as happens
    // when its panel is drawn again, is not that)
    if (inside && (node != notice_node || item != notice_item))
        notice = false;
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
    checkpoint(cx, "Add " + std::string(d.name));
    const uint32_t id = patch.add_node(d, (float)x, (float)y);
    if (engine.compile() != Err::Ok)
    {
        patch.remove_node(id);
        engine.compile();
        if (!undo_stack.empty() && batch == 0) // nothing changed: no step
            undo_stack.pop_back();
        say(cx, std::string(d.name) + ": the library refused its default settings; not added");
        return 0;
    }
    sync_panels(cx);
    refresh_rack(cx);
    touch(cx);
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
                rv->scroll.to(cx, std::max(0.f, r.right() - vp.w + kHp) * vp.zoom,
                    std::max(0.f, r.y - kHp) * vp.zoom);
    }
    return id;
}

void Editor::remove_module(Ctx cx, uint32_t node)
{
    const Node *n = engine.patch().find(node);
    if (!n)
        return;
    const std::string name(n->desc->name);
    checkpoint(cx, "Delete " + name);
    if (selected_node == node)
        selected_node = 0;
    if (last_ctrl_node == node)
        last_ctrl_node = 0;
    engine.patch().remove_node(node);
    if (hover_node == node)
    {
        hover_node = 0;
        hover_text.clear();
    }
    engine.compile();
    sync_panels(cx);
    sync_cables(cx); // its cables went with it
    touch(cx);
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
    checkpoint(cx, "Move " + std::string(n->desc->name));
    n = engine.patch().find(node);
    n->x = (float)x;
    n->y = (float)y;
    refresh_rack(cx);
    refresh_cables_of(cx, node); // its cables follow
    touch(cx);
    return true;
}

void Editor::set_param(Ctx cx, uint32_t node, size_t param, float value)
{
    const Node *n = engine.patch().find(node);
    if (!n || param >= n->params.size())
        return;
    last_ctrl_node = node;
    last_ctrl_param = (int)param;
    checkpoint(cx, std::string(n->desc->name) + " " + std::string(n->desc->params[param].name),
        (uint64_t)1 << 63 | (uint64_t)node << 16
            | param); // a drag or a run of wheel notches: one step
    engine.set_param(node, param, value);
    touch(cx);
    auto it = panels.find(node);
    if (it != panels.end())
        it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    say(cx, describe(*n->desc, n->desc->params[param], n->params[param]));
    if (hover_node == node
        && hover_item == (int)param) // the control under the pointer: its new value
        hover_text = describe_item(node, hover_item);
}

void Editor::strip_key(Ctx cx, uint32_t node, int note, bool down)
{
    if (note < 0 || note > 127)
        return;
    engine.key(node, down, (uint8_t)note, 100);
    if (down)
        say(cx, "Note " + note_name(note) + (audio_running() ? "" : "  (audio is off)"));
}

void Editor::refresh_keyboards(Ctx cx)
{
    for (const Node &n : engine.patch().nodes())
        if (n.desc->id == "keyboard")
            if (auto it = panels.find(n.id); it != panels.end())
                it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
}

void Editor::set_step(Ctx cx, uint32_t node, size_t index, const StepData &step)
{
    const Node *n = engine.patch().find(node);
    if (!n || !n->desc->has_steps || index >= n->steps.size() || n->steps[index] == step)
        return;
    checkpoint(cx, std::string(n->desc->name) + " step " + std::to_string(index + 1),
        (uint64_t)1 << 62 | (uint64_t)node << 16 | index); // a drag on one step: one undo step
    engine.set_step(node, index, step);
    touch(cx);
    if (auto it = panels.find(node); it != panels.end())
        it->second.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    hover_node = node;
    hover_item = kHoverStep - (int)index;
    hover_text = describe_item(node, hover_item);
    hint_line.update(cx, [](HintLine &, Ctx c) { c.notify(); });
}

// ---- files

Editor::~Editor()
{
    try
    {
        write_recovery();
    }
    catch (...)
    { // a destructor must not throw; the patch is lost, as it would have been without this
    }
}

void Editor::touch(Ctx cx)
{
    version = ++next_version;
    const bool was = dirty;
    dirty = version != saved_version;
    if (dirty != was)
        toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
}

// ---- undo

void Editor::checkpoint(Ctx cx, std::string what, uint64_t merge)
{
    if (batch > 0)
        return; // part of a larger action, which has its own step
    if (merge && merge == merge_key && !undo_stack.empty())
        return; // the same control again: the step it is in already holds the value before
    undo_stack.push_back(Version { engine.patch(), version, std::move(what) });
    if (undo_stack.size() > kUndoLimit)
        undo_stack.erase(undo_stack.begin());
    redo_stack.clear();
    merge_key = merge;
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
}

void Editor::forget_view_state(Ctx cx)
{
    drag = CableDrag { };
    mdrag = ModuleDrag { };
    pdrag = PanelDrag { };
    hover_node = 0;
    hover_text.clear();
    file_menu_open = false;
    ask = Ask { };
    selected_node = 0;
    if (RackView *rv = rack.get(cx))
        rv->close_menus();
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
}

void Editor::restore(Ctx cx, Patch target)
{
    Patch old = std::move(engine.patch());
    engine.patch() = std::move(target);
    engine.compile();
    // a module that is still there keeps running; what it was set to before reaches it again
    for (const Node &n : engine.patch().nodes())
    {
        const Node *o = old.find(n.id);
        if (!o || o->desc != n.desc)
            continue; // (a module back from the dead was made from its settings)
        for (size_t i = 0; i < n.params.size(); i++)
            if (o->params[i] != n.params[i])
                engine.set_param(n.id, i, n.params[i]);
        for (size_t k = 0; k < n.steps.size() && k < o->steps.size(); k++)
            if (!(o->steps[k] == n.steps[k]))
                engine.set_step(n.id, k, n.steps[k]);
        if (o->device != n.device)
            engine.set_device(n.id, std::string(n.device));
    }
    forget_view_state(cx);
    sync_panels(cx);
    sync_cables(cx);
    for (auto &[id, view] : panels)
        view.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    refresh_rack(cx);
}

void Editor::undo(Ctx cx)
{
    if (undo_stack.empty())
    {
        say(cx, "Nothing to undo.");
        return;
    }
    Version v = std::move(undo_stack.back());
    undo_stack.pop_back();
    redo_stack.push_back(Version { engine.patch(), version, v.what });
    restore(cx, std::move(v.patch));
    version = v.version;
    dirty = version != saved_version;
    merge_key = 0;
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    say(cx, "Undone: " + v.what + ".");
}

void Editor::redo(Ctx cx)
{
    if (redo_stack.empty())
    {
        say(cx, "Nothing to redo.");
        return;
    }
    Version v = std::move(redo_stack.back());
    redo_stack.pop_back();
    undo_stack.push_back(Version { engine.patch(), version, v.what });
    restore(cx, std::move(v.patch));
    version = v.version;
    dirty = version != saved_version;
    merge_key = 0;
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    say(cx, "Redone: " + v.what + ".");
}

bool Editor::search_focused(Ctx cx)
{
    Browser *b = browser.get(cx);
    return b && b->search && cx.is_focused(Focus(b->search.focus(cx.app())));
}

void Editor::close_overlays(Ctx cx)
{
    learn_cancel(cx); // Escape
    rack.update(cx,
        [](RackView &r, Ctx c)
        {
            if (r.close_menus()) // a panel's, a cable's, a control's or a device menu
                c.notify();
        });
    if (!file_menu_open && !ask.shown && !device_menu_open)
        return;
    file_menu_open = false;
    device_menu_open = false;
    ask = Ask { };
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
}

void Editor::request(Ctx cx, Then then, int factory)
{
    file_menu_open = false;
    if (dirty && !engine.patch().nodes().empty())
    {
        ask = Ask { true, then, factory };
        overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
        say(cx, "Unsaved changes to " + patch_name + ": save them first?");
        return;
    }
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
    perform(cx, then, factory);
}

void Editor::answer(Ctx cx, int choice)
{
    const Ask a = ask;
    ask = Ask { };
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
    if (choice == 2)
    {
        say(cx, "Cancelled.");
        return;
    }
    if (choice == 1)
    {
        perform(cx, a.then, a.factory);
        return;
    }
    // save first, then go on: at once if the patch has a file, after the chooser if not
    if (!file_path.empty())
    {
        if (save_to(cx, file_path))
            perform(cx, a.then, a.factory);
        return;
    }
    after_save = a.then;
    after_save_factory = a.factory;
    save_as(cx);
}

void Editor::perform(Ctx cx, Then then, int factory)
{
    std::string err;
    switch (then)
    {
    case Then::Nothing:
        break;
    case Then::New:
        new_patch(cx);
        break;
    case Then::OpenDialog:
        if (chooser)
            chooser(cx, false, file_path);
        break;
    case Then::Factory:
    {
        const auto f = factory_patches();
        if (factory >= 0 && (size_t)factory < f.size())
        {
            const FactoryPatch &fp = f[(size_t)factory];
            // an example opens untitled: Save asks where, so the example itself is never
            // overwritten
            if (!open_text(cx, std::string(fp.text), std::string(fp.title) + " (example)", "",
                    false, &err))
                say(cx, err);
        }
        break;
    }
    case Then::Recover:
    {
        std::string       text;
        const std::string path = recovery_path(state_dir);
        if (!read_file(path, &text, &err)
            || !open_text(cx, text, "recovered patch", "", true, &err))
        {
            say(cx, err);
            break;
        }
        std::error_code ec;
        std::filesystem::remove(path, ec); // it is in the window now, unsaved
        say(cx,
            "Recovered the patch that was not saved when PulseRack last closed. Save it to keep "
            "it.");
        break;
    }
    }
}

namespace
{
    std::string with_extension(std::string path)
    {
        const std::string name = file_name(path);
        if (name.find('.') == std::string::npos)
            path += ".rack";
        return path;
    }
} // namespace

void Editor::new_patch(Ctx cx)
{
    std::string err;
    if (open_text(cx, "pulserack 1\n", "untitled", "", false, &err))
        say(cx, "A new, empty patch.");
}

void Editor::save(Ctx cx)
{
    file_menu_open = false;
    if (file_path.empty())
        save_as(cx);
    else
        save_to(cx, file_path);
}

void Editor::save_as(Ctx cx)
{
    file_menu_open = false;
    if (chooser)
        chooser(cx, true, file_path.empty() ? std::string("untitled.rack") : file_path);
}

bool Editor::save_to(Ctx cx, const std::string &path)
{
    std::string err;
    if (!write_file_atomic(path, save_patch(engine.patch()), &err))
    {
        say(cx, "Not saved: " + err + ".");
        after_save = Then::Nothing;
        return false;
    }
    file_path = path;
    patch_name = file_name(path);
    saved_version = version;
    dirty = false;
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    say(cx, "Saved " + patch_name + ".");
    return true;
}

void Editor::file_chosen(
    Ctx cx, bool saving, std::optional<std::string> path, const std::string &error)
{
    const Then then = after_save;
    const int  factory = after_save_factory;
    after_save = Then::Nothing;
    if (!path)
    {
        say(cx, error.empty() ? "Cancelled." : "No file dialog: " + error + ".");
        return;
    }
    std::string err;
    if (saving)
    {
        if (save_to(cx, with_extension(*path)))
            perform(cx, then, factory);
    }
    else if (!open_path(cx, *path, &err))
        say(cx, "Not opened: " + err + ".");
}

bool Editor::open_path(Ctx cx, const std::string &path, std::string *error)
{
    std::string text;
    if (!read_file(path, &text, error))
        return false;
    return open_text(cx, text, file_name(path), path, false, error);
}

bool Editor::open_text(Ctx cx, const std::string &text, const std::string &name,
    const std::string &path, bool dirty_after, std::string *error)
{
    Patch            loaded;
    const LoadResult r = load_patch(text, loaded);
    if (!r.ok)
    {
        *error = name + ": " + r.error;
        return false;
    }
    const int moved = resolve_overlaps(loaded);
    // swap it in; if the library refuses a module's settings, the old patch comes back
    Patch old = std::move(engine.patch());
    engine.patch() = std::move(loaded);
    if (engine.compile() != Err::Ok)
    {
        *error
            = name + ": module " + std::to_string(engine.failed_node()) + " refused its settings";
        engine.patch() = std::move(old);
        engine.compile();
        return false;
    }
    // what the view held about the old patch goes
    drag = CableDrag { };
    mdrag = ModuleDrag { };
    pdrag = PanelDrag { };
    selected.reset();
    hover_node = 0;
    hover_text.clear();
    leds.clear();
    cable_colour.clear(); // colours in file order again
    next_colour = 0;
    file_menu_open = false;
    ask = Ask { };
    if (RackView *rv = rack.get(cx))
    {
        rv->close_menus();
        rv->scroll.to(cx, 0, 0);
    }
    file_path = path;
    patch_name = name;
    undo_stack.clear(); // a new history for a new document
    redo_stack.clear();
    merge_key = 0;
    version = ++next_version;
    saved_version = dirty_after ? 0 : version;
    dirty = dirty_after;
    sync_panels(cx);
    sync_cables(cx);
    for (auto &[id, view] : panels) // a kept id may now be another module
        view.update(cx, [](PanelView &, Ctx c) { c.notify(); });
    overlay.update(cx, [](Overlay &, Ctx c) { c.notify(); });
    drag_view.update(cx, [](DragView &, Ctx c) { c.notify(); });
    toolbar.update(cx, [](Toolbar &, Ctx c) { c.notify(); });
    std::string msg = "Opened " + name + ".";
    if (moved)
        msg += "  " + std::to_string(moved) + " panel(s) moved apart: they overlapped.";
    if (!r.warnings.empty())
        msg += "  " + std::to_string(r.warnings.size())
            + " warning(s), the first: " + r.warnings[0];
    say(cx, msg);
    return true;
}

void Editor::write_recovery() const
{
    const std::string path = recovery_path(state_dir);
    if (path.empty() || !dirty || engine.patch().nodes().empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(state_dir, ec);
    std::string err;
    (void)write_file_atomic(path,
        "# Kept when PulseRack closed with unsaved changes to " + patch_name + ".\n"
            + save_patch(engine.patch()),
        &err);
}

bool Editor::has_recovery() const
{
    const std::string path = recovery_path(state_dir);
    std::error_code   ec;
    return !path.empty() && std::filesystem::exists(path, ec);
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
    e->overlay = app.new_view<Overlay>();
    app.get(e->overlay)->ed = ed;
    e->state_dir = opt.state_dir;
    e->config_dir = opt.config_dir;
    {
        std::vector<std::string> warnings;
        std::string              err;
        e->settings = load_settings(opt.config_dir, &warnings, &err);
        e->audio_device = e->settings.output;
        for (const std::string &w : warnings)
            std::fprintf(stderr, "pulserack: %s\n", w.c_str());
        if (!err.empty())
            std::fprintf(stderr, "pulserack: %s\n", err.c_str());
    }
    e->chooser = [ed](Ctx cx, bool save, const std::string &suggested)
    { native_file_dialog(cx, ed, save, suggested); };
    // the file commands' keys, and Escape for the menu and the question
    app.bind_key("ctrl-n", "file::new");
    app.bind_key("ctrl-o", "file::open");
    app.bind_key("ctrl-s", "file::save");
    app.bind_key("ctrl-shift-s", "file::save_as");
    app.bind_key("escape", "ui::escape");
    app.bind_key("ctrl-z", "edit::undo");
    app.bind_key("ctrl-shift-z", "edit::redo");
    app.bind_key("ctrl-y", "edit::redo");
    app.bind_key("ctrl-0", "view::zoom_reset");
    app.on_global_action("view::zoom_reset",
        [ed](Ctx cx)
        {
            if (Editor *e = ed.get(cx))
            { // about the middle of the view
                const RackViewport vp = rack_viewport(cx, ed);
                const gp_point     o = rack_origin();
                e->set_zoom(cx, 1, { o.x + vp.w * vp.zoom / 2, o.y + vp.h * vp.zoom / 2 });
            }
        });
    app.on_global_action("edit::undo",
        [ed](Ctx cx)
        {
            if (Editor *e = ed.get(cx);
                e && !e->search_focused(cx)) // the search box's own text, then
                e->undo(cx);
        });
    app.on_global_action("edit::redo",
        [ed](Ctx cx)
        {
            if (Editor *e = ed.get(cx); e && !e->search_focused(cx))
                e->redo(cx);
        });
    app.on_global_action("file::new", [ed](Ctx cx) { ed.get(cx)->request(cx, Editor::Then::New); });
    app.on_global_action(
        "file::open", [ed](Ctx cx) { ed.get(cx)->request(cx, Editor::Then::OpenDialog); });
    app.on_global_action("file::save", [ed](Ctx cx) { ed.get(cx)->save(cx); });
    app.on_global_action("file::save_as", [ed](Ctx cx) { ed.get(cx)->save_as(cx); });
    app.on_global_action("ui::escape", [ed](Ctx cx) { ed.get(cx)->close_overlays(cx); });
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

    // A MIDI module's device is a name in the patch; the device manager gives each name its slot
    // (set before a patch is loaded; with no MIDI a named device is not heard).
    e->engine.midi_slot_of = [e]([[maybe_unused]] const std::string &name) -> int
    {
#if PULSERACK_MIDI
        if (e->midi)
            return e->midi->slot_for(name);
#endif
        return -1;
    };
    if (!opt.patch_path.empty() && !e->open_path(app.ctx(), opt.patch_path, error))
        return { };
    if (e->has_recovery())
        e->status
            = "A patch with unsaved changes was kept when PulseRack last closed: Open > Recover.";

    auto root = app.new_view<Root>();
    app.get(root)->ed = ed;
    gp_window_options w { };
    w.title = "PulseRack";
    w.width = (int)st::kWindowW;
    w.height = (int)st::kWindowH;
    app.open_window(w, root);
    e->engine.set_midi(e->midi_boxes.get()); // (no audio thread yet)
    if (opt.midi)
    {
        e->start_midi(app.ctx(ed));
        e->engine.refresh_devices(); // the patch's device names have slots now
    }
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
    // a point of the panel (in panel pixels) on the screen
    auto at = [&](float x, float y) -> gp_point
    {
        return { o.x + (n->x * kHp + x - vp.x) * vp.zoom, o.y + (n->y * kHp + y - vp.y) * vp.zoom };
    };
    if (param < 0)
        return at(n->desc->width_hp * kHp / 2, kTitleHeight / 2);
    const PanelLayout &L = panel_layout(*n->desc);
    const Rect        &r = L.params[(size_t)param];
    if (n->desc->params[(size_t)param].control == Control::Knob)
        return at(kPanelBorder + r.x + r.w / 2, kPanelBorder + r.y + kKnobSize / 2);
    return at(kPanelBorder + r.x + r.w / 2, kPanelBorder + r.y + r.h / 2);
}

gp_point control_menu_item(Ctx cx, Entity<Editor> ed, int index)
{
    Editor     *e = ed.get(cx);
    RackView   *rv = e ? e->rack.get(cx) : nullptr;
    const Node *n = rv && rv->ctrl_node ? e->engine.patch().find(rv->ctrl_node) : nullptr;
    if (!n)
        return { -1, -1 };
    const RackViewport vp = viewport_of(cx, *e);
    const gp_point     o = rack_origin();
    const Pt           at = control_menu_origin(*n, rv->ctrl_param);
    return { o.x + (at.x + kControlMenuW / 2 - vp.x) * vp.zoom,
        o.y + (at.y + kControlMenuPad + ((float)index + 0.5f) * kMenuItemH - vp.y) * vp.zoom };
}

bool rack_menu_open(Ctx cx, Entity<Editor> ed)
{
    Editor   *e = ed.get(cx);
    RackView *rv = e ? e->rack.get(cx) : nullptr;
    return rv && (rv->menu_node || rv->ctrl_node || rv->dev_node || rv->menu_cable);
}

gp_point device_box_center(Ctx cx, Entity<Editor> ed, uint32_t node)
{
    Editor     *e = ed.get(cx);
    const Node *n = e ? e->engine.patch().find(node) : nullptr;
    if (!n || !midi_sink(*n->desc))
        return { -1, -1 };
    const RackViewport vp = viewport_of(cx, *e);
    const gp_point     o = rack_origin();
    const Rect         r = device_box(panel_layout(*n->desc));
    return { o.x + (n->x * kHp + kPanelBorder + r.x + r.w / 2 - vp.x) * vp.zoom,
        o.y + (n->y * kHp + kPanelBorder + r.y + r.h / 2 - vp.y) * vp.zoom };
}

gp_point device_menu_item(Ctx cx, Entity<Editor> ed, int index)
{
    Editor     *e = ed.get(cx);
    RackView   *rv = e ? e->rack.get(cx) : nullptr;
    const Node *n = rv && rv->dev_node ? e->engine.patch().find(rv->dev_node) : nullptr;
    if (!n || index < 0 || (size_t)index > e->device_menu_names(n->id).size())
        return { -1, -1 };
    const RackViewport vp = viewport_of(cx, *e);
    const gp_point     o = rack_origin();
    const Pt           at = device_menu_origin(*n);
    return { o.x + (at.x + kDeviceMenuW / 2 - vp.x) * vp.zoom,
        o.y + (at.y + kControlMenuPad + ((float)index + 0.5f) * kMenuItemH - vp.y) * vp.zoom };
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
    return { o.x + (n->x * kHp - vp.x + kPanelBorder + j.x + j.w / 2) * vp.zoom,
        o.y + (n->y * kHp - vp.y + kPanelBorder + j.y + j.h / 2) * vp.zoom };
}

} // namespace rack::ui
