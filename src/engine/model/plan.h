// The compiled patch: the nodes in processing order, with every input resolved to a pointer into a
// buffer (or to nothing). Built on the UI thread, immutable once built; the audio thread only calls
// run().
#pragma once

#include "engine/model/patch.h"

#include <functional>
#include <memory>
#include <vector>

namespace rack
{

class Plan
{
public:
    using StateLookup = std::function<void *(uint32_t node)>;

    // Order: a topological sort with ties broken by node id, so a patch always renders the same
    // samples. Buffers: one PCL_MAX_BLOCK-sample buffer per connected signal output, reused once
    // its last reader has run; one event buffer per event output, sized by the producing module's
    // rule. Returns false for a graph with a loop.
    static bool build(const Patch &patch, const StateLookup &state_of, Plan &out);

    // Renders one block (1 .. PCL_MAX_BLOCK frames). `left` and `right` are float buffers the Audio
    // Output modules add into. `activity`, if given, gets one value per step (in order()): the
    // largest |sample| of its connected signal outputs, or 1 if it sent an event.
    void run(uint64_t time, size_t frames, float *left, float *right, float *activity = nullptr);

    size_t                step_count() const { return steps_.size(); }
    std::vector<uint32_t> order() const; // node ids, in processing order
    size_t                signal_buffer_count() const { return sig_.size(); }
    size_t                event_buffer_count() const { return evt_.size(); }

private:
    struct Step
    {
        const ModuleDesc *desc;
        uint32_t          node;
        void             *state;
        NodeIo            io;
        size_t            sig_in_at, sig_out_at, evt_in_at,
            evt_out_at; // offsets into the pools (the spans are made last)
    };
    std::vector<Step>                            steps_;
    std::vector<std::unique_ptr<pcl_sample_t[]>> sig_;
    std::vector<std::unique_ptr<pcl_event_t[]>>  evt_data_;
    std::vector<EventBuf>                        evt_;
    std::unique_ptr<EventBuf> empty_; // on the heap: the steps point at it, and a Plan may be moved
    std::vector<const pcl_sample_t *> sig_in_pool_;
    std::vector<pcl_sample_t *>       sig_out_pool_;
    std::vector<const EventBuf *>     evt_in_pool_;
    std::vector<EventBuf *>           evt_out_pool_;
    std::unique_ptr<pcl_sample_t[]>   scratch_, zeros_;
};

} // namespace rack
