/*

  Copyright (c) 2026 The Kraken Explorer contributors

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.

*/


#pragma once

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string>
#include <type_traits>

#if __cpp_lib_to_chars < 201611L
#include <xlocale.h> // strtod_l, LC_C_LOCALE (Apple)
#endif

// std::from_chars for float and double. Apple's libc++ only has the floating-point overloads
// from macOS 26 on, so before that strtod_l in the C locale stands in, held to from_chars
// rules: no leading whitespace or '+', and "0x1p3" parses as the 0 in front of the x.
template <typename T>
    requires std::is_floating_point_v<T>
std::from_chars_result float_from_chars(const char* first, const char* last, T& value)
{
#if __cpp_lib_to_chars >= 201611L
    return std::from_chars(first, last, value);
#else
    const char* digits = first < last && *first == '-' ? first + 1 : first;
    if (digits == last || *digits == '+' || std::isspace(static_cast<unsigned char>(*digits)))
    {
        return {first, std::errc::invalid_argument};
    }
    if (last - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        last = digits + 1;
    }
    const std::string text(first, last); // strtod wants a terminated string
    char* end = nullptr;
    errno = 0;
    T v{};
    if constexpr (std::is_same_v<T, float>)
    {
        v = strtof_l(text.c_str(), &end, LC_C_LOCALE);
    }
    else
    {
        v = static_cast<T>(strtod_l(text.c_str(), &end, LC_C_LOCALE));
    }
    if (end == text.c_str())
    {
        return {first, std::errc::invalid_argument};
    }
    const char* ptr = first + (end - text.c_str());
    if (errno == ERANGE)
    {
        return {ptr, std::errc::result_out_of_range};
    }
    value = v;
    return {ptr, std::errc{}};
#endif
}
