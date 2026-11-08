// A tiny test harness: TEST(name) registers a case, CHECK(cond) counts and reports failures. No
// dependencies.
#pragma once

#include <cstdio>
#include <vector>

namespace rt
{

struct Test
{
    const char *name;
    void (*fn)();
};
inline std::vector<Test> &tests()
{
    static std::vector<Test> t;
    return t;
}
struct Reg
{
    Reg(const char *n, void (*f)()) { tests().push_back({ n, f }); }
};
inline int &checks()
{
    static int n = 0;
    return n;
}
inline int &failures()
{
    static int n = 0;
    return n;
}
inline const char *&current()
{
    static const char *c = "";
    return c;
}

inline void check(bool ok, const char *expr, const char *file, int line)
{
    checks()++;
    if (!ok)
    {
        failures()++;
        if (failures() <= 40)
            std::printf("  FAIL %s:%d [%s] %s\n", file, line, current(), expr);
    }
}

} // namespace rt

#define TEST(name)                                                                                 \
    static void    name();                                                                         \
    static rt::Reg reg_##name(#name, name);                                                        \
    static void    name()
#define CHECK(c) rt::check(static_cast<bool>(c), #c, __FILE__, __LINE__)
