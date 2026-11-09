// LIN diagnostic request dialog: the hex byte parser and formatter behind its Data field.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui/diag_dialog.h"

TEST_CASE("parse_hex_bytes: pairs with any spacing, both cases; odd digits and other characters refused")
{
    using B = std::vector<uint8_t>;
    CHECK(parse_hex_bytes("") == B{});
    CHECK(parse_hex_bytes("   ") == B{});
    CHECK(parse_hex_bytes("00") == B{0x00});
    CHECK(parse_hex_bytes("ff") == B{0xFF});
    CHECK(parse_hex_bytes("B2 3C") == B{0xB2, 0x3C});
    CHECK(parse_hex_bytes("b23c") == B{0xB2, 0x3C});
    CHECK(parse_hex_bytes(" b2\t3c ") == B{0xB2, 0x3C});
    CHECK(parse_hex_bytes("B 2 3 C") == B{0xB2, 0x3C}); // nibbles may be split by spaces
    CHECK_FALSE(parse_hex_bytes("B23").has_value());
    CHECK_FALSE(parse_hex_bytes("0x12").has_value());
    CHECK_FALSE(parse_hex_bytes("12,34").has_value());
    CHECK_FALSE(parse_hex_bytes("12 3G").has_value());
    CHECK_FALSE(parse_hex_bytes("12\n34").has_value());
}

TEST_CASE("format_hex_bytes: upper case pairs separated by one space, and parse reads it back")
{
    CHECK(format_hex_bytes(std::vector<uint8_t>{}).empty());
    CHECK(format_hex_bytes(std::vector<uint8_t>{0x00, 0x0A, 0xFF}) == "00 0A FF");
    std::vector<uint8_t> all;
    for (int b = 0; b < 256; ++b)
    {
        all.push_back(static_cast<uint8_t>(b));
    }
    CHECK(parse_hex_bytes(format_hex_bytes(all)) == all);
}
