#include "engine/engine.h"

#include "engine/rt/modules_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

namespace rack
{

Engine::Engine(uint32_t sample_rate)
    : sample_rate_(sample_rate)
{
}

Engine::~Engine()
{
    // no audio thread may be running now (its owner stopped it first)
    delete pending_.exchange(nullptr);
    Snapshot *s = nullptr;
    while (retired_.pop(s))
        delete s;
    delete cur_;
}

// =====================================================================================================
// The UI thread

const void *Engine::state(uint32_t node) const
{
    auto it = live_.find(node);
    return it == live_.end() ? nullptr : it->second.mem.get();
}

const Plan &Engine::plan() const
{
    static const Plan empty;
    return cur_ ? cur_->plan : empty;
}

bool Engine::scope_frame(uint32_t node, ScopeFrame &out)
{
    auto it = live_.find(node);
    return it != live_.end() && scope_take(*it->second.desc, it->second.mem.get(), out);
}

int Engine::telemetry_slot(uint32_t node) const
{
    auto it = live_.find(node);
    return it == live_.end() ? -1 : it->second.slot;
}

bool Engine::init_state(const Node &n, void *mem)
{
    const ModuleDesc &d = *n.desc;
    std::memset(mem, 0, d.state_size);
    if (!d.init(mem, NodeContext { sample_rate_ }, n.params.data()))
        return false;
    midi_set_device(d, mem, device_code(n)); // (a MIDI module; nothing for the others)
    if (takes_sample(d) && !n.sample.empty())
    { // a Sampler gets its file's frames (one that does not load plays nothing: the patch still
      // opens, and sample_error says why)
        std::string err;
        if (const auto data = sample_data(n.sample, &err); data && !load_sample(d, mem, *data))
            sample_errors_[n.sample] = "cannot be played";
    }
    if (d.has_steps)
        for (size_t i = 0; i < n.steps.size(); i++)
            if (!(n.steps[i] == StepData { }) && !d.apply_step(mem, i, n.steps[i]))
                apply_failures_++;
    return true;
}

Engine::StateMem Engine::make_state(const Node &n, bool *ok)
{
    const ModuleDesc &d = *n.desc;
    StateMem          mem(::operator new(d.state_size, std::align_val_t(d.state_align)),
        StateDeleter { &d });
    *ok = init_state(n, mem.get());
    return mem;
}

void Engine::retire(StateMem mem, uint64_t gen) { dead_.emplace_back(gen, std::move(mem)); }

void Engine::collect()
{
    Snapshot *s = nullptr;
    while (retired_.pop(s))
        delete s;
    const uint64_t adopted = adopted_gen_.load(std::memory_order_acquire);
    std::erase_if(dead_, [&](const auto &d) { return d.first <= adopted; });
}

Engine::Snapshot *Engine::build_snapshot(bool restart)
{
    auto *s = new Snapshot;
    s->gen = gen_;
    s->restart = restart;
    s->ok = Plan::build(patch_, [this](uint32_t id) { return live_.at(id).mem.get(); }, s->plan);
    if (s->ok)
    {
        for (uint32_t id : s->plan.order())
        {
            const Live &l = live_.at(id);
            s->probes.push_back(Probe { l.slot, l.desc, l.mem.get() });
            if (const MidiPushFn push = midi_sink(*l.desc))
                s->midi_ins.push_back({ push, l.mem.get() });
            if (const MidiTakeFn take = midi_source(*l.desc))
                s->midi_outs.push_back({ take, l.mem.get() });
        }
        s->activity.assign(s->probes.size(), 0.f);
    }
    // the key script, in time order (stable, so the order written is kept at one sample)
    for (const KeyScript &k : patch_.keys)
    {
        auto it = live_.find(k.node);
        if (it == live_.end() || it->second.desc != &kKeyboardDesc)
            continue;
        s->script.push_back(ScriptEvent { (uint64_t)std::llround(k.seconds * (double)sample_rate_),
            it->second.mem.get(), (uint8_t)(k.on ? PCL_EVT_NOTE_ON : PCL_EVT_NOTE_OFF), k.note,
            k.on ? k.velocity : (uint8_t)0 });
    }
    std::stable_sort(s->script.begin(), s->script.end(),
        [](const ScriptEvent &a, const ScriptEvent &b) { return a.at < b.at; });
    return s;
}

void Engine::publish(Snapshot *s)
{
    // a plan the audio thread has not taken yet is replaced: it never ran, so it can go now
    delete pending_.exchange(s, std::memory_order_acq_rel);
    pump();
}

void Engine::pump()
{
    if (!attached_)
        begin_block();
    collect();
}

Err Engine::compile()
{
    collect();
    failed_node_ = 0;
    const uint64_t g = ++gen_;
    // modules that are gone, or whose id now belongs to another type (a patch replaced wholesale)
    for (auto it = live_.begin(); it != live_.end();)
    {
        const Node *n = patch_.find(it->first);
        if (n && n->desc == it->second.desc && n->sample == it->second.sample)
        {
            ++it;
            continue;
        }
        free_slots_.push_back(it->second.slot);
        retire(std::move(it->second.mem), g);
        it = live_.erase(it);
    }
    // modules that are new (made here: the audio thread cannot see them until the plan is
    // published)
    for (const Node &n : patch_.nodes())
    {
        if (live_.count(n.id))
            continue;
        bool     ok = false;
        StateMem mem = make_state(n, &ok);
        if (!ok)
        {
            failed_node_ = n.id;
            return Err::InitFailed;
        }
        int slot = -1;
        if (!free_slots_.empty())
        {
            slot = free_slots_.back();
            free_slots_.pop_back();
        }
        else if (next_slot_ < (int)kTelemetrySlots)
            slot = next_slot_++;
        if (slot >= 0)
        {
            telemetry_.activity[(size_t)slot].store(0.f);
            telemetry_.dropped[(size_t)slot].store(0);
            telemetry_.position[(size_t)slot].store(-1);
        }
        live_[n.id] = Live { n.desc, std::move(mem), slot, n.sample };
    }
    // the files no Sampler names any more
    std::erase_if(samples_,
        [&](const auto &kv)
        {
            for (const Node &n : patch_.nodes())
                if (n.sample == kv.first)
                    return false;
            return true;
        });
    Snapshot  *s = build_snapshot(false);
    const bool ok = s->ok;
    publish(s);
    return ok ? Err::Ok : Err::BadPatch;
}

void Engine::send(const Cmd &c)
{
    if (!cmds_.push(c))
        telemetry_.commands_lost.fetch_add(1, std::memory_order_relaxed);
    pump();
}

Err Engine::set_param(uint32_t node, size_t param, float value)
{
    collect();
    float     stored = 0;
    const Err e = patch_.set_param(node, param, value, &stored);
    if (e != Err::Ok)
        return e;
    auto it = live_.find(node);
    if (it != live_.end() && it->second.desc->params[param].rebuild)
    { // made again from the patch, which has the new value; the old state goes when the audio
      // thread has the new plan
        bool     made = false;
        StateMem mem = make_state(*patch_.find(node), &made);
        if (!made)
            apply_failures_++; // as a refused setter: the module keeps playing as it was
        else
        {
            retire(std::move(it->second.mem), ++gen_);
            it->second.mem = std::move(mem);
            publish(build_snapshot(false));
        }
    }
    else if (it != live_.end())
    {
        Cmd c { };
        c.type = Cmd::Param;
        c.state = it->second.mem.get();
        c.apply = it->second.desc->params[param].apply;
        c.value = stored;
        send(c);
    }
    return Err::Ok;
}

int Engine::device_code(const Node &n) const
{
    if (sends_midi(*n.desc))
    { // an output: none named is none
        const int slot = n.device.empty() || !midi_out_slot_of ? -1 : midi_out_slot_of(n.device);
        return slot >= 0 ? slot : kMidiNoDevice;
    }
    if (n.device.empty())
        return kMidiAnyDevice;
    const int slot = midi_slot_of ? midi_slot_of(n.device) : -1;
    return slot >= 0 ? slot : kMidiNoDevice;
}

void Engine::set_midi_out(midi::Outbox *outbox) { outbox_ = outbox; }

Err Engine::set_device(uint32_t node, const std::string &name)
{
    collect();
    Node *n = patch_.find(node);
    if (!n)
        return Err::NoSuchNode;
    if (!has_midi_device(*n->desc))
        return Err::NoSuchParam; // not a MIDI module: it has no device
    n->device = name;
    auto it = live_.find(node);
    if (it != live_.end() && has_midi_device(*it->second.desc))
    {
        Cmd c { };
        c.type = Cmd::Device;
        c.desc = it->second.desc;
        c.state = it->second.mem.get();
        c.value = (float)device_code(*n);
        send(c);
    }
    return Err::Ok;
}

std::shared_ptr<const SampleData> Engine::sample_data(const std::string &path, std::string *error)
{
    if (auto it = samples_.find(path); it != samples_.end())
        return it->second;
    auto data = std::make_shared<SampleData>();
    if (!read_wav(path, data.get(), error))
    {
        sample_errors_[path] = *error;
        return nullptr;
    }
    sample_errors_.erase(path);
    samples_[path] = data;
    return data;
}

Err Engine::set_sample(uint32_t node, const std::string &path, std::string *error)
{
    collect();
    Node *n = patch_.find(node);
    if (!n)
        return Err::NoSuchNode;
    if (!takes_sample(*n->desc))
        return Err::NoSuchParam;
    if (!path.empty())
    {
        samples_.erase(path); // read it again: the file may have changed since
        if (!sample_data(path, error))
            return Err::InitFailed;
    }
    n->sample = path;
    auto it = live_.find(node);
    if (it == live_.end())
        return Err::Ok; // (made with it at the next compile)
    bool     made = false;
    StateMem mem = make_state(*n, &made);
    if (!made)
    {
        apply_failures_++;
        return Err::Ok;
    }
    retire(std::move(it->second.mem), ++gen_);
    it->second.mem = std::move(mem);
    it->second.sample = path;
    publish(build_snapshot(false));
    return Err::Ok;
}

std::string Engine::sample_error(uint32_t node) const
{
    const Node *n = patch_.find(node);
    if (!n || n->sample.empty())
        return { };
    auto it = sample_errors_.find(n->sample);
    return it == sample_errors_.end() ? std::string() : it->second;
}

void Engine::refresh_devices()
{
    for (const Node &n : patch_.nodes())
        if (has_midi_device(*n.desc))
            set_device(n.id, std::string(n.device));
}

namespace
{
    int64_t steady_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
} // namespace

void Engine::set_midi_clock(int64_t (*now_ns)()) { midi_now_ = now_ns; }

Err Engine::set_step(uint32_t node, size_t index, const StepData &step)
{
    collect();
    const Err e = patch_.set_step(node, index, step);
    if (e != Err::Ok)
        return e;
    auto it = live_.find(node);
    if (it != live_.end())
    {
        Cmd c { };
        c.type = Cmd::Step;
        c.state = it->second.mem.get();
        c.apply_step = it->second.desc->apply_step;
        c.index = index;
        c.step = patch_.find(node)->steps[index];
        send(c);
    }
    return Err::Ok;
}

Err Engine::key(uint32_t node, bool on, uint8_t note, uint8_t velocity, int64_t at)
{
    collect();
    auto it = live_.find(node);
    if (it == live_.end())
        return Err::NoSuchNode;
    if (it->second.desc != &kKeyboardDesc)
        return Err::NotAKeyboard;
    Cmd c { };
    c.type = Cmd::Key;
    c.state = it->second.mem.get();
    c.key_type = (uint8_t)(on ? PCL_EVT_NOTE_ON : PCL_EVT_NOTE_OFF);
    c.note = note;
    c.velocity = on ? velocity : (uint8_t)0;
    c.at = at; // < 0: when the audio thread takes it
    send(c);
    return Err::Ok;
}

// A library module's own reset keeps what was set since it was made (an oscillator's frequency,
// after notes), so a reset of the whole rack makes every module again from the patch's settings:
// the result is the same as a rack that was just compiled. The new states are made here and swapped
// in.
void Engine::reset()
{
    collect();
    const uint64_t g = ++gen_;
    for (auto &[id, l] : live_)
        if (const Node *n = patch_.find(id))
        {
            bool     ok = false;
            StateMem mem = make_state(*n, &ok);
            retire(std::move(l.mem), g);
            l.mem = std::move(mem);
        }
    publish(build_snapshot(true));
}

void Engine::set_sample_rate(uint32_t rate)
{
    sample_rate_ = rate;
    reset();
}

void Engine::attach_audio(bool on)
{
    attached_ = on;
    pump(); // detaching: what the audio thread left in the ring and the pending plan take effect
            // now
}

// =====================================================================================================
// The audio side

void Engine::apply(const Cmd &c)
{
    switch (c.type)
    {
    case Cmd::Param:
        if (!c.apply(c.state, c.value))
            apply_failures_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Cmd::Step:
        if (!c.apply_step(c.state, c.index, c.step))
            apply_failures_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Cmd::Device:
        midi_set_device(*c.desc, c.state, (int)c.value);
        break;
    case Cmd::Key:
        keyboard_push(c.state, c.at < 0 ? time_.load(std::memory_order_relaxed) : (uint64_t)c.at,
            c.key_type, c.note, c.velocity);
        break;
    }
}

// The start of every block. The order matters: the pending plan is taken first, then every command
// sent before it was published is applied (to the states that were current when it was sent), and
// only then is the new plan adopted and acknowledged; after the acknowledgement the UI may free the
// states the new plan no longer uses, and no command still to come can name them.
void Engine::begin_block()
{
    Snapshot *p = nullptr;
    if (!retired_.full()) // room to hand the current plan back, else adopt next block
        p = pending_.exchange(nullptr, std::memory_order_acq_rel);
    Cmd c;
    while (cmds_.pop(c))
        apply(c);
    if (!p)
        return;
    if (cur_)
        retired_.push(cur_); // cannot fail: checked above, and only this thread pushes
    cur_ = p;
    if (p->restart)
        time_.store(0, std::memory_order_relaxed);
    const uint64_t now = time_.load(std::memory_order_relaxed);
    script_pos_ = 0;
    while (script_pos_ < p->script.size() && p->script[script_pos_].at < now)
        script_pos_++; // already in the past
    adopted_gen_.store(p->gen, std::memory_order_release);
}

void Engine::feed_script(uint64_t upto)
{
    const auto &script = cur_->script;
    while (script_pos_ < script.size() && script[script_pos_].at < upto)
    {
        const ScriptEvent &e = script[script_pos_++];
        keyboard_push(e.state, e.at, e.type, e.note, e.velocity);
    }
}

void Engine::set_midi(midi::Inboxes *inboxes) { midi_ = inboxes; }

void Engine::discard_midi()
{
    if (!midi_ || attached_)
        return;
    midi::Msg m;
    for (midi::Inbox &box : *midi_)
        while (box.ring.pop(m))
        {
        }
}

// Every message that has arrived, to every MIDI In and MIDI CC module, device by device (the order
// within a device is kept). With no such module they are taken and dropped, so nothing piles up.
//
// When it is heard (docs/midi.md, step 8). In a device call of `frames` samples, a message that
// arrived `age` samples before the call is placed `frames - age` samples into it: one that came
// just now at the end, one that came a whole call ago at the start. Every message is therefore
// heard the same time after it arrived (one device buffer), in place of "at the start of the next
// block", which was up to a buffer of jitter. A message with no timestamp, or outside a device call
// (frames 0), is heard at once.
void Engine::feed_midi(size_t frames)
{
    if (!midi_)
        return;
    const uint64_t t0 = time_.load(std::memory_order_relaxed);
    const int64_t  now = frames ? (midi_now_ ? midi_now_() : steady_ns()) : 0;
    midi::Msg      m;
    for (midi::Inbox &box : *midi_)
        while (box.ring.pop(m))
        {
            if (m.type == midi::Type::Control)
                controls_.push(m); // for the UI (MIDI learn); dropped if it is not taking them
            if (!cur_ || !cur_->ok)
                continue;
            uint64_t at = t0;
            if (frames && m.time_ns > 0)
            {
                const double age
                    = std::max(0.0, (double)(now - m.time_ns) * 1e-9 * (double)sample_rate_);
                // (to the nearest sample: a whole number of samples is rarely one of nanoseconds)
                at = t0
                    + (age + 0.5 >= (double)frames
                            ? 0
                            : std::min<uint64_t>(frames - 1, frames - (uint64_t)(age + 0.5)));
            }
            for (const auto &[push, state] : cur_->midi_ins)
                push(state, m, at);
        }
}

// What the MIDI Out modules made in the block that starts at `block_t0`, into the outbox: a message
// at sample s of the device call is due when that sample will be heard, a call's length after the
// call began (the same latency as MIDI in). With no audio thread, at once.
void Engine::send_midi(uint64_t block_t0)
{
    MidiOutMsg buf[256];
    for (const auto &[take, state] : cur_->midi_outs)
    {
        const size_t n = take(state, buf, std::size(buf));
        for (size_t i = 0; i < n; i++)
        {
            midi::OutMsg m;
            m.slot = buf[i].slot;
            m.len = buf[i].len;
            std::memcpy(m.bytes, buf[i].bytes, 3);
            if (call_ns_)
            {
                const double into = (double)(block_t0 + buf[i].offset - call_t0_ + call_frames_);
                m.due_ns = call_ns_ + (int64_t)(into * 1e9 / (double)sample_rate_);
            }
            outbox_->push(m);
        }
    }
}

bool Engine::take_control(midi::Msg &out)
{
    if (!attached_)
        feed_midi(0); // no audio thread: this thread is the audio side
    return controls_.pop(out);
}

namespace
{
    void raise_to(std::atomic<float> &a, float v)
    { // one writer (this thread); the reader resets it to 0, and a lost race only skips one reading
        if (v > a.load(std::memory_order_relaxed))
            a.store(v, std::memory_order_relaxed);
    }
} // namespace

void Engine::render(size_t frames, float *left, float *right)
{
    size_t done = 0;
    while (done < frames)
    {
        const size_t n = std::min<size_t>(frames - done, PCL_MAX_BLOCK);
        float       *l = left + done;
        float       *r = right + done;
        begin_block();
        if (done == 0)
        {
            feed_midi(frames); // once per device call, after the plan for it is in place
            if (outbox_)
            {
                call_ns_ = attached_ ? (midi_now_ ? midi_now_() : steady_ns()) : 0;
                call_t0_ = time_.load(std::memory_order_relaxed);
                call_frames_ = frames;
            }
        }
        std::memset(l, 0, n * sizeof(float));
        std::memset(r, 0, n * sizeof(float));
        const uint64_t t = time_.load(std::memory_order_relaxed);
        if (cur_ && cur_->ok)
        {
            feed_script(t + n);
            cur_->plan.run(t, n, l, r, cur_->activity.data());
            if (outbox_ && !cur_->midi_outs.empty())
                send_midi(t);
            for (size_t i = 0; i < cur_->probes.size(); i++)
            {
                const Probe &pr = cur_->probes[i];
                if (pr.slot < 0)
                    continue;
                raise_to(telemetry_.activity[(size_t)pr.slot], cur_->activity[i]);
                telemetry_.dropped[(size_t)pr.slot].store(
                    dropped_events(*pr.desc, pr.state), std::memory_order_relaxed);
                telemetry_.position[(size_t)pr.slot].store(
                    module_position(*pr.desc, pr.state), std::memory_order_relaxed);
            }
        }
        float pl = 0, pr = 0;
        for (size_t i = 0; i < n;
            i++) // the hard clamp at the host boundary; NaN/inf silenced, counted
        {
            if (!std::isfinite(l[i]))
            {
                l[i] = 0.0f;
                nonfinite_.fetch_add(1, std::memory_order_relaxed);
            }
            if (!std::isfinite(r[i]))
            {
                r[i] = 0.0f;
                nonfinite_.fetch_add(1, std::memory_order_relaxed);
            }
            l[i] = std::clamp(l[i], -1.0f, 1.0f);
            r[i] = std::clamp(r[i], -1.0f, 1.0f);
            pl = std::max(pl, std::fabs(l[i]));
            pr = std::max(pr, std::fabs(r[i]));
        }
        raise_to(telemetry_.peak[0], pl);
        raise_to(telemetry_.peak[1], pr);
        time_.store(t + n, std::memory_order_relaxed);
        done += n;
    }
}

} // namespace rack
