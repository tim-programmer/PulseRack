#include "engine/model/patchfile.h"

#include "engine/model/registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace rack
{

namespace
{

    std::string num(double v)
    {
        char b[40];
        std::snprintf(b, sizeof b, "%.9g", v);
        return b;
    }

    bool parse_double(std::string_view t, double &v)
    {
        if (t.empty())
            return false;
        const std::string s(t);
        char             *end = nullptr;
        v = std::strtod(s.c_str(), &end);
        return end == s.c_str() + s.size() && std::isfinite(v);
    }
    bool parse_uint(std::string_view t, uint32_t &v, uint32_t max)
    {
        double d;
        if (!parse_double(t, d) || d < 0 || d > (double)max || d != std::floor(d))
            return false;
        v = (uint32_t)d;
        return true;
    }

    // An enumerated choice is written by its name, with a space written as '_' ('saw up' is saw_up)
    // so that a record stays one line of words.
    std::string file_label(std::string_view label)
    {
        std::string s(label);
        std::replace(s.begin(), s.end(), ' ', '_');
        return s;
    }

    std::vector<std::string_view> split(std::string_view line)
    {
        std::vector<std::string_view> t;
        size_t                        i = 0;
        while (i < line.size())
        {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
                i++;
            const size_t s = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
                i++;
            if (i > s)
                t.push_back(line.substr(s, i - s));
        }
        return t;
    }

    // "key=value" -> value, or empty if the token does not start with "key="
    std::string_view option(std::string_view tok, std::string_view key)
    {
        if (tok.size() > key.size() && tok.substr(0, key.size()) == key && tok[key.size()] == '=')
            return tok.substr(key.size() + 1);
        return { };
    }

} // namespace

std::string save_patch(const Patch &patch)
{
    std::string               s = "pulserack " + std::to_string(kPatchFileVersion) + "\n";
    std::vector<const Node *> nodes;
    for (const Node &n : patch.nodes())
        nodes.push_back(&n);
    std::sort(
        nodes.begin(), nodes.end(), [](const Node *a, const Node *b) { return a->id < b->id; });

    for (const Node *n : nodes)
    {
        const ModuleDesc &d = *n->desc;
        s += "node " + std::to_string(n->id) + " " + std::string(d.id) + " x=" + num(n->x)
            + " y=" + num(n->y) + "\n";
        for (size_t i = 0; i < d.params.size(); i++)
        {
            const ParamDesc &p = d.params[i];
            s += "param " + std::to_string(n->id) + " " + std::string(p.id) + " ";
            s += p.scale == Scale::Enum && (size_t)n->params[i] < p.labels.size()
                ? file_label(p.labels[(size_t)n->params[i]])
                : num(n->params[i]);
            s += "\n";
        }
        for (size_t i = 0; i < n->steps.size(); i++)
        {
            const StepData &st = n->steps[i];
            if (st == StepData { })
                continue;
            s += "step " + std::to_string(n->id) + " " + std::to_string(i)
                + " note=" + std::to_string(st.note) + " vel=" + std::to_string(st.velocity)
                + " gate=" + std::to_string(st.gate) + " tie=" + std::to_string(st.tie) + "\n";
        }
    }
    for (const KeyScript &k : patch.keys)
        s += "key " + std::to_string(k.node) + " " + num(k.seconds) + (k.on ? " on " : " off ")
            + std::to_string(k.note) + " " + std::to_string(k.velocity) + "\n";

    std::vector<Cable> cables(patch.cables().begin(), patch.cables().end());
    std::sort(cables.begin(), cables.end(), [](const Cable &a, const Cable &b)
        { return a.to.node != b.to.node ? a.to.node < b.to.node : a.to.port < b.to.port; });
    for (const Cable &c : cables)
        s += "cable " + std::to_string(c.from.node) + "."
            + std::string(patch.find(c.from.node)->desc->ports[c.from.port].id) + " -> "
            + std::to_string(c.to.node) + "."
            + std::string(patch.find(c.to.node)->desc->ports[c.to.port].id) + "\n";
    return s;
}

LoadResult load_patch(std::string_view text, Patch &out)
{
    LoadResult         r;
    Patch              p;
    std::set<uint32_t> skipped; // ids of nodes of an unknown type
    size_t             lineno = 0, pos = 0;
    bool               header = false;
    auto               fail = [&](const std::string &why)
    {
        r.ok = false;
        r.error = "line " + std::to_string(lineno) + ": " + why;
        return r;
    };

    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos)
            nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        lineno++;
        if (line.size() > kMaxLine)
            return fail("line too long");
        const size_t hash = line.find('#');
        if (hash != std::string_view::npos)
            line = line.substr(0, hash);
        const auto t = split(line);
        if (t.empty())
            continue;

        if (!header)
        {
            uint32_t v;
            if (t[0] != "pulserack" || t.size() != 2 || !parse_uint(t[1], v, 1000000))
                return fail("not a pulserack patch file");
            if ((int)v != kPatchFileVersion)
                return fail("unknown file version " + std::to_string(v));
            header = true;
            continue;
        }

        if (t[0] == "node")
        {
            uint32_t id;
            if (t.size() < 3 || !parse_uint(t[1], id, 1000000) || id == 0)
                return fail("bad node record");
            if (p.nodes().size() + skipped.size() >= kMaxNodes)
                return fail("too many modules");
            const ModuleDesc *d = find_module(t[2]);
            if (!d)
            {
                skipped.insert(id);
                r.warnings.push_back("line " + std::to_string(lineno) + ": unknown module type '"
                    + std::string(t[2]) + "', skipped");
                continue;
            }
            double x = 0, y = 0;
            for (size_t i = 3; i < t.size(); i++)
            {
                if (auto v = option(t[i], "x"); !v.empty())
                {
                    if (!parse_double(v, x))
                        return fail("bad x");
                }
                else if (auto w = option(t[i], "y"); !w.empty())
                {
                    if (!parse_double(w, y))
                        return fail("bad y");
                }
                else
                    return fail("unknown option '" + std::string(t[i]) + "'");
            }
            if (!p.add_node_with_id(*d, id, (float)x, (float)y))
                return fail("module id " + std::to_string(id) + " used twice");
        }
        else if (t[0] == "param")
        {
            uint32_t id;
            if (t.size() != 4 || !parse_uint(t[1], id, 1000000))
                return fail("bad param record");
            if (skipped.count(id))
                continue;
            Node *n = p.find(id);
            if (!n)
                return fail("param for a module that is not defined yet");
            const int pi = find_param(*n->desc, t[2]);
            if (pi < 0)
            {
                r.warnings.push_back("line " + std::to_string(lineno) + ": "
                    + std::string(n->desc->id) + " has no control '" + std::string(t[2])
                    + "', skipped");
                continue;
            }
            const ParamDesc &pd = n->desc->params[(size_t)pi];
            double           v;
            if (!parse_double(t[3], v))
            {
                size_t k = 0;
                while (k < pd.labels.size() && file_label(pd.labels[k]) != t[3])
                    k++;
                if (k == pd.labels.size())
                    return fail("bad value '" + std::string(t[3]) + "' for " + std::string(pd.id));
                v = (double)k;
            }
            const float stored = clamp_param(pd, (float)v);
            if (stored != (float)v)
                r.warnings.push_back("line " + std::to_string(lineno) + ": " + std::string(pd.id)
                    + " " + std::string(t[3]) + " is outside its range, clamped");
            n->params[(size_t)pi] = stored;
        }
        else if (t[0] == "step")
        {
            uint32_t id, idx;
            if (t.size() < 3 || !parse_uint(t[1], id, 1000000)
                || !parse_uint(t[2], idx, kMaxSteps - 1))
                return fail("bad step record");
            if (skipped.count(id))
                continue;
            Node *n = p.find(id);
            if (!n || !n->desc->has_steps)
                return fail("step for a module that has no steps");
            StepData st;
            for (size_t i = 3; i < t.size(); i++)
            {
                uint32_t v;
                if (auto a = option(t[i], "note"); !a.empty())
                {
                    if (!parse_uint(a, v, 127))
                        return fail("bad note");
                    st.note = (uint8_t)v;
                }
                else if (auto b = option(t[i], "vel"); !b.empty())
                {
                    if (!parse_uint(b, v, 127))
                        return fail("bad velocity");
                    st.velocity = (uint8_t)v;
                }
                else if (auto c = option(t[i], "gate"); !c.empty())
                {
                    if (!parse_uint(c, v, 96))
                        return fail("bad gate");
                    st.gate = (uint8_t)v;
                }
                else if (auto e = option(t[i], "tie"); !e.empty())
                {
                    if (!parse_uint(e, v, 1))
                        return fail("bad tie");
                    st.tie = (uint8_t)v;
                }
                else
                    return fail("unknown option '" + std::string(t[i]) + "'");
            }
            n->steps[idx] = st;
        }
        else if (t[0] == "key")
        {
            uint32_t id, note, vel = 100;
            double   sec;
            if (t.size() < 5 || t.size() > 6 || !parse_uint(t[1], id, 1000000)
                || !parse_double(t[2], sec) || sec < 0 || (t[3] != "on" && t[3] != "off")
                || !parse_uint(t[4], note, 127) || (t.size() == 6 && !parse_uint(t[5], vel, 127)))
                return fail("bad key record");
            if (skipped.count(id))
                continue;
            const Node *n = p.find(id);
            if (!n)
                return fail("key for a module that is not defined yet");
            if (p.keys.size() >= kMaxKeys)
                return fail("too many key events");
            p.keys.push_back(KeyScript { id, sec, t[3] == "on", (uint8_t)note, (uint8_t)vel });
        }
        else if (t[0] == "cable")
        {
            if (t.size() != 4 || t[2] != "->")
                return fail("bad cable record");
            if (p.cables().size() >= kMaxCables)
                return fail("too many cables");
            struct End
            {
                uint32_t    node;
                std::string port;
            } e[2];
            for (int k = 0; k < 2; k++)
            {
                const std::string_view tok = t[k == 0 ? 1 : 3];
                const size_t           dot = tok.find('.');
                if (dot == std::string_view::npos
                    || !parse_uint(tok.substr(0, dot), e[k].node, 1000000))
                    return fail("bad cable end '" + std::string(tok) + "'");
                e[k].port = std::string(tok.substr(dot + 1));
            }
            if (skipped.count(e[0].node) || skipped.count(e[1].node))
                continue;
            const Node *a = p.find(e[0].node);
            const Node *b = p.find(e[1].node);
            if (!a || !b)
                return fail("cable to a module that is not defined yet");
            const int pa = find_port(*a->desc, e[0].port), pb = find_port(*b->desc, e[1].port);
            if (pa < 0 || pb < 0)
            {
                r.warnings.push_back(
                    "line " + std::to_string(lineno) + ": unknown jack in cable, skipped");
                continue;
            }
            const Err err
                = p.connect(PortRef { a->id, (uint16_t)pa }, PortRef { b->id, (uint16_t)pb });
            if (err != Err::Ok)
                return fail(std::string("cable refused: ") + describe(err));
        }
        else
            return fail("unknown record '" + std::string(t[0]) + "'");
    }
    if (!header)
    {
        lineno = 1;
        return fail("empty file");
    }
    out = std::move(p);
    r.ok = true;
    return r;
}

} // namespace rack
