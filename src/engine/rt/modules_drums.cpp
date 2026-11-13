// The drum modules: whole drum voices, each one module with its own controls (worklist, "New modules").
// Their sound is made here in the rack from PulseCoreLib's parts. The envelopes are the rack's own:
// the library's ADSR acts on a gate edge at its next control point and takes an off-and-on inside
// one control period as no edge, so it cannot restart a drum on the very sample of each hit.
//
// Kick: a sine body (pcl_osc, reset to phase 0 on each hit) whose pitch falls from `sweep`
// octaves above the tune to the tune (an exponential envelope into the oscillator's pitch input),
// under an exponential decay; a click of noise through a band-pass (pcl_noise, pcl_svf) for the
// punch; drive through tanh. A hit comes from TRIG rising through 0.5 or from a NOTE on (its
// velocity is the accent); ACCENT and DECAY are read at each hit.
#include "engine/rt/modules_common.h"

#include <cmath>

namespace rack
{
namespace
{

    constexpr std::string_view kDrum = "Drum";

    enum KickParam
    {
        KICK_TUNE,
        KICK_SWEEP,
        KICK_SWEEP_TIME,
        KICK_DECAY,
        KICK_PUNCH,
        KICK_DRIVE,
        KICK_ACCENT,
        KICK_COUNT
    };
    constexpr float  kClickHz = 3500, kClickMs = 4, kSilent = 1e-6f;
    constexpr size_t kMaxHits = 32; // per block (one every 8 samples: far beyond any drum roll)

    struct KickNode
    {
        pcl_osc_t   body;
        pcl_noise_t noise;
        pcl_svf_t   click;
        uint32_t    sr;
        float       sweep_ms, decay_ms, punch, drive, accent; // the settings
        // the voice: its envelopes (1 at a hit, falling by their factor each sample), the hit's
        // level and decay factor
        float amp, pitch, snap, amp_k, pitch_k, snap_k, level, hit_punch, hit_decay_cv;
        bool  trig_high; // the TRIG input's Schmitt trigger
        pcl_sample_t pm[PCL_MAX_BLOCK], tone[PCL_MAX_BLOCK], noise_buf[PCL_MAX_BLOCK],
            band[PCL_MAX_BLOCK];
    };

    // The factor per sample that takes an envelope to 1/1000 (-60 dB) in `ms`.
    float fall(float ms, uint32_t sr)
    {
        return (float)std::exp(std::log(0.001) / (std::max(ms, 0.1f) * 0.001 * (double)sr));
    }

    // The decay factors from the settings (and the hit's DECAY): a turn of either knob reaches the
    // hit that is sounding.
    void kick_factors(KickNode *n)
    {
        n->amp_k = fall(n->decay_ms * std::exp2(2 * n->hit_decay_cv), n->sr);
        n->pitch_k = (float)std::exp(-1.0 / (std::max(n->sweep_ms, 0.1f) * 0.001 * n->sr));
    }

    bool kick_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<KickNode>(s);
        n->sr = ctx.sample_rate;
        n->sweep_ms = p[KICK_SWEEP_TIME];
        n->decay_ms = p[KICK_DECAY];
        n->punch = p[KICK_PUNCH];
        n->drive = p[KICK_DRIVE];
        n->accent = p[KICK_ACCENT];
        n->snap_k = fall(kClickMs, ctx.sample_rate);
        kick_factors(n);
        pcl_osc_config_t o { };
        o.sample_rate_hz = ctx.sample_rate;
        o.waveform = PCL_OSC_SINE;
        o.frequency = to_hz(p[KICK_TUNE]);
        o.pitch_depth_oct = to_oct(p[KICK_SWEEP]);
        o.pulse_width = to_coef(0.5);
        pcl_noise_config_t nc { };
        nc.sample_rate_hz = ctx.sample_rate;
        nc.colour = PCL_NOISE_WHITE;
        nc.seed = 0;
        pcl_svf_config_t f { };
        f.sample_rate_hz = ctx.sample_rate;
        f.cutoff_hz = to_hz(clamp_hz(kClickHz, ctx.sample_rate));
        f.resonance = to_coef(0.2);
        return ok(pcl_osc_init(&n->body, &o)) && ok(pcl_noise_init(&n->noise, &nc))
            && ok(pcl_svf_init(&n->click, &f));
    }
    void kick_reset(void *s)
    {
        auto *n = as<KickNode>(s);
        pcl_osc_reset(&n->body);
        pcl_noise_reset(&n->noise);
        pcl_svf_reset(&n->click);
        n->amp = n->pitch = n->snap = 0;
        n->trig_high = false;
    }

    // A hit at the start of the next sample: the envelopes back to 1 and the body's phase to 0.
    // `accent` is 0 .. 1; `decay_cv` -1 .. 1 halves or doubles the decay twice over.
    void kick_hit(KickNode *n, float accent, float decay_cv)
    {
        // the accent scales the whole hit, and the click a second time: a soft hit is duller too
        const float a = 1 - n->accent + n->accent * std::clamp(accent, 0.f, 1.f);
        n->level = 0.9f * a;
        n->hit_punch = n->punch * a;
        n->hit_decay_cv = std::clamp(decay_cv, -1.f, 1.f);
        kick_factors(n);
        n->amp = n->pitch = n->snap = 1;
        // phase 0, and the oscillator's one sample of latency cleared: its first sample is 0, not
        // the last one of the previous hit (which would click), and every hit starts alike
        pcl_osc_reset(&n->body);
    }

    // Renders [from, to) of the block into out.
    void kick_run(KickNode *n, pcl_sample_t *out, size_t from, size_t to)
    {
        const size_t len = to - from;
        if (len == 0)
            return;
        // the pitch envelope into the body's pitch input (the library reads it at control rate)
        float pitch = n->pitch;
        for (size_t i = 0; i < len; i++)
        {
            n->pm[i] = to_sample(pitch);
            pitch = pitch < kSilent ? 0 : pitch * n->pitch_k;
        }
        n->pitch = pitch;
        pcl_osc_process(&n->body, n->tone, n->pm, nullptr, nullptr, nullptr, len);
        pcl_noise_process(&n->noise, n->noise_buf, len);
        pcl_svf_process(&n->click, n->noise_buf, nullptr, n->band, nullptr, nullptr, len);
        const float g = 1 + 8 * n->drive, norm = 1.f / std::tanh(g);
        for (size_t i = 0; i < len; i++)
        {
            // drive shapes the sound before the accent sets its level, so a soft hit stays soft
            float x = n->amp * sample_to_float(n->tone[i])
                + n->hit_punch * n->snap * sample_to_float(n->band[i]);
            if (n->drive > 0)
                x = std::tanh(g * x) * norm;
            out[from + i] = to_sample(std::clamp(n->level * x, -1.f, 1.f));
            n->amp = n->amp < kSilent ? 0 : n->amp * n->amp_k;
            n->snap = n->snap < kSilent ? 0 : n->snap * n->snap_k;
        }
    }

    void kick_process(void *s, NodeIo &io)
    {
        auto               *n = as<KickNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *trig = io.sig_in[0];
        const pcl_sample_t *accent = io.sig_in[1];
        const pcl_sample_t *decay = io.sig_in[2];
        const EventBuf     &ev = *io.evt_in[0];

        // the hits in this block, in order: TRIG's rising edges and the note-ons
        struct Hit
        {
            size_t at;
            float  velocity; // 0 .. 1, or -1 for a TRIG hit
        };
        Hit    hits[kMaxHits];
        size_t count = 0, e = 0;
        for (size_t i = 0; i < io.frames && count < kMaxHits; i++)
        {
            for (; e < ev.count && ev.data[e].offset <= i && count < kMaxHits; e++)
                if (ev.data[e].type == PCL_EVT_NOTE_ON && ev.data[e].velocity > 0)
                    hits[count++] = { i, (float)ev.data[e].velocity / 127.f };
            if (trig && count < kMaxHits)
            {
                const float v = sample_to_float(trig[i]);
                if (!n->trig_high && v >= 0.5f)
                {
                    n->trig_high = true;
                    hits[count++] = { i, -1.f };
                }
                else if (n->trig_high && v < 0.25f)
                    n->trig_high = false;
            }
        }

        size_t pos = 0;
        for (size_t h = 0; h < count; h++)
        {
            kick_run(n, out, pos, hits[h].at);
            pos = hits[h].at;
            // the accent: the ACCENT input if it is patched, else the note's velocity (a TRIG hit:
            // full)
            const float a = accent ? sample_to_float(accent[pos])
                : hits[h].velocity >= 0 ? hits[h].velocity
                                        : 1.f;
            kick_hit(n, a, decay ? sample_to_float(decay[pos]) : 0.f);
        }
        kick_run(n, out, pos, io.frames);
    }

    const PortDesc kKickPorts[] = {
        { "trig", "TRIG", Kind::Signal, false },
        { "note", "NOTE", Kind::Event, false },
        { "accent", "ACCENT", Kind::Signal, false },
        { "decay", "DECAY", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true },
    };
    const ParamDesc kKickParams[] = {
        param("tune", "Tune", "Hz", 30, 150, 50, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_osc_set_frequency(&as<KickNode>(s)->body, to_hz(v))); }),
        param("sweep", "Pitch sweep", "oct", 0, 4, 2, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_osc_set_pitch_depth(&as<KickNode>(s)->body, to_oct(v))); }),
        param("sweep_time", "Sweep time", "ms", 2, 200, 30, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<KickNode>(s);
                n->sweep_ms = v;
                kick_factors(n);
                return true;
            }),
        param("decay", "Decay", "ms", 20, 3000, 450, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<KickNode>(s);
                n->decay_ms = v;
                kick_factors(n);
                return true;
            }),
        param("punch", "Punch", "", 0, 1, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<KickNode>(s)->punch = v;
                return true;
            }),
        param("drive", "Drive", "", 0, 1, 0.2f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<KickNode>(s)->drive = v;
                return true;
            }),
        param("accent", "Accent", "", 0, 1, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<KickNode>(s)->accent = v;
                return true;
            }),
    };
    static_assert(std::size(kKickParams) == KICK_COUNT);

} // namespace

RACK_DESC(kKickDesc, "kick", "Kick", kDrum, 8, KickNode, kKickPorts, kKickParams, false, kick_init,
    kick_reset, kick_process, nullptr, nullptr,
    "A kick drum: a sine that falls in pitch, a click for punch, drive; hit by a trigger or a note");

} // namespace rack
