// PulseRack as CLAP plugins (docs/reaper.md, *Level 3*): the rack's engine inside a host such as
// REAPER, and the rack's window inside the host's plugin window. Two plugins, one file:
// **PulseRack**, an instrument, and **PulseRack FX**, an effect: the same rack with the track's audio
// coming in (the Audio In module).
//
// * the patch is the plugin's state, written as the patch file's text, so the host saves it with its
//   project (and a state is a .rack file: it opens in the standalone rack too);
// * the host's notes (CLAP note events, or MIDI 1.0 on the note port) reach every MIDI In and MIDI CC
//   module at their own sample, as if from one device: a module's Device box names a device of the
//   standalone rack, and in the plugin every name means the host;
// * eight stereo outputs: the main one (the Audio Output modules on Output 1) and seven more (those
//   on Output 2 to 8), which the host can route to tracks of their own;
// * the host's transport (tempo, playing, beat position) reaches the Host Clock modules;
// * 32 automatable parameters, Macro 1 to 32: each turns the control the patch gives it
//   (Patch::macros, chosen in the window), at the start of the engine block the change falls in;
// * the MIDI Out modules send to the host (the plugin's note output, MIDI 1.0), each message at the
//   sample it was made, so the track's MIDI output can record it or play another instrument;
// * the editor (clap.gui, with PULSERACK_BUILD_GUI): the rack's own window (ui/rack_ui.h), opened inside
//   the host's (an X11 window or an HWND) as a gpuic app the host's timer steps (clap.timer-support).
//   It edits the plugin's engine; it is made when the host shows it and freed when it hides it, and the
//   patch lives on in the engine. The host has the sound and the MIDI, so the window has no Audio
//   button and opens no devices.
//
// A new instrument starts with the MIDI voice example (MIDI In, oscillator, envelope, filter, VCA), so
// it sounds as soon as a MIDI item plays into it; a new effect with a stereo delay on its input.
//
// Threads, as CLAP spells them out: init, activate, deactivate and the state run on the host's main
// thread, which is the engine's UI thread; process runs on the audio thread, attached between
// activate and deactivate (engine.h, *Threads*). Nothing in process allocates or locks.
#include "engine/engine.h"
#include "engine/model/patchfile.h"
#include "midi/message.h"
#include "midi/outbox.h"
#include "ui/files.h"
#include "ui/geometry.h"
#include "ui/settings.h"

#if PULSERACK_CLAP_GUI
#include "ui/rack_ui.h"
#include "ui/style.h"

#include <gpuic/gpuic.hpp>
#endif

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

using rack::Engine;
using rack::Err;
using rack::Patch;
namespace midi = rack::midi;

constexpr const char *kDefaultPatch = "midi_voice"; // examples/midi_voice.rack
constexpr size_t      kMaxEvents = 4096;            // host messages per process call; more are dropped
using rack::kExtraOutputs;
using rack::kMacros;

// PulseRack FX's first patch: the track's audio through a delay on each side.
constexpr const char *kDefaultFxPatch = R"(pulserack 1
node 1 audio_in x=0 y=0
node 2 delay x=4 y=0
param 2 time 350
node 3 delay x=12 y=0
param 3 time 370
node 4 out x=20 y=0
cable 1.l -> 2.in
cable 1.r -> 3.in
cable 2.out -> 4.l
cable 3.out -> 4.r
)";

struct Plugin
{
    clap_plugin_t                   plugin { };
    bool                            fx = false; // PulseRack FX: the host's audio comes in
    const clap_host_t              *host = nullptr;
    const clap_host_params_t       *host_params = nullptr; // (optional: rescan)
    const clap_host_state_t        *host_state = nullptr; // (optional: mark_dirty)
    const clap_host_log_t          *host_log = nullptr;   // (optional)
    std::unique_ptr<Engine>         engine;
    std::vector<midi::Timed>        events; // the audio thread's, reserved at init
    midi::Outbox                    outbox; // what the MIDI Out modules send, for the host
    std::vector<rack::MacroChange>  macro_changes; // the audio thread's, reserved at init
    // each macro's value as the host last set it (what get_value says for one that turns nothing)
    std::array<std::atomic<float>, kMacros> macro_values { };
    std::vector<rack::Macro>        shown_macros; // what the host was last told the macros turn
    uint64_t                        events_lost = 0;
    bool                            active = false;
    bool                            loading = false; // a state load: not a change to report
    bool                            ready = false;   // init has finished
    // The host's timer (60 Hz, on its main thread): it runs the window while one is open, and
    // otherwise applies the track's control changes to the knobs the patch maps them to.
    const clap_host_timer_support_t *timers = nullptr;
    clap_id                          timer = CLAP_INVALID_ID;
#if PULSERACK_CLAP_GUI
    const clap_host_gui_t           *host_gui = nullptr; // (optional: request_resize)
    gp_app                          *app = nullptr; // the editor, while the host shows it
    gpuic::Entity<rack::ui::Editor>  editor;
    uint32_t width = (uint32_t)rack::ui::style::kWindowW, height = (uint32_t)rack::ui::style::kWindowH;
    rack::ui::Editor *open_editor() const
    {
        return app && editor ? gpuic::App(app).get(editor) : nullptr;
    }
#endif

    // The patch changed: the host's project has unsaved changes, a macro's control may have moved
    // (its value), and the macros may turn other controls now (their names).
    void changed()
    {
        if (host_state && !loading)
            host_state->mark_dirty(host);
        if (!host_params || !ready)
        {
            shown_macros = engine->patch().macros; // (before init has finished: nothing to tell)
            return;
        }
        const bool renamed = engine->patch().macros != shown_macros;
        shown_macros = engine->patch().macros;
        host_params->rescan(host,
            CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | (renamed ? CLAP_PARAM_RESCAN_INFO : 0));
    }

    void log(clap_log_severity severity, const std::string &text) const
    {
        if (host_log)
            host_log->log(host, severity, text.c_str());
    }
};

Plugin *of(const clap_plugin_t *p) { return static_cast<Plugin *>(p->plugin_data); }

// Replaces the patch with `text`, every module made again from it (as opening a file does in the
// rack: ui/rack_ui.cpp, Editor::open_text). False, and the old patch kept, if it does not load.
bool load_text(Plugin &p, std::string_view text, std::string *error)
{
    Patch                  loaded;
    const rack::LoadResult r = rack::load_patch(text, loaded);
    for (const std::string &w : r.warnings)
        p.log(CLAP_LOG_WARNING, "PulseRack: " + w);
    if (!r.ok)
    {
        *error = r.error;
        return false;
    }
    rack::ui::resolve_overlaps(loaded);
    Engine &e = *p.engine;
    Patch   old = std::move(e.patch());
    e.patch() = std::move(loaded);
    if (e.compile() != Err::Ok)
    {
        *error = "module " + std::to_string(e.failed_node()) + " refused its settings";
        e.patch() = std::move(old);
        e.compile();
        return false;
    }
    e.reset();
    p.changed();
    return true;
}

// ---------------------------------------------------------------- the host's events

// A CLAP note's velocity (0..1) as MIDI's (1..127 for a note on: 0 would be a note off).
uint16_t velocity_of(double v, bool on)
{
    const int x = (int)std::lround(std::clamp(v, 0.0, 1.0) * 127.0);
    return (uint16_t)(on ? std::max(x, 1) : x);
}

void take_event(Plugin &p, const clap_event_header_t *h)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    auto add = [&](const midi::Msg &m)
    {
        if (p.events.size() < kMaxEvents)
            p.events.push_back({ h->time, m });
        else
            p.events_lost++;
    };
    switch (h->type)
    {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE:
    {
        const auto *n = reinterpret_cast<const clap_event_note_t *>(h);
        const bool  on = h->type == CLAP_EVENT_NOTE_ON;
        midi::Msg   m;
        m.channel = (uint8_t)(n->channel >= 0 ? n->channel & 15 : 0);
        if (n->key < 0) // every key (the wildcard): only an ending makes sense
        {
            if (on)
                return;
            m.type = midi::Type::Control;
            m.data = 123; // all notes off
            if (n->channel >= 0)
                add(m);
            else
                for (uint8_t ch = 0; ch < 16; ch++)
                {
                    m.channel = ch;
                    add(m);
                }
            return;
        }
        m.type = on ? midi::Type::NoteOn : midi::Type::NoteOff;
        m.data = (uint8_t)(n->key & 127);
        m.value = velocity_of(n->velocity, on);
        add(m);
        return;
    }
    case CLAP_EVENT_PARAM_VALUE: // a macro (its id is its index)
    {
        const auto *v = reinterpret_cast<const clap_event_param_value_t *>(h);
        if (v->param_id >= kMacros)
            return;
        const float t = (float)std::clamp(v->value, 0.0, 1.0);
        p.macro_values[v->param_id].store(t, std::memory_order_relaxed);
        if (p.macro_changes.size() < kMaxEvents)
            p.macro_changes.push_back({ h->time, (uint8_t)v->param_id, t });
        return;
    }
    case CLAP_EVENT_MIDI:
    {
        const auto *e = reinterpret_cast<const clap_event_midi_t *>(h);
        midi::Msg   m;
        // (a message's length follows from its status byte; decode() checks it)
        const uint8_t status = e->data[0];
        const size_t  len = status >= 0xF8 ? 1 : (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
        if (midi::decode({ e->data, len }, &m))
            add(m);
        return;
    }
    default:
        return;
    }
}

// ---------------------------------------------------------------- the plugin

bool plugin_init(const clap_plugin_t *plugin)
{
    Plugin &p = *of(plugin);
    p.host_state = static_cast<const clap_host_state_t *>(p.host->get_extension(p.host, CLAP_EXT_STATE));
    p.host_log = static_cast<const clap_host_log_t *>(p.host->get_extension(p.host, CLAP_EXT_LOG));
    p.host_params = static_cast<const clap_host_params_t *>(p.host->get_extension(p.host, CLAP_EXT_PARAMS));
    p.timers = static_cast<const clap_host_timer_support_t *>(
        p.host->get_extension(p.host, CLAP_EXT_TIMER_SUPPORT));
    if (p.timers && !p.timers->register_timer(p.host, 16, &p.timer))
        p.timer = CLAP_INVALID_ID;
#if PULSERACK_CLAP_GUI
    p.host_gui = static_cast<const clap_host_gui_t *>(p.host->get_extension(p.host, CLAP_EXT_GUI));
#endif
    p.events.reserve(kMaxEvents);
    p.macro_changes.reserve(kMaxEvents);
    p.engine = std::make_unique<Engine>(48000); // made again at the host's rate in activate
    p.engine->travel_to_value = rack::ui::from_travel; // (a macro moves its control as its knob would)
    // every device a patch names is the host (slot 0); a MIDI Out has nowhere to send yet
    p.engine->midi_slot_of = [](const std::string &) { return 0; };
    p.engine->midi_out_slot_of = [](const std::string &) { return 0; };
    p.engine->set_midi_out(&p.outbox);
    std::string_view first = kDefaultFxPatch;
    if (!p.fx)
        for (const rack::ui::FactoryPatch &f : rack::ui::factory_patches())
            if (f.name == kDefaultPatch)
                first = f.text;
    p.loading = true;
    if (std::string error; !load_text(p, first, &error))
        p.log(CLAP_LOG_ERROR, "PulseRack: the first patch: " + error);
    p.loading = false;
    p.ready = true;
    return true;
}

void gui_destroy(const clap_plugin_t *plugin);

void plugin_destroy(const clap_plugin_t *plugin)
{
    Plugin *p = of(plugin);
#if PULSERACK_CLAP_GUI
    gui_destroy(plugin);
#endif
    if (p->timer != CLAP_INVALID_ID)
        p->timers->unregister_timer(p->host, p->timer);
    if (p->active && p->engine)
        p->engine->attach_audio(false);
    delete p;
}

bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t, uint32_t)
{
    Plugin &p = *of(plugin);
    if (sample_rate < 1000 || sample_rate > 1e6)
        return false;
    if ((uint32_t)std::lround(sample_rate) != p.engine->sample_rate())
        p.engine->set_sample_rate((uint32_t)std::lround(sample_rate));
    p.engine->attach_audio(true);
    p.active = true;
    return true;
}

void plugin_deactivate(const clap_plugin_t *plugin)
{
    Plugin &p = *of(plugin);
    p.engine->attach_audio(false);
    p.active = false;
    if (p.events_lost)
        p.log(CLAP_LOG_WARNING,
            "PulseRack: " + std::to_string(p.events_lost) + " MIDI messages were dropped (too many in one block)");
    p.events_lost = 0;
}

bool plugin_start_processing(const clap_plugin_t *) { return true; }
void plugin_stop_processing(const clap_plugin_t *) { }
void plugin_reset(const clap_plugin_t *) { }

clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    Plugin &p = *of(plugin);
    p.events.clear();
    p.macro_changes.clear();
    if (const clap_input_events_t *in = process->in_events)
    {
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; i++)
            take_event(p, in->get(in, i));
    }
    // (the host gives them in time order; the clamp is for one that does not)
    for (midi::Timed &t : p.events)
        t.offset = std::min(t.offset, process->frames_count ? process->frames_count - 1 : 0);
    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2
        || !process->audio_outputs[0].data32)
        return CLAP_PROCESS_ERROR; // (the port is stereo: a host gives two channels)
    rack::HostCall call;
    call.midi = p.events;
    call.macros = p.macro_changes;
    if (const clap_event_transport_t *t = process->transport)
    {
        call.transport = (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
        call.playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        call.tempo = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 120.0;
        call.beat = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
    }
    if (p.fx && process->audio_inputs_count >= 1 && process->audio_inputs[0].data32
        && process->audio_inputs[0].channel_count >= 1)
    {
        const clap_audio_buffer_t &in = process->audio_inputs[0];
        call.in[0] = in.data32[0];
        call.in[1] = in.channel_count >= 2 ? in.data32[1] : in.data32[0];
    }
    // (the input is read block by block while the output is written: a host that gives the same
    // buffer for both is not told this plugin can work in place, so it does not)
    for (uint32_t k = 1; k < process->audio_outputs_count && k <= kExtraOutputs; k++)
    {
        const clap_audio_buffer_t &o = process->audio_outputs[k];
        if (o.data32 && o.channel_count >= 2)
        {
            call.out[k - 1][0] = o.data32[0];
            call.out[k - 1][1] = o.data32[1];
        }
    }
    float **out = process->audio_outputs[0].data32;
    p.engine->render(process->frames_count, out[0], out[1], call);
    // what the MIDI Out modules sent in this call, to the host at its own sample
    midi::OutMsg m;
    while (p.outbox.ring.pop(m))
    {
        if (!process->out_events)
            continue;
        clap_event_midi_t e { };
        e.header = { sizeof e, std::min(m.offset, process->frames_count ? process->frames_count - 1 : 0),
            CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        e.port_index = 0;
        std::memcpy(e.data, m.bytes, 3);
        if (m.len < 3)
            e.data[2] = 0;
        if (m.len < 2)
            e.data[1] = 0;
        process->out_events->try_push(process->out_events, &e.header);
    }
    return CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------- clap.audio-ports
// Out: the main output, then Output 2 to 8. In (PulseRack FX only): the track's audio.

uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    return is_input ? (of(plugin)->fx ? 1 : 0) : 1 + (uint32_t)kExtraOutputs;
}
bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    if (index >= audio_ports_count(plugin, is_input))
        return false;
    *info = { };
    info->id = is_input ? 100 : index;
    if (is_input)
        std::snprintf(info->name, sizeof info->name, "%s", "Input");
    else if (index == 0)
        std::snprintf(info->name, sizeof info->name, "%s", "Output");
    else
        std::snprintf(info->name, sizeof info->name, "Output %u", index + 1);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kAudioPorts = { audio_ports_count, audio_ports_get };

// ---------------------------------------------------------------- clap.note-ports: notes or MIDI in, MIDI out

// (the output: what the MIDI Out modules send, MIDI 1.0 only)
uint32_t note_ports_count(const clap_plugin_t *, bool) { return 1; }
bool     note_ports_get(const clap_plugin_t *, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    if (index != 0)
        return false;
    *info = { };
    info->id = 0;
    info->supported_dialects = is_input ? CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::snprintf(info->name, sizeof info->name, "%s", is_input ? "MIDI in" : "MIDI out");
    return true;
}
const clap_plugin_note_ports_t kNotePorts = { note_ports_count, note_ports_get };

// ---------------------------------------------------------------- the macros into the model

// What the macros set on the audio thread, into the patch (and onto the panels, with a window).
void pump_host_params(Plugin &p)
{
#if PULSERACK_CLAP_GUI
    if (rack::ui::Editor *e = p.open_editor())
    {
        e->pump_host_params(gpuic::App(p.app).ctx(p.editor));
        return;
    }
#endif
    rack::HostParam hp;
    while (p.engine->take_host_param(hp))
    {
    }
}

// ---------------------------------------------------------------- clap.params: Macro 1 to 32

const rack::Macro *macro_of(const Patch &patch, uint32_t index)
{
    for (const rack::Macro &m : patch.macros)
        if (m.index == index)
            if (const rack::Node *n = patch.find(m.node); n && m.param < n->desc->params.size())
                return &m;
    return nullptr;
}

uint32_t params_count(const clap_plugin_t *) { return (uint32_t)kMacros; }
bool     params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    if (index >= kMacros)
        return false;
    const Patch &patch = of(plugin)->engine->patch();
    *info = { };
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0;
    info->max_value = 1;
    info->default_value = 0;
    if (const rack::Macro *m = macro_of(patch, index))
    {
        const rack::Node *n = patch.find(m->node);
        std::snprintf(info->name, sizeof info->name, "Macro %u: %.*s (%.*s %u)", index + 1,
            (int)n->desc->params[m->param].name.size(), n->desc->params[m->param].name.data(),
            (int)n->desc->name.size(), n->desc->name.data(), n->id);
        info->default_value = rack::ui::to_travel(n->desc->params[m->param], n->desc->params[m->param].def);
    }
    else
        std::snprintf(info->name, sizeof info->name, "Macro %u", index + 1);
    std::snprintf(info->module, sizeof info->module, "%s", "Macros");
    return true;
}
bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    Plugin &p = *of(plugin);
    if (id >= kMacros)
        return false;
    pump_host_params(p);
    if (const rack::Macro *m = macro_of(p.engine->patch(), id))
    {
        const rack::Node *n = p.engine->patch().find(m->node);
        *value = rack::ui::to_travel(n->desc->params[m->param], n->params[m->param]);
    }
    else
        *value = p.macro_values[id].load(std::memory_order_relaxed);
    return true;
}
bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    if (id >= kMacros || !size)
        return false;
    const Patch &patch = of(plugin)->engine->patch();
    if (const rack::Macro *m = macro_of(patch, id))
    {
        const rack::ParamDesc &d = patch.find(m->node)->desc->params[m->param];
        const float            v = rack::ui::from_travel(d, (float)value);
        if (!d.labels.empty() && v >= 0 && (size_t)v < d.labels.size())
            std::snprintf(out, size, "%.*s", (int)d.labels[(size_t)v].size(), d.labels[(size_t)v].data());
        else
            std::snprintf(out, size, "%.3g%s%.*s", v, d.unit.empty() ? "" : " ", (int)d.unit.size(), d.unit.data());
    }
    else
        std::snprintf(out, size, "%.0f %%", value * 100);
    return true;
}
bool params_text_to_value(const clap_plugin_t *, clap_id, const char *, double *) { return false; }
// Changes outside process: on the audio thread while active, else on the main thread (detached).
void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *)
{
    Plugin &p = *of(plugin);
    p.macro_changes.clear();
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            take_event(p, h);
    }
    for (rack::MacroChange &c : p.macro_changes)
        c.offset = 0;
    p.engine->host_macros(p.macro_changes);
}
const clap_plugin_params_t kParams = { params_count, params_get_info, params_get_value,
    params_value_to_text, params_text_to_value, params_flush };

// ---------------------------------------------------------------- clap.state: the patch file's text

bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    pump_host_params(*of(plugin)); // (what the automation set last is what is saved)
    const std::string text = rack::save_patch(of(plugin)->engine->patch());
    for (size_t done = 0; done < text.size();) // (a stream may take fewer bytes than given)
    {
        const int64_t n = stream->write(stream, text.data() + done, text.size() - done);
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    Plugin     &p = *of(plugin);
    std::string text;
    char        buf[4096];
    for (int64_t n; (n = stream->read(stream, buf, sizeof buf)) != 0;)
    {
        if (n < 0)
            return false;
        text.append(buf, (size_t)n);
        if (text.size() > (64u << 20)) // (no patch is this big)
            return false;
    }
    std::string error;
    p.loading = true;
    bool ok;
#if PULSERACK_CLAP_GUI
    if (rack::ui::Editor *e = p.open_editor()) // through the window, so that it shows the new patch
        ok = e->open_text(gpuic::App(p.app).ctx(p.editor), text, "the project's patch", "", false, &error);
    else
#endif
        ok = load_text(p, text, &error);
    p.loading = false;
    if (!ok)
    {
        p.log(CLAP_LOG_ERROR, "PulseRack: the saved patch did not load: " + error);
        return false;
    }
    return true;
}
const clap_plugin_state_t kState = { state_save, state_load };

// ---------------------------------------------------------------- clap.gui: the rack's window

#if PULSERACK_CLAP_GUI
#ifdef _WIN32
constexpr const char *kGuiApi = CLAP_WINDOW_API_WIN32;
#else
constexpr const char *kGuiApi = CLAP_WINDOW_API_X11;
#endif
constexpr uint32_t kMinWidth = 900, kMinHeight = 500;

bool gui_is_api_supported(const clap_plugin_t *, const char *api, bool is_floating)
{
    return !is_floating && !std::strcmp(api, kGuiApi);
}
bool gui_get_preferred_api(const clap_plugin_t *, const char **api, bool *is_floating)
{
    *api = kGuiApi;
    *is_floating = false;
    return true;
}
bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    Plugin &p = *of(plugin);
    if (is_floating || std::strcmp(api, kGuiApi) != 0 || p.app || p.timer == CLAP_INVALID_ID)
        return false; // (without the host's timer the window would stand still)
#ifndef _WIN32
    setenv("SDL_VIDEO_DRIVER", "x11", 0); // (the host's window is an X11 one, XWayland's on Wayland)
#endif
    p.app = gp_app_new();
    return p.app != nullptr;
}
void gui_destroy(const clap_plugin_t *plugin)
{
    Plugin &p = *of(plugin);
    if (!p.app)
        return;
    gp_app_free(p.app); // (the window goes before the host's, as embedding asks; the engine stays)
    p.app = nullptr;
    p.editor = { };
}
bool gui_set_scale(const clap_plugin_t *, double) { return false; } // (SDL takes it from the window)
bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    *width = of(plugin)->width;
    *height = of(plugin)->height;
    return true;
}
bool gui_can_resize(const clap_plugin_t *) { return true; }
bool gui_get_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *hints)
{
    *hints = { };
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    return true;
}
bool gui_adjust_size(const clap_plugin_t *, uint32_t *width, uint32_t *height)
{
    *width = std::max(*width, kMinWidth);
    *height = std::max(*height, kMinHeight);
    return true;
}
bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    Plugin &p = *of(plugin);
    p.width = std::max(width, kMinWidth);
    p.height = std::max(height, kMinHeight);
    if (rack::ui::Editor *e = p.open_editor(); e && e->window)
        gp_window_set_size(e->window, (int)p.width, (int)p.height);
    return true;
}
bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    Plugin &p = *of(plugin);
    if (!p.app || p.editor || std::strcmp(window->api, kGuiApi) != 0)
        return false;
    rack::ui::Options opt;
    opt.engine = p.engine.get();
#ifdef _WIN32
    opt.parent = window->win32;
#else
    opt.parent = reinterpret_cast<void *>(static_cast<uintptr_t>(window->x11));
#endif
    opt.width = (int)p.width;
    opt.height = (int)p.height;
    opt.host_name = p.host->name ? p.host->name : "";
    opt.on_change = [pp = &p] { pp->changed(); };
    opt.config_dir = rack::ui::default_config_dir(); // (the global MIDI learn, shared with the rack)
    std::string error;
    p.editor = rack::ui::open_rack(gpuic::App(p.app), opt, &error);
    rack::ui::Editor *e = p.open_editor();
    if (!e || !e->window)
    {
        p.log(CLAP_LOG_ERROR, "PulseRack: the window did not open" + (error.empty() ? "" : ": " + error));
        return false;
    }
    return true;
}
bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) { return false; }
void gui_suggest_title(const clap_plugin_t *, const char *) { }
bool gui_show(const clap_plugin_t *) { return true; }
bool gui_hide(const clap_plugin_t *) { return true; }
const clap_plugin_gui_t kGui = { gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy,
    gui_set_scale, gui_get_size, gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size,
    gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide };

#else
void gui_destroy(const clap_plugin_t *) { }
#endif

// ---------------------------------------------------------------- clap.timer-support

// The track's control changes while no window is open: each one the patch maps ("MIDI learn for
// this knob only") turns its knob. (With a window open, the window does this and the global
// learn as well.)
void apply_mappings(Plugin &p)
{
    Engine   &e = *p.engine;
    midi::Msg m;
    bool      any = false;
    while (e.take_control(m))
        if (const rack::Mapping *l = rack::ui::find_mapping(e.patch(), m, "", true))
        {
            const rack::Node *n = e.patch().find(l->node);
            const float       v = rack::ui::controller_value(n->desc->params[l->param], m.value);
            if (v != n->params[l->param] && e.set_param(l->node, l->param, v) == Err::Ok)
                any = true;
        }
    if (any)
        p.changed();
}

void on_timer(const clap_plugin_t *plugin, clap_id id)
{
    Plugin &p = *of(plugin);
    if (id != p.timer)
        return;
#if PULSERACK_CLAP_GUI
    if (p.app)
    {
        gp_app_step(p.app, nullptr);
        return;
    }
#endif
    apply_mappings(p);
    pump_host_params(p);
    p.engine->collect();
}
const clap_plugin_timer_support_t kTimerSupport = { on_timer };

const void *plugin_get_extension(const clap_plugin_t *, const char *id)
{
#if PULSERACK_CLAP_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &kGui;
#endif
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &kTimerSupport;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &kState;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kParams;
    return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    of(plugin)->engine->collect(); // (plans and states the audio thread let go of)
}

// ---------------------------------------------------------------- the factory and the entry

const char *const kFeatures[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO, nullptr };
const char *const kFxFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
    CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "pulserack.rack",
    "PulseRack",
    "PulseRack",
    "",
    "",
    "",
    PULSERACK_VERSION,
    "A modular rack: the patch plays the track's notes",
    kFeatures,
};
const clap_plugin_descriptor_t kFxDescriptor = {
    CLAP_VERSION_INIT,
    "pulserack.fx",
    "PulseRack FX",
    "PulseRack",
    "",
    "",
    "",
    PULSERACK_VERSION,
    "A modular rack as an effect: the track's audio comes in through Audio In",
    kFxFeatures,
};

const clap_plugin_t *create_plugin(const clap_plugin_factory_t *, const clap_host_t *host, const char *id)
{
    if (!clap_version_is_compatible(host->clap_version))
        return nullptr;
    const bool fx = !std::strcmp(id, kFxDescriptor.id);
    if (!fx && std::strcmp(id, kDescriptor.id) != 0)
        return nullptr;
    auto *p = new Plugin;
    p->host = host;
    p->fx = fx;
    p->plugin = { fx ? &kFxDescriptor : &kDescriptor, p, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
        plugin_start_processing, plugin_stop_processing, plugin_reset, plugin_process, plugin_get_extension,
        plugin_on_main_thread };
    return &p->plugin;
}

uint32_t                        factory_count(const clap_plugin_factory_t *) { return 2; }
const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *, uint32_t index)
{
    return index == 0 ? &kDescriptor : index == 1 ? &kFxDescriptor : nullptr;
}
const clap_plugin_factory_t kFactory = { factory_count, factory_descriptor, create_plugin };

bool        entry_init(const char *) { return true; }
void        entry_deinit() { }
const void *entry_get_factory(const char *id)
{
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry
    = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
