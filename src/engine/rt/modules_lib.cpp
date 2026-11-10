// Wrappers for the PulseCoreLib modules: ports, parameters and the process function of each. A
// module type is one descriptor and one wrapper; nothing else in the rack names a library setter.
// (docs/patch-engine.md)
#include "engine/rt/modules_common.h"

#include <cstring>

namespace rack
{
namespace
{

    constexpr std::string_view kSource = "Source", kModulator = "Modulator", kFilter = "Filter",
                               kEvent = "Event";

    // ============================================================================================================
    // Oscillator
    constexpr std::string_view kWaveLabels[] = { "sine", "triangle", "saw", "pulse" };
    enum OscParam
    {
        OSC_WAVE,
        OSC_FREQ,
        OSC_PITCH_DEPTH,
        OSC_PW,
        OSC_PWM_DEPTH,
        OSC_COUNT
    };

    struct OscNode
    {
        pcl_osc_t osc;
        uint32_t  sr;
    };

    bool osc_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<OscNode>(s);
        n->sr = ctx.sample_rate;
        pcl_osc_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.waveform = (pcl_osc_waveform_t)to_u32(p[OSC_WAVE]);
        c.frequency = to_hz(clamp_hz(p[OSC_FREQ], ctx.sample_rate));
        c.pitch_depth_oct = to_oct(p[OSC_PITCH_DEPTH]);
        c.pulse_width = to_coef(p[OSC_PW]);
        c.pwm_depth = to_coef(p[OSC_PWM_DEPTH]);
        return ok(pcl_osc_init(&n->osc, &c));
    }
    void osc_reset(void *s) { pcl_osc_reset(&as<OscNode>(s)->osc); }

    // A NOTE_ON retunes the oscillator on its own sample: the block is cut there (as tools/render.c
    // does), so the note is sample-accurate. The frequency then glides over the library's ~5 ms
    // smoothing.
    void osc_process(void *s, NodeIo &io)
    {
        auto               *n = as<OscNode>(s);
        pcl_sample_t       *out = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        const pcl_sample_t *pitch = io.sig_in[0];
        const pcl_sample_t *pwm = io.sig_in[1];
        const pcl_sample_t *sync_in = io.sig_in[2];
        pcl_sample_t       *sync_out = io.sig_out[1];
        const EventBuf     &ev = *io.evt_in[0];
        size_t              pos = 0, i = 0;

        // SYNC carries "wrap fractions" in [0, 1] (0 = no event), which only another oscillator's
        // SYNC OUT makes. The rack lets any signal be patched in, and a negative sample is outside
        // the library's contract: in the Q31 build it overflows a signed integer (found by UBSan
        // with noise patched into SYNC). So the input is clamped to [0, 1] on its way in; a
        // sync-out signal passes unchanged.
        // PulseCoreLib has since made the same clamp itself; this one can go when that
        // fix is on its main.
        pcl_sample_t sync_clean[PCL_MAX_BLOCK];
        if (sync_in)
        {
            for (size_t k = 0; k < io.frames; k++)
                sync_clean[k] = sync_in[k] < 0
                    ? (pcl_sample_t)0
                    : (sync_in[k] > kFullScale ? kFullScale : sync_in[k]);
            sync_in = sync_clean;
        }

        while (pos < io.frames)
        {
            while (i < ev.count && ev.data[i].offset <= pos)
            {
                if (ev.data[i].type == PCL_EVT_NOTE_ON && ev.data[i].velocity > 0)
                    (void)pcl_osc_set_frequency(&n->osc, pcl_midi_to_hz(ev.data[i].note));
                i++;
            }
            const size_t end
                = i < ev.count ? std::min<size_t>(io.frames, ev.data[i].offset) : io.frames;
            pcl_osc_process(&n->osc, out + pos, pitch ? pitch + pos : nullptr,
                pwm ? pwm + pos : nullptr, sync_in ? sync_in + pos : nullptr,
                sync_out ? sync_out + pos : nullptr, end - pos);
            pos = end;
        }
    }

    const PortDesc kOscPorts[] = {
        { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false },
        { "pwm", "PWM", Kind::Signal, false },
        { "sync", "SYNC", Kind::Signal, false },
        { "out", "OUT", Kind::Signal, true },
        { "sync_out", "SYNC OUT", Kind::Signal, true },
    };
    // The ranges stay a little inside the library's limits (pulse width 0.01 .. 0.99, PWM depth 0
    // .. 0.49): a float such as 0.99f is a hair above the library's 0.99 once converted to Q30, and
    // the fixed-point builds refuse it.
    const ParamDesc kOscParams[] = {
        param(
            "waveform", "Waveform", "", 0, 3, 2, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(
                    pcl_osc_set_waveform(&as<OscNode>(s)->osc, (pcl_osc_waveform_t)to_u32(v)));
            },
            kWaveLabels),
        param("frequency", "Frequency", "Hz", 20, 8000, 220, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<OscNode>(s);
                return ok(pcl_osc_set_frequency(&n->osc, to_hz(clamp_hz(v, n->sr))));
            }),
        param("pitch_depth", "Pitch depth", "oct", 0, 4, 1, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_osc_set_pitch_depth(&as<OscNode>(s)->osc, to_oct(v))); }),
        param("pulse_width", "Pulse width", "", 0.02f, 0.98f, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_osc_set_pulse_width(&as<OscNode>(s)->osc, to_coef(v))); }),
        param("pwm_depth", "PWM depth", "", 0, 0.48f, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_osc_set_pwm_depth(&as<OscNode>(s)->osc, to_coef(v))); }),
    };
    static_assert(std::size(kOscParams) == OSC_COUNT);

    // ============================================================================================================
    // Noise
    constexpr std::string_view kColourLabels[] = { "white", "pink", "brown" };
    enum NoiseParam
    {
        NOISE_COLOUR,
        NOISE_SEED,
        NOISE_COUNT
    };

    struct NoiseNode
    {
        pcl_noise_t noise;
    };
    bool noise_init(void *s, const NodeContext &ctx, const float *p)
    {
        pcl_noise_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.colour = (pcl_noise_colour_t)to_u32(p[NOISE_COLOUR]);
        c.seed = to_u32(p[NOISE_SEED]);
        return ok(pcl_noise_init(&as<NoiseNode>(s)->noise, &c));
    }
    void noise_reset(void *s) { pcl_noise_reset(&as<NoiseNode>(s)->noise); }
    void noise_process(void *s, NodeIo &io)
    {
        pcl_noise_process(
            &as<NoiseNode>(s)->noise, io.sig_out[0] ? io.sig_out[0] : io.scratch, io.frames);
    }
    const PortDesc  kNoisePorts[] = { { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kNoiseParams[] = {
        param(
            "colour", "Colour", "", 0, 2, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(
                    pcl_noise_set_colour(&as<NoiseNode>(s)->noise, (pcl_noise_colour_t)to_u32(v)));
            },
            kColourLabels),
        param("seed", "Seed", "", 0, 16777215, 0, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                pcl_noise_set_seed(&as<NoiseNode>(s)->noise, to_u32(v));
                return true;
            }),
    };
    static_assert(std::size(kNoiseParams) == NOISE_COUNT);

    // ============================================================================================================
    // Sample & Hold
    enum SahParam
    {
        SAH_RATE,
        SAH_SLEW,
        SAH_SEED,
        SAH_COUNT
    };
    struct SahNode
    {
        pcl_sah_t sah;
    };
    bool sah_init(void *s, const NodeContext &ctx, const float *p)
    {
        pcl_sah_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.seed = to_u32(p[SAH_SEED]);
        c.rate_hz = to_hz(p[SAH_RATE]);
        c.slew_ms = to_u32(p[SAH_SLEW]);
        return ok(pcl_sah_init(&as<SahNode>(s)->sah, &c));
    }
    void sah_reset(void *s) { pcl_sah_reset(&as<SahNode>(s)->sah); }
    void sah_process(void *s, NodeIo &io)
    {
        const EventBuf &ev = *io.evt_in[0];
        pcl_sah_process(&as<SahNode>(s)->sah, io.sig_out[0] ? io.sig_out[0] : io.scratch,
            io.sig_in[0], ev.data, ev.count, io.frames);
    }
    const PortDesc  kSahPorts[] = { { "trig", "TRIG", Kind::Signal, false },
        { "clock", "CLOCK", Kind::Event, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kSahParams[] = {
        param("rate", "Rate", "Hz", 0, 1000, 4, Scale::Linear, Control::Knob,
            [](void *s, float v) { return ok(pcl_sah_set_rate(&as<SahNode>(s)->sah, to_hz(v))); }),
        param("slew", "Slew", "ms", 0, 10000, 0, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_sah_set_slew_ms(&as<SahNode>(s)->sah, to_u32(v))); }),
        param("seed", "Seed", "", 0, 16777215, 0, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                pcl_sah_set_seed(&as<SahNode>(s)->sah, to_u32(v));
                return true;
            }),
    };
    static_assert(std::size(kSahParams) == SAH_COUNT);

    // ============================================================================================================
    // LFO
    constexpr std::string_view kLfoShapes[]
        = { "sine", "triangle", "saw up", "saw down", "square", "random step", "random smooth" };
    enum LfoParam
    {
        LFO_SHAPE,
        LFO_FREQ,
        LFO_RATE_DEPTH,
        LFO_PW,
        LFO_UNIPOLAR,
        LFO_SEED,
        LFO_COUNT
    };
    struct LfoNode
    {
        pcl_lfo_t lfo;
    };
    bool lfo_init(void *s, const NodeContext &ctx, const float *p)
    {
        pcl_lfo_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.shape = (pcl_lfo_shape_t)to_u32(p[LFO_SHAPE]);
        c.frequency = to_hz(p[LFO_FREQ]);
        c.rate_depth_oct = to_oct(p[LFO_RATE_DEPTH]);
        c.pulse_width = to_coef(p[LFO_PW]);
        c.unipolar = to_int(p[LFO_UNIPOLAR]);
        c.start_phase = 0;
        c.seed = to_u32(p[LFO_SEED]);
        return ok(pcl_lfo_init(&as<LfoNode>(s)->lfo, &c));
    }
    void lfo_reset(void *s) { pcl_lfo_reset(&as<LfoNode>(s)->lfo); }
    void lfo_process(void *s, NodeIo &io)
    {
        pcl_lfo_process(&as<LfoNode>(s)->lfo, io.sig_out[0] ? io.sig_out[0] : io.scratch,
            io.sig_in[0], io.frames);
    }
    const PortDesc kLfoPorts[]
        = { { "rate", "RATE", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kLfoParams[] = {
        param(
            "shape", "Shape", "", 0, 6, 0, Scale::Enum, Control::Selector, [](void *s, float v)
            { return ok(pcl_lfo_set_shape(&as<LfoNode>(s)->lfo, (pcl_lfo_shape_t)to_u32(v))); },
            kLfoShapes),
        param("frequency", "Frequency", "Hz", 0.01f, 100, 1, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_lfo_set_frequency(&as<LfoNode>(s)->lfo, to_hz(v))); }),
        param("rate_depth", "Rate depth", "oct", 0, 4, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_lfo_set_rate_depth(&as<LfoNode>(s)->lfo, to_oct(v))); }),
        param("pulse_width", "Pulse width", "", 0.02f, 0.98f, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_lfo_set_pulse_width(&as<LfoNode>(s)->lfo, to_coef(v))); }),
        param("unipolar", "Unipolar", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_lfo_set_unipolar(&as<LfoNode>(s)->lfo, to_int(v));
                return true;
            }),
        param("seed", "Seed", "", 0, 16777215, 0, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                pcl_lfo_set_seed(&as<LfoNode>(s)->lfo, to_u32(v));
                return true;
            }),
    };
    static_assert(std::size(kLfoParams) == LFO_COUNT);

    // ============================================================================================================
    // ADSR
    enum AdsrParam
    {
        ADSR_ATTACK,
        ADSR_DECAY,
        ADSR_SUSTAIN,
        ADSR_RELEASE,
        ADSR_ACURVE,
        ADSR_DCURVE,
        ADSR_RCURVE,
        ADSR_RUNTHRU,
        ADSR_COUNT
    };
    struct AdsrNode
    {
        pcl_adsr_t adsr;
    };
    bool adsr_init(void *s, const NodeContext &ctx, const float *p)
    {
        pcl_adsr_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.attack_ms = to_u32(p[ADSR_ATTACK]);
        c.decay_ms = to_u32(p[ADSR_DECAY]);
        c.release_ms = to_u32(p[ADSR_RELEASE]);
        c.sustain = to_coef(p[ADSR_SUSTAIN]);
        c.attack_curve = to_coef(p[ADSR_ACURVE]);
        c.decay_curve = to_coef(p[ADSR_DCURVE]);
        c.release_curve = to_coef(p[ADSR_RCURVE]);
        c.run_through = to_int(p[ADSR_RUNTHRU]);
        return ok(pcl_adsr_init(&as<AdsrNode>(s)->adsr, &c));
    }
    void adsr_reset(void *s) { pcl_adsr_reset(&as<AdsrNode>(s)->adsr); }
    void adsr_process(void *s, NodeIo &io)
    {
        const EventBuf &ev = *io.evt_in[0];
        pcl_adsr_process(&as<AdsrNode>(s)->adsr, io.sig_out[0] ? io.sig_out[0] : io.scratch,
            io.sig_in[0], ev.data, ev.count, io.frames);
    }
    const PortDesc  kAdsrPorts[] = { { "gate", "GATE", Kind::Signal, false },
        { "note", "NOTE", Kind::Event, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kAdsrParams[] = {
        param("attack", "Attack", "ms", 1, 10000, 10, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_adsr_set_attack_ms(&as<AdsrNode>(s)->adsr, to_u32(v))); }),
        param("decay", "Decay", "ms", 1, 10000, 200, Scale::Log, Control::Knob, [](void *s, float v)
            { return ok(pcl_adsr_set_decay_ms(&as<AdsrNode>(s)->adsr, to_u32(v))); }),
        param("sustain", "Sustain", "", 0, 1, 0.7f, Scale::Linear, Control::Slider,
            [](void *s, float v)
            { return ok(pcl_adsr_set_sustain(&as<AdsrNode>(s)->adsr, to_coef(v))); }),
        param("release", "Release", "ms", 1, 10000, 300, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_adsr_set_release_ms(&as<AdsrNode>(s)->adsr, to_u32(v))); }),
        param("attack_curve", "Attack curve", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_adsr_set_attack_curve(&as<AdsrNode>(s)->adsr, to_coef(v))); }),
        param("decay_curve", "Decay curve", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_adsr_set_decay_curve(&as<AdsrNode>(s)->adsr, to_coef(v))); }),
        param("release_curve", "Release curve", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_adsr_set_release_curve(&as<AdsrNode>(s)->adsr, to_coef(v))); }),
        param("run_through", "Run through", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_adsr_set_run_through(&as<AdsrNode>(s)->adsr, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kAdsrParams) == ADSR_COUNT);

    // ============================================================================================================
    // VCA
    struct VcaNode
    {
        pcl_vca_t vca;
    };
    bool vca_init(void *s, const NodeContext &ctx, const float *p)
    {
        pcl_vca_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.gain = to_coef(p[0]);
        return ok(pcl_vca_init(&as<VcaNode>(s)->vca, &c));
    }
    void vca_reset(void *s) { pcl_vca_reset(&as<VcaNode>(s)->vca); }
    void vca_process(void *s, NodeIo &io)
    {
        pcl_vca_process(&as<VcaNode>(s)->vca, io.sig_in[0] ? io.sig_in[0] : io.zeros,
            io.sig_out[0] ? io.sig_out[0] : io.scratch, io.sig_in[1], io.frames);
    }
    const PortDesc  kVcaPorts[] = { { "in", "IN", Kind::Signal, false },
        { "gain", "GAIN", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kVcaParams[] = {
        param("gain", "Gain", "", 0, 1.9f, 1, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_vca_set_gain(&as<VcaNode>(s)->vca, to_coef(v))); }),
    };

    // ============================================================================================================
    // State-variable filter
    enum SvfParam
    {
        SVF_CUTOFF,
        SVF_RES,
        SVF_DEPTH,
        SVF_SAT,
        SVF_COUNT
    };
    struct SvfNode
    {
        pcl_svf_t svf;
        uint32_t  sr;
    };
    bool svf_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<SvfNode>(s);
        n->sr = ctx.sample_rate;
        pcl_svf_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.cutoff_hz = to_hz(clamp_hz(p[SVF_CUTOFF], ctx.sample_rate));
        c.resonance = to_coef(p[SVF_RES]);
        c.mod_depth_oct = to_oct(p[SVF_DEPTH]);
        c.saturate = to_int(p[SVF_SAT]);
        return ok(pcl_svf_init(&n->svf, &c));
    }
    void svf_reset(void *s) { pcl_svf_reset(&as<SvfNode>(s)->svf); }
    void svf_process(void *s, NodeIo &io)
    {
        pcl_svf_process(&as<SvfNode>(s)->svf, io.sig_in[0] ? io.sig_in[0] : io.zeros, io.sig_out[0],
            io.sig_out[1], io.sig_out[2], io.sig_in[1], io.frames);
    }
    const PortDesc  kSvfPorts[] = { { "in", "IN", Kind::Signal, false },
        { "cutoff", "CUTOFF", Kind::Signal, false }, { "lp", "LP", Kind::Signal, true },
        { "bp", "BP", Kind::Signal, true }, { "hp", "HP", Kind::Signal, true } };
    const ParamDesc kSvfParams[] = {
        param("cutoff", "Cutoff", "Hz", 20, 20000, 1000, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<SvfNode>(s);
                return ok(pcl_svf_set_cutoff(&n->svf, to_hz(clamp_hz(v, n->sr))));
            }),
        param("resonance", "Resonance", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_svf_set_resonance(&as<SvfNode>(s)->svf, to_coef(v))); }),
        param("mod_depth", "Mod depth", "oct", 0, 4, 2, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_svf_set_mod_depth(&as<SvfNode>(s)->svf, to_oct(v))); }),
        param("saturate", "Saturate", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_svf_set_saturate(&as<SvfNode>(s)->svf, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kSvfParams) == SVF_COUNT);

    // ============================================================================================================
    // Biquad filter
    constexpr std::string_view kBiquadTypes[] = { "low-pass", "high-pass", "band-pass", "notch",
        "all-pass", "peak", "low shelf", "high shelf" };
    enum BiquadParam
    {
        BQ_TYPE,
        BQ_CUTOFF,
        BQ_Q,
        BQ_GAIN,
        BQ_DEPTH,
        BQ_COUNT
    };
    struct BiquadNode
    {
        pcl_biquad_t bq;
        uint32_t     sr;
    };
    bool biquad_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<BiquadNode>(s);
        n->sr = ctx.sample_rate;
        pcl_biquad_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.type = (pcl_biquad_type_t)to_u32(p[BQ_TYPE]);
        c.cutoff_hz = to_hz(clamp_hz(p[BQ_CUTOFF], ctx.sample_rate));
        c.q = to_quality(p[BQ_Q]);
        c.gain_db = to_db(p[BQ_GAIN]);
        c.mod_depth_oct = to_oct(p[BQ_DEPTH]);
        return ok(pcl_biquad_init(&n->bq, &c));
    }
    void biquad_reset(void *s) { pcl_biquad_reset(&as<BiquadNode>(s)->bq); }
    void biquad_process(void *s, NodeIo &io)
    {
        pcl_biquad_process(&as<BiquadNode>(s)->bq, io.sig_in[0] ? io.sig_in[0] : io.zeros,
            io.sig_out[0] ? io.sig_out[0] : io.scratch, io.sig_in[1], io.frames);
    }
    const PortDesc  kBiquadPorts[] = { { "in", "IN", Kind::Signal, false },
        { "cutoff", "CUTOFF", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kBiquadParams[] = {
        param(
            "type", "Type", "", 0, 7, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(
                    pcl_biquad_set_type(&as<BiquadNode>(s)->bq, (pcl_biquad_type_t)to_u32(v)));
            },
            kBiquadTypes),
        param("cutoff", "Cutoff", "Hz", 20, 20000, 1000, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<BiquadNode>(s);
                return ok(pcl_biquad_set_cutoff(&n->bq, to_hz(clamp_hz(v, n->sr))));
            }),
        param("q", "Q", "", 0.2f, 100, 0.707f, Scale::Log, Control::Knob, [](void *s, float v)
            { return ok(pcl_biquad_set_q(&as<BiquadNode>(s)->bq, to_quality(v))); }),
        param("gain", "Gain", "dB", -18, 18, 0, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_biquad_set_gain(&as<BiquadNode>(s)->bq, to_db(v))); }),
        param("mod_depth", "Mod depth", "oct", 0, 4, 2, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_biquad_set_mod_depth(&as<BiquadNode>(s)->bq, to_oct(v))); }),
    };
    static_assert(std::size(kBiquadParams) == BQ_COUNT);

    // ============================================================================================================
    // Clock
    enum ClockParam
    {
        CLK_BPM,
        CLK_PPQN,
        CLK_RUN,
        CLK_COUNT
    };
    struct ClockNode
    {
        pcl_clock_t clk;
    };
    bool clock_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto              *n = as<ClockNode>(s);
        pcl_clock_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.bpm = to_hz(p[CLK_BPM]);
        c.ppqn = to_u32(p[CLK_PPQN]);
        c.running = to_int(p[CLK_RUN]);
        return ok(pcl_clock_init(&n->clk, &c));
    }
    void clock_reset(void *s) { pcl_clock_reset(&as<ClockNode>(s)->clk); }
    void clock_process(void *s, NodeIo &io)
    {
        EventBuf &out = *io.evt_out[0];
        out.count = pcl_clock_process(&as<ClockNode>(s)->clk, io.frames, out.data, out.cap);
    }
    size_t clock_cap(size_t)
    {
        return 64;
    } // 256 samples at the shortest pulse period (5 samples) is 53 events
    const PortDesc  kClockPorts[] = { { "clock", "CLOCK", Kind::Event, true } };
    const ParamDesc kClockParams[] = {
        param("bpm", "Tempo", "bpm", 10, 999, 120, Scale::Log, Control::Knob, [](void *s, float v)
            { return ok(pcl_clock_set_bpm(&as<ClockNode>(s)->clk, to_hz(v))); }),
        param("ppqn", "Pulses per beat", "", 1, 96, 24, Scale::Int, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_clock_set_ppqn(&as<ClockNode>(s)->clk, to_u32(v))); }),
        param("run", "Run", "", 0, 1, 1, Scale::Toggle, Control::Button,
            [](void *s, float v)
            {
                auto *n = as<ClockNode>(s);
                if (v >= 0.5f)
                {
                    if (!pcl_clock_is_running(&n->clk))
                        pcl_clock_start(&n->clk);
                }
                else
                    pcl_clock_stop(&n->clk);
                return true;
            }),
    };
    static_assert(std::size(kClockParams) == CLK_COUNT);

    // ============================================================================================================
    // Arpeggiator
    constexpr std::string_view kArpModes[]
        = { "up", "down", "up/down", "up/down incl.", "random", "as played", "chord" };
    enum ArpParam
    {
        ARP_MODE,
        ARP_OCT,
        ARP_DIV,
        ARP_GATE,
        ARP_LATCH,
        ARP_COUNT
    };
    struct ArpNode
    {
        pcl_arp_t arp;
    };
    bool arp_init(void *s, const NodeContext &, const float *p)
    {
        pcl_arp_config_t c { };
        c.mode = (pcl_arp_mode_t)to_u32(p[ARP_MODE]);
        c.octaves = to_u32(p[ARP_OCT]);
        c.division = to_u32(p[ARP_DIV]);
        c.gate = to_u32(p[ARP_GATE]);
        c.latch = to_int(p[ARP_LATCH]);
        c.seed = 0;
        return ok(pcl_arp_init(&as<ArpNode>(s)->arp, &c));
    }
    void arp_reset(void *s) { pcl_arp_reset(&as<ArpNode>(s)->arp); }
    void arp_process(void *s, NodeIo &io)
    {
        const EventBuf &in = *io.evt_in[0];
        EventBuf       &out = *io.evt_out[0];
        out.count = pcl_arp_process(&as<ArpNode>(s)->arp, in.data, in.count, out.data, out.cap);
    }
    // A step makes at most 2 * PCL_ARP_MAX_NOTES events (a chord's releases and starts), a key
    // release at most PCL_ARP_MAX_NOTES.
    size_t arp_cap(size_t in_cap)
    {
        return std::clamp<size_t>(in_cap * 2 * PCL_ARP_MAX_NOTES, 64, 4096);
    }
    const PortDesc kArpPorts[]
        = { { "in", "IN", Kind::Event, false }, { "out", "OUT", Kind::Event, true } };
    const ParamDesc kArpParams[] = {
        param(
            "mode", "Mode", "", 0, 6, 0, Scale::Enum, Control::Selector, [](void *s, float v)
            { return ok(pcl_arp_set_mode(&as<ArpNode>(s)->arp, (pcl_arp_mode_t)to_u32(v))); },
            kArpModes),
        param("octaves", "Octaves", "", 1, 4, 1, Scale::Int, Control::Knob, [](void *s, float v)
            { return ok(pcl_arp_set_octaves(&as<ArpNode>(s)->arp, to_u32(v))); }),
        param("division", "Division", "pulses", 1, 96, 6, Scale::Int, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_arp_set_division(&as<ArpNode>(s)->arp, to_u32(v))); }),
        param("gate", "Gate", "pulses", 1, 96, 3, Scale::Int, Control::Knob,
            [](void *s, float v) { return ok(pcl_arp_set_gate(&as<ArpNode>(s)->arp, to_u32(v))); }),
        param("latch", "Latch", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_arp_set_latch(&as<ArpNode>(s)->arp, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kArpParams) == ARP_COUNT);

    // ============================================================================================================
    // Sequencer
    constexpr std::string_view kSeqDirections[] = { "forward", "backward", "ping-pong", "random" };
    enum SeqParam
    {
        SEQ_LENGTH,
        SEQ_DIV,
        SEQ_GATE,
        SEQ_SWING,
        SEQ_DIR,
        SEQ_TRANSPOSE,
        SEQ_COUNT
    };
    struct SeqNode
    {
        pcl_seq_t seq;
        uint32_t  division; // a copy, so that a swing is never set at or above it (the library
                            // refuses that)
    };
    bool seq_init(void *s, const NodeContext &, const float *p)
    {
        pcl_seq_config_t c { };
        c.length = to_u32(p[SEQ_LENGTH]);
        c.division = to_u32(p[SEQ_DIV]);
        c.gate = to_u32(p[SEQ_GATE]);
        c.swing = std::min(
            to_u32(p[SEQ_SWING]), c.division - 1); // as the swing setter does: below the division
        c.direction = (pcl_seq_direction_t)to_u32(p[SEQ_DIR]);
        c.transpose = to_int(p[SEQ_TRANSPOSE]);
        c.seed = 0;
        as<SeqNode>(s)->division = c.division;
        return ok(pcl_seq_init(&as<SeqNode>(s)->seq, &c));
    }
    void seq_reset(void *s) { pcl_seq_reset(&as<SeqNode>(s)->seq); }
    void seq_process(void *s, NodeIo &io)
    {
        const EventBuf &in = *io.evt_in[0];
        EventBuf       &out = *io.evt_out[0];
        out.count = pcl_seq_process(&as<SeqNode>(s)->seq, in.data, in.count, out.data, out.cap);
    }
    bool seq_apply_step(void *s, size_t index, const StepData &st)
    {
        pcl_seq_step_t step { st.note, st.velocity, st.gate, (uint8_t)(st.tie ? PCL_SEQ_TIE : 0) };
        return ok(pcl_seq_set_step(&as<SeqNode>(s)->seq, index, &step));
    }
    // A pulse makes at most 2 events, a START or STOP one; the clock input carries one of each per
    // pulse.
    size_t         seq_cap(size_t in_cap) { return std::max<size_t>(in_cap * 2 + 2, 64); }
    const PortDesc kSeqPorts[]
        = { { "in", "IN", Kind::Event, false }, { "out", "OUT", Kind::Event, true } };
    const ParamDesc kSeqParams[] = {
        param("length", "Length", "steps", 1, 64, 8, Scale::Int, Control::Knob, [](void *s, float v)
            { return ok(pcl_seq_set_length(&as<SeqNode>(s)->seq, to_u32(v))); }),
        param("division", "Division", "pulses", 1, 96, 6, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<SeqNode>(s);
                n->division = to_u32(v);
                return ok(pcl_seq_set_division(&n->seq, n->division));
            }),
        param("gate", "Gate", "pulses", 1, 96, 3, Scale::Int, Control::Knob,
            [](void *s, float v) { return ok(pcl_seq_set_gate(&as<SeqNode>(s)->seq, to_u32(v))); }),
        param("swing", "Swing", "pulses", 0, 95, 0, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<SeqNode>(s);
                return ok(pcl_seq_set_swing(&n->seq, std::min(to_u32(v), n->division - 1)));
            }),
        param(
            "direction", "Direction", "", 0, 3, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(
                    pcl_seq_set_direction(&as<SeqNode>(s)->seq, (pcl_seq_direction_t)to_u32(v)));
            },
            kSeqDirections),
        param("transpose", "Transpose", "semitones", -48, 48, 0, Scale::Int, Control::Knob,
            [](void *s, float v)
            {
                pcl_seq_set_transpose(&as<SeqNode>(s)->seq, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kSeqParams) == SEQ_COUNT);

} // namespace

#define RACK_DESC(                                                                                 \
    var, id, name, cat, hp, T, ports, params, steps, init, reset, process, apply_step, cap, blurb) \
    const ModuleDesc var = { id, name, cat, hp, sizeof(T), alignof(T), ports, params, steps, init, \
        reset, process, apply_step, cap, blurb }

RACK_DESC(kOscDesc, "osc", "Oscillator", kSource, 8, OscNode, kOscPorts, kOscParams, false,
    osc_init, osc_reset, osc_process, nullptr, nullptr,
    "Band-limited sine, triangle, saw and pulse; plays notes, with pitch, PWM and hard sync");
RACK_DESC(kNoiseDesc, "noise", "Noise", kSource, 6, NoiseNode, kNoisePorts, kNoiseParams, false,
    noise_init, noise_reset, noise_process, nullptr, nullptr, "White, pink or brown noise");
RACK_DESC(kSahDesc, "sah", "Sample & Hold", kSource, 6, SahNode, kSahPorts, kSahParams, false,
    sah_init, sah_reset, sah_process, nullptr, nullptr,
    "Samples and holds a random value on a trigger, a clock or its own rate");
RACK_DESC(kLfoDesc, "lfo", "LFO", kModulator, 8, LfoNode, kLfoPorts, kLfoParams, false, lfo_init,
    lfo_reset, lfo_process, nullptr, nullptr,
    "Slow oscillator for modulation, with random shapes and a rate input");
RACK_DESC(kAdsrDesc, "adsr", "ADSR", kModulator, 8, AdsrNode, kAdsrPorts, kAdsrParams, false,
    adsr_init, adsr_reset, adsr_process, nullptr, nullptr,
    "Attack-decay-sustain-release envelope from a gate or from notes");
RACK_DESC(kVcaDesc, "vca", "VCA", kModulator, 4, VcaNode, kVcaPorts, kVcaParams, false, vca_init,
    vca_reset, vca_process, nullptr, nullptr, "Amplifier whose gain a signal can control");
RACK_DESC(kSvfDesc, "svf", "State-variable filter", kFilter, 8, SvfNode, kSvfPorts, kSvfParams,
    false, svf_init, svf_reset, svf_process, nullptr, nullptr,
    "Resonant filter with low-, band- and high-pass outputs at once");
RACK_DESC(kBiquadDesc, "biquad", "Biquad filter", kFilter, 8, BiquadNode, kBiquadPorts,
    kBiquadParams, false, biquad_init, biquad_reset, biquad_process, nullptr, nullptr,
    "Second-order filter: low-, high- and band-pass, notch, all-pass, peak, shelves");
RACK_DESC(kClockDesc, "clock", "Clock", kEvent, 6, ClockNode, kClockPorts, kClockParams, false,
    clock_init, clock_reset, clock_process, nullptr, clock_cap,
    "Tempo clock: sends clock pulses, start and stop");
RACK_DESC(kArpDesc, "arp", "Arpeggiator", kEvent, 8, ArpNode, kArpPorts, kArpParams, false,
    arp_init, arp_reset, arp_process, nullptr, arp_cap,
    "Plays the held keys one after another, in time with a clock");
RACK_DESC(kSeqDesc, "seq", "Sequencer", kEvent, 16, SeqNode, kSeqPorts, kSeqParams, true, seq_init,
    seq_reset, seq_process, seq_apply_step, seq_cap,
    "Step sequencer, up to 64 steps of note, velocity, gate and tie");

uint32_t dropped_events(const ModuleDesc &desc, const void *state)
{
    if (&desc == &kArpDesc)
        return pcl_arp_dropped(&static_cast<const ArpNode *>(state)->arp);
    if (&desc == &kSeqDesc)
        return pcl_seq_dropped(&static_cast<const SeqNode *>(state)->seq);
    if (&desc == &kClockDesc)
        return pcl_clock_dropped(&static_cast<const ClockNode *>(state)->clk);
    return 0;
}

} // namespace rack
