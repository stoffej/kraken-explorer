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

// System time <-> local (wall clock) time. std::chrono::current_zone() where the standard
// library has the time zone database (libstdc++, MSVC); Apple's libc++ has none, so there the
// C library does it (localtime_r / mktime). Without tzdata both treat local time as UTC.

#include <chrono>
#include <ctime>
#include <exception>
#include <type_traits>

#if __cpp_lib_chrono >= 201907L
// The process' time zone, nullptr without tzdata.
[[nodiscard]] inline const std::chrono::time_zone* local_zone()
{
    static const std::chrono::time_zone* const zone = []() -> const std::chrono::time_zone*
    {
        try
        {
            return std::chrono::current_zone();
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }();
    return zone;
}
#endif

template <typename Duration>
[[nodiscard]] std::chrono::local_time<std::common_type_t<Duration, std::chrono::seconds>>
to_local_time(std::chrono::sys_time<Duration> t)
{
    using namespace std::chrono;
    using Result = local_time<std::common_type_t<Duration, seconds>>;
#if __cpp_lib_chrono >= 201907L
    const time_zone* zone = local_zone();
    return zone != nullptr ? zone->to_local(t) : Result{t.time_since_epoch()};
#else
    const std::time_t s = system_clock::to_time_t(floor<seconds>(t));
    std::tm tm{};
    if (localtime_r(&s, &tm) == nullptr)
    {
        return Result{t.time_since_epoch()};
    }
    return Result{t.time_since_epoch() + seconds(tm.tm_gmtoff)};
#endif
}

// A local time repeated by a DST change maps to its earlier instant, one skipped by it to the
// moment of the change (tzdb's choose::earliest; mktime picks as the C library does).
template <typename Duration>
[[nodiscard]] std::chrono::sys_time<std::common_type_t<Duration, std::chrono::seconds>>
to_sys_time(std::chrono::local_time<Duration> t)
{
    using namespace std::chrono;
    using Result = sys_time<std::common_type_t<Duration, seconds>>;
#if __cpp_lib_chrono >= 201907L
    const time_zone* zone = local_zone();
    return zone != nullptr ? zone->to_sys(t, choose::earliest) : Result{t.time_since_epoch()};
#else
    const local_seconds secs = floor<seconds>(t);
    const local_days day = floor<days>(secs);
    const year_month_day ymd{day};
    const hh_mm_ss hms{secs - day};
    std::tm tm{};
    tm.tm_year = static_cast<int>(ymd.year()) - 1900;
    tm.tm_mon = static_cast<int>(static_cast<unsigned>(ymd.month())) - 1;
    tm.tm_mday = static_cast<int>(static_cast<unsigned>(ymd.day()));
    tm.tm_hour = static_cast<int>(hms.hours().count());
    tm.tm_min = static_cast<int>(hms.minutes().count());
    tm.tm_sec = static_cast<int>(hms.seconds().count());
    tm.tm_isdst = -1;
    const std::time_t s = std::mktime(&tm);
    if (s == -1)
    {
        return Result{t.time_since_epoch()};
    }
    return Result{seconds(s) + (t - secs)};
#endif
}
