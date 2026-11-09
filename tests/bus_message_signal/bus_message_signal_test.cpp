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

// Regression tests for BusMessage signal packing (see issue #34).
//
// Every expected value below was produced by cantools (an independent DBC
// implementation), not by Kraken Explorer itself. That matters: the big-endian bug in
// issue #34 survived for so long precisely because extract and inject were
// wrong in mutually cancelling ways, so any round-trip test written against
// Kraken Explorer alone passed. Regenerate these vectors with cantools if you ever
// need to extend the table -- never from Kraken Explorer output.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/bus_message.h"

namespace
{

struct SignalCase
{
    const char* name;
    uint16_t dbc_start_bit;   // start bit exactly as written in the DBC
    uint16_t length;
    bool big_endian;
    uint8_t dlc;
    const char* payload_hex;  // frame contents for the extract direction
    uint64_t expected_raw;    // value cantools decodes from payload_hex
    const char* injected_hex; // bytes cantools encodes into an all-zero frame
};

// clang-format off
constexpr SignalCase cases[] = {
    // Signals from the issue #34 reproducer DBC (reported by B-2U).
    { "RollingCounter", 11, 4, true, 32,
      "B399F42C41882B08BB90668F970F2C8A5491978077D15B12C4843FC14BF25C32",
      9ULL,
      "0009000000000000000000000000000000000000000000000000000000000000" },   // unaligned nibble, the reported symptom
    // More payloads for the reported signal: a single frame is not enough,
    // because for some payloads the old algorithm landed on the right value by
    // luck. Each of these three decoded wrongly before the fix.
    { "RollingCounter_b", 11, 4, true, 32,
      "676133992D3A3F22C21640BA6287AFB38916F09F7D336BB89C9637CAE6C95FD2",
      1ULL,
      "0001000000000000000000000000000000000000000000000000000000000000" },   // old code said 6
    { "RollingCounter_c", 11, 4, true, 32,
      "63DCAE362676A92DD9915615F2B787ADC6167850670A448E016C345CFDEE8814",
      12ULL,
      "000C000000000000000000000000000000000000000000000000000000000000" },   // old code said 13
    { "RollingCounter_d", 11, 4, true, 32,
      "7684E6329B76FA0D5B2137F3EBB729BFCA7198ECFB2DF180A92D091C052B79AF",
      4ULL,
      "0004000000000000000000000000000000000000000000000000000000000000" },   // old code said 8
    { "ChecksumByte", 7, 8, true, 32,
      "3967B4473E11B1107A61643A6AF8D4CF65ADDFB57C89A8710BC25819B31FF120",
      57ULL,
      "3900000000000000000000000000000000000000000000000000000000000000" },   // byte aligned
    { "StatusBits", 23, 2, true, 32,
      "453E425A957D1F2F45005A99A2B8ACA1127C5ECAD2A9B36E155A50BA7562D3F5",
      1ULL,
      "0000400000000000000000000000000000000000000000000000000000000000" },   // 2 bits at a byte end
    { "Field_18", 18, 3, true, 32,
      "5539D1949E87E9660BC29AC89BCDB0444764C5FE33D66580663999F5BDE3F2C7",
      1ULL,
      "0000010000000000000000000000000000000000000000000000000000000000" },   // 3 bits mid-byte
    { "Field_39", 39, 32, true, 32,
      "E1486FCFA0A1EE406317BF23CB0277A2F719D93A63EBF0F9D3811DC51CFE7AB1",
      2694966848ULL,
      "00000000A0A1EE40000000000000000000000000000000000000000000000000" },   // 32 bits, byte aligned
    { "Field_71", 71, 32, true, 32,
      "239B9779B90044719A46A71B61717452D0C931C9B151476E3BE0EFC4FA73609B",
      2588321563ULL,
      "00000000000000009A46A71B0000000000000000000000000000000000000000" },   // 32 bits, byte aligned, later in frame

    // Big-endian edge cases.
    { "be_1bit_msb", 7, 1, true, 8,
      "DCD64DBB1C825418",
      1ULL,
      "8000000000000000" },   // single MSB of byte 0
    { "be_1bit_lsb", 0, 1, true, 8,
      "D9E9198305137859",
      1ULL,
      "0100000000000000" },   // single LSB of byte 0
    { "be_cross2", 3, 10, true, 8,
      "4A5918251D5A65AC",
      662ULL,
      "0A58000000000000" },   // crosses a byte boundary unaligned
    { "be_cross3", 2, 20, true, 8,
      "550BE1F3531CBCB1",
      661443ULL,
      "050BE18000000000" },   // spans three bytes unaligned
    { "be_full64", 7, 64, true, 8,
      "EC2CECA9CFAC01BC",
      17018237306004046268ULL,
      "EC2CECA9CFAC01BC" },   // full 64-bit signal
    { "be_63", 6, 63, true, 8,
      "37AB0CB5FE845C88",
      4011313868902259848ULL,
      "37AB0CB5FE845C88" },   // 63 bits, unaligned start
    { "be_tail", 52, 9, true, 8,
      "ACDA971D732ABA55",
      421ULL,
      "0000000000001A50" },   // ends near the end of the frame

    // CAN FD: past the first 8 bytes, where the old 64-bit sliding window sat.
    { "be_fd_mid", 207, 24, true, 64,
      "60423BDE17B7294BB9B46D1F3A1798EB8C53D70E0E55640D5292B752EDFADF3C16F765D64A3168FEA58114A3AC4D7EDA81358F21522D7ED6499D7F9C3968F35F",
      9615186ULL,
      "0000000000000000000000000000000000000000000000000092B752000000000000000000000000000000000000000000000000000000000000000000000000" },   // starts in byte 25
    { "be_fd_unaligned", 250, 17, true, 64,
      "5C2BF2ED1C522225F8B32BD8853675BC3135269317155A968966336EB41E0081D712E8582A78219543B5EFC8E7CE9DA5686B4254BC59630306DB9B4D0011201F",
      30148ULL,
      "0000000000000000000000000000000000000000000000000000000000000001D710000000000000000000000000000000000000000000000000000000000000" },   // unaligned, crosses bytes 31-33
    { "be_fd_last", 499, 12, true, 64,
      "710F00360D781DEC24DFCFB5E20A23E1D4559A6BFFFE37058DD2B60067FCF38A615930944FD6D1917E89B83023B06993443FBE3C8DC26A91ADF2F09CE1FEDB00",
      2816ULL,
      "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000B00" },   // ends on the frame's last bit

    // Little-endian guards: the issue #34 fix must not disturb the Intel path.
    { "le_nibble", 11, 4, false, 8,
      "1D21A08AC364AC61",
      4ULL,
      "0020000000000000" },   // Intel nibble
    { "le_16", 4, 16, false, 8,
      "AB15A19C687FE0AE",
      4442ULL,
      "A015010000000000" },   // Intel 16-bit unaligned
    { "le_full64", 0, 64, false, 8,
      "8216551662096CEB",
      16963944213283935874ULL,
      "8216551662096CEB" },   // Intel full 64-bit
    { "le_fd", 300, 24, false, 64,
      "E51D778B5A32665BDDE0B1C88679D8EE21834C99A8DB0E595E3E57F51D9CD659A715EA0D5B1778E018D35065B969E8B228C5C77BA431FE8C2DDA7C5D4609291D",
      9308033ULL,
      "000000000000000000000000000000000000000000000000000000000000000000000000001078E0080000000000000000000000000000000000000000000000" },   // Intel, FD, beyond byte 8
};
// clang-format on

// Mirrors the Motorola start-bit conversion in the DBC parser. BusMessage
// receives the converted value, so the tests must apply the same conversion to
// stay faithful to the real path. Keep in sync if the parser convention changes.
[[nodiscard]] uint16_t to_internal_start_bit(uint16_t dbc_start_bit, bool big_endian) noexcept
{
    if (!big_endian) { return dbc_start_bit; }

    const uint16_t row = dbc_start_bit >> 3;
    const uint16_t column = dbc_start_bit & 0b111;
    return static_cast<uint16_t>((row * 8) + (7 - column));
}

[[nodiscard]] std::vector<uint8_t> from_hex(std::string_view hex)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

[[nodiscard]] BusMessage make_message(const SignalCase& c, const std::vector<uint8_t>& data)
{
    BusMessage m;
    set_length(m, c.dlc);
    std::copy(data.begin(), data.end(), m.data.begin());
    return m;
}

[[nodiscard]] std::string payload_hex(const BusMessage& m)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (int i = 0; i < m.len; ++i)
    {
        out += digits[m.data[i] >> 4];
        out += digits[m.data[i] & 0x0F];
    }
    return out;
}

} // namespace

// Decoding a known frame must produce the value cantools decodes.
TEST_CASE("extract matches cantools")
{
    for (const auto& c : cases)
    {
        CAPTURE(c.name);
        const BusMessage m = make_message(c, from_hex(c.payload_hex));
        CHECK(extract_raw_signal(m, to_internal_start_bit(c.dbc_start_bit, c.big_endian), c.length, c.big_endian)
              == c.expected_raw);
    }
}

// Encoding the same value into a zeroed frame must produce the bytes cantools
// encodes -- this is what pins down bit order independently of extraction.
TEST_CASE("inject matches cantools")
{
    for (const auto& c : cases)
    {
        CAPTURE(c.name);
        BusMessage m = make_message(c, {});
        inject_raw_signal(m, to_internal_start_bit(c.dbc_start_bit, c.big_endian), c.length, c.big_endian, c.expected_raw);
        CHECK(payload_hex(m) == c.injected_hex);
    }
}

// Weaker than the two above (it passed even with the issue #34 bug), but it
// still catches asymmetry between the two directions.
TEST_CASE("inject then extract round trips")
{
    for (const auto& c : cases)
    {
        CAPTURE(c.name);
        const uint16_t start = to_internal_start_bit(c.dbc_start_bit, c.big_endian);
        BusMessage m = make_message(c, from_hex(c.payload_hex));
        inject_raw_signal(m, start, c.length, c.big_endian, c.expected_raw);
        CHECK(extract_raw_signal(m, start, c.length, c.big_endian) == c.expected_raw);
    }
}

// Injecting must be a surgical bit operation: writing a signal must not disturb
// bits outside it, whatever the neighbouring payload looks like.
TEST_CASE("inject leaves other bits untouched")
{
    for (const auto& c : cases)
    {
        CAPTURE(c.name);
        const uint16_t start = to_internal_start_bit(c.dbc_start_bit, c.big_endian);

        // Locate the signal's bits: a bit belongs to the signal iff it ends up
        // identical whether the frame started as all zeros or all ones.
        BusMessage all_zero = make_message(c, {});
        BusMessage all_ones = make_message(c, std::vector<uint8_t>(c.dlc, 0xFF));
        inject_raw_signal(all_zero, start, c.length, c.big_endian, c.expected_raw);
        inject_raw_signal(all_ones, start, c.length, c.big_endian, c.expected_raw);

        // Write over the original payload and over its bitwise complement.
        const std::vector<uint8_t> payload = from_hex(c.payload_hex);
        std::vector<uint8_t> inverted = payload;
        for (auto& b : inverted) { b = static_cast<uint8_t>(~b); }

        for (const auto& background : { payload, inverted })
        {
            BusMessage m = make_message(c, background);
            inject_raw_signal(m, start, c.length, c.big_endian, c.expected_raw);

            for (int i = 0; i < c.dlc; ++i)
            {
                CAPTURE(i);
                const auto outside = static_cast<uint8_t>(all_zero.data[i] ^ all_ones.data[i]);
                CHECK((m.data[i] & outside) == (background[i] & outside));
            }
        }
    }
}

// A malformed DBC must not push the bit loops out of range.
TEST_CASE("invalid length and start bit are rejected")
{
    BusMessage m;
    set_length(m, 8);
    m.data.fill(0xA5);

    for (const uint16_t length : { uint16_t(0), uint16_t(65), uint16_t(1000) })
    {
        CAPTURE(length);
        CHECK(extract_raw_signal(m, 0, length, true) == 0);
        CHECK(extract_raw_signal(m, 0, length, false) == 0);

        BusMessage copy = m;
        inject_raw_signal(copy, 0, length, true, ~0ULL);
        inject_raw_signal(copy, 0, length, false, ~0ULL);
        CHECK(copy.data == m.data);
    }

    CHECK(extract_raw_signal(m, 512, 8, true) == 0);
    CHECK(extract_raw_signal(m, 512, 8, false) == 0);
}

// inject_raw_signal must not write outside the frame's declared length.
TEST_CASE("inject clips at len")
{
    for (const bool big_endian : { true, false })
    {
        CAPTURE(big_endian);
        BusMessage m;
        set_length(m, 2);
        m.data[2] = 0x11;   // beyond len

        // A 32-bit signal from byte 0 spans bytes 0..3 in either byte order.
        inject_raw_signal(m, to_internal_start_bit(big_endian ? 7 : 0, big_endian), 32, big_endian, 0xFFFFFFFFULL);

        CHECK(m.data[0] == 0xFF);
        CHECK(m.data[1] == 0xFF);
        CHECK(m.data[2] == 0x11);   // untouched
        CHECK(m.data[3] == 0x00);
    }
}

// An independent oracle: the signal's bits placed one by one. Little-endian (Intel): value bit i
// is stream bit start+i, LSB first within a byte. Big-endian (Motorola): the internal start bit is
// the MSB's position in an MSB-first stream, value bit length-1-i is stream bit start+i.
namespace
{

void oracle_inject(std::array<uint8_t, 64>& data, uint16_t start, uint16_t length, bool big_endian, uint64_t value)
{
    for (uint16_t i = 0; i < length; ++i)
    {
        const unsigned pos = start + i;
        const unsigned bit = big_endian ? 7 - pos % 8 : pos % 8;
        const unsigned value_bit = big_endian ? length - 1 - i : i;
        const bool one = (value >> value_bit) & 1ULL;
        data[pos / 8] = static_cast<uint8_t>(one ? data[pos / 8] | (1u << bit) : data[pos / 8] & ~(1u << bit));
    }
}

uint64_t oracle_extract(const std::array<uint8_t, 64>& data, uint16_t start, uint16_t length, bool big_endian)
{
    uint64_t value = 0;
    for (uint16_t i = 0; i < length; ++i)
    {
        const unsigned pos = start + i;
        const unsigned bit = big_endian ? 7 - pos % 8 : pos % 8;
        const unsigned value_bit = big_endian ? length - 1 - i : i;
        value |= static_cast<uint64_t>((data[pos / 8] >> bit) & 1u) << value_bit;
    }
    return value;
}

} // namespace

TEST_CASE("every start bit and a spread of lengths, both byte orders: inject and extract agree with a bit-by-bit oracle")
{
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    const auto next = [&]
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    int checked = 0;
    for (const bool big_endian : {false, true})
    {
        for (const uint16_t length : {1, 2, 3, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 48, 63, 64})
        {
            for (uint16_t start = 0; start + length <= 512; ++start)
            {
                BusMessage m;
                set_length(m, 64);
                for (auto& b : m.data)
                {
                    b = static_cast<uint8_t>(next());
                }
                const uint64_t mask = length == 64 ? ~0ULL : (1ULL << length) - 1;
                const uint64_t value = next() & mask;
                std::array<uint8_t, 64> expected = m.data;
                oracle_inject(expected, start, length, big_endian, value);
                inject_raw_signal(m, start, length, big_endian, value);
                if (m.data != expected || extract_raw_signal(m, start, length, big_endian) != value
                    || oracle_extract(m.data, start, length, big_endian) != value)
                {
                    CAPTURE(big_endian);
                    CAPTURE(length);
                    CAPTURE(start);
                    CHECK(m.data == expected);
                    CHECK(extract_raw_signal(m, start, length, big_endian) == value);
                }
                ++checked;
            }
        }
    }
    CHECK(checked > 10000);
}
