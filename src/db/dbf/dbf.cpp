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

#include "db/dbf/dbf.h"

#include "core/charconv.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <vector>

namespace
{

constexpr uint32_t extended_flag = 0x80000000u;

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    {
        s.remove_suffix(1);
    }
    return s;
}

// Comma-separated fields; a field in double quotes may hold commas ("ideal,1"). Quotes are kept.
std::vector<std::string_view> fields(std::string_view s)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"')
        {
            quoted = !quoted;
        }
        else if (s[i] == ',' && !quoted)
        {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim(s.substr(start)));
    return out;
}

std::string_view unquote(std::string_view s)
{
    return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
}

template <class T>
bool number(std::string_view s, T& v)
{
    s = trim(s);
    if (!s.empty() && s.front() == '+')
    {
        s.remove_prefix(1);
    }
    std::from_chars_result r{};
    if constexpr (std::is_floating_point_v<T>)
    {
        r = float_from_chars(s.data(), s.data() + s.size(), v);
    }
    else
    {
        r = std::from_chars(s.data(), s.data() + s.size(), v);
    }
    const auto [p, ec] = r;
    return ec == std::errc{} && p == s.data() + s.size();
}

// "[TAG] rest" -> rest when line starts with tag.
bool tagged(std::string_view line, std::string_view tag, std::string_view& rest)
{
    if (!line.starts_with(tag))
    {
        return false;
    }
    rest = trim(line.substr(tag.size()));
    return true;
}

// The text of `... "text";` (a DBC-style comment line), empty when there is none.
std::string_view comment_text(std::string_view line)
{
    const auto a = line.find('"');
    const auto b = line.rfind('"');
    return a == std::string_view::npos || b <= a ? std::string_view{} : line.substr(a + 1, b - a - 1);
}

// Raw value range of the signal's type, for signals without min / max.
std::pair<int64_t, int64_t> full_range(const CanDbSignal& s)
{
    if (s.value_type != SignalValueType::integer || s.length >= 64)
    {
        return s.is_unsigned ? std::pair<int64_t, int64_t>{0, -1} : std::pair{INT64_MIN, INT64_MAX}; // U64 max prints -1, as BUSMASTER's %I64d
    }
    if (s.is_unsigned)
    {
        return {0, static_cast<int64_t>((uint64_t{1} << s.length) - 1)};
    }
    return {-(int64_t{1} << (s.length - 1)), (int64_t{1} << (s.length - 1)) - 1};
}

char frame_format(uint32_t raw_id)
{
    return (raw_id & extended_flag) != 0 ? 'X' : 'S';
}

std::string clean(std::string_view text) // one line, no double quotes
{
    std::string out(text);
    std::ranges::replace(out, '"', '\'');
    std::ranges::replace(out, '\n', ' ');
    std::erase(out, '\r');
    return out;
}

} // namespace

bool dbf_parse(std::string_view text, CanDb& db, std::string* error)
{
    const auto fail = [&](int line_no, std::string_view why)
    {
        if (error != nullptr)
        {
            *error = std::format("line {}: {}", line_no, why);
        }
        return false;
    };
    db.messages.clear();
    CanDbMessage* msg = nullptr;
    CanDbSignal* sig = nullptr;
    double version = 1.3;
    enum class Desc { none, msg, sig } desc = Desc::none;
    int line_no = 0;
    while (!text.empty())
    {
        const auto nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        std::string_view rest;
        if (tagged(line, "[DATABASE_VERSION]", rest))
        {
            (void)number(rest, version);
        }
        else if (tagged(line, "[START_MSG]", rest))
        {
            const auto f = fields(rest);
            uint32_t id = 0;
            unsigned length = 0;
            if (f.size() < 6 || !number(f[1], id) || !number(f[2], length))
            {
                return fail(line_no, "bad [START_MSG]");
            }
            const uint32_t raw_id = (id & 0x1FFFFFFFu) | (f[5].starts_with('X') ? extended_flag : 0);
            msg = &db.messages[raw_id];
            *msg = CanDbMessage{.name = std::string(f[0]), .raw_id = raw_id, .dlc = static_cast<uint8_t>(std::min(length, 64u)),
                                .sender = f.size() > 6 ? std::string(f[6]) : std::string()};
            sig = nullptr;
        }
        else if (tagged(line, "[START_SIGNALS]", rest))
        {
            const auto f = fields(rest);
            unsigned length = 0, byte = 0, bit = 0, intel = 1;
            int64_t raw_max = 0, raw_min = 0;
            CanDbSignal s{.name = f.empty() ? std::string() : std::string(f[0])};
            if (msg == nullptr || f.size() < 11 || !number(f[1], length) || !number(f[2], byte) || !number(f[3], bit)
                || !number(f[5], raw_max) || !number(f[6], raw_min) || !number(f[7], intel) || !number(f[8], s.offset)
                || !number(f[9], s.factor) || length == 0 || length > 64 || byte == 0 || bit > 7)
            {
                return fail(line_no, "bad [START_SIGNALS]");
            }
            s.length = static_cast<uint16_t>(length);
            s.big_endian = intel == 0 && version >= 1.3; // BUSMASTER reads older files as all Intel
            const unsigned lsb = s.big_endian ? 8 * (byte - 1) + 7 - bit : 8 * (byte - 1) + bit; // sequential MSB-first for Motorola
            if (s.big_endian && lsb + 1 < length)
            {
                return fail(line_no, "Motorola signal starts before byte 1");
            }
            s.start_bit = static_cast<uint16_t>(s.big_endian ? lsb + 1 - length : lsb);
            const char type = f[4].empty() ? 'U' : f[4][0];
            s.is_unsigned = type == 'U' || type == 'B';
            s.value_type = type == 'F' ? SignalValueType::float32 : type == 'D' ? SignalValueType::float64 : SignalValueType::integer;
            const auto physical = [&](int64_t raw)
            {
                const double r = s.is_unsigned ? static_cast<double>(static_cast<uint64_t>(raw)) : static_cast<double>(raw);
                return r * s.factor + s.offset;
            };
            s.min = std::min(physical(raw_min), physical(raw_max));
            s.max = std::max(physical(raw_min), physical(raw_max));
            s.unit = std::string(unquote(f[10]));
            if (f.size() > 11 && f[11] == "M")
            {
                s.is_muxer = true;
            }
            else if (f.size() > 11 && f[11].size() > 1 && f[11][0] == 'm')
            {
                s.is_muxed = number(f[11].substr(1), s.mux_value);
            }
            msg->signals.push_back(std::move(s));
            sig = &msg->signals.back();
        }
        else if (tagged(line, "[VALUE_DESCRIPTION]", rest))
        {
            const auto f = fields(rest);
            int64_t value = 0;
            if (sig != nullptr && f.size() >= 2 && number(f.back(), value))
            {
                // App files write `text,value` unquoted (the text ends at the last comma).
                const std::string_view label = f.size() == 2 ? unquote(f[0]) : rest.substr(0, rest.rfind(','));
                sig->value_table[static_cast<uint64_t>(value)] = std::string(trim(label));
            }
        }
        else if (line.starts_with("[END_MSG]"))
        {
            if (msg != nullptr)
            {
                const auto it = std::ranges::find_if(msg->signals, [](const CanDbSignal& s) { return s.is_muxer; });
                msg->muxer = it == msg->signals.end() ? -1 : static_cast<int>(it - msg->signals.begin());
            }
            msg = nullptr;
            sig = nullptr;
        }
        else if (line.starts_with("[START_DESC_MSG]") || line.starts_with("[START_DESC_SIG]"))
        {
            desc = line[12] == 'M' ? Desc::msg : Desc::sig;
        }
        else if (line.starts_with("[END_DESC_"))
        {
            desc = Desc::none;
        }
        else if (desc != Desc::none && !line.empty() && line.front() != '[')
        {
            // `id S|X "text";` or `id S|X signal "text";`
            std::istringstream in{std::string(line.substr(0, line.find('"')))};
            uint32_t id = 0;
            std::string format, name;
            in >> id >> format >> name;
            const uint32_t raw_id = (id & 0x1FFFFFFFu) | (format == "X" ? extended_flag : 0);
            if (CanDbMessage* m = can_db_find_message(db, raw_id); m != nullptr)
            {
                if (desc == Desc::msg)
                {
                    m->comment = std::string(comment_text(line));
                }
                else if (CanDbSignal* s = can_db_find_signal(*m, name); s != nullptr)
                {
                    s->comment = std::string(comment_text(line));
                }
            }
        }
    }
    return true;
}

bool dbf_parse_file(const std::filesystem::path& path, CanDb& db, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        if (error != nullptr)
        {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    if (!dbf_parse(text, db, error))
    {
        return false;
    }
    db.path = path.string();
    return true;
}

void dbf_write(const CanDb& db, std::ostream& out)
{
    std::string s = "//******************************BUSMASTER Messages and signals Database ******************************//\n\n"
                    "[DATABASE_VERSION] 1.3\n\n[PROTOCOL] CAN\n\n[BUSMASTER_VERSION] [3.2.2]\n\n";
    std::format_to(std::back_inserter(s), "[NUMBER_OF_MESSAGES] {}\n\n", db.messages.size());
    std::set<std::string> nodes;
    for (const auto& [raw_id, m] : db.messages)
    {
        std::format_to(std::back_inserter(s), "[START_MSG] {},{},{},{},1,{}{}{}\n", m.name, raw_id & 0x1FFFFFFFu, m.dlc, m.signals.size(),
                       frame_format(raw_id), m.sender.empty() ? "" : ",", m.sender);
        if (!m.sender.empty())
        {
            nodes.insert(m.sender);
        }
        for (const CanDbSignal& sig : m.signals)
        {
            // (byte, bit) of the least significant bit; Motorola's sequential start_bit is the MSB.
            const unsigned lsb = sig.big_endian ? sig.start_bit + sig.length - 1u : sig.start_bit;
            const unsigned byte = lsb / 8 + 1;
            const unsigned bit = sig.big_endian ? 7 - lsb % 8 : lsb % 8;
            const char type = sig.value_type == SignalValueType::float32   ? 'F'
                              : sig.value_type == SignalValueType::float64 ? 'D'
                              : sig.length == 1 && sig.is_unsigned          ? 'B'
                              : sig.is_unsigned                             ? 'U'
                                                                            : 'I';
            auto [raw_min, raw_max] = full_range(sig);
            if ((sig.min != 0.0 || sig.max != 0.0) && sig.factor != 0.0)
            {
                const auto raw = [&](double v) { return static_cast<int64_t>(std::llround((v - sig.offset) / sig.factor)); };
                raw_min = std::min(raw(sig.min), raw(sig.max));
                raw_max = std::max(raw(sig.min), raw(sig.max));
            }
            const std::string mux = sig.is_muxer ? "M" : sig.is_muxed ? std::format("m{}", sig.mux_value) : std::string();
            std::format_to(std::back_inserter(s), "[START_SIGNALS] {},{},{},{},{},{},{},{},{},{},{},{},\n", sig.name, sig.length, byte, bit,
                           type, raw_max, raw_min, sig.big_endian ? 0 : 1, sig.offset, sig.factor, clean(sig.unit), mux);
            for (const auto& [value, label] : sig.value_table)
            {
                std::format_to(std::back_inserter(s), "[VALUE_DESCRIPTION] \"{}\",{}\n", clean(label), value);
            }
        }
        s += "[END_MSG]\n\n";
    }
    if (!nodes.empty())
    {
        s += "[NODE] ";
        for (auto it = nodes.begin(); it != nodes.end(); ++it)
        {
            s += (it == nodes.begin() ? "" : ",") + *it;
        }
        s += "\n\n";
    }
    s += "[START_DESC]\n[START_DESC_MSG]\n";
    for (const auto& [raw_id, m] : db.messages)
    {
        if (!m.comment.empty())
        {
            std::format_to(std::back_inserter(s), "{} {} \"{}\";\n", raw_id & 0x1FFFFFFFu, frame_format(raw_id), clean(m.comment));
        }
    }
    s += "[END_DESC_MSG]\n[START_DESC_SIG]\n";
    for (const auto& [raw_id, m] : db.messages)
    {
        for (const CanDbSignal& sig : m.signals)
        {
            if (!sig.comment.empty())
            {
                std::format_to(std::back_inserter(s), "{} {} {} \"{}\";\n", raw_id & 0x1FFFFFFFu, frame_format(raw_id), sig.name,
                               clean(sig.comment));
            }
        }
    }
    s += "[END_DESC_SIG]\n[END_DESC]\n";
    out << s;
}

bool dbf_write_file(const CanDb& db, const std::filesystem::path& path, std::string* error)
{
    std::ofstream out(path, std::ios::binary);
    if (out)
    {
        dbf_write(db, out);
    }
    if (!out)
    {
        if (error != nullptr)
        {
            *error = "cannot write " + path.string();
        }
        return false;
    }
    return true;
}
