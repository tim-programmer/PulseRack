// Wrappers for the PulseCoreLib modules added after the first set: the ladder filter, the unison
// oscillator, the delay, the reverb and the sampler (docs/patch-engine.md). As in modules_lib.cpp,
// a module type is one descriptor and one wrapper. The delay, the reverb and the sampler run on
// memory their init (or sampler_load) allocates on the UI thread, and their descriptor's destroy
// frees.
#include "engine/model/wav.h"
#include "engine/rt/modules_common.h"

#include <cmath>
#include <cstdlib>

namespace rack
{
namespace
{

    constexpr std::string_view kSource = "Source", kFilter = "Filter", kEffect = "Effect";

    // ============================================================================================================
    // Ladder filter
    constexpr std::string_view kLadderModes[] = { "LP 24", "LP 18", "LP 12", "LP 6", "BP 12",
        "BP 24", "HP 12", "HP 24" };
    enum LadderParam
    {
        LAD_MODE,
        LAD_CUTOFF,
        LAD_RES,
        LAD_DRIVE,
        LAD_DEPTH,
        LAD_SAT,
        LAD_BASS,
        LAD_COUNT
    };
    struct LadderNode
    {
        pcl_ladder_t ld;
        uint32_t     sr;
    };
    bool ladder_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<LadderNode>(s);
        n->sr = ctx.sample_rate;
        pcl_ladder_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.cutoff_hz = to_hz(clamp_hz(p[LAD_CUTOFF], ctx.sample_rate));
        c.resonance = to_coef(p[LAD_RES]);
        c.mod_depth_oct = to_oct(p[LAD_DEPTH]);
        c.mode = (pcl_ladder_mode_t)to_u32(p[LAD_MODE]);
        c.saturate = to_int(p[LAD_SAT]);
        c.drive = PCL_WIDE(p[LAD_DRIVE]);
        c.bass_comp = to_int(p[LAD_BASS]);
        return ok(pcl_ladder_init(&n->ld, &c));
    }
    void ladder_reset(void *s) { pcl_ladder_reset(&as<LadderNode>(s)->ld); }
    void ladder_process(void *s, NodeIo &io)
    {
        pcl_ladder_process(&as<LadderNode>(s)->ld, io.sig_in[0] ? io.sig_in[0] : io.zeros,
            io.sig_out[0] ? io.sig_out[0] : io.scratch, io.sig_in[1], io.frames);
    }
    const PortDesc kLadderPorts[] = { { "in", "IN", Kind::Signal, false },
        { "cutoff", "CUTOFF", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    // (resonance and drive stop a little inside the library's 1.1 and 8, as the oscillator's pulse
    // width does: a float limit is a hair above the library's once converted to fixed point)
    const ParamDesc kLadderParams[] = {
        param(
            "mode", "Mode", "", 0, 7, 0, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(pcl_ladder_set_mode(
                    &as<LadderNode>(s)->ld, (pcl_ladder_mode_t)to_u32(v)));
            },
            kLadderModes),
        param("cutoff", "Cutoff", "Hz", 20, 20000, 1000, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<LadderNode>(s);
                return ok(pcl_ladder_set_cutoff(&n->ld, to_hz(clamp_hz(v, n->sr))));
            }),
        param("resonance", "Resonance", "", 0, 1.08f, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_ladder_set_resonance(&as<LadderNode>(s)->ld, to_coef(v))); }),
        param("drive", "Drive", "", 1, 7.9f, 1, Scale::Log, Control::Knob, [](void *s, float v)
            { return ok(pcl_ladder_set_drive(&as<LadderNode>(s)->ld, PCL_WIDE(v))); }),
        param("mod_depth", "Mod depth", "oct", 0, 4, 2, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_ladder_set_mod_depth(&as<LadderNode>(s)->ld, to_oct(v))); }),
        param("saturate", "Saturate", "", 0, 1, 1, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_ladder_set_saturate(&as<LadderNode>(s)->ld, to_int(v));
                return true;
            }),
        param("bass_comp", "Keep bass", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_ladder_set_bass_comp(&as<LadderNode>(s)->ld, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kLadderParams) == LAD_COUNT);

    // ============================================================================================================
    // Unison oscillator
    constexpr std::string_view kUniWaves[] = { "sine", "triangle", "saw", "pulse" };
    enum UniParam
    {
        UNI_WAVE,
        UNI_FREQ,
        UNI_VOICES,
        UNI_DETUNE,
        UNI_WIDTH,
        UNI_PITCH_DEPTH,
        UNI_PW,
        UNI_PWM_DEPTH,
        UNI_COUNT
    };
    struct UniNode
    {
        pcl_unison_t u;
        uint32_t     sr;
    };
    bool uni_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<UniNode>(s);
        n->sr = ctx.sample_rate;
        pcl_unison_config_t c { };
        c.osc.sample_rate_hz = ctx.sample_rate;
        c.osc.waveform = (pcl_osc_waveform_t)to_u32(p[UNI_WAVE]);
        c.osc.frequency = to_hz(clamp_hz(p[UNI_FREQ], ctx.sample_rate));
        c.osc.pitch_depth_oct = to_oct(p[UNI_PITCH_DEPTH]);
        c.osc.pulse_width = to_coef(p[UNI_PW]);
        c.osc.pwm_depth = to_coef(p[UNI_PWM_DEPTH]);
        c.voices = to_u32(p[UNI_VOICES]);
        c.detune_oct = to_oct(p[UNI_DETUNE] / 1200.0);
        c.stereo_width = to_coef(p[UNI_WIDTH]);
        c.seed = 0;
        return ok(pcl_unison_init(&n->u, &c));
    }
    void uni_reset(void *s) { pcl_unison_reset(&as<UniNode>(s)->u); }
    // Notes retune it on their own sample, as the Oscillator's do. L alone is the mono mix; with R
    // patched the voices are spread across the two.
    void uni_process(void *s, NodeIo &io)
    {
        auto               *n = as<UniNode>(s);
        pcl_sample_t       *l = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        pcl_sample_t       *r = io.sig_out[1];
        const pcl_sample_t *pitch = io.sig_in[0];
        const pcl_sample_t *pwm = io.sig_in[1];
        const pcl_sample_t *sync = io.sig_in[2];
        const EventBuf     &ev = *io.evt_in[0];
        size_t              pos = 0, i = 0;
        while (pos < io.frames)
        {
            while (i < ev.count && ev.data[i].offset <= pos)
            {
                if (ev.data[i].type == PCL_EVT_NOTE_ON && ev.data[i].velocity > 0)
                    (void)pcl_unison_set_frequency(&n->u, pcl_midi_to_hz(ev.data[i].note));
                i++;
            }
            const size_t end
                = i < ev.count ? std::min<size_t>(io.frames, ev.data[i].offset) : io.frames;
            pcl_unison_process(&n->u, l + pos, r ? r + pos : nullptr, pitch ? pitch + pos : nullptr,
                pwm ? pwm + pos : nullptr, sync ? sync + pos : nullptr, end - pos);
            pos = end;
        }
    }
    const PortDesc kUniPorts[] = {
        { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false },
        { "pwm", "PWM", Kind::Signal, false },
        { "sync", "SYNC", Kind::Signal, false },
        { "l", "L", Kind::Signal, true },
        { "r", "R", Kind::Signal, true },
    };
    const ParamDesc kUniParams[] = {
        param(
            "waveform", "Waveform", "", 0, 3, 2, Scale::Enum, Control::Selector,
            [](void *s, float v)
            {
                return ok(pcl_unison_set_waveform(
                    &as<UniNode>(s)->u, (pcl_osc_waveform_t)to_u32(v)));
            },
            kUniWaves),
        param("frequency", "Frequency", "Hz", 20, 8000, 220, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<UniNode>(s);
                return ok(pcl_unison_set_frequency(&n->u, to_hz(clamp_hz(v, n->sr))));
            }),
        param("voices", "Voices", "", 1, PCL_UNISON_MAX_VOICES, 5, Scale::Int, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_voices(&as<UniNode>(s)->u, to_u32(v))); }),
        param("detune", "Detune", "cents", 0, 100, 20, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_detune(&as<UniNode>(s)->u, to_oct(v / 1200.0))); }),
        param("width", "Stereo width", "", 0, 1, 0.8f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_stereo_width(&as<UniNode>(s)->u, to_coef(v))); }),
        param("pitch_depth", "Pitch depth", "oct", 0, 4, 1, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_pitch_depth(&as<UniNode>(s)->u, to_oct(v))); }),
        param("pulse_width", "Pulse width", "", 0.02f, 0.98f, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_pulse_width(&as<UniNode>(s)->u, to_coef(v))); }),
        param("pwm_depth", "PWM depth", "", 0, 0.48f, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_unison_set_pwm_depth(&as<UniNode>(s)->u, to_coef(v))); }),
    };
    static_assert(std::size(kUniParams) == UNI_COUNT);

    // ============================================================================================================
    // Delay
    constexpr uint32_t kDelayMaxMs = 2000;
    enum DelayParam
    {
        DLY_TIME,
        DLY_FEEDBACK,
        DLY_TONE,
        DLY_MIX,
        DLY_DEPTH,
        DLY_GLIDE,
        DLY_SAT,
        DLY_COUNT
    };
    struct DelayNode
    {
        pcl_delay_t dl;
        void       *line; // from init, freed by destroy
        uint32_t    sr;
    };
    bool delay_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<DelayNode>(s);
        n->sr = ctx.sample_rate;
        pcl_delay_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.max_delay_ms = kDelayMaxMs;
        c.time_ms = PCL_MS(p[DLY_TIME]);
        c.mod_depth_ms = PCL_MS(p[DLY_DEPTH]);
        c.glide_ms = to_u32(p[DLY_GLIDE]);
        c.feedback = to_coef(p[DLY_FEEDBACK]);
        c.tone_hz = to_hz(clamp_hz(p[DLY_TONE], ctx.sample_rate));
        c.saturate = to_int(p[DLY_SAT]);
        c.mix = to_coef(p[DLY_MIX]);
        c.interp = PCL_DELAY_HERMITE; // the time is meant to be modulated
        c.storage = PCL_DELAY_STORE_NATIVE;
        const size_t bytes = pcl_delay_required_bytes(&c);
        if (bytes == 0 || !(n->line = std::malloc(bytes)))
            return false;
        return ok(pcl_delay_init(&n->dl, &c, n->line, bytes));
    }
    void delay_reset(void *s) { pcl_delay_reset(&as<DelayNode>(s)->dl); }
    void delay_destroy(void *s) { std::free(as<DelayNode>(s)->line); }
    void delay_process(void *s, NodeIo &io)
    {
        pcl_delay_process(&as<DelayNode>(s)->dl, io.sig_in[0] ? io.sig_in[0] : io.zeros,
            io.sig_out[0] ? io.sig_out[0] : io.scratch, io.sig_in[1], io.frames);
    }
    const PortDesc kDelayPorts[] = { { "in", "IN", Kind::Signal, false },
        { "time", "TIME", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kDelayParams[] = {
        param("time", "Time", "ms", 1, kDelayMaxMs, 350, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_delay_set_time(&as<DelayNode>(s)->dl, PCL_MS(v))); }),
        param("feedback", "Feedback", "", -0.98f, 0.98f, 0.4f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_delay_set_feedback(&as<DelayNode>(s)->dl, to_coef(v))); }),
        param("tone", "Tone", "Hz", 200, 20000, 8000, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<DelayNode>(s);
                return ok(pcl_delay_set_tone(&n->dl, to_hz(clamp_hz(v, n->sr))));
            }),
        param("mix", "Mix", "", 0, 1, 0.35f, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_delay_set_mix(&as<DelayNode>(s)->dl, to_coef(v))); }),
        param("mod_depth", "Mod depth", "ms", 0, 50, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_delay_set_mod_depth(&as<DelayNode>(s)->dl, PCL_MS(v))); }),
        param("glide", "Glide", "ms", 0, 2000, 50, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_delay_set_glide(&as<DelayNode>(s)->dl, to_u32(v))); }),
        param("saturate", "Saturate", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_delay_set_saturate(&as<DelayNode>(s)->dl, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kDelayParams) == DLY_COUNT);

    // ============================================================================================================
    // Reverb
    constexpr std::string_view kReverbKinds[] = { "small", "quality" };
    constexpr uint32_t         kReverbMaxPredelayMs = 200;
    enum ReverbParam
    {
        RV_KIND,
        RV_SIZE,
        RV_DECAY,
        RV_PREDELAY,
        RV_DAMP,
        RV_DAMP_HZ,
        RV_WIDTH,
        RV_MIX,
        RV_MOD,
        RV_FREEZE,
        RV_COUNT
    };
    struct ReverbNode
    {
        pcl_reverb_t rv;
        void        *mem; // from init, freed by destroy
        uint32_t     sr;
        float        damp, damp_hz; // the damping setter takes both
    };
    bool reverb_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<ReverbNode>(s);
        n->sr = ctx.sample_rate;
        n->damp = p[RV_DAMP];
        n->damp_hz = p[RV_DAMP_HZ];
        pcl_reverb_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.variant = (pcl_reverb_variant_t)to_u32(p[RV_KIND]);
        c.size = to_coef(p[RV_SIZE]);
        c.max_predelay_ms = kReverbMaxPredelayMs;
        c.predelay_ms = PCL_MS(p[RV_PREDELAY]);
        c.rt60_ms = PCL_MS(p[RV_DECAY] * 1000.0);
        c.hf_ratio = to_coef(p[RV_DAMP]);
        c.hf_hz = to_hz(clamp_hz(p[RV_DAMP_HZ], ctx.sample_rate));
        c.width = to_coef(p[RV_WIDTH]);
        c.mix = to_coef(p[RV_MIX]);
        c.mod_depth = to_coef(p[RV_MOD]);
        c.freeze = to_int(p[RV_FREEZE]);
        c.storage = PCL_DELAY_STORE_NATIVE;
        const size_t bytes = pcl_reverb_required_bytes(&c);
        if (bytes == 0 || !(n->mem = std::malloc(bytes)))
            return false;
        return ok(pcl_reverb_init(&n->rv, &c, n->mem, bytes));
    }
    void reverb_reset(void *s) { pcl_reverb_reset(&as<ReverbNode>(s)->rv); }
    void reverb_destroy(void *s) { std::free(as<ReverbNode>(s)->mem); }
    // Mono in, stereo out: L alone gets the mono sum, as the Audio Output plays L on both sides.
    void reverb_process(void *s, NodeIo &io)
    {
        pcl_sample_t *l = io.sig_out[0] ? io.sig_out[0] : io.scratch;
        pcl_reverb_process(&as<ReverbNode>(s)->rv, io.sig_in[0] ? io.sig_in[0] : io.zeros, l,
            io.sig_out[1], io.frames);
    }
    bool reverb_set_damping(ReverbNode *n)
    {
        return ok(pcl_reverb_set_damping(
            &n->rv, to_coef(n->damp), to_hz(clamp_hz(n->damp_hz, n->sr))));
    }
    const PortDesc kReverbPorts[] = { { "in", "IN", Kind::Signal, false },
        { "l", "L", Kind::Signal, true }, { "r", "R", Kind::Signal, true } };
    const ParamDesc kReverbParams[] = {
        rebuild_param("algorithm", "Algorithm", "", 0, 1, 1, Scale::Enum, Control::Selector,
            kReverbKinds),
        rebuild_param("size", "Size", "", 0.25f, 1, 0.7f, Scale::Linear, Control::Knob),
        param("decay", "Decay", "s", 0.1f, 30, 2.5f, Scale::Log, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_reverb_set_rt60(&as<ReverbNode>(s)->rv, PCL_MS(v * 1000.0))); }),
        param("predelay", "Pre-delay", "ms", 0, kReverbMaxPredelayMs, 10, Scale::Linear,
            Control::Knob,
            [](void *s, float v)
            { return ok(pcl_reverb_set_predelay(&as<ReverbNode>(s)->rv, PCL_MS(v))); }),
        param("damping", "Treble decay", "", 0.1f, 1, 0.5f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<ReverbNode>(s);
                n->damp = v;
                return reverb_set_damping(n);
            }),
        param("damping_hz", "Treble from", "Hz", 1000, 16000, 5000, Scale::Log, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<ReverbNode>(s);
                n->damp_hz = v;
                return reverb_set_damping(n);
            }),
        param("width", "Width", "", 0, 1, 1, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_reverb_set_width(&as<ReverbNode>(s)->rv, to_coef(v))); }),
        param("mix", "Mix", "", 0, 1, 0.3f, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_reverb_set_mix(&as<ReverbNode>(s)->rv, to_coef(v))); }),
        param("modulation", "Modulation", "", 0, 1, 0.3f, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_reverb_set_mod_depth(&as<ReverbNode>(s)->rv, to_coef(v))); }),
        param("freeze", "Freeze", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_reverb_set_freeze(&as<ReverbNode>(s)->rv, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kReverbParams) == RV_COUNT);

    // ============================================================================================================
    // Sampler: plays the WAV file the patch names (Node::sample), which the engine reads and hands
    // over with sampler_load after init. Until then (or with no file) it plays two silent frames.
    // The root note, loop mode and loop points are part of the library's sample description, which
    // a sounding note reads: changing them makes the module again (rebuild) rather than changing
    // the description under a note.
    constexpr std::string_view kLoopModes[] = { "one shot", "forward", "ping-pong", "sustain" };
    enum SamplerParam
    {
        SMP_LOOP,
        SMP_ROOT,
        SMP_LOOP_START,
        SMP_LOOP_END,
        SMP_START,
        SMP_GAIN,
        SMP_VELOCITY,
        SMP_PITCH_DEPTH,
        SMP_REVERSE,
        SMP_COUNT
    };
    const pcl_sample_t kSilence[2] = { };
    struct SamplerNode
    {
        pcl_sampler_t        s;
        pcl_sampler_sample_t smp;
        pcl_sample_t        *data; // from sampler_load, freed by destroy
        float                loop, root, loop_start, loop_end, start;
    };
    // The description of `frames` at `rate`, from the settings (loop points as fractions of the
    // sample, put in order and at least 2 frames apart, or no loop for a sample that short).
    void describe(SamplerNode *n, const pcl_sample_t *data, uint32_t frames, uint32_t rate)
    {
        pcl_sampler_sample_t d { };
        d.data = data;
        d.frames = frames;
        d.format = PCL_SAMPLER_NATIVE;
        d.sample_rate_hz = rate;
        d.root_note = (uint8_t)to_u32(n->root);
        d.loop_mode = (pcl_sampler_loop_t)to_u32(n->loop);
        uint32_t a = (uint32_t)std::floor(std::min(n->loop_start, n->loop_end) * (float)frames),
                 b = (uint32_t)std::ceil(std::max(n->loop_start, n->loop_end) * (float)frames);
        b = std::min(b, frames);
        if (frames < 2)
            d.loop_mode = PCL_SAMPLER_ONE_SHOT;
        else if (b - std::min(a, b) < 2)
        {
            a = std::min(a, frames - 2);
            b = a + 2;
        }
        d.loop_start = a;
        d.loop_end = b;
        n->smp = d;
    }
    uint32_t start_frames(const SamplerNode *n)
    {
        const uint32_t f = n->smp.frames;
        return std::min(f - 1, (uint32_t)(n->start * (float)f));
    }
    bool sampler_init(void *s, const NodeContext &ctx, const float *p)
    {
        auto *n = as<SamplerNode>(s);
        n->loop = p[SMP_LOOP];
        n->root = p[SMP_ROOT];
        n->loop_start = p[SMP_LOOP_START];
        n->loop_end = p[SMP_LOOP_END];
        n->start = p[SMP_START];
        describe(n, kSilence, 2, ctx.sample_rate);
        pcl_sampler_config_t c { };
        c.sample_rate_hz = ctx.sample_rate;
        c.sample = &n->smp;
        c.interp = PCL_SAMPLER_HERMITE;
        c.pitch_depth_oct = to_oct(p[SMP_PITCH_DEPTH]);
        c.gain = to_coef(p[SMP_GAIN]);
        c.velocity_sens = to_coef(p[SMP_VELOCITY]);
        c.start_offset = start_frames(n);
        c.reverse = to_int(p[SMP_REVERSE]);
        c.declick_ms = 3;
        return ok(pcl_sampler_init(&n->s, &c));
    }
    void sampler_reset(void *s) { pcl_sampler_reset(&as<SamplerNode>(s)->s); }
    void sampler_destroy(void *s) { std::free(as<SamplerNode>(s)->data); }
    void sampler_process(void *s, NodeIo &io)
    {
        const EventBuf &ev = *io.evt_in[0];
        pcl_sampler_process(&as<SamplerNode>(s)->s, io.sig_out[0] ? io.sig_out[0] : io.scratch,
            io.sig_in[0], ev.data, ev.count, io.frames);
    }
    const PortDesc kSamplerPorts[] = { { "note", "NOTE", Kind::Event, false },
        { "pitch", "PITCH", Kind::Signal, false }, { "out", "OUT", Kind::Signal, true } };
    const ParamDesc kSamplerParams[] = {
        rebuild_param("loop", "Loop", "", 0, 3, 0, Scale::Enum, Control::Selector, kLoopModes),
        rebuild_param("root", "Root note", "note", 0, 127, 60, Scale::Int, Control::Knob),
        rebuild_param("loop_start", "Loop start", "", 0, 1, 0, Scale::Linear, Control::Knob),
        rebuild_param("loop_end", "Loop end", "", 0, 1, 1, Scale::Linear, Control::Knob),
        param("start", "Start", "", 0, 0.99f, 0, Scale::Linear, Control::Knob,
            [](void *s, float v)
            {
                auto *n = as<SamplerNode>(s);
                n->start = v;
                return ok(pcl_sampler_set_start_offset(&n->s, start_frames(n)));
            }),
        param("gain", "Gain", "", 0, 1.9f, 1, Scale::Linear, Control::Knob, [](void *s, float v)
            { return ok(pcl_sampler_set_gain(&as<SamplerNode>(s)->s, to_coef(v))); }),
        param("velocity", "Velocity", "", 0, 1, 1, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_sampler_set_velocity_sens(&as<SamplerNode>(s)->s, to_coef(v))); }),
        param("pitch_depth", "Pitch depth", "oct", 0, 4, 1, Scale::Linear, Control::Knob,
            [](void *s, float v)
            { return ok(pcl_sampler_set_pitch_depth(&as<SamplerNode>(s)->s, to_oct(v))); }),
        param("reverse", "Reverse", "", 0, 1, 0, Scale::Toggle, Control::Switch,
            [](void *s, float v)
            {
                pcl_sampler_set_reverse(&as<SamplerNode>(s)->s, to_int(v));
                return true;
            }),
    };
    static_assert(std::size(kSamplerParams) == SMP_COUNT);

} // namespace

RACK_DESC(kLadderDesc, "ladder", "Ladder filter", kFilter, 10, LadderNode, kLadderPorts,
    kLadderParams, false, ladder_init, ladder_reset, ladder_process, nullptr, nullptr,
    "Four-pole ladder filter: warm low-pass that can scream, and seven other responses");
RACK_DESC(kUnisonDesc, "unison", "Unison oscillator", kSource, 12, UniNode, kUniPorts,
    kUniParams, false, uni_init, uni_reset, uni_process, nullptr, nullptr,
    "Up to eight detuned oscillators as one thick voice, mono or spread in stereo");
RACK_DESC_D(kDelayDesc, "delay", "Delay", kEffect, 10, DelayNode, kDelayPorts, kDelayParams,
    delay_init, delay_reset, delay_process, delay_destroy,
    "Echo up to 2 s with feedback and tone; modulate the time for chorus and flanging");
RACK_DESC_D(kReverbDesc, "reverb", "Reverb", kEffect, 12, ReverbNode, kReverbPorts,
    kReverbParams, reverb_init, reverb_reset, reverb_process, reverb_destroy,
    "Mono in, stereo out: a small room or a lush hall, with pre-delay and freeze");
RACK_DESC_D(kSamplerDesc, "sampler", "Sampler", kSource, 12, SamplerNode, kSamplerPorts,
    kSamplerParams, sampler_init, sampler_reset, sampler_process, sampler_destroy,
    "Plays a WAV file at the pitch of each note: one shot, or looped while a key is held");

bool takes_sample(const ModuleDesc &desc)
{
    return &desc == &kSamplerDesc || &desc == &kGrainVoiceDesc;
}

namespace
{
    bool sampler_load(void *state, const SampleData &d);
}

bool load_sample(const ModuleDesc &desc, void *state, const SampleData &data)
{
    if (&desc == &kSamplerDesc)
        return sampler_load(state, data);
    if (&desc == &kGrainVoiceDesc)
        return grain_load(state, data);
    return false;
}

namespace
{
bool sampler_load(void *state, const SampleData &d)
{
    auto *n = as<SamplerNode>(state);
    if (d.frames.empty() || d.frames.size() > kMaxSampleFrames)
        return false;
    auto *data = static_cast<pcl_sample_t *>(std::malloc(d.frames.size() * sizeof(pcl_sample_t)));
    if (!data)
        return false;
    for (size_t i = 0; i < d.frames.size(); i++)
        data[i] = to_sample(std::clamp(d.frames[i], -1.f, 1.f));
    const pcl_sampler_sample_t old = n->smp;
    describe(n, data, (uint32_t)d.frames.size(), d.rate);
    if (!ok(pcl_sampler_set_sample(&n->s, &n->smp)))
    { // (the library refused it: what was there stays)
        n->smp = old;
        std::free(data);
        return false;
    }
    (void)pcl_sampler_set_start_offset(&n->s, start_frames(n)); // (below the frames: accepted)
    std::free(n->data); // (a second load into the same state)
    n->data = data;
    return true;
}
} // namespace

} // namespace rack
