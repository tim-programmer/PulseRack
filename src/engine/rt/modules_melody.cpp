#include "engine/rt/modules_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace rack
{
namespace
{
    enum MelodyParam
    {
        External,
        Tempo,
        Division,
        Length,
        Root,
        ScaleChoice,
        Density,
        Accent,
        GateLength,
        Mutation,
        Lock,
        Seed
    };

    // A step is a scale degree counted from the root (negative is below it), and two
    // weights: a step rests when its rest weight is above Density, and is accented when its
    // accent weight is below Accent. Turning those knobs takes away or adds steps in a fixed
    // order, so the loop keeps its shape.
    struct MelodyNode
    {
        pcl_clock_t             clock;
        std::array<int8_t, 32>  degree;
        std::array<uint8_t, 32> rest, accent;
        uint32_t                random;
        unsigned                next, pulses, gate, active;
        int                     root, scale, length, division, seed, applied_seed;
        float                   mutation, density, accent_amount, gate_length;
        bool                    locked, external, previous_external;
    };

    uint32_t draw(MelodyNode &n)
    {
        n.random ^= n.random << 13;
        n.random ^= n.random >> 17;
        n.random ^= n.random << 5;
        return n.random;
    }

    constexpr int lowest(const MelodyNode &) { return -3; }
    int highest(const MelodyNode &n) { return kScaleSize[n.scale] + 4; }

    // Folds a degree back into the range, so a step past the edge turns around.
    int fold(const MelodyNode &n, int d)
    {
        const int lo = lowest(n), hi = highest(n);
        if (d > hi)
            d = 2 * hi - d;
        if (d < lo)
            d = 2 * lo - d;
        return std::clamp(d, lo, hi);
    }

    // The next note of the walk: mostly a step to a neighbouring degree, sometimes a repeat or
    // a third, now and then a leap, pulled back towards the middle of the range.
    int walk(MelodyNode &n, int from)
    {
        const int size = kScaleSize[n.scale];
        const int r = static_cast<int>(draw(n) % 100);
        if (r >= 94) // a leap home: the root or its octave, whichever is nearer
            return from > size / 2 ? size : 0;
        // A pentatonic degree is a wider step, so its leaps stop at three degrees (up to a sixth).
        const int   leap = 3 + static_cast<int>(draw(n) % 2);
        const int   distance = r < 14 ? 0 : r < 60 ? 1 : r < 84 ? 2 : std::min(leap, size == 5 ? 3 : 4);
        const float centre = size * 0.5f;
        const float up = std::clamp(0.5f - (static_cast<float>(from) - centre) * 0.09f, 0.1f, 0.9f);
        const bool  rising = static_cast<float>(draw(n) % 1000) < up * 1000.f;
        return fold(n, from + (rising ? distance : -distance));
    }

    // On the beat a step is more likely to sound and to be accented.
    uint8_t weight(MelodyNode &n, size_t step, bool favour_beats)
    {
        unsigned w = draw(n) % 256;
        if (favour_beats && step % 4 == 0)
            w /= 3;
        else if (favour_beats && step % 2 == 0)
            w = w * 2 / 3;
        return static_cast<uint8_t>(w);
    }

    void reseed(MelodyNode &n)
    {
        n.applied_seed = n.seed;
        n.random = static_cast<uint32_t>(n.seed) * 2654435761u | 1u;
        int d = 0;
        for (size_t i = 0; i < n.degree.size(); ++i)
        {
            if (i > 0)
                d = walk(n, d);
            n.degree[i] = static_cast<int8_t>(d);
            n.rest[i] = i == 0 ? 0 : weight(n, i, true);
            n.accent[i] = weight(n, i, true);
        }
    }

    // A changed step moves by one or two degrees, so the loop drifts instead of jumping; now
    // and then it also gets a new rest or accent weight. The first step stays on the root.
    void mutate(MelodyNode &n, size_t i)
    {
        const int distance = 1 + static_cast<int>(draw(n) % 2);
        if (i > 0)
            n.degree[i] = static_cast<int8_t>(
                fold(n, n.degree[i] + (draw(n) % 2 ? distance : -distance)));
        if (i > 0 && draw(n) % 4 == 0)
            n.rest[i] = weight(n, i, true);
        if (draw(n) % 4 == 0)
            n.accent[i] = weight(n, i, true);
    }

    unsigned note_of(const MelodyNode &n, int d)
    {
        const int size = kScaleSize[n.scale];
        const int octave = d >= 0 ? d / size : -((size - 1 - d) / size);
        const int note = n.root + 12 * octave + kScaleSteps[n.scale][d - octave * size];
        return static_cast<unsigned>(std::clamp(note, 0, 127));
    }

    void melody_reset(void *state)
    {
        auto &n = *as<MelodyNode>(state);
        pcl_clock_reset(&n.clock);
        pcl_clock_start(&n.clock);
        reseed(n);
        n.next = n.pulses = n.gate = 0;
        n.active = 128;
        n.previous_external = n.external;
    }

    template <auto Member> bool set(void *state, float value)
    {
        auto &field = as<MelodyNode>(state)->*Member;
        field = static_cast<std::remove_reference_t<decltype(field)>>(value);
        return true;
    }

    const PortDesc kPorts[] = {
        { "clock", "CLOCK", Kind::Event, false },
        { "note", "NOTE", Kind::Event, true },
    };
    const ParamDesc kParams[] = {
        param("external", "External clock", "", 0, 1, 0, Scale::Toggle, Control::Button,
            set<&MelodyNode::external>),
        param("bpm", "Tempo", "bpm", 20, 300, 110, Scale::Log, Control::Knob, [](void *s, float v)
            { return ok(pcl_clock_set_bpm(&as<MelodyNode>(s)->clock, to_hz(v))); }),
        param("division", "Division", "pulses", 1, 96, 6, Scale::Int, Control::Knob,
            set<&MelodyNode::division>),
        param("length", "Length", "steps", 1, 32, 8, Scale::Int, Control::Knob,
            set<&MelodyNode::length>),
        param("root", "Root note", "note", 0, 104, 60, Scale::Int, Control::Knob,
            set<&MelodyNode::root>),
        param("scale", "Scale", "", 0, 3, 1, Scale::Enum, Control::Selector,
            set<&MelodyNode::scale>, kScaleNames),
        param("density", "Density", "%", 0, 100, 85, Scale::Linear, Control::Knob,
            set<&MelodyNode::density>),
        param("accent", "Accent", "%", 0, 100, 30, Scale::Linear, Control::Knob,
            set<&MelodyNode::accent_amount>),
        param("gate", "Gate length", "%", 5, 100, 60, Scale::Linear, Control::Knob,
            set<&MelodyNode::gate_length>),
        param("mutation", "Mutation", "%", 0, 100, 5, Scale::Linear, Control::Knob,
            set<&MelodyNode::mutation>),
        param(
            "lock", "Lock", "", 0, 1, 0, Scale::Toggle, Control::Button, set<&MelodyNode::locked>),
        param("seed", "Seed", "", 1, 9999, 1, Scale::Int, Control::Knob, set<&MelodyNode::seed>),
    };

    bool melody_init(void *state, const NodeContext &ctx, const float *p)
    {
        auto              &n = *as<MelodyNode>(state);
        pcl_clock_config_t cfg { };
        cfg.sample_rate_hz = ctx.sample_rate;
        cfg.bpm = to_hz(p[Tempo]);
        cfg.ppqn = 24;
        if (!ok(pcl_clock_init(&n.clock, &cfg)))
            return false;
        for (size_t i = 0; i <= Seed; ++i)
            kParams[i].apply(state, p[i]);
        melody_reset(state);
        return true;
    }

    void melody_process(void *state, NodeIo &io)
    {
        auto        &n = *as<MelodyNode>(state);
        auto        &out = *io.evt_out[0];
        const auto  &in = *io.evt_in[0];
        pcl_event_t  internal[PCL_MAX_BLOCK + 2];
        const size_t internal_count
            = pcl_clock_process(&n.clock, io.frames, internal, std::size(internal));
        const auto  *events = n.external ? in.data : internal;
        const size_t count = n.external ? in.count : internal_count;
        out.count = 0;
        auto emit = [&](uint16_t at, uint8_t type, unsigned note, uint8_t velocity)
        {
            if (out.count < out.cap)
                out.data[out.count++] = { at, type, static_cast<uint8_t>(note), velocity };
        };
        auto release = [&](uint16_t at)
        {
            if (n.active < 128)
                emit(at, PCL_EVT_NOTE_OFF, n.active, 0);
            n.active = 128;
        };

        if (n.seed != n.applied_seed)
        {
            release(0);
            reseed(n);
            n.next = n.pulses = n.gate = 0;
        }
        if (n.previous_external != n.external)
        {
            release(0);
            n.pulses = n.gate = 0;
            n.previous_external = n.external;
        }

        for (size_t i = 0; i < count; ++i)
        {
            const auto &e = events[i];
            if (e.type == PCL_EVT_START || e.type == PCL_EVT_STOP)
            {
                release(e.offset);
                if (e.type == PCL_EVT_START)
                    n.next = n.pulses = 0;
                n.gate = 0;
                continue;
            }
            if (e.type != PCL_EVT_CLOCK)
                continue;
            if (n.gate && --n.gate == 0)
                release(e.offset);
            if (n.pulses && --n.pulses != 0)
                continue;

            release(e.offset);
            n.next %= static_cast<unsigned>(n.length);
            const size_t step = n.next++;
            n.pulses = n.division;
            if (!n.locked && (draw(n) >> 8) < n.mutation * (16777216.f / 100.f))
                mutate(n, step);
            if (step > 0 && n.rest[step] >= n.density * 2.56f)
                continue;
            n.active = note_of(n, n.degree[step]);
            const bool accented = n.accent[step] < n.accent_amount * 2.56f;
            emit(e.offset, PCL_EVT_NOTE_ON, n.active, accented ? 118 : 82);
            // A gate as long as the step is held until the next step's note.
            const int gate = static_cast<int>(std::lround(n.division * n.gate_length / 100.f));
            n.gate = gate >= n.division ? 0 : static_cast<unsigned>(std::max(1, gate));
        }
    }

    size_t melody_capacity(size_t input) { return std::max<size_t>(64, input * 2 + 1); }
} // namespace

RACK_DESC(kMelodyDesc, "melody", "Generative Melody", "Event", 16, MelodyNode, kPorts, kParams,
    false, melody_init, melody_reset, melody_process, nullptr, melody_capacity,
    "A melody loop that walks the scale, with rests and accents, and drifts as it plays. Lock holds it.");

} // namespace rack
