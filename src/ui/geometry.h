// Geometry of the rack, without any window: where a panel's controls and jacks sit, how a knob's
// travel maps to a value, how a value is written, and where panels may go. Computed from the
// descriptors alone, so a new module gets a panel without UI code (docs/gui.md), cables can find a
// jack from the model without waiting for a layout pass (milestone 3), and all of it is tested
// headless.
#pragma once

#include "engine/model/patch.h"

#include <array>
#include <optional>

#include <string>
#include <vector>

namespace rack::ui
{

// ---- units. A position in the patch is in rack units (HP) on both axes; a panel is width_hp wide
// and kPanelHeightHp high, so panels snap to a 16 px grid.
inline constexpr float kHp = 16;
inline constexpr int   kPanelHeightHp = 26;
inline constexpr float kPanelHeight = kPanelHeightHp * kHp; // 416 px
inline constexpr float kTitleHeight = 20;
inline constexpr float kPad = 4;         // inside the panel's edge
inline constexpr float kPanelBorder = 1; // the panel's border: its contents start inside it

struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool  contains(float px, float py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    bool overlaps(const Rect &o) const
    {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
};

// ---- a panel's layout, in the panel's own pixels (0, 0 is its top-left corner).
// Controls flow left to right in rows under the title, in descriptor order, each row centred; a
// selector takes a row of its own. Jacks flow the same way from the bottom edge up, inputs first.
struct PanelLayout
{
    float             width = 0, height = 0;
    std::vector<Rect> params; // one per descriptor parameter: the control's cell
    std::vector<Rect> ports;  // one per descriptor port: the jack's cell
    std::vector<Rect> jacks;  // one per port: the jack itself (its centre is where a cable ends)
    float             controls_bottom = 0, jacks_top = 0;
    bool              fits() const { return controls_bottom <= jacks_top; }
};
const PanelLayout &panel_layout(const ModuleDesc &d); // computed once per type

// The cell sizes of each kind of control (exposed for the tests and the renderer).
struct CellSize
{
    float w, h;
};
CellSize                  control_cell(Control c);
inline constexpr CellSize kJackCell { 36, 38 };
inline constexpr float    kJackSize = 18;
inline constexpr float    kKnobSize = 30;
inline constexpr float    kSliderTrack = 72;

// ---- values. A control's position is 0..1 along its travel; the descriptor's scale maps it to
// natural units (Log for frequencies and times, so a turn covers 20 Hz to 20 kHz usefully).
float to_travel(const ParamDesc &p, float value); // 0..1
float from_travel(const ParamDesc &p, float t); // clamped and rounded as the engine would store it
bool  is_discrete(const ParamDesc &p);          // Int, Enum, Toggle
float step_value(const ParamDesc &p, float value, int steps, bool fine); // one wheel notch each
// How a knob or slider moves under a vertical drag: px pixels up from where it was.
inline constexpr float kDragPixels = 200;  // the whole travel
inline constexpr float kFineFactor = 0.1f; // with Shift
float                  drag_value(
    const ParamDesc &p, float travel_before, float px_up, bool fine, float *travel_after);

// "1.20 kHz", "350 ms", "2.5 s", "saw", "on", "0.70", "-0.25", "120 bpm".
std::string format_value(const ParamDesc &p, float value);

// ---- placement on the rack (rack units).
struct Slot
{
    int x = 0, y = 0;
};
Rect node_rect(const ModuleDesc &d, float x_hp, float y_hp); // in pixels
// Would a panel of `d` at (x, y) overlap another node (except `ignore`), or leave the rack (x or y
// negative)?
bool collides(const Patch &p, const ModuleDesc &d, int x, int y, uint32_t ignore = 0);
// The first free place for a panel of `d`, scanning rows of panel height from (x0, y0) rightwards:
// first the `rows` rows that start there, within `width_hp`, then further right on the first row.
Slot free_slot(const Patch &p, const ModuleDesc &d, int x0, int y0, int width_hp, int rows = 2);
// Snaps every node to the grid and moves any that overlaps an earlier one (in id order) to the
// first free place on its row (rightwards). Returns how many had to move for an overlap. For
// patches written before panels had a size.
int resolve_overlaps(Patch &p);

// ---- the sequencer's step grid (milestone 6): between its controls and its jacks, 16 columns of
// one page of the 64 steps; above them, the page tabs. In the panel's pixels.
inline constexpr int   kGridColumns = 16, kGridPages = 4;
inline constexpr float kGridTabH = 16, kGridNoteH = 92, kGridPadH = 13;
inline constexpr int   kGridNoteLow = 24, kGridNoteHigh = 108; // the note bar's range: C1 .. C8
inline constexpr float kGridPxPerSemitone = 4;                 // a drag on a note bar
struct StepGrid
{
    Rect area;
    Rect tab(int page) const;
    Rect note_bar(int column) const; // the column's note (drag it)
    Rect on_pad(int column) const;   // on / rest
    Rect tie_pad(int column) const;  // tied to the next step
    // the column under a point of the panel, or -1
    int column_at(float x) const;
};
StepGrid step_grid(const PanelLayout &L); // for a module with steps
// The fraction of the note bar a note fills (0 .. 1 over C1 .. C8, clamped).
float note_fill(int note);

// ---- the Keyboard panel's piano (milestone 6): two octaves on end, the low notes at the bottom,
// the black keys over the left part of the white ones. In the panel's pixels; a key is a semitone
// 0..23.
inline constexpr int kStripSemitones = 24, kStripWhiteKeys = 14;
struct KeyStrip
{
    Rect area;
    Rect key(int semitone) const; // a key's rectangle
    bool is_black(int semitone) const;
    int  key_at(float x, float y) const; // the key under a point (a black key first), or -1
};
KeyStrip keyboard_strip(const PanelLayout &L); // for the Keyboard module

// ---- the oscilloscope's screen: between its controls and its jacks (panel pixels).
Rect scope_display(const PanelLayout &L);

// ---- cables (milestone 3). Everything in rack pixels, from the model alone, so a cable follows a
// panel while it is dragged without waiting for a layout pass.
struct Pt
{
    float x = 0, y = 0;
};
Pt jack_center(const Node &n, size_t port); // where a cable plugs in
// The jack under a rack point (within `slack` pixels of its edge), if any.
std::optional<PortRef> jack_at(const Patch &p, float x, float y, float slack = 4);
// A cable from a to b as a cubic Bezier that sags under gravity: both control points hang below
// their ends, further for a longer cable.
std::array<Pt, 4> cable_curve(Pt a, Pt b);
// A cable is output -> input whichever end a drag started from: returns (from, to), or the pair as
// given when it is not one output and one input (Patch::can_connect then says why).
std::pair<PortRef, PortRef> as_cable(const Patch &p, PortRef a, PortRef b);

} // namespace rack::ui
