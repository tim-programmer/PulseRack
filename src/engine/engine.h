// The engine: a patch plus the live module states and the compiled plan. Single-threaded for now:
// the command ring and the atomic plan swap of docs/architecture.md arrive with the audio device
// (milestone 4). Everything here is what the audio thread will later call (render) or what the UI
// thread will call (the rest).
#pragma once

#include "engine/model/patch.h"
#include "engine/model/plan.h"

#include <map>
#include <memory>
#include <string>

namespace rack
{

class Engine
{
public:
    explicit Engine(uint32_t sample_rate);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    Patch       &patch() { return patch_; }
    const Patch &patch() const { return patch_; }

    // Brings the live modules in line with the patch (new nodes are created from their parameters,
    // removed ones destroyed, existing ones keep their state) and builds a new plan. Call after
    // editing the patch's nodes or cables.
    Err compile();

    // A parameter change: the model and the running module.
    Err set_param(uint32_t node, size_t param, float value);
    Err set_step(uint32_t node, size_t index, const StepData &step);
    // A live key press for a Keyboard node, heard at the start of the next block (or at `at`, an
    // absolute sample).
    Err key(uint32_t node, bool on, uint8_t note, uint8_t velocity = 100, int64_t at = -1);

    void reset(); // every module back to its start, time 0, the key script from the top
    void render(size_t frames, float *left,
        float *right); // any number of frames; the buffers are overwritten

    uint64_t time() const { return time_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t failed_node() const
    {
        return failed_node_;
    } // the module whose settings the library refused at compile(), or 0
    uint32_t apply_failures() const
    {
        return apply_failures_;
    } // setters the library refused (should stay 0)
    uint32_t nonfinite_samples() const
    {
        return nonfinite_;
    } // NaN or infinity that reached the output (should stay 0)
    const void *state(uint32_t node) const; // tests and telemetry
    const Plan &plan() const { return plan_; }

private:
    struct StateDeleter
    {
        size_t align;
        void   operator()(void *p) const { ::operator delete(p, std::align_val_t(align)); }
    };
    struct Live
    {
        const ModuleDesc                   *desc;
        std::unique_ptr<void, StateDeleter> mem;
    };
    struct ScriptEvent
    {
        uint64_t at;
        uint32_t node;
        uint8_t  type, note, velocity;
    };
    void feed_script(uint64_t upto);
    bool init_state(
        const Node &n, void *mem); // zero, init from the node's parameters, load its steps

    uint32_t                 sample_rate_;
    Patch                    patch_;
    std::map<uint32_t, Live> live_;
    Plan                     plan_;
    bool                     have_plan_ = false;
    uint64_t                 time_ = 0;
    std::vector<ScriptEvent> script_;
    size_t                   script_pos_ = 0;
    uint32_t                 apply_failures_ = 0;
    uint32_t                 nonfinite_ = 0;
    uint32_t                 failed_node_ = 0;
};

} // namespace rack
