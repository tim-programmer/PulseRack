// The engine: a patch, the live module states and the compiled plan, run by one audio thread and
// edited by one UI thread (docs/architecture.md, *Threads*).
//
// * The audio thread owns every module: all calls into a module (process and the library's setters)
//   happen in render(). The UI thread changes the patch model and sends parameter, step and key
//   changes through a fixed-size command ring; render() drains it at the start of every block.
// * A change to the graph (compile, reset) builds new module states and a new plan on the UI thread
//   and publishes them with one atomic exchange; render() adopts them at a block boundary and hands
//   the old plan back. A removed module's state is freed only after the audio thread has adopted a
//   plan without it.
// * Telemetry (levels, activity, dropped events) goes the other way, through atomics.
//
// Until an audio thread is attached (rack-render, the tests, the window with audio off) the UI
// thread is also the audio side: every edit takes effect at once, exactly as the audio thread would
// apply it at the next block. So the same code runs in both cases, and the tests cover it.
#pragma once

#include "engine/model/patch.h"
#include "engine/model/plan.h"
#include "engine/model/scope.h"
#include "engine/model/wav.h"
#include "engine/rt/spsc.h"
#include "midi/inbox.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rack
{

inline constexpr size_t kTelemetrySlots = 1024; // as many as a patch may have modules

// What the audio thread reports. Levels are the largest |sample| since the UI last took them.
struct Telemetry
{
    std::array<std::atomic<float>, 2>               peak { };     // the output, left and right
    std::atomic<float>                              load { 0 };   // set by the audio device's owner
    std::array<std::atomic<float>, kTelemetrySlots> activity { }; // per module: its outputs
    std::array<std::atomic<uint32_t>, kTelemetrySlots> dropped { }; // per module: events it dropped
    std::array<std::atomic<int32_t>, kTelemetrySlots>
                          position { };        // per sequencer: its step, else -1
    std::atomic<uint64_t> commands_lost { 0 }; // the ring was full

    float take_peak(int ch) { return peak[(size_t)ch].exchange(0.f, std::memory_order_relaxed); }
    float take_activity(int slot)
    {
        return slot < 0 ? 0.f : activity[(size_t)slot].exchange(0.f, std::memory_order_relaxed);
    }
};

class Engine
{
public:
    explicit Engine(uint32_t sample_rate);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // ---- the UI thread
    Patch       &patch() { return patch_; }
    const Patch &patch() const { return patch_; }

    // Brings the live modules in line with the patch (new nodes are created from their parameters,
    // removed ones retired, existing ones keep their state) and publishes a new plan. Call after
    // editing the patch's nodes or cables.
    Err compile();
    // A parameter change: the model now, the running module at the next block.
    Err set_param(uint32_t node, size_t param, float value);
    Err set_step(uint32_t node, size_t index, const StepData &step);
    // A live key press for a Keyboard node, heard at the start of the next block (or at `at`, an
    // absolute sample).
    Err key(uint32_t node, bool on, uint8_t note, uint8_t velocity = 100, int64_t at = -1);
    // Where MIDI messages arrive (docs/midi.md, *Threads*): render() takes every message out of
    // every inbox at the start of a block and gives it to each MIDI In module. The inboxes must
    // outlive the engine's last render(). Set it (or null) only while no audio thread is attached.
    void set_midi(midi::Inboxes *inboxes);
    // Throws away what waits in the inboxes (detached only): before audio starts, so that what was
    // played while it was off is not heard all at once.
    void discard_midi();
    // The MIDI input a MIDI In or MIDI CC module listens to, by name (empty: every input); kept in
    // the patch. The name becomes a device slot through `midi_slot_of` (the application's: the
    // device manager's slot for the name, or -1); with none, or -1, the module hears nothing.
    // NoSuchParam for a module that takes no MIDI.
    Err                                     set_device(uint32_t node, const std::string &name);
    std::function<int(const std::string &)> midi_slot_of;
    // Looks every such module's device up again (after `midi_slot_of` was set or changed).
    void refresh_devices();
    // The WAV file a Sampler plays (kept in the patch as Node::sample): read now, and the module
    // made again with it. NoSuchParam for a module that plays no sample; InitFailed (and `error`)
    // if the file cannot be read, and nothing changes. An empty path: no sample.
    Err set_sample(uint32_t node, const std::string &path, std::string *error);
    // Why a Sampler's file did not load when the patch was compiled ("cannot be read", "not a WAV
    // file", ...), or empty if it did (or the node has none).
    std::string sample_error(uint32_t node) const;
    // The clock MIDI messages are stamped with (Msg::time_ns): steady_clock in nanoseconds unless a
    // test gives its own. render() turns a message's age into its place in the device's buffer.
    void set_midi_clock(int64_t (*now_ns)());
    // The control changes (CC) that came by MIDI, for MIDI learn: one per call, in order, false
    // when there is none. (The UI thread. The audio thread passes them on as it feeds the
    // modules; while none is attached this call feeds the modules itself.)
    bool take_control(midi::Msg &out);
    void reset(); // every module back to its start, time 0, the key script from the top
    // Every module made again at a new rate (the audio device's), time 0.
    void set_sample_rate(uint32_t rate);

    // While an audio thread is attached, edits wait for render() on that thread. Attach before the
    // device starts calling render(), detach after it has stopped for good.
    void attach_audio(bool on);
    bool audio_attached() const { return attached_; }
    // Frees plans and module states the audio thread has let go of (every UI call does it too).
    void collect();
    int  telemetry_slot(uint32_t node) const; // -1 if the node has no module
    // An oscilloscope node's newest frame, if one came since the last call (and it is a scope).
    bool scope_frame(uint32_t node, ScopeFrame &out);

    // ---- the audio thread (or the UI thread while none is attached)
    void render(size_t frames, float *left, float *right); // any number of frames; overwritten

    // ---- either thread
    Telemetry &telemetry() { return telemetry_; }
    uint64_t   time() const { return time_.load(std::memory_order_relaxed); }
    uint32_t   sample_rate() const { return sample_rate_; }
    uint32_t failed_node() const { return failed_node_; } // refused its settings at compile(), or 0
    uint32_t apply_failures() const { return apply_failures_.load(); } // setters refused (stays 0)
    uint32_t nonfinite_samples() const { return nonfinite_.load(); }   // NaN/inf at the output (0)

    // ---- tests (detached only)
    const void *state(uint32_t node) const;
    const Plan &plan() const;

private:
    struct StateDeleter
    {
        const ModuleDesc *desc;
        void              operator()(void *p) const
        {
            if (desc->destroy)
                desc->destroy(p);
            ::operator delete(p, std::align_val_t(desc->state_align));
        }
    };
    using StateMem = std::unique_ptr<void, StateDeleter>;
    struct Live
    {
        const ModuleDesc *desc;
        StateMem          mem;
        int               slot;
        std::string       sample; // the file a Sampler was made with (compile() remakes on a change)
    };
    struct ScriptEvent
    {
        uint64_t at;
        void    *state;
        uint8_t  type, note, velocity;
    };
    struct Probe // one plan step, for telemetry
    {
        int               slot;
        const ModuleDesc *desc;
        void             *state;
    };
    // What the audio thread runs: built on the UI thread, immutable once published.
    struct Snapshot
    {
        uint64_t                 gen = 0;
        bool                     ok = false;      // false: the plan could not be built; silence
        bool                     restart = false; // time back to 0 when adopted
        Plan                     plan;
        std::vector<ScriptEvent> script;
        // the modules that take MIDI: each one's function (modules_common.h, midi_sink) and state
        std::vector<std::pair<void (*)(void *, const midi::Msg &, uint64_t), void *>> midi_ins;
        std::vector<Probe> probes;   // in plan order
        std::vector<float> activity; // per step, written by Plan::run
    };
    struct Cmd
    {
        enum Type : uint8_t
        {
            Param,
            Step,
            Key,
            Device // value: the slot, or kMidiAnyDevice / kMidiNoDevice
        } type;
        const ModuleDesc *desc; // Device
        void             *state;
        bool (*apply)(void *, float);
        bool (*apply_step)(void *, size_t, const StepData &);
        size_t   index;
        float    value;
        StepData step;
        uint8_t  key_type, note, velocity;
        int64_t  at;
    };

    bool      init_state(const Node &n, void *mem); // zero, init from the node's parameters, steps
    StateMem  make_state(const Node &n, bool *ok);
    void      retire(StateMem mem, uint64_t gen);
    Snapshot *build_snapshot(bool restart);
    void      publish(Snapshot *s);
    void      send(const Cmd &c);
    void      pump(); // detached: be the audio side now
    // the audio side
    void begin_block();
    void apply(const Cmd &c);
    void feed_script(uint64_t upto);
    void feed_midi(size_t frames); // frames: the device call's length, 0 outside one
    int  device_code(const Node &n) const;
    // A Sampler's file, read once and kept while the patch names it (null and `error` if it does
    // not load)
    std::shared_ptr<const SampleData> sample_data(const std::string &path, std::string *error);

    // UI-owned
    uint32_t                                   sample_rate_;
    Patch                                      patch_;
    std::map<uint32_t, Live>                   live_;
    std::vector<std::pair<uint64_t, StateMem>> dead_; // freed once the audio thread adopted gen
    std::vector<int>                           free_slots_;
    int                                        next_slot_ = 0;
    uint64_t                                   gen_ = 0;
    uint32_t                                   failed_node_ = 0;
    std::map<std::string, std::shared_ptr<const SampleData>> samples_;
    std::map<std::string, std::string>                       sample_errors_; // path -> why
    bool                                       attached_ = false;

    // shared
    std::atomic<Snapshot *>   pending_ { nullptr };
    SpscRing<Snapshot *, 256> retired_;
    std::atomic<uint64_t>     adopted_gen_ { 0 };
    SpscRing<Cmd, 4096>       cmds_;
    SpscRing<midi::Msg, 1024> controls_; // MIDI control changes, audio thread to UI (full: dropped)
    std::atomic<uint64_t>     time_ { 0 };
    std::atomic<uint32_t>     apply_failures_ { 0 }, nonfinite_ { 0 };
    Telemetry                 telemetry_;

    // audio-owned
    midi::Inboxes *midi_ = nullptr; // (set while detached)
    int64_t (*midi_now_)() = nullptr;
    Snapshot *cur_ = nullptr;
    size_t    script_pos_ = 0;
};

} // namespace rack
