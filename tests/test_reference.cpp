// The wrappers and the plan must add nothing and lose nothing: a patch rendered by the engine
// equals, sample for sample, the same modules called directly through PulseCoreLib's API with the
// same settings and the same blocks. The comparison is exact in every sample type.
#include "testutil.h"

using namespace rt_util;

namespace
{

constexpr size_t kBlock = PCL_MAX_BLOCK;

template <class F> void blocks(size_t total, F &&f)
{
    for (size_t done = 0; done < total;)
    {
        const size_t n = std::min(kBlock, total - done);
        f(done, n);
        done += n;
    }
}

bool equal_exact(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!(a[i] == b[i]))
            return false; // == so that -0 equals +0
    return true;
}

// the Audio Output stage as the engine does it: float conversion, level, clamp
float out_stage(pcl_sample_t s, float level)
{
    return std::clamp(0.0f + sample_to_float(s) * level, -1.0f, 1.0f);
}

// the oscillator wrapper's logic: the block is cut at each NOTE_ON, which retunes the oscillator
void ref_osc(pcl_osc_t &osc, pcl_sample_t *out, const pcl_sample_t *pitch, const pcl_event_t *ev,
    size_t nev, size_t frames, const pcl_sample_t *pwm = nullptr,
    const pcl_sample_t *sync_in = nullptr)
{
    size_t pos = 0, i = 0;
    while (pos < frames)
    {
        while (i < nev && ev[i].offset <= pos)
        {
            if (ev[i].type == PCL_EVT_NOTE_ON && ev[i].velocity > 0)
                (void)pcl_osc_set_frequency(&osc, pcl_midi_to_hz(ev[i].note));
            i++;
        }
        const size_t end = i < nev ? std::min<size_t>(frames, ev[i].offset) : frames;
        pcl_osc_process(&osc, out + pos, pitch ? pitch + pos : nullptr, pwm ? pwm + pos : nullptr,
            sync_in ? sync_in + pos : nullptr, nullptr, end - pos);
        pos = end;
    }
}

pcl_osc_t make_osc(pcl_osc_waveform_t wave, float hz, float depth)
{
    pcl_osc_t        o;
    pcl_osc_config_t c { };
    c.sample_rate_hz = kRate;
    c.waveform = wave;
    c.frequency = to_hz(hz);
    c.pitch_depth_oct = to_oct(depth);
    c.pulse_width = to_coef(0.5f);
    c.pwm_depth = to_coef(0.0f);
    CHECK(pcl_osc_init(&o, &c) == PCL_OK);
    return o;
}
pcl_adsr_t make_adsr(uint32_t a, uint32_t d, float s, uint32_t r, float ac, float dc, float rc)
{
    pcl_adsr_t        e;
    pcl_adsr_config_t c { };
    c.sample_rate_hz = kRate;
    c.attack_ms = a;
    c.decay_ms = d;
    c.release_ms = r;
    c.sustain = to_coef(s);
    c.attack_curve = to_coef(ac);
    c.decay_curve = to_coef(dc);
    c.release_curve = to_coef(rc);
    c.run_through = 0;
    CHECK(pcl_adsr_init(&e, &c) == PCL_OK);
    return e;
}
pcl_vca_t make_vca(float gain)
{
    pcl_vca_t        v;
    pcl_vca_config_t c { };
    c.sample_rate_hz = kRate;
    c.gain = to_coef(gain);
    CHECK(pcl_vca_init(&v, &c) == PCL_OK);
    return v;
}
pcl_clock_t make_clock(float bpm, uint32_t ppqn)
{
    pcl_clock_t        k;
    pcl_clock_config_t c { };
    c.sample_rate_hz = kRate;
    c.bpm = to_hz(bpm);
    c.ppqn = ppqn;
    c.running = 1;
    CHECK(pcl_clock_init(&k, &c) == PCL_OK);
    return k;
}

} // namespace

TEST(reference_osc_vca_out)
{
    Rig            r;
    const uint32_t osc = r.add("osc"), vca = r.add("vca"), out = r.add("out");
    r.param(osc, "waveform", 3);
    r.param(osc, "frequency", 330);
    r.param(osc, "pulse_width", 0.3f);
    r.param(vca, "gain", 0.7f);
    CHECK(r.cable(osc, "out", vca, "in") == Err::Ok && r.cable(vca, "out", out, "l") == Err::Ok
        && r.compile());
    const size_t total = 5000;
    r.render(total);

    pcl_osc_t        o;
    pcl_osc_config_t oc { };
    oc.sample_rate_hz = kRate;
    oc.waveform = PCL_OSC_PULSE;
    oc.frequency = to_hz(330);
    oc.pitch_depth_oct = to_oct(1.0);
    oc.pulse_width = to_coef(0.3f);
    oc.pwm_depth = to_coef(0.0);
    CHECK(pcl_osc_init(&o, &oc) == PCL_OK);
    pcl_vca_t          v = make_vca(0.7f);
    std::vector<float> L(total);
    blocks(total,
        [&](size_t at, size_t n)
        {
            pcl_sample_t a[kBlock], b[kBlock];
            pcl_osc_process(&o, a, nullptr, nullptr, nullptr, nullptr, n);
            pcl_vca_process(&v, a, b, nullptr, n);
            for (size_t i = 0; i < n; i++)
                L[at + i] = out_stage(b[i], 0.8f);
        });
    CHECK(rms(r.L) > 0.05);
    CHECK(equal_exact(r.L, L));
    CHECK(equal_exact(r.R, L)); // L alone plays on both channels
}

TEST(reference_noise_lfo_svf_out)
{
    Rig            r;
    const uint32_t nz = r.add("noise"), lfo = r.add("lfo"), svf = r.add("svf"), out = r.add("out");
    r.param(nz, "colour", 1);
    r.param(nz, "seed", 1234);
    r.param(lfo, "shape", 1);
    r.param(lfo, "frequency", 3);
    r.param(lfo, "unipolar", 0);
    r.param(svf, "cutoff", 1500);
    r.param(svf, "resonance", 0.6f);
    r.param(svf, "mod_depth", 2);
    CHECK(r.cable(nz, "out", svf, "in") == Err::Ok && r.cable(lfo, "out", svf, "cutoff") == Err::Ok
        && r.cable(svf, "bp", out, "l") == Err::Ok && r.cable(svf, "lp", out, "r") == Err::Ok
        && r.compile());
    const size_t total = 9000;
    r.render(total);

    pcl_noise_t        n;
    pcl_noise_config_t nc { };
    nc.sample_rate_hz = kRate;
    nc.colour = PCL_NOISE_PINK;
    nc.seed = 1234;
    CHECK(pcl_noise_init(&n, &nc) == PCL_OK);
    pcl_lfo_t        l;
    pcl_lfo_config_t lc { };
    lc.sample_rate_hz = kRate;
    lc.shape = PCL_LFO_TRIANGLE;
    lc.frequency = to_hz(3);
    lc.rate_depth_oct = to_oct(0);
    lc.pulse_width = to_coef(0.5);
    lc.unipolar = 0;
    lc.start_phase = 0;
    lc.seed = 0;
    CHECK(pcl_lfo_init(&l, &lc) == PCL_OK);
    pcl_svf_t        f;
    pcl_svf_config_t fc { };
    fc.sample_rate_hz = kRate;
    fc.cutoff_hz = to_hz(1500);
    fc.resonance = to_coef(0.6f);
    fc.mod_depth_oct = to_oct(2);
    fc.saturate = 0;
    CHECK(pcl_svf_init(&f, &fc) == PCL_OK);
    std::vector<float> L(total), R(total);
    blocks(total,
        [&](size_t at, size_t k)
        {
            pcl_sample_t in[kBlock], mod[kBlock], lp[kBlock], bp[kBlock];
            pcl_noise_process(&n, in, k);
            pcl_lfo_process(&l, mod, nullptr, k);
            pcl_svf_process(&f, in, lp, bp, nullptr, mod, k);
            for (size_t i = 0; i < k; i++)
            {
                L[at + i] = out_stage(bp[i], 0.8f);
                R[at + i] = out_stage(lp[i], 0.8f);
            }
        });
    CHECK(rms(r.L) > 0.001 && rms(r.R) > 0.001);
    CHECK(equal_exact(r.L, L));
    CHECK(equal_exact(r.R, R));
}

TEST(reference_clock_sequencer_adsr_osc)
{
    Rig            r;
    const uint32_t clk = r.add("clock"), seq = r.add("seq"), osc = r.add("osc"),
                   adsr = r.add("adsr"), vca = r.add("vca"), out = r.add("out");
    r.param(clk, "bpm", 133);
    r.param(seq, "length", 6);
    r.param(seq, "division", 6);
    r.param(seq, "gate", 4);
    r.param(seq, "swing", 2);
    r.param(seq, "direction", 2);
    r.param(seq, "transpose", 3);
    const StepData steps[6] = { { 45, 100, 0, 0 }, { 52, 90, 2, 0 }, { 57, 0, 0, 0 },
        { 60, 110, 0, 1 }, { 64, 80, 0, 0 }, { 40, 120, 5, 0 } };
    for (size_t i = 0; i < 6; i++)
        CHECK(r.e.patch().set_step(seq, i, steps[i]) == Err::Ok);
    r.param(osc, "waveform", 2);
    r.param(adsr, "attack", 5);
    r.param(adsr, "decay", 120);
    r.param(adsr, "sustain", 0.5f);
    r.param(adsr, "release", 90);
    r.param(vca, "gain", 1);
    CHECK(r.cable(clk, "clock", seq, "in") == Err::Ok && r.cable(seq, "out", osc, "note") == Err::Ok
        && r.cable(seq, "out", adsr, "note") == Err::Ok);
    CHECK(r.cable(osc, "out", vca, "in") == Err::Ok && r.cable(adsr, "out", vca, "gain") == Err::Ok
        && r.cable(vca, "out", out, "l") == Err::Ok);
    CHECK(r.compile());
    const size_t total = 3 * kRate;
    r.render(total);

    pcl_clock_t      k = make_clock(133, 24);
    pcl_seq_t        s;
    pcl_seq_config_t sc { };
    sc.length = 6;
    sc.division = 6;
    sc.gate = 4;
    sc.swing = 2;
    sc.direction = PCL_SEQ_PINGPONG;
    sc.transpose = 3;
    sc.seed = 0;
    CHECK(pcl_seq_init(&s, &sc) == PCL_OK);
    for (size_t i = 0; i < 6; i++)
    {
        pcl_seq_step_t st { steps[i].note, steps[i].velocity, steps[i].gate,
            (uint8_t)(steps[i].tie ? PCL_SEQ_TIE : 0) };
        CHECK(pcl_seq_set_step(&s, i, &st) == PCL_OK);
    }
    pcl_osc_t          o = make_osc(PCL_OSC_SAW, 220, 1);
    pcl_adsr_t         a = make_adsr(5, 120, 0.5f, 90, 0.3f, 0.3f, 0.3f);
    pcl_vca_t          v = make_vca(1);
    std::vector<float> L(total);
    blocks(total,
        [&](size_t at, size_t n)
        {
            pcl_event_t  cev[64], sev[512];
            const size_t nc = pcl_clock_process(&k, n, cev, 64);
            const size_t ns = pcl_seq_process(&s, cev, nc, sev, 512);
            pcl_sample_t wave[kBlock], env[kBlock], res[kBlock];
            ref_osc(o, wave, nullptr, sev, ns, n);
            pcl_adsr_process(&a, env, nullptr, sev, ns, n);
            pcl_vca_process(&v, wave, res, env, n);
            for (size_t i = 0; i < n; i++)
                L[at + i] = out_stage(res[i], 0.8f);
        });
    CHECK(rms(r.L) > 0.02);
    CHECK(equal_exact(r.L, L));
    CHECK(r.e.nonfinite_samples() == 0 && r.e.apply_failures() == 0);
}

TEST(reference_keys_merge_arpeggiator_voice)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), clk = r.add("clock"), mg = r.add("merge"),
                   arp = r.add("arp"), osc = r.add("osc"), adsr = r.add("adsr"), vca = r.add("vca"),
                   out = r.add("out");
    r.param(clk, "bpm", 140);
    r.param(arp, "mode", 2);
    r.param(arp, "octaves", 2);
    r.param(arp, "division", 6);
    r.param(arp, "gate", 4);
    r.param(osc, "waveform", 1);
    r.param(adsr, "attack", 3);
    r.param(adsr, "release", 60);
    CHECK(r.cable(kb, "notes", mg, "a") == Err::Ok && r.cable(clk, "clock", mg, "b") == Err::Ok
        && r.cable(mg, "out", arp, "in") == Err::Ok);
    CHECK(r.cable(arp, "out", osc, "note") == Err::Ok
        && r.cable(arp, "out", adsr, "note") == Err::Ok);
    CHECK(r.cable(osc, "out", vca, "in") == Err::Ok && r.cable(adsr, "out", vca, "gain") == Err::Ok
        && r.cable(vca, "out", out, "l") == Err::Ok);
    struct Key
    {
        double  t;
        bool    on;
        uint8_t note;
    };
    const Key keys[]
        = { { 0.10, true, 60 }, { 0.10, true, 64 }, { 0.12, true, 67 }, { 1.30, false, 64 },
              { 1.60, false, 60 }, { 2.0, true, 55 }, { 2.4, false, 67 }, { 2.9, false, 55 } };
    for (const Key &k : keys)
        r.key(kb, k.t, k.on, k.note);
    CHECK(r.compile());
    const size_t total = 3 * kRate;
    r.render(total);

    pcl_clock_t      ck = make_clock(140, 24);
    pcl_arp_t        ap;
    pcl_arp_config_t ac { };
    ac.mode = PCL_ARP_UPDOWN;
    ac.octaves = 2;
    ac.division = 6;
    ac.gate = 4;
    ac.latch = 0;
    ac.seed = 0;
    CHECK(pcl_arp_init(&ap, &ac) == PCL_OK);
    pcl_osc_t          o = make_osc(PCL_OSC_TRIANGLE, 220, 1);
    pcl_adsr_t         ad = make_adsr(3, 200, 0.7f, 60, 0.3f, 0.3f, 0.3f);
    pcl_vca_t          v = make_vca(1);
    std::vector<float> L(total);
    blocks(total,
        [&](size_t at, size_t n)
        {
            pcl_event_t kev[64], cev[64], mev[128], aev[4096];
            size_t      nk = 0;
            for (const Key &k :
                keys) // the keyboard module: events due in this block, at their offsets
            {
                const uint64_t when = (uint64_t)std::llround(k.t * kRate);
                if (when >= at && when < at + n)
                    kev[nk++] = pcl_event_t { (uint16_t)(when - at),
                        (uint8_t)(k.on ? PCL_EVT_NOTE_ON : PCL_EVT_NOTE_OFF), k.note,
                        (uint8_t)(k.on ? 100 : 0) };
            }
            const size_t nc = pcl_clock_process(&ck, n, cev, 64);
            size_t       i = 0, j = 0, m = 0; // merge: input A (keys) first at a tie
            while (i < nk || j < nc)
                mev[m++]
                    = (j >= nc || (i < nk && kev[i].offset <= cev[j].offset)) ? kev[i++] : cev[j++];
            const size_t na = pcl_arp_process(&ap, mev, m, aev, 4096);
            pcl_sample_t wave[kBlock], env[kBlock], res[kBlock];
            ref_osc(o, wave, nullptr, aev, na, n);
            pcl_adsr_process(&ad, env, nullptr, aev, na, n);
            pcl_vca_process(&v, wave, res, env, n);
            for (size_t q = 0; q < n; q++)
                L[at + q] = out_stage(res[q], 0.8f);
        });
    CHECK(rms(r.L) > 0.02);
    CHECK(equal_exact(r.L, L));
}

TEST(reference_sah_attenuverter_oscillator_biquad)
{
    Rig            r;
    const uint32_t clk = r.add("clock"), sah = r.add("sah"), att = r.add("attenuverter"),
                   osc = r.add("osc"), bq = r.add("biquad"), out = r.add("out");
    r.param(clk, "bpm", 240);
    r.param(sah, "rate", 0);
    r.param(sah, "slew", 20);
    r.param(sah, "seed", 9);
    r.param(att, "scale", 0.5f);
    r.param(att, "offset", 0.1f);
    r.param(osc, "waveform", 2);
    r.param(osc, "frequency", 200);
    r.param(osc, "pitch_depth", 2);
    r.param(bq, "type", 0);
    r.param(bq, "cutoff", 2500);
    r.param(bq, "q", 2);
    CHECK(r.cable(clk, "clock", sah, "clock") == Err::Ok
        && r.cable(sah, "out", att, "in") == Err::Ok
        && r.cable(att, "out", osc, "pitch") == Err::Ok);
    CHECK(r.cable(osc, "out", bq, "in") == Err::Ok && r.cable(bq, "out", out, "l") == Err::Ok
        && r.compile());
    const size_t total = 2 * kRate;
    r.render(total);

    pcl_clock_t      ck = make_clock(240, 24);
    pcl_sah_t        sh;
    pcl_sah_config_t sc { };
    sc.sample_rate_hz = kRate;
    sc.seed = 9;
    sc.rate_hz = to_hz(0);
    sc.slew_ms = 20;
    CHECK(pcl_sah_init(&sh, &sc) == PCL_OK);
    pcl_osc_t           o = make_osc(PCL_OSC_SAW, 200, 2);
    pcl_biquad_t        b;
    pcl_biquad_config_t bc { };
    bc.sample_rate_hz = kRate;
    bc.type = PCL_BIQUAD_LP;
    bc.cutoff_hz = to_hz(2500);
    bc.q = to_quality(2);
    bc.gain_db = to_db(0);
    bc.mod_depth_oct = to_oct(2);
    CHECK(pcl_biquad_init(&b, &bc) == PCL_OK);
    const pcl_coef_t   scale = to_coef(0.5f);
    const pcl_sample_t offset = to_sample(0.1f);
    std::vector<float> L(total);
    blocks(total,
        [&](size_t at, size_t n)
        {
            pcl_event_t  cev[64];
            const size_t nc = pcl_clock_process(&ck, n, cev, 64);
            pcl_sample_t rnd[kBlock], mod[kBlock], wave[kBlock], filt[kBlock];
            pcl_sah_process(&sh, rnd, nullptr, cev, nc, n);
            for (size_t i = 0; i < n; i++)
                mod[i] = sat_acc((wide_acc_t)mul_coef(rnd[i], scale) + (wide_acc_t)offset);
            ref_osc(o, wave, mod, nullptr, 0, n);
            pcl_biquad_process(&b, wave, filt, nullptr, n);
            for (size_t i = 0; i < n; i++)
                L[at + i] = out_stage(filt[i], 0.8f);
        });
    CHECK(rms(r.L) > 0.01);
    CHECK(equal_exact(r.L, L));
}

TEST(reference_oscillator_with_notes_pitch_pwm_and_sync_together)
{
    Rig            r;
    const uint32_t kb = r.add("keyboard"), lfa = r.add("lfo"), lfb = r.add("lfo"),
                   master = r.add("osc"), slave = r.add("osc"), out = r.add("out");
    r.param(lfa, "shape", 1);
    r.param(lfa, "frequency", 5);
    r.param(lfb, "shape", 0);
    r.param(lfb, "frequency", 0.7f);
    r.param(master, "waveform", 0);
    r.param(master, "frequency", 90);
    r.param(slave, "waveform", 3);
    r.param(slave, "frequency", 220);
    r.param(slave, "pitch_depth", 1.5f);
    r.param(slave, "pulse_width", 0.4f);
    r.param(slave, "pwm_depth", 0.3f);
    CHECK(r.cable(kb, "notes", slave, "note") == Err::Ok
        && r.cable(lfa, "out", slave, "pitch") == Err::Ok
        && r.cable(lfb, "out", slave, "pwm") == Err::Ok);
    CHECK(r.cable(master, "sync_out", slave, "sync") == Err::Ok
        && r.cable(slave, "out", out, "l") == Err::Ok);
    struct Key
    {
        double  t;
        uint8_t note;
    };
    const Key keys[] = { { 0.0301, 57 }, { 0.1273, 64 }, { 0.2117, 52 }, { 0.3, 69 },
        { 0.3004, 60 }, { 0.5551, 48 } };
    for (const Key &k : keys)
        r.key(kb, k.t, true, k.note);
    CHECK(r.compile());
    const size_t total = kRate;
    r.render(total);

    pcl_osc_t m = make_osc(PCL_OSC_SINE, 90, 1);
    pcl_osc_t sl;
    {
        pcl_osc_config_t c { };
        c.sample_rate_hz = kRate;
        c.waveform = PCL_OSC_PULSE;
        c.frequency = to_hz(220);
        c.pitch_depth_oct = to_oct(1.5f);
        c.pulse_width = to_coef(0.4f);
        c.pwm_depth = to_coef(0.3f);
        CHECK(pcl_osc_init(&sl, &c) == PCL_OK);
    }
    pcl_lfo_t la, lb;
    {
        pcl_lfo_config_t c { };
        c.sample_rate_hz = kRate;
        c.shape = PCL_LFO_TRIANGLE;
        c.frequency = to_hz(5);
        c.rate_depth_oct = to_oct(0);
        c.pulse_width = to_coef(0.5f);
        CHECK(pcl_lfo_init(&la, &c) == PCL_OK);
        c.shape = PCL_LFO_SINE;
        c.frequency = to_hz(0.7f);
        CHECK(pcl_lfo_init(&lb, &c) == PCL_OK);
    }
    std::vector<float> L(total);
    blocks(total,
        [&](size_t at, size_t n)
        {
            pcl_event_t kev[64];
            size_t      nk = 0;
            for (const Key &k : keys)
            {
                const uint64_t when = (uint64_t)std::llround(k.t * kRate);
                if (when >= at && when < at + n)
                    kev[nk++] = pcl_event_t { (uint16_t)(when - at), PCL_EVT_NOTE_ON, k.note, 100 };
            }
            pcl_sample_t unused[kBlock], sync[kBlock], pitch[kBlock], pwm[kBlock], wave[kBlock];
            pcl_osc_process(&m, unused, nullptr, nullptr, nullptr, sync, n);
            pcl_lfo_process(&la, pitch, nullptr, n);
            pcl_lfo_process(&lb, pwm, nullptr, n);
            ref_osc(sl, wave, pitch, kev, nk, n, pwm, sync);
            for (size_t i = 0; i < n; i++)
                L[at + i] = out_stage(wave[i], 0.8f);
        });
    CHECK(rms(r.L) > 0.05);
    CHECK(equal_exact(r.L, L));
}
