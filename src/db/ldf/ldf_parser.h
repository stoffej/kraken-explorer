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

#pragma once
// ============================================================
//  ldf_parser.hpp  –  LIN Description File Parser (C++23)
//  Single-header, no external dependencies
// ============================================================
//
//  Supported sections
//  ------------------
//  LIN_description_file
//  LIN_protocol_version, LIN_speed
//  Node_attributes   (NAD, configured_NAD, initial_NAD, P2_min,
//                     ST_min, N_As_timeout, N_Cr_timeout)
//  Nodes             (Master / Slaves)
//  Signals           (scalar init value; array init values are skipped)
//  Frames            (unconditional)
//  Sporadic_frames
//  Signal_encoding_types  (logical, physical)
//  Signal_representation
//  Schedule_tables   (frame entries; diagnostic / configuration
//                     commands are kept with an empty frame_name)
//
//  Everything else (Channel_name, Event_triggered_frames,
//  Diagnostic_frames, product_id, ...) is skipped brace-aware.
//
//  Unit handling
//  -------------
//  Numbers  : decimal, hex (0x…), binary (0b…), float
//  Suffixes : k / K  → ×1 000
//             M      → ×1 000 000
//  Time     : value alone → seconds
//             ms          → milliseconds  → stored as seconds
//             us / µs     → microseconds  → stored as seconds
//             kbps / bps  → stored as bps (double)
//
//  Usage
//  -----
//  #include "ldf_parser.hpp"
//
//  auto result = ldf::parse_file("my_bus.ldf");
//  if (!result) { std::cerr << result.error() << '\n'; return 1; }
//  const ldf::LdfFile& ldf = *result;
//
// ============================================================

#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/charconv.h"

namespace ldf
{

// ============================================================
//  Data structures
// ============================================================

struct Signal
{
    std::string name;
    uint32_t bit_length = 0;
    uint64_t init_value = 0; // 0 for array init values
    std::string publisher;
};

struct FrameSignalRef
{
    std::string signal_name;
    uint32_t bit_offset = 0;
};

struct Frame
{
    std::string name;
    uint8_t id = 0;
    std::string publisher;
    uint8_t length = 0; // bytes
    std::vector<FrameSignalRef> signals;
};

struct SporadicFrame
{
    std::string name;
    std::vector<std::string> frames;
};

// ---------- Schedule table entries ----------

struct ScheduleTableEntry
{
    std::string frame_name; // empty for MasterReq / SlaveResp / configuration commands
    double delay_s = 0.0;   // in seconds
};

struct ScheduleTable
{
    std::string name;
    std::vector<ScheduleTableEntry> entries;
};

// ---------- Signal encoding ----------

struct LogicalValue
{
    uint32_t signal_value = 0;
    std::string text;
};

struct PhysicalRange
{
    double min_value = 0;
    double max_value = 0;
    double scale = 1;
    double offset = 0;
    std::string unit;
};

using EncodingValue = std::variant<LogicalValue, PhysicalRange>;

struct SignalEncodingType
{
    std::string name;
    std::vector<EncodingValue> values;
};

// ---------- Node attributes ----------

struct NodeAttribute
{
    std::string name;
    uint8_t nad = 0;
    std::optional<uint8_t> configured_nad;
    std::optional<uint8_t> initial_nad;
    double p2_min_s = 0;
    double st_min_s = 0;
    double n_as_timeout_s = 0;
    double n_cr_timeout_s = 0;
};

// ---------- Top-level ----------

struct Nodes
{
    std::string master;
    double master_time_base_s = 0;
    double master_jitter_s = 0;
    std::vector<std::string> slaves;
};

struct LdfFile
{
    std::string lin_protocol_version;
    double lin_speed_bps = 19200; // default 19.2 kbaud

    Nodes nodes;
    std::vector<Signal> signals;
    std::vector<Frame> frames;
    std::vector<SporadicFrame> sporadic_frames;
    std::vector<ScheduleTable> schedule_tables;
    std::vector<SignalEncodingType> signal_encoding_types;
    std::unordered_map<std::string, std::vector<std::string>> signal_representation; // encoding → signals

    std::vector<NodeAttribute> node_attributes;
};

// ============================================================
//  Lexer / tokeniser helpers
// ============================================================

namespace detail
{

// Thrown on syntax errors; a plain struct, caught by type in ldf::parse().
struct ParseError
{
    std::string message;
};

// ---------- character utilities ----------

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}
static bool is_hex_digit(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_alnum(char c)
{
    return is_alpha(c) || is_digit(c);
}
static bool is_ident(char c)
{
    return is_alnum(c) || c == '_' || c == '-' || c == '.';
}

// ============================================================
//  Parser state
// ============================================================

struct LdfParser
{
    std::string src;
    size_t pos = 0;
    int line = 1;
};

// --------------------------------------------------------
//  Low-level helpers
// --------------------------------------------------------

inline char peek(const LdfParser& p, size_t off = 0)
{
    size_t i = p.pos + off;
    return i < p.src.size() ? p.src[i] : '\0';
}
inline char advance(LdfParser& p)
{
    char c = p.src[p.pos++];
    if (c == '\n') ++p.line;
    return c;
}
inline bool at_end(const LdfParser& p)
{
    return p.pos >= p.src.size();
}

inline void skip_ws_comments(LdfParser& p)
{
    while (!at_end(p))
    {
        // whitespace
        if (is_ws(peek(p)))
        {
            advance(p);
            continue;
        }
        // C++ line comment
        if (peek(p) == '/' && peek(p, 1) == '/')
        {
            while (!at_end(p) && peek(p) != '\n') advance(p);
            continue;
        }
        // C block comment
        if (peek(p) == '/' && peek(p, 1) == '*')
        {
            advance(p);
            advance(p);
            while (!at_end(p))
            {
                if (peek(p) == '*' && peek(p, 1) == '/')
                {
                    advance(p);
                    advance(p);
                    break;
                }
                advance(p);
            }
            continue;
        }
        break;
    }
}

inline void expect(LdfParser& p, char c)
{
    skip_ws_comments(p);
    if (at_end(p) || peek(p) != c)
        throw ParseError{std::format("Line {}: expected '{}' got '{}'", p.line, c, at_end(p) ? '\0' : peek(p))};
    advance(p);
    skip_ws_comments(p);
}

inline bool try_char(LdfParser& p, char c)
{
    skip_ws_comments(p);
    if (!at_end(p) && peek(p) == c)
    {
        advance(p);
        skip_ws_comments(p);
        return true;
    }
    return false;
}

inline bool try_keyword(LdfParser& p, std::string_view kw)
{
    skip_ws_comments(p);
    if (p.src.compare(p.pos, kw.size(), kw) == 0 &&
        (p.pos + kw.size() >= p.src.size() || !is_ident(p.src[p.pos + kw.size()])))
    {
        p.pos += kw.size();
        skip_ws_comments(p);
        return true;
    }
    return false;
}

inline std::string read_identifier(LdfParser& p)
{
    skip_ws_comments(p);
    if (at_end(p) || !is_ident(peek(p)))
        throw ParseError{std::format("Line {}: expected identifier got '{}'", p.line, peek(p))};
    std::string id;
    while (!at_end(p) && is_ident(peek(p))) id += advance(p);
    skip_ws_comments(p);
    return id;
}

inline std::string read_quoted_string(LdfParser& p)
{
    skip_ws_comments(p);
    // Accept both "2.1" and 2.1 / 2_1 / identifier
    if (peek(p) == '"')
    {
        advance(p);
        std::string s;
        while (!at_end(p) && peek(p) != '"')
        {
            char c = advance(p);
            if (c == '\\' && !at_end(p)) s += advance(p);
            else s += c;
        }
        if (at_end(p)) throw ParseError{std::string("Unterminated string literal")};
        advance(p); // closing "
        skip_ws_comments(p);
        return s;
    }
    // unquoted: read until whitespace, ';', ',', '{'
    if (at_end(p) || (!is_ident(peek(p)) && peek(p) != '-' && peek(p) != '+'))
        throw ParseError{std::format("Line {}: expected string or identifier, got '{}'", p.line, peek(p))};
    std::string s;
    while (!at_end(p) && peek(p) != ';' && peek(p) != ',' && peek(p) != '{' && !is_ws(peek(p))) s += advance(p);
    skip_ws_comments(p);
    return s;
}

// --------------------------------------------------------
//  Number / unit parsing
// --------------------------------------------------------

// Read a raw numeric string (decimal / hex / binary / float).
// Returns the value as double and whether it had a decimal point.
inline double read_raw_number(LdfParser& p)
{
    skip_ws_comments(p);
    if (at_end(p)) throw ParseError{std::format("Line {}: expected number", p.line)};

    // hex
    if (peek(p) == '0' && (peek(p, 1) == 'x' || peek(p, 1) == 'X'))
    {
        p.pos += 2;
        if (!is_hex_digit(peek(p))) throw ParseError{std::format("Line {}: invalid hex literal", p.line)};
        std::string s;
        while (is_hex_digit(peek(p))) s += advance(p);
        uint64_t v = std::stoull(s, nullptr, 16);
        return static_cast<double>(v);
    }
    // binary
    if (peek(p) == '0' && (peek(p, 1) == 'b' || peek(p, 1) == 'B'))
    {
        p.pos += 2;
        std::string s;
        while (peek(p) == '0' || peek(p) == '1') s += advance(p);
        if (s.empty()) throw ParseError{std::format("Line {}: invalid binary literal", p.line)};
        return static_cast<double>(std::stoull(s, nullptr, 2));
    }
    // decimal / float
    std::string s;
    if (peek(p) == '-' || peek(p) == '+') s += advance(p);
    while (!at_end(p) && (is_digit(peek(p)) || peek(p) == '.')) s += advance(p);
    // exponent
    if (!at_end(p) && (peek(p) == 'e' || peek(p) == 'E'))
    {
        s += advance(p);
        if (!at_end(p) && (peek(p) == '+' || peek(p) == '-')) s += advance(p);
        while (!at_end(p) && is_digit(peek(p))) s += advance(p);
    }
    double v = 0.0;
    float_from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// Multiplier suffix:  k/K → 1e3,  M → 1e6,  (none) → 1
inline double read_multiplier_suffix(LdfParser& p)
{
    if (at_end(p)) return 1.0;
    char c = peek(p);
    if (c == 'k' || c == 'K')
    {
        advance(p);
        return 1e3;
    }
    if (c == 'M')
    {
        advance(p);
        return 1e6;
    }
    return 1.0;
}

// Read a speed value → always returns bps as double
// Accepts: 19200  /  19.2k  /  19200 bps  /  19.2 kbps
inline double read_speed(LdfParser& p)
{
    double v = read_raw_number(p);
    double mul = read_multiplier_suffix(p);
    v *= mul;
    // optional unit: bps / kbps
    skip_ws_comments(p);
    if (try_keyword(p, "kbps")) v *= 1e3;
    else try_keyword(p, "bps");
    return v;
}

// Read a time value → returns seconds
// Accepts: 5  /  5ms  /  5 ms  /  500us  /  500 us  /  500µs
inline double read_time_s(LdfParser& p)
{
    double v = read_raw_number(p);
    skip_ws_comments(p);
    // suffix (no whitespace between value and suffix in many LDF files)
    if (try_keyword(p, "ms")) return v * 1e-3;
    if (try_keyword(p, "us")) return v * 1e-6;
    // µ is multi-byte UTF-8: 0xC2 0xB5
    if ((unsigned char)peek(p) == 0xC2 && (unsigned char)peek(p, 1) == 0xB5)
    {
        p.pos += 2;
        skip_ws_comments(p);
        try_keyword(p, "s");
        return v * 1e-6;
    }
    if (try_keyword(p, "s")) return v;
    return v; // assume seconds
}

// Read an integer (hex / binary / decimal)
inline uint64_t read_integer(LdfParser& p)
{
    return static_cast<uint64_t>(read_raw_number(p));
}

// --------------------------------------------------------
//  Block helpers
// --------------------------------------------------------

inline void skip_unknown_block(LdfParser& p)
{
    // skip until '{' then match braces
    while (!at_end(p) && peek(p) != '{') advance(p);
    if (at_end(p)) return;
    int depth = 0;
    while (!at_end(p))
    {
        char c = advance(p);
        if (c == '{') ++depth;
        else if (c == '}')
        {
            if (--depth == 0) break;
        }
    }
    skip_ws_comments(p);
}

// Skips an unknown "key = value;" or "key { ... }" field (brace-aware).
inline void skip_unknown_field(LdfParser& p)
{
    while (!at_end(p) && peek(p) != ';' && peek(p) != '{' && peek(p) != '}') advance(p);
    if (peek(p) == '{') skip_unknown_block(p);
    else try_char(p, ';');
}

// identifier {, identifier} ;
inline std::vector<std::string> read_ident_list(LdfParser& p)
{
    std::vector<std::string> out;
    do
    {
        out.push_back(read_identifier(p));
    } while (try_char(p, ','));
    expect(p, ';');
    return out;
}

// --------------------------------------------------------
//  Section parsers
// --------------------------------------------------------

// --- Nodes ---
inline void ldf_parse_nodes(LdfParser& p, Nodes& n)
{
    expect(p, '{');
    while (!at_end(p) && peek(p) != '}')
    {
        std::string kw = read_identifier(p);
        if (kw == "Master")
        {
            expect(p, ':');
            n.master = read_identifier(p);
            expect(p, ',');
            n.master_time_base_s = read_time_s(p);
            expect(p, ',');
            n.master_jitter_s = read_time_s(p);
            expect(p, ';');
        }
        else if (kw == "Slaves")
        {
            expect(p, ':');
            n.slaves = read_ident_list(p);
        }
        else { skip_unknown_field(p); }
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Signals ---
inline void ldf_parse_signals(LdfParser& p, std::vector<Signal>& sigs)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        Signal s;
        s.name = read_identifier(p);
        expect(p, ':');
        s.bit_length = static_cast<uint32_t>(read_integer(p));
        expect(p, ',');

        // init value: scalar, or { b0, b1, … } which is consumed and discarded
        if (try_char(p, '{'))
        {
            do
            {
                read_integer(p);
            } while (try_char(p, ','));
            expect(p, '}');
        }
        else { s.init_value = read_integer(p); }
        expect(p, ',');
        s.publisher = read_identifier(p);
        // optional subscriber list – discarded
        while (try_char(p, ',')) read_identifier(p);
        expect(p, ';');
        sigs.push_back(std::move(s));
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Frames ---
inline void ldf_parse_frames(LdfParser& p, std::vector<Frame>& frames)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        Frame f;
        f.name = read_identifier(p);
        expect(p, ':');
        f.id = static_cast<uint8_t>(read_integer(p));
        expect(p, ',');
        f.publisher = read_identifier(p);
        expect(p, ',');
        f.length = static_cast<uint8_t>(read_integer(p));
        expect(p, '{');
        skip_ws_comments(p);
        while (!at_end(p) && peek(p) != '}')
        {
            FrameSignalRef ref;
            ref.signal_name = read_identifier(p);
            expect(p, ',');
            ref.bit_offset = static_cast<uint32_t>(read_integer(p));
            expect(p, ';');
            f.signals.push_back(ref);
            skip_ws_comments(p);
        }
        expect(p, '}');
        frames.push_back(std::move(f));
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Sporadic frames ---
inline void ldf_parse_sporadic_frames(LdfParser& p, std::vector<SporadicFrame>& sf)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        SporadicFrame s;
        s.name = read_identifier(p);
        expect(p, ':');
        s.frames = read_ident_list(p);
        sf.push_back(std::move(s));
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Schedule tables ---
inline void ldf_parse_schedule_tables(LdfParser& p, std::vector<ScheduleTable>& tables)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        ScheduleTable t;
        t.name = read_identifier(p);
        expect(p, '{');
        skip_ws_comments(p);
        while (!at_end(p) && peek(p) != '}')
        {
            ScheduleTableEntry e;
            std::string cmd = read_identifier(p);

            // MasterReq / SlaveResp and every "Cmd { args }" configuration command
            // are not frames: skip their arguments and leave frame_name empty.
            if (peek(p) == '{') skip_unknown_block(p);
            else if (cmd != "MasterReq" && cmd != "SlaveResp") e.frame_name = std::move(cmd);

            // delay
            if (!try_keyword(p, "delay")) throw ParseError{std::format("Line {}: expected 'delay'", p.line)};
            e.delay_s = read_time_s(p);
            expect(p, ';');
            t.entries.push_back(std::move(e));
            skip_ws_comments(p);
        }
        expect(p, '}');
        tables.push_back(std::move(t));
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Signal encoding types ---
inline void ldf_parse_signal_encoding_types(LdfParser& p, std::vector<SignalEncodingType>& enc)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        SignalEncodingType t;
        t.name = read_identifier(p);
        expect(p, '{');
        skip_ws_comments(p);
        while (!at_end(p) && peek(p) != '}')
        {
            std::string kind = read_identifier(p);
            if (kind == "logical_value")
            {
                expect(p, ',');
                LogicalValue lv;
                lv.signal_value = static_cast<uint32_t>(read_integer(p));
                if (peek(p) == ',')
                {
                    advance(p);
                    skip_ws_comments(p);
                    lv.text = read_quoted_string(p);
                }
                expect(p, ';');
                t.values.push_back(lv);
            }
            else if (kind == "physical_value")
            {
                expect(p, ',');
                PhysicalRange pr;
                pr.min_value = read_raw_number(p);
                expect(p, ',');
                pr.max_value = read_raw_number(p);
                expect(p, ',');
                pr.scale = read_raw_number(p);
                expect(p, ',');
                pr.offset = read_raw_number(p);
                if (peek(p) == ',')
                {
                    advance(p);
                    skip_ws_comments(p);
                    pr.unit = read_quoted_string(p);
                }
                expect(p, ';');
                t.values.push_back(pr);
            }
            else { skip_unknown_field(p); } // bcd_value, ascii_value, ...
            skip_ws_comments(p);
        }
        expect(p, '}');
        enc.push_back(std::move(t));
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Signal representation ---
inline void ldf_parse_signal_representation(LdfParser& p,
                                            std::unordered_map<std::string, std::vector<std::string>>& rep)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        std::string enc = read_identifier(p);
        expect(p, ':');
        auto sigs = read_ident_list(p);
        auto& list = rep[enc];
        list.insert(list.end(), sigs.begin(), sigs.end());
        skip_ws_comments(p);
    }
    expect(p, '}');
}

// --- Node attributes ---
inline void ldf_parse_node_attributes(LdfParser& p, std::vector<NodeAttribute>& attrs)
{
    expect(p, '{');
    skip_ws_comments(p);
    while (!at_end(p) && peek(p) != '}')
    {
        NodeAttribute a;
        a.name = read_identifier(p);
        expect(p, '{');
        skip_ws_comments(p);
        while (!at_end(p) && peek(p) != '}')
        {
            std::string kw = read_identifier(p);
            if (kw == "configured_NAD")
            {
                expect(p, '=');
                a.configured_nad = static_cast<uint8_t>(read_integer(p));
                expect(p, ';');
            }
            else if (kw == "initial_NAD")
            {
                expect(p, '=');
                a.initial_nad = static_cast<uint8_t>(read_integer(p));
                expect(p, ';');
            }
            else if (kw == "NAD")
            {
                expect(p, '=');
                a.nad = static_cast<uint8_t>(read_integer(p));
                expect(p, ';');
            }
            else if (kw == "P2_min")
            {
                expect(p, '=');
                a.p2_min_s = read_time_s(p);
                expect(p, ';');
            }
            else if (kw == "ST_min")
            {
                expect(p, '=');
                a.st_min_s = read_time_s(p);
                expect(p, ';');
            }
            else if (kw == "N_As_timeout")
            {
                expect(p, '=');
                a.n_as_timeout_s = read_time_s(p);
                expect(p, ';');
            }
            else if (kw == "N_Cr_timeout")
            {
                expect(p, '=');
                a.n_cr_timeout_s = read_time_s(p);
                expect(p, ';');
            }
            else { skip_unknown_field(p); } // LIN_protocol, product_id, response_error, configurable_frames, ...
            skip_ws_comments(p);
        }
        expect(p, '}');
        attrs.push_back(std::move(a));
        skip_ws_comments(p);
    }
    expect(p, '}');
}

inline LdfFile ldf_parse(LdfParser& p)
{
    LdfFile ldf;
    skip_ws_comments(p);

    // The file MUST begin with "LIN_description_file"
    if (!try_keyword(p, "LIN_description_file"))
        throw ParseError{std::string("Expected 'LIN_description_file' at start")};
    expect(p, ';');
    skip_ws_comments(p);

    while (p.pos < p.src.size())
    {
        std::string kw = read_identifier(p);
        skip_ws_comments(p);

        if (kw == "LIN_protocol_version")
        {
            expect(p, '=');
            ldf.lin_protocol_version = read_quoted_string(p);
            expect(p, ';');
        }
        else if (kw == "LIN_speed")
        {
            expect(p, '=');
            ldf.lin_speed_bps = read_speed(p);
            expect(p, ';');
        }
        else if (kw == "Nodes") { ldf_parse_nodes(p, ldf.nodes); }
        else if (kw == "Signals") { ldf_parse_signals(p, ldf.signals); }
        else if (kw == "Frames") { ldf_parse_frames(p, ldf.frames); }
        else if (kw == "Sporadic_frames") { ldf_parse_sporadic_frames(p, ldf.sporadic_frames); }
        else if (kw == "Schedule_tables") { ldf_parse_schedule_tables(p, ldf.schedule_tables); }
        else if (kw == "Signal_encoding_types") { ldf_parse_signal_encoding_types(p, ldf.signal_encoding_types); }
        else if (kw == "Signal_representation") { ldf_parse_signal_representation(p, ldf.signal_representation); }
        else if (kw == "Node_attributes") { ldf_parse_node_attributes(p, ldf.node_attributes); }
        else
        {
            // Unknown "key = value;" or section – skipped brace-aware
            skip_unknown_field(p);
        }
        skip_ws_comments(p);
    }
    return ldf;
}

} // namespace detail

// ============================================================
//  Public API
// ============================================================

/// Parse an LDF string.
/// Returns a populated LdfFile on success, or an error message.
[[nodiscard]]
inline std::expected<LdfFile, std::string> parse(const std::string& source)
{
    try
    {
        detail::LdfParser p{.src = source};
        return detail::ldf_parse(p);
    }
    catch (const detail::ParseError& e)
    {
        return std::unexpected(e.message);
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::string("Exception: ") + e.what());
    }
}

/// Parse an LDF file by path.
[[nodiscard]]
inline std::expected<LdfFile, std::string> parse_file(const std::string& path)
{
    std::ifstream f(path, std::ios::in);
    if (!f) return std::unexpected("Cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

} // namespace ldf
