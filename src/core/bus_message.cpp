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

#include "bus_message.h"

#include <algorithm>
#include <format>
#include <iterator>

uint64_t extract_raw_signal(const BusMessage& m, uint16_t start_bit, uint16_t length, bool big_endian) noexcept
{
    if (length == 0 || length > 64 || start_bit >= bus_max_data_bytes * 8)
    {
        return 0;
    }

    // Big-endian (Motorola): `length` consecutive bits of the MSB-first bit stream
    // starting at start_bit, taken byte by byte from the most significant end.
    // The pre-port shift + conditional bswap64 only agreed with itself (issue #34).
    if (big_endian)
    {
        uint64_t value = 0;
        uint32_t byte_idx = start_bit / 8;
        uint32_t bit_off = start_bit % 8;

        for (uint16_t remaining = length; remaining > 0; byte_idx++, bit_off = 0)
        {
            const uint32_t avail = 8 - bit_off;
            const uint32_t take = std::min<uint32_t>(avail, remaining);
            const uint8_t byte = (byte_idx < bus_max_data_bytes) ? m.data[byte_idx] : 0u;
            const auto chunk = static_cast<uint8_t>((byte >> (avail - take)) & ((1u << take) - 1u));

            value = (value << take) | chunk;
            remaining = static_cast<uint16_t>(remaining - take);
        }
        return value;
    }

    // Little-endian (Intel): up to 8 bytes from start_bit / 8, zero past the buffer. A signal
    // that starts mid-byte and is longer than 64 - shift bits spills into a ninth byte.
    const int byte_offset = start_bit / 8;
    const int shift = start_bit % 8;
    uint64_t raw = 0;
    for (int i = 0; i < 8 && byte_offset + i < bus_max_data_bytes; ++i)
    {
        raw |= static_cast<uint64_t>(m.data[byte_offset + i]) << (8 * i);
    }
    raw >>= shift;
    if (shift != 0 && byte_offset + 8 < bus_max_data_bytes)
    {
        raw |= static_cast<uint64_t>(m.data[byte_offset + 8]) << (64 - shift);
    }

    const uint64_t mask = (length < 64) ? ((1ULL << length) - 1ULL) : ~0ULL;
    return raw & mask;
}

void inject_raw_signal(BusMessage& m, uint16_t start_bit, uint16_t length, bool big_endian, uint64_t value) noexcept
{
    if (length == 0 || length > 64 || start_bit >= bus_max_data_bytes * 8)
    {
        return;
    }

    const uint64_t mask = (length < 64) ? ((1ULL << length) - 1ULL) : ~0ULL;
    value &= mask;

    // Exact inverse of extract_raw_signal's big-endian path.
    if (big_endian)
    {
        uint32_t byte_idx = start_bit / 8;
        uint32_t bit_off = start_bit % 8;

        for (uint16_t remaining = length; remaining > 0; byte_idx++, bit_off = 0)
        {
            const uint32_t avail = 8 - bit_off;
            const uint32_t take = std::min<uint32_t>(avail, remaining);
            remaining = static_cast<uint16_t>(remaining - take);

            if (byte_idx >= m.len)
            {
                continue;
            }

            const uint32_t shift = avail - take;
            const auto chunk = static_cast<uint8_t>((value >> remaining) & ((1u << take) - 1u));
            const auto byte_mask = static_cast<uint8_t>(((1u << take) - 1u) << shift);

            m.data[byte_idx] = static_cast<uint8_t>((m.data[byte_idx] & ~byte_mask) | (chunk << shift));
        }
        return;
    }

    const int byte_offset = start_bit / 8;
    const int bit_shift = start_bit % 8;
    const int count = std::min(8, bus_max_data_bytes - byte_offset);

    uint64_t raw = 0;
    for (int i = 0; i < count; ++i)
    {
        raw |= static_cast<uint64_t>(m.data[byte_offset + i]) << (8 * i);
    }

    raw &= ~(mask << bit_shift);
    raw |= value << bit_shift;

    for (int i = 0; i < count && byte_offset + i < m.len; ++i)
    {
        m.data[byte_offset + i] = static_cast<uint8_t>(raw >> (8 * i));
    }
    // The bits shifted out above the 64-bit word: the top (length + shift - 64) bits of the value,
    // into the low bits of a ninth byte.
    const int spill = length + bit_shift - 64;
    if (spill > 0 && byte_offset + 8 < bus_max_data_bytes && byte_offset + 8 < m.len)
    {
        const auto spill_mask = static_cast<uint8_t>((1u << spill) - 1u);
        const auto spill_value = static_cast<uint8_t>((value >> (64 - bit_shift)) & spill_mask);
        m.data[byte_offset + 8] = static_cast<uint8_t>((m.data[byte_offset + 8] & ~spill_mask) | spill_value);
    }
}

void append_id(std::string& out, const BusMessage& m)
{
    if (m.type == BusType::LIN)
    {
        std::format_to(std::back_inserter(out), "0x{:02X}", m.id & 0x3F);
    }
    else if (has_flag(m, bus_flag::extended))
    {
        std::format_to(std::back_inserter(out), "0x{:08X}", can_id(m));
    }
    else
    {
        std::format_to(std::back_inserter(out), "0x{:03X}", can_id(m));
    }
}

void append_hex_bytes(std::string& out, std::span<const uint8_t> bytes, std::string_view sep)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i > 0)
        {
            out += sep;
        }
        out += hex[bytes[i] >> 4];
        out += hex[bytes[i] & 0x0F];
    }
}

void append_bytes(std::string& out, std::span<const uint8_t> bytes, bool ascii)
{
    if (!ascii)
    {
        append_hex_bytes(out, bytes, " ");
        return;
    }
    for (const uint8_t b : bytes)
    {
        out += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
        out += ' ';
    }
}

void append_error_flags(std::string& out, uint16_t errors)
{
    static constexpr struct { uint16_t flag; const char* name; } names[] = {
        { bus_error::ack,                "ACK"            },
        { bus_error::bit,                "BIT"            },
        { bus_error::stuff,              "STUFF"          },
        { bus_error::crc,                "CRC"            },
        { bus_error::form,               "FORM"           },
        { bus_error::bus_off,            "BUSOFF"         },
        { bus_error::overrun,            "OVERRUN"        },
        { bus_error::tx_timeout,         "TIMEOUT"        },
        { bus_error::lin_not_responded,  "NOT RESPONDED"  },
        { bus_error::lin_checksum_error, "CHECKSUM ERROR" },
        { bus_error::restarted,          "RESTARTED"      },
        { bus_error::error_warning,      "WARNING"        },
        { bus_error::error_passive,      "PASSIVE"        },
        { bus_error::error_active,       "ACTIVE"         },
        { bus_error::generic,            "ERROR"          },
    };

    out += "ERROR";
    char sep = ':';
    for (const auto& [flag, name] : names)
    {
        if (errors & flag)
        {
            out += sep;
            if (sep == ':')
            {
                out += ' ';
            }
            out += name;
            sep = '+';
        }
    }
}

void append_data(std::string& out, const BusMessage& m, bool ascii)
{
    if (m.len == 0)
    {
        return;
    }
    if (is_error_frame(m))
    {
        append_error_flags(out, m.errors);
        return;
    }
    append_bytes(out, std::span(m.data).first(m.len), ascii);
}
