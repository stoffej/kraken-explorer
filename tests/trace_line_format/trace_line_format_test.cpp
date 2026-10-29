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

// Text trace lines shared by the trace export and the TraceRecorder, and the ASC
// CAN FD parser used by the replay window.
//
// The CAN FD writer used to emit "<flags> 0 0 <len> <len>" instead of the Vector
// "<BRS> <ESI> <DLC> <len>". It survived because our own replay parser
// read exactly that wrong layout back. So the reference lines here are verbatim
// python-can 4.6.1 output (can.ASCWriter / can.CanutilsLogWriter), and the DLC
// table is ISO 11898-1:2015 Table 5 -- never Kraken Explorer output.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "core/bus_message.h"
#include "core/trace_line_format.h"

namespace
{

std::vector<std::string_view> tokens(std::string_view line)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) { ++i; }
        const size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) { ++i; }
        if (i > start)
        {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// First `count` tokens joined by single spaces, upper-cased.
std::string normalized(std::string_view line, size_t count = SIZE_MAX)
{
    const auto parts = tokens(line);
    std::string out;
    for (size_t i = 0; i < parts.size() && i < count; ++i)
    {
        if (i) { out += ' '; }
        out += parts[i];
    }
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::vector<uint8_t> sequence(int count)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(i));
    }
    return bytes;
}

std::string repeated(std::string_view s, int n, std::string_view sep = "")
{
    std::string out;
    for (int i = 0; i < n; ++i)
    {
        if (i) { out += sep; }
        out += s;
    }
    return out;
}

BusMessage fd_message(uint32_t id, bool extended, bool brs, bool rx, int64_t timestamp_us, const std::vector<uint8_t>& data)
{
    BusMessage m{ .id = id, .flags = bus_flag::fd, .ts_ns = timestamp_us * 1000 };
    if (extended) { m.flags |= bus_flag::extended; }
    if (brs)      { m.flags |= bus_flag::brs; }
    if (!rx)      { m.flags |= bus_flag::tx; }
    set_length(m, static_cast<int>(data.size()));
    std::ranges::copy(data, m.data.begin());
    return m;
}

// python-can can.ASCWriter, verbatim, for:
//   Message(timestamp=0.00, arbitration_id=0x456,      is_fd, bitrate_switch, channel=1, rx, data=00..0B)
//   Message(timestamp=0.01, arbitration_id=0x18DAF110, is_fd, extended,       channel=0, tx, data=A5 x 64)
//   Message(timestamp=0.02, arbitration_id=0x7E0,      is_fd, bitrate_switch, channel=0, rx, data=01 02 03)
// (python-can channels are 0-based, ASC channels 1-based.)
const std::string asc_fd_12 =
    " 0.000000 CANFD   2 Rx        456                                   1 0 9 12 "
    "00 01 02 03 04 05 06 07 08 09 0A 0B        0    0     3000        0        0        0        0        0";
const std::string asc_fd_64 = " 0.010000 CANFD   1 Tx   18DAF110x                                   0 0 f 64 "
    + repeated("A5", 64, " ")
    + "        0    0     1000        0        0        0        0        0";
const std::string asc_fd_3 =
    " 0.020000 CANFD   1 Rx        7E0                                   1 0 3  3 "
    "01 02 03        0    0     3000        0        0        0        0        0";

}


TEST_CASE("CAN FD length to DLC rounds up")
{
    struct Row { int length; int dlc; };
    constexpr Row rows[] = {
        { 0, 0 }, { 8, 8 }, { 12, 9 }, { 13, 10 }, { 20, 11 }, { 24, 12 }, { 25, 13 }, { 48, 14 }, { 49, 15 }, { 64, 15 },
    };
    for (const Row& r : rows)
    {
        CAPTURE(r.length);
        CHECK(bus_length_to_dlc(r.length) == r.dlc);
    }
}

TEST_CASE("ASC CAN FD line matches python-can")
{
    struct Row
    {
        const char* name;
        const std::string& reference;
        uint32_t id;
        bool extended, brs, rx;
        int channel;
        int64_t timestamp_us;
        std::vector<uint8_t> data;
    };
    const Row rows[] = {
        { "12 bytes, BRS", asc_fd_12, 0x456, false, true, true, 2, 0, sequence(12) },
        { "64 bytes, extended, TX", asc_fd_64, 0x18DAF110, true, false, false, 1, 10000, std::vector<uint8_t>(64, 0xA5) },
        { "3 bytes", asc_fd_3, 0x7E0, false, true, true, 1, 20000, { 1, 2, 3 } },
    };
    for (const Row& r : rows)
    {
        SUBCASE(r.name)
        {
            const BusMessage m = fd_message(r.id, r.extended, r.brs, r.rx, r.timestamp_us, r.data);
            std::string line;
            append_asc_line(line, m, 0, r.channel);
            // time CANFD channel dir id BRS ESI DLC length data...; python-can appends
            // optional duration/CRC/bit-timing fields after the data, which Kraken Explorer omits.
            CHECK(normalized(line) == normalized(r.reference, 9 + r.data.size()));
        }
    }
}

TEST_CASE("ASC remote frame line matches python-can: DLC, no data (T87b a1 F7)")
{
    // python-can 4.6 can.ASCWriter for Message(timestamp=0, arbitration_id=0x124, is_remote_frame=True,
    // dlc=3, channel=0), verbatim; Kraken Explorer adds the "Length = ..." suffix after it.
    const std::string reference = " 0.000000 1  124             Rx   r 3 ";
    BusMessage m{ .id = 0x124, .flags = bus_flag::rtr };
    set_length(m, 3);
    std::ranges::fill(m.data, 0xAA); // must not leak into the line
    std::string line;
    append_asc_line(line, m, 0, 1);
    CHECK(normalized(line.substr(0, line.find("Length"))) == normalized(reference));
}

TEST_CASE("candump CAN FD line matches python-can")
{
    struct Row
    {
        const char* name;
        std::string reference;
        const char* iface;
        uint32_t id;
        bool extended, brs;
        std::vector<uint8_t> data;
    };
    // python-can can.CanutilsLogWriter; its trailing R/T direction marker is not
    // part of the candump -L format and is ignored.
    const Row rows[] = {
        { "12 bytes, BRS", "(0.000000) can1 456##1000102030405060708090A0B R", "can1", 0x456, false, true, sequence(12) },
        { "64 bytes, extended", "(0.000000) can0 18DAF110##0" + repeated("A5", 64) + " T", "can0", 0x18DAF110, true, false,
          std::vector<uint8_t>(64, 0xA5) },
        { "3 bytes", "(0.000000) can0 7E0##1010203 R", "can0", 0x7E0, false, true, { 1, 2, 3 } },
    };
    for (const Row& r : rows)
    {
        SUBCASE(r.name)
        {
            const BusMessage m = fd_message(r.id, r.extended, r.brs, true, 0, r.data);
            std::string line;
            append_candump_line(line, m, r.iface);
            auto reference = tokens(r.reference);
            reference.resize(3);
            CHECK(tokens(line) == reference);
        }
    }
}

TEST_CASE("candump error frame per linux/can/error.h")
{
    struct Row { const char* name; uint16_t errors; const char* expected; };
    // can_id = CAN_ERR_FLAG | error classes, 8-byte payload, per <linux/can/error.h>.
    // python-can's CanutilsLogWriter writes an unclassified error as 20000080#.
    constexpr Row rows[] = {
        { "generic", bus_error::generic, "(0.000000) can0 20000080#0000000000000000" },
        { "bus off: CAN_ERR_BUSOFF", bus_error::bus_off, "(0.000000) can0 20000040#0000000000000000" },
        { "restarted: CAN_ERR_RESTARTED", bus_error::restarted, "(0.000000) can0 20000100#0000000000000000" },
        { "warning: CAN_ERR_CRTL, data[1] RX_WARNING | TX_WARNING", bus_error::error_warning,
          "(0.000000) can0 20000004#000C000000000000" },
        { "passive: CAN_ERR_CRTL, data[1] RX_PASSIVE | TX_PASSIVE", bus_error::error_passive,
          "(0.000000) can0 20000004#0030000000000000" },
        { "active: CAN_ERR_CRTL, data[1] CAN_ERR_CRTL_ACTIVE", bus_error::error_active,
          "(0.000000) can0 20000004#0040000000000000" },
        { "stuff: CAN_ERR_PROT | BUSERROR, data[2] CAN_ERR_PROT_STUFF", bus_error::stuff,
          "(0.000000) can0 20000088#0000040000000000" },
        { "crc: CAN_ERR_PROT | BUSERROR, data[3] CAN_ERR_PROT_LOC_CRC_SEQ", bus_error::crc,
          "(0.000000) can0 20000088#0000000800000000" },
        { "overrun: CAN_ERR_CRTL, data[1] CAN_ERR_CRTL_RX_OVERFLOW", bus_error::overrun,
          "(0.000000) can0 20000004#0001000000000000" },
        { "ack + bit: CAN_ERR_ACK | PROT | BUSERROR, data[2] CAN_ERR_PROT_BIT", bus_error::ack | bus_error::bit,
          "(0.000000) can0 200000A8#0000010000000000" },
        { "tx timeout: CAN_ERR_TX_TIMEOUT", bus_error::tx_timeout, "(0.000000) can0 20000001#0000000000000000" },
    };
    for (const Row& r : rows)
    {
        CAPTURE(r.name);
        const BusMessage m{ .errors = r.errors };
        REQUIRE(is_error_frame(m));
        std::string line;
        append_candump_line(line, m, "can0");
        CHECK(line == r.expected);
    }
}

TEST_CASE("candump timestamp is exact seconds.microseconds")
{
    const BusMessage m{ .id = 0x123, .ts_ns = 1700000000123456789LL };
    std::string line;
    append_candump_line(line, m, "vcan0");
    CHECK(line == "(1700000000.123456) vcan0 123#");
}

TEST_CASE("candump interface names keep one field")
{
    const BusMessage m{ .id = 0x123 };
    std::string line;
    append_candump_line(line, m, "PCAN-USB (USB1)");
    CHECK(line == "(0.000000) PCAN-USB_(USB1) 123#");
}

TEST_CASE("parse ASC CAN FD line")
{
    struct Row
    {
        const char* name;
        std::string line;
        uint32_t id;
        bool extended, brs, rx;
        int channel;
        std::vector<uint8_t> data;
    };
    const Row rows[] = {
        { "python-can 12 bytes", asc_fd_12, 0x456, false, true, true, 2, sequence(12) },
        { "python-can 64 bytes", asc_fd_64, 0x18DAF110, true, false, false, 1, std::vector<uint8_t>(64, 0xA5) },
        { "python-can 3 bytes", asc_fd_3, 0x7E0, false, true, true, 1, { 1, 2, 3 } },
        // Vector allows a symbolic frame name between id and BRS.
        { "symbolic name", " 0.020000 CANFD   1 Rx        7E0  EngineData  1 0 3  3 01 02 03", 0x7E0, false, true, true, 1,
          { 1, 2, 3 } },
        // Written by older versions before the writer was fixed; existing traces must keep loading.
        { "legacy layout",
          "   0.030000 CANFD   2 Rx             456 1 0 0 12 12 00 01 02 03 04 05 06 07 08 09 0A 0B ", 0x456, false, true,
          true, 2, sequence(12) },
    };
    for (const Row& r : rows)
    {
        SUBCASE(r.name)
        {
            BusMessage m;
            REQUIRE(parse_asc_canfd_line(tokens(r.line), m));
            CHECK(has_flag(m, bus_flag::fd));
            CHECK(m.id == r.id);
            CHECK(has_flag(m, bus_flag::extended) == r.extended);
            CHECK(has_flag(m, bus_flag::brs) == r.brs);
            CHECK(!has_flag(m, bus_flag::tx) == r.rx);
            CHECK(m.iface == r.channel);
            REQUIRE(m.len == r.data.size());
            CHECK(std::equal(r.data.begin(), r.data.end(), m.data.begin()));
        }
    }
}

TEST_CASE("parse ASC CAN FD rejects malformed lines")
{
    const std::string twelve_bytes = "00 01 02 03 04 05 06 07 08 09 0A 0B";
    const std::string lines[] = {
        " 0.0 CANFD 1 Rx 456 1 0 8 12 " + twelve_bytes,   // DLC disagrees with length
        " 0.0 CANFD 1 Rx 456 1 0 9 12 00 01 02",           // data truncated
        " 0.0 CANFD 1 Rx zz 1 0 3 3 01 02 03",             // bad id
        " 0.0 CANFD 1 Rx 456 1 0 3 3 01 1FF 03",           // byte out of range
        " 0.0 CANFD 1 Rx",                                 // too short
        "   0.000000 1  123             Rx   d 8 11 22 33 44 55 66 77 88",  // classic CAN line
    };
    for (const std::string& line : lines)
    {
        CAPTURE(line);
        BusMessage m;
        CHECK_FALSE(parse_asc_canfd_line(tokens(line), m));
    }
}

// PEAK .trc. Expected values are python-can 4.6.1 can.TRCReader output for the
// same lines (;$STARTTIME=45000.5 -> 1678881600 s); reference lines are verbatim
// can.TRCWriter output, except that PEAK numbers messages from 1, python-can from 0.
namespace
{

constexpr int64_t trc_start_ns = 1678881600LL * 1000000000LL;

TrcLayout trc_layout(std::initializer_list<std::string_view> header)
{
    TrcLayout layout;
    for (const std::string_view line : header)
    {
        parse_trc_header_line(line, layout);
    }
    return layout;
}

struct TrcRow
{
    const char* line;
    int64_t offset_ns;
    uint32_t id;
    uint16_t flags;
    int bus;
    std::vector<uint8_t> data;  // RTR: length only
};

void check_trc(const TrcLayout& layout, const TrcRow& r)
{
    CAPTURE(r.line);
    BusMessage m;
    REQUIRE(parse_trc_line(tokens(r.line), layout, m));
    CHECK(m.ts_ns == trc_start_ns + r.offset_ns);
    CHECK(m.id == r.id);
    CHECK(m.flags == r.flags);
    CHECK(m.iface == r.bus);
    CHECK(!is_error_frame(m));
    REQUIRE(m.len == r.data.size());
    CHECK(std::equal(r.data.begin(), r.data.end(), m.data.begin()));
}

}

TEST_CASE("parse TRC 1.1 lines")
{
    using namespace bus_flag;
    const TrcLayout layout = trc_layout({ ";$FILEVERSION=1.1\r", ";$STARTTIME=45000.5\r" });
    CHECK(layout.start_ns == trc_start_ns);
    const TrcRow rows[] = {
        { "     1)      1059.9  Rx         0300  7  00 00 00 00 04 00 00", 1059900000, 0x300, 0, 1, { 0, 0, 0, 0, 4, 0, 0 } },
        { "     2)      1283.2  Tx     18EFC867  8  01 02 03 04 05 06 07 08", 1283200000, 0x18EFC867, extended | tx, 1,
          { 1, 2, 3, 4, 5, 6, 7, 8 } },
        { "     3)      1500.0  Rx         0400  4  RTR", 1500000000, 0x400, rtr, 1, { 0, 0, 0, 0 } },
    };
    for (const TrcRow& r : rows)
    {
        check_trc(layout, r);
    }
    BusMessage m;
    CHECK_FALSE(parse_trc_line(tokens("    11)     21017.0  Warng  FFFFFFFF  4  00 00 00 08  BUSHEAVY"), layout, m));
}

TEST_CASE("parse TRC 2.0 lines")
{
    using namespace bus_flag;
    const TrcLayout layout = trc_layout({ ";$FILEVERSION=2.0", ";$STARTTIME=45000.5", ";$COLUMNS=N,O,T,I,d,l,D" });
    const TrcRow rows[] = {
        { "      1      1059.900 DT     0300 Rx 7  00 00 00 00 04 00 00", 1059900000, 0x300, 0, 1, { 0, 0, 0, 0, 4, 0, 0 } },
        { "      2      1753.227 FD     0400 Rx 12  01 02 03 04 05 06 07 08 09 0A 0B 0C", 1753227000, 0x400, fd, 1,
          { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 } },
        { "      3      1800.001 FB 18EFC867 Tx 3  AA BB CC", 1800001000, 0x18EFC867, extended | tx | fd | brs, 1,
          { 0xAA, 0xBB, 0xCC } },
        { "      4      1900.000 RR     0123 Rx 2", 1900000000, 0x123, rtr, 1, { 0, 0 } },
    };
    for (const TrcRow& r : rows)
    {
        check_trc(layout, r);
    }
}

TEST_CASE("parse TRC 2.1 lines")
{
    using namespace bus_flag;
    const TrcLayout layout = trc_layout({ ";$FILEVERSION=2.1", ";$STARTTIME=45000.5", ";$COLUMNS=N,O,T,B,I,d,R,L,D" });
    const TrcRow rows[] = {
        { "      1      1059.900 DT 1     0300 Rx -  7    00 00 00 00 04 00 00", 1059900000, 0x300, 0, 1,
          { 0, 0, 0, 0, 4, 0, 0 } },
        // L is the DLC code: 9 = 12 bytes.
        { "      2      1753.227 FB 2     0400 Tx -  9    01 02 03 04 05 06 07 08 09 0A 0B 0C", 1753227000, 0x400,
          tx | fd | brs, 2, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 } },
        // FE / BI: ESI set (not tracked by BusMessage).
        { "      3      1800.001 FE 1 18EFC867 Rx -  3    AA BB CC", 1800001000, 0x18EFC867, extended | fd | brs, 1,
          { 0xAA, 0xBB, 0xCC } },
        { "      4      1900.000 RR 2     0123 Rx -  2", 1900000000, 0x123, rtr, 2, { 0, 0 } },
    };
    for (const TrcRow& r : rows)
    {
        check_trc(layout, r);
    }

    // Error frames: python-can skips ER lines; per the PEAK TRC spec they carry
    // error type, direction, position and the two error counters.
    BusMessage m;
    REQUIRE(parse_trc_line(tokens("      5      2000.000 ER 1     -    Rx -  5    04 00 08 00 00"), layout, m));
    CHECK(is_error_frame(m));
    CHECK(m.ts_ns == trc_start_ns + 2000000000);

    CHECK_FALSE(parse_trc_line(tokens("      6      2100.000 DT 1     0300 Rx -  3    00 01"), layout, m));   // truncated
    CHECK_FALSE(parse_trc_line(tokens("      7      2200.000 ST 1     -    Rx -  4    00 00 00 04"), layout, m)); // status
    CHECK_FALSE(parse_trc_line(tokens("      8      2300.000 DT 1     zz   Rx -  1    00"), layout, m));         // bad id
}

TEST_CASE("TRC line matches python-can")
{
    // can.TRCWriter: "      0         0.000 DT  1     0123 Rx -  3    11 22 33"
    //                "      1        10.500 DT  2 18DAF110 Tx -  3    AA BB CC"
    const int64_t start = 1757000000LL * 1000000000LL;
    BusMessage a{ .id = 0x123, .ts_ns = start };
    set_length(a, 3);
    a.data = { 0x11, 0x22, 0x33 };
    BusMessage b{ .id = 0x18DAF110, .flags = bus_flag::extended | bus_flag::tx, .ts_ns = start + 10500000 };
    set_length(b, 3);
    b.data = { 0xAA, 0xBB, 0xCC };

    std::string line;
    append_trc_line(line, a, 1, start, 1);
    CHECK(line == "      1         0.000 DT  1     0123 Rx -  3    11 22 33");
    line.clear();
    append_trc_line(line, b, 2, start, 2);
    CHECK(line == "      2        10.500 DT  2 18DAF110 Tx -  3    AA BB CC");

    // Header: python-can writes the same three key lines for this start time.
    std::string header;
    append_trc_header(header, start);
    CHECK(header.starts_with(";$FILEVERSION=2.1\r\n;$STARTTIME=45904.648148148146\r\n;$COLUMNS=N,O,T,B,I,d,R,L,D\r\n"));
}
