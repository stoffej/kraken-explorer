#pragma once

// What the tests need from the OS in one spelling for Linux and Windows.

#include <cstdlib>
#include <filesystem>
#include <string>

// setenv(name, value, 1): Windows has _putenv_s, and a path's c_str() is wide there.
inline void test_setenv(const char* name, const std::filesystem::path& value)
{
#ifdef _WIN32
    _putenv_s(name, value.string().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
