#include "engine/model/patch.h"

#include <algorithm>
#include <cmath>

namespace rack
{

const char *describe(Err e)
{
    switch (e)
    {
    case Err::Ok:
        return "ok";
    case Err::NoSuchNode:
        return "no such module";
    case Err::NoSuchPort:
        return "no such jack";
    case Err::NoSuchParam:
        return "no such control";
    case Err::NoSuchStep:
        return "no such step";
    case Err::NotAnOutput:
        return "a cable must start at an output";
    case Err::NotAnInput:
        return "a cable must end at an input";
    case Err::KindMismatch:
        return "a signal cannot go to an event input, nor an event to a signal input";
    case Err::InputInUse:
        return "this input already has a cable";
    case Err::WouldLoop:
        return "this would make a loop";
    case Err::NotAKeyboard:
        return "this module takes no key events";
    case Err::InitFailed:
        return "a module refused its settings";
    case Err::BadPatch:
        return "the patch is not valid";
    }
    return "?";
}

float clamp_param(const ParamDesc &p, float v)
{
    if (std::isnan(v))
        return p.def;
    v = std::clamp(v, p.min, p.max);
    if (p.scale == Scale::Int || p.scale == Scale::Enum || p.scale == Scale::Toggle)
        v = std::round(v);
    return v;
}

uint32_t Patch::add_node(const ModuleDesc &desc, float x, float y)
{
    while (find(next_id_))
        next_id_++;
    const uint32_t id = next_id_++;
    (void)add_node_with_id(desc, id, x, y);
    return id;
}

bool Patch::add_node_with_id(const ModuleDesc &desc, uint32_t id, float x, float y)
{
    if (id == 0 || find(id))
        return false;
    Node n;
    n.id = id;
    n.desc = &desc;
    n.x = x;
    n.y = y;
    for (const ParamDesc &p : desc.params)
        n.params.push_back(p.def);
    if (desc.has_steps)
        n.steps.assign(kMaxSteps, StepData { });
    nodes_.push_back(std::move(n));
    return true;
}

Err Patch::remove_node(uint32_t id)
{
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [&](const Node &n) { return n.id == id; });
    if (it == nodes_.end())
        return Err::NoSuchNode;
    nodes_.erase(it);
    std::erase_if(cables_, [&](const Cable &c) { return c.from.node == id || c.to.node == id; });
    std::erase_if(keys, [&](const KeyScript &k) { return k.node == id; });
    std::erase_if(mappings, [&](const Mapping &m) { return m.node == id; });
    return Err::Ok;
}

const Node *Patch::find(uint32_t id) const
{
    for (const Node &n : nodes_)
        if (n.id == id)
            return &n;
    return nullptr;
}
Node *Patch::find(uint32_t id)
{
    return const_cast<Node *>(static_cast<const Patch *>(this)->find(id));
}

const Cable *Patch::cable_into(PortRef to) const
{
    for (const Cable &c : cables_)
        if (c.to == to)
            return &c;
    return nullptr;
}

bool Patch::reaches(uint32_t from_node, uint32_t to_node) const
{
    if (from_node == to_node)
        return true;
    std::vector<uint32_t> stack { from_node }, seen;
    while (!stack.empty())
    {
        const uint32_t n = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), n) != seen.end())
            continue;
        seen.push_back(n);
        for (const Cable &c : cables_)
        {
            if (c.from.node != n)
                continue;
            if (c.to.node == to_node)
                return true;
            stack.push_back(c.to.node);
        }
    }
    return false;
}

Err Patch::connect(PortRef from, PortRef to)
{
    const Err e = can_connect(from, to);
    if (e == Err::Ok)
        cables_.push_back(Cable { from, to });
    return e;
}

Err Patch::can_connect(PortRef from, PortRef to) const
{
    const Node *a = find(from.node);
    const Node *b = find(to.node);
    if (!a || !b)
        return Err::NoSuchNode;
    if (from.port >= a->desc->ports.size() || to.port >= b->desc->ports.size())
        return Err::NoSuchPort;
    const PortDesc &pa = a->desc->ports[from.port];
    const PortDesc &pb = b->desc->ports[to.port];
    if (!pa.is_output)
        return Err::NotAnOutput;
    if (pb.is_output)
        return Err::NotAnInput;
    if (pa.kind != pb.kind)
        return Err::KindMismatch;
    if (cable_into(to))
        return Err::InputInUse;
    if (reaches(to.node, from.node))
        return Err::WouldLoop; // includes a cable from a module to itself
    return Err::Ok;
}

Err Patch::disconnect(PortRef to)
{
    auto it
        = std::find_if(cables_.begin(), cables_.end(), [&](const Cable &c) { return c.to == to; });
    if (it == cables_.end())
        return Err::NoSuchPort;
    cables_.erase(it);
    return Err::Ok;
}

Err Patch::set_param(uint32_t node, size_t param, float value, float *stored)
{
    Node *n = find(node);
    if (!n)
        return Err::NoSuchNode;
    if (param >= n->desc->params.size())
        return Err::NoSuchParam;
    n->params[param] = clamp_param(n->desc->params[param], value);
    if (stored)
        *stored = n->params[param];
    return Err::Ok;
}

Err Patch::set_step(uint32_t node, size_t index, const StepData &step)
{
    Node *n = find(node);
    if (!n)
        return Err::NoSuchNode;
    if (!n->desc->has_steps || index >= n->steps.size())
        return Err::NoSuchStep;
    StepData s = step;
    s.note = std::min<uint8_t>(s.note, 127);
    s.velocity = std::min<uint8_t>(s.velocity, 127);
    s.gate = std::min<uint8_t>(s.gate, 96);
    s.tie = s.tie ? 1 : 0;
    n->steps[index] = s;
    return Err::Ok;
}

} // namespace rack
