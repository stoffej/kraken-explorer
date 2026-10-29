// DBC parser, end to end: DBC text in, populated CanDb out.
//
// The most important case here is the Motorola start-bit conversion. BusMessage
// receives an already-converted start bit, so the packing tests in
// bus_message_signal only pin the second half of that contract -- this file pins
// the parser side, which is where the issue #34 convention actually lives.
//
// Was a Qt Test; doctest now with the same vectors. Parsing goes through a real
// file, as before, so dbc_parse_file() is covered as well as the tokenizer.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbc/dbc_parser.h"
#include "db/model/can_db.h"

namespace
{

constexpr std::string_view dbc_preamble = R"(VERSION "test-1.0"

NS_:

BS_:

BU_: ECU_A ECU_B

)";

// Writes the DBC text to a temporary file and parses that file.
[[nodiscard]] bool parse(std::string_view body, CanDb& db, bool with_preamble = true,
                         std::vector<DbcError>* errors = nullptr)
{
    const auto path = std::filesystem::temp_directory_path() / "kraken_dbc_parser_test.dbc";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (with_preamble)
        {
            out << dbc_preamble;
        }
        out << body;
    }
    const bool ok = dbc_parse_file(path, db, errors);
    std::filesystem::remove(path);
    return ok;
}

[[nodiscard]] const CanDbSignal* signal(CanDb& db, uint32_t raw_id, std::string_view name)
{
    CanDbMessage* msg = can_db_find_message(db, raw_id);
    return msg ? can_db_find_signal(*msg, name) : nullptr;
}

} // namespace

TEST_CASE("parses message")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 256 TestMsg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", db));

    CHECK(db.messages.size() == 1);

    const CanDbMessage* msg = can_db_find_message(db, 256);
    REQUIRE(msg != nullptr);
    CHECK(msg->name == "TestMsg");
    CHECK(msg->dlc == 8);
    CHECK(msg->signals.size() == 1);
}

TEST_CASE("parses signal attributes")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Temp : 0|16@1- (0.03125,-273.15) [-273.15|1000] "degC" ECU_B
)", db));

    const CanDbSignal* sig = signal(db, 100, "Temp");
    REQUIRE(sig != nullptr);
    CHECK(sig->start_bit == 0);
    CHECK(sig->length == 16);
    CHECK(sig->factor == doctest::Approx(0.03125));
    CHECK(sig->offset == doctest::Approx(-273.15));
    CHECK(sig->min == doctest::Approx(-273.15));
    CHECK(sig->max == doctest::Approx(1000.0));
    CHECK(sig->unit == "degC");
    CHECK_FALSE(sig->is_unsigned);
    CHECK_FALSE(sig->big_endian);
}

TEST_CASE("exponents with e and E")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 101 Nav: 8 ECU_A
 SG_ Latitude : 0|32@1- (1e-07,0) [-90|90] "deg" ECU_B
 SG_ Big : 32|16@1+ (2.5E+3,-1e2) [0|1.5e8] "" ECU_B
)", db));
    const CanDbSignal* lat = signal(db, 101, "Latitude");
    REQUIRE(lat != nullptr);
    CHECK(lat->factor == doctest::Approx(1e-7));
    const CanDbSignal* big = signal(db, 101, "Big");
    REQUIRE(big != nullptr);
    CHECK(big->factor == doctest::Approx(2500.0));
    CHECK(big->offset == doctest::Approx(-100.0));
    CHECK(big->max == doctest::Approx(1.5e8));
}

// This is the conversion that issue #34 turned on. If it changes, the packing
// tests in bus_message_signal are measuring the wrong thing.
TEST_CASE("motorola start bit is converted")
{
    struct Row
    {
        int dbc_start_bit;
        int expected_internal;
    };
    // A Motorola start bit (the MSB position, in DBC "sawtooth" numbering) is
    // stored as a sequential MSB-first bit index: (byte * 8) + (7 - bit).
    constexpr Row rows[] = {
        {7, 0}, {0, 7}, {11, 12}, {23, 16}, {18, 21}, {39, 32}, {52, 51}, {63, 56},
    };

    for (const Row& r : rows)
    {
        CAPTURE(r.dbc_start_bit);
        CanDb db;
        REQUIRE(parse(std::format(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : {}|1@0+ (1,0) [0|1] "" ECU_B
)", r.dbc_start_bit), db));

        const CanDbSignal* sig = signal(db, 100, "Sig");
        REQUIRE(sig != nullptr);
        CHECK(sig->big_endian);
        CHECK(sig->start_bit == r.expected_internal);
    }
}

// Intel signals are stored exactly as written.
TEST_CASE("intel start bit is unchanged")
{
    for (const int start_bit : {0, 1, 7, 11, 32, 63})
    {
        CAPTURE(start_bit);
        CanDb db;
        REQUIRE(parse(std::format(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : {}|1@1+ (1,0) [0|1] "" ECU_B
)", start_bit), db));

        const CanDbSignal* sig = signal(db, 100, "Sig");
        REQUIRE(sig != nullptr);
        CHECK_FALSE(sig->big_endian);
        CHECK(sig->start_bit == start_bit);
    }
}

// @0 is Motorola/big-endian, @1 is Intel/little-endian -- easy to invert.
TEST_CASE("byte order flag mapping")
{
    CanDb big_endian;
    REQUIRE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 7|8@0+ (1,0) [0|255] "" ECU_B
)", big_endian));
    REQUIRE(signal(big_endian, 100, "Sig") != nullptr);
    CHECK(signal(big_endian, 100, "Sig")->big_endian);

    CanDb little_endian;
    REQUIRE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", little_endian));
    REQUIRE(signal(little_endian, 100, "Sig") != nullptr);
    CHECK_FALSE(signal(little_endian, 100, "Sig")->big_endian);
}

TEST_CASE("signed and unsigned flag")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ U : 0|8@1+ (1,0) [0|255] "" ECU_B
 SG_ S : 8|8@1- (1,0) [-128|127] "" ECU_B
)", db));

    REQUIRE(signal(db, 100, "U") != nullptr);
    REQUIRE(signal(db, 100, "S") != nullptr);
    CHECK(signal(db, 100, "U")->is_unsigned);
    CHECK_FALSE(signal(db, 100, "S")->is_unsigned);
}

// DBC stores extended ids with bit 31 set; the raw id keeps that marker.
TEST_CASE("parses extended identifier")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 2566844926 ExtMsg: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B
)", db));

    const CanDbMessage* msg = can_db_find_message(db, 2566844926u);
    REQUIRE(msg != nullptr);
    CHECK(msg->name == "ExtMsg");
    CHECK((msg->raw_id & 0x80000000u) != 0);
}

TEST_CASE("parses multiplexer")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 100 MuxMsg: 8 ECU_A
 SG_ Mode M : 0|4@1+ (1,0) [0|15] "" ECU_B
 SG_ ValueA m0 : 8|8@1+ (1,0) [0|255] "" ECU_B
 SG_ ValueB m1 : 8|8@1+ (1,0) [0|255] "" ECU_B
)", db));

    CanDbMessage* msg = can_db_find_message(db, 100);
    REQUIRE(msg != nullptr);

    const CanDbSignal* mode = can_db_find_signal(*msg, "Mode");
    REQUIRE(mode != nullptr);
    CHECK(mode->is_muxer);
    CHECK_FALSE(mode->is_muxed);
    // The message's muxer must be wired up, otherwise every muxed signal is
    // treated as absent and silently disappears from the trace.
    CHECK(can_db_muxer(*msg) == mode);

    const CanDbSignal* a = can_db_find_signal(*msg, "ValueA");
    REQUIRE(a != nullptr);
    CHECK(a->is_muxed);
    CHECK_FALSE(a->is_muxer);
    CHECK(a->mux_value == 0u);

    const CanDbSignal* b = can_db_find_signal(*msg, "ValueB");
    REQUIRE(b != nullptr);
    CHECK(b->is_muxed);
    CHECK(b->mux_value == 1u);
}

TEST_CASE("parses value table")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ State : 0|4@1+ (1,0) [0|15] "" ECU_B

VAL_ 100 State 0 "Off" 1 "On" 15 "Invalid" ;
)", db));

    const CanDbSignal* sig = signal(db, 100, "State");
    REQUIRE(sig != nullptr);
    CHECK(can_signal_value_name(*sig, 0) == "Off");
    CHECK(can_signal_value_name(*sig, 1) == "On");
    CHECK(can_signal_value_name(*sig, 15) == "Invalid");
    CHECK(can_signal_value_name(*sig, 7).empty());
}

TEST_CASE("VAL_ and CM_ name an extended message by its 29-bit id too")
{
    // As some tools write it: BO_ with bit 31, the references without.
    CanDb db;
    REQUIRE(parse(R"(BO_ 2667576602 Ext: 8 ECU_A
 SG_ sqi : 0|8@1- (1,0) [-3|1] "" ECU_B

CM_ SG_ 520092954 sqi "Signal quality";
VAL_ 520092954 sqi 1 "OK" -1 "GEN ERROR" ;
)", db));

    const CanDbSignal* sig = signal(db, 2667576602u, "sqi");
    REQUIRE(sig != nullptr);
    CHECK(sig->comment == "Signal quality");
    CHECK(can_signal_value_name(*sig, 1) == "OK");
}

TEST_CASE("VAL_ for a signal or message that is gone is skipped")
{
    CanDb db;
    std::vector<DbcError> errors;
    CHECK(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ State : 0|4@1+ (1,0) [0|15] "" ECU_B

VAL_ 100 Gone 0 "Off" 1 "On" ;
VAL_ 999 State 0 "Off" ;
VAL_ 100 State 1 "On" ;
)", db, true, &errors));
    CHECK(errors.empty());
    CHECK(can_signal_value_name(*signal(db, 100, "State"), 1) == "On");
}

TEST_CASE("parses comments")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 0|8@1+ (1,0) [0|255] "" ECU_B

CM_ BO_ 100 "message comment";
CM_ SG_ 100 Sig "signal comment";
)", db));

    const CanDbMessage* msg = can_db_find_message(db, 100);
    REQUIRE(msg != nullptr);
    CHECK(msg->comment == "message comment");
    REQUIRE(signal(db, 100, "Sig") != nullptr);
    CHECK(signal(db, 100, "Sig")->comment == "signal comment");
}

TEST_CASE("parses multiple messages and signals")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 100 First: 8 ECU_A
 SG_ A : 0|8@1+ (1,0) [0|255] "" ECU_B
 SG_ B : 8|8@1+ (1,0) [0|255] "" ECU_B

BO_ 200 Second: 4 ECU_B
 SG_ C : 0|16@0+ (1,0) [0|65535] "" ECU_A
)", db));

    CHECK(db.messages.size() == 2);
    REQUIRE(can_db_find_message(db, 100) != nullptr);
    REQUIRE(can_db_find_message(db, 200) != nullptr);
    CHECK(can_db_find_message(db, 100)->signals.size() == 2);
    CHECK(can_db_find_message(db, 200)->signals.size() == 1);
    CHECK(can_db_find_message(db, 200)->name == "Second");
    CHECK(can_db_find_message(db, 200)->dlc == 4);
}

// The parser is deliberately lenient about sections it does not implement
// (BA_, SIG_VALTYPE_, ...): they are skipped, not rejected. The consequence is
// worth knowing -- a file that is not a DBC at all parses "successfully" and
// simply yields no messages, so callers must check the message count rather than
// trust the return value alone.
TEST_CASE("unknown sections are skipped")
{
    CanDb unknown_only;
    CHECK(parse("this is not a DBC file at all\n", unknown_only, false));
    CHECK(unknown_only.messages.empty());

    // An unimplemented section between real ones must not disturb them.
    CanDb mixed;
    REQUIRE(parse(R"(BO_ 100 First: 8 ECU_A
 SG_ A : 0|8@1+ (1,0) [0|255] "" ECU_B

BA_DEF_ SG_ "GenSigStartValue" INT 0 10000;

BO_ 200 Second: 8 ECU_A
 SG_ B : 0|8@1+ (1,0) [0|255] "" ECU_B
)", mixed));

    CHECK(mixed.messages.size() == 2);
    CHECK(can_db_find_message(mixed, 100) != nullptr);
    CHECK(can_db_find_message(mixed, 200) != nullptr);
}

// A signal line missing its trailing fields must fail rather than yield a
// half-populated signal.
TEST_CASE("rejects truncated signal")
{
    CanDb db;
    CHECK_FALSE(parse(R"(BO_ 100 M: 8 ECU_A
 SG_ Sig : 0|8@1+
)", db));
}

// Diagnostics: every bad statement is reported with the line it starts on (the preamble
// is 8 lines, so the body starts at line 9), the parse still fails, and everything
// around the bad statements is kept: the message with a bad SG_ keeps its good signals
// (before and after), the message with a bad header is dropped, later messages parse.
TEST_CASE("errors carry line numbers and parsing continues past them")
{
    CanDb db;
    std::vector<DbcError> errors;
    CHECK_FALSE(parse(R"(BO_ 100 First: 8 ECU_A
 SG_ Good : 0|8@1+ (1,0) [0|255] "" ECU_B
 SG_ Bad : 8|8@1+
 SG_ AlsoGood : 16|8@1+ (1,0) [0|255] "" ECU_B

BO_ 200 : 8 ECU_A
 SG_ Lost : 0|8@1+ (1,0) [0|255] "" ECU_B

BO_ 300 Third: 8 ECU_A
 SG_ Y : 0|8@1+ (1,0) [0|255] "" ECU_B

CM_ SG_ 100 Missing "no such signal";
VAL_ 300 Y 0 "Zero" 1 "One" ;
)", db, true, &errors));

    REQUIRE(errors.size() == 3);
    CHECK(errors[0].line == 11);
    CHECK(errors[0].message.starts_with("SG_ Bad: "));
    CHECK(errors[1].line == 14);
    CHECK(errors[1].message.starts_with("BO_ 200: "));
    CHECK(errors[1].message.find("message name") != std::string::npos);
    CHECK(errors[2].line == 20);
    CHECK(errors[2].message.starts_with("CM_ SG_: "));
    CHECK(errors[2].message.find("Missing") != std::string::npos);

    CHECK(db.messages.size() == 2);
    const CanDbMessage* first = can_db_find_message(db, 100);
    REQUIRE(first != nullptr);
    CHECK(first->signals.size() == 2);
    CHECK(can_db_find_signal(*first, "Good") != nullptr);
    CHECK(can_db_find_signal(*first, "AlsoGood") != nullptr);
    CHECK(can_db_find_signal(*first, "Bad") == nullptr);
    CHECK(can_db_find_message(db, 200) == nullptr);
    REQUIRE(signal(db, 300, "Y") != nullptr);
    CHECK(can_signal_value_name(*signal(db, 300, "Y"), 1) == "One");   // VAL_ after the errors still lands
}

// A character no token starts with is reported with its line and skipped, the rest of
// the file still parses. Without an error vector the call just fails, as before.
TEST_CASE("tokenizer errors are reported with their line and skipped")
{
    constexpr std::string_view body = R"(BO_ 100 First: 8 ECU_A
 SG_ A : 0|8@1+ (1,0) [0|255] "" ECU_B

# not a DBC comment

BO_ 200 Second: 8 ECU_A
 SG_ B : 0|8@1+ (1,0) [0|255] "" ECU_B
)";
    CanDb db;
    std::vector<DbcError> errors;
    CHECK_FALSE(parse(body, db, true, &errors));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].line == 12);
    CHECK(errors[0].message.find("unexpected character") != std::string::npos);
    CHECK(db.messages.size() == 2);

    CanDb plain;
    CHECK_FALSE(parse(body, plain));
    CHECK(plain.messages.size() == 2);
}

// SIG_VALTYPE_: vectors from cantools 44.1 on the same DBC (Message.encode/decode), not
// from Kraken. Floats {IntelF 3.14159, MotoF -273.15} = d00f4940c40d9333, decoded as
// 3.141590118408203 / -273.1499938964844; Dbl {D 1e10} = 000000205fa00242.
TEST_CASE("SIG_VALTYPE_ marks IEEE float and double signals")
{
    CanDb db;
    REQUIRE(parse(R"(BO_ 256 Floats: 8 ECU_A
 SG_ IntelF : 0|32@1- (1,0) [0|0] "" ECU_B
 SG_ MotoF : 39|32@0- (0.5,10) [0|0] "" ECU_B
 SG_ Short : 32|16@1- (1,0) [0|0] "" ECU_B

BO_ 257 Dbl: 8 ECU_A
 SG_ D : 0|64@1- (1,0) [0|0] "" ECU_B

SIG_VALTYPE_ 256 IntelF : 1;
SIG_VALTYPE_ 256 MotoF : 1;
SIG_VALTYPE_ 256 Short : 1;
SIG_VALTYPE_ 257 D : 2;
SIG_VALTYPE_ 257 Missing : 2;
)", db));

    const CanDbSignal* intel_f = signal(db, 256, "IntelF");
    const CanDbSignal* moto_f = signal(db, 256, "MotoF");
    const CanDbSignal* d = signal(db, 257, "D");
    REQUIRE(intel_f != nullptr);
    REQUIRE(moto_f != nullptr);
    REQUIRE(d != nullptr);
    CHECK(intel_f->value_type == SignalValueType::float32);
    CHECK(moto_f->value_type == SignalValueType::float32);
    CHECK(d->value_type == SignalValueType::float64);
    CHECK(signal(db, 256, "Short")->value_type == SignalValueType::integer);   // 16 bit: warned, integer

    BusMessage f{.id = 256};
    set_length(f, 8);
    constexpr std::array<uint8_t, 8> f_bytes{0xd0, 0x0f, 0x49, 0x40, 0xc4, 0x0d, 0x93, 0x33};
    std::ranges::copy(f_bytes, f.data.begin());
    CHECK(can_signal_extract_physical(*intel_f, f) == 3.141590118408203);
    CHECK(can_signal_extract_physical(*moto_f, f) == doctest::Approx(-273.1499938964844).epsilon(1e-15));

    BusMessage enc{.id = 256};
    set_length(enc, 8);
    can_signal_inject_physical(*intel_f, enc, 3.14159);
    can_signal_inject_physical(*moto_f, enc, -273.15);
    CHECK(std::ranges::equal(std::span(enc.data).first(8), f_bytes));

    BusMessage dm{.id = 257};
    set_length(dm, 8);
    can_signal_inject_physical(*d, dm, 1e10);
    constexpr std::array<uint8_t, 8> d_bytes{0x00, 0x00, 0x00, 0x20, 0x5f, 0xa0, 0x02, 0x42};
    CHECK(std::ranges::equal(std::span(dm.data).first(8), d_bytes));
    CHECK(can_signal_extract_physical(*d, dm) == 1e10);
}
