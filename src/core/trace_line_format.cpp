/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "trace_line_format.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

#include "core/local_time.h"
#include "core/socket_can.h"
#include "core/text.h"

namespace
{

// Enhanced LIN checksum (LIN 2.x): sum of PID + data bytes, carry-folded, inverted
uint8_t lin_enhanced_checksum(uint8_t frame_id, const BusMessage& m)
{
    const uint8_t id = frame_id & 0x3Fu;
    const uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1u;
    const uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1u;
    uint16_t sum = id | (p0 << 6u) | (p1 << 7u);
    for (int i = 0; i < m.len; ++i)
    {
        sum += m.data[i];
        if (sum > 255)
        {
            sum -= 255;
        }
    }
    return static_cast<uint8_t>(~sum & 0xFFu);
}

// "11 22 33 ": every byte followed by a space, as the ASC lines have always been written.
void append_asc_data(std::string& out, const BusMessage& m)
{
    append_hex_bytes(out, std::span(m.data).first(m.len), " ");
    if (m.len > 0)
    {
        out += ' ';
    }
}

}

void append_asc_header(std::string& out, std::chrono::system_clock::time_point start)
{
    // Qt's "ddd MMM dd hh:mm:ss.zzz ap yyyy" in the C locale, local time.
    const auto local = to_local_time(std::chrono::floor<std::chrono::milliseconds>(start));
    const bool pm = std::format("{:%p}", local) == "PM";
    const std::string dt = std::format("{:%a %b %d %I:%M:%S} {} {:%Y}", local, pm ? "pm" : "am", local);
    std::format_to(std::back_inserter(out),
                   "date {0}\n"
                   "base hex  timestamps absolute\n"
                   "internal events logged\n"
                   "// version 8.5.0\n"
                   "Begin Triggerblock {0}\n"
                   "   0.000000 Start of measurement\n",
                   dt);
}

void append_asc_line(std::string& out, const BusMessage& m, int64_t start_ns, int channel)
{
    const double t = static_cast<double>(m.ts_ns - start_ns) / 1e9;
    const std::string_view dir = has_flag(m, bus_flag::tx) ? "Tx" : "Rx";
    auto it = std::back_inserter(out);

    if (m.type == BusType::LIN)
    {
        const auto lin_id = static_cast<uint8_t>(m.id & 0x3Fu);
        if (is_error_frame(m))
        {
            std::format_to(it, "{:11.6f} {}  LIN {:02x} {}   LIN_ChecksumError", t, channel, lin_id, dir);
            return;
        }
        std::format_to(it, "{:11.6f} {}  LIN {:02x} {}  d {} ", t, channel, lin_id, dir, m.len);
        append_asc_data(out, m);
        std::format_to(it, "checksum = {:02x} header_time = 0 full_time = 0", lin_enhanced_checksum(lin_id, m));
        return;
    }

    if (is_error_frame(m))
    {
        std::format_to(it, "{:11.6f} {}  ErrorFrame", t, channel);
        return;
    }

    const std::string_view x = has_flag(m, bus_flag::extended) ? "x" : "";
    const std::string id_hex = std::format("{:x}{}", m.id, x);

    if (has_flag(m, bus_flag::fd))
    {
        // Vector ASC CAN FD event:
        // <time> CANFD <channel> <Rx|Tx> <id> <BRS> <ESI> <DLC hex> <data length> <data>
        // ESI is not tracked by BusMessage and is written as 0.
        std::format_to(it, "{:11.6f} CANFD {:3} {} {:>15} {} 0 {:x} {} ", t, channel, dir, id_hex,
                       has_flag(m, bus_flag::brs) ? 1 : 0, bus_length_to_dlc(m.len), m.len);
        append_asc_data(out, m);
        return;
    }

    std::format_to(it, "{:11.6f} {}  {:<15} {}   {} {} ", t, channel, id_hex, dir,
                   has_flag(m, bus_flag::rtr) ? 'r' : 'd', m.len);
    if (!has_flag(m, bus_flag::rtr)) // a remote frame's length is the DLC it requests, it carries no data
    {
        append_asc_data(out, m);
    }
    std::format_to(it, "  Length = 0 BitCount = 0 ID = {}{}", m.id, x);
}

void append_candump_line(std::string& out, const BusMessage& m, std::string_view iface_name)
{
    // Integer seconds + microseconds: exact, unlike going through a double.
    auto it = std::format_to(std::back_inserter(out), "({}.{:06}) {}",
                             m.ts_ns / 1000000000, (m.ts_ns % 1000000000) / 1000, iface_name);
    // Fields are separated by spaces: a name with spaces (the PEAK channel "PCAN-USB (USB1)") is
    // written with underscores, or no candump reader, this one included, finds the frame.
    std::replace_if(out.end() - static_cast<std::ptrdiff_t>(iface_name.size()), out.end(),
                    [](char c) { return c == ' ' || c == '\t'; }, '_');

    if (is_error_frame(m))
    {
        // Error flag and error classes in the id, 8-byte error payload (<linux/can/error.h>).
        const socket_can::ErrorFrame error = socket_can::error_frame(m);
        std::format_to(it, " {:08X}#", socket_can::err_flag | error.classes);
        append_hex_bytes(out, error.data);
        return;
    }

    const int id_width = has_flag(m, bus_flag::extended) ? 8 : 3;
    const std::span<const uint8_t> payload(m.data.data(), m.len);
    if (has_flag(m, bus_flag::fd))
    {
        // CANFD: ## separator with flags nibble (bit 0 = BRS, bit 1 = ESI)
        std::format_to(it, " {:0{}X}##{}", m.id, id_width, has_flag(m, bus_flag::brs) ? 1 : 0);
        append_hex_bytes(out, payload);
    }
    else if (has_flag(m, bus_flag::rtr))
    {
        std::format_to(it, " {:0{}X}#R{}", m.id, id_width, m.len);
    }
    else
    {
        std::format_to(it, " {:0{}X}#", m.id, id_width);
        append_hex_bytes(out, payload);
    }
}

bool parse_asc_canfd_line(std::span<const std::string_view> parts, BusMessage& m)
{
    if (parts.size() < 9 || !iequals(parts[1], "CANFD"))
    {
        return false;
    }

    int channel = 0;
    if (!parse_number(parts[2], channel))
    {
        return false;
    }

    std::string_view id_str = parts[4];
    const bool extended = !id_str.empty() && (id_str.back() == 'x' || id_str.back() == 'X');
    if (extended)
    {
        id_str.remove_suffix(1);
    }
    uint32_t id = 0;
    if (!parse_number(id_str, id, 16))
    {
        return false;
    }

    // Optional symbolic frame name between the id and BRS.
    size_t idx = 5;
    if (int probe = 0; !parse_number(parts[idx], probe))
    {
        ++idx;
    }
    if (parts.size() < idx + 4)
    {
        return false;
    }

    int brs_or_flags = 0;
    if (!parse_number(parts[idx], brs_or_flags))
    {
        return false;
    }

    uint8_t dlc = 0;
    int len = 0;
    int len_again = 0;
    const bool dlc_ok = parse_number(parts[idx + 2], dlc, 16);
    const bool len_ok = parse_number(parts[idx + 3], len);
    size_t data_idx = 0;

    // Vector layout: the DLC code must agree with the data length.
    if (dlc_ok && len_ok && dlc < bus_dlc_lengths.size() && bus_dlc_lengths[dlc] == len)
    {
        data_idx = idx + 4;
    }
    // Legacy layout: flags, two reserved zeros, then the byte count twice.
    else if (parts.size() >= idx + 5 && parts[idx + 1] == "0" && parts[idx + 2] == "0"
             && len_ok && parse_number(parts[idx + 4], len_again) && len_again == len)
    {
        data_idx = idx + 5;
    }
    else
    {
        return false;
    }

    if (len < 0 || len > bus_max_data_bytes || parts.size() < data_idx + static_cast<size_t>(len))
    {
        return false;
    }

    std::array<uint8_t, bus_max_data_bytes> data{};
    for (int i = 0; i < len; ++i)
    {
        unsigned value = 0;
        if (!parse_number(parts[data_idx + static_cast<size_t>(i)], value, 16) || value > 0xFF)
        {
            return false;
        }
        data[static_cast<size_t>(i)] = static_cast<uint8_t>(value);
    }

    m.flags &= static_cast<uint16_t>(~(bus_flag::brs | bus_flag::extended | bus_flag::tx));
    m.flags |= bus_flag::fd;
    if (brs_or_flags & 0x1)
    {
        m.flags |= bus_flag::brs;
    }
    if (extended)
    {
        m.flags |= bus_flag::extended;
    }
    if (!iequals(parts[3], "Rx"))
    {
        m.flags |= bus_flag::tx;
    }
    m.id = id;
    m.iface = static_cast<uint16_t>(channel);
    set_length(m, len);
    m.data = data;
    return true;
}

namespace
{

constexpr int64_t ns_per_day = 86400LL * 1000000000LL;
constexpr double unix_epoch_days = 25569.0;  // 1970-01-01 in days since 1899-12-30

// "1059.900" (ms, up to 6 decimals) -> ns, exact.
bool parse_trc_ms(std::string_view s, int64_t& ns)
{
    const auto dot = s.find('.');
    int64_t ms = 0;
    if (!parse_number(s.substr(0, dot), ms) || ms < 0)
    {
        return false;
    }
    int64_t frac = 0;
    if (dot != std::string_view::npos)
    {
        const std::string_view f = s.substr(dot + 1, 6);
        if (!f.empty() && !parse_number(f, frac))
        {
            return false;
        }
        for (size_t i = f.size(); i < 6; ++i)
        {
            frac *= 10;
        }
    }
    ns = ms * 1000000 + frac;
    return true;
}

}

void parse_trc_header_line(std::string_view line, TrcLayout& layout)
{
    const auto eq = line.find('=');
    if (!line.starts_with(";$") || eq == std::string_view::npos)
    {
        return;
    }
    const std::string_view key = line.substr(2, eq - 2);
    const std::string_view value = trim(line.substr(eq + 1));
    if (key == "FILEVERSION")
    {
        // Fixed 1.x layouts; 2.x defaults for files without ;$COLUMNS ('R' = reserved, 'l' = byte count).
        if (value == "1.1")      { layout.columns = "NOdILD"; }
        else if (value == "1.3") { layout.columns = "NOBdIRLD"; }
        else if (value == "2.0") { layout.columns = "NOTIdlD"; }
        else if (value == "2.1") { layout.columns = "NOTBIdRLD"; }
    }
    else if (key == "STARTTIME")
    {
        double days = 0;
        if (parse_number(value, days))
        {
            layout.start_ns = std::llround((days - unix_epoch_days) * 86400e6) * 1000;
        }
    }
    else if (key == "COLUMNS")
    {
        // "N,O,T,B,I,d,R,L,D" -> "NOTBIdRLD"
        layout.columns.clear();
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] != ',' && (i == 0 || value[i - 1] == ','))
            {
                layout.columns += value[i];
            }
        }
    }
}

bool parse_trc_line(std::span<const std::string_view> parts, const TrcLayout& layout, BusMessage& m)
{
    m = BusMessage{};
    std::string_view type;
    std::string_view dir = "Rx";
    std::string_view id_str;
    int bus = 1;
    int length = -1;  // 'l': bytes
    int dlc = -1;     // 'L': DLC code
    int64_t offset_ns = 0;
    size_t data_idx = parts.size();
    for (size_t c = 0; c < layout.columns.size(); ++c)
    {
        const char col = layout.columns[c];
        if (col == 'D')
        {
            data_idx = c;
            break;
        }
        if (c >= parts.size())
        {
            return false;
        }
        const std::string_view v = parts[c];
        bool ok = true;
        switch (col)
        {
            case 'O': ok = parse_trc_ms(v, offset_ns); break;
            case 'T': type = v; break;
            case 'B': ok = parse_number(v, bus); break;
            case 'I': id_str = v; break;
            case 'd': dir = v; break;
            case 'l': ok = parse_number(v, length); break;
            case 'L': ok = parse_number(v, dlc); break;
            default: break;  // N, R and unknown columns
        }
        if (!ok)
        {
            return false;
        }
    }

    const bool rx = dir == "Rx";
    if (!rx && dir != "Tx" && dir != "Error")
    {
        return false;  // 1.x Warng / bus status lines
    }
    m.ts_ns = layout.start_ns + offset_ns;
    m.iface = static_cast<uint16_t>(bus);
    m.flags = rx ? 0 : bus_flag::tx;
    if (dir == "Error" || type == "ER")
    {
        // ponytail: the ER payload (error type, direction, position, counters) is not decoded; generic error. Decode when the trace shows error details.
        m.flags = 0;
        m.errors = bus_error::generic;
        return true;
    }

    if (type == "FD" || type == "FB" || type == "FE" || type == "BI")
    {
        m.flags |= bus_flag::fd | ((type == "FB" || type == "FE") ? bus_flag::brs : 0);
    }
    else if (type == "RR")
    {
        m.flags |= bus_flag::rtr;
    }
    else if (!type.empty() && type != "DT")
    {
        return false;  // ST, EC, EV, ...
    }

    if (!parse_number(id_str, m.id, 16) || m.id > can_id_mask_extended)
    {
        return false;
    }
    if (id_str.size() > 4 || m.id > can_id_mask_standard)
    {
        m.flags |= bus_flag::extended;
    }

    const bool fd = has_flag(m, bus_flag::fd);
    if (length < 0)
    {
        if (dlc < 0 || dlc > 15)
        {
            return false;
        }
        length = fd ? bus_dlc_lengths[static_cast<size_t>(dlc)] : std::min(dlc, 8);
    }
    if (length > (fd ? bus_max_data_bytes : 8))
    {
        return false;
    }
    set_length(m, length);

    // 1.x marks remote frames with "RTR" in place of the data.
    if (data_idx < parts.size() && parts[data_idx] == "RTR")
    {
        m.flags |= bus_flag::rtr;
    }
    if (has_flag(m, bus_flag::rtr))
    {
        return true;
    }
    if (parts.size() < data_idx + static_cast<size_t>(length))
    {
        return false;
    }
    for (int i = 0; i < length; ++i)
    {
        if (!parse_number(parts[data_idx + static_cast<size_t>(i)], m.data[static_cast<size_t>(i)], 16))
        {
            return false;
        }
    }
    return true;
}

void append_trc_header(std::string& out, int64_t start_ns)
{
    // python-can's TRCWriter header (v2.1); STARTTIME as the shortest round-trip double.
    const double days = unix_epoch_days + static_cast<double>(start_ns) / static_cast<double>(ns_per_day);
    std::format_to(std::back_inserter(out),
                   ";$FILEVERSION=2.1\r\n"
                   ";$STARTTIME={}\r\n"
                   ";$COLUMNS=N,O,T,B,I,d,R,L,D\r\n"
                   ";\r\n"
                   ";   Generated by Kraken Explorer\r\n"
                   ";-------------------------------------------------------------------------------\r\n"
                   ";   Message   Time    Type    ID     Rx/Tx\r\n"
                   ";   Number    Offset  |  Bus  [hex]  |  Reserved\r\n"
                   ";   |         [ms]    |  |    |      |  |  Data Length Code\r\n"
                   ";   |         |       |  |    |      |  |  |    Data [hex] ...\r\n"
                   ";   |         |       |  |    |      |  |  |    |\r\n"
                   ";---+-- ------+------ +- +- --+----- +- +- +--- +- -- -- -- -- -- -- --\r\n",
                   days);
}

void append_trc_line(std::string& out, const BusMessage& m, uint64_t number, int64_t start_ns, int bus)
{
    const int64_t us = std::max<int64_t>(m.ts_ns - start_ns, 0) / 1000;
    const std::string_view dir = has_flag(m, bus_flag::tx) ? "Tx" : "Rx";
    auto it = std::format_to(std::back_inserter(out), "{:>7} {:>9}.{:03} ", number, us / 1000, us % 1000);
    if (is_error_frame(m))
    {
        // PEAK error frame: error type (8 = other), direction (0 = Tx, 1 = Rx), bit position, RX / TX counter.
        std::format_to(it, "ER {:>2} {:>8} {} -  5    08 {:02X} 00 00 00", bus, "-", dir, has_flag(m, bus_flag::tx) ? 0 : 1);
        return;
    }
    std::string_view type = "DT";
    if (has_flag(m, bus_flag::fd))
    {
        type = has_flag(m, bus_flag::brs) ? "FB" : "FD";
    }
    else if (has_flag(m, bus_flag::rtr))
    {
        type = "RR";
    }
    // 4 hex digits for standard ids, 8 for extended: readers tell them apart by the width.
    std::format_to(it, "{} {:>2} {:>8} {} -  {}", type, bus,
                   has_flag(m, bus_flag::extended) ? std::format("{:08X}", m.id) : std::format("{:04X}", m.id), dir,
                   m.dlc);
    if (!has_flag(m, bus_flag::rtr) && m.len > 0)
    {
        out.append(m.dlc < 10 ? 4 : 3, ' ');  // DLC column 4 wide, then the data
        append_hex_bytes(out, std::span(m.data).first(m.len), " ");
    }
}
