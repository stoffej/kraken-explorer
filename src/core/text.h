/*

  Copyright (c) 2026 Schildkroet

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

// Small text helpers shared by the trace, DBC and replay parsers.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/charconv.h"

// Whole-string conversion: fails on empty input or trailing characters.
template <typename T>
[[nodiscard]] bool parse_number(std::string_view s, T& value, int base = 10)
{
    const char* end = s.data() + s.size();
    std::from_chars_result r{};
    if constexpr (std::is_floating_point_v<T>)
    {
        r = float_from_chars(s.data(), end, value);
    }
    else
    {
        r = std::from_chars(s.data(), end, value, base);
    }
    return r.ec == std::errc{} && r.ptr == end;
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII case-insensitive equality.
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

// Strips leading and trailing whitespace.
[[nodiscard]] constexpr std::string_view trim(std::string_view s) noexcept
{
    constexpr std::string_view space = " \t\n\r\f\v";
    const auto first = s.find_first_not_of(space);
    return first == std::string_view::npos ? std::string_view{} : s.substr(first, s.find_last_not_of(space) - first + 1);
}

// Elapsed seconds the way people read them: "12.345" below a minute, then "4:05.250",
// "3:04:05.250" and "2d 03:04:05.250". decimals: digits after the seconds' point.
inline void append_duration(std::string& out, double seconds, int decimals = 3)
{
    if (seconds < 0.0)
    {
        out += '-';
        seconds = -seconds;
    }
    const double scale = std::pow(10.0, decimals);
    const auto ticks = static_cast<long long>(std::llround(seconds * scale)); // rounded once, so 59.9996 shows as 1:00.000
    const long long whole = ticks / static_cast<long long>(scale);
    const long long frac = ticks % static_cast<long long>(scale);
    const long long d = whole / 86400;
    const long long h = whole / 3600 % 24;
    const long long m = whole / 60 % 60;
    const long long sec = whole % 60;
    auto it = std::back_inserter(out);
    if (d > 0)
    {
        std::format_to(it, "{}d {:02}:{:02}:{:02}", d, h, m, sec);
    }
    else if (h > 0)
    {
        std::format_to(it, "{}:{:02}:{:02}", h, m, sec);
    }
    else if (m > 0)
    {
        std::format_to(it, "{}:{:02}", m, sec);
    }
    else
    {
        std::format_to(it, "{}", sec);
    }
    if (decimals > 0)
    {
        std::format_to(it, ".{:0{}}", frac, decimals);
    }
}

[[nodiscard]] inline std::string format_duration(double seconds, int decimals = 3)
{
    std::string out;
    append_duration(out, seconds, decimals);
    return out;
}

// The inverse, for input fields: "90", "1:30", "1:02:03.5", "2d 1:02:03", "2d"; nullopt when malformed.
// After days two fields are h:m ("1d 0:10" = 10 minutes), as days are always shown with hh:mm:ss.
[[nodiscard]] inline std::optional<double> parse_duration(std::string_view s)
{
    s = trim(s);
    double total = 0.0;
    bool days = false;
    if (const auto d = s.find('d'); d != std::string_view::npos)
    {
        long long count = 0;
        if (!parse_number(trim(s.substr(0, d)), count) || count < 0)
        {
            return std::nullopt;
        }
        total = static_cast<double>(count) * 86400.0;
        days = true;
        s = trim(s.substr(d + 1));
        if (s.empty())
        {
            return total;
        }
    }
    double part = 0.0;
    double clock = 0.0;
    int fields = 0;
    for (;;)
    {
        const auto colon = s.find(':');
        if (++fields > 3 || !parse_number(s.substr(0, colon), part) || part < 0.0)
        {
            return std::nullopt;
        }
        clock = clock * 60.0 + part;
        if (colon == std::string_view::npos)
        {
            return total + (days && fields == 2 ? clock * 60.0 : clock);
        }
        s = s.substr(colon + 1);
    }
}

// Thousands grouped with a space for readability: 1234567 -> "1 234 567".
inline void append_grouped(std::string& out, uint64_t n)
{
    const std::string digits = std::to_string(n);
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0)
        {
            out += ' ';
        }
        out += digits[i];
    }
}
