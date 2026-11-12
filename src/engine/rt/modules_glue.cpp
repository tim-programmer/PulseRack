// The rack's own modules: the ones PulseCoreLib has no design note for (docs/patch-engine.md, "Glue
// modules"). They use the library's saturating helpers (sample_ops.h), so they behave the same in
// float, Q15 and Q31.
#include "engine/rt/modules_common.h"

#include "engine/model/scope.h"
#include "midi/inbox.h"

#include <algorithm>
#include <cmath>

#include <cstring>

namespace rack
{
namespace
{

    constexpr std::string_view kUtility = "Utility", kEvent = "Event";

    // ============================================================================================================
    // Keyboard A source of NOTE events. Key events are scheduled at absolute sample positions
    // (engine time) and sent out in the block they fall in, at the right offset; a live key press
    // is scheduled "now", so it starts at offset 0 of the next block.
    constexpr size_t kKeyQueue = 1024;
    struct KeyEvt
    {
        uint64_t at;
        uint8_t  type, note, vel;
    };
    struct KeyboardNode
    {
        KeyEvt   q[kKeyQueue];
        uint32_t head, n; // q[head .. n) is pending, sorted by `at` (stable)
    };

    bool kb_init(void *, const NodeContext &, const float *) { return true; }
    void kb_reset(void *s)
    {
        auto *k = as<KeyboardNode>(s);
        k->head = k->n = 0;
    }
    void kb_process(void *s, NodeIo &io)
    {
        auto     *k = as<KeyboardNode>(s);
        EventBuf &out = *io.evt_out[0];
        out.count = 0;
        while (k->head < k->n && k->q[k->head].at < io.time + io.frames)
        {
            const KeyEvt &e = k->q[k->head++];
            if (out.count < out.cap)
                out.data[out.count++]
                    = pcl_event_t { (uint16_t)(e.at < io.time ? 0 : e.at - io.time), e.type, e.note,
                          e.vel };
        }
        if (k->head == k->n)
            k->head = k->n = 0;
    }
    size_t         kb_cap(size_t) { return 64; }
    const PortDesc kKeyboardPorts[] = { { "notes", "NOTES", Kind::Event, true } };

    // ============================================================================================================
    // MIDI In Notes and controls from MIDI devices (docs/midi.md). The engine hands it every
    // message at the start of a block (midi_in_push, below); notes go out as events at offset 0 of
    // that block, and pitch bend, the mod wheel and pressure are signals, smoothed over a few
    // milliseconds so that steps of 1/127 do not click. It remembers which notes each device holds,
    // so that a device that goes away (or a change of channel) releases them.
    constexpr size_t kMidiQueue = 128;
    struct MidiInNode
    {
        pcl_event_t q[kMidiQueue]; // the notes for the next block
        uint32_t    n;
        uint8_t     held[midi::kMaxInputs][16]; // per device slot: a bit per note
        uint8_t     channel;                    // 0: every channel, else 1..16
        uint8_t     newest;                     // the note pressed last (key pressure follows it)
        float       bend, mod, pressure, velocity; // where the signals are going
        float       bend_s, mod_s, pressure_s;     // where they are
        float       k;                             // the smoothing, per sample
        uint32_t    dropped;
    };
    enum
    {
        MIDI_CHANNEL,
        MIDI_COUNT
    };
    void midi_queue(MidiInNode *m, uint8_t type, uint8_t note, uint8_t vel)
    {
        if (m->n < kMidiQueue)
            m->q[m->n++] = pcl_event_t { 0, type, note, vel };
        else
            m->dropped++;
    }
    void midi_release(MidiInNode *m, size_t slot) // a note off for every note the device holds
    {
        for (int note = 0; note < 128; note++)
            if (m->held[slot][note >> 3] & (1u << (note & 7)))
                midi_queue(m, PCL_EVT_NOTE_OFF, (uint8_t)note, 0);
        std::memset(m->held[slot], 0, sizeof m->held[slot]);
    }
    bool midi_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *m = as<MidiInNode>(s);
        m->channel = (uint8_t)std::clamp(to_int(p[MIDI_CHANNEL]), 0, 16);
        m->k = 1.0f - std::exp(-1.0f / (0.003f * (float)ctx.sample_rate)); // 3 ms
        return true;
    }
    void midi_reset(void *s)
    {
        auto *m = as<MidiInNode>(s);
        m->n = 0;
        std::memset(m->held, 0, sizeof m->held);
        m->bend = m->mod = m->pressure = m->velocity = 0;
        m->bend_s = m->mod_s = m->pressure_s = 0;
    }
    void midi_process(void *s, NodeIo &io)
    {
        auto     *m = as<MidiInNode>(s);
        EventBuf &out = *io.evt_out[0];
        out.count = 0;
        for (uint32_t i = 0; i < m->n; i++)
        {
            if (out.count < out.cap)
                out.data[out.count++] = m->q[i];
            else
                m->dropped++;
        }
        m->n = 0;
        // the signals, in the order of the ports: pitch bend, mod wheel, pressure, velocity
        pcl_sample_t *bend = io.sig_out[0], *mod = io.sig_out[1], *pres = io.sig_out[2];
        for (size_t i = 0; i < io.frames; i++)
        {
            m->bend_s += m->k * (m->bend - m->bend_s);
            m->mod_s += m->k * (m->mod - m->mod_s);
            m->pressure_s += m->k * (m->pressure - m->pressure_s);
            if (bend)
                bend[i] = to_sample(m->bend_s);
            if (mod)
                mod[i] = to_sample(m->mod_s);
            if (pres)
                pres[i] = to_sample(m->pressure_s);
        }
        if (pcl_sample_t *vel = io.sig_out[3])
            std::fill(vel, vel + io.frames, to_sample(m->velocity));
    }
    size_t         midi_cap(size_t) { return kMidiQueue; }
    const PortDesc kMidiInPorts[] = {
        { "notes", "NOTES", Kind::Event, true },
        { "bend", "BEND", Kind::Signal, true },
        { "mod", "MOD", Kind::Signal, true },
        { "pressure", "PRESS", Kind::Signal, true },
        { "velocity", "VEL", Kind::Signal, true },
    };
    constexpr std::string_view kMidiChannels[] = { "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16" };
    const ParamDesc kMidiInParams[] = {
        param("channel", "Channel", "", 0, 16, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            { // the notes held on the old channel are released: their note offs would not arrive
                auto         *m = as<MidiInNode>(s);
                const uint8_t c = (uint8_t)std::clamp(to_int(v), 0, 16);
                if (c != m->channel)
                    for (size_t slot = 0; slot < midi::kMaxInputs; slot++)
                        midi_release(m, slot);
                m->channel = c;
                return true;
            },
            kMidiChannels),
    };
    static_assert(std::size(kMidiInParams) == MIDI_COUNT);

    // ============================================================================================================
    // MIDI CC One controller (a knob, a fader, a pedal) as a signal, 0 .. 1, smoothed over a few
    // milliseconds so that its 128 steps do not click (docs/midi.md). It starts at 0 and keeps its
    // value when the controller number or the channel changes.
    struct MidiCcNode
    {
        uint8_t channel; // 0: every channel, else 1..16
        uint8_t cc;      // the controller number, 0..119
        float   value, value_s, k;
    };
    enum
    {
        CC_CHANNEL,
        CC_NUMBER,
        CC_COUNT
    };
    bool cc_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *c = as<MidiCcNode>(s);
        c->channel = (uint8_t)std::clamp(to_int(p[CC_CHANNEL]), 0, 16);
        c->cc = (uint8_t)std::clamp(to_int(p[CC_NUMBER]), 0, 119);
        c->k = 1.0f - std::exp(-1.0f / (0.003f * (float)ctx.sample_rate)); // 3 ms
        return true;
    }
    void cc_reset(void *s)
    {
        auto *c = as<MidiCcNode>(s);
        c->value = c->value_s = 0;
    }
    void cc_process(void *s, NodeIo &io)
    {
        auto         *c = as<MidiCcNode>(s);
        pcl_sample_t *out = io.sig_out[0];
        for (size_t i = 0; i < io.frames; i++)
        {
            c->value_s += c->k * (c->value - c->value_s);
            if (out)
                out[i] = to_sample(c->value_s);
        }
    }
    const PortDesc  kMidiCcPorts[] = { { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kMidiCcParams[] = {
        param(
            "channel", "Channel", "", 0, 16, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                as<MidiCcNode>(s)->channel = (uint8_t)std::clamp(to_int(v), 0, 16);
                return true;
            },
            kMidiChannels),
        param("cc", "Controller", "", 0, 119, 1, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                as<MidiCcNode>(s)->cc = (uint8_t)std::clamp(to_int(v), 0, 119);
                return true;
            }),
    };
    static_assert(std::size(kMidiCcParams) == CC_COUNT);

    // ============================================================================================================
    // Audio Output The sink: the only module that knows the host. It adds its inputs, scaled, to
    // the engine's float master buffers; the engine clamps the sum to +-1. One input alone plays on
    // both channels.
    enum OutParam
    {
        OUT_LEVEL,
        OUT_COUNT
    };
    struct AudioOutNode
    {
        float level;
    };
    bool out_init(void *s, const NodeContext &, const float *p)
    {
        as<AudioOutNode>(s)->level = p[OUT_LEVEL];
        return true;
    }
    void out_process(void *s, NodeIo &io)
    {
        const float         level = as<AudioOutNode>(s)->level;
        const pcl_sample_t *l = io.sig_in[0];
        const pcl_sample_t *r = io.sig_in[1];
        if (!l && !r)
            return;
        if (!l)
            l = r;
        if (!r)
            r = l;
        for (size_t i = 0; i < io.frames; i++)
        {
            io.master[0][i] += sample_to_float(l[i]) * level;
            io.master[1][i] += sample_to_float(r[i]) * level;
        }
    }
    const PortDesc kOutPorts[]
        = { { "l", "L", Kind::Signal, false }, { "r", "R", Kind::Signal, false } };
    const ParamDesc kOutParams[] = {
        param("level", "Level", "", 0, 1.5f, 0.8f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<AudioOutNode>(s)->level = v;
                return true;
            }),
    };
    static_assert(std::size(kOutParams) == OUT_COUNT);

    // ============================================================================================================
    // Mixer
    constexpr size_t kMixerInputs = 4;
    struct MixerNode
    {
        pcl_coef_t level[kMixerInputs];
    };
    bool mixer_init(void *s, const NodeContext &, const float *p)
    {
        for (size_t i = 0; i < kMixerInputs; i++)
            as<MixerNode>(s)->level[i] = to_coef(p[i]);
        return true;
    }
    void mixer_process(void *s, NodeIo &io)
    {
        auto         *m = as<MixerNode>(s);
        pcl_sample_t *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        for (size_t i = 0; i < io.frames; i++)
        {
            wide_acc_t acc = 0;
            for (size_t k = 0; k < kMixerInputs; k++)
                if (io.sig_in[k])
                    acc += (wide_acc_t)mul_coef(io.sig_in[k][i], m->level[k]);
            out[i] = sat_acc(acc);
        }
    }
    const PortDesc kMixerPorts[] = { { "in1", "IN 1", Kind::Signal, false },
        { "in2", "IN 2", Kind::Signal, false }, { "in3", "IN 3", Kind::Signal, false },
        { "in4", "IN 4", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
#define MIXER_LEVEL(n)                                                                             \
    param("level" #n, "Level " #n, "", 0, 1, 0.5f, Scale::Linear, Control::Slider,                 \
        [](void *s, float v)                                                                       \
        {                                                                                          \
            as<MixerNode>(s)->level[n - 1] = to_coef(v);                                           \
            return true;                                                                           \
        })
    const ParamDesc kMixerParams[]
        = { MIXER_LEVEL(1), MIXER_LEVEL(2), MIXER_LEVEL(3), MIXER_LEVEL(4) };
    static_assert(std::size(kMixerParams) == kMixerInputs);

    // ============================================================================================================
    // Attenuverter out = in * scale + offset, saturating: a modulation depth you can see and patch.
    enum AttParam
    {
        ATT_SCALE,
        ATT_OFFSET,
        ATT_COUNT
    };
    struct AttNode
    {
        pcl_coef_t   scale;
        pcl_sample_t offset;
    };
    bool att_init(void *s, const NodeContext &, const float *p)
    {
        auto *a = as<AttNode>(s);
        a->scale = to_coef(p[ATT_SCALE]);
        a->offset = to_sample(p[ATT_OFFSET]);
        return true;
    }
    void att_process(void *s, NodeIo &io)
    {
        auto               *a = as<AttNode>(s);
        const pcl_sample_t *in = io.sig_in[0] ? io.sig_in[0] : io.zeros;
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        for (size_t i = 0; i < io.frames; i++)
            out[i] = sat_acc((wide_acc_t)mul_coef(in[i], a->scale) + (wide_acc_t)a->offset);
    }
    const PortDesc kAttPorts[]
        = { { "in", "IN", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kAttParams[] = {
        param("scale", "Scale", "", -1, 1, 1, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<AttNode>(s)->scale = to_coef(v);
                return true;
            }),
        param("offset", "Offset", "", -1, 1, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<AttNode>(s)->offset = to_sample(v);
                return true;
            }),
    };
    static_assert(std::size(kAttParams) == ATT_COUNT);

    // ============================================================================================================
    // Note -> Gate Monophonic, last note wins: high from a NOTE_ON until its NOTE_OFF (the release
    // of a note that is no longer the last is ignored).
    struct GateNode
    {
        uint8_t note;
        uint8_t high;
    };
    bool gate_init(void *, const NodeContext &, const float *) { return true; }
    void gate_reset(void *s) { std::memset(s, 0, sizeof(GateNode)); }
    void gate_process(void *s, NodeIo &io)
    {
        auto           *g = as<GateNode>(s);
        const EventBuf &ev = *io.evt_in[0];
        pcl_sample_t   *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        size_t          pos = 0, i = 0;

        while (pos < io.frames)
        {
            while (i < ev.count && ev.data[i].offset <= pos)
            {
                const pcl_event_t &e = ev.data[i++];
                const bool         on = e.type == PCL_EVT_NOTE_ON && e.velocity > 0;
                const bool         off = e.type == PCL_EVT_NOTE_OFF
                    || (e.type == PCL_EVT_NOTE_ON && e.velocity == 0) || e.type == PCL_EVT_STOP;
                if (on)
                {
                    g->note = e.note;
                    g->high = 1;
                }
                else if (off && (e.type == PCL_EVT_STOP || (g->high && e.note == g->note)))
                    g->high = 0;
            }
            const size_t end
                = i < ev.count ? std::min<size_t>(io.frames, ev.data[i].offset) : io.frames;
            const pcl_sample_t level = g->high ? kFullScale : (pcl_sample_t)0;
            for (size_t k = pos; k < end; k++)
                out[k] = level;
            pos = end;
        }
    }
    const PortDesc kGatePorts[]
        = { { "notes", "NOTES", Kind::Event, false }, { "gate", "GATE", Kind::Signal, true } };

    // ============================================================================================================
    // Merge
    struct EmptyNode
    {
        uint8_t unused;
    };
    // Two event streams into one, sorted by offset; at one offset input A goes first (so keys come
    // before the clock pulse at the same sample, as tools/render.c does). Output capacity is the
    // sum of the input capacities, so nothing is ever dropped.
    bool merge_init(void *, const NodeContext &, const float *) { return true; }
    void merge_process(void *, NodeIo &io)
    {
        const EventBuf &a = *io.evt_in[0];
        const EventBuf &b = *io.evt_in[1];
        EventBuf       &out = *io.evt_out[0];
        size_t          i = 0, j = 0;
        out.count = 0;
        while ((i < a.count || j < b.count) && out.count < out.cap)
            out.data[out.count++]
                = (j >= b.count || (i < a.count && a.data[i].offset <= b.data[j].offset))
                ? a.data[i++]
                : b.data[j++];
    }
    size_t         merge_cap(size_t in_cap) { return std::max<size_t>(in_cap, 64); }
    const PortDesc kMergePorts[] = { { "a", "A", Kind::Event, false },
        { "b", "B", Kind::Event, false }, { "out", "OUT", Kind::Event, true } };

    // ============================================================================================================
    // Oscilloscope Two inputs drawn on its panel. It waits for its trigger (an edge of input A
    // through the level, or none at all), captures one window into kScopeColumns columns of lowest
    // and highest values, hands the frame to the window through a triple buffer (scope.h) and waits
    // again. If no edge comes within three windows (or 0.1 s), it captures anyway, as a scope's
    // auto mode does, so a signal that never crosses the level is still seen. It has no outputs: a
    // signal is looked at by plugging a second cable from where it comes from.
    enum ScopeParam
    {
        SCOPE_TIME,
        SCOPE_SCALE_A,
        SCOPE_SCALE_B,
        SCOPE_LEVEL, // (the four knobs first: one row, so the screen gets the room)
        SCOPE_TRIGGER,
        SCOPE_COUNT
    };
    constexpr std::string_view kTriggerLabels[] = { "free", "rise", "fall" };
    struct ScopeNode
    {
        ScopeTriple tb;
        uint32_t    sr;
        float       time_ms, scale[2], level;
        int         trigger;
        // the capture
        bool     capturing;
        float    prev;    // input A's last sample, for the edge
        uint64_t waited;  // samples since it was armed
        double   col_len; // samples a column covers
        uint64_t pos;     // samples into the window
        int      col;
        bool     triggered;
        uint32_t seq;
    };
    void scope_arm(ScopeNode *n)
    {
        n->capturing = false;
        n->waited = 0;
    }
    bool scope_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<ScopeNode>(s);
        n->tb.init();
        n->sr = ctx.sample_rate;
        n->time_ms = p[SCOPE_TIME];
        n->scale[0] = p[SCOPE_SCALE_A];
        n->scale[1] = p[SCOPE_SCALE_B];
        n->trigger = (int)std::lround(p[SCOPE_TRIGGER]);
        n->level = p[SCOPE_LEVEL];
        scope_arm(n);
        return true;
    }
    void scope_reset(void *s) { scope_arm(as<ScopeNode>(s)); }
    void scope_start(ScopeNode *n, bool triggered)
    {
        const double window = std::max<double>(kScopeColumns, n->time_ms * 0.001 * n->sr);
        n->capturing = true;
        n->triggered = triggered;
        n->col_len = window / kScopeColumns;
        n->pos = 0;
        n->col = 0;
        ScopeFrame &f = n->tb.back();
        for (int ch = 0; ch < 2; ch++)
        {
            f.lo[ch][0] = INFINITY;
            f.hi[ch][0] = -INFINITY;
        }
    }
    void scope_process(void *s, NodeIo &io)
    {
        auto               *n = as<ScopeNode>(s);
        const pcl_sample_t *in[2] = { io.sig_in[0], io.sig_in[1] };
        for (size_t i = 0; i < io.frames; i++)
        {
            const float a = in[0] ? sample_to_float(in[0][i]) : 0.f;
            if (!n->capturing)
            {
                bool go = false, triggered = true;
                if (n->trigger == 0)
                    go = true;
                else if (n->trigger == 1)
                    go = n->prev < n->level && a >= n->level;
                else
                    go = n->prev > n->level && a <= n->level;
                n->prev = a;
                if (!go
                    && ++n->waited > std::max<uint64_t>(
                           (uint64_t)(3 * n->time_ms * 0.001 * n->sr), n->sr / 10))
                    go = true, triggered = false; // auto: nothing crossed the level in time
                if (!go)
                    continue;
                scope_start(n, triggered);
            }
            ScopeFrame &f = n->tb.back();
            for (int ch = 0; ch < 2; ch++)
            {
                const float v = (in[ch] ? sample_to_float(in[ch][i]) : 0.f) * n->scale[ch];
                f.lo[ch][n->col] = std::min(f.lo[ch][n->col], v);
                f.hi[ch][n->col] = std::max(f.hi[ch][n->col], v);
            }
            n->pos++;
            if ((double)n->pos >= (n->col + 1) * n->col_len)
            {
                if (++n->col == kScopeColumns)
                { // the window is full: hand it over, and wait for the next trigger
                    f.seq = ++n->seq;
                    f.window_ms = (float)(n->pos * 1000.0 / n->sr);
                    f.has[0] = in[0] != nullptr;
                    f.has[1] = in[1] != nullptr;
                    f.triggered = n->triggered;
                    n->tb.publish();
                    scope_arm(n);
                    n->prev = a;
                    continue;
                }
                for (int ch = 0; ch < 2; ch++)
                {
                    f.lo[ch][n->col] = INFINITY;
                    f.hi[ch][n->col] = -INFINITY;
                }
            }
        }
    }
    const PortDesc kScopePorts[]
        = { { "in_a", "IN A", Kind::Signal, false }, { "in_b", "IN B", Kind::Signal, false } };
    const ParamDesc kScopeParams[] = {
        param("time", "Time", "ms", 5, 1000, 20, Scale::Log,
            Control::Knob, // (5 ms: 240 samples, a column each)
            [](void *s, float v)
            {
                as<ScopeNode>(s)->time_ms = v;
                return true;
            }),
        param("scale_a", "Scale A", "x", 0.1f, 10, 1, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                as<ScopeNode>(s)->scale[0] = v;
                return true;
            }),
        param("scale_b", "Scale B", "x", 0.1f, 10, 1, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                as<ScopeNode>(s)->scale[1] = v;
                return true;
            }),
        param("level", "Level", "", -1, 1, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<ScopeNode>(s)->level = v;
                return true;
            }),
        param(
            "trigger", "Trigger", "", 0, 2, 1, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                as<ScopeNode>(s)->trigger = (int)std::lround(v);
                return true;
            },
            kTriggerLabels),
    };
    static_assert(std::size(kScopeParams) == SCOPE_COUNT);

} // namespace

bool scope_take(const ModuleDesc &desc, void *state, ScopeFrame &out)
{
    return &desc == &kScopeDesc && as<ScopeNode>(state)->tb.take(out);
}

bool keyboard_push(void *state, uint64_t at, uint8_t type, uint8_t note, uint8_t velocity)
{
    auto *k = as<KeyboardNode>(state);
    if (k->n == kKeyQueue)
    {
        if (k->head == 0)
            return false;
        std::memmove(k->q, k->q + k->head, (k->n - k->head) * sizeof(KeyEvt));
        k->n -= k->head;
        k->head = 0;
    }
    uint32_t i = k->n;
    while (i > k->head && k->q[i - 1].at > at)
    {
        k->q[i] = k->q[i - 1];
        i--;
    }
    k->q[i] = KeyEvt { at, type, note, velocity };
    k->n++;
    return true;
}

void midi_in_push(void *state, const midi::Msg &msg)
{
    using midi::Type;
    auto        *m = as<MidiInNode>(state);
    const size_t slot = msg.slot < midi::kMaxInputs ? msg.slot : 0;
    if (msg.type == Type::DeviceLost)
    {
        midi_release(m, slot);
        return;
    }
    if (msg.type >= Type::Clock) // the realtime messages: not used yet (docs/midi.md, step 9)
        return;
    if (m->channel && msg.channel + 1 != m->channel)
        return;
    const uint8_t note = msg.data & 0x7F, bit = (uint8_t)(1u << (note & 7));
    const float   v = (float)std::min<int>(msg.value, 127) / 127.0f;
    switch (msg.type)
    {
    case Type::NoteOn:
        midi_queue(m, PCL_EVT_NOTE_ON, note, (uint8_t)std::min<int>(msg.value, 127));
        m->held[slot][note >> 3] |= bit;
        m->newest = note;
        m->velocity = v;
        break;
    case Type::NoteOff:
        midi_queue(m, PCL_EVT_NOTE_OFF, note, 0);
        m->held[slot][note >> 3] &= (uint8_t)~bit;
        break;
    case Type::Control:
        if (msg.data == 1) // the mod wheel
            m->mod = v;
        else if (msg.data == 120 || msg.data == 123) // all sound off, all notes off
            midi_release(m, slot);
        break;
    case Type::PitchBend: // 0..16383 with 8192 in the middle, to -1..1
        m->bend = std::clamp(((float)msg.value - 8192.0f) / 8191.0f, -1.0f, 1.0f);
        break;
    case Type::ChannelPressure:
        m->pressure = v;
        break;
    case Type::KeyPressure:
        if (note == m->newest)
            m->pressure = v;
        break;
    default:
        break;
    }
}

void midi_cc_push(void *state, const midi::Msg &msg)
{
    auto *c = as<MidiCcNode>(state);
    if (msg.type != midi::Type::Control || msg.data != c->cc)
        return;
    if (c->channel && msg.channel + 1 != c->channel)
        return;
    c->value = (float)std::min<int>(msg.value, 127) / 127.0f;
}

MidiPushFn midi_sink(const ModuleDesc &desc)
{
    return &desc == &kMidiInDesc ? midi_in_push : &desc == &kMidiCcDesc ? midi_cc_push : nullptr;
}

uint32_t midi_in_dropped(const void *state)
{
    return static_cast<const MidiInNode *>(state)->dropped;
}

#define RACK_GLUE(var, id, name, cat, hp, T, ports, params, init, reset, process, cap, blurb)      \
    const ModuleDesc var = { id, name, cat, hp, sizeof(T), alignof(T), ports, params, false, init, \
        reset, process, nullptr, cap, blurb }

RACK_GLUE(kKeyboardDesc, "keyboard", "Keyboard", kEvent, 6, KeyboardNode, kKeyboardPorts, { },
    kb_init, kb_reset, kb_process, kb_cap,
    "The computer keyboard and an on-screen strip as a piano");
RACK_GLUE(kMidiInDesc, "midi_in", "MIDI In", kEvent, 8, MidiInNode, kMidiInPorts, kMidiInParams,
    midi_init, midi_reset, midi_process, midi_cap,
    "Notes, pitch bend, the mod wheel and pressure from MIDI keyboards and controllers");
RACK_GLUE(kMidiCcDesc, "midi_cc", "MIDI CC", kEvent, 6, MidiCcNode, kMidiCcPorts, kMidiCcParams,
    cc_init, cc_reset, cc_process, nullptr,
    "One knob, fader or pedal of a MIDI controller as a signal");
RACK_GLUE(kAudioOutDesc, "out", "Audio Output", kUtility, 4, AudioOutNode, kOutPorts, kOutParams,
    out_init, nullptr, out_process, nullptr,
    "Sends a signal to the speakers; L alone plays on both");
RACK_GLUE(kMixerDesc, "mixer", "Mixer", kUtility, 6, MixerNode, kMixerPorts, kMixerParams,
    mixer_init, nullptr, mixer_process, nullptr, "Mixes four signals, each with its own level");
RACK_GLUE(kAttenuverterDesc, "attenuverter", "Attenuverter", kUtility, 4, AttNode, kAttPorts,
    kAttParams, att_init, nullptr, att_process, nullptr, "Scales, inverts and offsets a signal");
RACK_GLUE(kNoteToGateDesc, "note_to_gate", "Note to Gate", kEvent, 4, GateNode, kGatePorts, { },
    gate_init, gate_reset, gate_process, nullptr, "Turns notes into a gate signal");
RACK_GLUE(kMergeDesc, "merge", "Merge", kEvent, 4, EmptyNode, kMergePorts, { }, merge_init, nullptr,
    merge_process, merge_cap, "Merges two event streams into one, in time order");
RACK_GLUE(kScopeDesc, "scope", "Oscilloscope", kUtility, 16, ScopeNode, kScopePorts, kScopeParams,
    scope_init, scope_reset, scope_process, nullptr,
    "Draws two signals on its panel, triggered on an edge of the first, like a bench scope");

} // namespace rack
