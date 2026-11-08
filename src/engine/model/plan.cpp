#include "engine/model/plan.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace rack
{

namespace
{
    struct OutKey
    {
        uint32_t node;
        uint16_t port;
        bool     operator<(const OutKey &o) const
        {
            return node != o.node ? node < o.node : port < o.port;
        }
    };
} // namespace

bool Plan::build(const Patch &patch, const StateLookup &state_of, Plan &out)
{
    out = Plan();
    const auto                 nodes = patch.nodes();
    const auto                 cables = patch.cables();
    std::map<uint32_t, size_t> index; // node id -> index in nodes
    for (size_t i = 0; i < nodes.size(); i++)
        index[nodes[i].id] = i;

    // ---- order: Kahn's algorithm, always taking the smallest ready id
    std::map<uint32_t, int> indegree;
    for (const Node &n : nodes)
        indegree[n.id] = 0;
    for (const Cable &c : cables)
        indegree[c.to.node]++;
    std::set<uint32_t> ready;
    for (auto &[id, d] : indegree)
        if (d == 0)
            ready.insert(id);
    std::vector<uint32_t> order;
    while (!ready.empty())
    {
        const uint32_t id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        for (const Cable &c : cables)
            if (c.from.node == id && --indegree[c.to.node] == 0)
                ready.insert(c.to.node);
    }
    if (order.size() != nodes.size())
        return false;
    std::map<uint32_t, size_t> pos;
    for (size_t i = 0; i < order.size(); i++)
        pos[order[i]] = i;

    // ---- the last step that reads each output
    std::map<OutKey, size_t> last_reader;
    for (const Cable &c : cables)
    {
        size_t &l = last_reader[OutKey { c.from.node, c.from.port }];
        l = std::max(l, pos[c.to.node]);
    }

    size_t n_evt_out = 0;
    for (const Node &n : nodes)
        n_evt_out += count_slots(*n.desc).evt_out;
    out.evt_.reserve(n_evt_out);
    out.evt_data_.reserve(n_evt_out);
    out.scratch_ = std::make_unique<pcl_sample_t[]>(PCL_MAX_BLOCK);
    out.zeros_ = std::make_unique<pcl_sample_t[]>(PCL_MAX_BLOCK); // value-initialised: all zeros
    out.empty_ = std::make_unique<EventBuf>();

    // ---- walk the steps: give outputs buffers, resolve inputs, then release what this step read
    // for the last time
    std::vector<size_t>      free_sig;
    std::map<OutKey, size_t> sig_of; // output -> index in out.sig_
    std::map<OutKey, size_t> evt_of; // output -> index in out.evt_
    auto                     sig_buffer = [&]() -> size_t
    {
        if (!free_sig.empty())
        {
            const size_t b = free_sig.back();
            free_sig.pop_back();
            return b;
        }
        out.sig_.push_back(std::make_unique<pcl_sample_t[]>(PCL_MAX_BLOCK));
        return out.sig_.size() - 1;
    };

    // Pointers into the pools are made only after the pools are complete, so first record indexes
    // (-1 = nothing).
    struct Pending
    {
        std::vector<long> sig_in, sig_out, evt_in, evt_out;
    };
    std::vector<Pending> pending(order.size());

    for (size_t s = 0; s < order.size(); s++)
    {
        const Node       &node = nodes[index[order[s]]];
        const ModuleDesc &d = *node.desc;
        const PortSlots   slots = count_slots(d);
        Pending          &pd = pending[s];
        pd.sig_in.assign(slots.sig_in, -1);
        pd.sig_out.assign(slots.sig_out, -1);
        pd.evt_in.assign(slots.evt_in, -1);
        pd.evt_out.assign(slots.evt_out, -1);

        // inputs first: capacity of the event inputs is needed for the event outputs
        size_t in_cap = 0;
        for (uint16_t p = 0; p < d.ports.size(); p++)
        {
            const PortDesc &pr = d.ports[p];
            if (pr.is_output)
                continue;
            const Cable *c = patch.cable_into(PortRef { node.id, p });
            if (!c)
                continue;
            const OutKey k { c->from.node, c->from.port };
            if (pr.kind == Kind::Signal)
                pd.sig_in[slot_of(d, p)] = (long)sig_of.at(k);
            else
            {
                const size_t e = evt_of.at(k);
                pd.evt_in[slot_of(d, p)] = (long)e;
                in_cap += out.evt_[e].cap;
            }
        }
        // outputs
        for (uint16_t p = 0; p < d.ports.size(); p++)
        {
            const PortDesc &pr = d.ports[p];
            if (!pr.is_output)
                continue;
            const OutKey k { node.id, p };
            if (pr.kind == Kind::Signal)
            {
                if (last_reader.count(k))
                {
                    const size_t b = sig_buffer();
                    sig_of[k] = b;
                    pd.sig_out[slot_of(d, p)] = (long)b;
                }
            }
            else
            {
                const size_t cap = d.evt_out_cap ? d.evt_out_cap(in_cap) : 64;
                out.evt_data_.push_back(std::make_unique<pcl_event_t[]>(cap));
                out.evt_.push_back(EventBuf { out.evt_data_.back().get(), cap, 0 });
                evt_of[k] = out.evt_.size() - 1;
                pd.evt_out[slot_of(d, p)] = (long)(out.evt_.size() - 1);
            }
        }
        // release the signal buffers this step was the last to read
        for (auto &[k, last] : last_reader)
            if (last == s && sig_of.count(k))
                free_sig.push_back(sig_of[k]);
    }

    // ---- finalise the pools and make the spans
    for (size_t s = 0; s < order.size(); s++)
    {
        const Node    &node = nodes[index[order[s]]];
        const Pending &pd = pending[s];
        Step           st { };
        st.desc = node.desc;
        st.node = node.id;
        st.state = state_of(node.id);
        st.sig_in_at = out.sig_in_pool_.size();
        st.sig_out_at = out.sig_out_pool_.size();
        st.evt_in_at = out.evt_in_pool_.size();
        st.evt_out_at = out.evt_out_pool_.size();
        for (long b : pd.sig_in)
            out.sig_in_pool_.push_back(b < 0 ? nullptr : out.sig_[(size_t)b].get());
        for (long b : pd.sig_out)
            out.sig_out_pool_.push_back(b < 0 ? nullptr : out.sig_[(size_t)b].get());
        for (long b : pd.evt_in)
            out.evt_in_pool_.push_back(b < 0 ? out.empty_.get() : &out.evt_[(size_t)b]);
        for (long b : pd.evt_out)
            out.evt_out_pool_.push_back(&out.evt_[(size_t)b]);
        out.steps_.push_back(st);
    }
    for (Step &st : out.steps_)
    {
        const PortSlots slots = count_slots(*st.desc);
        st.io.sig_in = std::span<const pcl_sample_t *const>(
            out.sig_in_pool_.data() + st.sig_in_at, slots.sig_in);
        st.io.sig_out = std::span<pcl_sample_t *const>(
            out.sig_out_pool_.data() + st.sig_out_at, slots.sig_out);
        st.io.evt_in = std::span<const EventBuf *const>(
            out.evt_in_pool_.data() + st.evt_in_at, slots.evt_in);
        st.io.evt_out
            = std::span<EventBuf *const>(out.evt_out_pool_.data() + st.evt_out_at, slots.evt_out);
        st.io.scratch = out.scratch_.get();
        st.io.zeros = out.zeros_.get();
    }
    return true;
}

std::vector<uint32_t> Plan::order() const
{
    std::vector<uint32_t> o;
    for (const Step &s : steps_)
        o.push_back(s.node);
    return o;
}

void Plan::run(uint64_t time, size_t frames, float *left, float *right)
{
    for (Step &s : steps_)
    {
        for (EventBuf *e : s.io.evt_out)
            e->count = 0;
        s.io.master[0] = left;
        s.io.master[1] = right;
        s.io.time = time;
        s.io.frames = frames;
        s.desc->process(s.state, s.io);
    }
}

} // namespace rack
