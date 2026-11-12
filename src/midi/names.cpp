#include "midi/names.h"

#include <algorithm>
#include <map>

namespace rack::midi
{

std::vector<std::string> unique_names(std::span<const std::string> raw)
{
    std::vector<std::string>   out;
    std::map<std::string, int> seen;
    out.reserve(raw.size());
    for (const std::string &r : raw)
    {
        const std::string base = r.empty() ? std::string("MIDI input") : r;
        // the next free number: a device really named "X #2" must not collide with the second "X"
        int         n = seen[base];
        std::string name;
        do
        {
            n++;
            name = n == 1 ? base : base + " #" + std::to_string(n);
        } while (std::find(out.begin(), out.end(), name) != out.end());
        seen[base] = n;
        out.push_back(std::move(name));
    }
    return out;
}

} // namespace rack::midi
