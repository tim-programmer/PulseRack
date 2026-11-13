#include "ui/settings.h"

#include "ui/files.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace rack::ui
{

namespace
{
    constexpr const char *kMagic = "pulserack-settings";

    // "key rest of the line": names keep their spaces
    bool split(const std::string &line, std::string *key, std::string *rest)
    {
        const size_t sp = line.find(' ');
        *key = line.substr(0, sp);
        *rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        return !key->empty();
    }

    bool has_newline(const std::string &s) { return s.find_first_of("\r\n") != std::string::npos; }
} // namespace

std::string settings_text(const Settings &s)
{
    std::string t = std::string(kMagic) + " 1\n";
    if (!s.output.empty() && !has_newline(s.output))
        t += "output " + s.output + "\n";
    for (const std::string &m : s.midi_inputs)
        if (!m.empty() && !has_newline(m))
            t += "midi-input " + m + "\n";
    for (const Learn &l : s.learn)
        if (!has_newline(l.device))
            t += "learn " + std::to_string(l.channel + 1) + " " + std::to_string(l.cc) + " "
                + std::to_string(l.control + 1) + (l.device.empty() ? "" : " " + l.device) + "\n";
    return t;
}

bool parse_settings(const std::string &text, Settings *out, std::vector<std::string> *warnings)
{
    std::istringstream in(text);
    std::string        line, key, rest;
    if (!std::getline(in, line) || !split(line, &key, &rest) || key != kMagic)
        return false;
    Settings s;
    int      n = 1;
    while (std::getline(in, line))
    {
        n++;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        split(line, &key, &rest);
        if (key == "output")
            s.output = rest;
        else if (key == "midi-input" && !rest.empty())
            s.midi_inputs.push_back(rest);
        else if (key == "learn")
        {
            int ch = 0, cc = 0, control = 0, used = 0;
            if (std::sscanf(rest.c_str(), "%d %d %d%n", &ch, &cc, &control, &used) == 3 && ch >= 1
                && ch <= 16 && cc >= 0 && cc <= 119 && control >= 1 && control <= 256)
            {
                std::string device = rest.substr((size_t)used);
                if (!device.empty() && device[0] == ' ')
                    device.erase(0, 1);
                s.learn.push_back(Learn { ch - 1, cc, control - 1, device });
            }
            else if (warnings)
                warnings->push_back("settings line " + std::to_string(n) + ": not a learn line");
        }
        else if (warnings)
            warnings->push_back(
                "settings line " + std::to_string(n) + ": '" + key + "' is not known");
    }
    *out = std::move(s);
    return true;
}

std::string default_config_dir()
{
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x)
        return std::string(x) + "/pulserack";
#ifdef _WIN32
    if (const char *a = std::getenv("APPDATA"); a && *a)
        return std::string(a) + "/PulseRack";
#else
    if (const char *h = std::getenv("HOME"); h && *h)
        return std::string(h) + "/.config/pulserack";
#endif
    return { };
}

std::string settings_path(const std::string &config_dir)
{
    return config_dir.empty() ? std::string() : config_dir + "/settings";
}

Settings load_settings(
    const std::string &config_dir, std::vector<std::string> *warnings, std::string *error)
{
    Settings          s;
    const std::string path = settings_path(config_dir);
    std::error_code   ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
        return s;
    std::string text, err;
    if (!read_file(path, &text, &err))
    {
        if (error)
            *error = err;
        return s;
    }
    if (!parse_settings(text, &s, warnings) && error)
        *error = path + ": not a settings file";
    return s;
}

bool save_settings(const std::string &config_dir, const Settings &s, std::string *error)
{
    if (config_dir.empty())
        return true;
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    if (ec)
    {
        if (error)
            *error = config_dir + ": cannot be made (" + ec.message() + ")";
        return false;
    }
    std::string err;
    if (!write_file_atomic(settings_path(config_dir), settings_text(s), &err))
    {
        if (error)
            *error = err;
        return false;
    }
    return true;
}

} // namespace rack::ui
