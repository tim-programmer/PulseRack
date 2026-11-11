#include "ui/files.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rack::ui
{

std::string file_name(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool read_file(const std::string &path, std::string *out, std::string *error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        *error = file_name(path) + ": cannot be read (" + std::strerror(errno) + ")";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (f.bad())
    {
        *error = file_name(path) + ": reading failed";
        return false;
    }
    *out = ss.str();
    return true;
}

bool write_file_atomic(const std::string &path, const std::string &text, std::string *error)
{
    const std::string tmp = path + ".tmp";
    FILE             *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
    {
        *error = file_name(path) + ": cannot be written (" + std::strerror(errno) + ")";
        return false;
    }
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool flushed = std::fflush(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !flushed || !closed)
    {
        std::remove(tmp.c_str());
        *error = file_name(path) + ": writing failed (" + std::strerror(errno) + ")";
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        *error = file_name(path) + ": cannot be replaced (" + std::strerror(errno) + ")";
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string default_state_dir()
{
    if (const char *x = std::getenv("XDG_STATE_HOME"); x && *x)
        return std::string(x) + "/pulserack";
    if (const char *h = std::getenv("HOME"); h && *h)
        return std::string(h) + "/.local/state/pulserack";
    return { };
}

std::string recovery_path(const std::string &state_dir)
{
    return state_dir.empty() ? std::string() : state_dir + "/unsaved.rack";
}

} // namespace rack::ui
