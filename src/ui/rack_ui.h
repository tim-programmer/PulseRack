// The application's window (milestone 2): a toolbar, the module browser, the rack of panels and the
// hint line, written with gpuic's C++ layer. The GUI is a view of the engine (docs/gui.md): every
// edit goes through the Editor below, which changes the patch and the running modules together.
#pragma once

#include "engine/engine.h"

#include <gpuic/gpuic.hpp>

#include <map>
#include <string>

namespace rack::ui
{

struct RackView;
struct PanelView;
struct Browser;
struct Toolbar;
struct HintLine;

// The model of the window: the engine, the patch's name, the hint line's text, and the handles of
// the views that show them. An entity, so handlers anywhere reach it through a handle.
struct Editor
{
    Engine      engine { 48000 }; // no audio device yet (milestone 4): the nominal rate
    std::string patch_name = "untitled";
    std::string
        status; // what the last action did; the hint line shows it when the pointer is over nothing
    // What the pointer is over: a node's control (item >= 0), its title bar (kHoverTitle) or a jack
    // (kHoverPort - port); hover_text describes it, and the hint line shows it while it is not
    // empty.
    static constexpr int kHoverTitle = -1, kHoverPort = -2;
    uint32_t             hover_node = 0;
    int                  hover_item = 0;
    std::string          hover_text;

    gpuic::Entity<Editor>                        self;
    gpuic::Entity<RackView>                      rack;
    gpuic::Entity<Browser>                       browser;
    gpuic::Entity<Toolbar>                       toolbar;
    gpuic::Entity<HintLine>                      hint_line;
    std::map<uint32_t, gpuic::Entity<PanelView>> panels; // one cached view per node, by node id

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
    void        hover(gpuic::Ctx cx, uint32_t node, int item, bool inside);
    std::string hint() const
    {
        return hover_text.empty() ? status : hover_text;
    } // what the hint line shows
    std::string describe_item(uint32_t node, int item) const;

    // Replaces the patch with a file's; false and `error` set if it does not load.
    bool load_file(gpuic::Ctx cx, const std::string &path, std::string *error);

    // ---- for the views
    void sync_panels(gpuic::Ctx cx); // a view for every node, none for nodes that are gone
    void refresh_rack(gpuic::Ctx cx);
};

struct Options
{
    std::string patch_path; // empty: an empty rack
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
