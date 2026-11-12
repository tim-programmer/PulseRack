// The application's window: a toolbar, the module browser, the rack of panels with the cables
// between them, and the hint line, written with gpuic's C++ layer. The GUI is a view of the engine
// (docs/gui.md): every edit goes through the Editor below, which changes the patch and the running
// modules together.
#pragma once

#include "audio/audio_out.h"
#include "engine/engine.h"
#include "midi/inbox.h"
#if PULSERACK_MIDI
#include "midi/inputs.h"
#endif
#include "ui/geometry.h"
#include "ui/settings.h"

#include <gpuic/gpuic.hpp>

#include <array>
#include <functional>
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
struct Overlay;

// The model of the window: the engine, the patch's name, the hint line's text, and the handles of
// the views that show them. An entity, so handlers anywhere reach it through a handle.
struct Editor
{
    Engine engine { 48000 }; // made again at the audio device's rate when it starts
    ~Editor();               // keeps an unsaved patch (see write_recovery)
    // MIDI (docs/midi.md): where the devices' messages wait for the engine, then the devices that
    // put them there. Both are declared before the sound card: its thread reads the inboxes, so
    // they are destroyed after it has stopped (and the devices before the inboxes they push into).
    std::unique_ptr<midi::Inboxes> midi_boxes = std::make_unique<midi::Inboxes>();
#if PULSERACK_MIDI
    std::unique_ptr<midi::Inputs> midi; // null: MIDI is off, or the system has none (midi_error)
#endif
    std::string              midi_error;
    std::vector<std::string> midi_available; // the inputs, as listed when the device menu opened
    gpuic::Task              midi_task;      // 4 Hz: follows devices being plugged in and out
    void                     start_midi(gpuic::Ctx cx); // opens the settings' inputs
    void                     poll_midi(gpuic::Ctx cx);
    // Switches an input on or off (the device menu): the settings file, then the device.
    void toggle_midi_input(gpuic::Ctx cx, const std::string &name);
    // What the device menu lists: the inputs present, then those switched on but not plugged in.
    std::vector<std::string> midi_menu_names() const;
    bool                     midi_input_open(const std::string &name) const;
    int                      midi_open_count() const;
    // The sound card, declared after the engine so it stops before the engine goes (and the Editor
    // is an entity, so both go before gpuic's SDL_Quit). Null until audio is first started.
    std::unique_ptr<audio::AudioOut> audio;
    std::string                      audio_error; // why audio is not running, if it failed
    std::string audio_device; // the output chosen, by name (empty: the system's default)
    std::vector<audio::OutputDevice> devices; // as listed when the device menu opened
    bool                             device_menu_open = false;
    uint32_t                         shown_dropouts = 0; // what the toolbar shows
    gpuic::Task                      meter_task;         // 30 Hz while audio runs: LEDs and meters

    // What the panels show from telemetry, so that only a change re-renders a panel.
    struct Led
    {
        int  level = 0; // 0 dark .. 3 bright
        bool dropped = false;
    };
    std::map<uint32_t, Led> leds;
    std::array<int, 2>      meter { 0, 0 }; // the output level, 0 .. 40 (the Audio Output panels)
    int                     load_pct = -1;  // the toolbar's CPU figure
    std::map<uint32_t, int> playhead;       // a sequencer's step being played, as its grid shows it
    std::map<uint32_t, ScopeFrame> scopes;  // each oscilloscope's newest frame (taken at 30 Hz)

    // The computer keyboard as a piano: the key's note, held until its key comes up.
    int                            octave = 0; // Z and X: down and up, -4 .. +4 around C4
    std::map<std::string, uint8_t> held;

    std::string patch_name = "untitled";
    std::string file_path;     // where Save writes; empty: untitled (Save asks)
    bool        dirty = false; // changed since it was opened or saved
    std::string state_dir;     // where an unsaved patch is kept at close; empty: nowhere
    std::string config_dir;    // where the settings file is; empty: none (the tests)
    Settings    settings;      // this machine's (ui/settings.h); saved whenever one changes
    std::string
        status; // what the last action did; the hint line shows it when the pointer is over nothing
    // What the pointer is over: a node's control (item >= 0), its title bar (kHoverTitle) or a jack
    // (kHoverPort - port); hover_text describes it, and the hint line shows it while it is not
    // empty.
    // A cable is hovered as (its input's node, kHoverCable - its input's port).
    // A sequencer's step is hovered as (its node, kHoverStep - the step's index).
    static constexpr int kHoverTitle = -1, kHoverPort = -2, kHoverCable = -1000, kHoverStep = -2000,
                         kHoverStrip = -3000; // the Keyboard panel's piano
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
        std::optional<Cable>   picked;  // the cable lifted off a jack, if it began that way
        bool                   lift_output = false; // ... off its output (it hangs from its input)
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

    // ---- files (milestone 5)
    gpuic::Entity<Overlay>
         overlay; // the Open menu and the unsaved-changes question, over the window
    bool file_menu_open = false;
    enum class Then : uint8_t // what to do once unsaved changes are dealt with
    {
        Nothing,
        New,
        OpenDialog,
        Factory,
        Recover
    };
    struct Ask
    {
        bool shown = false;
        Then then = Then::Nothing;
        int  factory = -1;
    } ask;
    Then after_save = Then::Nothing; // a Save asked for by that question, then this
    int  after_save_factory = -1;
    // Asks the user for a file: `save` for a place to save to. The answer comes back through
    // file_chosen (now or later, on this thread). The window uses SDL's native dialogs; the tests
    // put their own here.
    std::function<void(gpuic::Ctx, bool save, const std::string &suggested)> chooser;

    // ---- a panel moved on the rack: it follows the pointer freely (its model position is the free
    // one meanwhile, so its cables follow too); the outline shows the snapped place it would land
    // on, and the release puts it there if that is free, or back where it was.
    struct PanelDrag
    {
        bool     active = false, moved = false;
        uint32_t node = 0;
        gp_point start { };      // window pixels, at the press
        Pt       grab { };       // the pointer's offset in the panel (rack pixels)
        float    x0 = 0, y0 = 0; // where it was (rack units)
        int      x = 0, y = 0;   // where it would land
        bool     fits = false;
    } pdrag;

    // ---- edits (each updates the patch, the engine and the views that show it)
    // Adds a module at the first free place near the visible part of the rack; 0 if the library
    // refused its settings.
    uint32_t add_module(gpuic::Ctx cx, const ModuleDesc &d);
    // Adds a module at a given place (rack units); 0 if the place is taken.
    uint32_t add_module_at(gpuic::Ctx cx, const ModuleDesc &d, int x, int y);
    void     remove_module(gpuic::Ctx cx, uint32_t node);
    bool     move_module(gpuic::Ctx cx, uint32_t node, int x, int y); // false: would overlap
    void     set_param(gpuic::Ctx cx, uint32_t node, size_t param, float value);
    // A sequencer's step (milestone 6): the model, the running module, one undo step per step
    // edited.
    void set_step(gpuic::Ctx cx, uint32_t node, size_t index, const StepData &step);
    // A key of a Keyboard panel's piano, pressed or released (that module only).
    void strip_key(gpuic::Ctx cx, uint32_t node, int note, bool down);
    void refresh_keyboards(gpuic::Ctx cx);     // their pianos show the notes the computer keys hold
    void say(gpuic::Ctx cx, std::string text); // the status the hint line falls back to
    // The pointer entered (inside) or left a node's control, title or jack (items as above).
    void hover(gpuic::Ctx cx, uint32_t node, int item, bool inside);
    // What the hint line shows: while a cable is dragged, what releasing it would do; otherwise
    // what is under the pointer, or the last action.
    std::string hint() const
    {
        return drag.active || mdrag.moved || pdrag.moved || hover_text.empty() ? status
                                                                               : hover_text;
    }
    std::string describe_item(uint32_t node, int item) const;

    // Cables: either end may be given first (a drag can start at an input). Each compiles the
    // engine.
    Err  connect(gpuic::Ctx cx, PortRef a, PortRef b);
    void disconnect(gpuic::Ctx cx, PortRef to);
    void select_cable(gpuic::Ctx cx, std::optional<PortRef> to);
    // A module selected by a click on its panel (a cable or a module is selected, not both); Delete
    // removes whichever is.
    // While a cable, a panel or a module from the browser is dragged, the pointer near the rack's
    // edge scrolls it (and the drag follows): a 60 Hz task while any drag is on.
    gp_point    last_pointer { };
    gpuic::Task edge_task;
    void        edge_scroll_start(gpuic::Ctx cx, gp_point window);
    void        edge_tick(gpuic::Ctx cx);
    uint32_t    selected_node = 0;
    // The rack's magnification (the wheel over the rack; Ctrl+0 puts it back to 1). The model stays
    // in rack pixels: only RackView scales, and to_rack() and the viewport divide by it.
    float                  zoom = 1;
    static constexpr float kZoomMin = 0.4f, kZoomMax = 2.5f;
    void set_zoom(gpuic::Ctx cx, float z, gp_point about_window); // keeps that point where it is
    void select_node(gpuic::Ctx cx, uint32_t node);               // 0: none
    // The arrow keys turn the control under the pointer, else the last one touched.
    uint32_t last_ctrl_node = 0;
    int      last_ctrl_param = -1;
    bool     nudge(gpuic::Ctx cx, int steps, bool fine); // false: no control to turn
    void     delete_selected(gpuic::Ctx cx);
    // A press on a jack starts a cable: from an output or an empty input a new one, from a
    // connected input its cable is lifted off. Positions are window pixels.
    // Shift on a plugged output lifts its newest cable by the output end (a plain drag from an
    // output always makes a new cable).
    void jack_press(gpuic::Ctx cx, PortRef jack, gp_point window, uint32_t mods = 0);
    void jack_drag(gpuic::Ctx cx, gp_point window);
    void jack_release(gpuic::Ctx cx, gp_point window);
    // A press on a browser card, then moves: past a few pixels it becomes a drag; released on a
    // free place on the rack it adds the module there.
    void module_drag_begin(gpuic::Ctx cx, const ModuleDesc &d, gp_point window);
    void module_drag_move(gpuic::Ctx cx, gp_point window);
    void module_drag_end(gpuic::Ctx cx, gp_point window);
    // A press on a panel's title or face: past a few pixels it becomes a move (see PanelDrag).
    void panel_drag_begin(gpuic::Ctx cx, uint32_t node, gp_point window);
    void panel_drag_move(gpuic::Ctx cx, gp_point window);
    void panel_drag_end(gpuic::Ctx cx, gp_point window);
    // Where a module dragged by its title's middle would land, for a pointer at `window`.
    Slot drop_slot(gpuic::Ctx cx, const ModuleDesc &d, gp_point window);

    Pt          to_rack(gpuic::Ctx cx, gp_point window); // window pixels -> rack pixels
    std::string port_name(PortRef r) const;              // "Oscillator OUT"

    // ---- undo (milestone 6). Each user action stores the patch as it was before; undo puts it
    // back (modules that are still there keep running, and every changed parameter or step reaches
    // them). Consecutive changes of one control are one step. Opening a file starts a new history.
    struct Version
    {
        Patch       patch;
        uint64_t    version;
        std::string what; // "Delete Oscillator", for the hint line
    };
    std::vector<Version> undo_stack, redo_stack;
    uint64_t version = 0, saved_version = 0, next_version = 0; // dirty: version != saved
    uint64_t merge_key = 0; // the control the last step is about (0: none)
    int      batch = 0;     // inside an action made of others: no steps of their own
    static constexpr size_t kUndoLimit = 200;
    void                    checkpoint(gpuic::Ctx cx, std::string what, uint64_t merge = 0);
    void                    undo(gpuic::Ctx cx);
    void                    redo(gpuic::Ctx cx);
    void restore(gpuic::Ctx cx, Patch target); // the patch, with the running modules following it
    void forget_view_state(gpuic::Ctx cx);     // drags, menus, selection, hover
    bool search_focused(gpuic::Ctx cx);        // keys are the search box's then

    // ---- files. Each of New, Open, a factory patch and Recover asks first if there are unsaved
    // changes (the `ask` question); perform() does it without asking.
    void request(gpuic::Ctx cx, Then then, int factory = -1);
    void answer(gpuic::Ctx cx, int choice); // the question: 0 save, 1 do not save, 2 cancel
    void perform(gpuic::Ctx cx, Then then, int factory);
    void new_patch(gpuic::Ctx cx);
    void save(gpuic::Ctx cx);    // to file_path, or asks where if untitled
    void save_as(gpuic::Ctx cx); // asks where
    bool save_to(gpuic::Ctx cx, const std::string &path);
    // What the chooser answers: a path, or nothing (cancelled); `error` if the dialog failed.
    void file_chosen(
        gpuic::Ctx cx, bool save, std::optional<std::string> path, const std::string &error = { });
    // Replaces the patch with a file's; false and `error` set (nothing changed) if it does not
    // load.
    bool open_path(gpuic::Ctx cx, const std::string &path, std::string *error);
    bool open_text(gpuic::Ctx cx, const std::string &text, const std::string &name,
        const std::string &path, bool dirty_after, std::string *error);
    void touch(gpuic::Ctx cx); // something changed: the patch is unsaved
    // The unsaved patch, kept in state_dir when the window closes (gpuic cannot ask before it
    // does).
    void write_recovery() const;
    bool has_recovery() const;
    void close_overlays(gpuic::Ctx cx);

    // ---- sound (milestone 4)
    bool        start_audio(gpuic::Ctx cx); // false: no device (audio_error says why)
    void        stop_audio(gpuic::Ctx cx);
    void        open_device_menu(gpuic::Ctx cx);
    void        choose_device(gpuic::Ctx cx, const std::string &device); // and restart audio on it
    bool        audio_running() const { return audio && audio->running(); }
    std::string audio_status() const; // for the toolbar
    std::string midi_status() const;  // "  ·  MIDI 2" while inputs are open, else empty
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
    std::string state_dir;  // where an unsaved patch is kept at close; empty: nowhere (the tests)
    std::string config_dir; // where the settings file is; empty: none, the defaults (the tests)
    bool        midi = false; // open the MIDI inputs the settings name (the tests feed the engine)
};

// Builds the views and opens the window. Returns the editor (tests drive it), or a null handle and
// `error` if the patch did not load.
gpuic::Entity<Editor> open_rack(gpuic::App app, const Options &opt, std::string *error);

// Where things are, in window pixels, for tests and for the browser's placement of new modules.
// The rack's top-left corner in the window.
gp_point rack_origin();
// The visible rack area, in rack pixels (what the model uses): its top left corner and size. On the
// screen it is w * zoom by h * zoom window pixels.
struct RackViewport
{
    float x = 0, y = 0, w = 0, h = 0;
    float zoom = 1;
};
RackViewport rack_viewport(gpuic::Ctx cx, gpuic::Entity<Editor> ed);
// The window position of a node's control or jack centre, or of its title bar (param = -1).
gp_point control_center(gpuic::Ctx cx, gpuic::Entity<Editor> ed, uint32_t node, int param);
gp_point port_center(gpuic::Ctx cx, gpuic::Entity<Editor> ed, uint32_t node, size_t port);

} // namespace rack::ui
