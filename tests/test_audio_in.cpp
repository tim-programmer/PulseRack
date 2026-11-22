// Audio In: the sound card's inputs in the standalone rack (capture slots chosen by the module's
// device), the host's audio in a plugin.
#include "testutil.h"

#include "engine/model/patchfile.h"

using namespace rt_util;

namespace
{
// A rig whose Audio In goes straight to the output, rendered with what a call gives it.
struct InRig
{
    Rig      r;
    uint32_t in = r.add("audio_in"), out = r.add("out");
    InRig()
    {
        CHECK(r.cable(in, "l", out, "l") == Err::Ok);
        CHECK(r.cable(in, "r", out, "r") == Err::Ok);
        r.param(out, "level", 1);
    }
    // renders 64 frames with each capture slot k holding the constant 0.1 * (k + 1) on the left
    // and minus that on the right (slot 3: a mono input, its right left null), and the host's in
    // 0.5 / -0.5
    std::pair<float, float> hear()
    {
        CHECK(r.compile());
        static float cap[kCaptureSlots][2][64], host_in[2][64];
        HostCall     call;
        for (size_t k = 0; k < kCaptureSlots; k++)
            for (int c = 0; c < 2; c++)
            {
                std::fill_n(cap[k][c], 64, (c ? -0.1f : 0.1f) * (float)(k + 1));
                call.capture[k][c] = k == 3 && c == 1 ? nullptr : cap[k][c];
            }
        std::fill_n(host_in[0], 64, 0.5f);
        std::fill_n(host_in[1], 64, -0.5f);
        call.in[0] = host_in[0];
        call.in[1] = host_in[1];
        float l[64], rr[64];
        r.e.render(64, l, rr, call);
        return { l[63], rr[63] };
    }
};
} // namespace

TEST(audio_in_reads_the_capture_slot_its_device_names)
{
    InRig a;
    // the application's slots: the default input in slot 0, "USB Mic" in 2, "Mono" in 3, no other
    a.r.e.audio_in_slot_of = [](const std::string &name)
    { return name.empty() ? 0 : name == "USB Mic" ? 2 : name == "Mono" ? 3 : -1; };
    auto [l, r] = a.hear();
    CHECK(std::abs(l - 0.1f) < 1e-4f && std::abs(r + 0.1f) < 1e-4f); // the default: slot 0
    CHECK(a.r.e.set_device(a.in, "USB Mic") == Err::Ok);
    std::tie(l, r) = a.hear();
    CHECK(std::abs(l - 0.3f) < 1e-4f && std::abs(r + 0.3f) < 1e-4f);
    CHECK(a.r.e.set_device(a.in, "Mono") == Err::Ok); // a mono input on both sides
    std::tie(l, r) = a.hear();
    CHECK(std::abs(l - 0.4f) < 1e-4f && std::abs(r - 0.4f) < 1e-4f);
    CHECK(a.r.e.set_device(a.in, "Unplugged") == Err::Ok); // not there: silence
    std::tie(l, r) = a.hear();
    CHECK(l == 0 && r == 0);
    // plugged in again: refresh_devices asks for its slot again
    a.r.e.audio_in_slot_of = [](const std::string &name) { return name == "Unplugged" ? 1 : -1; };
    a.r.e.refresh_devices();
    std::tie(l, r) = a.hear();
    CHECK(std::abs(l - 0.2f) < 1e-4f);
}

TEST(audio_in_reads_the_hosts_input_where_there_is_no_slot_lookup)
{
    InRig a; // a plugin, rack-render: no audio_in_slot_of
    CHECK(a.r.e.set_device(a.in, "USB Mic") == Err::Ok);
    const auto [l, r] = a.hear();
    CHECK(std::abs(l - 0.5f) < 1e-4f && std::abs(r + 0.5f) < 1e-4f);
}

TEST(audio_in_keeps_its_device_in_the_patch)
{
    InRig a;
    CHECK(a.r.e.set_device(a.in, "Built-in Audio Analog Stereo") == Err::Ok);
    const std::string text = save_patch(a.r.e.patch());
    CHECK(text.find("device 1 Built-in Audio Analog Stereo\n") != std::string::npos);
    Patch            back;
    const LoadResult lr = load_patch(text, back);
    CHECK(lr.ok && lr.warnings.empty());
    CHECK(back.find(a.in)->device == "Built-in Audio Analog Stereo");
    CHECK(save_patch(back) == text);
}
