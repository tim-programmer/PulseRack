// The application's window: a toolbar, the module browser, the rack of panels with the cables
// between them, and the hint line, written with gpuic's C++ layer. The GUI is a view of the engine
// (docs/gui.md): every edit goes through the Editor below, which changes the patch and the running
// modules together.
#pragma once

#include "audio/audio_out.h"
#include "engine/engine.h"
#include "ui/geometry.h"

#include <gpuic/gpuic.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rack::ui
{

struct RackView;
struct PanelView;
struct Browser;
struct Toolbar;
struct HintLine;
struct CableView;
struct DragView;
struct PointerChip;

// The model of the window: the engine, the patch's name, the hint line's text, and the handles of
// the views that show them. An entity, so handlers anywhere reach it through a handle.
struct Editor
{
    Engine engine { 48000 }; // made again at the audio device's rate when it starts
    // The sound card, declared after the engine so it stops before the engine goes (and the Editor
    // is an entity, so both go before gpuic's SDL_Quit). Null until audio is first started.
    std::unique_ptr<audio::AudioOut> audio;
    std::string                      audio_error; // why audio is not running, if it failed
    gpuic::Task                      meter_task;  // 30 Hz while audio runs: LEDs and meters

    // What the panels show from telemetry, so that only a change re-renders a panel.
    struct Led
    {
        int  level = 0; // 0 dark .. 3 bright
        bool dropped = false;
    };
    std::map<uint32_t, Led> leds;
    std::array<int, 2>      meter { 0, 0 }; // the output level, 0 .. 40 (the Audio Output panels)
    int                     load_pct = -1;  // the toolbar's CPU figure

    // The computer keyboard as a piano: the key's note, held until its key comes up.
    int                            octave = 0; // Z and X: down and up, -4 .. +4 around C4
    std::map<std::string, uint8_t> held;

    std::string patch_name = "untitled";
    std::string
        status; // what the last action did; the hint line shows it when the pointer is over nothing
    // What the pointer is over: a node's control (item >= 0), its title bar (kHoverTitle) or a jack
    // (kHoverPort - port); hover_text describes it, and the hint line shows it while it is not
    // empty.
    // A cable is hovered as (its input's node, kHoverCable - its input's port).
    static constexpr int kHoverTitle = -1, kHoverPort = -2, kHoverCable = -1000;
    uint32_t             hover_node = 0;
    int                  hover_item = 0;
    std::string          hover_text;

    gpuic::Entity<Editor>                        self;
    gpuic::Entity<RackView>                      rack;
    gpuic::Entity<Browser>                       browser;
    gpuic::Entity<Toolbar>                       toolbar;
    gpuic::Entity<HintLine>                      hint_line;
    std::map<uint32_t, gpuic::Entity<PanelView>> panels; // one cached view per node, by node id

    // ---- cables. A cable is known by its input (an input takes one cable).
    using CableKey = std::pair<uint32_t, uint16_t>;
    std::map<CableKey, gpuic::Entity<CableView>> cable_views;  // one cached view per cable
    std::map<CableKey, int>                      cable_colour; // its palette entry, given in turn
    int                                          next_colour = 0;
    std::optional<PortRef>                       selected; // the selected cable's input
    gpuic::Entity<DragView>                      drag_view;
    struct CableDrag
    {
        bool                   active = false;
        PortRef                fixed;             // the jack the cable hangs from
        Pt                     loose;             // the other end, at the pointer (rack pixels)
        std::optional<PortRef> over;              // the jack under the pointer
        Err                    verdict = Err::Ok; // what connecting there would say
        std::vector<PortRef>   targets; // every jack it could connect to (lit while dragging)
        std::optional<Cable>   picked;  // the cable lifted off an input, if it began that way
    } drag;

    // ---- a module dragged from the browser onto the rack
    struct ModuleDrag
    {
        bool              active = false; // a card is pressed
        bool              moved = false;  // ... and has moved far enough to be a drag
        const ModuleDesc *desc = nullptr;
        gp_point          start { }, at { }; // window pixels
        bool              over_rack = false;
        int               x = 0, y = 0; // where it would go (rack units)
        bool              fits = false;
    } mdrag;
    gpuic::Entity<PointerChip> chip; // the module's name following the pointer

    // ---- edits (each updates the patch, the engine and the views that show it)
    // Adds a module at the first free place near the visible part of the rack; 0 if the library
    // refused its settings.
    uint32_t add_module(gpuic::Ctx cx, const ModuleDesc &d);
    // Adds a module at a given place (rack units); 0 if the place is taken.
    uint32_t add_module_at(gpuic::Ctx cx, const ModuleDesc &d, int x, int y);
    void     remove_module(gpuic::Ctx cx, uint32_t node);
    bool     move_module(gpuic::Ctx cx, uint32_t node, int x, int y); // false: would overlap
    void     set_param(gpuic::Ctx cx, uint32_t node, size_t param, float value);
    void     say(gpuic::Ctx cx, std::string text); // the status the hint line falls back to
    // The pointer entered (inside) or left a node's control, title or jack (items as above).
    void hover(gpuic::Ctx cx, uint32_t node, int item, bool inside);
    // What the hint line shows: while a cable is dragged, what releasing it would do; otherwise
    // what is under the pointer, or the last action.
    std::string hint() const
    {
        return drag.active || mdrag.moved || hover_text.empty() ? status : hover_text;
    }
    std::string describe_item(uint32_t node, int item) const;

    // Cables: either end may be given first (a drag can start at an input). Each compiles the
    // engine.
    Err  connect(gpuic::Ctx cx, PortRef a, PortRef b);
    void disconnect(gpuic::Ctx cx, PortRef to);
    void select_cable(gpuic::Ctx cx, std::optional<PortRef> to);
    void delete_selected(gpuic::Ctx cx);
    // A press on a jack starts a cable: from an output or an empty input a new one, from a
    // connected input its cable is lifted off. Positions are window pixels.
    void jack_press(gpuic::Ctx cx, PortRef jack, gp_point window);
    void jack_drag(gpuic::Ctx cx, gp_point window);
    void jack_release(gpuic::Ctx cx, gp_point window);
    // A press on a browser card, then moves: past a few pixels it becomes a drag; released on a
    // free place on the rack it adds the module there.
    void module_drag_begin(gpuic::Ctx cx, const ModuleDesc &d, gp_point window);
    void module_drag_move(gpuic::Ctx cx, gp_point window);
    void module_drag_end(gpuic::Ctx cx, gp_point window);
    // Where a module dragged by its title's middle would land, for a pointer at `window`.
    Slot drop_slot(gpuic::Ctx cx, const ModuleDesc &d, gp_point window);

    Pt          to_rack(gpuic::Ctx cx, gp_point window); // window pixels -> rack pixels
    std::string port_name(PortRef r) const;              // "Oscillator OUT"

    // Replaces the patch with a file's; false and `error` set if it does not load.
    bool load_file(gpuic::Ctx cx, const std::string &path, std::string *error);

    // ---- sound (milestone 4)
    bool        start_audio(gpuic::Ctx cx); // false: no device (audio_error says why)
    void        stop_audio(gpuic::Ctx cx);
    bool        audio_running() const { return audio && audio->running(); }
    std::string audio_status() const; // for the toolbar
    void        tick(gpuic::Ctx cx);  // takes the telemetry and updates what changed
    // A key of the computer keyboard (down or up); false if it is not the piano's (it goes on).
    bool piano(gpuic::Ctx cx, const gp_key_event &ev, bool down);

    // ---- for the views
    void sync_panels(gpuic::Ctx cx); // a view for every node, none for nodes that are gone
    void sync_cables(gpuic::Ctx cx); // the same for cables
    void refresh_cables_of(gpuic::Ctx cx, uint32_t node); // after a panel moved
    void refresh_rack(gpuic::Ctx cx);
};

struct Options
{
    std::string patch_path; // empty: an empty rack
    bool audio = true;      // open the sound card at start (the tests drive the engine themselves)
};

// Builds the views and opens the window. Returns the editor (tests drive it), or a null handle and
// `error` if the patch did not load.
gpuic::Entity<Editor> open_rack(gpuic::App app, const Options &opt, std::string *error);

// Where things are, in window pixels, for tests and for the browser's placement of new modules.
// The rack's top-left corner in the window.
gp_point rack_origin();
// The visible rack area: scroll offset (pixels) and size.
struct RackViewport
{
    float x = 0, y = 0, w = 0, h = 0;
};
RackViewport rack_viewport(gpuic::Ctx cx, gpuic::Entity<Editor> ed);
// The window position of a node's control or jack centre, or of its title bar (param = -1).
gp_point control_center(gpuic::Ctx cx, gpuic::Entity<Editor> ed, uint32_t node, int param);
gp_point port_center(gpuic::Ctx cx, gpuic::Entity<Editor> ed, uint32_t node, size_t port);

} // namespace rack::ui
