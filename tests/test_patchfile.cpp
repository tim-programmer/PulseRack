#include "testutil.h"

#include <random>

using namespace rt_util;

namespace
{

// Every module once, random settings, random valid cables, steps and keys.
void fill_random(Patch &p, uint32_t seed)
{
    std::mt19937          rng(seed);
    std::vector<uint32_t> ids;
    for (const ModuleDesc *d : all_modules())
        ids.push_back(p.add_node(*d, (float)(rng() % 1000) / 10.f, (float)(rng() % 300) / 10.f));
    for (uint32_t id : ids)
    {
        Node *n = p.find(id);
        for (size_t i = 0; i < n->desc->params.size(); i++)
        {
            const ParamDesc &pd = n->desc->params[i];
            const float      t = (float)(rng() % 10001) / 10000.f;
            (void)p.set_param(id, i, pd.min + t * (pd.max - pd.min));
        }
        if (n->desc->has_steps)
            for (size_t i = 0; i < kMaxSteps; i++)
                if (rng() % 3 == 0)
                    (void)p.set_step(id, i,
                        StepData { (uint8_t)(rng() % 128), (uint8_t)(rng() % 128),
                            (uint8_t)(rng() % 20), (uint8_t)(rng() % 2) });
        if (n->desc == &kKeyboardDesc)
            for (int k = 0; k < 12; k++)
                p.keys.push_back(KeyScript { id, (double)(rng() % 1500) / 1000.0, k % 2 == 0,
                    (uint8_t)(36 + rng() % 40), (uint8_t)(1 + rng() % 127) });
    }
    for (int k = 0; k < 120; k++)
    {
        const Node  *a = p.find(ids[rng() % ids.size()]);
        const Node  *b = p.find(ids[rng() % ids.size()]);
        const size_t pa = rng()
            % a->desc->ports
                  .size(); // drawn first: the order of evaluation of arguments is unspecified
        const size_t pb = rng() % b->desc->ports.size();
        (void)p.connect({ a->id, (uint16_t)pa }, { b->id, (uint16_t)pb });
    }
}

LoadResult load(const std::string &text, Patch &p) { return load_patch(text, p); }

} // namespace

TEST(patchfile_golden_text)
{
    Patch          p;
    const uint32_t clk = p.add_node(*find_module("clock"), 12, 0),
                   seq = p.add_node(*find_module("seq"), 40, 0);
    CHECK(p.set_param(clk, 0, 126.5f) == Err::Ok && p.set_param(seq, 4, 2) == Err::Ok);
    CHECK(p.set_step(seq, 0, StepData { 48, 100, 0, 0 }) == Err::Ok
        && p.set_step(seq, 3, StepData { 55, 90, 4, 1 }) == Err::Ok);
    CHECK(p.connect({ clk, 0 }, { seq, 0 }) == Err::Ok);
    p.keys.push_back(
        KeyScript { clk, 0.5, true, 60, 100 }); // (a script for a node that takes no keys is kept
                                                // in the model, written, and ignored by the engine)
    const std::string expected = "pulserack 1\n"
                                 "node 1 clock x=12 y=0\n"
                                 "param 1 bpm 126.5\n"
                                 "param 1 ppqn 24\n"
                                 "param 1 run 1\n"
                                 "node 2 seq x=40 y=0\n"
                                 "param 2 length 8\n"
                                 "param 2 division 6\n"
                                 "param 2 gate 3\n"
                                 "param 2 swing 0\n"
                                 "param 2 direction ping-pong\n"
                                 "param 2 transpose 0\n"
                                 "step 2 0 note=48 vel=100 gate=0 tie=0\n"
                                 "step 2 3 note=55 vel=90 gate=4 tie=1\n"
                                 "key 1 0.5 on 60 100\n"
                                 "cable 1.clock -> 2.in\n";
    CHECK(save_patch(p) == expected);
}

TEST(patchfile_round_trip_and_identical_audio)
{
    for (uint32_t seed = 1; seed <= 12; seed++)
    {
        Rig a;
        fill_random(a.e.patch(), seed);
        const std::string text = save_patch(a.e.patch());
        Rig               b;
        const LoadResult  lr = load(text, b.e.patch());
        CHECK(lr.ok && lr.warnings.empty());
        if (!lr.ok)
            std::printf("    seed %u: %s\n", seed, lr.error.c_str());
        CHECK(save_patch(b.e.patch()) == text); // text -> model -> text is stable
        CHECK(a.compile() && b.compile());
        a.render(6000);
        b.render(6000);
        CHECK(same_bits(a.L, b.L) && same_bits(a.R, b.R)); // and the loaded patch sounds the same
        CHECK(a.e.nonfinite_samples() == 0 && b.e.nonfinite_samples() == 0);
    }
}

TEST(patchfile_rejects_bad_files_whole)
{
    // Long strings: a wrapped one would become two adjacent literals, which clang warns about
    // inside an array.
    // clang-format off
    const char *bad[] = {
        "",              // empty
        "hello\n",       // not a patch
        "pulserack 2\n", // unknown version
        "pulserack 1 extra\n",
        "node 1 osc\n",                          // no header
        "pulserack 1\nnode x osc\n",             // bad id
        "pulserack 1\nnode 0 osc\n",             // id 0
        "pulserack 1\nnode 1 osc\nnode 1 vca\n", // id used twice
        "pulserack 1\nnode 1 osc bogus=3\n",     // unknown option
        "pulserack 1\nnode 1 osc x=abc\n",
        "pulserack 1\nparam 1 frequency 3\n",               // module not defined yet
        "pulserack 1\nnode 1 osc\nparam 1 frequency abc\n", // not a number or a label
        "pulserack 1\nnode 1 osc\nparam 1 waveform bogus\n",
        "pulserack 1\nnode 1 osc\nparam 1 frequency nan\n",
        "pulserack 1\nnode 1 osc\nparam 1 frequency\n", // too short
        "pulserack 1\nnode 1 osc\nstep 1 0 note=60\n",  // osc has no steps
        "pulserack 1\nnode 1 seq\nstep 1 64 note=60\n", // step out of range
        "pulserack 1\nnode 1 seq\nstep 1 0 note=200\n",
        "pulserack 1\nnode 1 seq\nstep 1 0 wobble=1\n",
        "pulserack 1\nnode 1 keyboard\nkey 1 -1 on 60\n", // negative time
        "pulserack 1\nnode 1 keyboard\nkey 1 0 maybe 60\n",
        "pulserack 1\nnode 1 keyboard\nkey 1 0 on 200\n",
        "pulserack 1\nnode 1 osc\nnode 2 vca\ncable 1.out 2.in\n", // missing arrow
        "pulserack 1\nnode 1 osc\nnode 2 vca\ncable 1.out -> 2.in extra\n",
        "pulserack 1\nnode 1 osc\ncable 1.out -> 1.pitch\n",          // a loop
        "pulserack 1\nnode 1 osc\nnode 2 seq\ncable 1.out -> 2.in\n", // signal into event
        "pulserack 1\nnode 1 osc\nnode 2 vca\nnode 3 vca\ncable 1.out -> 2.in\ncable 3.out -> 2.in\n",                                         // two cables into one input
        "pulserack 1\nnode 1 osc\ncable 1.out -> 9.in\n", // module not defined
        "pulserack 1\nnode 1 osc\ncable 1out -> 1.in\n",
        "pulserack 1\nwibble\n", // unknown record
    };
    // clang-format on
    for (const char *text : bad)
    {
        Patch keep;
        keep.add_node(*find_module("noise"));
        const LoadResult lr = load(text, keep);
        if (lr.ok)
            std::printf("    accepted: %s\n", text);
        CHECK(!lr.ok);
        CHECK(!lr.error.empty() && lr.error.rfind("line ", 0) == 0);
        CHECK(keep.nodes().size() == 1); // the patch passed in is left alone on failure
    }
}

TEST(patchfile_limits)
{
    Patch       p;
    std::string many = "pulserack 1\n";
    for (size_t i = 1; i <= kMaxNodes + 1; i++)
        many += "node " + std::to_string(i) + " noise\n";
    CHECK(!load(many, p).ok);
    std::string ok = "pulserack 1\n";
    for (size_t i = 1; i <= kMaxNodes; i++)
        ok += "node " + std::to_string(i) + " noise\n";
    CHECK(load(ok, p).ok && p.nodes().size() == kMaxNodes);
    CHECK(!load("pulserack 1\n" + std::string(kMaxLine + 1, 'x') + "\n", p).ok);
    CHECK(load("pulserack 1\n#" + std::string(kMaxLine - 1, 'x') + "\n", p)
            .ok); // exactly the limit is fine
    CHECK(!load("pulserack 1\n#" + std::string(kMaxLine, 'x') + "\n", p).ok); // one more is not
    std::string keys = "pulserack 1\nnode 1 keyboard\n";
    for (size_t i = 0; i <= kMaxKeys; i++)
        keys += "key 1 0 on 60\n";
    CHECK(!load(keys, p).ok);
}

TEST(patchfile_warns_and_skips_what_it_does_not_know)
{
    const std::string text
        = "# a patch from a newer rack\r\n"
          "pulserack 1\r\n"
          "\r\n"
          "node 1 osc x=1 y=2   # trailing comment\r\n"
          "param 1 waveform triangle\r\n"
          "param 1 sparkle 7\r\n"       // unknown control: warning
          "param 1 frequency 99999\r\n" // out of range: clamped, with a warning
          "node 2 flux_capacitor\r\n"   // unknown module: skipped with everything that mentions it
          "param 2 gigawatts 1.21\r\n"
          "step 2 0 note=1\r\n"
          "key 2 0 on 60\r\n"
          "node 3 vca\r\n"
          "cable 1.out -> 2.in\r\n"
          "cable 1.out -> 3.warp\r\n" // unknown jack: warning
          "cable 1.out -> 3.in\r\n";
    Patch            p;
    const LoadResult lr = load(text, p);
    CHECK(lr.ok);
    CHECK(p.nodes().size() == 2 && p.find(2) == nullptr);
    CHECK(p.cables().size() == 1);
    CHECK(p.find(1)->params[0] == 1.0f && p.find(1)->params[1] == 8000.0f);
    CHECK(lr.warnings.size()
        == 4); // flux_capacitor, sparkle, the clamped frequency, the unknown jack
}

TEST(patchfile_random_damage_never_crashes)
{
    Patch p;
    fill_random(p, 99);
    const std::string good = save_patch(p);
    std::mt19937      rng(5);
    int               loaded = 0, rejected = 0;
    for (int round = 0; round < 3000; round++)
    {
        std::string t = good;
        const int   edits = 1 + (int)(rng() % 4);
        for (int e = 0; e < edits; e++)
        {
            const size_t at = rng() % t.size();
            switch (rng() % 4)
            {
            case 0:
                t[at] = (char)(rng() % 256);
                break;
            case 1:
                t.erase(at, 1 + rng() % 12);
                break;
            case 2:
            {
                const size_t count = 1 + rng() % 5;
                const char   ch = (char)(32 + rng() % 90);
                t.insert(at, std::string(count, ch));
            }
            break;
            case 3:
                t.resize(at);
                break;
            }
            if (t.empty())
                t = "x";
        }
        Patch            q;
        const LoadResult lr = load(t, q);
        (lr.ok ? loaded : rejected)++;
        if (lr.ok) // whatever it accepted must be a valid patch the engine can run
        {
            Engine e(kRate);
            e.patch() = q;
            if (e.compile() == Err::Ok)
            {
                std::vector<float> l(300), r(300);
                e.render(300, l.data(), r.data());
            }
        }
    }
    CHECK(loaded > 50 && rejected > 50); // the damage is neither always harmless nor always fatal
}

TEST(patchfile_writes_cables_in_the_order_of_their_inputs)
{
    Patch          p;
    const uint32_t noise = p.add_node(*find_module("noise")), a = p.add_node(*find_module("vca")),
                   b = p.add_node(*find_module("vca")), out = p.add_node(*find_module("out"));
    const uint16_t aout = (uint16_t)find_port(*p.find(a)->desc, "out"),
                   bout = (uint16_t)find_port(*p.find(b)->desc, "out");
    CHECK(
        p.connect({ b, bout }, { out, 1 }) == Err::Ok); // added in the reverse of the written order
    CHECK(p.connect({ a, aout }, { out, 0 }) == Err::Ok);
    CHECK(p.connect({ noise, 0 }, { a, 0 }) == Err::Ok);
    const std::string text = save_patch(p);
    const size_t      c1 = text.find("cable 1.out -> 2.in"), c2 = text.find("cable 2.out -> 4.l"),
                      c3 = text.find("cable 3.out -> 4.r");
    CHECK(c1 != std::string::npos && c2 != std::string::npos && c3 != std::string::npos);
    CHECK(c1 < c2 && c2 < c3); // by the input's module, then its jack
}

TEST(patchfile_errors_name_the_right_line)
{
    Patch      p;
    LoadResult lr = load("pulserack 1\n\n# a comment\nbogus\n", p);
    CHECK(!lr.ok && lr.error.rfind("line 4:", 0) == 0);
    lr = load("pulserack 1\nnode 1 osc\n\r\n\nparam 1 frequency\n", p);
    CHECK(!lr.ok && lr.error.rfind("line 5:", 0) == 0);
    lr = load("# only a comment\n\nnot a header\n", p);
    CHECK(!lr.ok && lr.error.rfind("line 3:", 0) == 0);
}

TEST(patchfile_modules_of_unknown_types_count_towards_the_limit)
{
    Patch       p;
    std::string many = "pulserack 1\n";
    for (size_t i = 1; i <= 5; i++)
        many += "node " + std::to_string(i) + " flux_" + std::to_string(i) + "\n";
    CHECK(load(many, p).ok && p.nodes().empty());
    std::string over = "pulserack 1\n";
    for (size_t i = 1; i <= kMaxNodes + 1; i++)
        over += "node " + std::to_string(i) + " flux\n";
    CHECK(!load(over, p).ok);
    std::string exactly = "pulserack 1\n";
    for (size_t i = 1; i <= kMaxNodes; i++)
        exactly += "node " + std::to_string(i) + (i % 2 ? " flux\n" : " noise\n");
    CHECK(load(exactly, p).ok && p.nodes().size() == kMaxNodes / 2);
}

TEST(patchfile_a_step_with_no_options_is_the_default_step)
{
    Patch p;
    CHECK(load("pulserack 1\nnode 1 seq\nstep 1 0 note=50 vel=90\nstep 1 0\nstep 1 5\n", p).ok);
    CHECK(p.find(1)->steps[0] == StepData { } && p.find(1)->steps[5] == StepData { });
}

TEST(patchfile_a_midi_module_keeps_its_device_by_name)
{
    Patch          p;
    const uint32_t in = p.add_node(*find_module("midi_in"), 0, 0),
                   cc = p.add_node(*find_module("midi_cc"), 10, 0),
                   any = p.add_node(*find_module("midi_in"), 20, 0);
    p.find(in)->device = "Arturia KeyStep 37 MIDI 1";
    p.find(cc)->device = "nanoKONTROL2 #2"; // a '#' is part of the name, not a comment
    const std::string text = save_patch(p);
    CHECK(text.find("device " + std::to_string(in) + " Arturia KeyStep 37 MIDI 1\n")
        != std::string::npos);
    CHECK(text.find("device " + std::to_string(cc) + " nanoKONTROL2 #2\n") != std::string::npos);
    CHECK(text.find("device " + std::to_string(any)) == std::string::npos); // every input: no line
    Patch back;
    CHECK(load(text, back).ok && load(text, back).warnings.empty());
    CHECK(back.find(in)->device == "Arturia KeyStep 37 MIDI 1");
    CHECK(back.find(cc)->device == "nanoKONTROL2 #2");
    CHECK(back.find(any)->device.empty());
    CHECK(save_patch(back) == text);

    // spaces inside the name are kept, those around it (and a CR) are not
    CHECK(load("pulserack 1\nnode 1 midi_in\ndevice 1   My  Device # 3 \t\r\n", back).ok);
    CHECK(back.find(1)->device == "My  Device # 3");
    // a comment elsewhere still is one, and "device" inside one is not a record
    CHECK(load("pulserack 1\nnode 1 midi_in # device 1 x\n# device 1 y\n", back).ok);
    CHECK(back.find(1)->device.empty());
    // refused: no name, no such module, a bad id; skipped with a warning: a module with no MIDI
    CHECK(!load("pulserack 1\nnode 1 midi_in\ndevice 1\n", back).ok);
    CHECK(!load("pulserack 1\nnode 1 midi_in\ndevice 1   \n", back).ok);
    CHECK(!load("pulserack 1\nnode 1 midi_in\ndevice 2 x\n", back).ok);
    CHECK(!load("pulserack 1\nnode 1 midi_in\ndevice one x\n", back).ok);
    CHECK(!load("pulserack 1\nnode 1 midi_in\ndevice\n", back).ok);
    const LoadResult r = load("pulserack 1\nnode 1 osc\ndevice 1 KeyStep\n", back);
    CHECK(r.ok && r.warnings.size() == 1 && back.find(1)->device.empty());
    // of a module of an unknown type: skipped with it
    const LoadResult u = load("pulserack 1\nnode 1 from_the_future\ndevice 1 KeyStep\n", back);
    CHECK(u.ok && u.warnings.size() == 1);
    // a name that would break the file is not written
    p.find(in)->device = "a\nnode 9 osc";
    CHECK(save_patch(p).find("node 9") == std::string::npos);
}
