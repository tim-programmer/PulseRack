#include "midi/inputs.h"

#include "midi/names.h"

#include <libremidi/libremidi.hpp>

#include <algorithm>
#include <array>
#include <atomic>

namespace rack::midi
{

namespace
{
    std::string text_of(const stdx::error &e)
    {
        const auto m = e.message();
        return std::string(m.data(), m.size());
    }

    // The inputs present, and their unique names (same order). Leaves out the port libremidi's own
    // observer makes on ALSA to hear about devices coming and going: it is not a device.
    void list_inputs(const libremidi::observer &obs, std::vector<libremidi::input_port> *ports,
        std::vector<std::string> *names)
    {
        std::vector<std::string> raw;
        ports->clear();
        for (libremidi::input_port &p : obs.get_input_ports())
        {
            if (p.port_name == "libremidi-observe")
                continue;
            raw.push_back(p.display_name.empty() ? p.port_name : p.display_name);
            ports->push_back(std::move(p));
        }
        *names = unique_names(raw);
    }
} // namespace

struct Inputs::Impl
{
    explicit Impl(Inboxes &b)
        : boxes(b)
    {
    }

    struct Open
    {
        std::string                         name;
        int                                 slot = -1;
        std::unique_ptr<libremidi::midi_in> in;
    };

    Inboxes                             &boxes;
    std::unique_ptr<libremidi::observer> observer;
    std::atomic<bool>                    changed { true }; // set by the observer's thread
    std::vector<std::string>             wanted;
    std::array<std::string, kMaxInputs>  slot_name; // empty: free
    std::vector<Open>                    open;

    int slot_of(const std::string &name) const
    {
        for (size_t i = 0; i < kMaxInputs; i++)
            if (slot_name[i] == name)
                return int(i);
        return -1;
    }

    // Closing stops the device's thread, so the UI thread may then push into its inbox.
    void close(Open &o)
    {
        o.in.reset();
        Msg lost;
        lost.type = Type::DeviceLost;
        lost.slot = uint8_t(o.slot);
        boxes[size_t(o.slot)].push(lost);
    }

    bool sync(std::vector<std::string> *errors)
    {
        std::vector<libremidi::input_port> ports;
        std::vector<std::string>           names;
        list_inputs(*observer, &ports, &names);
        auto present = [&](const std::string &n) -> int
        {
            const auto it = std::find(names.begin(), names.end(), n);
            return it == names.end() ? -1 : int(it - names.begin());
        };
        auto is_wanted = [&](const std::string &n)
        { return std::find(wanted.begin(), wanted.end(), n) != wanted.end(); };

        bool moved = false;
        for (size_t i = 0; i < open.size();)
        {
            Open &o = open[i];
            if (present(o.name) < 0 || !is_wanted(o.name) || !o.in->is_port_connected())
            {
                close(o);
                open.erase(open.begin() + long(i));
                moved = true;
            }
            else
                i++;
        }
        for (const std::string &n : wanted)
        {
            const int at = present(n);
            if (at < 0
                || std::any_of(
                    open.begin(), open.end(), [&](const Open &o) { return o.name == n; }))
                continue;
            int slot = slot_of(n);
            if (slot < 0)
                for (size_t i = 0; i < kMaxInputs && slot < 0; i++)
                    if (slot_name[i].empty())
                        slot_name[i] = n, slot = int(i);
            if (slot < 0)
            {
                if (errors)
                    errors->push_back(n + ": no room (at most " + std::to_string(kMaxInputs)
                        + " MIDI inputs in one run)");
                continue;
            }
            libremidi::input_configuration conf;
            Inbox                         *box = &boxes[size_t(slot)];
            conf.on_message = [box, slot](const libremidi::message &m)
            {
                Msg x;
                if (!decode(std::span<const uint8_t>(m.bytes.data(), m.bytes.size()), &x))
                    return;
                x.slot = uint8_t(slot);
                x.time_ns = m.timestamp;
                box->push(x);
            };
            conf.ignore_sysex = true;
            conf.ignore_timing = false; // the clock is wanted (step 9)
            conf.ignore_sensing = true;
            conf.timestamps = libremidi::timestamp_mode::SystemMonotonic;
            auto in = std::make_unique<libremidi::midi_in>(
                conf, libremidi::midi_in_configuration_for(*observer));
            if (const stdx::error e = in->open_port(ports[size_t(at)], "PulseRack");
                e != stdx::error { })
            {
                if (errors)
                    errors->push_back(n + ": cannot be opened (" + text_of(e) + ")");
                continue;
            }
            open.push_back({ n, slot, std::move(in) });
            moved = true;
        }
        return moved;
    }
};

Inputs::Inputs(Inboxes &inboxes)
    : impl_(std::make_unique<Impl>(inboxes))
{
}

Inputs::~Inputs()
{
    for (Impl::Open &o : impl_->open)
        impl_->close(o);
    impl_->open.clear();
    impl_->observer.reset(); // before Impl, whose `changed` its callbacks set
}

bool Inputs::start(std::string *error)
{
    if (impl_->observer)
        return true;
    libremidi::observer_configuration conf;
    Impl                             *impl = impl_.get();
    // Any port coming or going raises the flag; poll() then looks at the ports itself. (Outputs
    // too: a device's ports come and go together, and libremidi reports each by its own kind.)
    conf.input_added = [impl](const libremidi::input_port &) { impl->changed = true; };
    conf.input_removed = [impl](const libremidi::input_port &) { impl->changed = true; };
    conf.output_added = [impl](const libremidi::output_port &) { impl->changed = true; };
    conf.output_removed = [impl](const libremidi::output_port &) { impl->changed = true; };
    conf.track_hardware = true;
    conf.track_virtual = true; // software that sends MIDI (a sequencer, a virtual keyboard)
    // libremidi (ALSA) reports a port's removal only if it reported the port before: without this
    // it never says that a device which was there at the start has gone.
    conf.notify_in_constructor = true;
    // only while constructing: the observer outlives this call, `error` does not
    struct StartError
    {
        std::atomic<bool> constructing { true };
        std::string       why;
    };
    auto se = std::make_shared<StartError>();
    conf.on_error = [se](std::string_view text, const auto &)
    {
        if (se->constructing)
            se->why = std::string(text);
    };
    auto obs = std::make_unique<libremidi::observer>(conf);
    se->constructing = false;
    if (obs->get_current_api() == libremidi::API::DUMMY || !se->why.empty())
    {
        if (error)
            *error = se->why.empty() ? std::string("no MIDI system") : se->why;
        return false;
    }
    impl_->observer = std::move(obs);
    impl_->changed = true;
    return true;
}

bool Inputs::started() const { return impl_->observer != nullptr; }

std::string Inputs::api_name() const
{
    if (!impl_->observer)
        return { };
    return std::string(libremidi::get_api_display_name(impl_->observer->get_current_api()));
}

std::vector<std::string> Inputs::available() const
{
    if (!impl_->observer)
        return { };
    std::vector<libremidi::input_port> ports;
    std::vector<std::string>           names;
    list_inputs(*impl_->observer, &ports, &names);
    return names;
}

void Inputs::set_enabled(const std::vector<std::string> &names, std::vector<std::string> *errors)
{
    impl_->wanted.clear();
    for (const std::string &n : names)
        if (std::find(impl_->wanted.begin(), impl_->wanted.end(), n) == impl_->wanted.end())
            impl_->wanted.push_back(n);
    if (impl_->observer)
    {
        impl_->changed = false;
        impl_->sync(errors);
    }
}

const std::vector<std::string> &Inputs::enabled() const { return impl_->wanted; }

bool Inputs::poll(std::vector<std::string> *errors)
{
    if (!impl_->observer || !impl_->changed.exchange(false))
        return false;
    return impl_->sync(errors);
}

bool Inputs::is_open(const std::string &name) const
{
    return std::any_of(impl_->open.begin(), impl_->open.end(),
        [&](const Impl::Open &o) { return o.name == name; });
}

int Inputs::slot_of(const std::string &name) const { return impl_->slot_of(name); }

} // namespace rack::midi
