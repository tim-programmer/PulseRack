#include "rack_test.h"

#include <cstring>

int main(int argc, char **argv)
{
    int ran = 0, failed_cases = 0;
    for (const rt::Test &t : rt::tests())
    {
        if (argc > 1 && !std::strstr(t.name, argv[1]))
            continue;
        rt::current() = t.name;
        const int before = rt::failures();
        t.fn();
        const bool bad = rt::failures() != before;
        std::printf("%s %s\n", bad ? "FAIL" : "ok  ", t.name);
        ran++;
        failed_cases += bad;
    }
    std::printf("\n%d cases, %d checks, %d failed checks in %d failed cases\n", ran, rt::checks(),
        rt::failures(), failed_cases);
    return rt::failures() == 0 && ran > 0 ? 0 : 1;
}
