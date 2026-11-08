// The patch model: nodes, cables, parameter values. Pure data and rules, no audio. Every edit that
// could break the graph is checked here, so the engine can assume a patch is valid: a cable joins
// an output to an input of the same kind, an input takes one cable, and the graph has no loop.
#pragma once

#include "engine/model/descriptor.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rack
{

struct PortRef
{
    uint32_t node = 0;
    uint16_t port = 0; // index into the descriptor's port list
    bool     operator==(const PortRef &) const = default;
};
struct Cable
{
    PortRef from, to;
};
struct Node
{
    uint32_t              id = 0;
    const ModuleDesc     *desc = nullptr;
    float                 x = 0, y = 0; // rack position (milestone 2)
    std::vector<float>    params;       // natural units, one per descriptor parameter
    std::vector<StepData> steps;        // the sequencer's steps (empty for other types)
};
// A scripted key press for a Keyboard node, so a patch can play itself (rack-render, tests, factory
// patches).
struct KeyScript
{
    uint32_t node = 0;
    double   seconds = 0;
    bool     on = true;
    uint8_t  note = 60, velocity = 100;
};

enum class Err
{
    Ok,
    NoSuchNode,
    NoSuchPort,
    NoSuchParam,
    NoSuchStep,
    NotAnOutput,
    NotAnInput,
    KindMismatch,
    InputInUse,
    WouldLoop,
    NotAKeyboard,
    InitFailed,
    BadPatch
};
const char *describe(Err e); // for the hint line

float clamp_param(const ParamDesc &p,
    float v); // into [min, max]; whole numbers for Int, Enum and Toggle; NaN becomes the default

class Patch
{
public:
    uint32_t add_node(const ModuleDesc &desc, float x = 0, float y = 0); // returns the new id
    bool     add_node_with_id(const ModuleDesc &desc, uint32_t id, float x = 0,
        float y = 0);                  // false if the id is taken (file loading)
    Err      remove_node(uint32_t id); // and its cables
    Err      connect(PortRef from, PortRef to);
    Err      disconnect(PortRef to); // an input has at most one cable
    Err      set_param(uint32_t node, size_t param, float value, float *stored = nullptr);
    Err      set_step(uint32_t node, size_t index, const StepData &step);

    const Node            *find(uint32_t id) const;
    Node                  *find(uint32_t id);
    std::span<const Node>  nodes() const { return nodes_; }
    std::span<const Cable> cables() const { return cables_; }
    const Cable           *cable_into(PortRef to) const;
    bool                   reaches(uint32_t from_node,
        uint32_t          to_node) const; // is there a path of cables from one node to the other?

    std::vector<KeyScript> keys;

private:
    std::vector<Node>  nodes_;
    std::vector<Cable> cables_;
    uint32_t           next_id_ = 1;
};

} // namespace rack
