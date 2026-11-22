// A minimal CLAP host for pulserack.clap (docs/reaper.md, *Level 3*): it loads the built plugin as a DAW
// does, through its entry and factory, and checks what a host relies on. `clap_host path/to/pulserack.clap`
//
// * the descriptor, the ports (no audio in, one stereo out, one note input);
// * a fresh instance is silent until a note comes, and a note sounds from its own sample (CLAP note
//   events and MIDI 1.0 bytes alike), across the engine's block boundaries;
// * the state is the patch file's text; loaded into another instance it plays the same, bit for bit;
// * a patch that plays itself (examples/sequence.rack) sounds with no notes; a bad state is refused
//   and the old patch kept.
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

int failures = 0;
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------- the host

// clap.timer-support: the timers the plugin registered; the test fires them itself
std::vector<clap_id> timers;
bool host_register_timer(const clap_host_t *, uint32_t, clap_id *id)
{
    *id = (clap_id)timers.size() + 100;
    timers.push_back(*id);
    return true;
}
bool host_unregister_timer(const clap_host_t *, clap_id id)
{
    std::erase(timers, id);
    return true;
}
const clap_host_timer_support_t kTimerSupport = { host_register_timer, host_unregister_timer };

const void *host_get_extension(const clap_host_t *, const char *id)
{
    return !std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) ? &kTimerSupport : nullptr;
}
void        host_request(const clap_host_t *) { }

const clap_host_t kHost = { CLAP_VERSION_INIT, nullptr, "clap_host test", "PulseRack", "", "1",
    host_get_extension, host_request, host_request, host_request };

// the events of one process call
struct Events
{
    std::vector<std::vector<unsigned char>> list;
    clap_input_events_t                     in { this, size, get };

    static uint32_t size(const clap_input_events_t *l)
    {
        return (uint32_t) static_cast<const Events *>(l->ctx)->list.size();
    }
    static const clap_event_header_t *get(const clap_input_events_t *l, uint32_t i)
    {
        return reinterpret_cast<const clap_event_header_t *>(
            static_cast<const Events *>(l->ctx)->list[i].data());
    }
    void note(uint32_t time, bool on, int16_t key, double velocity = 0.8)
    {
        clap_event_note_t e { };
        e.header = { sizeof e, time, CLAP_CORE_EVENT_SPACE_ID,
            (uint16_t)(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
        e.note_id = -1;
        e.port_index = 0;
        e.channel = 0;
        e.key = key;
        e.velocity = velocity;
        add(&e, sizeof e);
    }
    void midi(uint32_t time, unsigned char a, unsigned char b, unsigned char c)
    {
        clap_event_midi_t e { };
        e.header = { sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        e.port_index = 0;
        e.data[0] = a;
        e.data[1] = b;
        e.data[2] = c;
        add(&e, sizeof e);
    }
    void add(const void *e, size_t n)
    {
        const auto *b = static_cast<const unsigned char *>(e);
        list.emplace_back(b, b + n);
    }
};

const clap_output_events_t kNoOut = { nullptr,
    [](const clap_output_events_t *, const clap_event_header_t *) { return true; } };

// what the plugin sends: MIDI messages, with the time (in the whole run) each came at
struct Sent
{
    uint64_t      at;
    unsigned char b0, b1, b2;
};

struct Instance
{
    const clap_plugin_t *p = nullptr;
    int64_t              steady = 0;
    std::vector<Sent>    sent;
    // what a run gives besides notes: the transport (the beat moves on while playing), the audio
    // in (PulseRack FX), and what came out of Output 2 to 8 (left channels)
    bool                            transport = false, playing = false;
    double                          tempo = 120, beat = 0;
    const std::vector<float>       *in = nullptr; // from the run's first sample
    std::vector<std::vector<float>> extra = std::vector<std::vector<float>>(7);

    explicit Instance(const clap_plugin_factory_t *f, const char *id)
    {
        p = f->create_plugin(f, &kHost, id);
        CHECK(p && p->init(p));
        CHECK(p && p->activate(p, 48000, 1, 4096));
        CHECK(p && p->start_processing(p));
    }
    ~Instance()
    {
        if (!p)
            return;
        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }
    // `frames` of sound, in calls of `block` frames; `events` go into the first call (their times
    // in it)
    void run(std::vector<float> &l, std::vector<float> &r, size_t frames, size_t block,
        Events *events = nullptr)
    {
        for (size_t done = 0; done < frames;)
        {
            const uint32_t           n = (uint32_t)std::min(block, frames - done);
            std::vector<float>       bl(n), br(n);
            float                   *chans[2] = { bl.data(), br.data() };
            std::vector<std::vector<float>> xb(14, std::vector<float>(n));
            float                   *xc[14];
            clap_audio_buffer_t      outs[8];
            outs[0] = { chans, nullptr, 2, 0, 0 };
            for (int k = 0; k < 7; k++)
            {
                xc[2 * k] = xb[(size_t)(2 * k)].data();
                xc[2 * k + 1] = xb[(size_t)(2 * k + 1)].data();
                outs[k + 1] = { &xc[2 * k], nullptr, 2, 0, 0 };
            }
            std::vector<float>  il(n), ir(n);
            float              *ic[2] = { il.data(), ir.data() };
            clap_audio_buffer_t inb { ic, nullptr, 2, 0, 0 };
            if (in)
                for (uint32_t i = 0; i < n; i++)
                    il[i] = ir[i] = done + i < in->size() ? (*in)[done + i] : 0.0f;
            clap_event_transport_t tr { };
            tr.header = { sizeof tr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = tempo;
            tr.song_pos_beats = (clap_beattime)std::llround(beat * (double)CLAP_BEATTIME_FACTOR);
            Events                   none;
            clap_process_t           pr { };
            pr.steady_time = steady;
            pr.frames_count = n;
            pr.audio_outputs = outs;
            pr.audio_outputs_count = 8;
            pr.audio_inputs = &inb;
            pr.audio_inputs_count = in ? 1 : 0;
            pr.transport = transport ? &tr : nullptr;
            pr.in_events = done == 0 && events ? &events->in : &none.in;
            struct Ctx
            {
                Instance *i;
                uint64_t  t0;
            } octx { this, (uint64_t)steady };
            clap_output_events_t outev { &octx,
                [](const clap_output_events_t *o, const clap_event_header_t *h)
                {
                    auto *c = static_cast<Ctx *>(o->ctx);
                    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_MIDI)
                    {
                        const auto *m = reinterpret_cast<const clap_event_midi_t *>(h);
                        c->i->sent.push_back({ c->t0 + h->time, m->data[0], m->data[1], m->data[2] });
                    }
                    return true;
                } };
            pr.out_events = &outev;
            CHECK(p->process(p, &pr) == CLAP_PROCESS_CONTINUE);
            l.insert(l.end(), bl.begin(), bl.end());
            r.insert(r.end(), br.begin(), br.end());
            for (int k = 0; k < 7; k++)
                extra[(size_t)k].insert(extra[(size_t)k].end(), xb[(size_t)(2 * k)].begin(),
                    xb[(size_t)(2 * k)].end());
            if (playing)
                beat += n * tempo / 60.0 / 48000.0;
            steady += n;
            done += n;
        }
    }
    void tick() // the host's timers, once
    {
        auto *ts = static_cast<const clap_plugin_timer_support_t *>(
            p->get_extension(p, CLAP_EXT_TIMER_SUPPORT));
        CHECK(ts != nullptr);
        for (clap_id id : std::vector<clap_id>(timers))
            ts->on_timer(p, id);
    }
    std::string save()
    {
        auto *st = static_cast<const clap_plugin_state_t *>(p->get_extension(p, CLAP_EXT_STATE));
        std::string     text;
        clap_ostream_t  os { &text,
             [](const clap_ostream_t *s, const void *buf, uint64_t n) -> int64_t
             {
                const size_t k = std::min<uint64_t>(n, 7); // (a stream that takes a few bytes at a time)
                static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), k);
                return (int64_t)k;
            } };
        CHECK(st && st->save(p, &os));
        return text;
    }
    bool load(const std::string &text)
    {
        auto *st = static_cast<const clap_plugin_state_t *>(p->get_extension(p, CLAP_EXT_STATE));
        struct Src
        {
            const std::string *t;
            size_t             at = 0;
        } src { &text };
        clap_istream_t is { &src,
            [](const clap_istream_t *s, void *buf, uint64_t n) -> int64_t
            {
                auto        *c = static_cast<Src *>(s->ctx);
                const size_t k = std::min<uint64_t>({ n, uint64_t(5), uint64_t(c->t->size() - c->at) });
                std::memcpy(buf, c->t->data() + c->at, k);
                c->at += k;
                return (int64_t)k;
            } };
        return st && st->load(p, &is);
    }
};

float peak(const std::vector<float> &v, size_t from, size_t to)
{
    float m = 0;
    for (size_t i = from; i < to && i < v.size(); i++)
        m = std::fmax(m, std::fabs(v[i]));
    return m;
}

size_t first_sound(const std::vector<float> &v)
{
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] != 0.0f)
            return i;
    return v.size();
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: clap_host pulserack.clap examples-dir\n");
        return 2;
    }
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(argv[1]);
    auto   *entry = lib ? reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(lib, "clap_entry")) : nullptr;
#else
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        std::fprintf(stderr, "%s\n", dlerror());
    auto *entry = lib ? static_cast<const clap_plugin_entry_t *>(dlsym(lib, "clap_entry")) : nullptr;
#endif
    if (!entry)
    {
        std::fprintf(stderr, "%s: no clap_entry\n", argv[1]);
        return 1;
    }
    CHECK(clap_version_is_compatible(entry->clap_version));
    CHECK(entry->init(argv[1]));
    const auto *f
        = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    CHECK(f && f->get_plugin_count(f) == 2);
    const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, 0);
    const clap_plugin_descriptor_t *dfx = f->get_plugin_descriptor(f, 1);
    CHECK(dfx && !std::strcmp(dfx->name, "PulseRack FX"));
    CHECK(d && !std::strcmp(d->name, "PulseRack"));
    bool instrument = false;
    for (const char *const *ft = d->features; ft && *ft; ft++)
        instrument |= !std::strcmp(*ft, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    CHECK(instrument);
    const char *id = d->id;

    { // ports
        Instance i(f, id);
        auto *ap = static_cast<const clap_plugin_audio_ports_t *>(i.p->get_extension(i.p, CLAP_EXT_AUDIO_PORTS));
        auto *np = static_cast<const clap_plugin_note_ports_t *>(i.p->get_extension(i.p, CLAP_EXT_NOTE_PORTS));
        CHECK(ap && ap->count(i.p, true) == 0 && ap->count(i.p, false) == 8);
        clap_audio_port_info_t info { };
        CHECK(ap && ap->get(i.p, 0, false, &info) && info.channel_count == 2);
        CHECK(np && np->count(i.p, true) == 1 && np->count(i.p, false) == 1);
        clap_note_port_info_t ni { };
        CHECK(np && np->get(i.p, 0, true, &ni) && (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI));
    }

    { // silent until a note; a note sounds from its own sample, in the middle of a call and of an engine block
        Instance           i(f, id);
        std::vector<float> l, r;
        i.run(l, r, 4800, 480);
        CHECK(peak(l, 0, l.size()) == 0.0f);
        Events ev;
        ev.note(300, true, 60);
        const size_t at = l.size() + 300;
        i.run(l, r, 9600, 1024, &ev);
        // (from that sample, give or take the envelope's control rate: not from the call's start)
        CHECK(first_sound(l) >= at && first_sound(l) < at + 16);
        CHECK(peak(l, at, at + 4800) > 0.05f);
        Events off;
        off.note(0, false, 60);
        i.run(l, r, 48000, 512, &off); // the release (400 ms) dies away
        CHECK(peak(l, l.size() - 4800, l.size()) < 1e-3f);
    }

    { // the host's call size changes nothing: a note at the same sample, calls of 64 or of 1000 frames
        std::vector<float> l1, r1, l2, r2;
        for (int k = 0; k < 2; k++)
        {
            Instance i(f, id);
            auto    &l = k ? l2 : l1;
            auto    &r = k ? r2 : r1;
            i.run(l, r, 960, k ? 64 : 960); // (both calls end at 960)
            Events ev;
            ev.note(523, true, 62); // inside the first call either way
            i.run(l, r, 9600, k ? 1000 : 9600, &ev);
        }
        CHECK(peak(l1, 0, l1.size()) > 0.05f);
        CHECK(l1 == l2 && r1 == r2);
    }

    { // MIDI 1.0 bytes on the note port: the same
        Instance           i(f, id);
        std::vector<float> l, r;
        Events             ev;
        ev.midi(77, 0x90, 64, 100);
        i.run(l, r, 4800, 256, &ev);
        CHECK(first_sound(l) >= 77 && first_sound(l) < 77 + 16);
        CHECK(peak(l, 0, l.size()) > 0.05f);
    }

    { // the state: the patch file's text; another instance plays it the same
        Instance          a(f, id);
        const std::string text = a.save();
        // (a patch file, after its description line if it has one)
        CHECK(text.rfind("pulserack 1", 0) == 0
            || (text.rfind("# ", 0) == 0 && text.find("\npulserack 1\n") == text.find('\n')));
        CHECK(text.find("midi_in") != std::string::npos);
        Instance b(f, id);
        CHECK(b.load(text));
        CHECK(b.save() == text);
        std::vector<float> la, ra, lb, rb;
        Events             ev;
        ev.note(10, true, 48);
        ev.note(300, true, 55, 0.5);
        a.run(la, ra, 24000, 333, &ev);
        b.run(lb, rb, 24000, 333, &ev);
        CHECK(la == lb && ra == rb);
        CHECK(!b.load("pulserack 1\nnode 1 midi_in x=0 y=0\ncable 1.nope -> 7.in\nthis is not a patch\n"));
        CHECK(b.save() == text); // kept
    }

    { // the track's control change turns the knob the patch maps it to (no window open), whatever
      // device the mapping was made with
        Instance          i(f, id);
        CHECK(!timers.empty());
        std::string text = i.save();
        text += "map 5 gain 1 7 KeyStep 37\n"; // the VCA's gain, CC 7 on channel 1
        CHECK(i.load(text));
        std::vector<float> l, r;
        Events             ev;
        ev.midi(10, 0xB0, 7, 0);
        i.run(l, r, 512, 512, &ev);
        i.tick();
        const std::string after = i.save();
        CHECK(after.find("param 5 gain 0\n") != std::string::npos);
        Events ev2;
        ev2.midi(10, 0xB1, 7, 127); // another channel: nothing
        i.run(l, r, 512, 512, &ev2);
        i.tick();
        CHECK(i.save() == after);
    }

    { // a MIDI Out sends to the host, each message at its own sample: the track's notes passed
      // through to channel 3
        Instance i(f, id);
        CHECK(i.load("pulserack 1\nnode 1 midi_in x=0 y=0\nnode 2 midi_out x=8 y=0\nparam 2 channel ch_3\n"
                     "cable 1.notes -> 2.notes\n"));
        std::vector<float> l, r;
        i.run(l, r, 1000, 1000);
        Events ev;
        ev.note(100, true, 60, 100.0 / 127);
        ev.note(700, false, 60, 0);
        i.run(l, r, 1024, 1024, &ev);
        CHECK(i.sent.size() == 2);
        if (i.sent.size() == 2)
        {
            CHECK(i.sent[0].b0 == 0x92 && i.sent[0].b1 == 60 && i.sent[0].b2 == 100);
            CHECK(i.sent[1].b0 == 0x82 && i.sent[1].b1 == 60);
            CHECK(i.sent[0].at == 1100 && i.sent[1].at == 1700);
        }
    }

    { // the Host Clock: START when the host plays, a pulse on every 24th of its beat grid (120 bpm
      // at 48 kHz: every 1000 samples, from wherever playing starts), STOP when it stops, and START
      // again when it jumps back. Seen through a MIDI Out (clock bytes) at their samples.
        Instance i(f, id);
        CHECK(i.load("pulserack 1\nnode 1 host_clock x=0 y=0\nnode 2 midi_out x=8 y=0\n"
                     "cable 1.clock -> 2.clock\n"));
        std::vector<float> l, r;
        i.transport = true;
        i.run(l, r, 2000, 500); // stopped: nothing
        CHECK(i.sent.empty());
        i.playing = true;
        i.beat = 1.5 + 100.0 / 1000 / 24; // 100 samples after a pulse
        i.run(l, r, 4000, 333);           // from sample 2000
        CHECK(i.sent.size() == 5 && i.sent[0].b0 == 0xFA && i.sent[0].at == 2000);
        for (size_t k = 1; k < i.sent.size() && k < 5; k++) // the grid: 900 samples on, then every 1000
            CHECK(i.sent[k].b0 == 0xF8 && i.sent[k].at == 2000 + 900 + 1000 * (k - 1));
        i.sent.clear();
        i.beat = 0; // back to the start (a loop)
        i.run(l, r, 1000, 1000);
        CHECK(i.sent.size() == 2 && i.sent[0].b0 == 0xFA && i.sent[1].b0 == 0xF8
            && i.sent[1].at == i.sent[0].at);
        i.sent.clear();
        i.playing = false;
        i.run(l, r, 1000, 1000);
        CHECK(i.sent.size() == 1 && i.sent[0].b0 == 0xFC);
    }

    { // Macro 1 to 32: named after the control the patch gives each one; the host's automation turns
      // it, and the model (what is saved) follows
        Instance i(f, id);
        auto    *pp = static_cast<const clap_plugin_params_t *>(i.p->get_extension(i.p, CLAP_EXT_PARAMS));
        CHECK(pp && pp->count(i.p) == 32);
        std::string text = i.save() + "macro 3 5 gain\n";
        CHECK(i.load(text));
        clap_param_info_t info { };
        CHECK(pp && pp->get_info(i.p, 2, &info) && info.id == 2 && !std::strcmp(info.name, "Macro 3: Gain (VCA 5)")
            && (info.flags & CLAP_PARAM_IS_AUTOMATABLE));
        CHECK(pp && pp->get_info(i.p, 0, &info) && !std::strcmp(info.name, "Macro 1"));
        double v = -1;
        CHECK(pp && pp->get_value(i.p, 2, &v) && std::fabs(v - 1 / 1.9) < 1e-6); // gain 1 of 0..1.9
        Events ev;
        clap_event_param_value_t pv { };
        pv.header = { sizeof pv, 5, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        pv.param_id = 2;
        pv.value = 0.25;
        ev.add(&pv, sizeof pv);
        std::vector<float> l, r;
        i.run(l, r, 512, 512, &ev);
        CHECK(pp && pp->get_value(i.p, 2, &v) && std::fabs(v - 0.25) < 1e-6);
        CHECK(i.save().find("param 5 gain 0.47") != std::string::npos); // (0.474999994)
        char txt[64];
        CHECK(pp && pp->value_to_text(i.p, 2, 0.25, txt, sizeof txt) && std::string(txt) == "0.475");
        // a flush outside process does the same
        pv.value = 1.0;
        Events fl;
        fl.add(&pv, sizeof pv);
        pp->flush(i.p, &fl.in, &kNoOut);
        CHECK(pp && pp->get_value(i.p, 2, &v) && std::fabs(v - 1.0) < 1e-6);
        // a macro that turns nothing keeps the value it was given
        pv.param_id = 7;
        pv.value = 0.75;
        Events ev2;
        ev2.add(&pv, sizeof pv);
        i.run(l, r, 512, 512, &ev2);
        CHECK(pp && pp->get_value(i.p, 7, &v) && std::fabs(v - 0.75) < 1e-6);
    }

    { // an Audio Output on Output 3 plays there, not on the main output
        Instance i(f, id);
        CHECK(i.load("pulserack 1\nnode 1 osc x=0 y=0\nnode 2 out x=8 y=0\nparam 2 output 3\n"
                     "cable 1.out -> 2.l\n"));
        std::vector<float> l, r;
        i.run(l, r, 4800, 480);
        CHECK(peak(l, 0, l.size()) == 0.0f && peak(i.extra[1], 0, 4800) > 0.1f
            && peak(i.extra[0], 0, 4800) == 0.0f);
    }

    { // PulseRack FX: the track's audio in through Audio In (level 1, out level 0.8)
        CHECK(dfx != nullptr);
        Instance i(f, dfx->id);
        auto *ap = static_cast<const clap_plugin_audio_ports_t *>(i.p->get_extension(i.p, CLAP_EXT_AUDIO_PORTS));
        CHECK(ap && ap->count(i.p, true) == 1 && ap->count(i.p, false) == 8);
        CHECK(i.save().find("audio_in") != std::string::npos); // its first patch
        CHECK(i.load("pulserack 1\nnode 1 audio_in x=0 y=0\nnode 2 out x=4 y=0\ncable 1.l -> 2.l\n"));
        std::vector<float> in(2000), l, r;
        for (size_t k = 0; k < in.size(); k++)
            in[k] = 0.5f * std::sin((float)k * 0.05f);
        i.in = &in;
        i.run(l, r, 2000, 256);
        bool same = true;
        for (size_t k = 0; k < in.size(); k++)
            same = same && std::fabs(l[k] - in[k] * 0.8f) < 1e-6f && std::fabs(r[k] - in[k] * 0.8f) < 1e-6f;
        CHECK(same);
    }

    { // a patch that plays itself
        std::ifstream     in(std::string(argv[2]) + "/sequence.rack", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        CHECK(!ss.str().empty());
        Instance i(f, id);
        CHECK(i.load(ss.str()));
        std::vector<float> l, r;
        i.run(l, r, 96000, 512);
        CHECK(peak(l, 0, l.size()) > 0.05f);
    }

    entry->deinit();
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("clap_host: all checks passed\n");
    return 0;
}
