// The rack's own modules: the ones PulseCoreLib has no design note for (docs/patch-engine.md, "Glue
// modules"). They use the library's saturating helpers (sample_ops.h), so they behave the same in
// float, Q15 and Q31.
#include "engine/rt/modules_common.h"

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

} // namespace

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

#define RACK_GLUE(var, id, name, cat, hp, T, ports, params, init, reset, process, cap, blurb)      \
    const ModuleDesc var = { id, name, cat, hp, sizeof(T), alignof(T), ports, params, false, init, \
        reset, process, nullptr, cap, blurb }

RACK_GLUE(kKeyboardDesc, "keyboard", "Keyboard", kEvent, 6, KeyboardNode, kKeyboardPorts, { },
    kb_init, kb_reset, kb_process, kb_cap,
    "The computer keyboard and an on-screen strip as a piano");
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

} // namespace rack
