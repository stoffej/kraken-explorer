#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "core/text.h"

TEST_CASE("durations read as seconds, minutes, hours and days")
{
    CHECK(format_duration(0.0) == "0.000");
    CHECK(format_duration(12.3456) == "12.346");
    CHECK(format_duration(59.9996) == "1:00.000"); // rounded before splitting
    CHECK(format_duration(245.25) == "4:05.250");
    CHECK(format_duration(11045.25) == "3:04:05.250");
    CHECK(format_duration(1107664.0) == "12d 19:41:04.000");
    CHECK(format_duration(1107664.0, 0) == "12d 19:41:04");
    CHECK(format_duration(-90.5, 1) == "-1:30.5");

    CHECK(parse_duration("90") == 90.0);
    CHECK(parse_duration(" 1:30 ") == 90.0);
    CHECK(parse_duration("1:02:03.5") == 3723.5);
    CHECK(parse_duration("2d 1:02:03") == 2 * 86400.0 + 3723.0);
    CHECK(parse_duration("2d") == 2 * 86400.0);
    CHECK(parse_duration("1d 0:10") == 86400.0 + 600.0); // after days: h:m
    CHECK(parse_duration("1d 5") == 86405.0);
    CHECK(parse_duration(format_duration(1107664.25)) == 1107664.25);
    CHECK_FALSE(parse_duration("").has_value());
    CHECK_FALSE(parse_duration("1:2:3:4").has_value());
    CHECK_FALSE(parse_duration("abc").has_value());
    CHECK_FALSE(parse_duration("-5").has_value());
}

TEST_CASE("big numbers are grouped by thousands")
{
    const auto grouped = [](uint64_t n)
    {
        std::string s;
        append_grouped(s, n);
        return s;
    };
    CHECK(grouped(0) == "0");
    CHECK(grouped(999) == "999");
    CHECK(grouped(1000) == "1 000");
    CHECK(grouped(182764569) == "182 764 569");
    CHECK(grouped(1000000000) == "1 000 000 000");
}

TEST_CASE("durations: format and parse agree for any value and decimals; odd inputs are refused")
{
    for (const int decimals : {0, 1, 3, 6})
    {
        const double step = std::pow(10.0, -decimals);
        for (const double seconds : {0.0, step, 59.0, 59.0 + 59 * step, 60.0, 3599.0, 3600.0, 86399.0, 86400.0,
                                     86400.0 + 59.0, 86400.0 + 600.0, 3 * 86400.0 + 3723.0, 1107664.0 + 7 * step})
        {
            CAPTURE(decimals);
            CAPTURE(seconds);
            const std::string text = format_duration(seconds, decimals);
            const auto back = parse_duration(text);
            REQUIRE(back.has_value());
            CHECK(std::abs(*back - seconds) < step * 0.5 + 1e-9);
        }
    }
    CHECK(parse_duration("1:30.25") == 90.25);
    CHECK(parse_duration("0:0:5") == 5.0);
    CHECK(parse_duration("1d") == 86400.0);
    CHECK(parse_duration("  3  ") == 3.0);
    CHECK_FALSE(parse_duration("inf").has_value());
    CHECK_FALSE(parse_duration("nan").has_value());
    CHECK_FALSE(parse_duration("1:inf").has_value());
    CHECK_FALSE(parse_duration(":30").has_value());
    CHECK_FALSE(parse_duration("1:").has_value());
    CHECK_FALSE(parse_duration("1::30").has_value());
    CHECK_FALSE(parse_duration("d").has_value());
    CHECK_FALSE(parse_duration("1d d").has_value());
    CHECK_FALSE(parse_duration("-1d").has_value());
    CHECK_FALSE(parse_duration("1:-30").has_value());
    CHECK_FALSE(parse_duration("0x10").has_value());
    CHECK_FALSE(parse_duration("1,5").has_value());
    CHECK_FALSE(parse_duration("1 30").has_value());
}

TEST_CASE("parse_number: whole string only, both signs for signed, bases, no leading plus or spaces")
{
    int i = 0;
    CHECK(parse_number("-12", i));
    CHECK(i == -12);
    CHECK_FALSE(parse_number("+12", i));
    CHECK_FALSE(parse_number(" 12", i));
    CHECK_FALSE(parse_number("12 ", i));
    CHECK_FALSE(parse_number("", i));
    CHECK_FALSE(parse_number("99999999999", i)); // overflow
    uint8_t b = 0;
    CHECK(parse_number("ff", b, 16));
    CHECK(b == 0xFF);
    CHECK_FALSE(parse_number("100", b, 16));
    CHECK_FALSE(parse_number("-1", b, 16));
    double d = 0.0;
    CHECK(parse_number("1e3", d));
    CHECK(d == 1000.0);
    CHECK(parse_number(".5", d));
    CHECK_FALSE(parse_number("1.5x", d));
    CHECK(trim("\t a b \r\n") == "a b");
    CHECK(trim("   ").empty());
    CHECK(iequals("AbC", "aBc"));
    CHECK_FALSE(iequals("abc", "abcd"));
    std::string g;
    append_grouped(g, 0);
    CHECK(g == "0");
    g.clear();
    append_grouped(g, 999);
    CHECK(g == "999");
    g.clear();
    append_grouped(g, 1000);
    CHECK(g == "1 000");
    g.clear();
    append_grouped(g, UINT64_MAX);
    CHECK(g == "18 446 744 073 709 551 615");
}
