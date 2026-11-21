// Module descriptors: one table entry per module type drives the browser, the panel renderer, the
// patch file, the engine's clamping and the tests (docs/patch-engine.md). Everything here is
// read-only data plus plain function pointers, so the audio thread can use it.
#pragma once

#include "pcl/pcl.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rack
{

enum class Kind : uint8_t
{
    Signal,
    Event
};
enum class Scale : uint8_t
{
    Linear,
    Log,
    Int,
    Enum,
    Toggle
}; // Int, Enum and Toggle are whole numbers
enum class Control : uint8_t
{
    Knob,
    Slider,
    Switch,
    Button,
    Selector,
    Hidden // no control of its own: the panel sets it some other way (FM Synth's envelope graph)
};

struct PortDesc
{
    std::string_view id, name;
    Kind             kind;
    bool             is_output;
};

struct ParamDesc
{
    std::string_view                  id, name, unit; // "cutoff", "Cutoff", "Hz"
    float                             min, max, def;  // natural units
    Scale                             scale;
    std::span<const std::string_view> labels; // Enum: the names of the choices (value = index)
    Control control; // where it sits on the panel is computed (src/ui/geometry.h)
    bool (*apply)(void *state,
        float value); // natural units -> the library's setter; false if the library refused
    // A setting the library fixes when the module is made (a reverb's size): a change makes the
    // module again from the patch (its sound starts over); `apply` is not used for it.
    bool rebuild = false;
};

// A module's own sound, chosen on its panel (FM Synth): the settings it changes, by parameter
// id; every other setting goes back to its default. The module's "preset" parameter (an Enum
// whose labels are the presets' names) records which was chosen last.
struct PresetValue
{
    std::string_view param;
    float            value;
};
struct Preset
{
    std::string_view              name;
    std::span<const PresetValue> values;
};

struct StepData
{ // one step of the sequencer
    uint8_t note = 60, velocity = 0, gate = 0, tie = 0;
    bool    operator==(const StepData &) const = default;
};
inline constexpr size_t kMaxSteps = PCL_SEQ_MAX_STEPS;

struct NodeContext
{
    uint32_t sample_rate;
};

struct EventBuf
{
    pcl_event_t *data = nullptr;
    size_t       cap = 0;
    size_t       count = 0;
};

// What a plugin's host gives one block (docs/reaper.md), for the modules that take it: the Host
// Clock (the transport), Audio In (the host's input) and an Audio Output on one of the extra
// outputs. In the standalone rack there is no transport, no input and no extra output.
inline constexpr size_t kExtraOutputs = 7; // outputs 2 .. 8 (output 1 is the main one, `master`)
struct HostBlock
{
    bool                transport = false; // the host says where it is (and whether it plays)
    bool                playing = false;
    double              tempo = 120; // beats a minute
    double              beat = 0;    // where the block's first sample is, in beats from the start
    const float        *in[2] = { nullptr, nullptr }; // the host's audio in (an effect), or none
    float              *out[kExtraOutputs][2] = { }; // the extra outputs (accumulated), or none
};

// What a wrapper sees for one block. Indexes are per kind and direction, in the order the ports are
// listed in the descriptor.
struct NodeIo
{
    std::span<const pcl_sample_t *const> sig_in; // nullptr when the input has no cable
    std::span<pcl_sample_t *const> sig_out; // nullptr when nothing is connected: "don't compute it"
    std::span<const EventBuf *const> evt_in;  // never nullptr; count 0 when unconnected
    std::span<EventBuf *const>       evt_out; // never nullptr; the wrapper sets count
    pcl_sample_t       *scratch;   // PCL_MAX_BLOCK samples, for a required output nobody reads
    const pcl_sample_t *zeros;     // PCL_MAX_BLOCK zero samples, for a required input nobody drives
    float              *master[2]; // host output (Audio Output only), accumulated
    uint64_t            time;      // sample index of the first sample of this block
    size_t              frames;    // 1 .. PCL_MAX_BLOCK
    const HostBlock    *host = nullptr; // never null while the plan runs (see HostBlock)
};

struct ModuleDesc
{
    std::string_view           id, name, category; // "osc", "Oscillator", "Source"
    uint8_t                    width_hp; // panel width in rack units (the height is fixed)
    size_t                     state_size, state_align;
    std::span<const PortDesc>  ports;
    std::span<const ParamDesc> params;
    bool                       has_steps; // the sequencer's 64 steps
    bool (*init)(void *state, const NodeContext &ctx,
        const float *params); // state is zeroed; params as in `params`
    void (*reset)(void *state);
    void (*process)(void *state, NodeIo &io);
    bool (*apply_step)(void *state, size_t index, const StepData &step);
    size_t (*evt_out_cap)(size_t
            total_in_cap); // event output capacity from the capacity of its event inputs; null = 64
    std::string_view description; // one line for the browser's card
    // Frees what init allocated (a delay line), or null when init allocates nothing. Called on the
    // UI thread when the state is let go, also after a failed init (the state was zeroed first).
    void (*destroy)(void *state) = nullptr;
    std::span<const Preset> presets = { }; // its own sounds, or none
};

// Where a port sits among the ports of its kind and direction (the index a wrapper uses).
struct PortSlots
{
    size_t sig_in = 0, sig_out = 0, evt_in = 0, evt_out = 0; // counts
};
PortSlots count_slots(const ModuleDesc &d);

// The index of the module's "preset" parameter, or -1 when it has none (or no presets).
int preset_param(const ModuleDesc &d);
// Every parameter's value for preset `index` (clamped to the presets there are): the preset's
// own values, the defaults for the rest, and `index` for the preset parameter itself.
std::vector<float> preset_values(const ModuleDesc &d, size_t index);
size_t    slot_of(const ModuleDesc &d, size_t port_index);

} // namespace rack
