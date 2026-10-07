#include "ui/replay.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <zlib.h>

#include "app.h"
#include "core/platform.h"
#include "core/text.h"
#include "core/trace_line_format.h"
#include "ui/depth_gauge.h"
#include "ui/frame_cache.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/status_bar.h"

namespace
{

// ---------------------------------------------------------------- parsing helpers

// "12.345678" -> ns, exact for up to 9 fractional digits.
bool parse_seconds_ns(std::string_view s, int64_t& ns)
{
    const auto dot = s.find('.');
    int64_t sec = 0;
    if (!parse_number(s.substr(0, dot), sec))
    {
        return false;
    }
    int64_t frac = 0;
    if (dot != std::string_view::npos)
    {
        std::string_view f = s.substr(dot + 1, 9);
        if (!f.empty() && !parse_number(f, frac))
        {
            return false;
        }
        for (std::size_t i = f.size(); i < 9; ++i)
        {
            frac *= 10;
        }
    }
    ns = sec * 1000000000 + frac;
    return true;
}

// The parts live until the next call on this thread: one vector reused instead of one per line
// (the allocation was the parsers' biggest cost under perf).
const std::vector<std::string_view>& split_ws(std::string_view line)
{
    thread_local std::vector<std::string_view> parts;
    parts.clear();
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
        {
            ++i;
        }
        const std::size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
        {
            ++i;
        }
        if (i > b)
        {
            parts.push_back(line.substr(b, i - b));
        }
    }
    return parts;
}

// No std::string per frame: a view is compared, and the channel of the previous frame is tried
// first (nearly every line has it). The name is only copied when it is new.
uint16_t channel_index(ReplayFile& f, std::string_view name)
{
    if (f.last_channel < f.channels.size() && f.channels[f.last_channel] == name)
    {
        return static_cast<uint16_t>(f.last_channel);
    }
    const auto it = std::ranges::find(f.channels, name);
    if (it != f.channels.end())
    {
        f.last_channel = static_cast<std::size_t>(it - f.channels.begin());
        return static_cast<uint16_t>(f.last_channel);
    }
    f.channels.emplace_back(name);
    f.last_channel = f.channels.size() - 1;
    return static_cast<uint16_t>(f.last_channel);
}

// "CH <n>" without a std::string allocation per frame.
uint16_t numbered_channel(ReplayFile& f, unsigned n)
{
    char buf[16];
    const int len = std::snprintf(buf, sizeof buf, "CH %u", n);
    return channel_index(f, std::string_view(buf, static_cast<std::size_t>(len)));
}

bool parse_hex_bytes(std::string_view hex, BusMessage& m)
{
    if (hex.size() % 2 != 0 || hex.size() / 2 > bus_max_data_bytes)
    {
        return false;
    }
    const auto nibble = [](char c) -> int
    {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (std::size_t i = 0; i < hex.size() / 2; ++i)
    {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        m.data[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    set_length(m, static_cast<int>(hex.size() / 2));
    return true;
}

// Every 4096 calls: false on a stop request (the frame cache build publishes the progress itself).
bool parse_tick(const ReplayParseProgress& p, std::size_t& n)
{
    return ++n % 4096 != 0 || !p.stop.stop_requested();
}

template <class F>
void for_each_line(std::string_view data, const ReplayParseProgress& p, F&& fn)
{
    const std::size_t total = data.size();
    std::size_t n = 0;
    while (!data.empty() && parse_tick(p, n))
    {
        const auto nl = data.find('\n');
        fn(data.substr(0, nl));
        data = nl == std::string_view::npos ? std::string_view{} : data.substr(nl + 1);
    }
}

// "(ts) iface ID#DATA", "ID##<flags>DATA" (FD), "ID#R[len]" (RTR); error flag in the id.
// "(sec.frac) iface id#payload", scanned in place: splitting every line into fields was a fifth of
// the parse (perf), and candump is the format the big logs come in.
void parse_candump(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    for_each_line(data, p, [&](std::string_view line)
    {
        const auto close = line.find(')');
        if (line.size() < 5 || line.front() != '(' || close == std::string_view::npos)
        {
            return;
        }
        BusMessage m;
        if (!parse_seconds_ns(line.substr(1, close - 1), m.ts_ns))
        {
            return;
        }
        std::size_t i = close + 1;
        while (i < line.size() && is_space(line[i]))
        {
            ++i;
        }
        std::size_t e = i;
        while (e < line.size() && !is_space(line[e]))
        {
            ++e;
        }
        const std::string_view iface = line.substr(i, e - i);
        for (i = e; i < line.size() && is_space(line[i]); ++i)
        {
        }
        for (e = i; e < line.size() && !is_space(line[e]); ++e)
        {
        }
        const std::string_view frame = line.substr(i, e - i);
        if (iface.empty() || frame.empty())
        {
            return;
        }
        const auto hash = frame.find('#');
        uint32_t raw = 0;
        if (hash == std::string_view::npos || !parse_number(frame.substr(0, hash), raw, 16))
        {
            return;
        }
        std::string_view payload = frame.substr(hash + 1);
        if ((raw & 0x20000000u) != 0) // CAN_ERR_FLAG
        {
            m.errors = bus_error::generic;
            m.id = raw & can_id_mask_extended;
        }
        else
        {
            m.id = raw & can_id_mask_extended;
            if (hash == 8 || m.id > can_id_mask_standard)
            {
                m.flags |= bus_flag::extended;
            }
            if (!payload.empty() && payload.front() == '#')
            {
                uint8_t fd_flags = 0;
                if (payload.size() < 2 || !parse_number(payload.substr(1, 1), fd_flags, 16))
                {
                    return;
                }
                m.flags |= bus_flag::fd | ((fd_flags & 1) != 0 ? bus_flag::brs : 0);
                if (!parse_hex_bytes(payload.substr(2), m))
                {
                    return;
                }
            }
            else if (!payload.empty() && (payload.front() == 'R' || payload.front() == 'r'))
            {
                int len = 0;
                if (payload.size() > 1 && !parse_number(payload.substr(1), len))
                {
                    return;
                }
                m.flags |= bus_flag::rtr;
                set_length(m, std::min(len, 8));
            }
            else if (!parse_hex_bytes(payload, m) || m.len > 8)
            {
                return;
            }
        }
        m.iface = channel_index(f, iface);
        f.frames.push_back(m);
    });
}

// Vector ASC events: classic, CANFD, LIN and ErrorFrame lines; everything else is skipped.
// ASC "date Tue Nov 14 10:13:20.000 pm 2023" (Qt / our writer) or "date Tue Nov 14 22:13:20 2023"
// (24 h) in local time -> ns since the epoch; 0 when the line does not parse. Frame times count from it.
int64_t asc_date_ns(std::string_view header)
{
    const auto at = header.find("date ");
    if (at == std::string_view::npos)
    {
        return 0;
    }
    const auto& t = split_ws(header.substr(at + 5, header.find('\n', at) - at - 5));
    static constexpr std::string_view months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto month = t.size() >= 5 ? std::ranges::find_if(months, [&](std::string_view m) { return iequals(m, t[1].substr(0, 3)); })
                                     : std::end(months);
    unsigned day = 0, h = 0, min = 0;
    int year = 0;
    int64_t sec_ns = 0;
    const std::string_view clock = t.size() >= 5 ? t[3] : std::string_view{};
    const auto c1 = clock.find(':');
    const auto c2 = clock.rfind(':');
    if (month == std::end(months) || !parse_number(t[2], day) || c1 == std::string_view::npos || c2 == c1
        || !parse_number(clock.substr(0, c1), h) || !parse_number(clock.substr(c1 + 1, c2 - c1 - 1), min)
        || !parse_seconds_ns(clock.substr(c2 + 1), sec_ns) || !parse_number(t.back(), year))
    {
        return 0;
    }
    if (t.size() >= 6 && (iequals(t[4], "pm") || iequals(t[4], "am")))
    {
        h = h % 12 + (iequals(t[4], "pm") ? 12 : 0);
    }
    using namespace std::chrono;
    const local_time<nanoseconds> local = local_days{year_month_day{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month - std::begin(months)) + 1},
                                                                    std::chrono::day{day}}}
                                          + hours{h} + minutes{min} + nanoseconds{sec_ns};
    try
    {
        return current_zone()->to_sys(local).time_since_epoch().count();
    }
    catch (const std::exception&) // a local time skipped or repeated by a DST change
    {
        return duration_cast<nanoseconds>(local.time_since_epoch()).count();
    }
}

void parse_asc(std::string_view header, std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    const int64_t base = asc_date_ns(header);
    for_each_line(data, p, [&](std::string_view line)
    {
        const auto& parts = split_ws(line);
        BusMessage m;
        if (parts.size() < 3 || !parse_seconds_ns(parts[0], m.ts_ns))
        {
            return;
        }
        std::string_view channel = parts[1];
        if (iequals(parts[2], "ErrorFrame"))
        {
            m.errors = bus_error::generic;
        }
        else if (iequals(parts[1], "CANFD"))
        {
            const int64_t ts = m.ts_ns;
            if (!parse_asc_canfd_line(parts, m))
            {
                return;
            }
            m.ts_ns = ts;
            channel = parts[2];
        }
        else if (iequals(parts[2], "LIN") && parts.size() >= 6)
        {
            // <t> <ch> LIN <id> <Rx|Tx> d <len> <bytes...> checksum = ..., or LIN_<error>
            m.type = BusType::LIN;
            if (!parse_number(parts[3], m.id, 16))
            {
                return;
            }
            m.id &= 0x3F;
            m.flags |= iequals(parts[4], "Tx") ? bus_flag::tx : 0;
            if (parts[5].starts_with("LIN_"))
            {
                m.errors = bus_error::lin_checksum_error;
            }
            else
            {
                int len = 0;
                if (!iequals(parts[5], "d") || parts.size() < 7 || !parse_number(parts[6], len) || len > 8)
                {
                    return;
                }
                set_length(m, len);
                for (int i = 0; i < len && 7 + i < static_cast<int>(parts.size()); ++i)
                {
                    if (!parse_number(parts[7 + i], m.data[i], 16))
                    {
                        break; // "checksum"
                    }
                }
            }
        }
        else
        {
            // <t> <ch> <id>[x] <Rx|Tx> <d|r> <dlc> <bytes...>
            if (parts.size() < 6)
            {
                return;
            }
            std::string_view id = parts[2];
            if (id.ends_with('x') || id.ends_with('X'))
            {
                id.remove_suffix(1);
                m.flags |= bus_flag::extended;
            }
            int len = 0;
            if (!parse_number(id, m.id, 16) || !parse_number(parts[5], len) || len > 8)
            {
                return;
            }
            m.flags |= iequals(parts[3], "Tx") ? bus_flag::tx : 0;
            m.flags |= iequals(parts[4], "r") ? bus_flag::rtr : 0;
            set_length(m, len);
            if (!has_flag(m, bus_flag::rtr))
            {
                for (int i = 0; i < len; ++i)
                {
                    if (6 + i >= static_cast<int>(parts.size()) || !parse_number(parts[6 + i], m.data[i], 16))
                    {
                        return;
                    }
                }
            }
        }
        thread_local std::string name; // "CH <n>" without an allocation per line
        name.assign("CH ").append(channel);
        m.iface = channel_index(f, name);
        f.frames.push_back(m);
    });
    if (base != 0)
    {
        for (BusMessage& m : f.frames)
        {
            m.ts_ns += base;
        }
    }
}

// PEAK PCAN trace: ";" header / comment lines, then one frame per line (see parse_trc_line).
void parse_trc(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    TrcLayout layout;
    for_each_line(data, p, [&](std::string_view line)
    {
        if (line.starts_with(';'))
        {
            parse_trc_header_line(line, layout);
            return;
        }
        BusMessage m;
        if (parse_trc_line(split_ws(line), layout, m))
        {
            m.iface = numbered_channel(f, m.iface);
            f.frames.push_back(m);
        }
    });
}

// Bounds-checked little/big-endian reads from the file bytes.
struct Bytes
{
    std::string_view d;
    bool swap = false;

    [[nodiscard]] bool has(std::size_t off, std::size_t n) const { return off <= d.size() && n <= d.size() - off; }
    [[nodiscard]] uint32_t u32(std::size_t off) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(d.data() + off);
        return swap ? (uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3])
                    : (uint32_t{p[3]} << 24 | uint32_t{p[2]} << 16 | uint32_t{p[1]} << 8 | p[0]);
    }
    [[nodiscard]] uint16_t u16(std::size_t off) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(d.data() + off);
        return static_cast<uint16_t>(swap ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]));
    }
};

constexpr uint32_t linktype_socketcan = 227;

// struct can_frame / canfd_frame; can_id in network byte order (LINKTYPE_CAN_SOCKETCAN).
bool parse_socketcan(std::string_view pkt, BusMessage& m)
{
    if (pkt.size() < 8)
    {
        return false;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(pkt.data());
    const uint32_t can_id = uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
    const bool fd = pkt.size() == 72;
    m.id = can_id & can_id_mask_extended;
    m.flags |= (can_id & 0x80000000u) != 0 ? bus_flag::extended : 0;
    if ((can_id & 0x20000000u) != 0)
    {
        m.errors = bus_error::generic;
        return true;
    }
    m.flags |= (can_id & 0x40000000u) != 0 ? bus_flag::rtr : 0;
    if (fd)
    {
        m.flags |= bus_flag::fd | ((p[5] & 1) != 0 ? bus_flag::brs : 0);
    }
    const int len = std::min<int>(p[4], fd ? 64 : 8);
    if (len > static_cast<int>(pkt.size()) - 8)
    {
        return false;
    }
    set_length(m, len);
    if (!has_flag(m, bus_flag::rtr))
    {
        std::memcpy(m.data.data(), p + 8, static_cast<std::size_t>(len));
    }
    return true;
}

// header: the 24-byte global header; data: the records after it (a whole file's, or one block of them).
void parse_pcap(std::string_view header, std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    Bytes hdr{header};
    if (!hdr.has(0, 24))
    {
        return;
    }
    const uint32_t magic = hdr.u32(0);
    bool nano = false;
    if (magic == 0xA1B2C3D4 || magic == 0xA1B23C4D)
    {
        nano = magic == 0xA1B23C4D;
    }
    else if (magic == 0xD4C3B2A1 || magic == 0x4D3CB2A1)
    {
        hdr.swap = true;
        nano = magic == 0x4D3CB2A1;
    }
    else
    {
        return;
    }
    if (hdr.u32(20) != linktype_socketcan)
    {
        return;
    }
    const Bytes b{data, hdr.swap};
    const uint16_t channel = channel_index(f, "pcap0");
    std::size_t n = 0;
    for (std::size_t off = 0; b.has(off, 16) && parse_tick(p, n);)
    {
        const uint32_t incl = b.u32(off + 8);
        if (!b.has(off + 16, incl))
        {
            break;
        }
        BusMessage m;
        m.ts_ns = int64_t{b.u32(off)} * 1000000000 + int64_t{b.u32(off + 4)} * (nano ? 1 : 1000);
        if (parse_socketcan(data.substr(off + 16, incl), m))
        {
            m.iface = channel;
            f.frames.push_back(m);
        }
        off += 16 + incl;
    }
}

// Windows SYSTEMTIME (year, month, day of week, day, hour, minute, second, ms) as UTC ns.
int64_t blf_systemtime_ns(const Bytes& b, std::size_t off)
{
    using namespace std::chrono;
    const auto day = year{b.u16(off)} / month{b.u16(off + 2)} / std::chrono::day{b.u16(off + 6)};
    if (!day.ok())
    {
        return 0;
    }
    const auto t = sys_days{day} + hours{b.u16(off + 8)} + minutes{b.u16(off + 10)} + seconds{b.u16(off + 12)}
                   + milliseconds{b.u16(off + 14)};
    return duration_cast<nanoseconds>(t.time_since_epoch()).count();
}

// One BLF object (from its "LOBJ"): CAN, CAN FD and CAN error frames, LIN frames, checksum
// errors and sleep/wake events (layouts as in Vector's binlog.h), everything else skipped.
// ponytail: LIN_RCV_ERROR / LIN_SND_ERROR (slave did not respond) are skipped, add them if
// a capture with them shows up.
void blf_object(std::string_view obj, int64_t start_ns, ReplayFile& f)
{
    const Bytes b{obj};
    const uint16_t header_size = b.u16(4);
    const uint32_t type = b.u32(12);
    if (!b.has(0, 32) || !b.has(0, header_size))
    {
        return;
    }
    // Header v1 and v2 both keep flags at 16 and the timestamp at 24; flag 1 = 10 us units, else ns.
    const uint64_t ts = uint64_t{b.u32(24)} | uint64_t{b.u32(28)} << 32;
    const Bytes body{obj.substr(header_size)};
    BusMessage m;
    m.ts_ns = start_ns + static_cast<int64_t>(ts * (b.u32(16) == 1 ? 10000 : 1));
    uint16_t channel = 0;
    std::size_t data_off = 0;
    int len = 0;
    switch (type)
    {
        case 1:  // CAN_MESSAGE
        case 86: // CAN_MESSAGE2
        {
            if (!body.has(0, 16))
            {
                return;
            }
            channel = body.u16(0);
            const uint8_t flags = static_cast<uint8_t>(body.d[2]);
            m.flags |= (flags & 0x01) != 0 ? bus_flag::tx : 0;
            m.flags |= (flags & 0x80) != 0 ? bus_flag::rtr : 0;
            len = std::min(static_cast<uint8_t>(body.d[3]) & 0x0F, 8);
            m.id = body.u32(4);
            data_off = 8;
            break;
        }
        case 100: // CAN_FD_MESSAGE
        {
            if (!body.has(0, 84))
            {
                return;
            }
            channel = body.u16(0);
            const uint8_t flags = static_cast<uint8_t>(body.d[2]);
            const uint8_t fd_flags = static_cast<uint8_t>(body.d[13]);
            m.flags |= (flags & 0x01) != 0 ? bus_flag::tx : 0;
            m.flags |= (flags & 0x80) != 0 ? bus_flag::rtr : 0;
            m.flags |= (fd_flags & 0x01) != 0 ? bus_flag::fd : 0;
            m.flags |= (fd_flags & 0x02) != 0 ? bus_flag::brs : 0;
            len = std::min<int>(static_cast<uint8_t>(body.d[14]), 64);
            m.id = body.u32(4);
            data_off = 20;
            break;
        }
        case 101: // CAN_FD_MESSAGE_64
        {
            if (!body.has(0, 40))
            {
                return;
            }
            channel = static_cast<uint8_t>(body.d[0]);
            const uint32_t flags = body.u32(12);
            m.flags |= body.d[34] == 1 ? bus_flag::tx : 0;
            m.flags |= (flags & 0x0010) != 0 ? bus_flag::rtr : 0;
            m.flags |= (flags & 0x1000) != 0 ? bus_flag::fd : 0;
            m.flags |= (flags & 0x2000) != 0 ? bus_flag::brs : 0;
            len = std::min<int>(static_cast<uint8_t>(body.d[2]), 64);
            m.id = body.u32(4);
            data_off = 40;
            break;
        }
        case 2:  // CAN_ERROR
        case 73: // CAN_ERROR_EXT
            if (!body.has(0, 2))
            {
                return;
            }
            channel = body.u16(0);
            m.errors = bus_error::generic;
            break;
        case 11: // LIN_MESSAGE
        case 12: // LIN_CRC_ERROR, same layout: channel, id, dlc, data[8], fsm id/state, header/full time, crc, dir
        {
            if (!body.has(0, 20))
            {
                return;
            }
            m.type = BusType::LIN;
            channel = body.u16(0);
            m.id = static_cast<uint8_t>(body.d[2]);
            len = std::min<int>(static_cast<uint8_t>(body.d[3]), 8);
            m.flags |= body.d[18] == 1 ? bus_flag::tx : 0;
            m.errors = type == 12 ? bus_error::lin_checksum_error : 0;
            data_off = 4;
            break;
        }
        case 57: // LIN_MESSAGE2
        case 60: // LIN_CRC_ERROR2: LinDatabyteTimestampEvent (bus event with channel at 12, synch field,
                 // descriptor with id/dlc at 37/38, nine databyte timestamps), then data[8], crc, dir
        {
            if (!body.has(0, 123))
            {
                return;
            }
            m.type = BusType::LIN;
            channel = body.u16(12);
            m.id = static_cast<uint8_t>(body.d[37]);
            len = std::min<int>(static_cast<uint8_t>(body.d[38]), 8);
            m.flags |= body.d[122] == 1 ? bus_flag::tx : 0;
            m.errors = type == 60 ? bus_error::lin_checksum_error : 0;
            data_off = 112;
            break;
        }
        case 20: // LIN_SLEEP: channel, reason, flags (bit 1: the bus is awake after this event)
            if (!body.has(0, 4))
            {
                return;
            }
            m.type = BusType::LIN;
            channel = body.u16(0);
            m.flags |= (body.d[3] & 0x02) != 0 ? bus_flag::lin_wakeup : bus_flag::lin_sleep;
            break;
        default:
            return;
    }
    if (m.type == BusType::LIN)
    {
        m.id &= 0x3F; // the id byte carries the parity bits
    }
    else
    {
        m.flags |= (m.id & 0x80000000u) != 0 ? bus_flag::extended : 0;
        m.id &= can_id_mask_extended;
    }
    if (!has_flag(m, bus_flag::rtr) && len > 0)
    {
        if (!body.has(data_off, static_cast<std::size_t>(len)))
        {
            return;
        }
        std::memcpy(m.data.data(), body.d.data() + data_off, static_cast<std::size_t>(len));
    }
    set_length(m, len); // a LIN checksum error keeps the bytes it was received with, like the drivers
    m.iface = numbered_channel(f, channel);
    f.frames.push_back(m);
}

// The whole objects at the front of s; returns the bytes used (an object cut by the end of a
// container waits for the next one).
std::size_t blf_objects(std::string_view s, int64_t start_ns, ReplayFile& f)
{
    std::size_t pos = 0;
    for (;;)
    {
        pos = s.find("LOBJ", pos);
        if (pos == std::string_view::npos || s.size() - pos < 16)
        {
            return pos == std::string_view::npos ? s.size() : pos;
        }
        const uint32_t size = Bytes{s}.u32(pos + 8);
        if (size < 16)
        {
            pos += 4;
            continue;
        }
        if (s.size() - pos < size)
        {
            return pos;
        }
        blf_object(s.substr(pos, size), start_ns, f);
        pos += size;
    }
}

// Vector BLF: "LOGG" header, then LOBJ objects; LOG_CONTAINER (type 10) objects carry a stream of
// more objects, stored (method 0) or zlib-compressed (2), which may continue in the next container.
// header: the "LOGG" header; data: the top-level objects after it (a whole file's, or one block
// of them: a cut between top-level objects keeps every container, and so its stream, whole).
void parse_blf(std::string_view header, std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    const Bytes hdr{header};
    if (!hdr.has(0, 72) || !header.starts_with("LOGG"))
    {
        return;
    }
    const int64_t start_ns = blf_systemtime_ns(hdr, 40);
    const Bytes b{data};
    std::string pending; // container bytes not parsed yet
    std::string unpacked;
    bool head_seen = false; // the stream's first "LOBJ" found: what came before it is blf_head
    std::size_t n = 0;
    for (std::size_t off = 0; b.has(off, 16) && parse_tick(p, n);)
    {
        if (data.substr(off, 4) != "LOBJ")
        {
            const auto next = data.find("LOBJ", off + 1);
            if (next == std::string_view::npos)
            {
                break;
            }
            off = next;
            continue;
        }
        const uint32_t size = b.u32(off + 8);
        if (size < 16 || !b.has(off, size))
        {
            break;
        }
        const std::string_view obj = data.substr(off, size);
        if (b.u32(off + 12) == 10 && size >= 32) // LOG_CONTAINER
        {
            const uint16_t method = b.u16(off + 16);
            const std::string_view payload = obj.substr(32);
            if (method == 0)
            {
                pending.append(payload);
            }
            else if (method == 2)
            {
                uLongf out_size = b.u32(off + 24);
                unpacked.resize(out_size);
                if (uncompress(reinterpret_cast<Bytef*>(unpacked.data()), &out_size,
                               reinterpret_cast<const Bytef*>(payload.data()), static_cast<uLong>(payload.size())) == Z_OK)
                {
                    pending.append(unpacked.data(), out_size);
                }
            }
            if (!head_seen)
            {
                // A block cut between containers starts inside the object the previous block's
                // stream ended with: its tail here, its head in blf_tail there (the frame cache
                // build parses the two together).
                const auto first = pending.find("LOBJ");
                head_seen = first != std::string::npos;
                if (head_seen)
                {
                    f.blf_head.assign(pending, 0, first);
                }
            }
            if (head_seen)
            {
                pending.erase(0, blf_objects(pending, start_ns, f));
            }
        }
        else
        {
            blf_objects(obj, start_ns, f);
        }
        off += size + size % 4;
    }
    f.blf_tail = std::move(pending);
}

// --- ASAM MDF4 ---------------------------------------------------------------------------

// One MDF4 block: "##XX" id, length, links, then its data section.
struct MdfBlock
{
    std::string_view id;
    std::vector<uint64_t> links;
    Bytes data{};
};

std::optional<MdfBlock> mdf_block(const Bytes& b, uint64_t off)
{
    if (off == 0 || !b.has(off, 24) || b.d.substr(off, 2) != "##")
    {
        return std::nullopt;
    }
    const uint64_t length = b.u32(off + 8) | uint64_t{b.u32(off + 12)} << 32;
    const uint64_t count = b.u32(off + 16) | uint64_t{b.u32(off + 20)} << 32;
    if (length < 24 + count * 8 || !b.has(off, length))
    {
        return std::nullopt;
    }
    MdfBlock blk{.id = b.d.substr(off + 2, 2)};
    for (uint64_t i = 0; i < count; ++i)
    {
        const std::size_t at = off + 24 + i * 8;
        blk.links.push_back(b.u32(at) | uint64_t{b.u32(at + 4)} << 32);
    }
    blk.data = Bytes{b.d.substr(off + 24 + count * 8, length - 24 - count * 8)};
    return blk;
}

uint64_t mdf_link(const MdfBlock& blk, std::size_t i) { return i < blk.links.size() ? blk.links[i] : 0; }

uint64_t mdf_u64(const Bytes& b, std::size_t off) { return b.has(off, 8) ? b.u32(off) | uint64_t{b.u32(off + 4)} << 32 : 0; }

std::string mdf_text(const Bytes& b, uint64_t off)
{
    const auto blk = mdf_block(b, off);
    return blk ? std::string(blk->data.d.substr(0, blk->data.d.find('\0'))) : std::string();
}

// The records of a data group (or the signal data of a VLSD channel): DT/SD, DZ (deflate,
// optionally transposed), DL lists and HL headers, concatenated.
// ponytail: the whole group in RAM, stream DL entries if huge MF4 files show up.
void mdf_data(const Bytes& b, uint64_t off, std::string& out, int depth = 0)
{
    for (int guard = 0; off != 0 && depth < 8 && guard < 1'000'000; ++guard)
    {
        const auto blk = mdf_block(b, off);
        if (!blk)
        {
            return;
        }
        if (blk->id == "DT" || blk->id == "DV" || blk->id == "SD")
        {
            out.append(blk->data.d);
            return;
        }
        if (blk->id == "DZ" && blk->data.has(0, 24))
        {
            const uint8_t zip = static_cast<uint8_t>(blk->data.d[2]);
            const uint32_t columns = blk->data.u32(4);
            uLongf size = mdf_u64(blk->data, 8);
            const std::string_view packed = blk->data.d.substr(24);
            std::string raw(size, '\0');
            if (uncompress(reinterpret_cast<Bytef*>(raw.data()), &size, reinterpret_cast<const Bytef*>(packed.data()),
                           static_cast<uLong>(packed.size())) != Z_OK)
            {
                return;
            }
            raw.resize(size);
            if (zip == 1 && columns > 0) // transposed: rows were stored column by column
            {
                const std::size_t rows = raw.size() / columns;
                std::string t(raw);
                for (std::size_t r = 0; r < rows; ++r)
                {
                    for (std::size_t c = 0; c < columns; ++c)
                    {
                        t[r * columns + c] = raw[c * rows + r];
                    }
                }
                raw = std::move(t);
            }
            out.append(raw);
            return;
        }
        if (blk->id == "HL")
        {
            off = mdf_link(*blk, 0);
            continue;
        }
        if (blk->id == "DL")
        {
            for (std::size_t i = 1; i < blk->links.size(); ++i)
            {
                mdf_data(b, blk->links[i], out, depth + 1);
            }
            off = mdf_link(*blk, 0);
            continue;
        }
        return;
    }
}

// A fixed-length channel of a record: byte/bit position and size.
struct MdfField
{
    bool present = false;
    uint32_t byte = 0;
    uint8_t bit = 0;
    uint32_t bits = 0;
    uint8_t type = 0; // cn_data_type: 0 = UINT LE, 4 = REAL LE, 10 = bytes
};

uint64_t mdf_uint(std::string_view rec, const MdfField& f)
{
    uint64_t v = 0;
    const std::size_t n = std::min<std::size_t>((f.bit + f.bits + 7) / 8, 8);
    if (!f.present || f.byte + n > rec.size())
    {
        return 0;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        v |= uint64_t{static_cast<uint8_t>(rec[f.byte + i])} << (8 * i);
    }
    v >>= f.bit;
    return f.bits >= 64 ? v : v & ((uint64_t{1} << f.bits) - 1);
}

double mdf_time(std::string_view rec, const MdfField& f)
{
    if (!f.present || f.byte + f.bits / 8 > rec.size())
    {
        return 0.0;
    }
    if (f.type == 4 || f.type == 5)
    {
        if (f.bits == 32)
        {
            float v;
            std::memcpy(&v, rec.data() + f.byte, 4);
            return v;
        }
        double v;
        std::memcpy(&v, rec.data() + f.byte, 8);
        return v;
    }
    return static_cast<double>(mdf_uint(rec, f));
}

// The frame channels of one channel group: the ASAM bus logging layout (CAN_DataFrame.ID, ...;
// also CAN_ErrorFrame / CAN_RemoteFrame groups, and LIN_Frame / LIN_ChecksumError), or Kraken
// Explorer's own export (CAN_ID as a SocketCAN canid_t, DLC = length, Dir, DataBytes).
struct MdfCanGroup
{
    uint64_t record_id = 0;
    uint32_t size = 0; // data + invalidation bytes
    bool vlsd = false;
    bool error = false;
    bool remote = false;
    bool canid_t = false; // our export
    bool lin = false;
    bool lin_error = false;
    bool data_vlsd = false; // DataBytes is a VLSD channel: the record holds an offset into sd
    std::string sd;         // its signal data, u32 length + bytes per sample
    MdfField time, channel, id, ide, dlc, length, data, dir, edl, brs;
};

void mdf_channels(const Bytes& b, uint64_t off, MdfCanGroup& g, int depth = 0)
{
    for (int guard = 0; off != 0 && depth < 4 && guard < 10000; ++guard)
    {
        const auto cn = mdf_block(b, off);
        if (!cn || cn->id != "CN" || !cn->data.has(0, 16))
        {
            return;
        }
        const std::string name = mdf_text(b, mdf_link(*cn, 2));
        const MdfField f{.present = true, .byte = cn->data.u32(4), .bit = static_cast<uint8_t>(cn->data.d[3]),
                         .bits = cn->data.u32(8), .type = static_cast<uint8_t>(cn->data.d[2])};
        const uint8_t cn_type = static_cast<uint8_t>(cn->data.d[0]);
        const auto dot = name.rfind('.');
        const std::string_view leaf = dot == std::string::npos ? std::string_view(name) : std::string_view(name).substr(dot + 1);
        g.error = g.error || name.starts_with("CAN_ErrorFrame");
        g.remote = g.remote || name.starts_with("CAN_RemoteFrame");
        g.lin = g.lin || name.starts_with("LIN_");
        g.lin_error = g.lin_error || name.starts_with("LIN_ChecksumError");
        if (cn_type == 2 || cn_type == 3)
        {
            g.time = f;
        }
        else if (leaf == "BusChannel") g.channel = f;
        else if (leaf == "ID") g.id = f;
        else if (leaf == "CAN_ID") { g.id = f; g.canid_t = true; }
        else if (leaf == "IDE") g.ide = f;
        else if (leaf == "DLC") g.dlc = f;
        else if (leaf == "DataLength") g.length = f;
        else if (leaf == "DataBytes" && (cn_type == 0 || cn_type == 1))
        {
            g.data = f;
            g.data_vlsd = cn_type == 1;
            if (g.data_vlsd)
            {
                mdf_data(b, mdf_link(*cn, 5), g.sd); // cn_data: SD block(s), or DZ/DL of them
            }
        }
        else if (leaf == "Dir") g.dir = f;
        else if (leaf == "EDL") g.edl = f;
        else if (leaf == "BRS") g.brs = f;
        mdf_channels(b, mdf_link(*cn, 1), g, depth + 1); // composition: CAN_DataFrame's members
        off = mdf_link(*cn, 0);
    }
}

void mdf_frame(std::string_view rec, const MdfCanGroup& g, int64_t start_ns, ReplayFile& f)
{
    BusMessage m;
    m.ts_ns = start_ns + static_cast<int64_t>(std::llround(mdf_time(rec, g.time) * 1e9));
    uint64_t id = mdf_uint(rec, g.id);
    int len = 0;
    if (g.lin)
    {
        m.type = BusType::LIN;
        m.errors = g.lin_error ? bus_error::lin_checksum_error : 0;
        len = g.length.present ? static_cast<int>(mdf_uint(rec, g.length)) : std::min<int>(static_cast<int>(mdf_uint(rec, g.dlc)), 8);
    }
    else if (g.canid_t)
    {
        m.flags |= (id & 0x80000000u) != 0 ? bus_flag::extended : 0;
        m.flags |= (id & 0x40000000u) != 0 ? bus_flag::rtr : 0;
        m.errors = (id & 0x20000000u) != 0 ? bus_error::generic : 0;
        len = static_cast<int>(mdf_uint(rec, g.dlc));
        m.flags |= len > 8 ? bus_flag::fd : 0;
    }
    else
    {
        m.flags |= mdf_uint(rec, g.ide) != 0 || (id & 0x80000000u) != 0 ? bus_flag::extended : 0;
        m.flags |= g.remote ? bus_flag::rtr : 0;
        m.flags |= mdf_uint(rec, g.edl) != 0 ? bus_flag::fd : 0;
        m.flags |= mdf_uint(rec, g.brs) != 0 ? bus_flag::brs : 0;
        m.errors = g.error ? bus_error::generic : 0;
        const auto dlc = static_cast<int>(mdf_uint(rec, g.dlc) & 0x0F);
        len = g.length.present && !g.remote ? static_cast<int>(mdf_uint(rec, g.length))
              : has_flag(m, bus_flag::fd) ? bus_dlc_lengths[static_cast<std::size_t>(dlc)] : std::min(dlc, 8);
    }
    m.id = static_cast<uint32_t>(id) & (g.lin ? 0x3Fu : can_id_mask_extended);
    m.flags |= mdf_uint(rec, g.dir) == 1 ? bus_flag::tx : 0;
    len = std::clamp(len, 0, 64);
    if (m.errors == 0 || g.lin) // a LIN checksum error keeps the bytes it was received with
    {
        set_length(m, len);
        if (!has_flag(m, bus_flag::rtr) && g.data.present && g.data.byte <= rec.size())
        {
            std::string_view bytes = rec.substr(g.data.byte);
            if (g.data_vlsd)
            {
                const Bytes sd{g.sd};
                const uint64_t at = mdf_uint(rec, g.data);
                bytes = sd.has(at, 4) ? sd.d.substr(at + 4, sd.u32(at)) : std::string_view();
            }
            std::copy_n(bytes.data(), std::min(bytes.size(), static_cast<std::size_t>(len)), m.data.begin());
        }
    }
    const auto ch = g.channel.present ? mdf_uint(rec, g.channel) : 0;
    m.iface = numbered_channel(f, static_cast<unsigned>(ch));
    f.frames.push_back(m);
}

// ASAM MDF 4.x CAN bus logging: every channel group with an ID channel is read as CAN frames,
// sorted or unsorted (record ids) data groups, DT/DZ/DL/HL data. Frames come out per group,
// the frame cache sorts them by time.
void parse_mf4(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    const Bytes b{data};
    if (!b.has(0, 64) || !data.starts_with("MDF     "))
    {
        return;
    }
    const auto hd = mdf_block(b, 64);
    if (!hd || hd->id != "HD")
    {
        return;
    }
    // hd_time_flags bit 0: the start is local time; bit 1: tz/dst offsets (minutes) are valid.
    // Local time without offsets (python-can) is taken in this machine's zone, as asammdf does.
    auto start_ns = static_cast<int64_t>(mdf_u64(hd->data, 0));
    const uint8_t time_flags = hd->data.has(12, 1) ? static_cast<uint8_t>(hd->data.d[12]) : 0;
    if ((time_flags & 3) == 3)
    {
        start_ns -= (int64_t{static_cast<int16_t>(hd->data.u16(8))} + static_cast<int16_t>(hd->data.u16(10))) * 60'000'000'000;
    }
    else if ((time_flags & 1) != 0)
    {
        try
        {
            using namespace std::chrono;
            const local_time<nanoseconds> local{nanoseconds{start_ns}};
            start_ns = current_zone()->to_sys(local, choose::earliest).time_since_epoch().count();
        }
        catch (const std::exception&)
        {
            // no time zone database: keep it as UTC
        }
    }
    std::string records;
    std::size_t n = 0;
    for (uint64_t dg_off = mdf_link(*hd, 0); dg_off != 0;)
    {
        const auto dg = mdf_block(b, dg_off);
        if (!dg || dg->id != "DG" || !parse_tick(p, n))
        {
            break;
        }
        const uint8_t id_size = dg->data.has(0, 1) ? static_cast<uint8_t>(dg->data.d[0]) : 0;
        std::vector<MdfCanGroup> groups;
        for (uint64_t cg_off = mdf_link(*dg, 1); cg_off != 0;)
        {
            const auto cg = mdf_block(b, cg_off);
            if (!cg || cg->id != "CG" || !cg->data.has(0, 32))
            {
                break;
            }
            MdfCanGroup g{.record_id = mdf_u64(cg->data, 0), .size = cg->data.u32(24) + cg->data.u32(28),
                          .vlsd = (cg->data.u16(16) & 1) != 0};
            mdf_channels(b, mdf_link(*cg, 1), g);
            groups.push_back(std::move(g));
            cg_off = mdf_link(*cg, 0);
        }
        records.clear();
        mdf_data(b, mdf_link(*dg, 2), records);
        for (std::size_t pos = 0; pos < records.size();)
        {
            uint64_t rid = 0;
            if (id_size > 0)
            {
                if (pos + id_size > records.size())
                {
                    break;
                }
                for (std::size_t i = 0; i < std::min<std::size_t>(id_size, 8); ++i)
                {
                    rid |= uint64_t{static_cast<uint8_t>(records[pos + i])} << (8 * i);
                }
                pos += id_size;
            }
            const auto g = std::ranges::find_if(groups, [&](const MdfCanGroup& x) { return id_size == 0 || x.record_id == rid; });
            if (g == groups.end())
            {
                break; // unknown record id: the rest cannot be framed
            }
            if (g->vlsd) // variable length record: u32 length + bytes, skipped
            {
                if (pos + 4 > records.size())
                {
                    break;
                }
                pos += 4 + Bytes{records}.u32(pos);
                continue;
            }
            if (g->size == 0 || pos + g->size > records.size())
            {
                break;
            }
            if (g->id.present)
            {
                mdf_frame(std::string_view(records).substr(pos, g->size), *g, start_ns, f);
            }
            pos += g->size;
        }
        dg_off = mdf_link(*dg, 0);
    }
}

void parse_pcapng(std::string_view data, ReplayFile& f, const ReplayParseProgress& p)
{
    struct Idb
    {
        uint16_t channel = 0;
        uint16_t link = 0;
        uint8_t tsresol = 6;
    };
    std::vector<Idb> idbs;
    Bytes b{data};
    std::size_t n = 0;
    for (std::size_t off = 0; b.has(off, 12) && parse_tick(p, n);)
    {
        if (b.u32(off) == 0x0A0D0D0A) // SHB: its byte-order magic decides the endianness
        {
            b.swap = false;
            if (b.u32(off + 8) != 0x1A2B3C4D)
            {
                b.swap = true;
                if (b.u32(off + 8) != 0x1A2B3C4D)
                {
                    return;
                }
            }
            idbs.clear();
        }
        const uint32_t type = b.u32(off);
        const uint32_t total = b.u32(off + 4);
        if (total < 12 || total % 4 != 0 || !b.has(off, total))
        {
            return;
        }
        if (type == 1 && total >= 20) // IDB: link type, options if_name (2), if_tsresol (9)
        {
            Idb idb{.link = b.u16(off + 8)};
            std::string name = std::format("if{}", idbs.size());
            for (std::size_t o = off + 16; o + 4 <= off + total - 4;)
            {
                const uint16_t code = b.u16(o);
                const uint16_t len = b.u16(o + 2);
                if (code == 0 || o + 4 + len > off + total - 4)
                {
                    break;
                }
                if (code == 2 && len > 0)
                {
                    name = std::string(data.substr(o + 4, len));
                    name.erase(std::ranges::find(name, '\0'), name.end());
                }
                else if (code == 9 && len == 1)
                {
                    idb.tsresol = static_cast<uint8_t>(data[o + 4]);
                }
                o += 4 + ((len + 3u) & ~3u);
            }
            idb.channel = channel_index(f, name);
            idbs.push_back(idb);
        }
        else if (type == 6 && total >= 32) // EPB
        {
            const uint32_t if_id = b.u32(off + 8);
            const uint32_t incl = b.u32(off + 20);
            if (if_id < idbs.size() && idbs[if_id].link == linktype_socketcan && 28 + uint64_t{incl} <= total)
            {
                const Idb& idb = idbs[if_id];
                const uint64_t raw = uint64_t{b.u32(off + 12)} << 32 | b.u32(off + 16);
                BusMessage m;
                if ((idb.tsresol & 0x80) == 0 && idb.tsresol <= 9)
                {
                    uint64_t scale = 1;
                    for (int i = idb.tsresol; i < 9; ++i)
                    {
                        scale *= 10;
                    }
                    m.ts_ns = static_cast<int64_t>(raw * scale);
                }
                else // ponytail: power-of-2 or sub-ns resolutions, via double; exact integer math if a capture uses them
                {
                    const double unit = (idb.tsresol & 0x80) != 0 ? 1.0 / static_cast<double>(1ull << (idb.tsresol & 0x7F))
                                                                   : 1.0 / std::pow(10.0, idb.tsresol);
                    m.ts_ns = static_cast<int64_t>(static_cast<double>(raw) * unit * 1e9);
                }
                if (parse_socketcan(data.substr(off + 28, incl), m))
                {
                    m.iface = idb.channel;
                    f.frames.push_back(m);
                }
            }
        }
        off += total;
    }
}

// ---------------------------------------------------------------- UI helpers

const ReplayIdRow* find_row(const std::vector<ReplayIdRow>& rows, const BusMessage& m)
{
    const uint32_t id = is_error_frame(m) ? replay_error_id : m.id;
    const auto it = std::ranges::lower_bound(rows, std::pair{m.iface, id}, {},
                                             [](const ReplayIdRow& r) { return std::pair{r.channel, r.id}; });
    return it == rows.end() || it->channel != m.iface || it->id != id ? nullptr : &*it;
}

bool row_enabled(const std::vector<ReplayIdRow>& rows, const BusMessage& m)
{
    const ReplayIdRow* row = find_row(rows, m);
    return row != nullptr && (has_flag(m, bus_flag::tx) ? row->tx_on : row->rx_on);
}

void draw_filter_table(App& app, Replay& r)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##filter", 7, flags))
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Interface / CAN ID", ImGuiTableColumnFlags_WidthFixed, 150.0f * px);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("RX", ImGuiTableColumnFlags_WidthFixed, 30.0f * px);
    ImGui::TableSetupColumn("TX", ImGuiTableColumnFlags_WidthFixed, 30.0f * px);
    ImGui::TableSetupColumn("Break", ImGuiTableColumnFlags_WidthFixed, 45.0f * px);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f * px);
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, 150.0f * px);
    ImGui::TableHeadersRow();

    for (uint16_t ch = 0; ch < r.data.channels.size(); ++ch)
    {
        ImGui::PushID(ch);
        const bool lin = r.data.channel_lin[ch] != 0;
        const auto first = std::ranges::find_if(r.data.rows, [&](const ReplayIdRow& row) { return row.channel == ch; });
        const auto last = std::find_if(first, r.data.rows.end(), [&](const ReplayIdRow& row) { return row.channel != ch; });
        int total = 0;
        bool all_on = true;
        for (auto it = first; it != last; ++it)
        {
            total += it->count;
            all_on = all_on && (it->rx_on || !it->has_rx) && (it->tx_on || !it->has_tx);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool expanded = ImGui::TreeNodeEx("##ch", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns
                                                                | ImGuiTreeNodeFlags_AllowOverlap); // checkbox + Output combo get their clicks
        ImGui::SameLine();
        bool on = all_on;
        if (ImGui::Checkbox(std::format("{} ({})", r.data.channels[ch], lin ? "LIN" : "CAN").c_str(), &on))
        {
            for (auto it = first; it != last; ++it)
            {
                it->rx_on = on && it->has_rx;
                it->tx_on = on && it->has_tx;
            }
        }
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%d", total);
        ImGui::TableSetColumnIndex(6);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const int target = r.mapping[ch];
        const std::string preview = target >= 0 && static_cast<std::size_t>(target) < app.ifaces.size()
            ? app.ifaces[static_cast<std::size_t>(target)].info.name : "Trace only";
        ImGui::BeginDisabled(lin);
        if (ImGui::BeginCombo("##out", preview.c_str()))
        {
            if (ImGui::Selectable("Trace only", target < 0))
            {
                r.mapping[ch] = replay_trace_only;
            }
            for (const Iface& iface : app.ifaces)
            {
                if (iface.info.bus_type != BusType::LIN && ImGui::Selectable(iface.info.name.c_str(), target == iface.index))
                {
                    r.mapping[ch] = iface.index;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (lin)
        {
            ImGui::SetItemTooltip("LIN frames can only be replayed to the trace");
        }

        if (expanded)
        {
            for (auto it = first; it != last; ++it)
            {
                ReplayIdRow& row = *it;
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent();
                if (row.id == replay_error_id)
                {
                    ImGui::TextUnformatted("ERROR");
                }
                else
                {
                    ImGui::Text("0x%X", row.id);
                }
                ImGui::Unindent();
                ImGui::TableNextColumn();
                if (row.id != replay_error_id && !lin)
                {
                    // Looked up in the DBC of the mapped interface's network (trace only: any network).
                    const BusMessage probe{.id = row.id,
                                           .flags = row.extended ? bus_flag::extended : uint16_t{0},
                                           .iface = static_cast<uint16_t>(target >= 0 ? target : UINT16_MAX)};
                    if (const CanDbMessage* db = setup_find_can_message(app.setup, probe))
                    {
                        ImGui::TextUnformatted(db->name.c_str());
                    }
                }
                ImGui::TableNextColumn();
                if (row.has_rx)
                {
                    ImGui::Checkbox("##rx", &row.rx_on);
                }
                ImGui::TableNextColumn();
                if (row.has_tx)
                {
                    ImGui::Checkbox("##tx", &row.tx_on);
                }
                ImGui::TableNextColumn();
                ImGui::Checkbox("##brk", &row.brk);
                ImGui::SetItemTooltip("Breakpoint: pause the replay before every frame with this id");
                ImGui::TableNextColumn();
                ImGui::Text("%d", row.count);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace

std::size_t replay_header_size(std::string_view data, TraceFileFormat format)
{
    if (format == TraceFileFormat::Pcap)
    {
        return std::min<std::size_t>(24, data.size());
    }
    if (format == TraceFileFormat::Blf && data.size() >= 72 && data.starts_with("LOGG"))
    {
        return std::min<std::size_t>(Bytes{data}.u32(4), data.size());
    }
    if (format == TraceFileFormat::VectorAsc)
    {
        // Through the "date" line (the time the frame times count from), when the header has one.
        const std::string_view head = data.substr(0, std::min<std::size_t>(data.size(), 4096));
        const auto at = head.starts_with("date ") ? 0 : head.find("\ndate ");
        const auto end = at == std::string_view::npos ? at : head.find('\n', at + 1);
        return end == std::string_view::npos ? 0 : end + 1;
    }
    return 0;
}

void replay_parse_into(std::string_view header, std::string_view data, TraceFileFormat format, ReplayFile& f,
                       const ReplayParseProgress& progress)
{
    f.frames.clear(); // keeps its capacity: the frame cache build parses block after block into the same files
    f.channels.clear();
    f.blf_head.clear();
    f.blf_tail.clear();
    if (format == TraceFileFormat::CanDump || format == TraceFileFormat::VectorAsc || format == TraceFileFormat::Trc)
    {
        // >= ~30 bytes a line: one reservation instead of regrowing (and copying) the vector. Text
        // only: a binary file is one block, its reservation could reach the whole file.
        f.frames.reserve(data.size() / 30);
    }
    switch (format)
    {
        case TraceFileFormat::CanDump:   parse_candump(data, f, progress); break;
        case TraceFileFormat::VectorAsc: parse_asc(header, data, f, progress); break;
        case TraceFileFormat::Pcap:      parse_pcap(header, data, f, progress); break;
        case TraceFileFormat::PcapNg:    parse_pcapng(data, f, progress); break;
        case TraceFileFormat::Trc:       parse_trc(data, f, progress); break;
        case TraceFileFormat::Blf:       parse_blf(header, data, f, progress); break;
        case TraceFileFormat::VectorMdf: parse_mf4(data, f, progress); break;
    }
}

void replay_parse_into(std::string_view data, TraceFileFormat format, ReplayFile& f, const ReplayParseProgress& progress)
{
    const std::size_t header = replay_header_size(data, format);
    replay_parse_into(data.substr(0, header), data.substr(header), format, f, progress);
}

ReplayFile replay_parse(std::string_view data, TraceFileFormat format, const ReplayParseProgress& progress)
{
    ReplayFile f;
    replay_parse_into(data, format, f, progress);
    return f;
}

std::vector<ReplayIdRow> replay_id_rows(const ReplayFile& file)
{
    std::vector<ReplayIdRow> rows;
    for (const BusMessage& m : file.frames)
    {
        const uint32_t id = is_error_frame(m) ? replay_error_id : m.id;
        auto it = std::ranges::find_if(rows, [&](const ReplayIdRow& r) { return r.channel == m.iface && r.id == id; });
        if (it == rows.end())
        {
            it = rows.insert(rows.end(), {.channel = m.iface, .id = id, .extended = has_flag(m, bus_flag::extended)});
        }
        ++it->count;
        (has_flag(m, bus_flag::tx) ? it->has_tx : it->has_rx) = true;
    }
    for (ReplayIdRow& r : rows)
    {
        r.rx_on = r.has_rx;
        r.tx_on = r.has_tx;
    }
    std::ranges::sort(rows, {}, [](const ReplayIdRow& r) { return std::pair{r.channel, r.id}; });
    return rows;
}

bool replay_step(const BusMessage& m, int64_t t0, const std::vector<ReplayIdRow>& rows, const std::vector<int>& mapping,
                 ReplayStep& step)
{
    if (!row_enabled(rows, m))
    {
        return false;
    }
    const int mapped = m.iface < mapping.size() ? mapping[m.iface] : replay_trace_only;
    const bool sendable = m.type == BusType::CAN && !is_error_frame(m);
    step = {.msg = m, .at_ns = std::max<int64_t>(0, m.ts_ns - t0), .target = sendable ? mapped : replay_trace_only};
    return true;
}


std::span<const FrameCacheRec> replay_range(std::span<const FrameCacheRec> frames, std::string_view from, std::string_view to)
{
    if (frames.empty())
    {
        return frames;
    }
    const int64_t t0 = frames.front().ts_ns;
    const auto at = [t0](std::string_view text) -> std::optional<int64_t>
    {
        const auto s = parse_duration(text);
        return s ? std::optional(t0 + static_cast<int64_t>(*s * 1e9)) : std::nullopt;
    };
    const auto lo = at(from) ? std::ranges::lower_bound(frames, *at(from), {}, &FrameCacheRec::ts_ns) : frames.begin();
    const auto hi = at(to) ? std::ranges::upper_bound(frames, *at(to), {}, &FrameCacheRec::ts_ns) : frames.end();
    return lo < hi ? std::span(lo, hi) : std::span<const FrameCacheRec>{};
}

std::span<const FrameCacheRec> replay_frames(const ReplayLoaded& d)
{
    return d.cache != nullptr ? d.cache->recs : std::span<const FrameCacheRec>{};
}

void replay_run(std::stop_token stop, Replay& r, std::deque<Iface>& ifaces, Tasks& tasks, double speed, bool loop)
{
    using namespace std::chrono;
    std::mutex mutex;
    std::condition_variable_any cv; // only to sleep until the deadline or the stop request
    bool any = false; // a frame after the first one is enabled: looping makes sense
    // Trace-only frames go to the main thread in batches when not paced (speed 0): one task per
    // frame would flood it with millions of tasks.
    std::vector<BusMessage> batch;
    const auto flush = [&]
    {
        if (!batch.empty())
        {
            tasks_post(tasks, [b = std::move(batch)](App& app) { trace_append(app.trace, b); });
            batch.clear();
        }
    };
    do
    {
        const std::span<const FrameCacheRec> frames = r.play_frames;
        const std::span<const FrameCachePayload> overflow = // play_frames are the cache's records, it holds their payloads
            r.play_cache != nullptr ? r.play_cache->overflow : std::span<const FrameCachePayload>{};
        auto start = steady_clock::now();
        ReplayStep step;
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            if (!replay_step(frame_cache_decode(frames[i], overflow), frames.front().ts_ns, r.play_rows, r.play_mapping, step))
            {
                r.position = i + 1;
                continue;
            }
            any = any || step.at_ns > 0;
            const auto offset = duration_cast<steady_clock::duration>(duration<double, std::nano>(speed > 0.0 ? static_cast<double>(step.at_ns) / speed : 0.0));
            const ReplayIdRow* row = find_row(r.play_rows, step.msg);
            if ((row != nullptr && row->brk) || std::ranges::binary_search(r.play_breaks, i))
            {
                int playing = replay_playing; // a step onto the frame pauses after it anyway
                r.hold.compare_exchange_strong(playing, replay_paused);
            }
            bool held = false;
            const auto hold_here = [&]
            {
                if (r.hold != replay_paused)
                {
                    return;
                }
                flush(); // the trace shows everything before the pause
                if (tasks.wake != nullptr)
                {
                    tasks.wake();
                }
                // replay_stop requests the stop before it releases the hold: no wait is entered after it.
                while (r.hold == replay_paused && !stop.stop_requested())
                {
                    r.hold.wait(replay_paused);
                }
                held = true;
            };
            hold_here();
            if (speed > 0.0 && !held && r.hold == replay_playing)
            {
                std::unique_lock lock(mutex);
                cv.wait_until(lock, stop, start + offset, [] { return false; });
                lock.unlock();
                hold_here(); // paused during the wait: the frame is due, it goes out on resume
            }
            if (stop.stop_requested())
            {
                r.running = false;
                return;
            }
            if (held || r.hold == replay_stepping)
            {
                start = steady_clock::now() - offset; // this frame is "now": the rest keeps its spacing
            }
            int stepping = replay_stepping; // one frame per step: paused again before the next
            r.hold.compare_exchange_strong(stepping, replay_paused);
            BusMessage msg = step.msg;
            msg.flags &= static_cast<uint16_t>(~bus_flag::tx);
            if (step.target >= 0 && static_cast<std::size_t>(step.target) < ifaces.size())
            {
                msg.iface = static_cast<uint16_t>(step.target);
                if (iface_send(ifaces[static_cast<std::size_t>(step.target)], msg))
                {
                    r.position = i + 1; // the TX echo puts it in the trace
                    continue;
                }
            }
            // Trace only, or the interface is closed: show it in the trace as the old window did,
            // at the time it was played; unpaced, the file's own time (wall time would squash it).
            if (speed > 0.0)
            {
                msg.ts_ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
            }
            batch.push_back(msg);
            if (speed > 0.0 || batch.size() >= 4096)
            {
                flush();
            }
            r.position = i + 1;
        }
        flush();
    } while (loop && any && !stop.stop_requested());
    flush();
    r.running = false;
}

namespace
{

// Loader thread body: opens the file's frame cache, building it first when the file is new or
// changed (the slow part, once per file), and hands the result over through done. A stop request
// ends it without a result (only after replay_load_cancel dropped the future).
void replay_load_run(std::stop_token stop, std::promise<ReplayLoaded> done, Replay& r, std::string path, void (*wake)())
{
    ReplayLoaded out;
    const std::filesystem::path cache_path = frame_cache_path(path);
    auto cache = frame_cache_open(path, cache_path);
    r.load_changed = !cache && cache.error() == "Stale cache";
    if (!cache)
    {
        cache = frame_cache_build(path, cache_path, trace_format_from_path(path).value_or(TraceFileFormat::VectorAsc),
                                  {.stop = stop, .fraction = &r.load_fraction, .frames = &r.load_frames});
        if (stop.stop_requested())
        {
            if (cache)
            {
                frame_cache_close(*cache);
            }
            return;
        }
        out.parsed = true;
    }
    if (!cache)
    {
        out.info = "Error: " + cache.error();
    }
    else
    {
        out.cache = std::shared_ptr<const FrameCache>(new FrameCache(*cache), [](const FrameCache* c)
        {
            frame_cache_close(*const_cast<FrameCache*>(c));
            delete c;
        });
        const std::span<const FrameCacheRec> frames = out.cache->recs;
        out.channels = out.cache->channels;
        out.channel_lin.assign(out.channels.size(), 0);
        for (const FrameCacheRow& row : out.cache->rows)
        {
            out.rows.push_back({.channel = row.channel, .id = row.id, .extended = row.extended != 0,
                                .count = static_cast<int>(std::min<uint64_t>(row.count, INT32_MAX)),
                                .has_rx = (row.dirs & 1) != 0, .has_tx = (row.dirs & 2) != 0,
                                .rx_on = (row.dirs & 1) != 0, .tx_on = (row.dirs & 2) != 0});
            if (frames[out.cache->row_frames[row.first]].type == BusType::LIN)
            {
                out.channel_lin[row.channel] = 1;
            }
        }
        const double duration = static_cast<double>(frames.back().ts_ns - frames.front().ts_ns) / 1e9;
        std::string count;
        append_grouped(count, frames.size());
        out.info = std::format("{} messages  ·  {}{}", count, format_duration(duration), duration < 60.0 ? " s" : "");
        out.path = std::move(path);
    }
    r.load_fraction = 1.0f;
    done.set_value(std::move(out));
    if (wake != nullptr)
    {
        wake();
    }
}

} // namespace

void replay_load_cancel(Replay& r)
{
    r.loader = {}; // request_stop + join
    r.loading = {};
}

void replay_load(App& app, Replay& r, const std::string& path)
{
    replay_stop(r);
    replay_load_cancel(r);
    r.data = {};
    r.mapping.clear();
    r.play_frames = {};
    r.play_cache = nullptr;
    r.position = 0;
    r.load_fraction = 0.0f;
    r.load_frames = 0;
    r.load_started = std::chrono::steady_clock::now();
    std::error_code size_ec;
    r.load_bytes = std::filesystem::file_size(path, size_ec);
    r.data.info = std::format("Diving into {}...", std::filesystem::path(path).filename().string());
    std::promise<ReplayLoaded> done;
    r.loading = done.get_future();
    r.loader = std::jthread(replay_load_run, std::move(done), std::ref(r), path, app.tasks.wake);
}

void replay_add_databases(App& app, const Replay& r, const std::vector<std::string>& paths)
{
    // The network of the first mapped interface; trace-only replays (the default, and while the
    // file is still loading) decode from any network, so the first one does.
    int net = -1;
    for (const int target : r.mapping)
    {
        if (target >= 0 && (net = setup_network_of(app.setup, static_cast<uint16_t>(target))) >= 0)
        {
            break;
        }
    }
    if (net < 0)
    {
        if (app.setup.networks.empty())
        {
            app.setup.networks.push_back({.name = "Network 1"});
        }
        net = 0;
    }
    for (const auto& path : paths)
    {
        if (!setup_add_can_db(app.setup.networks[static_cast<std::size_t>(net)], path))
        {
            status_bar_notice(app.status_bar, std::format("Failed to load database {}", path));
        }
    }
    setup_rebuild_cache(app.setup);
}

bool replay_load_poll(App& app, Replay& r)
{
    if (!r.loading.valid() || r.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }
    r.data = r.loading.get();
    r.loader.join();
    if (r.data.cache != nullptr)
    {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.load_started).count();
        r.data.info += std::format("  ·  loaded in {}{}", format_duration(secs, secs < 10.0 ? 2 : 0), secs < 60.0 ? " s" : "");
    }
    if (r.load_changed && r.data.cache != nullptr)
    {
        std::string count;
        append_grouped(count, r.data.cache->recs.size());
        status_bar_notice(app.status_bar, std::format("{} changed on disk: reloaded, {} messages",
                                                      std::filesystem::path(r.data.path).filename().string(), count));
    }
    std::error_code ec;
    r.watched_size = r.data.path.empty() ? 0 : std::filesystem::file_size(r.data.path, ec);
    r.watched_mtime = r.data.path.empty() ? 0 : std::filesystem::last_write_time(r.data.path, ec).time_since_epoch().count();
    if (r.data.cache != nullptr && !app.measuring && app.trace.file.data() != r.data.cache->recs.data())
    {
        // The whole file in the trace, mapped (a measurement owns the trace while it runs).
        trace_open_file(app.trace, r.data.cache->recs, r.data.cache->overflow);
        app.trace_file = r.data.cache;
    }
    // Default output: the interface with the channel's name (e.g. candump "vcan0"), else trace only.
    for (uint16_t ch = 0; ch < r.data.channels.size(); ++ch)
    {
        int target = replay_trace_only;
        for (const Iface& iface : app.ifaces)
        {
            if (iface.info.name == r.data.channels[ch] && iface.info.bus_type == BusType::CAN && r.data.channel_lin[ch] == 0)
            {
                target = iface.index;
            }
        }
        r.mapping.push_back(target);
    }
    if (r.autoplay && app.measuring && !r.running && !replay_frames(r.data).empty())
    {
        replay_start(r, app.ifaces, app.tasks); // --replay FILE --measure: the measurement started first
    }
    return true;
}

void replay_start(Replay& r, std::deque<Iface>& ifaces, Tasks& tasks, int hold)
{
    replay_stop(r);
    const std::span<const FrameCacheRec> all = replay_frames(r.data);
    r.play_frames = replay_range(all, r.range_from, r.range_to);
    r.play_cache = r.data.cache;
    r.play_rows = r.data.rows;
    r.play_mapping = r.mapping;
    r.play_breaks.clear();
    for (const auto part : std::views::split(std::string_view(r.break_at), ','))
    {
        // The first frame at or after the time, as an index into the part being played.
        if (const auto secs = parse_duration(trim(std::string_view(part))); secs && !r.play_frames.empty())
        {
            const int64_t at = all.front().ts_ns + static_cast<int64_t>(*secs * 1e9);
            const auto it = std::ranges::lower_bound(r.play_frames, at, {}, &FrameCacheRec::ts_ns);
            if (it != r.play_frames.end())
            {
                r.play_breaks.push_back(static_cast<std::size_t>(it - r.play_frames.begin()));
            }
        }
    }
    std::ranges::sort(r.play_breaks);
    r.hold = hold;
    r.position = 0;
    r.running = true;
    platform_fine_timers();
    r.player = std::jthread(replay_run, std::ref(r), std::ref(ifaces), std::ref(tasks),
                            r.fast ? 0.0 : std::clamp(static_cast<double>(r.speed), 0.1, 10.0), r.loop);
}

void replay_stop(Replay& r)
{
    r.player.request_stop(); // before the hold is released: a paused player must not pause again
    r.hold = replay_playing;
    r.hold.notify_all();
    r.player = {}; // join
    r.running = false;
}

void replay_pause(Replay& r)
{
    int playing = replay_playing;
    r.hold.compare_exchange_strong(playing, replay_paused);
}

void replay_resume(Replay& r)
{
    r.hold = replay_playing;
    r.hold.notify_all();
}

void replay_single_step(Replay& r)
{
    r.hold = replay_stepping;
    r.hold.notify_all();
}

void replay_watch(App& app, Replay& r)
{
    // The loaded file changed on disk (a logger still writing it, a new export): reload it once
    // a second's check sees it, unless it is playing. Only a content change reloads: the hash runs
    // off the main thread (frame_cache_open keeps the cache of a touched file) and wakes the loop.
    const auto now = std::chrono::steady_clock::now();
    if (!r.data.path.empty() && !r.loader.joinable() && !r.running && now - r.watched >= std::chrono::seconds(1))
    {
        r.watched = now;
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(r.data.path, ec);
        const int64_t mtime = std::filesystem::last_write_time(r.data.path, ec).time_since_epoch().count();
        if (!ec && (size != r.watched_size || mtime != r.watched_mtime) && !r.watch_check.valid())
        {
            r.watched_size = size;
            r.watched_mtime = mtime;
            r.watch_check = std::async(std::launch::async, [path = r.data.path, wake = app.tasks.wake]
            {
                auto c = frame_cache_open(path, frame_cache_path(path));
                if (c)
                {
                    frame_cache_close(*c);
                }
                if (wake != nullptr)
                {
                    wake(); // the next frame picks the verdict up
                }
                return !c.has_value();
            });
        }
    }
    if (r.watch_check.valid() && r.watch_check.wait_for(std::chrono::seconds(0)) == std::future_status::ready
        && r.watch_check.get() && !r.running)
    {
        replay_load(app, r, std::string(r.data.path));
    }
}

void draw_replay(App& app, WorkspaceTab& tab, Replay& r)
{
    // Autoplay follows the measurement, whether or not the window is shown.
    if (app.measuring != r.was_measuring)
    {
        r.was_measuring = app.measuring;
        if (r.autoplay && app.measuring && !r.running && !replay_frames(r.data).empty())
        {
            replay_start(r, app.ifaces, app.tasks);
        }
        else if (r.autoplay && !app.measuring && r.running)
        {
            replay_stop(r);
            r.position = 0;
        }
    }
    if (!r.running && r.player.joinable())
    {
        r.player.join(); // finished on its own
    }
    replay_load_poll(app, r);
    replay_watch(app, r);
    if (r.loader.joinable() && r.load_changed)
    {
        status_bar_notice(app.status_bar, std::format("File changed on disk, reloading... {:.0f} %", r.load_fraction * 100.0f), 1.0);
    }
    if (r.was_open && !r.open && r.loader.joinable())
    {
        replay_load_cancel(r); // window closed while diving (a reload of a changed file goes on closed)
        r.data.info.clear();
    }
    r.was_open = r.open;
    if (!r.open && r.running)
    {
        replay_stop(r); // closed with its X: nothing may keep sending unseen (T87b a3 F4)
    }
    const WorkspaceTab* current = workspace_current(app.workspace);
    if (!r.open || current == nullptr || current->uid != tab.uid)
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f; // the size is in 1x pixels: scale with the font (HiDPI)
    ImGui::SetNextWindowSize(ImVec2(640.0f * px, 480.0f * px), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(workspace_window_name(tab, "Replay").c_str(), &r.open))
    {
        const bool running = r.running;
        ImGui::BeginDisabled(running);
        if (icon_text_button("Load", Icon::DocumentOpen))
        {
            file_dialog_open(r.load_dialog, FileDialogMode::Open, "Load Trace File", r.data.path, trace_read_filters);
        }
        for (const auto& path : file_dialog_draw(r.load_dialog))
        {
            replay_load(app, r, path);
            file_dialog_open(r.db_dialog, FileDialogMode::OpenMultiple, "Load CAN Databases for the Replay (Cancel: none)", "",
                             can_db_read_filters);
        }
        if (const std::vector<std::string> dbs = file_dialog_draw(r.db_dialog); !dbs.empty())
        {
            replay_add_databases(app, r, dbs);
        }
        same_line_or_wrap(icon_text_button_width("Play"));
        ImGui::BeginDisabled(replay_frames(r.data).empty());
        if (icon_text_button("Play", Icon::PlaybackStart))
        {
            replay_start(r, app.ifaces, app.tasks);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        const bool paused = running && r.hold == replay_paused;
        same_line_or_wrap(icon_text_button_width("Continue"));
        ImGui::BeginDisabled(!running);
        if (paused ? icon_text_button("Continue", Icon::PlaybackStart) : icon_text_button("Pause", Icon::PlaybackPause))
        {
            paused ? replay_resume(r) : replay_pause(r);
        }
        ImGui::EndDisabled();
        same_line_or_wrap(icon_text_button_width("Step"));
        ImGui::BeginDisabled(running ? !paused : replay_frames(r.data).empty());
        if (icon_text_button("Step", Icon::PlaybackStep))
        {
            if (paused)
            {
                replay_single_step(r);
            }
            else
            {
                replay_start(r, app.ifaces, app.tasks, replay_stepping);
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Send the next message and pause");
        same_line_or_wrap(icon_text_button_width("Stop"));
        ImGui::BeginDisabled(!running);
        if (icon_text_button("Stop", Icon::PlaybackStop))
        {
            replay_stop(r);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Autoplay", &r.autoplay);
        ImGui::SetItemTooltip("Automatically start/stop replay with measurement");
        ImGui::SameLine();
        ImGui::BeginDisabled(running);
        ImGui::Checkbox("Loop", &r.loop);
        ImGui::SetItemTooltip("Restart replay after all messages were sent");
        const float check = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
        same_line_or_wrap(check + ImGui::CalcTextSize("As fast as possible").x);
        ImGui::Checkbox("As fast as possible", &r.fast);
        ImGui::SetItemTooltip("No timing: every frame right after the previous one, with the file's timestamps\n"
                              "in the trace (a real bus may drop frames)");
        same_line_or_wrap(ImGui::GetFontSize() * 6.0f + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Speed").x);
        ImGui::BeginDisabled(r.fast);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::SliderFloat("Speed", &r.speed, 0.1f, 10.0f, "%.1fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Playback speed multiplier, 1.0x = normal speed");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // The file by name (the whole path in its tooltip), then the strip, then the figures.
        if (r.data.path.empty())
        {
            ImGui::TextDisabled("No trace file loaded");
        }
        else
        {
            ImGui::TextUnformatted(std::filesystem::path(r.data.path).filename().string().c_str());
            ImGui::SetItemTooltip("%s", r.data.path.c_str());
        }
        const std::string from0 = r.range_from; // A / B before this frame's edits (markers or fields)
        const std::string to0 = r.range_to;
        const std::size_t in_range = replay_range(replay_frames(r.data), r.range_from, r.range_to).size();
        if (r.loader.joinable())
        {
            const float f = r.load_fraction;
            draw_depth_gauge(0.0f, f, ImVec2(-FLT_MIN, 0.0f), true);
            ImGui::SetItemTooltip("Diving... %.0f %% loaded", f * 100.0f);
        }
        else
        {
            // The strip is the file's time axis: markers A / B are the replay's start and end (the
            // "Play from / to" fields, kept in step both ways), the water runs from A to the frame
            // being played.
            const std::span<const FrameCacheRec> all = replay_frames(r.data);
            const double duration = all.empty() ? 0.0 : static_cast<double>(all.back().ts_ns - all.front().ts_ns) / 1e9;
            const auto at = [&](const std::string& text, float open) -> float
            {
                const auto secs = parse_duration(text);
                return secs && duration > 0.0 ? static_cast<float>(std::clamp(*secs / duration, 0.0, 1.0)) : open;
            };
            float a = at(r.range_from, 0.0f);
            float b = std::max(a, at(r.range_to, 1.0f));
            const float a0 = a;
            const float b0 = b;
            float played = a;
            if ((running || r.position > 0) && r.position > 0 && duration > 0.0)
            {
                const auto& cur = r.play_frames[std::min<std::size_t>(r.position, r.play_frames.size()) - 1];
                played = static_cast<float>(std::clamp(static_cast<double>(cur.ts_ns - all.front().ts_ns) / 1e9 / duration, 0.0, 1.0));
            }
            ImGui::BeginDisabled(running); // the range is fixed while playing
            draw_depth_gauge(a, played, ImVec2(-FLT_MIN, 0.0f), running, &a, &b);
            ImGui::EndDisabled();
            if (a != a0)
            {
                r.range_from = a > 0.0f ? format_duration(a * duration) : std::string();
            }
            if (b != b0)
            {
                r.range_to = b < 1.0f ? format_duration(b * duration) : std::string();
            }
            const std::size_t total = running || r.position > 0 ? r.play_frames.size() : in_range;
            ImGui::SetItemTooltip("%s", std::format("{} / {} messages\nA {}  B {}  (drag the markers)", static_cast<std::size_t>(r.position), total,
                                                    format_duration(a * duration), format_duration(b * duration)).c_str());
        }
        if (r.loader.joinable())
        {
            // Counts up while the file is read; the total is estimated from the bytes read so far.
            const uint64_t read = r.load_frames;
            const double share = r.load_fraction / 0.9;
            std::string line = "Messages: ";
            append_grouped(line, read);
            if (read > 0 && share > 0.0 && share < 1.0)
            {
                line += " / ~";
                append_grouped(line, static_cast<uint64_t>(static_cast<double>(read) / share));
            }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.load_started).count();
            const double mb = static_cast<double>(r.load_bytes) / 1e6 * share;
            ImGui::TextUnformatted(r.data.info.c_str()); // "Diving into ..."
            ImGui::TextUnformatted(line.c_str());
            ImGui::Text("Time: %s%s  ·  %.0f MB/s", format_duration(secs, 1).c_str(), secs < 60.0 ? " s" : "",
                        secs > 0.0 ? mb / secs : 0.0);
        }
        else
        {
            ImGui::TextDisabled("%s", r.data.info.empty() ? "Trace file info..." : r.data.info.c_str());
        }
        {
            const auto now = std::chrono::steady_clock::now();
            const std::size_t pos = r.position;
            if (running && !r.rate_running) // a run starts
            {
                r.rate_started = r.rate_at = now;
                r.rate_from = pos;
                r.rate_fps = 0.0;
            }
            const double since = std::chrono::duration<double>(now - r.rate_at).count();
            if (running && since >= 0.5)
            {
                r.rate_fps = static_cast<double>(pos - std::min(pos, r.rate_from)) / since;
                r.rate_from = pos;
                r.rate_at = now;
            }
            else if (!running && r.rate_running) // it ended: the run's average
            {
                const double total = std::chrono::duration<double>(now - r.rate_started).count();
                r.rate_fps = total > 0.0 ? static_cast<double>(pos) / total : 0.0;
            }
            r.rate_running = running;
        }
        if (const std::size_t next = r.position; paused && next < r.play_frames.size())
        {
            std::string index;
            append_grouped(index, next + 1);
            const double at = static_cast<double>(r.play_frames[next].ts_ns - replay_frames(r.data).front().ts_ns) / 1e9;
            ImGui::Text("Paused before message %s at %s", index.c_str(), format_duration(at, 3).c_str());
        }
        else if (running || r.position > 0)
        {
            std::string rate;
            append_grouped(rate, static_cast<uint64_t>(r.rate_fps + 0.5));
            ImGui::Text("%s frames/s%s", rate.c_str(), running ? "" : " (average)");
        }

        ImGui::BeginDisabled(running);
        const float px = ImGui::GetFontSize() / 15.0f;
        const auto time_field = [px](const char* id, const char* hint, std::string& text)
        {
            ImGui::SetNextItemWidth(130.0f * px);
            const bool bad = !text.empty() && !parse_duration(text);
            if (bad)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme_text(ThemeText::error));
            }
            ImGui::InputTextWithHint(id, hint, &text);
            if (bad)
            {
                ImGui::PopStyleColor();
            }
            ImGui::SetItemTooltip("Time since the first frame: 90, 1:30, 1:02:03.5 or 2d 1:02:03");
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Play from");
        ImGui::SameLine();
        time_field("##range_from", "start", r.range_from);
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        time_field("##range_to", "end", r.range_to);
        if (const std::span<const FrameCacheRec> all = replay_frames(r.data);
            (r.range_from != from0 || r.range_to != to0) && !all.empty())
        {
            // A / B moved: the tab's graphs show that stretch, with their cursors on its ends.
            const double end = static_cast<double>(all.back().ts_ns - all.front().ts_ns) * 1e-9;
            const double a = std::clamp(parse_duration(r.range_from).value_or(0.0), 0.0, end);
            const double b = std::clamp(parse_duration(r.range_to).value_or(end), a, end);
            for (GraphState& g : tab.graphs)
            {
                if (g.start_ns < 0)
                {
                    continue; // nothing ingested yet: no time base
                }
                const double base = static_cast<double>(all.front().ts_ns - g.start_ns) * 1e-9;
                g.follow = false;
                g.x_min = base + a;
                g.x_max = base + std::max(b, a + 1e-3);
                g.cursor_on = true;
                g.cursor_a = base + a;
                g.cursor_b = base + b;
            }
        }
        ImGui::SameLine();
        std::string count;
        append_grouped(count, in_range);
        ImGui::TextDisabled("%s messages", count.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Break at");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##break_at", "times since the first frame, e.g. 1:30, 2:10.5", &r.break_at);
        ImGui::SetItemTooltip("Breakpoints: the replay pauses before the first message at or after each time.\n"
                              "The Break column below pauses before every message of an id.");
        ImGui::EndDisabled();

        ImGui::BeginDisabled(running); // ponytail: filters and mapping are snapshotted at Play; make them live if editing mid-playback is wanted
        const bool select_all = ImGui::Button("Select All");
        ImGui::SameLine();
        if (ImGui::Button("Deselect All") || select_all)
        {
            for (ReplayIdRow& row : r.data.rows)
            {
                row.rx_on = select_all && row.has_rx;
                row.tx_on = select_all && row.has_tx;
            }
        }
        draw_filter_table(app, r);
        ImGui::EndDisabled();
    }
    ImGui::End();
}
