// FM Synth: a polyphonic four-operator FM synthesizer with its own presets, a chorus, a ping-pong
// delay and an arpeggiator (docs/patch-engine.md, *FM Synth*). The operators, envelopes and
// effects are written here; the arpeggiator and its clock are PulseCoreLib's.
#include "engine/rt/modules_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <type_traits>

namespace rack
{
namespace
{
    constexpr float  kTwoPi = 6.28318530718f, kSilent = 1e-5f;
    constexpr size_t kVoices = 8, kOps = 4;

    // ---- the operators' frequency ratios: whole numbers first, then a few inharmonic ones
    // (named "x2", not "2": a number in a patch file is read as the choice's index)
    constexpr std::string_view kRatioNames[] = { "x0.5", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
        "x8", "x9", "x10", "x11", "x12", "x14", "x16", "x1.41", "x2.76", "x3.5", "x5.4" };
    constexpr float kRatios[] = { 0.5f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16, 1.41f, 2.76f,
        3.5f, 5.4f };
    static_assert(std::size(kRatioNames) == std::size(kRatios));
    constexpr float ratio(float r) // a ratio's index, for the presets
    {
        for (size_t i = 0; i < std::size(kRatios); i++)
            if (kRatios[i] == r)
                return (float)i;
        return 1;
    }

    // ---- the algorithms: which operators modulate which (always a higher number into a lower
    // one, so they are computed 4, 3, 2, 1) and which are heard
    struct Algorithm
    {
        uint8_t into[kOps]; // bit j set: operator j+1 modulates this one
        uint8_t carriers;   // bit i set: operator i+1 is heard
    };
    constexpr uint8_t bit(int op) { return (uint8_t)(1u << (op - 1)); }
    constexpr Algorithm kAlgorithms[] = {
        { { bit(2), bit(3), bit(4), 0 }, bit(1) },                          // 4>3>2>1
        { { bit(2), bit(3) | bit(4), 0, 0 }, bit(1) },                      // 4+3>2>1
        { { bit(2) | bit(3), 0, bit(4), 0 }, bit(1) },                      // 4>3, 2 > 1
        { { bit(2), 0, bit(4), 0 }, bit(1) | bit(3) },                      // 4>3  2>1
        { { bit(4), bit(4), bit(4), 0 }, bit(1) | bit(2) | bit(3) },        // 4 > 3, 2, 1
        { { 0, bit(3), bit(4), 0 }, bit(1) | bit(2) },                      // 4>3>2  1
        { { 0, 0, bit(4), 0 }, bit(1) | bit(2) | bit(3) },                  // 4>3  2  1
        { { 0, 0, 0, 0 }, bit(1) | bit(2) | bit(3) | bit(4) },              // 4 3 2 1
    };
    constexpr std::string_view kAlgorithmNames[]
        = { "4>3>2>1", "4+3>2>1", "4>3,2>1", "4>3 2>1", "4>3,2,1", "4>3>2 1", "4>3 2 1", "4 3 2 1" };
    static_assert(std::size(kAlgorithmNames) == std::size(kAlgorithms));

    constexpr std::string_view kArpModes[] = { "up", "down", "up-down", "random", "as played" };
    constexpr pcl_arp_mode_t   kArpModeValues[]
        = { PCL_ARP_UP, PCL_ARP_DOWN, PCL_ARP_UPDOWN, PCL_ARP_RANDOM, PCL_ARP_AS_PLAYED };
    constexpr std::string_view kArpRates[] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    constexpr uint32_t         kArpPulses[] = { 24, 12, 8, 6, 4, 3 }; // at 24 pulses a beat

    // In panel order: the operators' Ratio and Level, then the sound, then the effects and the
    // arpeggiator (two full rows of knobs). The envelopes come last: they have no knobs, as the
    // panel's envelope graph sets them.
    enum Param
    {
        PRESET,
        OP_KNOBS, // each operator: ratio, level
        ALGORITHM = OP_KNOBS + 2 * kOps,
        FEEDBACK,
        BRIGHTNESS,
        VELOCITY,
        VOLUME,
        VIBRATO,
        VIBRATO_RATE,
        CHORUS,
        DELAY_MIX,
        DELAY_TIME,
        DELAY_FEEDBACK,
        ARP,
        ARP_MODE,
        ARP_RATE,
        ARP_OCTAVES,
        TEMPO,
        EXTERNAL,
        ENVELOPES, // each operator: attack, decay, sustain, release
        PARAM_COUNT = ENVELOPES + 4 * kOps
    };
    enum OpParam
    {
        RATIO,
        LEVEL,
        ATTACK,
        DECAY,
        SUSTAIN,
        RELEASE
    };

    enum Stage : uint8_t
    {
        IDLE,
        ATTACKING,
        DECAYING, // towards the sustain level, where it stays while the key is down
        RELEASING
    };

    struct Voice
    {
        float    phase[kOps], env[kOps], fb[2];
        Stage    stage[kOps];
        float    velocity; // 0 .. 1
        uint32_t age;      // when it was struck (a count), for stealing the oldest
        uint8_t  note;
        bool     held, active;
    };

    struct Operator
    {
        float ratio, level, attack_ms, decay_ms, sustain, release_ms;
        float attack_step, decay_k, release_k; // from the times, at the sample rate
    };

    struct FmSynthNode
    {
        Voice       voice[kVoices];
        Operator    op[kOps];
        uint32_t    sr, strikes;
        int         algorithm;
        float       feedback, brightness, velocity_amount, vibrato, vibrato_rate, volume;
        float       vibrato_phase;
        // chorus: one short line, read by two moving taps (left and right)
        float      *chorus_line;
        uint32_t    chorus_len, chorus_w;
        float       chorus_mix, chorus_phase;
        // delay: two lines crossing over (ping-pong), darkened in the feedback
        float      *delay_line[2];
        uint32_t    delay_len, delay_w;
        float       delay_mix, delay_time_ms, delay_feedback, delay_samples, delay_tone[2];
        // arpeggiator and its own clock
        pcl_arp_t   arp;
        pcl_clock_t clock;
        bool        arp_on, arp_was_on, external;
        void       *memory; // the lines, from init, freed by destroy
    };

    FmSynthNode &node(void *s) { return *as<FmSynthNode>(s); }

    float fall_k(float ms, uint32_t sr) // a fall to 1/1000 over `ms`
    {
        return (float)std::exp(std::log(0.001) / (std::max(ms, 0.1f) * 0.001 * (double)sr));
    }
    void op_times(Operator &o, uint32_t sr)
    {
        o.attack_step = 1.f / std::max(1.f, o.attack_ms * 0.001f * (float)sr);
        o.decay_k = fall_k(o.decay_ms, sr);
        o.release_k = fall_k(o.release_ms, sr);
    }

    template <size_t Op, OpParam What> bool op_set(void *s, float v)
    {
        auto     &n = node(s);
        Operator &o = n.op[Op];
        switch (What)
        {
        case RATIO:
            o.ratio = kRatios[std::min<size_t>((size_t)std::lround(v), std::size(kRatios) - 1)];
            break;
        case LEVEL:
            o.level = v;
            break;
        case ATTACK:
            o.attack_ms = v;
            break;
        case DECAY:
            o.decay_ms = v;
            break;
        case SUSTAIN:
            o.sustain = v;
            break;
        case RELEASE:
            o.release_ms = v;
            break;
        }
        op_times(o, n.sr);
        return true;
    }

    template <auto Member> bool set(void *s, float value)
    {
        auto &field = node(s).*Member;
        field = static_cast<std::remove_reference_t<decltype(field)>>(value);
        return true;
    }

    // ---- the parameters
#define FM_OP_KNOBS(N, RATIO_DEF, LEVEL_DEF)                                                         \
    param("op" #N "_ratio", "Op " #N " ratio", "", 0, std::size(kRatios) - 1, ratio(RATIO_DEF),      \
        Scale::Enum, Control::Knob, op_set<N - 1, RATIO>, kRatioNames),                             \
        param("op" #N "_level", "Op " #N " level", "", 0, 1, LEVEL_DEF, Scale::Linear,              \
            Control::Knob, op_set<N - 1, LEVEL>)
#define FM_OP_ENVELOPE(N, A, D, S, R)                                                               \
    param("op" #N "_attack", "Op " #N " attack", "ms", 1, 5000, A, Scale::Log, Control::Hidden,     \
        op_set<N - 1, ATTACK>),                                                                     \
        param("op" #N "_decay", "Op " #N " decay", "ms", 5, 10000, D, Scale::Log, Control::Hidden,  \
            op_set<N - 1, DECAY>),                                                                  \
        param("op" #N "_sustain", "Op " #N " sustain", "", 0, 1, S, Scale::Linear, Control::Hidden, \
            op_set<N - 1, SUSTAIN>),                                                                \
        param("op" #N "_release", "Op " #N " release", "ms", 5, 10000, R, Scale::Log,               \
            Control::Hidden, op_set<N - 1, RELEASE>)

    // the presets' names, which are the labels of the Preset selector (kPresets below)
    constexpr std::string_view kPresetNames[] = { "E. piano", "Bell", "FM bass", "Brass", "Organ",
        "Glass pad", "Harp", "Marimba", "Lead", "Clav", "Arp pluck", "Init (one sine)" };

    // The defaults are the first preset, the electric piano: a soft pair (2 into 1) for the
    // body and a bright, fast pair (4 at 14 times into 3) for the tine.
    const ParamDesc kParams[] = {
        param("preset", "Preset", "", 0, std::size(kPresetNames) - 1, 0, Scale::Enum,
            Control::Selector, [](void *, float) { return true; }, kPresetNames),
        FM_OP_KNOBS(1, 1, 0.8f),
        FM_OP_KNOBS(2, 1, 0.35f),
        FM_OP_KNOBS(3, 1, 0.45f),
        FM_OP_KNOBS(4, 14, 0.3f),
        param("algorithm", "Algorithm", "", 0, std::size(kAlgorithms) - 1, 3, Scale::Enum,
            Control::Knob, set<&FmSynthNode::algorithm>, kAlgorithmNames),
        param("feedback", "Op 4 feedback", "", 0, 1, 0, Scale::Linear, Control::Knob,
            set<&FmSynthNode::feedback>),
        param("brightness", "Brightness", "", 0, 2, 1, Scale::Linear, Control::Knob,
            set<&FmSynthNode::brightness>),
        param("velocity", "Velocity", "", 0, 1, 0.7f, Scale::Linear, Control::Knob,
            set<&FmSynthNode::velocity_amount>),
        param("volume", "Volume", "", 0, 1, 0.7f, Scale::Linear, Control::Knob,
            set<&FmSynthNode::volume>),
        param("vibrato", "Vibrato", "semitones", 0, 1, 0, Scale::Linear, Control::Knob,
            set<&FmSynthNode::vibrato>),
        param("vibrato_rate", "Vibrato rate", "Hz", 0.5f, 10, 5, Scale::Log, Control::Knob,
            set<&FmSynthNode::vibrato_rate>),
        param("chorus", "Chorus", "", 0, 1, 0.35f, Scale::Linear, Control::Knob,
            set<&FmSynthNode::chorus_mix>),
        param("delay_mix", "Delay", "", 0, 1, 0.12f, Scale::Linear, Control::Knob,
            set<&FmSynthNode::delay_mix>),
        param("delay_time", "Delay time", "ms", 40, 1500, 375, Scale::Log, Control::Knob,
            set<&FmSynthNode::delay_time_ms>),
        param("delay_feedback", "Delay feedback", "", 0, 0.9f, 0.3f, Scale::Linear, Control::Knob,
            set<&FmSynthNode::delay_feedback>),
        param("arp", "Arpeggiate", "", 0, 1, 0, Scale::Toggle, Control::Button,
            set<&FmSynthNode::arp_on>),
        param("arp_mode", "Arp mode", "", 0, std::size(kArpModes) - 1, 0, Scale::Enum,
            Control::Knob,
            [](void *s, float v)
            {
                const size_t i = std::min<size_t>((size_t)std::lround(v), std::size(kArpModes) - 1);
                return ok(pcl_arp_set_mode(&node(s).arp, kArpModeValues[i]));
            },
            kArpModes),
        param("arp_rate", "Arp rate", "", 0, std::size(kArpRates) - 1, 3, Scale::Enum, Control::Knob,
            [](void *s, float v)
            {
                const uint32_t pulses
                    = kArpPulses[std::min<size_t>((size_t)std::lround(v), std::size(kArpRates) - 1)];
                auto &n = node(s);
                return ok(pcl_arp_set_division(&n.arp, pulses))
                    && ok(pcl_arp_set_gate(&n.arp, std::max(1u, pulses / 2)));
            },
            kArpRates),
        param("arp_octaves", "Arp octaves", "", 1, PCL_ARP_MAX_OCTAVES, 1, Scale::Int, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_arp_set_octaves(&node(s).arp, (uint32_t)std::lround(v))); }),
        param("bpm", "Tempo", "bpm", 40, 240, 120, Scale::Log, Control::Knob,
            [](void *s, float v) { return ok(pcl_clock_set_bpm(&node(s).clock, to_hz(v))); }),
        param("external", "External clock", "", 0, 1, 0, Scale::Toggle, Control::Button,
            set<&FmSynthNode::external>),
        FM_OP_ENVELOPE(1, 2, 2500, 0.25f, 400),
        FM_OP_ENVELOPE(2, 2, 900, 0.15f, 400),
        FM_OP_ENVELOPE(3, 1, 700, 0, 300),
        FM_OP_ENVELOPE(4, 1, 120, 0, 120),
    };
#undef FM_OP_KNOBS
#undef FM_OP_ENVELOPE
    static_assert(std::size(kParams) == PARAM_COUNT);

    const PortDesc kPorts[] = {
        { "note", "NOTE", Kind::Event, false },
        { "clock", "CLOCK", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false },
        { "bright", "BRIGHT", Kind::Signal, false },
        { "l", "L", Kind::Signal, true },
        { "r", "R", Kind::Signal, true },
    };

    // ---- the presets (every operator setting is given, so none is left from another sound)
#define OP(N, RATIO_VALUE, LEVEL_VALUE, A, D, S, R)                                                 \
    { "op" #N "_ratio", ratio(RATIO_VALUE) }, { "op" #N "_level", LEVEL_VALUE },                     \
        { "op" #N "_attack", A }, { "op" #N "_decay", D }, { "op" #N "_sustain", S },               \
        { "op" #N "_release", R }
    constexpr PresetValue kBell[] = { OP(1, 1, 0.7f, 1, 3000, 0, 2500),
        OP(2, 3.5f, 0.45f, 1, 2500, 0, 2000), OP(3, 4, 0.25f, 1, 1500, 0, 1500),
        OP(4, 5.4f, 0.3f, 1, 1200, 0, 1000), { "algorithm", 3 }, { "chorus", 0.2f },
        { "delay_mix", 0.2f }, { "velocity", 0.6f } };
    constexpr PresetValue kBass[] = { OP(1, 1, 0.9f, 1, 600, 0.5f, 80),
        OP(2, 1, 0.55f, 1, 300, 0.25f, 80), OP(3, 2, 0.2f, 1, 150, 0, 80),
        OP(4, 1, 0.15f, 1, 100, 0, 80), { "algorithm", 0 }, { "feedback", 0.35f },
        { "chorus", 0 }, { "delay_mix", 0 }, { "velocity", 0.5f } };
    constexpr PresetValue kBrass[] = { OP(1, 1, 0.85f, 40, 400, 0.8f, 200),
        OP(2, 1, 0.6f, 80, 600, 0.5f, 250), OP(3, 1, 0.2f, 120, 800, 0.3f, 300),
        OP(4, 2, 0.15f, 60, 500, 0.2f, 300), { "algorithm", 1 }, { "feedback", 0.5f },
        { "vibrato", 0.15f }, { "chorus", 0.2f }, { "delay_mix", 0.1f } };
    constexpr PresetValue kOrgan[] = { OP(1, 1, 0.7f, 3, 100, 1, 60), OP(2, 2, 0.5f, 3, 100, 1, 60),
        OP(3, 4, 0.35f, 3, 100, 1, 60), OP(4, 0.5f, 0.5f, 3, 100, 1, 60), { "algorithm", 7 },
        { "feedback", 0.2f }, { "velocity", 0 }, { "chorus", 0.5f }, { "delay_mix", 0 } };
    constexpr PresetValue kGlassPad[] = { OP(1, 1, 0.6f, 800, 2000, 0.7f, 2000),
        OP(2, 2, 0.35f, 900, 2000, 0.6f, 2000), OP(3, 3, 0.25f, 1200, 2000, 0.5f, 2500),
        OP(4, 1.41f, 0.3f, 1500, 3000, 0.4f, 2500), { "algorithm", 4 }, { "chorus", 0.6f },
        { "delay_mix", 0.3f }, { "delay_time", 500 }, { "delay_feedback", 0.45f },
        { "velocity", 0.3f } };
    constexpr PresetValue kHarp[] = { OP(1, 1, 0.8f, 1, 1500, 0, 1200),
        OP(2, 2, 0.3f, 1, 200, 0, 200), OP(3, 1, 0.25f, 1, 400, 0, 300),
        OP(4, 4, 0.2f, 1, 150, 0, 150), { "algorithm", 2 }, { "chorus", 0.25f },
        { "delay_mix", 0.25f } };
    constexpr PresetValue kMarimba[] = { OP(1, 1, 0.8f, 1, 400, 0, 300),
        OP(2, 4, 0.35f, 1, 60, 0, 60), OP(3, 4, 0.15f, 1, 150, 0, 150),
        OP(4, 10, 0.15f, 1, 30, 0, 30), { "algorithm", 3 }, { "velocity", 0.8f },
        { "chorus", 0 }, { "delay_mix", 0.1f } };
    constexpr PresetValue kLead[] = { OP(1, 1, 0.8f, 5, 500, 0.85f, 150),
        OP(2, 1, 0.5f, 5, 800, 0.4f, 150), OP(3, 2, 0.25f, 5, 600, 0.2f, 150),
        OP(4, 1, 0.3f, 5, 400, 0.3f, 150), { "algorithm", 0 }, { "feedback", 0.6f },
        { "vibrato", 0.3f }, { "vibrato_rate", 5.5f }, { "chorus", 0.15f }, { "delay_mix", 0.2f },
        { "delay_time", 300 }, { "delay_feedback", 0.35f } };
    constexpr PresetValue kClav[] = { OP(1, 1, 0.75f, 1, 500, 0, 80), OP(2, 3, 0.5f, 1, 250, 0.1f, 80),
        OP(3, 1, 0.2f, 1, 150, 0, 80), OP(4, 1, 0.25f, 1, 100, 0, 80), { "algorithm", 0 },
        { "feedback", 0.4f }, { "velocity", 0.8f }, { "chorus", 0.1f }, { "delay_mix", 0 } };
    constexpr PresetValue kArpPluck[] = { OP(1, 1, 0.8f, 1, 300, 0, 200),
        OP(2, 2, 0.4f, 1, 150, 0, 150), OP(3, 1, 0.3f, 1, 200, 0, 200),
        OP(4, 7, 0.2f, 1, 60, 0, 60), { "algorithm", 3 }, { "arp", 1 }, { "arp_mode", 2 },
        { "arp_rate", 3 }, { "arp_octaves", 2 }, { "delay_mix", 0.3f }, { "delay_feedback", 0.4f } };
    constexpr PresetValue kInit[] = { OP(1, 1, 0.8f, 2, 100, 1, 100), OP(2, 1, 0, 2, 100, 1, 100),
        OP(3, 1, 0, 2, 100, 1, 100), OP(4, 1, 0, 2, 100, 1, 100), { "algorithm", 0 },
        { "velocity", 0.5f }, { "chorus", 0 }, { "delay_mix", 0 } };
#undef OP
    const Preset kPresets[] = { { kPresetNames[0], { } }, { kPresetNames[1], kBell },
        { kPresetNames[2], kBass }, { kPresetNames[3], kBrass }, { kPresetNames[4], kOrgan },
        { kPresetNames[5], kGlassPad }, { kPresetNames[6], kHarp }, { kPresetNames[7], kMarimba },
        { kPresetNames[8], kLead }, { kPresetNames[9], kClav }, { kPresetNames[10], kArpPluck },
        { kPresetNames[11], kInit } };
    static_assert(std::size(kPresets) == std::size(kPresetNames));

    // ---- voices
    void release_all(FmSynthNode &n)
    {
        for (Voice &v : n.voice)
            if (v.active && v.held)
            {
                v.held = false;
                for (Stage &st : v.stage)
                    if (st != IDLE)
                        st = RELEASING;
            }
    }

    // A new note takes the voice already playing it, or a silent one, or the oldest let go, or
    // the oldest. Its envelopes start again from where they are, so a stolen voice does not click.
    void note_on(FmSynthNode &n, uint8_t note, uint8_t velocity)
    {
        Voice *pick = nullptr;
        for (Voice &v : n.voice)
            if (v.active && v.note == note)
                pick = &v;
        auto oldest = [&](auto &&eligible)
        {
            Voice *best = nullptr;
            for (Voice &v : n.voice)
                if (eligible(v) && (!best || v.age < best->age))
                    best = &v;
            return best;
        };
        if (!pick)
            for (Voice &v : n.voice)
                if (!v.active)
                {
                    pick = &v;
                    break;
                }
        if (!pick)
            pick = oldest([](const Voice &v) { return !v.held; });
        if (!pick)
            pick = oldest([](const Voice &) { return true; });
        Voice &v = *pick;
        if (!v.active)
        {
            std::fill_n(v.phase, kOps, 0.f);
            std::fill_n(v.env, kOps, 0.f);
            v.fb[0] = v.fb[1] = 0;
        }
        v.note = note;
        v.velocity = (float)velocity / 127.f;
        v.age = ++n.strikes;
        v.held = v.active = true;
        std::fill_n(v.stage, kOps, ATTACKING);
    }

    void note_off(FmSynthNode &n, uint8_t note)
    {
        for (Voice &v : n.voice)
            if (v.active && v.held && v.note == note)
            {
                v.held = false;
                for (Stage &st : v.stage)
                    st = RELEASING;
            }
    }

    void handle(FmSynthNode &n, const pcl_event_t &e)
    {
        if (e.type == PCL_EVT_NOTE_ON && e.velocity > 0)
            note_on(n, e.note, e.velocity);
        else if (e.type == PCL_EVT_NOTE_OFF || e.type == PCL_EVT_NOTE_ON)
            note_off(n, e.note);
        else if (e.type == PCL_EVT_STOP)
            release_all(n);
    }

    float envelope(Voice &v, size_t i, const Operator &o)
    {
        float &e = v.env[i];
        switch (v.stage[i])
        {
        case ATTACKING:
            e += o.attack_step;
            if (e >= 1)
            {
                e = 1;
                v.stage[i] = DECAYING;
            }
            break;
        case DECAYING:
            e = o.sustain + (e - o.sustain) * o.decay_k;
            break;
        case RELEASING:
            e *= o.release_k;
            if (e < kSilent)
            {
                e = 0;
                v.stage[i] = IDLE;
            }
            break;
        case IDLE:
            break;
        }
        return e;
    }

    // ---- effects
    float read_line(const float *line, uint32_t len, uint32_t w, float delay)
    {
        float pos = (float)w - delay;
        while (pos < 0)
            pos += (float)len;
        const uint32_t i0 = (uint32_t)pos % len, i1 = (i0 + 1) % len;
        const float    t = pos - std::floor(pos);
        return line[i0] + (line[i1] - line[i0]) * t;
    }

    bool fm_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto &n = node(s);
        n.sr = ctx.sample_rate;
        n.chorus_len = ctx.sample_rate / 20 + 4;           // 50 ms
        n.delay_len = (uint32_t)(ctx.sample_rate * 1.6) + 4; // the longest time, and room
        const size_t floats = n.chorus_len + 2 * (size_t)n.delay_len;
        n.memory = std::calloc(floats, sizeof(float));
        if (!n.memory)
            return false;
        n.chorus_line = static_cast<float *>(n.memory);
        n.delay_line[0] = n.chorus_line + n.chorus_len;
        n.delay_line[1] = n.delay_line[0] + n.delay_len;

        pcl_clock_config_t clock { };
        clock.sample_rate_hz = ctx.sample_rate;
        clock.bpm = to_hz(p[TEMPO]);
        clock.ppqn = 24;
        if (!ok(pcl_clock_init(&n.clock, &clock)))
            return false;
        pcl_arp_config_t arp { };
        arp.mode = PCL_ARP_UP;
        arp.octaves = 1;
        arp.division = 6;
        arp.gate = 3;
        if (!ok(pcl_arp_init(&n.arp, &arp)))
            return false;
        for (size_t i = 0; i < PARAM_COUNT; ++i)
            if (!kParams[i].apply(s, p[i]))
                return false;
        n.delay_samples = n.delay_time_ms * 0.001f * (float)n.sr;
        pcl_clock_start(&n.clock);
        return true;
    }

    void fm_reset(void *s)
    {
        auto &n = node(s);
        for (Voice &v : n.voice)
            v = Voice { };
        std::fill_n(n.chorus_line, n.chorus_len, 0.f);
        std::fill_n(n.delay_line[0], 2 * (size_t)n.delay_len, 0.f);
        n.delay_tone[0] = n.delay_tone[1] = 0;
        n.delay_samples = n.delay_time_ms * 0.001f * (float)n.sr;
        pcl_arp_reset(&n.arp);
        pcl_clock_reset(&n.clock);
        pcl_clock_start(&n.clock);
        n.arp_was_on = n.arp_on;
    }

    void fm_destroy(void *s) { std::free(node(s).memory); }

    void render(FmSynthNode &n, NodeIo &io, size_t from, size_t to)
    {
        const Algorithm &alg = kAlgorithms[std::clamp(n.algorithm, 0, (int)std::size(kAlgorithms) - 1)];
        const float      sr = (float)n.sr, inv_sr = 1.f / sr;
        int              carriers = 0;
        for (size_t i = 0; i < kOps; ++i)
            carriers += (alg.carriers >> i) & 1;
        const float carrier_gain = 0.55f / std::sqrt((float)carriers);
        const float chorus_rate = 0.6f * inv_sr, delay_target = n.delay_time_ms * 0.001f * sr;
        const float delay_glide = 1.f - std::exp(-1.f / (0.05f * sr)); // a 50 ms glide
        const float tone_k = 1.f - std::exp(-kTwoPi * 3500.f * inv_sr);

        for (size_t s = from; s < to; ++s)
        {
            const float vibrato
                = std::exp2(n.vibrato / 12.f * std::sin(kTwoPi * n.vibrato_phase));
            n.vibrato_phase += n.vibrato_rate * inv_sr;
            n.vibrato_phase -= std::floor(n.vibrato_phase);
            const float bend = io.sig_in[0] ? std::exp2(sample_to_float(io.sig_in[0][s])) : 1.f;
            const float bright = std::max(0.f,
                n.brightness + (io.sig_in[1] ? sample_to_float(io.sig_in[1][s]) : 0.f));

            float mix = 0;
            for (Voice &v : n.voice)
            {
                if (!v.active)
                    continue;
                const float hz = 440.f * std::exp2(((float)v.note - 69.f) / 12.f) * vibrato * bend;
                const float touch = 1.f - n.velocity_amount * (1.f - v.velocity);
                // Less modulation high up, where it would only add harshness and aliasing.
                const float track = hz <= 400.f ? 1.f : std::sqrt(400.f / hz);
                float       out[kOps] = { };
                float       heard = 0;
                bool        sounding = false;
                for (size_t k = kOps; k-- > 0;)
                {
                    const Operator &o = n.op[k];
                    const float     env = envelope(v, k, o);
                    const float     step = hz * o.ratio * inv_sr;
                    float           pm = 0;
                    for (size_t j = 0; j < kOps; ++j)
                        if ((alg.into[k] >> j) & 1)
                            pm += out[j];
                    if (k == kOps - 1)
                        pm += n.feedback * 1.6f * (v.fb[0] + v.fb[1]) * 0.5f;
                    const float wave = step < 0.45f ? std::sin(kTwoPi * v.phase[k] + pm) : 0.f;
                    v.phase[k] += std::min(step, 0.49f);
                    v.phase[k] -= std::floor(v.phase[k]);
                    const float value = wave * env;
                    if (k == kOps - 1)
                    {
                        v.fb[1] = v.fb[0];
                        v.fb[0] = value;
                    }
                    // as a modulator: up to about 7 radians of phase at full level
                    out[k] = value * 7.f * o.level * o.level * bright * touch * track;
                    if ((alg.carriers >> k) & 1)
                    {
                        heard += value * o.level;
                        sounding = sounding || v.stage[k] != IDLE;
                    }
                }
                mix += heard * carrier_gain * (0.35f + 0.65f * touch);
                if (!sounding)
                    v.active = v.held = false;
            }

            // chorus: two taps 7 to 15 ms back, moving in quadrature, one to each side
            n.chorus_line[n.chorus_w] = mix;
            const float sweep = kTwoPi * n.chorus_phase;
            const float tap_l
                = read_line(n.chorus_line, n.chorus_len, n.chorus_w, (0.011f + 0.004f * std::sin(sweep)) * sr);
            const float tap_r
                = read_line(n.chorus_line, n.chorus_len, n.chorus_w, (0.011f + 0.004f * std::cos(sweep)) * sr);
            n.chorus_w = (n.chorus_w + 1) % n.chorus_len;
            n.chorus_phase += chorus_rate;
            n.chorus_phase -= std::floor(n.chorus_phase);
            const float wet = n.chorus_mix, dry = 1.f - 0.4f * wet;
            float       l = mix * dry + tap_l * wet * 0.7f, r = mix * dry + tap_r * wet * 0.7f;

            // delay: the left line feeds the right and the right the left, darker each time
            n.delay_samples += (delay_target - n.delay_samples) * delay_glide;
            const float echo_l = read_line(n.delay_line[0], n.delay_len, n.delay_w, n.delay_samples);
            const float echo_r = read_line(n.delay_line[1], n.delay_len, n.delay_w, n.delay_samples);
            n.delay_tone[0] += (echo_l - n.delay_tone[0]) * tone_k;
            n.delay_tone[1] += (echo_r - n.delay_tone[1]) * tone_k;
            n.delay_line[0][n.delay_w] = 0.5f * (l + r) + n.delay_feedback * n.delay_tone[1];
            n.delay_line[1][n.delay_w] = n.delay_feedback * n.delay_tone[0];
            n.delay_w = (n.delay_w + 1) % n.delay_len;
            l += n.delay_mix * echo_l;
            r += n.delay_mix * echo_r;

            // a gentle limit, so a full chord does not clip hard
            auto limit = [](float x) { return x / (1.f + std::abs(x) * 0.25f); };
            if (io.sig_out[0])
                io.sig_out[0][s] = to_sample(std::clamp(limit(l * n.volume), -1.f, 1.f));
            if (io.sig_out[1])
                io.sig_out[1][s] = to_sample(std::clamp(limit(r * n.volume), -1.f, 1.f));
        }
    }

    void fm_process(void *s, NodeIo &io)
    {
        auto &n = node(s);
        // The arpeggiator always hears the keys, so its held set is right when it is switched
        // on; its clock pulses reach it only while it is on.
        pcl_event_t internal[PCL_MAX_BLOCK + 2];
        const size_t internal_count = pcl_clock_process(&n.clock, io.frames, internal, std::size(internal));
        const EventBuf &keys = *io.evt_in[0];
        const EventBuf &ext = *io.evt_in[1];
        if (n.arp_on != n.arp_was_on)
        {
            const pcl_event_t stop { 0, PCL_EVT_STOP, 0, 0 };
            pcl_event_t       ignored[PCL_ARP_MAX_NOTES * 2 + 2];
            pcl_arp_process(&n.arp, &stop, 1, ignored, std::size(ignored));
            release_all(n);
            n.arp_was_on = n.arp_on;
        }

        // the keys and the clock, in time order (a key before a pulse at the same sample)
        constexpr size_t kCap = 4 * PCL_MAX_BLOCK + 64;
        pcl_event_t      merged[kCap];
        size_t           count = 0;
        const pcl_event_t *pulses = n.external ? ext.data : internal;
        const size_t       pulse_count = !n.arp_on ? 0 : n.external ? ext.count : internal_count;
        for (size_t a = 0, b = 0; (a < keys.count || b < pulse_count) && count < kCap;)
        {
            const bool key = a < keys.count
                && (b >= pulse_count || keys.data[a].offset <= pulses[b].offset);
            const pcl_event_t &e = key ? keys.data[a++] : pulses[b++];
            if (key && e.type != PCL_EVT_NOTE_ON && e.type != PCL_EVT_NOTE_OFF)
                continue;
            merged[count++] = e;
        }
        pcl_event_t        arp_out[kCap];
        const pcl_event_t *events = keys.data;
        size_t             event_count = keys.count;
        const size_t       arp_count = pcl_arp_process(&n.arp, merged, count, arp_out, kCap);
        if (n.arp_on)
        {
            events = arp_out;
            event_count = arp_count;
        }

        size_t pos = 0, i = 0;
        while (pos < io.frames)
        {
            for (; i < event_count && events[i].offset <= pos; ++i)
                handle(n, events[i]);
            const size_t end = i < event_count ? std::min<size_t>(io.frames, events[i].offset) : io.frames;
            if (end > pos)
                render(n, io, pos, end);
            pos = end;
        }
    }
} // namespace

const ModuleDesc kFmSynthDesc = { "fm_synth", "FM Synth", "Voice", 46, sizeof(FmSynthNode),
    alignof(FmSynthNode), kPorts, kParams, false, fm_init, fm_reset, fm_process, nullptr, nullptr,
    "Four-operator FM, eight voices, with presets, chorus, delay and an arpeggiator", fm_destroy,
    kPresets };

} // namespace rack
