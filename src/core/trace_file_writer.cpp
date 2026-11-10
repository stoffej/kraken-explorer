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

#include "trace_file_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <ostream>
#include <string_view>
#include <ranges>
#include <unordered_map>
#include <vector>

#include "core/pcapng.h"
#include "core/socket_can.h"
#include "core/trace_line_format.h"

// VERSION_STRING is a global compile definition for the application; keep a fallback.
#ifndef VERSION_STRING
#define VERSION_STRING "dev"
#endif

namespace
{

void write_bytes(std::ostream& out, const std::vector<uint8_t>& bytes)
{
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void append_raw(std::vector<uint8_t>& out, std::string_view s)
{
    out.insert(out.end(), s.begin(), s.end());
}

// TXBLOCK / MDBLOCK size: 24-byte header + text + NUL, padded to 8 bytes.
[[nodiscard]] constexpr uint64_t mdf_text_block_size(std::string_view text) noexcept
{
    return (24 + static_cast<uint64_t>(text.size()) + 1 + 7) & ~uint64_t{7};
}

struct MdfChannel
{
    std::string_view name;
    std::string_view unit;
    uint8_t type;        // cn_type: 0 = fixed length, 2 = master
    uint8_t sync_type;   // cn_sync_type: 0 = none, 1 = time
    uint8_t data_type;   // cn_data_type: 0 = UINT LE, 4 = REAL LE, 10 = byte array
    uint32_t byte_offset;
    uint32_t bit_count;
    uint64_t offset = 0;
    uint64_t name_offset = 0;
    uint64_t unit_offset = 0;
};


[[nodiscard]] int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr std::size_t line_chunk = std::size_t{1} << 16; // frames per chunk; a chunk starts at a multiple

// Every frame's bytes, encoded on up to 16 threads in chunks of line_chunk frames and handed to
// sink in file order: one thread formatted ~3.7M text lines/s (a 2 GB candump took 12 s), the
// binary records are as independent. encode(buf, m, i) appends frame i's bytes and may only read
// shared state. Exceptions from a chunk (a stopped write) reach the caller.
template <class Buffer, class Frames, class Encode, class Sink>
void encode_parallel(const Frames& messages, const Encode& encode, const Sink& sink)
{
    constexpr std::size_t chunk = line_chunk;
    const std::size_t n = std::ranges::size(messages);
    const std::size_t chunks = (n + chunk - 1) / chunk;
    const std::size_t threads = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 16);
    std::vector<Buffer> bufs(threads);
    std::vector<std::future<void>> inflight(threads);
    const auto launch = [&](std::size_t k)
    {
        inflight[k % threads] = std::async(std::launch::async, [&, k]
        {
            Buffer& b = bufs[k % threads];
            b.clear();
            for (std::size_t i = k * chunk; i < std::min(n, (k + 1) * chunk); ++i)
            {
                encode(b, messages[i], i);
            }
        });
    };
    for (std::size_t k = 0; k < std::min(threads, chunks); ++k)
    {
        launch(k);
    }
    for (std::size_t k = 0; k < chunks; ++k)
    {
        inflight[k % threads].get();
        sink(bufs[k % threads]);
        if (k + threads < chunks)
        {
            launch(k + threads);
        }
    }
}

template <class Frames, class Line>
void write_lines(std::ostream& out, const Frames& messages, const Line& line)
{
    encode_parallel<std::string>(messages, line, [&](const std::string& t) { out.write(t.data(), static_cast<std::streamsize>(t.size())); });
}

template <class Frames, class Encode>
void write_records(std::ostream& out, const Frames& messages, const Encode& encode)
{
    encode_parallel<std::vector<uint8_t>>(messages, encode, [&](const std::vector<uint8_t>& b) { write_bytes(out, b); });
}

// Linux candump -L lines.
template <class Frames>
void write_candump(std::ostream& out, const Frames& messages, const IfaceNameFn& iface_name)
{
    std::vector<std::string> names; // iface_name per interface, looked up once (it returns a new string)
    for (const BusMessage& m : messages)
    {
        if (m.iface >= names.size())
        {
            names.resize(m.iface + 1u);
        }
        if (names[m.iface].empty())
        {
            names[m.iface] = iface_name(m.iface);
        }
    }
    write_lines(out, messages, [&](std::string& t, const BusMessage& m, std::size_t)
    {
        append_candump_line(t, m, names[m.iface]);
        t += '\n';
    });
}

// Vector ASC; times relative to the earliest message (the trace may be unsorted), channels numbered 1.. in order
// of first appearance. An empty trace still gets header and footer.
template <class Frames>
void write_vector_asc(std::ostream& out, const Frames& messages)
{
    using namespace std::chrono;
    // One pass for the earliest time and the channel numbers (1.. in order of first appearance).
    int64_t start_ns = messages.empty() ? now_ns() : INT64_MAX;
    std::vector<int> channel; // per iface, 0 = not seen
    int channels = 0;
    for (const BusMessage& m : messages)
    {
        start_ns = std::min(start_ns, m.ts_ns);
        if (m.iface >= channel.size())
        {
            channel.resize(m.iface + 1u, 0);
        }
        if (channel[m.iface] == 0)
        {
            channel[m.iface] = ++channels;
        }
    }

    std::string line;
    append_asc_header(line, system_clock::time_point(duration_cast<system_clock::duration>(nanoseconds(start_ns))));
    out << line;
    write_lines(out, messages, [&](std::string& t, const BusMessage& m, std::size_t)
    {
        append_asc_line(t, m, start_ns, channel[m.iface]);
        t += '\n';
    });
    out << asc_footer << '\n';
}

// PEAK PCAN trace 2.1; like ASC, times relative to the earliest message and buses numbered 1.. in order of first
// appearance. LIN frames have no TRC representation and are left out.
template <class Frames>
void write_trc(std::ostream& out, const Frames& messages)
{
    // One pass for the earliest time, the bus numbers (1.. in order of first appearance) and the
    // message numbers (CAN frames only) at each 64k-frame mark, so the lines format in parallel.
    constexpr std::size_t mark = line_chunk;
    int64_t start_ns = messages.empty() ? now_ns() : INT64_MAX;
    std::vector<int> bus;
    int buses = 0;
    std::vector<uint64_t> number_at; // CAN frames before frame k * mark
    uint64_t can_frames = 0;
    std::size_t i = 0;
    for (const BusMessage& m : messages)
    {
        if (i++ % mark == 0)
        {
            number_at.push_back(can_frames);
        }
        start_ns = std::min(start_ns, m.ts_ns);
        if (m.type != BusType::CAN)
        {
            continue;
        }
        ++can_frames;
        if (m.iface >= bus.size())
        {
            bus.resize(m.iface + 1u, 0);
        }
        if (bus[m.iface] == 0)
        {
            bus[m.iface] = ++buses;
        }
    }

    std::string line;
    append_trc_header(line, start_ns);
    out << line;
    write_lines(out, messages, [&](std::string& t, const BusMessage& m, std::size_t index)
    {
        static thread_local uint64_t number = 0; // this thread's chunk: CAN frames so far
        if (index % line_chunk == 0)
        {
            number = number_at[index / mark];
        }
        if (m.type != BusType::CAN)
        {
            return;
        }
        append_trc_line(t, m, ++number, start_ns, bus[m.iface]);
        t += "\r\n";
    });
}

// ASAM MDF 4.10 with one channel group of fixed-size records:
//   t (REAL, seconds since the first message, master)
//   | CAN_ID (canid_t, see core/socket_can.h: flags, error classes for error frames)
//   | DLC (payload length) | Dir (0 = Rx, 1 = Tx) | DataBytes (64-byte array)
template <class Frames>
void write_vector_mdf(std::ostream& out, const Frames& messages)
{
    // The master channel t must be non-decreasing; frames of several interfaces may not be.
    if (!std::ranges::is_sorted(messages, {}, &BusMessage::ts_ns))
    {
        std::vector<BusMessage> sorted(messages.begin(), messages.end());
        std::ranges::stable_sort(sorted, {}, &BusMessage::ts_ns);
        write_vector_mdf(out, std::span<const BusMessage>(sorted));
        return;
    }
    const int64_t t_start_ns = messages.empty() ? now_ns() / 1000000 * 1000000 : messages.front().ts_ns;
    std::vector<uint8_t> b;
    (void)append_mdf_header(b, t_start_ns, static_cast<uint64_t>(messages.size()), false);
    write_bytes(out, b);
    write_records(out, messages, [&](std::vector<uint8_t>& r, const BusMessage& m, std::size_t)
    {
        append_mdf_record(r, m, t_start_ns);
    });
}

// Windows SYSTEMTIME of a UTC ns timestamp: year, month, day of week, day, hour, minute, second, ms.
void append_systemtime(std::vector<uint8_t>& b, int64_t ns)
{
    using namespace std::chrono;
    const sys_time<nanoseconds> t{nanoseconds{ns}};
    const auto day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss hms{floor<milliseconds>(t - day)};
    for (const unsigned v : {static_cast<unsigned>(static_cast<int>(ymd.year())), static_cast<unsigned>(ymd.month()),
                             weekday{day}.c_encoding(), static_cast<unsigned>(ymd.day()),
                             static_cast<unsigned>(hms.hours().count()), static_cast<unsigned>(hms.minutes().count()),
                             static_cast<unsigned>(hms.seconds().count()), static_cast<unsigned>(hms.subseconds().count())})
    {
        append_le(b, static_cast<uint16_t>(v));
    }
}

// Vector BLF: CAN_MESSAGE, CAN_FD_MESSAGE_64 and CAN_ERROR_EXT objects, LIN_MESSAGE /
// LIN_CRC_ERROR / LIN_SLEEP for LIN frames (header v1, ns timestamps since the file start: the
// first frame's whole millisecond, as SYSTEMTIME holds no less) in stored (uncompressed)
// LOG_CONTAINERs of 128 KiB. Channel = iface + 1.
// ponytail: no zlib containers (files ~5x bigger than Vector's); a LIN frame nobody answered
// goes out as a LIN_MESSAGE (Vector has LIN_RCV_ERROR for it).
// One BLF object (CAN, CAN FD, error, LIN, LIN sleep/wakeup) of m, padded as the readers expect.
// Returns its size, so a first pass can size the file without building it.
std::size_t append_blf_object(std::vector<uint8_t>* inner_ptr, const BusMessage& m, int64_t start)
{
    const bool fd = has_flag(m, bus_flag::fd);
    const bool lin = m.type == BusType::LIN;
    const uint32_t type = lin ? (has_flag(m, bus_flag::lin_sleep | bus_flag::lin_wakeup) ? 20
                                 : (m.errors & bus_error::lin_checksum_error) != 0 ? 12 : 11)
                              : is_error_frame(m) ? 73 : fd ? 101 : 1;
    const uint32_t body = type == 73 ? 32 : type == 101 ? 40u + m.len : type == 20 ? 8 : lin ? 24 : 16;
    const uint32_t id = can_id(m) | (has_flag(m, bus_flag::extended) ? 0x80000000u : 0);
    if (inner_ptr == nullptr)
    {
        return 32 + body + body % 4;
    }
    std::vector<uint8_t>& inner = *inner_ptr;
    append_raw(inner, "LOBJ");
    append_le(inner, uint16_t{32});        // header size
    append_le(inner, uint16_t{1});         // header version
    append_le(inner, uint32_t{32 + body}); // object size
    append_le(inner, type);
    append_le(inner, uint32_t{2});         // flags: TIME_ONE_NANS
    append_le(inner, uint16_t{0});         // client index
    append_le(inner, uint16_t{0});         // object version
    append_le(inner, static_cast<uint64_t>(std::max<int64_t>(0, m.ts_ns - start)));
    const auto channel = static_cast<uint16_t>(m.iface + 1);
    if (type == 73)
    {
        append_le(inner, channel);
        append_le(inner, uint16_t{0});     // length
        inner.resize(inner.size() + 28);   // flags, ecc, position, dlc, frame length, id, ext flags, data
    }
    else if (type == 101)
    {
        inner.push_back(static_cast<uint8_t>(channel));
        inner.push_back(m.dlc);
        inner.push_back(m.len);            // valid data bytes
        inner.push_back(0);                // tx count
        append_le(inner, id);
        append_le(inner, uint32_t{0});     // frame length
        append_le(inner, uint32_t{0x1000u | (has_flag(m, bus_flag::brs) ? 0x2000u : 0u)}); // EDL, BRS
        inner.resize(inner.size() + 18);   // bit timings, offsets, bit count
        inner.push_back(has_flag(m, bus_flag::tx) ? 1 : 0);
        inner.push_back(0);                // ext data offset
        append_le(inner, uint32_t{0});     // crc
        inner.insert(inner.end(), m.data.begin(), m.data.begin() + m.len);
    }
    else if (type == 20)
    {
        append_le(inner, channel);
        inner.push_back(0);                // reason
        inner.push_back(has_flag(m, bus_flag::lin_wakeup) ? 0x02 : 0); // flags bit 1: awake after this event
        append_le(inner, uint32_t{0});
    }
    else if (lin)
    {
        append_le(inner, channel);
        inner.push_back(static_cast<uint8_t>(m.id));
        inner.push_back(m.len);
        inner.insert(inner.end(), m.data.begin(), m.data.begin() + 8);
        inner.resize(inner.size() + 4);    // fsm id, fsm state, header time, full time
        append_le(inner, uint16_t{0});     // crc
        inner.push_back(has_flag(m, bus_flag::tx) ? 1 : 0);
        inner.resize(inner.size() + 5);    // reserved
    }
    else
    {
        append_le(inner, channel);
        inner.push_back(static_cast<uint8_t>((has_flag(m, bus_flag::tx) ? 0x01 : 0) | (has_flag(m, bus_flag::rtr) ? 0x80 : 0)));
        inner.push_back(m.dlc);
        append_le(inner, id);
        inner.insert(inner.end(), m.data.begin(), m.data.begin() + 8);
    }
    inner.resize(inner.size() + body % 4); // padding, as the readers expect
    return 32 + body + body % 4;
}

template <class Frames>
void write_blf(std::ostream& out, const Frames& messages)
{
    const int64_t first = messages.empty() ? now_ns() : messages.front().ts_ns;
    const int64_t start = first - (first % 1'000'000 + 1'000'000) % 1'000'000;
    // First pass: the objects' total size and count, for the file header; then the containers are
    // written as they fill (objects may straddle two containers, as in Vector's files).
    uint64_t total = 0;
    uint32_t count = 0;
    for (const BusMessage& m : messages)
    {
        total += append_blf_object(nullptr, m, start);
        ++count;
    }

    constexpr std::size_t chunk = 128 * 1024;
    std::vector<uint8_t> b;
    append_raw(b, "LOGG");
    append_le(b, uint32_t{144});               // header size
    b.insert(b.end(), {0, 0, 0, 0, 2, 6, 8, 1}); // application id + version, BLF 2.6.8.1
    const uint64_t containers = (total + chunk - 1) / chunk;
    const uint64_t tail = total - (containers == 0 ? 0 : (containers - 1) * chunk);
    append_le(b, static_cast<uint64_t>(144 + containers * 32 + total + tail % 4)); // file size
    append_le(b, static_cast<uint64_t>(144 + containers * 32 + total));            // uncompressed size
    append_le(b, count);
    append_le(b, uint32_t{0});                 // objects read
    append_systemtime(b, start);
    append_systemtime(b, messages.empty() ? start : messages.back().ts_ns);
    b.resize(144);
    write_bytes(out, b);

    std::vector<uint8_t> inner;
    const auto container = [&](const uint8_t* bytes, std::size_t n)
    {
        b.clear();
        append_raw(b, "LOBJ");
        append_le(b, uint16_t{16});            // base header only
        append_le(b, uint16_t{1});
        append_le(b, static_cast<uint32_t>(32 + n));
        append_le(b, uint32_t{10});            // LOG_CONTAINER
        append_le(b, uint16_t{0});             // stored
        b.resize(b.size() + 6);
        append_le(b, static_cast<uint32_t>(n)); // uncompressed size
        b.resize(b.size() + 4);
        b.insert(b.end(), bytes, bytes + n);
        b.resize(b.size() + n % 4);
        write_bytes(out, b);
    };
    encode_parallel<std::vector<uint8_t>>(
        messages, [&](std::vector<uint8_t>& objects, const BusMessage& m, std::size_t) { append_blf_object(&objects, m, start); },
        [&](const std::vector<uint8_t>& objects)
        {
            inner.insert(inner.end(), objects.begin(), objects.end());
            std::size_t used = 0;
            for (; inner.size() - used >= chunk; used += chunk)
            {
                container(inner.data() + used, chunk);
            }
            inner.erase(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(used));
        });
    if (!inner.empty())
    {
        container(inner.data(), inner.size());
    }
}

// libpcap, microsecond timestamps, LINKTYPE_CAN_SOCKETCAN.
template <class Frames>
void write_pcap(std::ostream& out, const Frames& messages)
{
    std::vector<uint8_t> b;
    append_le(b, uint32_t{0xA1B2C3D4});  // magic: microsecond timestamps
    append_le(b, uint16_t{2});
    append_le(b, uint16_t{4});
    append_le(b, int32_t{0});            // thiszone: UTC
    append_le(b, uint32_t{0});           // sigfigs
    append_le(b, uint32_t{72});          // snaplen: CANFD_MTU
    append_le(b, uint32_t{227});         // LINKTYPE_CAN_SOCKETCAN

    write_bytes(out, b);
    write_records(out, messages, [](std::vector<uint8_t>& r, const BusMessage& m, std::size_t)
    {
        const std::size_t at = r.size();
        r.resize(at + 16); // record header, filled in once the frame's size is known
        append_socketcan_frame(r, m);
        const auto size = static_cast<uint32_t>(r.size() - at - 16);
        const int64_t ts_us = m.ts_ns / 1000;
        const uint32_t head[4] = {static_cast<uint32_t>(ts_us / 1000000), static_cast<uint32_t>(ts_us % 1000000), size, size}; // incl_len, orig_len
        for (int k = 0; k < 16; ++k)
        {
            r[at + k] = static_cast<uint8_t>(head[k / 4] >> (8 * (k % 4)));
        }
    });
}

// pcapng: one section, one interface description per interface in order of
// first appearance, then one enhanced packet per message.
template <class Frames>
void write_pcapng(std::ostream& out, const Frames& messages, const IfaceNameFn& iface_name)
{
    std::unordered_map<uint16_t, uint32_t> index;
    std::vector<uint16_t> order;
    for (const BusMessage& m : messages)
    {
        if (index.try_emplace(m.iface, static_cast<uint32_t>(order.size())).second)
        {
            order.push_back(m.iface);
        }
    }

    std::vector<uint8_t> b;
    pcapng_append_section_header(b);
    for (const uint16_t iface : order)
    {
        pcapng_append_interface(b, iface_name(iface));
    }
    write_bytes(out, b);
    write_records(out, messages, [&](std::vector<uint8_t>& r, const BusMessage& m, std::size_t) { pcapng_append_packet(r, m, index.at(m.iface)); });
}

template <class Frames>
void write_frames(std::ostream& out, TraceFileFormat format, const Frames& messages, const IfaceNameFn& iface_name)
{
    switch (format)
    {
        case TraceFileFormat::CanDump:   write_candump(out, messages, iface_name); return;
        case TraceFileFormat::VectorAsc: write_vector_asc(out, messages); return;
        case TraceFileFormat::VectorMdf: write_vector_mdf(out, messages); return;
        case TraceFileFormat::Pcap:      write_pcap(out, messages); return;
        case TraceFileFormat::PcapNg:    write_pcapng(out, messages, iface_name); return;
        case TraceFileFormat::Trc:       write_trc(out, messages); return;
        case TraceFileFormat::Blf:       write_blf(out, messages); return;
    }
}

struct WriteStopped
{
};

} // namespace

MdfLayout append_mdf_header(std::vector<uint8_t>& out, int64_t start_ns, uint64_t record_count, bool unfinalized)
{
    MdfLayout layout;
    // Block sizes follow the ASAM MDF 4.1 block definitions: 24-byte common
    // header, 8 bytes per link, then the block's data section.
    constexpr uint64_t sz_id = 64;
    constexpr uint64_t sz_hd = 104;  // 6 links + 32 data
    constexpr uint64_t sz_fh = 56;   // 2 links + 16 data
    constexpr uint64_t sz_dg = 64;   // 4 links + 8 data
    constexpr uint64_t sz_cg = 104;  // 6 links + 32 data
    constexpr uint64_t sz_cn = 160;  // 8 links + 72 data

    // Microsecond resolution, as the trace timestamps always had.
    const auto start_time_ns = static_cast<uint64_t>(start_ns / 1000) * 1000;

    // The file history comment must be an MDBLOCK with <FHcomment>.
    const std::string fh_comment =
        std::string("<FHcomment xmlns=\"http://www.asam.net/mdf/v4\"><TX>Kraken Explorer trace export</TX>"
                    "<tool_id>Kraken Explorer</tool_id><tool_vendor>Kraken Explorer</tool_vendor><tool_version>")
        + VERSION_STRING + "</tool_version></FHcomment>";
    constexpr std::string_view acquisition_name = "CAN";

    std::array<MdfChannel, 5> channels = {{
        { "t",         "s", 2, 1, 4,  0,  64  },
        { "CAN_ID",    "",  0, 0, 0,  8,  32  },
        { "DLC",       "",  0, 0, 0,  12, 8   },
        { "Dir",       "",  0, 0, 0,  13, 8   },
        { "DataBytes", "",  0, 0, 10, 14, 512 },
    }};

    // --- Block offsets ---
    uint64_t off = sz_id + sz_hd;
    const uint64_t o_fh = off;      off += sz_fh;
    const uint64_t o_md_fh = off;   off += mdf_text_block_size(fh_comment);
    const uint64_t o_dg = off;      off += sz_dg;
    const uint64_t o_cg = off;      off += sz_cg;
    const uint64_t o_tx_cg = off;   off += mdf_text_block_size(acquisition_name);
    for (MdfChannel& ch : channels)
    {
        ch.offset = off;            off += sz_cn;
        ch.name_offset = off;       off += mdf_text_block_size(ch.name);
        if (!ch.unit.empty())
        {
            ch.unit_offset = off;   off += mdf_text_block_size(ch.unit);
        }
    }
    const uint64_t o_dt = off;

    std::vector<uint8_t>& b = out;
    const std::size_t base = b.size();

    auto zeros = [&b](size_t count) { b.insert(b.end(), count, 0); };
    auto block_header = [&b](std::string_view id, uint64_t length, uint64_t link_count)
    {
        append_raw(b, id);
        append_le(b, uint32_t{0});  // reserved
        append_le(b, length);
        append_le(b, link_count);
    };
    auto text_block = [&](std::string_view id, std::string_view text)
    {
        const uint64_t size = mdf_text_block_size(text);
        block_header(id, size, 0);
        append_raw(b, text);
        zeros(static_cast<size_t>(size - 24 - text.size()));  // NUL terminator + padding
    };

    // ===== IDBLOCK (no common header) =====
    append_raw(b, unfinalized ? "UnFinMF " : "MDF     ");
    append_raw(b, "4.10    ");
    append_raw(b, "Kraken  ");  // id_prog: 8 chars, space padded
    zeros(4);                        // id_reserved1
    append_le(b, uint16_t{410});     // id_ver
    zeros(30);                       // id_reserved2
    append_le(b, uint16_t{unfinalized ? mdf_unfin_flags : uint16_t{0}}); // id_unfin_flags
    append_le(b, uint16_t{0});       // id_custom_unfin_flags

    // ===== HDBLOCK =====
    block_header("##HD", sz_hd, 6);
    append_le(b, o_dg);              // hd_dg_first
    append_le(b, o_fh);              // hd_fh_first
    append_le(b, uint64_t{0});       // hd_ch_first
    append_le(b, uint64_t{0});       // hd_at_first
    append_le(b, uint64_t{0});       // hd_ev_first
    append_le(b, uint64_t{0});       // hd_md_comment
    append_le(b, start_time_ns);     // hd_start_time_ns (UTC)
    append_le(b, int16_t{0});        // hd_tz_offset_min
    append_le(b, int16_t{0});        // hd_dst_offset_min
    append_le(b, uint8_t{0});        // hd_time_flags: UTC, offsets not valid
    append_le(b, uint8_t{0});        // hd_time_class: local PC reference time
    append_le(b, uint8_t{0});        // hd_flags: start angle / distance not valid
    append_le(b, uint8_t{0});        // hd_reserved
    append_le(b, 0.0);               // hd_start_angle_rad
    append_le(b, 0.0);               // hd_start_distance_m

    // ===== FHBLOCK =====
    block_header("##FH", sz_fh, 2);
    append_le(b, uint64_t{0});       // fh_fh_next
    append_le(b, o_md_fh);           // fh_md_comment
    append_le(b, start_time_ns);     // fh_time_ns
    append_le(b, int16_t{0});        // fh_tz_offset_min
    append_le(b, int16_t{0});        // fh_dst_offset_min
    append_le(b, uint8_t{0});        // fh_time_flags
    zeros(3);                        // fh_reserved
    text_block("##MD", fh_comment);

    // ===== DGBLOCK =====
    block_header("##DG", sz_dg, 4);
    append_le(b, uint64_t{0});       // dg_dg_next
    append_le(b, o_cg);              // dg_cg_first
    append_le(b, o_dt);              // dg_data
    append_le(b, uint64_t{0});       // dg_md_comment
    append_le(b, uint8_t{0});        // dg_rec_id_size: single channel group
    zeros(7);                        // dg_reserved

    // ===== CGBLOCK =====
    block_header("##CG", sz_cg, 6);
    append_le(b, uint64_t{0});       // cg_cg_next
    append_le(b, channels[0].offset);// cg_cn_first
    append_le(b, o_tx_cg);           // cg_tx_acq_name
    append_le(b, uint64_t{0});       // cg_si_acq_source
    append_le(b, uint64_t{0});       // cg_sr_first
    append_le(b, uint64_t{0});       // cg_md_comment
    append_le(b, uint64_t{0});       // cg_record_id
    layout.cycle_count_at = b.size() - base;
    append_le(b, record_count);      // cg_cycle_count
    append_le(b, uint16_t{0});       // cg_flags
    append_le(b, uint16_t{0});       // cg_path_separator
    zeros(4);                        // cg_reserved
    append_le(b, mdf_record_size);   // cg_data_bytes
    append_le(b, uint32_t{0});       // cg_inval_bytes
    text_block("##TX", acquisition_name);

    // ===== CNBLOCKs =====
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const MdfChannel& ch = channels[i];
        block_header("##CN", sz_cn, 8);
        append_le(b, i + 1 < channels.size() ? channels[i + 1].offset : uint64_t{0});  // cn_cn_next
        append_le(b, uint64_t{0});   // cn_composition
        append_le(b, ch.name_offset);// cn_tx_name
        append_le(b, uint64_t{0});   // cn_si_source
        append_le(b, uint64_t{0});   // cn_cc_conversion
        append_le(b, uint64_t{0});   // cn_data
        append_le(b, ch.unit_offset);// cn_md_unit
        append_le(b, uint64_t{0});   // cn_md_comment
        append_le(b, ch.type);
        append_le(b, ch.sync_type);
        append_le(b, ch.data_type);
        append_le(b, uint8_t{0});    // cn_bit_offset
        append_le(b, ch.byte_offset);
        append_le(b, ch.bit_count);
        append_le(b, uint32_t{0});   // cn_flags: ranges and limits not valid
        append_le(b, uint32_t{0});   // cn_inval_bit_pos
        append_le(b, uint8_t{0});    // cn_precision
        append_le(b, uint8_t{0});    // cn_reserved
        append_le(b, uint16_t{0});   // cn_attachment_count
        for (int k = 0; k < 6; ++k)
        {
            append_le(b, 0.0);       // value range, limit, extended limit
        }
        text_block("##TX", ch.name);
        if (!ch.unit.empty())
        {
            text_block("##TX", ch.unit);
        }
    }

    // ===== DTBLOCK =====
    layout.dt_length_at = b.size() - base + 8;
    block_header("##DT", 24 + (unfinalized ? 0 : record_count * mdf_record_size), 0);
    return layout;
}

void append_mdf_record(std::vector<uint8_t>& r, const BusMessage& m, int64_t start_ns)
{
    const uint8_t len = std::min<uint8_t>(m.len, 64);
    // canid_t as in <linux/can.h>: standard and extended frames with the same
    // number stay distinct, error frames keep their error class.
    // Microsecond resolution, as the trace timestamps always had.
    append_le(r, static_cast<double>(m.ts_ns / 1000 - start_ns / 1000) / 1e6);
    append_le(r, socket_can::frame_id(m));
    append_le(r, len);
    append_le(r, uint8_t{has_flag(m, bus_flag::tx) ? uint8_t{1} : uint8_t{0}});
    r.insert(r.end(), m.data.begin(), m.data.begin() + len);
    r.resize(r.size() + 64 - len);
}

bool mdf_finalize(std::ostream& out, const MdfLayout& layout, uint64_t record_count)
{
    std::vector<uint8_t> b;
    const auto patch = [&](uint64_t at, auto value)
    {
        b.clear();
        append_le(b, value);
        out.seekp(static_cast<std::streamoff>(at));
        write_bytes(out, b);
    };
    const auto end = out.tellp();
    out.seekp(0);
    out.write("MDF     ", 8);
    patch(60, uint16_t{0}); // id_unfin_flags
    patch(layout.cycle_count_at, record_count);
    patch(layout.dt_length_at, uint64_t{24} + record_count * mdf_record_size);
    out.seekp(end);
    return static_cast<bool>(out);
}

void write_trace_file(std::ostream& out, TraceFileFormat format, std::span<const BusMessage> messages,
                      const IfaceNameFn& iface_name)
{
    write_frames(out, format, messages, iface_name);
}

bool write_trace_file(std::ostream& out, TraceFileFormat format, std::span<const FrameCacheRec> recs,
                      std::span<const FrameCachePayload> overflow, const IfaceNameFn& iface_name,
                      std::atomic<uint64_t>* done, const std::stop_token& stop)
{
    // Every writer but candump and pcap reads the frames twice (start time, sizes or interfaces
    // first): progress counts reads, reported as frames.
    const uint64_t passes = format == TraceFileFormat::CanDump || format == TraceFileFormat::Pcap ? 1 : 2;
    // Text formats decode on several threads (write_lines): each thread counts its own reads and adds
    // them in batches. The binary writers read on this thread only and keep a plain counter, which
    // measured faster (thread_local per frame cost them ~50 %).
    std::atomic<uint64_t> reads{0};
    const auto batch = [&]
    {
        if (stop.stop_requested())
        {
            throw WriteStopped{}; // out of the writer's loop without a stop check in each one
        }
        const uint64_t total = reads.fetch_add(0x10000, std::memory_order_relaxed) + 0x10000;
        if (done != nullptr)
        {
            done->store(std::min<uint64_t>(total / passes, recs.size()), std::memory_order_relaxed);
        }
    };
    const bool text = format == TraceFileFormat::CanDump || format == TraceFileFormat::VectorAsc || format == TraceFileFormat::Trc;
    try
    {
        if (text)
        {
            write_frames(out, format, recs | std::views::transform([&](const FrameCacheRec& r)
            {
                static thread_local uint32_t mine = 0;
                if (++mine == 0x10000)
                {
                    mine = 0;
                    batch();
                }
                return frame_cache_decode(r, overflow);
            }), iface_name);
        }
        else
        {
            uint32_t mine = 0;
            write_frames(out, format, recs | std::views::transform([&](const FrameCacheRec& r)
            {
                if (++mine == 0x10000)
                {
                    mine = 0;
                    batch();
                }
                return frame_cache_decode(r, overflow);
            }), iface_name);
        }
    }
    catch (const WriteStopped&)
    {
        return false;
    }
    if (done != nullptr)
    {
        done->store(recs.size(), std::memory_order_relaxed);
    }
    return true;
}
