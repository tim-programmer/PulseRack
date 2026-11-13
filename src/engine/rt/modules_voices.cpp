// The voice modules (worklist, "New modules"): each a complete voice from one stream of notes, with
// its own envelope and amplifier, so it sounds with nothing else patched; each a module of its own
// with only the controls its sound needs. Their sound is made here in the rack, from PulseCoreLib's
// parts where the library has one (oscillators, the unison stack, filters, noise), and in the
// rack's own float code where it has none (audio-rate phase modulation, a plucked string's loop,
// grains).
//
// Every voice is monophonic with the last note held taking priority: a note played while another
// is held glides there without starting the envelope again; a new note after all were let go
// starts it again, on its own sample. Velocity sets the level. PITCH is in octaves (1 per unit).
#include "engine/model/wav.h"
#include "engine/rt/modules_common.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rack
{
namespace
{

    constexpr std::string_view kVoice = "Voice";
    constexpr float            kSilent = 1e-6f, kTwoPi = 6.28318530718f;

    // ============================================================================================================
    // The core every voice shares: the notes held, the envelope (a linear attack to full, held
    // while a note is, then an exponential release) and the velocity.
    struct VoiceCore
    {
        uint8_t  held[16];
        uint8_t  nheld;
        uint8_t  note;     // the note sounding (the last held, or the last let go)
        float    velocity; // 0 .. 1
        uint8_t  stage;    // 0 idle, 1 attack, 2 held, 3 release
        float    env, attack_step, release_k;
        uint32_t sr;
    };
    enum : int
    {
        VC_NONE,
        VC_GLIDE,  // the note changed, the envelope goes on
        VC_STRIKE, // a new note: the envelope starts again
    };
    float fall_k(float ms, uint32_t sr)
    {
        return (float)std::exp(std::log(0.001) / (std::max(ms, 0.1f) * 0.001 * (double)sr));
    }
    void vc_times(VoiceCore &v, float attack_ms, float release_ms)
    {
        v.attack_step = 1.f / std::max(1.f, attack_ms * 0.001f * (float)v.sr);
        v.release_k = fall_k(release_ms, v.sr);
    }
    void vc_init(VoiceCore &v, uint32_t sr, float attack_ms, float release_ms)
    {
        v.sr = sr;
        v.note = 60;
        vc_times(v, attack_ms, release_ms);
    }
    void vc_reset(VoiceCore &v)
    {
        v.nheld = 0;
        v.stage = 0;
        v.env = 0;
    }
    int vc_event(VoiceCore &v, const pcl_event_t &e)
    {
        const bool on = e.type == PCL_EVT_NOTE_ON && e.velocity > 0;
        const bool off = e.type == PCL_EVT_NOTE_OFF || (e.type == PCL_EVT_NOTE_ON && e.velocity == 0);
        if (e.type == PCL_EVT_STOP)
        {
            v.nheld = 0;
            if (v.stage)
                v.stage = 3;
            return VC_NONE;
        }
        if (on)
        {
            const bool legato = v.nheld > 0;
            uint8_t    k = 0;
            for (uint8_t i = 0; i < v.nheld; i++) // (again: to the top)
                if (v.held[i] != e.note)
                    v.held[k++] = v.held[i];
            v.nheld = k;
            if (v.nheld == std::size(v.held))
                std::memmove(v.held, v.held + 1, --v.nheld);
            v.held[v.nheld++] = e.note;
            v.note = e.note;
            if (legato && v.stage == 2)
                return VC_GLIDE;
            v.velocity = (float)e.velocity / 127.f;
            v.stage = 1;
            return VC_STRIKE;
        }
        if (off)
        {
            uint8_t k = 0;
            for (uint8_t i = 0; i < v.nheld; i++)
                if (v.held[i] != e.note)
                    v.held[k++] = v.held[i];
            const bool was_top = v.nheld > 0 && v.held[v.nheld - 1] == e.note;
            v.nheld = k;
            if (v.nheld && was_top)
            {
                v.note = v.held[v.nheld - 1];
                return VC_GLIDE;
            }
            if (!v.nheld && v.stage)
                v.stage = 3;
        }
        return VC_NONE;
    }
    // The envelope's next value.
    float vc_next(VoiceCore &v)
    {
        switch (v.stage)
        {
        case 1:
            v.env += v.attack_step;
            if (v.env >= 1)
            {
                v.env = 1;
                v.stage = 2;
            }
            break;
        case 3:
            v.env *= v.release_k;
            if (v.env < kSilent)
            {
                v.env = 0;
                v.stage = 0;
            }
            break;
        default:
            break;
        }
        return v.env;
    }
    float vc_level(const VoiceCore &v) { return 0.35f + 0.65f * v.velocity; }
    float note_hz(uint8_t note) { return 440.f * std::exp2(((float)note - 69.f) / 12.f); }

    // Runs a block, cut at each note: `seg(from, to)` renders a stretch, `on(kind)` hears a change.
    template <class Seg, class On>
    void voice_block(VoiceCore &v, const EventBuf &ev, size_t frames, Seg seg, On on)
    {
        size_t pos = 0, i = 0;
        while (pos < frames)
        {
            for (; i < ev.count && ev.data[i].offset <= pos; i++)
                if (const int r = vc_event(v, ev.data[i]))
                    on(r);
            const size_t end = i < ev.count ? std::min<size_t>(frames, ev.data[i].offset) : frames;
            if (end > pos)
                seg(pos, end);
            pos = end;
        }
    }
    float pitch_at(const pcl_sample_t *pitch, size_t i)
    {
        return pitch ? sample_to_float(pitch[i]) : 0.f;
    }

    constexpr float kModOct = 4; // the library parts' pitch and cutoff inputs: 4 octaves a unit
    template <class T, float T::*A, float T::*R> bool set_times(void *s, float v, bool attack)
    {
        auto *n = as<T>(s);
        (attack ? n->*A : n->*R) = v;
        vc_times(n->vc, n->*A, n->*R);
        return true;
    }

    // ============================================================================================================
    // Sync voice: a master oscillator at the note (never heard) hard-syncs a slave at Ratio times
    // the note; the slave's pitch sweeps down from Sweep octaves above over Sweep time at each new
    // note: the classic tearing sync sound. The slave is a saw or a pulse (Pulse width; PWM in).
    enum SyncParam
    {
        SY_WAVE,
        SY_RATIO,
        SY_SWEEP,
        SY_SWEEP_TIME,
        SY_PW,
        SY_ATTACK,
        SY_RELEASE,
        SY_COUNT
    };
    constexpr std::string_view kSyncWaves[] = { "saw", "pulse" };
    struct SyncNode
    {
        VoiceCore    vc;
        pcl_osc_t    master, slave;
        float        ratio, sweep, sweep_ms, attack, release;
        float        sweep_env, sweep_k;
        pcl_sample_t pm_m[PCL_MAX_BLOCK], pm_s[PCL_MAX_BLOCK], sync[PCL_MAX_BLOCK],
            wave[PCL_MAX_BLOCK], dummy[PCL_MAX_BLOCK];
    };
    void sync_tune(SyncNode *n)
    {
        const float f = note_hz(n->vc.note);
        (void)pcl_osc_set_frequency(&n->master, to_hz(clamp_hz(f, n->vc.sr)));
        (void)pcl_osc_set_frequency(&n->slave, to_hz(clamp_hz(f * n->ratio, n->vc.sr)));
    }
    bool sync_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<SyncNode>(s);
        n->ratio = p[SY_RATIO];
        n->sweep = p[SY_SWEEP];
        n->sweep_ms = p[SY_SWEEP_TIME];
        n->attack = p[SY_ATTACK];
        n->release = p[SY_RELEASE];
        vc_init(n->vc, ctx.sample_rate, n->attack, n->release);
        pcl_osc_config_t m { };
        m.sample_rate_hz = ctx.sample_rate;
        m.waveform = PCL_OSC_SAW;
        m.frequency = to_hz(note_hz(60));
        m.pitch_depth_oct = to_oct(kModOct);
        m.pulse_width = to_coef(0.5);
        pcl_osc_config_t sl = m;
        sl.waveform = to_u32(p[SY_WAVE]) ? PCL_OSC_PULSE : PCL_OSC_SAW;
        sl.frequency = to_hz(clamp_hz(note_hz(60) * n->ratio, ctx.sample_rate));
        sl.pulse_width = to_coef(p[SY_PW]);
        sl.pwm_depth = to_coef(0.45);
        return ok(pcl_osc_init(&n->master, &m)) && ok(pcl_osc_init(&n->slave, &sl));
    }
    void sync_reset(void *s)
    {
        auto *n = as<SyncNode>(s);
        vc_reset(n->vc);
        pcl_osc_reset(&n->master);
        pcl_osc_reset(&n->slave);
        n->sweep_env = 0;
    }
    void sync_process(void *s, NodeIo &io)
    {
        auto               *n = as<SyncNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *pitch = io.sig_in[0];
        const pcl_sample_t *pwm = io.sig_in[1];
        voice_block(
            n->vc, *io.evt_in[0], io.frames,
            [&](size_t from, size_t to)
            {
                const size_t len = to - from;
                for (size_t i = 0; i < len; i++)
                {
                    const float pi = pitch_at(pitch, from + i);
                    n->pm_m[i] = to_sample(std::clamp(pi / kModOct, -1.f, 1.f));
                    n->pm_s[i] = to_sample(
                        std::clamp((pi + n->sweep * n->sweep_env) / kModOct, -1.f, 1.f));
                    n->sweep_env = n->sweep_env < kSilent ? 0 : n->sweep_env * n->sweep_k;
                }
                pcl_osc_process(&n->master, n->dummy, n->pm_m, nullptr, nullptr, n->sync, len);
                pcl_osc_process(&n->slave, n->wave, n->pm_s, pwm ? pwm + from : nullptr, n->sync,
                    nullptr, len);
                const float lvl = 0.8f * vc_level(n->vc);
                for (size_t i = 0; i < len; i++)
                    out[from + i] = to_sample(lvl * vc_next(n->vc) * sample_to_float(n->wave[i]));
            },
            [&](int kind)
            {
                sync_tune(n);
                if (kind == VC_STRIKE)
                {
                    n->sweep_env = 1;
                    n->sweep_k = (float)std::exp(-1.0 / (std::max(n->sweep_ms, 1.f) * 0.001 * n->vc.sr));
                }
            });
    }
    const PortDesc kSyncPorts[] = { { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false }, { "pwm", "PWM", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kSyncParams[] = {
        param(
            "wave", "Wave", "", 0, 1, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(pcl_osc_set_waveform(
                    &as<SyncNode>(s)->slave, to_u32(v) ? PCL_OSC_PULSE : PCL_OSC_SAW));
            },
            kSyncWaves),
        param("ratio", "Ratio", "x", 1, 8, 2.5f, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<SyncNode>(s);
                n->ratio = v;
                sync_tune(n);
                return true;
            }),
        param("sweep", "Sweep", "oct", 0, 4, 1.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<SyncNode>(s)->sweep = v;
                return true;
            }),
        param("sweep_time", "Sweep time", "ms", 10, 3000, 300, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                as<SyncNode>(s)->sweep_ms = v; // (from the next note)
                return true;
            }),
        param("pulse_width", "Pulse width", "", 0.05f, 0.95f, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_osc_set_pulse_width(&as<SyncNode>(s)->slave, to_coef(v))); }),
        param("attack", "Attack", "ms", 1, 2000, 5, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<SyncNode, &SyncNode::attack, &SyncNode::release>(s, v, true); }),
        param("release", "Release", "ms", 5, 4000, 300, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<SyncNode, &SyncNode::attack, &SyncNode::release>(s, v, false); }),
    };
    static_assert(std::size(kSyncParams) == SY_COUNT);

    // ============================================================================================================
    // Stack voice: the unison stack (pcl_unison: up to 8 detuned saws, spread in stereo) through a
    // ladder filter per side (pcl_ladder, driven) whose cutoff the envelope opens by Contour
    // octaves. L alone is the mono mix through one filter.
    enum StackParam
    {
        ST_VOICES,
        ST_DETUNE,
        ST_SPREAD,
        ST_CUTOFF,
        ST_RES,
        ST_DRIVE,
        ST_CONTOUR,
        ST_ATTACK,
        ST_RELEASE,
        ST_COUNT
    };
    struct StackNode
    {
        VoiceCore    vc;
        pcl_unison_t u;
        pcl_ladder_t f[2];
        float        contour, attack, release;
        pcl_sample_t pm[PCL_MAX_BLOCK], cm[PCL_MAX_BLOCK], side[2][PCL_MAX_BLOCK];
        float        env[PCL_MAX_BLOCK];
    };
    bool stack_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<StackNode>(s);
        n->contour = p[ST_CONTOUR];
        n->attack = p[ST_ATTACK];
        n->release = p[ST_RELEASE];
        vc_init(n->vc, ctx.sample_rate, n->attack, n->release);
        pcl_unison_config_t u { };
        u.osc.sample_rate_hz = ctx.sample_rate;
        u.osc.waveform = PCL_OSC_SAW;
        u.osc.frequency = to_hz(note_hz(60));
        u.osc.pitch_depth_oct = to_oct(kModOct);
        u.osc.pulse_width = to_coef(0.5);
        u.voices = to_u32(p[ST_VOICES]);
        u.detune_oct = to_oct(p[ST_DETUNE] / 1200.0);
        u.stereo_width = to_coef(p[ST_SPREAD]);
        pcl_ladder_config_t l { };
        l.sample_rate_hz = ctx.sample_rate;
        l.cutoff_hz = to_hz(clamp_hz(p[ST_CUTOFF], ctx.sample_rate));
        l.resonance = to_coef(p[ST_RES]);
        l.mod_depth_oct = to_oct(kModOct);
        l.mode = PCL_LADDER_LP24;
        l.saturate = 1;
        l.drive = PCL_WIDE(p[ST_DRIVE]);
        return ok(pcl_unison_init(&n->u, &u)) && ok(pcl_ladder_init(&n->f[0], &l))
            && ok(pcl_ladder_init(&n->f[1], &l));
    }
    void stack_reset(void *s)
    {
        auto *n = as<StackNode>(s);
        vc_reset(n->vc);
        pcl_unison_reset(&n->u);
        pcl_ladder_reset(&n->f[0]);
        pcl_ladder_reset(&n->f[1]);
    }
    void stack_process(void *s, NodeIo &io)
    {
        auto               *n = as<StackNode>(s);
        pcl_sample_t       *outs[2] = { io.sig_out[0] ? io.sig_out[0] : io.scratch, io.sig_out[1] };
        const pcl_sample_t *pitch = io.sig_in[0];
        const pcl_sample_t *cut = io.sig_in[1];
        voice_block(
            n->vc, *io.evt_in[0], io.frames,
            [&](size_t from, size_t to)
            {
                const size_t len = to - from;
                for (size_t i = 0; i < len; i++)
                {
                    n->env[i] = vc_next(n->vc);
                    n->pm[i] = to_sample(std::clamp(pitch_at(pitch, from + i) / kModOct, -1.f, 1.f));
                    n->cm[i] = to_sample(std::clamp(
                        (n->contour * n->env[i] + pitch_at(cut, from + i)) / kModOct, -1.f, 1.f));
                }
                const int sides = outs[1] ? 2 : 1;
                pcl_unison_process(&n->u, n->side[0], sides == 2 ? n->side[1] : nullptr, n->pm,
                    nullptr, nullptr, len);
                const float lvl = 0.7f * vc_level(n->vc);
                for (int c = 0; c < sides; c++)
                {
                    pcl_ladder_process(&n->f[c], n->side[c], n->side[c], n->cm, len);
                    for (size_t i = 0; i < len; i++)
                        outs[c][from + i] = to_sample(
                            std::clamp(lvl * n->env[i] * sample_to_float(n->side[c][i]), -1.f, 1.f));
                }
            },
            [&](int)
            {
                (void)pcl_unison_set_frequency(
                    &n->u, to_hz(clamp_hz(note_hz(n->vc.note), n->vc.sr)));
            });
    }
    const PortDesc kStackPorts[] = {
        { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false },
        { "cutoff", "CUTOFF", Kind::Signal, false },
        { "l", "L", Kind::Signal, true },
        { "r", "R", Kind::Signal, true },
    };
    const ParamDesc kStackParams[] = {
        param("voices", "Voices", "", 1, PCL_UNISON_MAX_VOICES, 6, Scale::Int, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_voices(&as<StackNode>(s)->u, to_u32(v))); }),
        param("detune", "Detune", "cents", 0, 100, 25, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_detune(&as<StackNode>(s)->u, to_oct(v / 1200.0))); }),
        param("spread", "Spread", "", 0, 1, 0.8f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_stereo_width(&as<StackNode>(s)->u, to_coef(v))); }),
        param("cutoff", "Cutoff", "Hz", 50, 12000, 1200, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<StackNode>(s);
                const pcl_hz_t hz = to_hz(clamp_hz(v, n->vc.sr));
                return ok(pcl_ladder_set_cutoff(&n->f[0], hz))
                    && ok(pcl_ladder_set_cutoff(&n->f[1], hz));
            }),
        param("resonance", "Resonance", "", 0, 1.05f, 0.25f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<StackNode>(s);
                return ok(pcl_ladder_set_resonance(&n->f[0], to_coef(v)))
                    && ok(pcl_ladder_set_resonance(&n->f[1], to_coef(v)));
            }),
        param("drive", "Drive", "", 1, 7.9f, 1.5f, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<StackNode>(s);
                return ok(pcl_ladder_set_drive(&n->f[0], PCL_WIDE(v)))
                    && ok(pcl_ladder_set_drive(&n->f[1], PCL_WIDE(v)));
            }),
        param("contour", "Contour", "oct", 0, 4, 1.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<StackNode>(s)->contour = v;
                return true;
            }),
        param("attack", "Attack", "ms", 1, 4000, 10, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<StackNode, &StackNode::attack, &StackNode::release>(s, v, true); }),
        param("release", "Release", "ms", 5, 6000, 500, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<StackNode, &StackNode::attack, &StackNode::release>(s, v, false); }),
    };
    static_assert(std::size(kStackParams) == ST_COUNT);

    // ============================================================================================================
    // FM voice: two sine operators, the modulator at Ratio times the note bending the carrier's
    // phase by Index radians (more at each new note: Env amount, falling over Env decay), with
    // Feedback on the modulator. Phase modulation at audio rate is not in the library (its
    // oscillators take their pitch at control rate), so the operators are the rack's own, in float.
    enum FmParam
    {
        FM_RATIO,
        FM_INDEX,
        FM_ENV,
        FM_ENV_DECAY,
        FM_FEEDBACK,
        FM_ATTACK,
        FM_RELEASE,
        FM_COUNT
    };
    constexpr std::string_view kFmRatios[] = { "x0.5", "x1", "x1.5", "x2", "x3", "x4", "x5", "x6",
        "x7", "x8", "x9", "x10", "x11", "x12" };
    constexpr float kFmRatioValue[] = { 0.5f, 1, 1.5f, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    struct FmNode
    {
        VoiceCore vc;
        float     ratio, index, env_amount, env_decay, feedback, attack, release;
        double    pc, pm; // the operators' phases, 0 .. 1
        float     prev, ienv, ienv_k;
    };
    bool fm_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<FmNode>(s);
        n->ratio = kFmRatioValue[std::min<size_t>(to_u32(p[FM_RATIO]), std::size(kFmRatioValue) - 1)];
        n->index = p[FM_INDEX];
        n->env_amount = p[FM_ENV];
        n->env_decay = p[FM_ENV_DECAY];
        n->feedback = p[FM_FEEDBACK];
        n->attack = p[FM_ATTACK];
        n->release = p[FM_RELEASE];
        vc_init(n->vc, ctx.sample_rate, n->attack, n->release);
        return true;
    }
    void fm_reset(void *s)
    {
        auto *n = as<FmNode>(s);
        vc_reset(n->vc);
        n->pc = n->pm = 0;
        n->prev = n->ienv = 0;
    }
    void fm_process(void *s, NodeIo &io)
    {
        auto               *n = as<FmNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *pitch = io.sig_in[0];
        const pcl_sample_t *index_in = io.sig_in[1];
        const double        inv_sr = 1.0 / (double)n->vc.sr;
        voice_block(
            n->vc, *io.evt_in[0], io.frames,
            [&](size_t from, size_t to)
            {
                const float base = note_hz(n->vc.note), lvl = 0.8f * vc_level(n->vc);
                for (size_t i = from; i < to; i++)
                {
                    const float f = std::min(base * std::exp2(pitch_at(pitch, i)),
                        0.45f * (float)n->vc.sr);
                    const float idx = std::max(0.f,
                        n->index + n->env_amount * n->ienv + 5 * pitch_at(index_in, i));
                    const float mod
                        = std::sin(kTwoPi * (float)n->pm + 1.5f * n->feedback * n->prev);
                    n->prev = mod;
                    const float car = std::sin(kTwoPi * (float)n->pc + idx * mod);
                    out[i] = to_sample(lvl * vc_next(n->vc) * car);
                    n->pc += f * inv_sr;
                    n->pm += f * n->ratio * inv_sr;
                    n->pc -= std::floor(n->pc);
                    n->pm -= std::floor(n->pm);
                    n->ienv = n->ienv < kSilent ? 0 : n->ienv * n->ienv_k;
                }
            },
            [&](int kind)
            {
                if (kind != VC_STRIKE)
                    return;
                if (n->vc.env < kSilent)
                { // from silence: both phases from 0, every note alike
                    n->pc = n->pm = 0;
                    n->prev = 0;
                }
                n->ienv = 1;
                n->ienv_k = fall_k(n->env_decay, n->vc.sr);
            });
    }
    const PortDesc kFmPorts[] = { { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false }, { "index", "INDEX", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true } };
    template <float FmNode::*F> bool fm_set(void *s, float v)
    {
        as<FmNode>(s)->*F = v;
        return true;
    }
    const ParamDesc kFmParams[] = {
        param(
            "ratio", "Ratio", "", 0, 13, 3, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                as<FmNode>(s)->ratio
                    = kFmRatioValue[std::min<size_t>(to_u32(v), std::size(kFmRatioValue) - 1)];
                return true;
            },
            kFmRatios),
        param("index", "Index", "", 0, 10, 1.5f, Scale::Linear, Control::Knob, fm_set<&FmNode::index>),
        param("env_amount", "Env amount", "", 0, 10, 3, Scale::Linear, Control::Knob,
            fm_set<&FmNode::env_amount>),
        param("env_decay", "Env decay", "ms", 10, 4000, 350, Scale::Log, Control::Knob,
            fm_set<&FmNode::env_decay>),
        param("feedback", "Feedback", "", 0, 1, 0, Scale::Linear, Control::Knob,
            fm_set<&FmNode::feedback>),
        param("attack", "Attack", "ms", 1, 2000, 3, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<FmNode, &FmNode::attack, &FmNode::release>(s, v, true); }),
        param("release", "Release", "ms", 5, 4000, 400, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<FmNode, &FmNode::attack, &FmNode::release>(s, v, false); }),
    };
    static_assert(std::size(kFmParams) == FM_COUNT);

    // ============================================================================================================
    // Air voice: noise (pcl_noise, of a Colour) through a resonant band-pass (pcl_svf) tuned to the
    // note: high Resonance whistles, low is wind. Breath adds the noise above the note (the
    // filter's high-pass) for the air around a blown note.
    enum AirParam
    {
        AIR_COLOUR,
        AIR_RES,
        AIR_BREATH,
        AIR_ATTACK,
        AIR_RELEASE,
        AIR_COUNT
    };
    constexpr std::string_view kAirColours[] = { "white", "pink", "brown" };
    struct AirNode
    {
        VoiceCore    vc;
        pcl_noise_t  noise;
        pcl_svf_t    f;
        float        res, breath, attack, release;
        pcl_sample_t n[PCL_MAX_BLOCK], bp[PCL_MAX_BLOCK], hp[PCL_MAX_BLOCK], pm[PCL_MAX_BLOCK];
    };
    // The band's level falls as it narrows: made up for, so the knob changes the sound more than
    // the volume.
    float air_gain(float res) { return 1.f + 9.f * res * res; }
    bool  air_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<AirNode>(s);
        n->res = p[AIR_RES];
        n->breath = p[AIR_BREATH];
        n->attack = p[AIR_ATTACK];
        n->release = p[AIR_RELEASE];
        vc_init(n->vc, ctx.sample_rate, n->attack, n->release);
        pcl_noise_config_t nc { };
        nc.sample_rate_hz = ctx.sample_rate;
        nc.colour = (pcl_noise_colour_t)to_u32(p[AIR_COLOUR]);
        nc.seed = 0;
        pcl_svf_config_t f { };
        f.sample_rate_hz = ctx.sample_rate;
        f.cutoff_hz = to_hz(note_hz(60));
        f.resonance = to_coef(n->res);
        f.mod_depth_oct = to_oct(kModOct);
        return ok(pcl_noise_init(&n->noise, &nc)) && ok(pcl_svf_init(&n->f, &f));
    }
    void air_reset(void *s)
    {
        auto *n = as<AirNode>(s);
        vc_reset(n->vc);
        pcl_noise_reset(&n->noise);
        pcl_svf_reset(&n->f);
    }
    void air_process(void *s, NodeIo &io)
    {
        auto               *n = as<AirNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *pitch = io.sig_in[0];
        voice_block(
            n->vc, *io.evt_in[0], io.frames,
            [&](size_t from, size_t to)
            {
                const size_t len = to - from;
                for (size_t i = 0; i < len; i++)
                    n->pm[i] = to_sample(std::clamp(pitch_at(pitch, from + i) / kModOct, -1.f, 1.f));
                pcl_noise_process(&n->noise, n->n, len);
                pcl_svf_process(&n->f, n->n, nullptr, n->bp, n->hp, n->pm, len);
                const float lvl = 0.7f * vc_level(n->vc), g = air_gain(n->res);
                for (size_t i = 0; i < len; i++)
                {
                    const float x = g * sample_to_float(n->bp[i]) + 0.4f * n->breath * sample_to_float(n->hp[i]);
                    out[from + i] = to_sample(std::clamp(lvl * vc_next(n->vc) * x, -1.f, 1.f));
                }
            },
            [&](int)
            { (void)pcl_svf_set_cutoff(&n->f, to_hz(clamp_hz(note_hz(n->vc.note), n->vc.sr))); });
    }
    const PortDesc kAirPorts[] = { { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kAirParams[] = {
        param(
            "colour", "Colour", "", 0, 2, 1, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(pcl_noise_set_colour(
                    &as<AirNode>(s)->noise, (pcl_noise_colour_t)to_u32(v)));
            },
            kAirColours),
        param("resonance", "Resonance", "", 0, 1, 0.85f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<AirNode>(s);
                n->res = v;
                return ok(pcl_svf_set_resonance(&n->f, to_coef(v)));
            }),
        param("breath", "Breath", "", 0, 1, 0.2f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<AirNode>(s)->breath = v;
                return true;
            }),
        param("attack", "Attack", "ms", 1, 4000, 120, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<AirNode, &AirNode::attack, &AirNode::release>(s, v, true); }),
        param("release", "Release", "ms", 5, 6000, 600, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<AirNode, &AirNode::attack, &AirNode::release>(s, v, false); }),
    };
    static_assert(std::size(kAirParams) == AIR_COUNT);

    // ============================================================================================================
    // String voice: a plucked string (Karplus-Strong). Each new note fills a delay line one period
    // long with a burst of noise, softened by Brightness and combed at the Position along the string
    // where it was plucked; the line feeds back through a gentle low-pass (Damping) with the gain
    // that lets it ring for Decay seconds, or for Release once the key is let go. The loop's
    // fractional length (read with linear interpolation, the filter's half-sample counted) tunes it.
    enum StringParam
    {
        STR_DAMP,
        STR_BRIGHT,
        STR_POS,
        STR_DECAY,
        STR_RELEASE,
        STR_COUNT
    };
    constexpr size_t kLine = 8192; // the longest period, in samples
    struct StringNode
    {
        VoiceCore vc;
        float     damp, bright, pos, decay_s, release_ms;
        float    *line;  // kLine samples, then kLine for a pluck's burst (from init, freed by destroy)
        uint32_t  w;     // the next write
        float     prev;  // the loop filter's last input
        uint32_t  rng;
    };
    uint32_t xorshift(uint32_t &x)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }
    float string_period(const StringNode *n, float hz)
    {
        return std::clamp((float)n->vc.sr / std::max(hz, 1.f) - 0.5f * n->damp, 2.f, (float)kLine - 4);
    }
    bool string_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<StringNode>(s);
        n->damp = p[STR_DAMP];
        n->bright = p[STR_BRIGHT];
        n->pos = p[STR_POS];
        n->decay_s = p[STR_DECAY];
        n->release_ms = p[STR_RELEASE];
        n->rng = 0x9E3779B9u;
        vc_init(n->vc, ctx.sample_rate, 1, n->release_ms);
        n->line = static_cast<float *>(std::calloc(2 * kLine, sizeof(float)));
        return n->line != nullptr;
    }
    void string_destroy(void *s) { std::free(as<StringNode>(s)->line); }
    void string_reset(void *s)
    {
        auto *n = as<StringNode>(s);
        vc_reset(n->vc);
        std::memset(n->line, 0, kLine * sizeof(float));
        n->prev = 0;
    }
    // A pluck: the burst into the line, added to what still rings there.
    void string_pluck(StringNode *n, float period)
    {
        const size_t len = (size_t)std::ceil(period);
        float       *burst = n->line + kLine;
        const float  a = 0.03f + 0.97f * n->bright * n->bright; // the burst's low-pass
        float        y = 0, energy = 0;
        for (size_t i = 0; i < len; i++)
        {
            const float x = (float)(xorshift(n->rng) >> 8) / 8388608.f - 1.f;
            y += a * (x - y);
            burst[i] = y;
        }
        const size_t at = std::max<size_t>(1, (size_t)std::lround(n->pos * (float)len));
        for (size_t i = len; i-- > at;) // combed: plucked at `pos` along the string
            burst[i] -= burst[i - at];
        for (size_t i = 0; i < len; i++)
            energy += burst[i] * burst[i];
        const float g = 0.6f * vc_level(n->vc) / std::sqrt(std::max(energy / (float)len, 1e-9f));
        for (size_t i = 0; i < len; i++)
            n->line[(n->w + kLine - len + i) % kLine] += g * burst[i];
    }
    void string_process(void *s, NodeIo &io)
    {
        auto               *n = as<StringNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *pitch = io.sig_in[0];
        voice_block(
            n->vc, *io.evt_in[0], io.frames,
            [&](size_t from, size_t to)
            {
                const float base = note_hz(n->vc.note);
                const bool  held = n->vc.stage == 1 || n->vc.stage == 2;
                for (size_t i = from; i < to; i++)
                {
                    const float hz = base * std::exp2(pitch_at(pitch, i));
                    const float d = string_period(n, hz);
                    // the gain per trip round the loop that rings for the time left
                    const float t = held ? n->decay_s : n->release_ms * 0.001f;
                    const float g = std::pow(0.001f, 1.f / (std::max(hz, 1.f) * std::max(t, 0.005f)));
                    const float rd = (float)n->w + (float)kLine - d;
                    const size_t i0 = (size_t)rd;
                    const float  fr = rd - (float)i0;
                    const float  x = n->line[i0 % kLine] * (1 - fr) + n->line[(i0 + 1) % kLine] * fr;
                    const float  y = (1 - n->damp) * x + n->damp * 0.5f * (x + n->prev);
                    n->prev = x;
                    n->line[n->w] = g * y;
                    n->w = (n->w + 1) % kLine;
                    out[i] = to_sample(std::clamp(x, -1.f, 1.f));
                }
            },
            [&](int kind)
            {
                if (kind == VC_STRIKE)
                    string_pluck(n, string_period(n, note_hz(n->vc.note)));
            });
    }
    const PortDesc kStringPorts[] = { { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    template <float StringNode::*F> bool string_set(void *s, float v)
    {
        as<StringNode>(s)->*F = v;
        return true;
    }
    const ParamDesc kStringParams[] = {
        param("damping", "Damping", "", 0, 1, 0.5f, Scale::Linear, Control::Knob,
            string_set<&StringNode::damp>),
        param("brightness", "Brightness", "", 0, 1, 0.7f, Scale::Linear, Control::Knob,
            string_set<&StringNode::bright>),
        param("position", "Position", "", 0.05f, 0.5f, 0.2f, Scale::Linear, Control::Knob,
            string_set<&StringNode::pos>),
        param("decay", "Decay", "s", 0.1f, 20, 3, Scale::Log, Control::Knob,
            string_set<&StringNode::decay_s>),
        param("release", "Release", "ms", 10, 3000, 150, Scale::Log, Control::Knob,
            string_set<&StringNode::release_ms>),
    };
    static_assert(std::size(kStringParams) == STR_COUNT);

    // ============================================================================================================
    // Grain voice: short grains of a WAV file (the patch names it, as for the Sampler), each Size
    // long under a smooth window, started Density times a second around Position (scattered by
    // Spread), each at the note's pitch against the Root note. The grains are the rack's own: the
    // library's sampler is one playhead, and a cloud needs many.
    enum GrainParam
    {
        GR_SIZE,
        GR_DENSITY,
        GR_POS,
        GR_SPREAD,
        GR_ROOT,
        GR_ATTACK,
        GR_RELEASE,
        GR_COUNT
    };
    constexpr int kGrains = 48;
    struct Grain
    {
        double pos, step; // in the file's frames
        float  age, len;  // samples
        bool   on;
    };
    struct GrainNode
    {
        VoiceCore vc;
        float    *data; // the file, from grain_load (freed by destroy)
        uint32_t  frames, rate;
        float     size_ms, density, pos, spread, root, attack, release;
        Grain     g[kGrains];
        float     until; // samples to the next grain
        uint32_t  rng;
    };
    bool grain_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<GrainNode>(s);
        n->size_ms = p[GR_SIZE];
        n->density = p[GR_DENSITY];
        n->pos = p[GR_POS];
        n->spread = p[GR_SPREAD];
        n->root = p[GR_ROOT];
        n->attack = p[GR_ATTACK];
        n->release = p[GR_RELEASE];
        n->rng = 0x2545F491u;
        vc_init(n->vc, ctx.sample_rate, n->attack, n->release);
        return true;
    }
    void grain_reset(void *s)
    {
        auto *n = as<GrainNode>(s);
        vc_reset(n->vc);
        for (Grain &g : n->g)
            g.on = false;
        n->until = 0;
    }
    void grain_destroy(void *s) { std::free(as<GrainNode>(s)->data); }
    float grain_rand(GrainNode *n) { return (float)(xorshift(n->rng) >> 8) / 16777216.f; } // 0..1
    void  grain_process(void *s, NodeIo &io)
    {
        auto               *n = as<GrainNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *pitch = io.sig_in[0];
        const pcl_sample_t *pos_in = io.sig_in[1];
        voice_block(
            n->vc, *io.evt_in[0], io.frames,
            [&](size_t from, size_t to)
            {
                if (!n->data)
                {
                    for (size_t i = from; i < to; i++)
                        out[i] = 0;
                    return;
                }
                const float sr = (float)n->vc.sr, len = std::max(4.f, n->size_ms * 0.001f * sr);
                const float lvl = 0.8f * vc_level(n->vc)
                    / std::sqrt(std::max(1.f, n->density * n->size_ms * 0.001f));
                for (size_t i = from; i < to; i++)
                {
                    const float env = vc_next(n->vc);
                    if (env > 0 && (n->until -= 1) <= 0)
                    { // a new grain
                        n->until += sr / std::max(n->density, 0.1f);
                        for (Grain &g : n->g)
                            if (!g.on)
                            {
                                const float where = std::clamp(n->pos + pitch_at(pos_in, i)
                                        + n->spread * (grain_rand(n) - 0.5f), 0.f, 1.f);
                                g.on = true;
                                g.age = 0;
                                g.len = len;
                                g.pos = (double)where * (n->frames - 1);
                                g.step = std::exp2(((float)n->vc.note - n->root) / 12.f
                                             + pitch_at(pitch, i))
                                    * (float)n->rate / sr;
                                break;
                            }
                    }
                    float sum = 0;
                    for (Grain &g : n->g)
                    {
                        if (!g.on)
                            continue;
                        const double p = std::fmod(g.pos, (double)n->frames);
                        const size_t k = (size_t)p;
                        const float  fr = (float)(p - (double)k);
                        const float  x = n->data[k] * (1 - fr) + n->data[(k + 1) % n->frames] * fr;
                        sum += x * (0.5f - 0.5f * std::cos(kTwoPi * g.age / g.len));
                        g.pos += g.step;
                        if ((g.age += 1) >= g.len)
                            g.on = false;
                    }
                    out[i] = to_sample(std::clamp(lvl * env * sum, -1.f, 1.f));
                }
            },
            [&](int) { });
    }
    const PortDesc kGrainPorts[] = { { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false }, { "position", "POS", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true } };
    template <float GrainNode::*F> bool grain_set(void *s, float v)
    {
        as<GrainNode>(s)->*F = v;
        return true;
    }
    const ParamDesc kGrainParams[] = {
        param("size", "Size", "ms", 10, 500, 80, Scale::Log, Control::Knob, grain_set<&GrainNode::size_ms>),
        param("density", "Density", "/s", 1, 100, 20, Scale::Log, Control::Knob,
            grain_set<&GrainNode::density>),
        param("position", "Position", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            grain_set<&GrainNode::pos>),
        param("spread", "Spread", "", 0, 1, 0.1f, Scale::Linear, Control::Knob,
            grain_set<&GrainNode::spread>),
        param("root", "Root note", "note", 0, 127, 60, Scale::Int, Control::Knob,
            grain_set<&GrainNode::root>),
        param("attack", "Attack", "ms", 1, 4000, 50, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<GrainNode, &GrainNode::attack, &GrainNode::release>(s, v, true); }),
        param("release", "Release", "ms", 5, 6000, 800, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return set_times<GrainNode, &GrainNode::attack, &GrainNode::release>(s, v, false); }),
    };
    static_assert(std::size(kGrainParams) == GR_COUNT);

} // namespace

RACK_DESC(kSyncVoiceDesc, "sync_voice", "Sync voice", kVoice, 8, SyncNode, kSyncPorts,
    kSyncParams, false, sync_init, sync_reset, sync_process, nullptr, nullptr,
    "A hard-synced oscillator whose pitch sweeps down at each note: tearing leads and stabs");
RACK_DESC(kStackVoiceDesc, "stack_voice", "Stack voice", kVoice, 12, StackNode, kStackPorts,
    kStackParams, false, stack_init, stack_reset, stack_process, nullptr, nullptr,
    "Up to eight detuned saws in stereo through a driven ladder filter that opens with each note");
RACK_DESC(kFmVoiceDesc, "fm_voice", "FM voice", kVoice, 8, FmNode, kFmPorts, kFmParams, false,
    fm_init, fm_reset, fm_process, nullptr, nullptr,
    "Two sine operators: bells, electric pianos, basses and metallic plucks");
RACK_DESC(kAirVoiceDesc, "air_voice", "Air voice", kVoice, 8, AirNode, kAirPorts, kAirParams,
    false, air_init, air_reset, air_process, nullptr, nullptr,
    "Noise through a resonant filter tuned to the note: whistles, wind and breath");
RACK_DESC_D(kStringVoiceDesc, "string_voice", "String voice", kVoice, 8, StringNode, kStringPorts,
    kStringParams, string_init, string_reset, string_process, string_destroy,
    "A plucked string: a burst of noise ringing in a tuned loop; guitars, harps, kotos");
RACK_DESC_D(kGrainVoiceDesc, "grain_voice", "Grain voice", kVoice, 12, GrainNode, kGrainPorts,
    kGrainParams, grain_init, grain_reset, grain_process, grain_destroy,
    "Clouds of short grains of a WAV file, at the pitch of each note");

bool grain_load(void *state, const SampleData &d)
{
    auto *n = as<GrainNode>(state);
    if (d.frames.size() < 2 || d.frames.size() > kMaxSampleFrames)
        return false;
    auto *data = static_cast<float *>(std::malloc(d.frames.size() * sizeof(float)));
    if (!data)
        return false;
    std::memcpy(data, d.frames.data(), d.frames.size() * sizeof(float));
    std::free(n->data);
    n->data = data;
    n->frames = (uint32_t)d.frames.size();
    n->rate = d.rate;
    return true;
}

} // namespace rack
