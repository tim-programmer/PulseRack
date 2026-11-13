#include "midi/outputs.h"

#include "midi/names.h"

#include <libremidi/libremidi.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace rack::midi
{

namespace
{
    std::string text_of(const stdx::error &e)
    {
        const auto m = e.message();
        return std::string(m.data(), m.size());
    }

    int64_t now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void list_outputs(const libremidi::observer &obs, std::vector<libremidi::output_port> *ports,
        std::vector<std::string> *names)
    {
        std::vector<std::string> raw;
        ports->clear();
        for (libremidi::output_port &p : obs.get_output_ports())
        {
            raw.push_back(p.display_name.empty() ? p.port_name : p.display_name);
            ports->push_back(std::move(p));
        }
        *names = unique_names(raw);
    }

    // CC 123 (all notes off) on every channel: what an output hears before the rack lets go of it,
    // so that no note is left sounding on the other side.
    void all_notes_off(const libremidi::midi_out &out)
    {
        for (unsigned char ch = 0; ch < 16; ch++)
            (void)out.send_message((unsigned char)(0xB0 | ch), 123, 0);
    }
} // namespace

struct Outputs::Impl
{
    explicit Impl(Outbox &b)
        : box(b)
    {
    }

    Outbox                                &box;
    std::unique_ptr<libremidi::observer>   observer;
    std::atomic<bool>                      changed { true }; // set by the observer's thread
    std::array<std::string, kMaxOutputs>   slot_name;        // empty: free
    std::mutex                             lock;             // the ports: the UI and the sender
    std::array<std::unique_ptr<libremidi::midi_out>, kMaxOutputs> port;
    std::thread                            sender;
    std::atomic<bool>                      stop { false };

    int slot_of(const std::string &name) const
    {
        for (size_t i = 0; i < kMaxOutputs; i++)
            if (!name.empty() && slot_name[i] == name)
                return int(i);
        return -1;
    }

    // Opens every named output that is present, closes those that went away.
    bool sync(std::vector<std::string> *errors)
    {
        std::vector<libremidi::output_port> ports;
        std::vector<std::string>            names;
        list_outputs(*observer, &ports, &names);
        bool                        moved = false;
        std::lock_guard<std::mutex> g(lock);
        for (size_t s = 0; s < kMaxOutputs; s++)
        {
            if (slot_name[s].empty())
                continue;
            const auto it = std::find(names.begin(), names.end(), slot_name[s]);
            const bool present = it != names.end();
            if (port[s] && (!present || !port[s]->is_port_connected()))
            {
                port[s].reset();
                moved = true;
            }
            if (!port[s] && present)
            {
                auto out = std::make_unique<libremidi::midi_out>(libremidi::output_configuration { },
                    libremidi::midi_out_configuration_for(*observer));
                if (const stdx::error e = out->open_port(ports[size_t(it - names.begin())], "PulseRack");
                    e != stdx::error { })
                {
                    if (errors)
                        errors->push_back(slot_name[s] + ": cannot be opened (" + text_of(e) + ")");
                    continue;
                }
                port[s] = std::move(out);
                moved = true;
            }
        }
        return moved;
    }

    // The sender: takes what the engine put in the outbox and sends each message when it is due,
    // checking every half millisecond.
    void run()
    {
        std::vector<OutMsg> waiting;
        while (!stop.load(std::memory_order_relaxed))
        {
            OutMsg m;
            while (box.ring.pop(m))
                waiting.insert(std::upper_bound(waiting.begin(), waiting.end(), m,
                                   [](const OutMsg &a, const OutMsg &b)
                                   { return a.due_ns < b.due_ns; }),
                    m);
            const int64_t now = now_ns();
            size_t        k = 0;
            {
                std::lock_guard<std::mutex> g(lock);
                for (; k < waiting.size() && waiting[k].due_ns <= now; k++)
                    if (waiting[k].slot < kMaxOutputs && port[waiting[k].slot])
                        (void)port[waiting[k].slot]->send_message(waiting[k].bytes, waiting[k].len);
            }
            waiting.erase(waiting.begin(), waiting.begin() + long(k));
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
};

Outputs::Outputs(Outbox &outbox)
    : impl_(std::make_unique<Impl>(outbox))
{
}

Outputs::~Outputs()
{
    if (impl_->sender.joinable())
    {
        impl_->stop = true;
        impl_->sender.join();
    }
    for (auto &p : impl_->port)
        if (p && p->is_port_connected())
            all_notes_off(*p);
    for (auto &p : impl_->port)
        p.reset();
    impl_->observer.reset(); // before Impl, whose `changed` its callbacks set
}

bool Outputs::start(std::string *error)
{
    if (impl_->observer)
        return true;
    libremidi::observer_configuration conf;
    Impl                             *impl = impl_.get();
    conf.output_added = [impl](const libremidi::output_port &) { impl->changed = true; };
    conf.output_removed = [impl](const libremidi::output_port &) { impl->changed = true; };
    conf.track_hardware = true;
    conf.track_virtual = true; // software that takes MIDI (a synth, a DAW)
    conf.notify_in_constructor = true; // (see Inputs::start)
    auto obs = std::make_unique<libremidi::observer>(conf);
    if (obs->get_current_api() == libremidi::API::DUMMY)
    {
        if (error)
            *error = "no MIDI system";
        return false;
    }
    impl_->observer = std::move(obs);
    impl_->changed = true;
    impl_->sender = std::thread([impl] { impl->run(); });
    return true;
}

bool Outputs::started() const { return impl_->observer != nullptr; }

std::vector<std::string> Outputs::available() const
{
    if (!impl_->observer)
        return { };
    std::vector<libremidi::output_port> ports;
    std::vector<std::string>            names;
    list_outputs(*impl_->observer, &ports, &names);
    return names;
}

int Outputs::slot_of(const std::string &name) const { return impl_->slot_of(name); }

int Outputs::slot_for(const std::string &name)
{
    int slot = impl_->slot_of(name);
    if (slot >= 0 || name.empty())
        return slot;
    for (size_t i = 0; i < kMaxOutputs && slot < 0; i++)
        if (impl_->slot_name[i].empty())
        {
            std::lock_guard<std::mutex> g(impl_->lock);
            impl_->slot_name[i] = name;
            slot = int(i);
        }
    if (slot >= 0 && impl_->observer)
        impl_->sync(nullptr); // open it now if it is there
    return slot;
}

std::string Outputs::name_of(int slot) const
{
    return slot >= 0 && (size_t)slot < kMaxOutputs ? impl_->slot_name[(size_t)slot] : std::string();
}

bool Outputs::is_open(const std::string &name) const
{
    const int s = impl_->slot_of(name);
    if (s < 0)
        return false;
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->port[(size_t)s] != nullptr;
}

bool Outputs::poll(std::vector<std::string> *errors)
{
    if (!impl_->observer || !impl_->changed.exchange(false))
        return false;
    return impl_->sync(errors);
}

} // namespace rack::midi
