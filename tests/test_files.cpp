// Patch files on disk (src/ui/files.h): the built-in factory patches, safe writing, the state dir.
#include "portable_env.h"
#include "testutil.h"

#include "ui/files.h"
#include "ui/geometry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace rt_util;
using namespace rack::ui;
namespace fs = std::filesystem;

namespace
{
fs::path scratch_dir(const char *name)
{
    fs::path d = fs::temp_directory_path() / ("pulserack_test_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
} // namespace

TEST(files_factory_patches_are_the_examples_and_all_load)
{
    const auto f = factory_patches();
    CHECK(f.size() == 12);
    for (const FactoryPatch &p : f)
    {
        CHECK(!p.name.empty() && !p.title.empty() && !p.about.empty());
        CHECK(p.about.front() != '#' && p.about.find('\n') == std::string_view::npos);
        // the same text as the file in examples/
        std::ifstream in(
            std::string(RACK_SOURCE_DIR) + "/examples/" + std::string(p.name) + ".rack");
        std::stringstream ss;
        ss << in.rdbuf();
        CHECK(ss.str() == p.text);
        // loads without a warning, compiles, and its panels do not overlap
        Engine           e(48000);
        const LoadResult r = load_patch(p.text, e.patch());
        CHECK(r.ok && r.warnings.empty());
        CHECK(e.compile() == Err::Ok);
        CHECK(resolve_overlaps(e.patch()) == 0);
    }
    bool kv = false;
    for (const FactoryPatch &p : f)
        kv |= p.name == "keyboard_voice" && p.title == "Keyboard voice";
    CHECK(kv);
}

TEST(files_atomic_write_round_trips_and_replaces)
{
    const fs::path    d = scratch_dir("write");
    const std::string path = (d / "a.rack").string();
    std::string       err, back;
    CHECK(write_file_atomic(path, "pulserack 1\n", &err));
    CHECK(read_file(path, &back, &err) && back == "pulserack 1\n");
    CHECK(write_file_atomic(path, "pulserack 1\nnode 1 osc x=0 y=0\n", &err)); // replaces it
    CHECK(read_file(path, &back, &err) && back == "pulserack 1\nnode 1 osc x=0 y=0\n");
    CHECK(!fs::exists(path + ".tmp")); // nothing left behind
    CHECK(file_name(path) == "a.rack" && file_name("b.rack") == "b.rack");
    fs::remove_all(d);
}

TEST(files_a_failed_write_keeps_the_old_file)
{
    const fs::path    d = scratch_dir("fail");
    const std::string path = (d / "keep.rack").string();
    std::string       err, back;
    CHECK(write_file_atomic(path, "old\n", &err));
    // a directory where the temporary file would go: the write fails, and the old file is intact
    fs::create_directories(path + ".tmp");
    err.clear();
    CHECK(!write_file_atomic(path, "new\n", &err));
    CHECK(!err.empty() && err.find("keep.rack") != std::string::npos);
    CHECK(read_file(path, &back, &err) && back == "old\n");
    // and a missing directory
    CHECK(!write_file_atomic((d / "no" / "such" / "x.rack").string(), "x\n", &err));
    CHECK(!read_file((d / "missing.rack").string(), &back, &err)
        && err.find("missing.rack") != std::string::npos);
    fs::remove_all(d);
}

TEST(files_state_dir_follows_xdg)
{
    const char       *old = std::getenv("XDG_STATE_HOME");
    const std::string saved = old ? old : "";
    setenv("XDG_STATE_HOME", "/tmp/xdg-state", 1);
    CHECK(default_state_dir() == "/tmp/xdg-state/pulserack");
    CHECK(recovery_path("/tmp/xdg-state/pulserack") == "/tmp/xdg-state/pulserack/unsaved.rack");
    CHECK(recovery_path("").empty()); // no state dir: no recovery
    unsetenv("XDG_STATE_HOME");
#ifdef _WIN32
    if (const char *a = std::getenv("LOCALAPPDATA"))
        CHECK(default_state_dir() == std::string(a) + "/PulseRack");
#else
    if (const char *h = std::getenv("HOME"))
        CHECK(default_state_dir() == std::string(h) + "/.local/state/pulserack");
#endif
    if (old)
        setenv("XDG_STATE_HOME", saved.c_str(), 1);
}
