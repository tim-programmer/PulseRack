#include "engine/engine.h"

#include "engine/rt/modules_common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace rack
{

Engine::Engine(uint32_t sample_rate)
    : sample_rate_(sample_rate)
{
}
Engine::~Engine() = default;

const void *Engine::state(uint32_t node) const
{
    auto it = live_.find(node);
    return it == live_.end() ? nullptr : it->second.mem.get();
}

bool Engine::init_state(const Node &n, void *mem)
{
    const ModuleDesc &d = *n.desc;
    std::memset(mem, 0, d.state_size);
    if (!d.init(mem, NodeContext { sample_rate_ }, n.params.data()))
        return false;
    if (d.has_steps)
        for (size_t i = 0; i < n.steps.size(); i++)
            if (!(n.steps[i] == StepData { }) && !d.apply_step(mem, i, n.steps[i]))
                apply_failures_++;
    return true;
}

Err Engine::compile()
{
    failed_node_ = 0;
    // modules that are gone
    for (auto it = live_.begin(); it != live_.end();)
        it = patch_.find(it->first) ? std::next(it) : live_.erase(it);

    // modules that are new
    for (const Node &n : patch_.nodes())
    {
        if (live_.count(n.id))
            continue;
        const ModuleDesc                   &d = *n.desc;
        std::unique_ptr<void, StateDeleter> mem(
            ::operator new(d.state_size, std::align_val_t(d.state_align)),
            StateDeleter { d.state_align });
        if (!init_state(n, mem.get()))
        {
            failed_node_ = n.id;
            return Err::InitFailed;
        }
        live_[n.id] = Live { &d, std::move(mem) };
    }

    // the key script, in time order (stable, so the order written is kept at one sample)
    script_.clear();
    for (const KeyScript &k : patch_.keys)
    {
        const Node *n = patch_.find(k.node);
        if (!n || n->desc != &kKeyboardDesc)
            continue;
        script_.push_back(ScriptEvent { (uint64_t)std::llround(k.seconds * (double)sample_rate_),
            k.node, (uint8_t)(k.on ? PCL_EVT_NOTE_ON : PCL_EVT_NOTE_OFF), k.note,
            k.on ? k.velocity : (uint8_t)0 });
    }
    std::stable_sort(script_.begin(), script_.end(),
        [](const ScriptEvent &a, const ScriptEvent &b) { return a.at < b.at; });
    script_pos_ = 0;
    while (script_pos_ < script_.size() && script_[script_pos_].at < time_)
        script_pos_++; // already in the past

    have_plan_ = Plan::build(patch_, [this](uint32_t id) { return live_.at(id).mem.get(); }, plan_);
    return have_plan_ ? Err::Ok : Err::BadPatch;
}

Err Engine::set_param(uint32_t node, size_t param, float value)
{
    float     stored = 0;
    const Err e = patch_.set_param(node, param, value, &stored);
    if (e != Err::Ok)
        return e;
    auto it = live_.find(node);
    if (it != live_.end() && !it->second.desc->params[param].apply(it->second.mem.get(), stored))
        apply_failures_++;
    return Err::Ok;
}

Err Engine::set_step(uint32_t node, size_t index, const StepData &step)
{
    const Err e = patch_.set_step(node, index, step);
    if (e != Err::Ok)
        return e;
    auto it = live_.find(node);
    if (it != live_.end()
        && !it->second.desc->apply_step(
            it->second.mem.get(), index, patch_.find(node)->steps[index]))
        apply_failures_++;
    return Err::Ok;
}

Err Engine::key(uint32_t node, bool on, uint8_t note, uint8_t velocity, int64_t at)
{
    auto it = live_.find(node);
    if (it == live_.end())
        return Err::NoSuchNode;
    if (it->second.desc != &kKeyboardDesc)
        return Err::NotAKeyboard;
    const uint64_t when = at < 0 ? time_ : (uint64_t)at;
    keyboard_push(it->second.mem.get(), when, on ? PCL_EVT_NOTE_ON : PCL_EVT_NOTE_OFF, note,
        on ? velocity : (uint8_t)0);
    return Err::Ok;
}

void Engine::feed_script(uint64_t upto)
{
    while (script_pos_ < script_.size() && script_[script_pos_].at < upto)
    {
        const ScriptEvent &e = script_[script_pos_++];
        auto               it = live_.find(e.node);
        if (it != live_.end())
            keyboard_push(it->second.mem.get(), e.at, e.type, e.note, e.velocity);
    }
}

// A library module's own reset keeps what was set since it was made (an oscillator's frequency,
// after notes), so a reset of the whole rack makes every module again from the patch's settings:
// the result is the same as a rack that was just compiled.
void Engine::reset()
{
    for (auto &[id, l] : live_)
        if (const Node *n = patch_.find(id))
            (void)init_state(*n, l.mem.get());
    time_ = 0;
    script_pos_ = 0;
}

void Engine::render(size_t frames, float *left, float *right)
{
    size_t done = 0;
    while (done < frames)
    {
        const size_t n = std::min<size_t>(frames - done, PCL_MAX_BLOCK);
        float       *l = left + done;
        float       *r = right + done;
        std::memset(l, 0, n * sizeof(float));
        std::memset(r, 0, n * sizeof(float));
        if (have_plan_)
        {
            feed_script(time_ + n);
            plan_.run(time_, n, l, r);
        }
        for (size_t i = 0; i < n;
            i++) // the hard clamp at the host boundary; a NaN or infinity is silenced and counted
        {
            if (!std::isfinite(l[i]))
            {
                l[i] = 0.0f;
                nonfinite_++;
            }
            if (!std::isfinite(r[i]))
            {
                r[i] = 0.0f;
                nonfinite_++;
            }
            l[i] = std::clamp(l[i], -1.0f, 1.0f);
            r[i] = std::clamp(r[i], -1.0f, 1.0f);
        }
        time_ += n;
        done += n;
    }
}

} // namespace rack
