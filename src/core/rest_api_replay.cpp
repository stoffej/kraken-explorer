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

// REST routes of the Replay view, the Trace window (filter, view, goto, frames) and Value Search
// (hits, raw search). Everything acts on the current workspace tab.

#include "core/rest_api_routes.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <ranges>

#include <imgui.h>

#include "app.h"
#include "core/charconv.h"
#include "core/rest_api.h"
#include "core/text.h"
#include "db/model/can_db.h"
#include "ui/replay.h"
#include "ui/trace_window.h"
#include "ui/value_search.h"
#include "ui/workspace_tabs.h"

namespace
{

std::string error(int& status, int code, std::string_view text)
{
    status = code;
    return std::format("{{\"error\":{}}}", json_string(text));
}

// "a=1&b=x" -> {"a":"1","b":"x"} for json_get. ponytail: no %XX decoding; numbers and names only.
std::string query_json(std::string_view q)
{
    std::string out;
    for (const auto part : std::views::split(q, '&'))
    {
        const std::string_view kv(part.begin(), part.end());
        const std::size_t eq = kv.find('=');
        out += std::format("{}{}:{}", out.empty() ? "{" : ",", json_string(kv.substr(0, eq)),
                           json_string(eq == std::string_view::npos ? "" : kv.substr(eq + 1)));
    }
    return out.empty() ? "{}" : out + "}";
}

std::optional<uint64_t> number(std::string_view v)
{
    uint64_t n = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    return ec == std::errc{} && end == v.data() + v.size() ? std::optional(n) : std::nullopt;
}

// "0x123" or "123" (hex, as the Trace window shows ids).
std::optional<uint32_t> hex_id(std::string_view v)
{
    v = trim(v);
    if (v.starts_with("0x") || v.starts_with("0X"))
    {
        v.remove_prefix(2);
    }
    uint32_t id = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), id, 16);
    return ec == std::errc{} && end == v.data() + v.size() && !v.empty() ? std::optional(id) : std::nullopt;
}

const Iface* iface_by_name(const App& app, std::string_view name)
{
    const auto it = std::ranges::find(app.ifaces, name, [](const Iface& i) { return std::string_view(i.info.name); });
    return it == app.ifaces.end() ? nullptr : &*it;
}

std::string_view iface_label(const App& app, int index)
{
    return index >= 0 && static_cast<std::size_t>(index) < app.ifaces.size() ? std::string_view(app.ifaces[static_cast<std::size_t>(index)].info.name)
                                                                               : std::string_view{};
}

void focus(const WorkspaceTab& tab, const char* title)
{
    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::SetWindowFocus(workspace_window_name(tab, title).c_str());
    }
}

std::string json_number(double v)
{
    return std::isfinite(v) ? std::format("{}", v) : "null";
}

// --- Replay --------------------------------------------------------------------------------

std::string replay_json(App& app, Replay& r)
{
    replay_load_poll(app, r); // the window does it every frame; a closed window does not draw
    const std::span<const FrameCacheRec> all = replay_frames(r.data);
    std::string channels;
    for (std::size_t ch = 0; ch < r.data.channels.size(); ++ch)
    {
        const int target = ch < r.mapping.size() ? r.mapping[ch] : replay_trace_only;
        const bool on = std::ranges::any_of(r.data.rows, [&](const ReplayIdRow& row) { return row.channel == ch && (row.rx_on || row.tx_on); });
        channels += std::format("{}{{\"channel\":{},\"name\":{},\"iface\":{},\"enabled\":{}}}", channels.empty() ? "" : ",", ch,
                                json_string(r.data.channels[ch]), json_string(iface_label(app, target)), on);
    }
    const bool running = r.running;
    const double duration = all.empty() ? 0.0 : static_cast<double>(all.back().ts_ns - all.front().ts_ns) / 1e9;
    return std::format("{{\"open\":{},\"path\":{},\"info\":{},\"loading\":{},\"load_fraction\":{},\"load_frames\":{},"
                       "\"frames\":{},\"duration\":{},\"channels\":[{}],\"running\":{},\"paused\":{},\"position\":{},"
                       "\"play_frames\":{},\"speed\":{},\"fast\":{},\"loop\":{},\"autoplay\":{},\"from\":{},\"to\":{},"
                       "\"break_at\":{}}}",
                       r.open, json_string(r.data.path), json_string(r.data.info), r.loader.joinable(),
                       json_number(r.load_fraction.load()), r.load_frames.load(), all.size(), json_number(duration), channels,
                       running, running && r.hold == replay_paused, r.position.load(),
                       running || r.position > 0 ? r.play_frames.size() : replay_range(all, r.range_from, r.range_to).size(),
                       json_number(r.speed), r.fast, r.loop, r.autoplay, json_string(r.range_from), json_string(r.range_to),
                       json_string(r.break_at));
}

// POST /replay/config: any of speed, fast, loop, autoplay, from, to, break_at.
std::string replay_config(App& app, Replay& r, std::string_view body, int& status)
{
    const std::string speed = json_get(body, "speed");
    const std::string from = json_get(body, "from");
    const std::string to = json_get(body, "to");
    float s = r.speed;
    if (!speed.empty() && (float_from_chars(speed.data(), speed.data() + speed.size(), s).ptr != speed.data() + speed.size()
                           || !(s >= 0.1f && s <= 10.0f)))
    {
        return error(status, 400, "speed: 0.1 .. 10");
    }
    for (const std::string* t : {&from, &to})
    {
        if (!t->empty() && !parse_duration(*t))
        {
            return error(status, 400, "from / to: time since the first frame, \"90\", \"1:30\", \"2d 1:02:03\"");
        }
    }
    const auto has = [&](std::string_view key) { return body.find(std::format("\"{}\"", key)) != std::string_view::npos; };
    if (r.running && std::ranges::any_of(std::initializer_list<std::string_view>{"speed", "fast", "loop", "from", "to", "break_at"}, has))
    {
        return error(status, 409, "stop the replay first (fixed while playing; only autoplay changes)");
    }
    r.speed = s;
    const auto flag = [&](const char* key, bool& out)
    {
        const std::string v = json_get(body, key);
        out = v.empty() ? out : v == "true";
    };
    flag("fast", r.fast);
    flag("loop", r.loop);
    flag("autoplay", r.autoplay);
    if (has("from"))
    {
        r.range_from = from;
    }
    if (has("to"))
    {
        r.range_to = to;
    }
    if (has("break_at"))
    {
        r.break_at = json_get(body, "break_at");
    }
    return replay_json(app, r);
}

// POST /replay/channel {"channel":"vcan0" or 0, "iface":"vcan1" ("" = trace only), "enabled":true}
std::string replay_channel(App& app, Replay& r, std::string_view body, int& status)
{
    replay_load_poll(app, r);
    if (r.running)
    {
        return error(status, 409, "stop the replay first (fixed while playing)");
    }
    const std::string name = json_get(body, "channel");
    auto it = std::ranges::find(r.data.channels, name);
    std::size_t ch = static_cast<std::size_t>(it - r.data.channels.begin());
    if (it == r.data.channels.end())
    {
        const auto n = number(name);
        if (!n || *n >= r.data.channels.size())
        {
            return error(status, 400, "no such channel (see GET /replay)");
        }
        ch = *n;
    }
    if (ch >= r.mapping.size())
    {
        return error(status, 409, "the file is still loading");
    }
    if (body.find("\"iface\"") != std::string_view::npos)
    {
        const std::string target = json_get(body, "iface");
        if (target.empty())
        {
            r.mapping[ch] = replay_trace_only;
        }
        else
        {
            const Iface* i = iface_by_name(app, target);
            if (i == nullptr || i->info.bus_type == BusType::LIN)
            {
                return error(status, 400, "iface: a CAN interface (see /status), or \"\" for trace only");
            }
            if (ch < r.data.channel_lin.size() && r.data.channel_lin[ch] != 0)
            {
                return error(status, 400, "LIN frames can only be replayed to the trace");
            }
            r.mapping[ch] = i->index;
        }
    }
    if (const std::string on = json_get(body, "enabled"); !on.empty())
    {
        for (ReplayIdRow& row : r.data.rows)
        {
            if (row.channel == ch)
            {
                row.rx_on = on == "true" && row.has_rx;
                row.tx_on = on == "true" && row.has_tx;
            }
        }
    }
    return replay_json(app, r);
}

std::optional<std::string> replay_routes(App& app, WorkspaceTab& tab, std::string_view method, std::string_view path,
                                         std::string_view body, int& status)
{
    Replay& r = app.replays[tab.uid];
    if (method == "GET" && path == "/replay")
    {
        return replay_json(app, r);
    }
    if (method != "POST")
    {
        return std::nullopt;
    }
    if (path == "/replay/load")
    {
        const std::string file = json_get(body, "path");
        std::error_code ec;
        if (file.empty() || !std::filesystem::is_regular_file(file, ec))
        {
            return error(status, 400, "path: an existing trace file (.asc, .blf, .candump, .log, .mf4, .pcap, .pcapng, .trc)");
        }
        r.open = true; // docks next to the Log on its first draw (draw_replay)
        replay_load(app, r, std::filesystem::absolute(file, ec).string());
        focus(tab, "Replay");
        return replay_json(app, r);
    }
    if (path == "/replay/open" || path == "/replay/close")
    {
        r.open = path == "/replay/open";
        if (r.open)
        {
            focus(tab, "Replay");
        }
        return replay_json(app, r);
    }
    if (path == "/replay/config")
    {
        return replay_config(app, r, body, status);
    }
    if (path == "/replay/channel")
    {
        return replay_channel(app, r, body, status);
    }
    replay_load_poll(app, r);
    const bool paused = r.running && r.hold == replay_paused;
    if (path == "/replay/play" || path == "/replay/step")
    {
        if (path == "/replay/step" && paused)
        {
            replay_single_step(r);
            return replay_json(app, r);
        }
        if (r.running)
        {
            return error(status, 409, path == "/replay/play" ? "already playing (stop first)" : "pause first");
        }
        if (replay_frames(r.data).empty())
        {
            return error(status, 409, r.loader.joinable() ? "the file is still loading" : "no trace file loaded (POST /replay/load)");
        }
        r.open = true; // closing the window stops the player (draw_replay)
        replay_start(r, app.ifaces, app.tasks, path == "/replay/step" ? replay_stepping : replay_playing);
        return replay_json(app, r);
    }
    if (path == "/replay/pause" || path == "/replay/resume")
    {
        if (!r.running)
        {
            return error(status, 409, "not playing");
        }
        path == "/replay/pause" ? replay_pause(r) : replay_resume(r);
        return replay_json(app, r);
    }
    if (path == "/replay/stop")
    {
        replay_stop(r);
        return replay_json(app, r);
    }
    return std::nullopt;
}

// --- Trace window --------------------------------------------------------------------------

constexpr std::string_view tab_names[] = {"monitor", "uds", "j1939"};

std::string trace_filter_json(const App& app, const TraceWindowState& s)
{
    std::string ids;
    for (const uint32_t id : s.filter.hidden_ids)
    {
        ids += std::format("{}\"0x{:X}\"", ids.empty() ? "" : ",", id);
    }
    std::string ifaces;
    for (const uint16_t i : s.filter.hidden_ifaces)
    {
        ifaces += std::format("{}{}", ifaces.empty() ? "" : ",", json_string(iface_label(app, i)));
    }
    return std::format("{{\"text\":{},\"rx\":{},\"tx\":{},\"hidden_ids\":[{}],\"hidden_ifaces\":[{}],\"tab\":\"{}\",\"view\":\"{}\"}}",
                       json_string(s.filter_edit), s.filter.show_rx, s.filter.show_tx, ids, ifaces,
                       tab_names[static_cast<int>(s.tab)],
                       s.modes[static_cast<int>(s.tab)] == TraceViewMode::Rolling ? "log" : "monitor");
}

// POST /trace/filter: text, rx, tx, hidden_ids ("0x123, 7E8"), hidden_ifaces ("vcan0, vcan1");
// a missing key keeps its setting.
std::string trace_filter(const App& app, TraceWindowState& s, std::string_view body, int& status)
{
    std::set<uint32_t> ids = s.filter.hidden_ids;
    std::set<uint16_t> ifaces = s.filter.hidden_ifaces;
    if (body.find("\"hidden_ids\"") != std::string_view::npos)
    {
        ids.clear();
        const std::string hidden_ids = json_get(body, "hidden_ids");
        for (const auto part : std::views::split(std::string_view(hidden_ids), ','))
        {
            const std::string_view t = trim(std::string_view(part.begin(), part.end()));
            const auto id = hex_id(t);
            if (!t.empty() && !id)
            {
                return error(status, 400, "hidden_ids: hex ids, comma separated (\"0x123, 7E8\")");
            }
            if (id)
            {
                ids.insert(*id);
            }
        }
    }
    if (body.find("\"hidden_ifaces\"") != std::string_view::npos)
    {
        ifaces.clear();
        const std::string hidden_ifaces = json_get(body, "hidden_ifaces");
        for (const auto part : std::views::split(std::string_view(hidden_ifaces), ','))
        {
            const std::string_view t = trim(std::string_view(part.begin(), part.end()));
            const Iface* i = iface_by_name(app, t);
            if (!t.empty() && i == nullptr)
            {
                return error(status, 400, std::format("hidden_ifaces: no such interface \"{}\" (see /status)", t));
            }
            if (i != nullptr)
            {
                ifaces.insert(i->index);
            }
        }
    }
    s.filter.hidden_ids = std::move(ids);
    s.filter.hidden_ifaces = std::move(ifaces);
    if (body.find("\"text\"") != std::string_view::npos)
    {
        s.filter_edit = s.filter.text = json_get(body, "text");
        s.filter_edited = {};
    }
    const std::string rx = json_get(body, "rx");
    const std::string tx = json_get(body, "tx");
    s.filter.show_rx = rx.empty() ? s.filter.show_rx : rx == "true";
    s.filter.show_tx = tx.empty() ? s.filter.show_tx : tx == "true";
    s.filter_dirty = true;
    return trace_filter_json(app, s);
}

// POST /trace/view {"tab":"monitor|uds|j1939","view":"monitor|log"}: view applies to that tab.
std::string trace_view(const App& app, TraceWindowState& s, std::string_view body, int& status)
{
    int tab = static_cast<int>(s.tab);
    if (const std::string t = json_get(body, "tab"); !t.empty())
    {
        const auto it = std::ranges::find(tab_names, t);
        if (it == std::end(tab_names))
        {
            return error(status, 400, "tab: monitor, uds or j1939");
        }
        tab = static_cast<int>(it - std::begin(tab_names));
        s.tab = static_cast<TraceTab>(tab);
        s.tab_goto = tab;
    }
    if (const std::string v = json_get(body, "view"); !v.empty())
    {
        if (v != "log" && v != "monitor")
        {
            return error(status, 400, "view: monitor (aggregated) or log");
        }
        s.modes[tab] = v == "log" ? TraceViewMode::Rolling : TraceViewMode::Aggregated;
        s.scroll_pending = true;
        s.selected = -1;
    }
    return trace_filter_json(app, s);
}

// GET /trace/frames?last=N or ?from=INDEX&count=N, decode=false for raw frames only.
std::string trace_frames(const App& app, std::string_view q, int& status)
{
    const Trace& t = app.trace;
    const std::string from_text = json_get(q, "from");
    const std::string count_text = json_get(q, "count");
    const std::string last_text = json_get(q, "last");
    const auto from = number(from_text);
    const auto count = number(count_text.empty() ? "100" : count_text);
    const auto last = number(last_text.empty() ? "20" : last_text);
    if ((!from_text.empty() && !from) || !count || !last)
    {
        return error(status, 400, "from, count, last: whole numbers");
    }
    constexpr uint64_t max_frames = 1000; // ponytail: one page per request; ask again with from
    uint64_t begin = 0;
    uint64_t end = 0;
    if (from)
    {
        begin = std::clamp(*from, t.begin, t.end);
        end = begin + std::min({*count, max_frames, t.end - begin});
    }
    else
    {
        end = t.end;
        begin = end - std::min({*last, max_frames, trace_size(t)});
    }
    const bool decode = json_get(q, "decode") != "false";
    const int64_t t0 = trace_size(t) > 0 ? trace_at(t, t.begin).ts_ns : 0;
    std::string out;
    std::string data;
    for (uint64_t i = begin; i < end; ++i)
    {
        const BusMessage m = trace_at(t, i);
        data.clear();
        append_hex_bytes(data, std::span(m.data.data(), m.len), " ");
        std::string signals;
        const CanDbMessage* msg = decode && m.type == BusType::CAN ? setup_find_can_message(app.setup, m) : nullptr;
        if (msg != nullptr)
        {
            for (const CanDbSignal& sig : msg->signals)
            {
                if (can_signal_present(*msg, sig, m))
                {
                    signals += std::format("{}{}:{}", signals.empty() ? "" : ",", json_string(sig.name),
                                           json_number(can_signal_extract_physical(sig, m)));
                }
            }
        }
        out += std::format("{}{{\"index\":{},\"ts_ns\":{},\"t\":{},\"iface\":{},\"id\":\"0x{:X}\",\"extended\":{},\"fd\":{},"
                           "\"tx\":{},\"error\":{},\"len\":{},\"data\":{}",
                           out.empty() ? "" : ",", i, m.ts_ns, json_number(static_cast<double>(m.ts_ns - t0) / 1e9),
                           json_string(iface_label(app, m.iface)), can_id(m), has_flag(m, bus_flag::extended),
                           has_flag(m, bus_flag::fd), has_flag(m, bus_flag::tx), is_error_frame(m), m.len, json_string(data));
        out += msg != nullptr ? std::format(",\"name\":{},\"signals\":{{{}}}}}", json_string(msg->name), signals) : "}";
    }
    return std::format("{{\"begin\":{},\"end\":{},\"frames\":[{}]}}", t.begin, t.end, out);
}

std::optional<std::string> trace_routes(App& app, WorkspaceTab& tab, std::string_view method, std::string_view path,
                                        std::string_view query, std::string_view body, int& status)
{
    TraceWindowState& s = app.trace_windows[tab.uid];
    if (method == "GET" && path == "/trace/frames")
    {
        return trace_frames(app, query, status);
    }
    if (method == "GET" && path == "/trace/filter")
    {
        return trace_filter_json(app, s);
    }
    if (method != "POST")
    {
        return std::nullopt;
    }
    if (path == "/trace/filter")
    {
        return trace_filter(app, s, body, status);
    }
    if (path == "/trace/view")
    {
        return trace_view(app, s, body, status);
    }
    if (path == "/trace/goto")
    {
        const auto index = number(json_get(body, "index"));
        if (!index || *index < app.trace.begin || *index >= app.trace.end)
        {
            return error(status, 400, std::format("index: a trace index in [{}, {}) (see /trace/frames)", app.trace.begin, app.trace.end));
        }
        trace_window_select_frame(s, app.trace, *index, 3);
        focus(tab, "Trace");
        return std::format("{{\"index\":{}}}", *index);
    }
    return std::nullopt;
}

// --- Value search --------------------------------------------------------------------------

std::string value_search_state(const ValueSearch& v)
{
    return std::format("\"raw\":{},\"signal\":{},\"raw_id\":{},\"raw_data\":{},\"from\":{},\"to\":{},\"scanning\":{},"
                       "\"scanned\":{},\"to_scan\":{},\"status\":{},\"hits\":{},\"truncated\":{}",
                       v.raw, json_string(v.signal ? v.signal->label : ""), json_string(v.raw_id), json_string(v.raw_data),
                       json_string(v.from), json_string(v.to), v.job.valid(), v.scanned.load(), v.to_scan, json_string(v.status),
                       v.hits.size(), v.truncated);
}

// GET /value_search/hits?from=0&count=100
std::string value_search_hits(ValueSearch& v, std::string_view q, int& status)
{
    value_search_poll(v);
    const std::string from_text = json_get(q, "from");
    const std::string count_text = json_get(q, "count");
    const auto from = number(from_text.empty() ? "0" : from_text);
    const auto count = number(count_text.empty() ? "100" : count_text);
    if (!from || !count)
    {
        return error(status, 400, "from, count: whole numbers");
    }
    const std::size_t begin = std::min<std::size_t>(*from, v.hits.size());
    const std::size_t end = begin + std::min<std::size_t>({*count, 10000, v.hits.size() - begin});
    std::string out;
    for (std::size_t i = begin; i < end; ++i)
    {
        const ValueHit& h = v.hits[i];
        out += std::format("{}{{\"index\":{},\"ts_ns\":{},\"value\":{}}}", out.empty() ? "" : ",", h.index, h.ts_ns, json_number(h.value));
    }
    return std::format("{{{},\"list\":[{}]}}", value_search_state(v), out);
}

std::optional<std::string> value_search_routes(App& app, WorkspaceTab& tab, std::string_view method, std::string_view path,
                                               std::string_view query, std::string_view body, int& status)
{
    ValueSearch& v = app.value_searches[tab.uid];
    if (method == "GET" && path == "/value_search/hits")
    {
        return value_search_hits(v, query, status);
    }
    if (method == "POST" && path == "/value_search/raw")
    {
        const std::string id = json_get(body, "id");
        const std::string data = json_get(body, "data");
        if (id.empty() && data.empty())
        {
            return error(status, 400, "id and / or data missing");
        }
        if (!raw_pattern_parse(id, data))
        {
            return error(status, 400, "id: hex (\"123\", \"1ABCDEF0\"); data: hex pairs, ?? for any byte (\"01 ?? FF\")");
        }
        v.raw = true;
        v.raw_id = id;
        v.raw_data = data;
        v.hits.clear();
        v.status.clear();
        raw_search_start(app, v);
        focus(tab, "Value Search");
        return std::format("{{{}}}", value_search_state(v));
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> rest_replay_routes(App& app, std::string_view method, std::string_view full_path, std::string_view body,
                                              int& status)
{
    const std::size_t q = full_path.find('?');
    const std::string_view path = full_path.substr(0, q);
    const bool mine = path == "/replay" || path.starts_with("/replay/") || path == "/trace/filter" || path == "/trace/view"
        || path == "/trace/goto" || path == "/trace/frames" || path == "/value_search/hits" || path == "/value_search/raw";
    if (!mine)
    {
        return std::nullopt;
    }
    WorkspaceTab* tab = workspace_current(app.workspace);
    if (tab == nullptr)
    {
        return error(status, 409, "no workspace tab");
    }
    // GET parameters from the query string, else from a JSON body.
    const std::string query = q == std::string_view::npos ? std::string(body) : query_json(full_path.substr(q + 1));
    status = 200;
    if (path.starts_with("/replay"))
    {
        return replay_routes(app, *tab, method, path, body, status);
    }
    if (path.starts_with("/trace/"))
    {
        return trace_routes(app, *tab, method, path, query, body, status);
    }
    return value_search_routes(app, *tab, method, path, query, body, status);
}
