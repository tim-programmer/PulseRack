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
#include <initializer_list>

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

    // ---- what every drum shares: its hits, and its envelopes' factors

    // A hit: its sample in the block, its velocity (0 .. 1; -1 for a trigger input's), which input
    // it came from (0: NOTE or the first trigger, else the trigger's own kind) and its note.
    struct Hit
    {
        size_t  at;
        float   velocity;
        uint8_t kind, note;
    };
    // A trigger input: a hit when it rises through 0.5 (a Schmitt trigger: low again under 0.25).
    struct Trig
    {
        const pcl_sample_t *in; // null: not patched
        bool               *high;
        uint8_t             kind;
    };
    // The hits of a block in time order (at one sample: the notes, then the triggers in order).
    size_t collect_hits(Hit *hits, size_t frames, const EventBuf &ev, std::initializer_list<Trig> trigs)
    {
        size_t count = 0, e = 0;
        for (size_t i = 0; i < frames && count < kMaxHits; i++)
        {
            for (; e < ev.count && ev.data[e].offset <= i && count < kMaxHits; e++)
                if (ev.data[e].type == PCL_EVT_NOTE_ON && ev.data[e].velocity > 0)
                    hits[count++] = { i, (float)ev.data[e].velocity / 127.f, 0, ev.data[e].note };
            for (const Trig &t : trigs)
            {
                if (!t.in || count >= kMaxHits)
                    continue;
                const float v = sample_to_float(t.in[i]);
                if (!*t.high && v >= 0.5f)
                {
                    *t.high = true;
                    hits[count++] = { i, -1.f, t.kind, 0 };
                }
                else if (*t.high && v < 0.25f)
                    *t.high = false;
            }
        }
        return count;
    }
    // A hit's accent, 0 .. 1: the ACCENT input if it is patched, else the note's velocity (a
    // trigger's hit: full); and the level it makes with the Accent knob's `amount`.
    float accent_of(const Hit &h, const pcl_sample_t *accent_in)
    {
        return accent_in ? sample_to_float(accent_in[h.at]) : h.velocity >= 0 ? h.velocity : 1.f;
    }
    float accent_level(float amount, float accent)
    {
        return 1 - amount + amount * std::clamp(accent, 0.f, 1.f);
    }

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
        const float a = accent_level(n->accent, accent);
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

        Hit          hits[kMaxHits];
        const size_t count = collect_hits(hits, io.frames, ev, { { trig, &n->trig_high, 0 } });
        size_t       pos = 0;
        for (size_t h = 0; h < count; h++)
        {
            kick_run(n, out, pos, hits[h].at);
            pos = hits[h].at;
            kick_hit(n, accent_of(hits[h], accent), decay ? sample_to_float(decay[pos]) : 0.f);
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

    // ============================================================================================================
    // Snare / Clap: a short sine body (pcl_osc, reset on each hit, its pitch dropping a little)
    // under noise (pcl_noise through pcl_svf: its band-pass and some of its high-pass) with a decay
    // of its own. Clap turns the noise into three quick bursts, Spread apart, before its tail, and
    // takes the body away as it does: at 0 a snare, at 1 a clap, between the two a snare with a
    // clap's smear. DECAY (-1..1, at each hit) shortens or lengthens the noise's tail.
    enum SnareParam
    {
        SN_TUNE,
        SN_BODY,
        SN_TONE,
        SN_SNAP,
        SN_DECAY,
        SN_CLAP,
        SN_SPREAD,
        SN_ACCENT,
        SN_COUNT
    };
    constexpr float kSnareDropOct = 0.6f, kSnareDropMs = 8, kBurstMs = 18;
    struct SnareNode
    {
        pcl_osc_t   body;
        pcl_noise_t noise;
        pcl_svf_t   filt;
        uint32_t    sr;
        float       body_ms, snap, decay_ms, clap, spread_ms, accent; // the settings
        float       amp, pitch, nenv, amp_k, pitch_k, burst_k, tail_k, level, hit_decay_cv;
        uint32_t    bursts_left, burst_timer;
        bool        trig_high;
        pcl_sample_t pm[PCL_MAX_BLOCK], tone[PCL_MAX_BLOCK], noise_buf[PCL_MAX_BLOCK],
            band[PCL_MAX_BLOCK], high[PCL_MAX_BLOCK];
    };
    void snare_factors(SnareNode *n)
    {
        n->amp_k = fall(n->body_ms, n->sr);
        n->tail_k = fall(n->decay_ms * std::exp2(2 * n->hit_decay_cv), n->sr);
        n->burst_k = fall(kBurstMs, n->sr);
        n->pitch_k = (float)std::exp(-1.0 / (kSnareDropMs * 0.001 * n->sr));
    }
    bool snare_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<SnareNode>(s);
        n->sr = ctx.sample_rate;
        n->body_ms = p[SN_BODY];
        n->snap = p[SN_SNAP];
        n->decay_ms = p[SN_DECAY];
        n->clap = p[SN_CLAP];
        n->spread_ms = p[SN_SPREAD];
        n->accent = p[SN_ACCENT];
        snare_factors(n);
        pcl_osc_config_t o { };
        o.sample_rate_hz = ctx.sample_rate;
        o.waveform = PCL_OSC_SINE;
        o.frequency = to_hz(p[SN_TUNE]);
        o.pitch_depth_oct = to_oct(kSnareDropOct);
        o.pulse_width = to_coef(0.5);
        pcl_noise_config_t nc { };
        nc.sample_rate_hz = ctx.sample_rate;
        nc.colour = PCL_NOISE_WHITE;
        nc.seed = 0;
        pcl_svf_config_t f { };
        f.sample_rate_hz = ctx.sample_rate;
        f.cutoff_hz = to_hz(clamp_hz(p[SN_TONE], ctx.sample_rate));
        f.resonance = to_coef(0.35);
        return ok(pcl_osc_init(&n->body, &o)) && ok(pcl_noise_init(&n->noise, &nc))
            && ok(pcl_svf_init(&n->filt, &f));
    }
    void snare_reset(void *s)
    {
        auto *n = as<SnareNode>(s);
        pcl_osc_reset(&n->body);
        pcl_noise_reset(&n->noise);
        pcl_svf_reset(&n->filt);
        n->amp = n->pitch = n->nenv = 0;
        n->bursts_left = 0;
        n->trig_high = false;
    }
    void snare_hit(SnareNode *n, float accent, float decay_cv)
    {
        n->level = 0.9f * accent_level(n->accent, accent);
        n->hit_decay_cv = std::clamp(decay_cv, -1.f, 1.f);
        snare_factors(n);
        n->amp = n->pitch = n->nenv = 1;
        // a clap: this burst and two more, then the tail; a snare: the tail at once
        n->bursts_left = n->clap > 0.01f ? 2 : 0;
        n->burst_timer = (uint32_t)std::max(1.f, n->spread_ms * 0.001f * (float)n->sr);
        pcl_osc_reset(&n->body);
    }
    void snare_run(SnareNode *n, pcl_sample_t *out, size_t from, size_t to)
    {
        const size_t len = to - from;
        if (len == 0)
            return;
        float pitch = n->pitch;
        for (size_t i = 0; i < len; i++)
        {
            n->pm[i] = to_sample(pitch);
            pitch = pitch < kSilent ? 0 : pitch * n->pitch_k;
        }
        n->pitch = pitch;
        pcl_osc_process(&n->body, n->tone, n->pm, nullptr, nullptr, nullptr, len);
        pcl_noise_process(&n->noise, n->noise_buf, len);
        pcl_svf_process(&n->filt, n->noise_buf, nullptr, n->band, n->high, nullptr, len);
        const float body_gain = 1 - n->clap, g = 1.6f, norm = 1.f / std::tanh(g);
        const uint32_t spread = (uint32_t)std::max(1.f, n->spread_ms * 0.001f * (float)n->sr);
        for (size_t i = 0; i < len; i++)
        {
            const float noise = sample_to_float(n->band[i]) + 0.5f * sample_to_float(n->high[i]);
            const float x = body_gain * n->amp * sample_to_float(n->tone[i])
                + n->snap * n->nenv * noise;
            out[from + i] = to_sample(std::clamp(n->level * std::tanh(g * x) * norm, -1.f, 1.f));
            n->amp = n->amp < kSilent ? 0 : n->amp * n->amp_k;
            const bool bursting = n->bursts_left > 0 || (n->clap > 0.01f && n->burst_timer > 0);
            n->nenv = n->nenv < kSilent ? 0 : n->nenv * (bursting ? n->burst_k : n->tail_k);
            if (n->burst_timer > 0 && --n->burst_timer == 0 && n->bursts_left > 0)
            { // the next burst; the last one goes on into the tail
                n->bursts_left--;
                n->nenv = 1;
                if (n->bursts_left > 0)
                    n->burst_timer = spread;
            }
        }
    }
    void snare_process(void *s, NodeIo &io)
    {
        auto         *n = as<SnareNode>(s);
        pcl_sample_t *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        Hit           hits[kMaxHits];
        const size_t  count
            = collect_hits(hits, io.frames, *io.evt_in[0], { { io.sig_in[0], &n->trig_high, 0 } });
        size_t pos = 0;
        for (size_t h = 0; h < count; h++)
        {
            snare_run(n, out, pos, hits[h].at);
            pos = hits[h].at;
            snare_hit(n, accent_of(hits[h], io.sig_in[1]),
                io.sig_in[2] ? sample_to_float(io.sig_in[2][pos]) : 0.f);
        }
        snare_run(n, out, pos, io.frames);
    }
    const PortDesc kSnarePorts[] = {
        { "trig", "TRIG", Kind::Signal, false },
        { "note", "NOTE", Kind::Event, false },
        { "accent", "ACCENT", Kind::Signal, false },
        { "decay", "DECAY", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true },
    };
    template <float SnareNode::*F> bool snare_set(void *s, float v)
    {
        auto *n = as<SnareNode>(s);
        n->*F = v;
        snare_factors(n);
        return true;
    }
    const ParamDesc kSnareParams[] = {
        param("tune", "Tune", "Hz", 100, 400, 185, Scale::Log, Control::Knob, [](void *s, float v)
            { return ok(pcl_osc_set_frequency(&as<SnareNode>(s)->body, to_hz(v))); }),
        param("body", "Body", "ms", 20, 500, 110, Scale::Log, Control::Knob,
            snare_set<&SnareNode::body_ms>),
        param("tone", "Tone", "Hz", 800, 10000, 3200, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<SnareNode>(s);
                return ok(pcl_svf_set_cutoff(&n->filt, to_hz(clamp_hz(v, n->sr))));
            }),
        param("snap", "Snap", "", 0, 1, 0.65f, Scale::Linear, Control::Knob,
            snare_set<&SnareNode::snap>),
        param("decay", "Decay", "ms", 30, 1000, 220, Scale::Log, Control::Knob,
            snare_set<&SnareNode::decay_ms>),
        param("clap", "Clap", "", 0, 1, 0, Scale::Linear, Control::Knob,
            snare_set<&SnareNode::clap>),
        param("spread", "Spread", "ms", 4, 30, 11, Scale::Linear, Control::Knob,
            snare_set<&SnareNode::spread_ms>),
        param("accent", "Accent", "", 0, 1, 0.5f, Scale::Linear, Control::Knob,
            snare_set<&SnareNode::accent>),
    };
    static_assert(std::size(kSnareParams) == SN_COUNT);

    // ============================================================================================================
    // Metal (hats and cymbals): six square waves at inharmonic ratios (pcl_osc, running freely, as
    // in the classic analog hats), blended with noise by Noise, through a band-pass and a high-pass
    // (pcl_svf) at Tone, under one decay: short for a closed hit, long for an open one. TRIG and a
    // note hit closed, OPEN (and the notes of open hats and cymbals in General MIDI: 46, 49, 51,
    // 52, 55, 57, 59) open; any hit cuts the one before it, so a closed hat chokes an open one, as
    // on a real hi-hat; CHOKE fades it out at once.
    enum MetalParam
    {
        MT_TUNE,
        MT_NOISE,
        MT_TONE,
        MT_CLOSED,
        MT_OPEN,
        MT_ACCENT,
        MT_COUNT
    };
    constexpr float kMetalHz[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
    constexpr float kChokeMs = 12;
    enum : uint8_t
    {
        HIT_CLOSED,
        HIT_OPEN,
        HIT_CHOKE
    };
    struct MetalNode
    {
        pcl_osc_t   osc[6];
        pcl_noise_t noise;
        pcl_svf_t   band, high;
        uint32_t    sr;
        float       tune, noise_mix, tone_hz, closed_ms, open_ms, accent; // the settings
        float       env, k, level;
        bool        trig_high, open_high, choke_high;
        pcl_sample_t sum[PCL_MAX_BLOCK], one[PCL_MAX_BLOCK], noise_buf[PCL_MAX_BLOCK],
            bp[PCL_MAX_BLOCK], hp[PCL_MAX_BLOCK];
    };
    bool metal_retune(MetalNode *n)
    {
        bool good = true;
        for (int i = 0; i < 6; i++)
            good = good
                && ok(pcl_osc_set_frequency(
                    &n->osc[i], to_hz(clamp_hz(kMetalHz[i] * n->tune, n->sr))));
        return good;
    }
    bool metal_filters(MetalNode *n)
    {
        return ok(pcl_svf_set_cutoff(&n->band, to_hz(clamp_hz(n->tone_hz, n->sr))))
            && ok(pcl_svf_set_cutoff(&n->high, to_hz(clamp_hz(n->tone_hz * 0.6, n->sr))));
    }
    bool metal_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<MetalNode>(s);
        n->sr = ctx.sample_rate;
        n->tune = p[MT_TUNE];
        n->noise_mix = p[MT_NOISE];
        n->tone_hz = p[MT_TONE];
        n->closed_ms = p[MT_CLOSED];
        n->open_ms = p[MT_OPEN];
        n->accent = p[MT_ACCENT];
        bool good = true;
        for (int i = 0; i < 6; i++)
        {
            pcl_osc_config_t o { };
            o.sample_rate_hz = ctx.sample_rate;
            o.waveform = PCL_OSC_PULSE;
            o.frequency = to_hz(clamp_hz(kMetalHz[i] * n->tune, ctx.sample_rate));
            o.pulse_width = to_coef(0.5);
            good = good && ok(pcl_osc_init(&n->osc[i], &o));
        }
        pcl_noise_config_t nc { };
        nc.sample_rate_hz = ctx.sample_rate;
        nc.colour = PCL_NOISE_WHITE;
        nc.seed = 0;
        pcl_svf_config_t f { };
        f.sample_rate_hz = ctx.sample_rate;
        f.cutoff_hz = to_hz(clamp_hz(n->tone_hz, ctx.sample_rate));
        f.resonance = to_coef(0.4);
        pcl_svf_config_t h = f;
        h.cutoff_hz = to_hz(clamp_hz(n->tone_hz * 0.6, ctx.sample_rate));
        h.resonance = to_coef(0.1);
        return good && ok(pcl_noise_init(&n->noise, &nc)) && ok(pcl_svf_init(&n->band, &f))
            && ok(pcl_svf_init(&n->high, &h));
    }
    void metal_reset(void *s)
    {
        auto *n = as<MetalNode>(s);
        for (pcl_osc_t &o : n->osc)
            pcl_osc_reset(&o);
        pcl_noise_reset(&n->noise);
        pcl_svf_reset(&n->band);
        pcl_svf_reset(&n->high);
        n->env = 0;
        n->trig_high = n->open_high = n->choke_high = false;
    }
    void metal_run(MetalNode *n, pcl_sample_t *out, size_t from, size_t to)
    {
        const size_t len = to - from;
        if (len == 0)
            return;
        for (size_t i = 0; i < len; i++)
            n->sum[i] = 0;
        for (pcl_osc_t &o : n->osc)
        {
            pcl_osc_process(&o, n->one, nullptr, nullptr, nullptr, nullptr, len);
            for (size_t i = 0; i < len; i++)
                n->sum[i] = to_sample(sample_to_float(n->sum[i]) + sample_to_float(n->one[i]) / 6);
        }
        pcl_noise_process(&n->noise, n->noise_buf, len);
        for (size_t i = 0; i < len; i++)
            n->sum[i] = to_sample((1 - n->noise_mix) * sample_to_float(n->sum[i])
                + n->noise_mix * 0.7f * sample_to_float(n->noise_buf[i]));
        pcl_svf_process(&n->band, n->sum, nullptr, n->bp, nullptr, nullptr, len);
        pcl_svf_process(&n->high, n->bp, nullptr, nullptr, n->hp, nullptr, len);
        for (size_t i = 0; i < len; i++)
        {
            out[from + i] = to_sample(
                std::clamp(2.5f * n->level * n->env * sample_to_float(n->hp[i]), -1.f, 1.f));
            n->env = n->env < kSilent ? 0 : n->env * n->k;
        }
    }
    bool is_open_note(uint8_t note)
    {
        return note == 46 || note == 49 || note == 51 || note == 52 || note == 55 || note == 57
            || note == 59;
    }
    void metal_process(void *s, NodeIo &io)
    {
        auto         *n = as<MetalNode>(s);
        pcl_sample_t *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        Hit           hits[kMaxHits];
        const size_t  count = collect_hits(hits, io.frames, *io.evt_in[0],
             { { io.sig_in[0], &n->trig_high, HIT_CLOSED }, { io.sig_in[1], &n->open_high, HIT_OPEN },
                 { io.sig_in[2], &n->choke_high, HIT_CHOKE } });
        size_t pos = 0;
        for (size_t h = 0; h < count; h++)
        {
            metal_run(n, out, pos, hits[h].at);
            pos = hits[h].at;
            const Hit &x = hits[h];
            if (x.kind == HIT_CHOKE)
            {
                n->k = fall(kChokeMs, n->sr); // (from where it is)
                continue;
            }
            const bool open = x.kind == HIT_OPEN || (x.velocity >= 0 && is_open_note(x.note));
            n->level = accent_level(n->accent, accent_of(x, io.sig_in[3]));
            n->env = 1;
            n->k = fall(open ? n->open_ms : n->closed_ms, n->sr);
        }
        metal_run(n, out, pos, io.frames);
    }
    const PortDesc kMetalPorts[] = {
        { "trig", "TRIG", Kind::Signal, false },
        { "open", "OPEN", Kind::Signal, false },
        { "choke", "CHOKE", Kind::Signal, false },
        { "note", "NOTE", Kind::Event, false },
        { "accent", "ACCENT", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true },
    };
    const ParamDesc kMetalParams[] = {
        param("tune", "Tune", "x", 0.5f, 2, 1, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<MetalNode>(s);
                n->tune = v;
                return metal_retune(n);
            }),
        param("noise", "Noise", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<MetalNode>(s)->noise_mix = v;
                return true;
            }),
        param("tone", "Tone", "Hz", 3000, 15000, 8000, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<MetalNode>(s);
                n->tone_hz = v;
                return metal_filters(n);
            }),
        param("closed", "Closed", "ms", 10, 400, 60, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                as<MetalNode>(s)->closed_ms = v; // (from the next hit)
                return true;
            }),
        param("open", "Open", "ms", 100, 4000, 700, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                as<MetalNode>(s)->open_ms = v;
                return true;
            }),
        param("accent", "Accent", "", 0, 1, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                as<MetalNode>(s)->accent = v;
                return true;
            }),
    };
    static_assert(std::size(kMetalParams) == MT_COUNT);

} // namespace

RACK_DESC(kKickDesc, "kick", "Kick", kDrum, 8, KickNode, kKickPorts, kKickParams, false, kick_init,
    kick_reset, kick_process, nullptr, nullptr,
    "A kick drum: a sine that falls in pitch, a click for punch, drive; hit by a trigger or a note");

RACK_DESC(kSnareDesc, "snare", "Snare / Clap", kDrum, 8, SnareNode, kSnarePorts, kSnareParams, false,
    snare_init, snare_reset, snare_process, nullptr, nullptr,
    "A snare's body and rattle, or a clap's bursts of noise, or anything between");
RACK_DESC(kMetalDesc, "metal", "Metal", kDrum, 8, MetalNode, kMetalPorts, kMetalParams, false,
    metal_init, metal_reset, metal_process, nullptr, nullptr,
    "Hi-hats and cymbals: six square waves and noise; closed, open and choke");

} // namespace rack
