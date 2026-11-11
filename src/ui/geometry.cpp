#include "ui/geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace rack::ui
{

CellSize control_cell(Control c)
{
    switch (c)
    {
    case Control::Knob:
        return { 52, 72 }; // face, a name of up to two lines, the value
    case Control::Slider:
        return { 34, 112 }; // track, name, value
    case Control::Switch:
    case Control::Button:
        return { 52, 48 }; // the switch with its LED, the name
    case Control::Selector:
        return { 0, 40 }; // the name, then "< value >"; as wide as the panel
    }
    return { 52, 72 };
}

namespace
{
    constexpr float kGapX = 4, kGapY = 4;

    // Lays cells out in centred rows from y0 down; `own_row` cells take a whole row. Returns the
    // cells and the bottom of the last row.
    std::vector<Rect> flow(const std::vector<CellSize> &cells, const std::vector<bool> &own_row,
        float x0, float inner_w, float y0, float *bottom)
    {
        std::vector<Rect> out(cells.size());
        float             y = y0;
        size_t            i = 0;
        *bottom = y0;
        while (i < cells.size())
        {
            // gather one row
            size_t j = i;
            float  row_w = 0, row_h = 0;
            while (j < cells.size())
            {
                const float w = own_row[j] ? inner_w : cells[j].w;
                const float need = row_w + (j > i ? kGapX : 0) + w;
                if (j > i && (own_row[j] || own_row[j - 1] || need > inner_w))
                    break;
                row_w = need;
                row_h = std::max(row_h, cells[j].h);
                j++;
            }
            float x = x0 + std::max(0.f, (inner_w - row_w) / 2);
            for (size_t k = i; k < j; k++)
            {
                const float w = own_row[k] ? inner_w : cells[k].w;
                out[k] = { x, y, w, cells[k].h };
                x += w + kGapX;
            }
            *bottom = y + row_h;
            y += row_h + kGapY;
            i = j;
        }
        return out;
    }

    PanelLayout compute(const ModuleDesc &d)
    {
        PanelLayout L;
        L.width = d.width_hp * kHp;
        L.height = kPanelHeight;
        const float inner_w = L.width - 2 * kPad;

        std::vector<CellSize> cells;
        std::vector<bool>     own;
        for (const ParamDesc &p : d.params)
        {
            cells.push_back(control_cell(p.control));
            own.push_back(p.control == Control::Selector);
        }
        L.params = flow(cells, own, kPad, inner_w, kTitleHeight + kPad, &L.controls_bottom);
        if (d.params.empty())
            L.controls_bottom = kTitleHeight;

        // jacks: inputs first, then outputs, flowed as a block and then moved to the bottom edge
        std::vector<size_t> order;
        for (size_t i = 0; i < d.ports.size(); i++)
            if (!d.ports[i].is_output)
                order.push_back(i);
        for (size_t i = 0; i < d.ports.size(); i++)
            if (d.ports[i].is_output)
                order.push_back(i);
        std::vector<CellSize> jc(order.size(), kJackCell);
        std::vector<bool>     jown(order.size(), false);
        float                 jb = 0;
        std::vector<Rect>     flowed = flow(jc, jown, kPad, inner_w, 0, &jb);
        const float           shift = L.height - kPad - jb;
        L.ports.resize(d.ports.size());
        L.jacks.resize(d.ports.size());
        for (size_t k = 0; k < order.size(); k++)
        {
            Rect r = flowed[k];
            r.y += shift;
            L.ports[order[k]] = r;
            L.jacks[order[k]] = { r.x + (r.w - kJackSize) / 2, r.y + 3, kJackSize, kJackSize };
        }
        L.jacks_top = d.ports.empty() ? L.height - kPad : shift;
        return L;
    }

    std::string number(float v)
    {
        char        buf[32];
        const float a = std::fabs(v);
        if (a >= 100)
            std::snprintf(buf, sizeof buf, "%.0f", (double)v);
        else if (a >= 10)
            std::snprintf(buf, sizeof buf, "%.1f", (double)v);
        else
            std::snprintf(buf, sizeof buf, "%.2f", (double)v);
        return buf;
    }
} // namespace

const PanelLayout &panel_layout(const ModuleDesc &d)
{
    static std::map<const ModuleDesc *, PanelLayout> cache;
    auto                                             it = cache.find(&d);
    if (it == cache.end())
        it = cache.emplace(&d, compute(d)).first;
    return it->second;
}

bool is_discrete(const ParamDesc &p)
{
    return p.scale == Scale::Int || p.scale == Scale::Enum || p.scale == Scale::Toggle;
}

float to_travel(const ParamDesc &p, float value)
{
    float t;
    if (p.scale == Scale::Log && p.min > 0)
        t = std::log(value / p.min) / std::log(p.max / p.min);
    else
        t = (value - p.min) / (p.max - p.min);
    if (!(t >= 0)) // also NaN
        return 0;
    return std::min(t, 1.f);
}

float from_travel(const ParamDesc &p, float t)
{
    t = std::clamp(t, 0.f, 1.f);
    float v;
    if (p.scale == Scale::Log && p.min > 0)
        v = p.min * std::pow(p.max / p.min, t);
    else
        v = p.min + t * (p.max - p.min);
    if (t >= 1) // exact ends, whatever the rounding of pow
        v = p.max;
    if (t <= 0)
        v = p.min;
    return clamp_param(p, v);
}

float step_value(const ParamDesc &p, float value, int steps, bool fine)
{
    if (is_discrete(p))
        return clamp_param(p, value + (float)steps);
    return from_travel(p, to_travel(p, value) + (float)steps * (fine ? 0.001f : 0.01f));
}

float drag_value(
    const ParamDesc &p, float travel_before, float px_up, bool fine, float *travel_after)
{
    const float t
        = std::clamp(travel_before + px_up / kDragPixels * (fine ? kFineFactor : 1.f), 0.f, 1.f);
    if (travel_after)
        *travel_after = t;
    return from_travel(p, t);
}

std::string format_value(const ParamDesc &p, float value)
{
    char buf[48];
    if (p.scale == Scale::Enum)
    {
        const size_t i = (size_t)std::lround(value);
        return i < p.labels.size() ? std::string(p.labels[i]) : std::to_string(i);
    }
    if (p.scale == Scale::Toggle)
        return value >= 0.5f ? "on" : "off";
    const std::string unit(p.unit);
    if (p.scale == Scale::Int)
    {
        std::snprintf(buf, sizeof buf, "%ld", std::lround(value));
        return unit.empty() ? buf : std::string(buf) + " " + unit;
    }
    if (unit == "Hz")
        return value >= 1000 ? number(value / 1000) + " kHz" : number(value) + " Hz";
    if (unit == "ms")
        return value >= 1000 ? number(value / 1000) + " s" : number(value) + " ms";
    return unit.empty() ? number(value) : number(value) + " " + unit;
}

Rect node_rect(const ModuleDesc &d, float x_hp, float y_hp)
{
    return { x_hp * kHp, y_hp * kHp, d.width_hp * kHp, kPanelHeight };
}

namespace
{
    bool spans_overlap(int a, int aw, int b, int bw) { return a < b + bw && b < a + aw; }
} // namespace

bool collides(const Patch &p, const ModuleDesc &d, int x, int y, uint32_t ignore)
{
    if (x < 0 || y < 0)
        return true;
    for (const Node &n : p.nodes())
    {
        if (n.id == ignore)
            continue;
        if (spans_overlap(x, d.width_hp, (int)std::lround(n.x), n.desc->width_hp)
            && spans_overlap(y, kPanelHeightHp, (int)std::lround(n.y), kPanelHeightHp))
            return true;
    }
    return false;
}

Slot free_slot(const Patch &p, const ModuleDesc &d, int x0, int y0, int width_hp, int rows)
{
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    for (int r = 0; r < rows; r++)
        for (int x = x0; x + d.width_hp <= x0 + width_hp; x++)
            if (!collides(p, d, x, y0 + r * kPanelHeightHp))
                return { x, y0 + r * kPanelHeightHp };
    for (int x = x0;; x++) // terminates: there are finitely many nodes
        if (!collides(p, d, x, y0))
            return { x, y0 };
}

int resolve_overlaps(Patch &p)
{
    std::vector<uint32_t> ids;
    for (const Node &n : p.nodes())
        ids.push_back(n.id);
    std::sort(ids.begin(), ids.end());
    struct Placed
    {
        int x, y, w;
    };
    std::vector<Placed> placed;
    auto                hits = [&](int x, int y, int w)
    {
        for (const Placed &q : placed)
            if (spans_overlap(x, w, q.x, q.w)
                && spans_overlap(y, kPanelHeightHp, q.y, kPanelHeightHp))
                return true;
        return false;
    };
    int moved = 0;
    for (uint32_t id : ids)
    {
        Node     *n = p.find(id);
        const int w = n->desc->width_hp;
        const int y = std::max(0, (int)std::lround(n->y));
        int       x = std::max(0, (int)std::lround(n->x));
        const int x_was = x;
        while (hits(x, y, w))
            x++;
        moved += x != x_was;
        n->x = (float)x;
        n->y = (float)y;
        placed.push_back({ x, y, w });
    }
    return moved;
}

StepGrid step_grid(const PanelLayout &L)
{
    StepGrid g;
    g.area = { kPad, L.controls_bottom + 4, L.width - 2 * kPad,
        L.jacks_top - 4 - (L.controls_bottom + 4) };
    return g;
}

Rect StepGrid::tab(int page) const
{
    const float w = (area.w - 3 * 4) / kGridPages;
    return { area.x + (float)page * (w + 4), area.y, w, kGridTabH };
}

namespace
{
    Rect column_rect(const Rect &area, int c, float y, float h)
    {
        const float pitch = area.w / kGridColumns;
        return { area.x + (float)c * pitch + 1, y, pitch - 2, h };
    }
} // namespace

Rect StepGrid::note_bar(int c) const
{
    return column_rect(area, c, area.y + kGridTabH + 4, kGridNoteH);
}
Rect StepGrid::on_pad(int c) const
{
    return column_rect(area, c, area.y + kGridTabH + 4 + kGridNoteH + 4, kGridPadH);
}
Rect StepGrid::tie_pad(int c) const
{
    return column_rect(area, c, area.y + kGridTabH + 4 + kGridNoteH + 4 + kGridPadH + 3, kGridPadH);
}
int StepGrid::column_at(float x) const
{
    if (x < area.x || x >= area.right())
        return -1;
    return std::min(kGridColumns - 1, (int)((x - area.x) / (area.w / kGridColumns)));
}

float note_fill(int note)
{
    return std::clamp(
        (float)(note - kGridNoteLow) / (float)(kGridNoteHigh - kGridNoteLow), 0.f, 1.f);
}

KeyStrip keyboard_strip(const PanelLayout &L)
{
    KeyStrip k;
    k.area = { kPad + 4, L.controls_bottom + 6, L.width - 2 * kPad - 8,
        L.jacks_top - 8 - (L.controls_bottom + 6) };
    return k;
}

namespace
{
    // the white key (0..6) a semitone of an octave is, or the white key below it for a black one
    constexpr int  kWhiteOf[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
    constexpr bool kBlack[12]
        = { false, true, false, true, false, false, true, false, true, false, true, false };
} // namespace

bool KeyStrip::is_black(int s) const { return s >= 0 && kBlack[s % 12]; }

Rect KeyStrip::key(int s) const
{
    const float h = area.h / kStripWhiteKeys;
    const int   white = (s / 12) * 7 + kWhiteOf[s % 12]; // 0 is the lowest, at the bottom
    if (!is_black(s))
        return { area.x, area.bottom() - (float)(white + 1) * h, area.w, h };
    const float bh = h * 0.6f; // a black key straddles the line above its white key
    return { area.x, area.bottom() - (float)(white + 1) * h - bh / 2, area.w * 0.58f, bh };
}

int KeyStrip::key_at(float x, float y) const
{
    if (!area.contains(x, y))
        return -1;
    for (int s = 0; s < kStripSemitones; s++) // the black keys lie over the white ones
        if (is_black(s) && key(s).contains(x, y))
            return s;
    for (int s = 0; s < kStripSemitones; s++)
        if (!is_black(s) && key(s).contains(x, y))
            return s;
    return -1;
}

Pt jack_center(const Node &n, size_t port)
{
    const Rect &j = panel_layout(*n.desc).jacks[port];
    return { n.x * kHp + kPanelBorder + j.x + j.w / 2, n.y * kHp + kPanelBorder + j.y + j.h / 2 };
}

std::optional<PortRef> jack_at(const Patch &p, float x, float y, float slack)
{
    for (const Node &n : p.nodes())
    {
        const Rect r = node_rect(*n.desc, n.x, n.y);
        if (!Rect { r.x - slack, r.y - slack, r.w + 2 * slack, r.h + 2 * slack }.contains(x, y))
            continue;
        for (size_t k = 0; k < n.desc->ports.size(); k++)
        {
            const Pt    c = jack_center(n, k);
            const float half = kJackSize / 2 + slack;
            if (std::fabs(x - c.x) <= half && std::fabs(y - c.y) <= half)
                return PortRef { n.id, (uint16_t)k };
        }
    }
    return std::nullopt;
}

std::array<Pt, 4> cable_curve(Pt a, Pt b)
{
    const float dist = std::hypot(b.x - a.x, b.y - a.y);
    const float sag = std::min(30.f + 0.35f * dist, 240.f);
    return { a, Pt { a.x + (b.x - a.x) * 0.1f, a.y + sag },
        Pt { b.x - (b.x - a.x) * 0.1f, b.y + sag }, b };
}

std::pair<PortRef, PortRef> as_cable(const Patch &p, PortRef a, PortRef b)
{
    const Node *na = p.find(a.node), *nb = p.find(b.node);
    if (na && nb && a.port < na->desc->ports.size() && b.port < nb->desc->ports.size()
        && !na->desc->ports[a.port].is_output && nb->desc->ports[b.port].is_output)
        return { b, a };
    return { a, b };
}

} // namespace rack::ui
