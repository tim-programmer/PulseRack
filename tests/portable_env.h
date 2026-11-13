// setenv and unsetenv for the tests on Windows, which has _putenv_s instead (an empty value removes the variable).
#pragma once

#include <cstdlib>

#ifdef _WIN32
inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && std::getenv(name))
        return 0;
    return _putenv_s(name, value);
}
inline int unsetenv(const char *name) { return _putenv_s(name, ""); }
#endif
