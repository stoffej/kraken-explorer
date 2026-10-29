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

#include "dbc_parser.h"

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/text.h"

namespace
{

// Bit mask so readers can ask for several token types at once.
namespace tok
{
constexpr uint32_t whitespace = 1u << 0;
constexpr uint32_t identifier = 1u << 1;
constexpr uint32_t string = 1u << 2;
constexpr uint32_t number = 1u << 3;
constexpr uint32_t colon = 1u << 4;
constexpr uint32_t pipe = 1u << 5;
constexpr uint32_t at = 1u << 6;
constexpr uint32_t plus = 1u << 7;
constexpr uint32_t parenth_open = 1u << 8;
constexpr uint32_t parenth_close = 1u << 9;
constexpr uint32_t bracket_open = 1u << 10;
constexpr uint32_t bracket_close = 1u << 11;
constexpr uint32_t comma = 1u << 12;
constexpr uint32_t semicolon = 1u << 13;
constexpr uint32_t minus = 1u << 14;
constexpr uint32_t all = 0xFFFFFFFFu;
}

struct DbcToken
{
    uint32_t type = 0;
    std::string data;           // UTF-8
    int line = 0;               // 1-based line of the first character
    int line_breaks = 0;
    bool string_done = false;   // string tokens: closing quote seen
    bool string_escape = false; // string tokens: previous char was '\'
};

struct DbcCursor
{
    std::vector<DbcToken> tokens;
    size_t pos = 0;

    std::vector<DbcError>* errors = nullptr;
    bool failed = false;
    // The statement being parsed (a section keyword or an SG_ line): error prefix,
    // its line and the index of its keyword token, where recovery restarts.
    std::string stmt;
    int stmt_line = 0;
    size_t stmt_pos = 0;
};

// QChar::isSpace() over Latin-1.
[[nodiscard]] bool is_space(unsigned char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0x85 || c == 0xA0;
}

[[nodiscard]] bool is_digit(unsigned char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool is_id_start(unsigned char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// Full match of ^\d+(\.\d*)?([eE][-+]?\d*)?$ - the number token grows while this holds.
[[nodiscard]] bool is_number_text(std::string_view s) noexcept
{
    size_t i = 0;
    while (i < s.size() && is_digit(s[i])) { ++i; }
    if (i == 0) { return false; }
    if (i < s.size() && s[i] == '.')
    {
        ++i;
        while (i < s.size() && is_digit(s[i])) { ++i; }
    }
    if (i < s.size() && (s[i] == 'E' || s[i] == 'e'))
    {
        ++i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) { ++i; }
        while (i < s.size() && is_digit(s[i])) { ++i; }
    }
    return i == s.size();
}

[[nodiscard]] uint32_t single_char_type(unsigned char c) noexcept
{
    switch (c)
    {
        case ':': return tok::colon;
        case '|': return tok::pipe;
        case '@': return tok::at;
        case '+': return tok::plus;
        case '-': return tok::minus;
        case '(': return tok::parenth_open;
        case ')': return tok::parenth_close;
        case '[': return tok::bracket_open;
        case ']': return tok::bracket_close;
        case ',': return tok::comma;
        case ';': return tok::semicolon;
        default: return 0;
    }
}

// Type of a token starting with c, 0 if no token may start with it.
[[nodiscard]] uint32_t token_type_for(unsigned char c) noexcept
{
    if (is_space(c)) { return tok::whitespace; }
    if (is_digit(c)) { return tok::number; }
    if (c == '"') { return tok::string; }
    if (const uint32_t t = single_char_type(c)) { return t; }
    if (is_id_start(c)) { return tok::identifier; }
    return 0;
}

// Updates string-token state as a side effect, like DbcStringToken::acceptsChar.
[[nodiscard]] bool token_accepts(DbcToken& t, unsigned char c)
{
    switch (t.type)
    {
        case tok::whitespace:
            return is_space(c);
        case tok::identifier:
            return t.data.empty() ? is_id_start(c) : (is_id_start(c) || is_digit(c));
        case tok::number:
        {
            std::string s = t.data;
            s.push_back(static_cast<char>(c));
            return is_number_text(s);
        }
        case tok::string:
            if (t.string_done) { return false; }
            if (t.data.empty()) { return c == '"'; }
            if (t.string_escape)
            {
                t.string_escape = false;
            }
            else
            {
                t.string_escape = (c == '\\');
                t.string_done = (c == '"');
            }
            return true;
        default:
            return t.data.empty() && single_char_type(c) == t.type;
    }
}

void token_append(DbcToken& t, unsigned char c)
{
    if (c == '\n') { t.line_breaks++; }
    if (c < 0x80)
    {
        t.data.push_back(static_cast<char>(c));
    }
    else
    {
        // Latin-1 code point -> UTF-8
        t.data.push_back(static_cast<char>(0xC0 | (c >> 6)));
        t.data.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

void report(DbcCursor& cur, int line, std::string message)
{
    cur.failed = true;
    if (cur.errors)
    {
        cur.errors->push_back(DbcError{.line = line, .message = std::move(message)});
    }
}

// false when a character no token can start with was seen; such characters are
// reported (line and column) and skipped, the rest of the text is still tokenized.
// The pre-port tokenizer stopped at the first one.
[[nodiscard]] bool tokenize(std::string_view text, DbcCursor& cur)
{
    DbcToken current;
    bool have_current = false;
    bool ok = true;
    int line = 1;
    int column = 0;

    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char>(ch);
        const int at_line = line;   // a '\n' belongs to the line it ends
        if (c == '\n')
        {
            line++;
            column = 1;
        }
        else
        {
            column++;
        }

        if (have_current)
        {
            if (token_accepts(current, c))
            {
                token_append(current, c);
                continue;
            }
            cur.tokens.push_back(std::move(current));
            have_current = false;
        }

        const uint32_t type = token_type_for(c);
        if (type == 0)
        {
            ok = false;
            report(cur, at_line, std::format("column {}: unexpected character 0x{:02X}", column, c));
            continue;
        }
        current = DbcToken{.type = type, .line = at_line};
        (void)token_accepts(current, c);   // arms the string-token state
        token_append(current, c);
        have_current = true;
    }

    // The pre-port tokenizer dropped the last token. A trailing whitespace token is
    // still dropped: sections rely on "no more tokens" as their ending, and a lone
    // line break at EOF would otherwise fail them. Any other final token is kept.
    if (have_current && current.type != tok::whitespace)
    {
        cur.tokens.push_back(std::move(current));
    }
    return ok;
}

[[nodiscard]] bool at_end(const DbcCursor& cur) noexcept { return cur.pos >= cur.tokens.size(); }

[[nodiscard]] bool is_section_ending(const DbcToken* t, bool newline_is_ending = false) noexcept
{
    if (!t) { return true; }
    const int needed = newline_is_ending ? 1 : 2;
    return t->type == tok::semicolon || (t->type == tok::whitespace && t->line_breaks >= needed);
}

// Skipped whitespace stays consumed even when no token matched (as before the port).
[[nodiscard]] const DbcToken* read_token(DbcCursor& cur, uint32_t mask, bool skip_whitespace = true,
                                         bool skip_section_ending = false, bool newline_is_ending = false)
{
    while (!at_end(cur))
    {
        const DbcToken& t = cur.tokens[cur.pos];
        if (t.type & mask)
        {
            cur.pos++;
            return &t;
        }
        if (is_section_ending(&t, newline_is_ending))
        {
            if (!skip_section_ending) { return nullptr; }
            cur.pos++;
        }
        else if (skip_whitespace && t.type == tok::whitespace)
        {
            cur.pos++;
        }
        else
        {
            return nullptr;
        }
    }
    return nullptr;
}

[[nodiscard]] bool expect_section_ending(DbcCursor& cur, bool newline_is_ending = false)
{
    if (at_end(cur)) { return true; }
    const DbcToken* t = read_token(cur, tok::whitespace | tok::semicolon);
    return t && is_section_ending(t, newline_is_ending);
}

// --- silent readers: optional parts, nothing is reported ------------------------------

[[nodiscard]] bool skip(DbcCursor& cur, uint32_t type, bool skip_whitespace = true)
{
    return read_token(cur, type, skip_whitespace) != nullptr;
}

[[nodiscard]] bool read_data(DbcCursor& cur, uint32_t type, std::string& out, bool skip_whitespace = true,
                             bool skip_section_ending = false, bool newline_is_ending = false)
{
    const DbcToken* t = read_token(cur, type, skip_whitespace, skip_section_ending, newline_is_ending);
    if (!t) { return false; }
    out = t->data;
    return true;
}

[[nodiscard]] bool read_identifier(DbcCursor& cur, std::string& out, bool newline_is_ending = false)
{
    return read_data(cur, tok::identifier, out, true, false, newline_is_ending);
}

// Removes '\' escapes, then the surrounding quotes.
[[nodiscard]] bool read_string(DbcCursor& cur, std::string& out)
{
    std::string quoted;
    if (!read_data(cur, tok::string, quoted)) { return false; }

    std::string unescaped;
    for (size_t i = 0; i < quoted.size(); ++i)
    {
        if (quoted[i] == '\\' && i + 1 < quoted.size() && quoted[i + 1] != '\n')
        {
            ++i;
        }
        unescaped.push_back(quoted[i]);
    }
    if (unescaped.size() < 2) { return false; }
    out = unescaped.substr(1, unescaped.size() - 2);
    return true;
}

// Optional sign token followed by a number token.
[[nodiscard]] bool read_number(DbcCursor& cur, std::string& out)
{
    out.clear();
    if (skip(cur, tok::minus))
    {
        out = "-";
    }
    else
    {
        (void)skip(cur, tok::plus);
    }
    std::string digits;
    if (!read_data(cur, tok::number, digits)) { return false; }
    out += digits;
    return true;
}

// --- reporting readers: required parts, a miss is an error of the current statement ---

// Records "<statement>: <what>" at the statement's line; always false, for `return fail(...)`.
[[nodiscard]] bool fail(DbcCursor& cur, std::string_view what)
{
    report(cur, cur.stmt_line, std::format("{}: {}", cur.stmt, what));
    return false;
}

[[nodiscard]] bool need(DbcCursor& cur, uint32_t type, std::string_view what)
{
    return skip(cur, type) || fail(cur, std::format("expected {}", what));
}

[[nodiscard]] bool need_identifier(DbcCursor& cur, std::string& out, std::string_view what)
{
    return read_identifier(cur, out) || fail(cur, std::format("expected {}", what));
}

[[nodiscard]] bool need_string(DbcCursor& cur, std::string& out, std::string_view what)
{
    return read_string(cur, out) || fail(cur, std::format("expected {} (quoted string)", what));
}

template <typename T>
[[nodiscard]] bool need_value(DbcCursor& cur, T& out, std::string_view what)
{
    std::string s;
    if (!read_number(cur, s)) { return fail(cur, std::format("expected {}", what)); }
    if (!parse_number(s, out)) { return fail(cur, std::format("invalid {} '{}'", what, s)); }
    return true;
}

[[nodiscard]] bool need_section_ending(DbcCursor& cur, bool newline_is_ending = false)
{
    return expect_section_ending(cur, newline_is_ending) || fail(cur, "expected end of statement");
}

// Starts a statement at the keyword token just read (cur.pos - 1).
void stmt_begin(DbcCursor& cur, const DbcToken& keyword)
{
    cur.stmt = keyword.data;
    cur.stmt_line = keyword.line;
    cur.stmt_pos = cur.pos - 1;
}

// --- recovery --------------------------------------------------------------------------

void skip_until_section_ending(DbcCursor& cur)
{
    while (!at_end(cur))
    {
        const DbcToken* t = read_token(cur, tok::all, false, false);
        if (!t || is_section_ending(t)) { return; }
    }
}

// After a failed section: forget what it consumed and skip from behind its keyword to
// the section ending, so the next section starts clean.
void recover_section(DbcCursor& cur)
{
    cur.pos = cur.stmt_pos + 1;
    skip_until_section_ending(cur);
}

// After a failed SG_ line: back to the end of that line (the first line break or ';'
// behind the keyword), left unconsumed so the BO_ loop sees a possible section ending.
void recover_line(DbcCursor& cur)
{
    cur.pos = cur.stmt_pos + 1;
    while (!at_end(cur))
    {
        const DbcToken& t = cur.tokens[cur.pos];
        if (t.type == tok::semicolon || (t.type == tok::whitespace && t.line_breaks > 0)) { return; }
        cur.pos++;
    }
}

// --- sections --------------------------------------------------------------------------

[[nodiscard]] bool parse_identifier_list(DbcCursor& cur, std::vector<std::string>& list, bool newline_is_ending = false)
{
    if (!need(cur, tok::colon, "':'")) { return false; }
    std::string id;
    while (read_identifier(cur, id, newline_is_ending))
    {
        list.push_back(id);
    }
    return need_section_ending(cur, newline_is_ending);
}

[[nodiscard]] bool parse_version(DbcCursor& cur)
{
    std::string ignored;   // nothing reads the version
    return need_string(cur, ignored, "version") && need_section_ending(cur);
}

[[nodiscard]] bool parse_bs(DbcCursor& cur)
{
    return need(cur, tok::colon, "':'") && need_section_ending(cur);
}

[[nodiscard]] bool parse_bu(DbcCursor& cur)
{
    std::vector<std::string> ignored;   // node names: nothing reads them
    return parse_identifier_list(cur, ignored, true);
}

// The SG_ keyword has been read; `sig` is appended to `msg` only when the line is complete.
[[nodiscard]] bool parse_bo_sg(DbcCursor& cur, CanDbMessage& msg)
{
    CanDbSignal sig;
    if (!need_identifier(cur, sig.name, "signal name")) { return false; }
    cur.stmt += " " + sig.name;

    std::string mux;
    if (read_identifier(cur, mux))
    {
        if (mux == "M")
        {
            sig.is_muxer = true;
        }
        else if (mux.starts_with('m') && parse_number(std::string_view(mux).substr(1), sig.mux_value))
        {
            sig.is_muxed = true;
        }
        else
        {
            return fail(cur, std::format("invalid multiplexer indicator '{}' (M or m<value>)", mux));
        }
    }

    int start_bit = 0;
    int length = 0;
    int byte_order = 0;
    if (!need(cur, tok::colon, "':'")) { return false; }
    if (!need_value(cur, start_bit, "start bit")) { return false; }
    sig.start_bit = static_cast<uint16_t>(start_bit);
    if (!need(cur, tok::pipe, "'|' and length")) { return false; }
    if (!need_value(cur, length, "length")) { return false; }
    sig.length = static_cast<uint16_t>(length);
    if (!need(cur, tok::at, "'@' and byte order")) { return false; }
    if (!need_value(cur, byte_order, "byte order")) { return false; }
    sig.big_endian = (byte_order == 0);

    // Motorola start bit (MSB in DBC sawtooth numbering) -> sequential MSB-first index.
    if (sig.big_endian)
    {
        const uint16_t row = sig.start_bit >> 3;
        const uint16_t column = sig.start_bit & 0b111;
        sig.start_bit = static_cast<uint16_t>((row * 8) + (7 - column));
    }

    if (skip(cur, tok::plus))
    {
        sig.is_unsigned = true;
    }
    else if (skip(cur, tok::minus))
    {
        sig.is_unsigned = false;
    }
    else
    {
        return fail(cur, "expected sign '+' or '-'");
    }

    if (!need(cur, tok::parenth_open, "'(' factor,offset ')'")) { return false; }
    if (!need_value(cur, sig.factor, "factor")) { return false; }
    if (!need(cur, tok::comma, "',' and offset")) { return false; }
    if (!need_value(cur, sig.offset, "offset")) { return false; }
    if (!need(cur, tok::parenth_close, "')'")) { return false; }

    if (!need(cur, tok::bracket_open, "'[' min|max ']'")) { return false; }
    if (!need_value(cur, sig.min, "minimum")) { return false; }
    if (!need(cur, tok::pipe, "'|' and maximum")) { return false; }
    if (!need_value(cur, sig.max, "maximum")) { return false; }
    if (!need(cur, tok::bracket_close, "']'")) { return false; }

    if (!need_string(cur, sig.unit, "unit")) { return false; }

    // Receivers are validated but not stored.
    std::string receiver;
    if (!need_identifier(cur, receiver, "receiver")) { return false; }
    while (skip(cur, tok::comma, false))
    {
        if (!need_identifier(cur, receiver, "receiver after ','")) { return false; }
    }

    msg.signals.push_back(std::move(sig));
    if (msg.signals.back().is_muxer)
    {
        msg.muxer = static_cast<int>(msg.signals.size() - 1);
    }
    return true;
}

[[nodiscard]] bool parse_bo(DbcCursor& cur, CanDb& db)
{
    long long can_id = 0;
    int dlc = 0;
    std::string name;
    std::string sender;

    if (!need_value(cur, can_id, "message id")) { return false; }
    cur.stmt += " " + std::to_string(can_id);
    if (!need_identifier(cur, name, "message name")) { return false; }
    if (!need(cur, tok::colon, "':' after message name")) { return false; }
    if (!need_value(cur, dlc, "dlc")) { return false; }
    if (!need_identifier(cur, sender, "sender")) { return false; }
    if (sender == "Vector__XXX") { sender.clear(); } // the DBC placeholder for "no node"

    const auto raw_id = static_cast<uint32_t>(can_id);
    CanDbMessage& msg = db.messages[raw_id];
    msg = CanDbMessage{
        .name = name,
        .raw_id = raw_id,
        .dlc = static_cast<uint8_t>(dlc),
        .sender = sender,
    };

    const std::string bo_stmt = cur.stmt;
    const int bo_line = cur.stmt_line;
    const size_t bo_pos = cur.stmt_pos;
    bool ok = true;
    while (!expect_section_ending(cur))
    {
        const DbcToken* sub = read_token(cur, tok::identifier);
        if (!sub || sub->data != "SG_")
        {
            std::string got = "end of file";
            if (sub) { got = sub->data; }
            else if (!at_end(cur)) { got = cur.tokens[cur.pos].data; }
            return fail(cur, std::format("expected SG_ or end of message, got '{}'", got));
        }
        stmt_begin(cur, *sub);
        if (!parse_bo_sg(cur, msg))
        {
            recover_line(cur);
            ok = false;
        }
        cur.stmt = bo_stmt;
        cur.stmt_line = bo_line;
        cur.stmt_pos = bo_pos;
    }
    // Bad signal lines are dropped, the message and its good signals are kept; the
    // caller's recover_section() then lands on the same section ending this loop did.
    return ok;
}

// The message a CM_, VAL_ or SIG_VALTYPE_ line names. Some tools write an extended id there as the
// bare 29-bit id, without the bit 31 its BO_ carries; cantools reads those files too.
CanDbMessage* find_referenced_message(CanDb& db, long long id)
{
    CanDbMessage* msg = can_db_find_message(db, static_cast<uint32_t>(id));
    if (msg == nullptr && id >= 0 && id <= 0x1FFFFFFF)
    {
        msg = can_db_find_message(db, static_cast<uint32_t>(id) | 0x80000000u);
    }
    return msg;
}

[[nodiscard]] bool parse_cm(DbcCursor& cur, CanDb& db)
{
    std::string s;
    if (read_string(cur, s))   // file comment: not kept
    {
        return true;
    }

    std::string kind;
    std::string id;
    long long ll = 0;
    if (!need_identifier(cur, kind, "BU_, BO_ or SG_ or a quoted string")) { return false; }
    cur.stmt += " " + kind;

    if (kind == "BU_")
    {
        if (!need_identifier(cur, id, "node name") || !need_string(cur, s, "comment")) { return false; }   // node comment: not kept
        return need_section_ending(cur);
    }
    if (kind == "BO_")
    {
        if (!need_value(cur, ll, "message id") || !need_string(cur, s, "comment")) { return false; }
        CanDbMessage* msg = find_referenced_message(db, ll);
        if (!msg) { return fail(cur, std::format("unknown message {}", ll)); }
        msg->comment = s;
        return need_section_ending(cur);
    }
    if (kind == "SG_")
    {
        if (!need_value(cur, ll, "message id")) { return false; }
        if (!need_identifier(cur, id, "signal name")) { return false; }
        CanDbMessage* msg = find_referenced_message(db, ll);
        if (!msg) { return fail(cur, std::format("unknown message {}", ll)); }
        CanDbSignal* sig = can_db_find_signal(*msg, id);
        if (!sig) { return fail(cur, std::format("unknown signal {} of message {}", id, msg->name)); }
        if (!need_string(cur, s, "comment")) { return false; }
        sig->comment = s;
        return need_section_ending(cur);
    }
    return fail(cur, std::format("unknown comment target '{}'", kind));
}

[[nodiscard]] bool parse_val(DbcCursor& cur, CanDb& db)
{
    long long can_id = 0;
    std::string signal_name;
    if (!need_value(cur, can_id, "message id")) { return false; }
    cur.stmt += " " + std::to_string(can_id);
    if (!need_identifier(cur, signal_name, "signal name")) { return false; }
    CanDbMessage* msg = find_referenced_message(db, can_id);
    CanDbSignal* sig = msg ? can_db_find_signal(*msg, signal_name) : nullptr;
    if (!sig)
    {
        // A value table left over from a signal that is gone: skipped, as SIG_VALTYPE_ and
        // other DBC tools do, rather than refusing the whole file.
        log_warning(std::format("dbc: VAL_ for unknown signal {} of message {}", signal_name, can_id));
    }

    while (!skip(cur, tok::semicolon))
    {
        long long value = 0;
        std::string name;
        if (!need_value(cur, value, "value or ';'") || !need_string(cur, name, "value name")) { return false; }
        if (sig)
        {
            sig->value_table[static_cast<uint64_t>(value)] = name;
        }
    }
    return true;
}

// SIG_VALTYPE_ <id> <signal> : 1|2 ;  (1 = IEEE float, 2 = IEEE double)
[[nodiscard]] bool parse_sig_valtype(DbcCursor& cur, CanDb& db)
{
    long long can_id = 0;
    std::string signal_name;
    int type = 0;
    if (!need_value(cur, can_id, "message id") || !need_identifier(cur, signal_name, "signal name")) { return false; }
    if (!need(cur, tok::colon, "':'") || !need_value(cur, type, "value type")) { return false; }
    CanDbMessage* msg = find_referenced_message(db, can_id);
    CanDbSignal* sig = msg ? can_db_find_signal(*msg, signal_name) : nullptr;
    if (!sig)
    {
        log_warning(std::format("dbc: SIG_VALTYPE_ for unknown signal {} of message {}", signal_name, can_id));
    }
    else if ((type == 1 && sig->length == 32) || (type == 2 && sig->length == 64))
    {
        sig->value_type = type == 1 ? SignalValueType::float32 : SignalValueType::float64;
    }
    else if (type != 0)
    {
        log_warning(std::format("dbc: SIG_VALTYPE_ {} of {} needs length {}, has {}; read as integer",
                                type, signal_name, type == 1 ? 32 : 64, sig->length));
    }
    return need_section_ending(cur);
}

// Every section, bad ones skipped. false when any failed.
[[nodiscard]] bool parse_sections(DbcCursor& cur, CanDb& db)
{
    while (!at_end(cur))
    {
        const DbcToken* keyword = read_token(cur, tok::identifier, true, true);
        if (!keyword)
        {
            if (at_end(cur)) { break; }   // only section endings were left
            const DbcToken& t = cur.tokens[cur.pos];
            report(cur, t.line, std::format("unexpected '{}', expected a section keyword", t.data));
            cur.pos++;
            skip_until_section_ending(cur);
            continue;
        }
        stmt_begin(cur, *keyword);
        const std::string& section = keyword->data;

        bool ok = true;
        if (section == "VERSION") { ok = parse_version(cur); }
        else if (section == "NS_")
        {
            std::vector<std::string> ignored;
            ok = parse_identifier_list(cur, ignored);
        }
        else if (section == "BS_") { ok = parse_bs(cur); }
        else if (section == "BU_") { ok = parse_bu(cur); }
        else if (section == "BO_") { ok = parse_bo(cur, db); }
        else if (section == "CM_") { ok = parse_cm(cur, db); }
        else if (section == "VAL_") { ok = parse_val(cur, db); }
        else if (section == "SIG_VALTYPE_") { ok = parse_sig_valtype(cur, db); }
        else { skip_until_section_ending(cur); }

        if (!ok) { recover_section(cur); }
    }
    return !cur.failed;
}

[[nodiscard]] bool parse_text(std::string_view text, CanDb& db, std::string_view source, const std::string* path,
                              std::vector<DbcError>* errors)
{
    DbcCursor cur;
    std::vector<DbcError> found;
    cur.errors = &found;
    (void)tokenize(text, cur);
    if (path) { db.path = *path; }
    (void)parse_sections(cur, db);

    if (found.empty()) { return true; }
    for (const DbcError& e : found)
    {
        log_error(std::format("dbc parse error in {} at line {}: {}", source, e.line, e.message));
    }
    if (errors) { errors->insert(errors->end(), found.begin(), found.end()); }
    return false;
}

} // namespace

bool dbc_parse(std::string_view text, CanDb& db, std::vector<DbcError>* errors)
{
    return parse_text(text, db, "<text>", nullptr, errors);
}

bool dbc_parse_file(const std::filesystem::path& path, CanDb& db, std::vector<DbcError>* errors)
{
    const std::string name = path.string();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        log_error(std::format("error parsing dbc file {}: cannot open", name));
        if (errors) { errors->push_back(DbcError{.line = 0, .message = "cannot open file"}); }
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse_text(text, db, name, &name, errors);
}
