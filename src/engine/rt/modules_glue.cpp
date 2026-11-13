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
    // The MIDI modules (docs/midi.md). The engine gives each one every MIDI message with the sample
    // it belongs at (midi_in_push and midi_cc_push, below); the message waits in the module's queue
    // and takes effect at that sample, inside the right block. A module may listen to one device
    // only (its slot), to every device, or, when its device is not known on this machine, to none.
    constexpr size_t kMidiTimed = 256;
    struct TimedMsg
    {
        uint64_t  at; // the sample it takes effect at (engine time)
        midi::Msg m;
    };
    struct MidiQueue
    {
        TimedMsg q[kMidiTimed];
        uint32_t head, n; // q[head .. n) waits, sorted by `at` (stable)
    };
    bool timed_push(MidiQueue &k, uint64_t at, const midi::Msg &m)
    {
        if (k.n == kMidiTimed)
        {
            if (k.head == 0)
                return false;
            std::memmove(k.q, k.q + k.head, (k.n - k.head) * sizeof(TimedMsg));
            k.n -= k.head;
            k.head = 0;
        }
        uint32_t i = k.n;
        while (i > k.head && k.q[i - 1].at > at)
        {
            k.q[i] = k.q[i - 1];
            i--;
        }
        k.q[i] = TimedMsg { at, m };
        k.n++;
        return true;
    }
    // The next message due before `end`, or null.
    const TimedMsg *timed_next(MidiQueue &k, uint64_t end)
    {
        if (k.head == k.n)
        {
            k.head = k.n = 0;
            return nullptr;
        }
        return k.q[k.head].at < end ? &k.q[k.head++] : nullptr;
    }
    bool listens_to(int device, const midi::Msg &m)
    {
        return device == kMidiAnyDevice || device == (int)m.slot;
    }

    // ============================================================================================================
    // MIDI In Notes and controls from MIDI devices. Notes go out as events at their own sample;
    // pitch bend, the mod wheel and pressure are signals, smoothed over a few milliseconds so that
    // steps of 1/127 do not click. It remembers which notes each device holds, so that a device
    // that goes away (or a change of channel or of device) releases them.
    constexpr size_t kMidiQueue = 128;
    struct MidiInNode
    {
        MidiQueue   in;            // the messages still to come
        pcl_event_t q[kMidiQueue]; // note offs made between blocks (a change of channel or device)
        uint32_t    n;
        uint8_t     held[midi::kMaxInputs][16]; // per device slot: a bit per note
        uint8_t     channel;                    // 0: every channel, else 1..16
        uint8_t     newest;                     // the note pressed last (key pressure follows it)
        int         device;                     // kMidiAnyDevice, kMidiNoDevice, or a slot
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
    // A note event at `offset` of the block being made (`out`), or, between blocks (`out` null),
    // for the start of the next one.
    void midi_emit(
        MidiInNode *m, EventBuf *out, size_t offset, uint8_t type, uint8_t note, uint8_t vel)
    {
        const pcl_event_t e { (uint16_t)offset, type, note, vel };
        if (out && out->count < out->cap)
            out->data[out->count++] = e;
        else if (!out && m->n < kMidiQueue)
            m->q[m->n++] = e;
        else
            m->dropped++;
    }
    // a note off for every note the device holds
    void midi_release(MidiInNode *m, size_t slot, EventBuf *out = nullptr, size_t offset = 0)
    {
        for (int note = 0; note < 128; note++)
            if (m->held[slot][note >> 3] & (1u << (note & 7)))
                midi_emit(m, out, offset, PCL_EVT_NOTE_OFF, (uint8_t)note, 0);
        std::memset(m->held[slot], 0, sizeof m->held[slot]);
    }
    void midi_release_all(MidiInNode *m)
    {
        for (size_t slot = 0; slot < midi::kMaxInputs; slot++)
            midi_release(m, slot);
    }
    bool midi_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *m = as<MidiInNode>(s);
        m->channel = (uint8_t)std::clamp(to_int(p[MIDI_CHANNEL]), 0, 16);
        m->device = kMidiAnyDevice;
        m->k = 1.0f - std::exp(-1.0f / (0.003f * (float)ctx.sample_rate)); // 3 ms
        return true;
    }
    void midi_reset(void *s)
    {
        auto *m = as<MidiInNode>(s);
        m->n = 0;
        m->in.head = m->in.n = 0;
        std::memset(m->held, 0, sizeof m->held);
        m->bend = m->mod = m->pressure = m->velocity = 0;
        m->bend_s = m->mod_s = m->pressure_s = 0;
    }
    // One message taking effect at `offset` of the block.
    void midi_apply(MidiInNode *m, const midi::Msg &msg, EventBuf &out, size_t offset)
    {
        using midi::Type;
        const size_t slot = msg.slot < midi::kMaxInputs ? msg.slot : 0;
        if (msg.type == Type::DeviceLost)
        {
            midi_release(m, slot, &out, offset);
            return;
        }
        const uint8_t note = msg.data & 0x7F, bit = (uint8_t)(1u << (note & 7));
        const float   v = (float)std::min<int>(msg.value, 127) / 127.0f;
        switch (msg.type)
        {
        case Type::NoteOn:
            midi_emit(
                m, &out, offset, PCL_EVT_NOTE_ON, note, (uint8_t)std::min<int>(msg.value, 127));
            m->held[slot][note >> 3] |= bit;
            m->newest = note;
            m->velocity = v;
            break;
        case Type::NoteOff:
            midi_emit(m, &out, offset, PCL_EVT_NOTE_OFF, note, 0);
            m->held[slot][note >> 3] &= (uint8_t)~bit;
            break;
        case Type::Control:
            if (msg.data == 1) // the mod wheel
                m->mod = v;
            else if (msg.data == 120 || msg.data == 123) // all sound off, all notes off
                midi_release(m, slot, &out, offset);
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
    void midi_process(void *s, NodeIo &io)
    {
        auto     *m = as<MidiInNode>(s);
        EventBuf &out = *io.evt_out[0];
        out.count = 0;
        for (uint32_t i = 0; i < m->n; i++) // what was released between blocks
        {
            if (out.count < out.cap)
                out.data[out.count++] = m->q[i];
            else
                m->dropped++;
        }
        m->n = 0;
        // the signals, in the order of the ports: pitch bend, mod wheel, pressure, velocity. They
        // are made in stretches, from one message to the next.
        pcl_sample_t *bend = io.sig_out[0], *mod = io.sig_out[1], *pres = io.sig_out[2],
                     *vel = io.sig_out[3];
        size_t pos = 0;
        auto   run = [&](size_t to)
        {
            for (; pos < to; pos++)
            {
                m->bend_s += m->k * (m->bend - m->bend_s);
                m->mod_s += m->k * (m->mod - m->mod_s);
                m->pressure_s += m->k * (m->pressure - m->pressure_s);
                if (bend)
                    bend[pos] = to_sample(m->bend_s);
                if (mod)
                    mod[pos] = to_sample(m->mod_s);
                if (pres)
                    pres[pos] = to_sample(m->pressure_s);
                if (vel)
                    vel[pos] = to_sample(m->velocity);
            }
        };
        while (const TimedMsg *t = timed_next(m->in, io.time + io.frames))
        {
            const size_t offset = t->at <= io.time ? 0 : (size_t)(t->at - io.time);
            run(std::max(pos, offset));
            midi_apply(m, t->m, out, offset);
        }
        run(io.frames);
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
                    midi_release_all(m);
                m->channel = c;
                return true;
            },
            kMidiChannels),
    };
    static_assert(std::size(kMidiInParams) == MIDI_COUNT);

    // ============================================================================================================
    // MIDI CC One controller (a knob, a fader, a pedal) as a signal, 0 .. 1, smoothed over a few
    // milliseconds so that its 128 steps do not click. It starts at 0 and keeps its value when the
    // controller number, the channel or the device changes.
    struct MidiCcNode
    {
        MidiQueue in;
        uint8_t   channel; // 0: every channel, else 1..16
        uint8_t   cc;      // the controller number, 0..119
        int       device;  // kMidiAnyDevice, kMidiNoDevice, or a slot
        float     value, value_s, k;
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
        c->device = kMidiAnyDevice;
        c->k = 1.0f - std::exp(-1.0f / (0.003f * (float)ctx.sample_rate)); // 3 ms
        return true;
    }
    void cc_reset(void *s)
    {
        auto *c = as<MidiCcNode>(s);
        c->value = c->value_s = 0;
        c->in.head = c->in.n = 0;
    }
    void cc_process(void *s, NodeIo &io)
    {
        auto         *c = as<MidiCcNode>(s);
        pcl_sample_t *out = io.sig_out[0];
        size_t        pos = 0;
        auto          run = [&](size_t to)
        {
            for (; pos < to; pos++)
            {
                c->value_s += c->k * (c->value - c->value_s);
                if (out)
                    out[pos] = to_sample(c->value_s);
            }
        };
        while (const TimedMsg *t = timed_next(c->in, io.time + io.frames))
        {
            run(std::max(pos, t->at <= io.time ? (size_t)0 : (size_t)(t->at - io.time)));
            c->value = (float)std::min<int>(t->m.value, 127) / 127.0f;
        }
        run(io.frames);
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
    // MIDI Clock Another device's or program's tempo (docs/midi.md, step 9): its MIDI clock, 24
    // ticks a beat, as clock pulses at this module's own pulses per beat, with its start and stop.
    // At 24 a tick is a pulse. Below, ticks are skipped evenly (12: every other one); above, a tick
    // makes several pulses, the first on the tick and the others spread over the time the last tick
    // took (a tick that comes sooner sends any still waiting at once, so no pulse is lost). With
    // Start/stop on, it sends pulses only between a start (or continue) and a stop, as a sequencer
    // should hear them; off, it passes every tick on and ignores the transport.
    enum
    {
        MCLK_PPQN,
        MCLK_FOLLOW,
        MCLK_COUNT
    };
    constexpr uint32_t kMidiPpqn = 24;
    struct MidiClockNode
    {
        MidiQueue in;
        int       device; // kMidiAnyDevice, kMidiNoDevice, or a slot
        uint32_t  ppqn;
        bool      follow, running;
        uint32_t  acc;           // pulses owed, in 24ths
        bool      ticked;        // a tick has come (since init or reset)
        uint64_t  last_tick;     // when the last one came
        uint64_t  period;        // samples between the last two ticks (0: not known)
        uint64_t  pending[4];    // pulses still to send in the coming samples (a tick makes at most 4)
        uint32_t  npending;
    };
    // A start: the next tick is the first of a beat, so it makes a pulse even below 24 ppqn.
    void mclk_rewind(MidiClockNode *c)
    {
        c->acc = (kMidiPpqn - c->ppqn % kMidiPpqn) % kMidiPpqn;
        c->npending = 0;
    }
    bool mclk_init(void *s, const NodeContext &, const float *p)
    {
        auto *c = as<MidiClockNode>(s);
        c->device = kMidiAnyDevice;
        c->ppqn = (uint32_t)std::clamp(to_int(p[MCLK_PPQN]), 1, 96);
        c->follow = to_int(p[MCLK_FOLLOW]) != 0;
        c->running = !c->follow;
        mclk_rewind(c);
        return true;
    }
    void mclk_reset(void *s)
    {
        auto *c = as<MidiClockNode>(s);
        c->in.head = c->in.n = 0;
        c->running = !c->follow;
        c->ticked = false;
        c->last_tick = c->period = 0;
        mclk_rewind(c);
    }
    void mclk_process(void *s, NodeIo &io)
    {
        using midi::Type;
        auto          *c = as<MidiClockNode>(s);
        EventBuf      &out = *io.evt_out[0];
        const uint64_t end = io.time + io.frames;
        out.count = 0;
        auto emit = [&](uint8_t type, uint64_t at)
        {
            if (out.count < out.cap)
                out.data[out.count++] = pcl_event_t {
                    (uint16_t)(at <= io.time ? 0 : at - io.time), type, 0, 0
                };
        };
        // the pulses waiting from earlier ticks, up to `upto` (all of them if `all`)
        auto flush = [&](uint64_t upto, bool all)
        {
            uint32_t k = 0;
            for (; k < c->npending && (all || c->pending[k] < upto); k++)
                emit(PCL_EVT_CLOCK, std::min(c->pending[k], upto));
            std::memmove(c->pending, c->pending + k, (c->npending - k) * sizeof(uint64_t));
            c->npending -= k;
        };
        while (true)
        {
            const uint64_t next = c->in.head < c->in.n ? c->in.q[c->in.head].at : end;
            flush(std::min(next, end), false);
            const TimedMsg *t = timed_next(c->in, end);
            if (!t)
                break;
            const uint64_t at = std::max(t->at, io.time);
            switch (t->m.type)
            {
            case Type::Clock:
            {
                if (c->ticked)
                    c->period = at - c->last_tick;
                c->ticked = true;
                c->last_tick = at;
                if (!c->running)
                    break;
                flush(at, true); // (late: this tick came before they were due)
                c->acc += c->ppqn;
                const uint32_t pulses = c->acc / kMidiPpqn;
                c->acc %= kMidiPpqn;
                for (uint32_t j = 0; j < pulses; j++)
                {
                    const uint64_t when = at + (c->period * j) / pulses;
                    if (j == 0 || when < end)
                        emit(PCL_EVT_CLOCK, when);
                    else if (c->npending < std::size(c->pending))
                        c->pending[c->npending++] = when;
                }
                break;
            }
            case Type::Start:
                if (!c->follow)
                    break;
                c->running = true;
                mclk_rewind(c);
                emit(PCL_EVT_START, at);
                break;
            case Type::Continue:
                if (c->follow)
                    c->running = true;
                break;
            case Type::Stop:
            case Type::DeviceLost:
                if (!c->follow || !c->running)
                    break;
                c->running = false;
                c->npending = 0;
                emit(PCL_EVT_STOP, at);
                break;
            default:
                break;
            }
        }
    }
    const PortDesc  kMidiClockPorts[] = { { "clock", "CLOCK", Kind::Event, true } };
    const ParamDesc kMidiClockParams[] = {
        param("ppqn", "Pulses per beat", "", 1, 96, 24, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                as<MidiClockNode>(s)->ppqn = (uint32_t)std::clamp(to_int(v), 1, 96);
                return true;
            }),
        param("follow", "Start/stop", "", 0, 1, 1, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                auto *c = as<MidiClockNode>(s);
                c->follow = to_int(v) != 0;
                if (!c->follow)
                    c->running = true; // every tick from now on
                return true;
            }),
    };
    static_assert(std::size(kMidiClockParams) == MCLK_COUNT);

    // ============================================================================================================
    // MIDI Out Notes and the clock to another device or program (docs/midi.md, step 9). NOTES:
    // note on and off on its channel; CLOCK: the clock's pulses as MIDI clock (whatever their
    // rate: send it 24 pulses a beat), and start and stop. The engine takes what it made after each
    // block (midi_out_take) and sends it a device buffer later, as MIDI in is heard. It remembers
    // the notes it holds, and lets go of them (note offs) on a change of channel or output, on
    // reset, and on stop.
    constexpr size_t kMidiOutMax = 256; // messages per block
    struct MidiOutNode
    {
        int        device;  // the output's slot, or kMidiAnyDevice / kMidiNoDevice: none
        uint8_t    channel; // 0..15
        int16_t    held[128]; // per note: the slot it was sent to, -1 if not held (ch: held_ch)
        uint8_t    held_ch[128];
        MidiOutMsg out[kMidiOutMax];
        uint32_t   nout;
        MidiOutMsg pending[kMidiOutMax]; // made between blocks: sent at the start of the next
        uint32_t   npending;
        uint32_t   dropped;
    };
    void mout_add(MidiOutMsg *buf, uint32_t &n, MidiOutNode *o, uint16_t offset, int slot,
        uint8_t len, uint8_t b0, uint8_t b1 = 0, uint8_t b2 = 0)
    {
        if (slot < 0)
            return; // no output: nothing is sent
        if (n == kMidiOutMax)
        {
            o->dropped++;
            return;
        }
        buf[n++] = MidiOutMsg { offset, (uint8_t)slot, len, { b0, b1, b2 } };
    }
    // Note offs for every note it holds, between blocks (they go out first in the next).
    void mout_release_all(MidiOutNode *o)
    {
        for (int k = 0; k < 128; k++)
            if (o->held[k] >= 0)
            {
                mout_add(o->pending, o->npending, o, 0, o->held[k], 3,
                    (uint8_t)(0x80 | o->held_ch[k]), (uint8_t)k, 0);
                o->held[k] = -1;
            }
    }
    bool mout_init(void *s, const NodeContext &, const float *p)
    {
        auto *o = as<MidiOutNode>(s);
        o->device = kMidiNoDevice;
        o->channel = (uint8_t)std::clamp(to_int(p[0]), 0, 15);
        for (int16_t &h : o->held)
            h = -1;
        return true;
    }
    void mout_reset(void *s) { mout_release_all(as<MidiOutNode>(s)); }
    void mout_process(void *s, NodeIo &io)
    {
        auto           *o = as<MidiOutNode>(s);
        const EventBuf &a = *io.evt_in[0], &b = *io.evt_in[1];
        const int       slot = o->device >= 0 ? o->device : -1;
        o->nout = 0;
        for (uint32_t k = 0; k < o->npending; k++)
            if (o->nout < kMidiOutMax)
                o->out[o->nout++] = o->pending[k];
        o->npending = 0;
        // the two inputs in time order, and at one sample in the library's order (pcl_event.h):
        // STOP, NOTE_OFF, START, CLOCK, NOTE_ON, so a note is let go before the next starts and a
        // start reaches the other side before the notes that follow it
        auto rank = [](const pcl_event_t &e)
        {
            switch (e.type)
            {
            case PCL_EVT_STOP:
                return 0;
            case PCL_EVT_NOTE_OFF:
                return 1;
            case PCL_EVT_START:
                return 2;
            case PCL_EVT_CLOCK:
                return 3;
            default:
                return e.velocity == 0 ? 1 : 4; // (a note on of velocity 0 is a note off)
            }
        };
        size_t i = 0, j = 0;
        while (i < a.count || j < b.count)
        {
            const bool from_a = j >= b.count
                || (i < a.count
                    && (a.data[i].offset < b.data[j].offset
                        || (a.data[i].offset == b.data[j].offset
                            && rank(a.data[i]) <= rank(b.data[j]))));
            const pcl_event_t &e = from_a ? a.data[i++] : b.data[j++];
            const uint8_t ch = o->channel;
            switch (e.type)
            {
            case PCL_EVT_NOTE_ON:
                if (e.velocity > 0 && e.note < 128)
                {
                    mout_add(o->out, o->nout, o, e.offset, slot, 3, (uint8_t)(0x90 | ch), e.note,
                        std::min<uint8_t>(e.velocity, 127));
                    if (slot >= 0)
                    {
                        o->held[e.note] = (int16_t)slot;
                        o->held_ch[e.note] = ch;
                    }
                    break;
                }
                [[fallthrough]]; // (velocity 0: a note off)
            case PCL_EVT_NOTE_OFF:
                if (e.note < 128 && o->held[e.note] >= 0)
                {
                    mout_add(o->out, o->nout, o, e.offset, o->held[e.note], 3,
                        (uint8_t)(0x80 | o->held_ch[e.note]), e.note, 0);
                    o->held[e.note] = -1;
                }
                break;
            case PCL_EVT_CLOCK:
                mout_add(o->out, o->nout, o, e.offset, slot, 1, 0xF8);
                break;
            case PCL_EVT_START:
                mout_add(o->out, o->nout, o, e.offset, slot, 1, 0xFA);
                break;
            case PCL_EVT_STOP:
                mout_add(o->out, o->nout, o, e.offset, slot, 1, 0xFC);
                for (int k = 0; k < 128; k++) // (a sequencer lets go of its note on a stop too)
                    if (o->held[k] >= 0)
                    {
                        mout_add(o->out, o->nout, o, e.offset, o->held[k], 3,
                            (uint8_t)(0x80 | o->held_ch[k]), (uint8_t)k, 0);
                        o->held[k] = -1;
                    }
                break;
            default:
                break;
            }
        }
    }
    // (not "1".."16": a patch file writes a choice by its name, and a name that is a number would
    // be read back as the choice's index)
    constexpr std::string_view kOutChannels[] = { "ch 1", "ch 2", "ch 3", "ch 4", "ch 5", "ch 6",
        "ch 7", "ch 8", "ch 9", "ch 10", "ch 11", "ch 12", "ch 13", "ch 14", "ch 15", "ch 16" };
    const PortDesc  kMidiOutPorts[] = { { "notes", "NOTES", Kind::Event, false },
        { "clock", "CLOCK", Kind::Event, false } };
    const ParamDesc kMidiOutParams[] = {
        param(
            "channel", "Channel", "", 0, 15, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                auto         *o = as<MidiOutNode>(s);
                const uint8_t ch = (uint8_t)std::clamp(to_int(v), 0, 15);
                if (ch != o->channel)
                    mout_release_all(o);
                o->channel = ch;
                return true;
            },
            kOutChannels),
    };

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
                    f.window_ms = (float)((double)n->pos * 1000.0 / n->sr);
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

// A message for a MIDI In node: it waits in the node's queue until its sample. Filtered here by the
// node's device and channel as they are now.
void midi_in_push(void *state, const midi::Msg &msg, uint64_t at)
{
    using midi::Type;
    auto *m = as<MidiInNode>(state);
    if (msg.type != Type::DeviceLost)
    {
        if (msg.type >= Type::Clock) // the realtime messages: not used yet (docs/midi.md, step 9)
            return;
        if (!listens_to(m->device, msg) || (m->channel && msg.channel + 1 != m->channel))
            return;
    }
    if (!timed_push(m->in, at, msg))
        m->dropped++;
}

void midi_cc_push(void *state, const midi::Msg &msg, uint64_t at)
{
    auto *c = as<MidiCcNode>(state);
    if (msg.type != midi::Type::Control || msg.data != c->cc || !listens_to(c->device, msg))
        return;
    if (c->channel && msg.channel + 1 != c->channel)
        return;
    timed_push(c->in, at, msg); // (full: this one change is lost; the next one is taken)
}

// The realtime messages for a MIDI Clock node (and the loss of its device, which stops it).
void midi_clock_push(void *state, const midi::Msg &msg, uint64_t at)
{
    using midi::Type;
    auto *c = as<MidiClockNode>(state);
    if (msg.type < Type::Clock || !listens_to(c->device, msg))
        return;
    timed_push(c->in, at, msg); // (full: this tick is lost)
}

size_t midi_out_take(void *state, MidiOutMsg *out, size_t cap)
{
    auto        *o = as<MidiOutNode>(state);
    const size_t n = std::min<size_t>(o->nout, cap);
    std::memcpy(out, o->out, n * sizeof(MidiOutMsg));
    o->nout = 0;
    return n;
}

MidiTakeFn midi_source(const ModuleDesc &desc)
{
    return &desc == &kMidiOutDesc ? midi_out_take : nullptr;
}

bool has_midi_device(const ModuleDesc &desc) { return midi_sink(desc) || midi_source(desc); }

MidiPushFn midi_sink(const ModuleDesc &desc)
{
    return &desc == &kMidiInDesc  ? midi_in_push
        : &desc == &kMidiCcDesc   ? midi_cc_push
        : &desc == &kMidiClockDesc ? midi_clock_push
                                  : nullptr;
}

// The device a MIDI module listens to: a slot, -1 for every device, -2 for none (its device is not
// known here). The audio thread, as for a parameter. A MIDI In lets go of the notes it holds.
bool midi_set_device(const ModuleDesc &desc, void *state, int device)
{
    if (&desc == &kMidiInDesc)
    {
        auto *m = as<MidiInNode>(state);
        if (device != m->device)
            midi_release_all(m);
        m->device = device;
    }
    else if (&desc == &kMidiCcDesc)
        as<MidiCcNode>(state)->device = device;
    else if (&desc == &kMidiClockDesc)
        as<MidiClockNode>(state)->device = device;
    else if (&desc == &kMidiOutDesc)
    {
        auto *o = as<MidiOutNode>(state);
        if (device != o->device)
            mout_release_all(o); // (to the output they were sent to)
        o->device = device;
    }
    else
        return false;
    return true;
}

uint32_t midi_in_dropped(const void *state)
{
    return static_cast<const MidiInNode *>(state)->dropped;
}

#define RACK_GLUE(var, id, name, cat, hp, T, ports, params, init, reset, process, cap, blurb)      \
    const ModuleDesc var = { id, name, cat, hp, sizeof(T), alignof(T), ports, params, false, init, \
        reset, process, nullptr, cap, blurb, nullptr }

RACK_GLUE(kKeyboardDesc, "keyboard", "Keyboard", kEvent, 6, KeyboardNode, kKeyboardPorts, { },
    kb_init, kb_reset, kb_process, kb_cap,
    "The computer keyboard and an on-screen strip as a piano");
RACK_GLUE(kMidiInDesc, "midi_in", "MIDI In", kEvent, 8, MidiInNode, kMidiInPorts, kMidiInParams,
    midi_init, midi_reset, midi_process, midi_cap,
    "Notes, pitch bend, the mod wheel and pressure from MIDI keyboards and controllers");
RACK_GLUE(kMidiCcDesc, "midi_cc", "MIDI CC", kEvent, 6, MidiCcNode, kMidiCcPorts, kMidiCcParams,
    cc_init, cc_reset, cc_process, nullptr,
    "One knob, fader or pedal of a MIDI controller as a signal");
RACK_GLUE(kMidiClockDesc, "midi_clock", "MIDI Clock", kEvent, 6, MidiClockNode, kMidiClockPorts,
    kMidiClockParams, mclk_init, mclk_reset, mclk_process, nullptr,
    "Another device's tempo: its MIDI clock as clock pulses, with its start and stop");
RACK_GLUE(kMidiOutDesc, "midi_out", "MIDI Out", kEvent, 6, MidiOutNode, kMidiOutPorts,
    kMidiOutParams, mout_init, mout_reset, mout_process, nullptr,
    "Sends notes and the clock to another device or program, on one channel");
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
