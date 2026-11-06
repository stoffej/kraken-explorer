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

#include "core/rest_api_routes.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <future>

#include "app.h"
#include "core/charconv.h"
#include "core/rest_api.h"
#include "core/text.h"
#include "core/trace_file_format.h"
#include "ui/graph.h"
#include <imgui.h>

namespace
{

std::string error(int& status, int code, std::string_view text)
{
    status = code;
    return std::format("{{\"error\":{}}}", json_string(text));
}

std::optional<int> json_int(std::string_view body, std::string_view key)
{
    const std::string v = json_get(body, key);
    int out = 0;
    if (v.empty() || std::from_chars(v.data(), v.data() + v.size(), out).ec != std::errc{})
    {
        return std::nullopt;
    }
    return out;
}

// "tab" by title, else the current tab.
WorkspaceTab* find_tab(App& app, std::string_view body)
{
    const std::string title = json_get(body, "tab");
    if (title.empty())
    {
        return workspace_current(app.workspace);
    }
    const auto it = std::ranges::find(app.workspace.tabs, title, &WorkspaceTab::title);
    return it == app.workspace.tabs.end() ? nullptr : &*it;
}

// "Message.Signal", or "Signal" when only one message has it (as /graph does).
std::optional<SignalEntry> find_signal(const Setup& setup, std::string_view name)
{
    SignalSearch s;
    signal_search_update(s, setup, "");
    std::optional<SignalEntry> found;
    for (const SignalEntry& e : s.entries)
    {
        if (e.label == name)
        {
            return e;
        }
        if (e.label.size() > name.size() && e.label.ends_with(name) && e.label[e.label.size() - name.size() - 1] == '.')
        {
            if (found)
            {
                return std::nullopt; // ambiguous
            }
            found = e;
        }
    }
    return found;
}

std::string hex_bytes(const BusMessage& m)
{
    std::string out;
    for (int i = 0; i < m.len; ++i)
    {
        out += std::format("{}{:02X}", i == 0 ? "" : " ", m.data[static_cast<std::size_t>(i)]);
    }
    return out;
}

// ---- Generator: GET /generator, POST /generator/add|remove|enable|signal ----

std::string generator_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    WorkspaceTab* tab = find_tab(app, body);
    if (tab == nullptr)
    {
        return error(status, 400, "no such tab");
    }
    TxGenerator& gen = app.tx_generators[tab->uid];
    tx_generator_start(gen, app.ifaces);
    std::scoped_lock lock(gen.mutex);
    if (sub == "add")
    {
        TxCyclic row{.interval_ms = std::clamp(json_int(body, "interval_ms").value_or(100), 1, 60000)};
        const std::string iface_name = json_get(body, "iface");
        const auto iface = std::ranges::find(app.ifaces, iface_name, [](const Iface& i) { return i.info.name; });
        if (!iface_name.empty() && iface == app.ifaces.end())
        {
            return error(status, 400, "no such interface");
        }
        row.msg.iface = iface != app.ifaces.end() ? iface->index : gen.iface;
        if (const std::string name = json_get(body, "message"); !name.empty())
        {
            for (const SetupNetwork& n : app.setup.networks)
            {
                for (const auto& db : n.can_dbs)
                {
                    for (const auto& [raw_id, m] : db->messages)
                    {
                        if (row.db == nullptr && m.name == name)
                        {
                            row.db = &m;
                        }
                    }
                }
            }
            if (row.db == nullptr)
            {
                return error(status, 400, std::format("no DBC message \"{}\" (load a DBC first?)", name));
            }
            row.name = row.db->name;
            row.msg.id = row.db->raw_id & can_id_mask_extended;
            row.msg.flags = (row.db->raw_id & 0x80000000u) != 0 ? bus_flag::extended : 0;
            set_length(row.msg, row.db->dlc);
            tx_apply_j1939_sa(gen, row.msg);
        }
        else
        {
            const std::string id = json_get(body, "id");
            const bool hex = id.starts_with("0x") || id.starts_with("0X");
            if (id.empty() || std::from_chars(id.data() + (hex ? 2 : 0), id.data() + id.size(), row.msg.id, hex ? 16 : 10).ec != std::errc{}
                || row.msg.id > can_id_mask_extended)
            {
                return error(status, 400, "message (DBC name) or id missing");
            }
            row.name = "Manual";
            row.msg.flags = row.msg.id > can_id_mask_standard || json_get(body, "extended") == "true" ? bus_flag::extended : 0;
            set_length(row.msg, bus_dlc_lengths[bus_length_to_dlc(std::clamp(json_int(body, "dlc").value_or(8), 0, bus_max_data_bytes))]);
        }
        if (row.msg.len > 8)
        {
            row.msg.flags |= bus_flag::fd;
        }
        gen.rows.push_back(row);
        gen.rx_dirty = true;
        gen.changed = true;
        return std::format("{{\"index\":{}}}", gen.rows.size() - 1);
    }
    if (sub.empty())
    {
        std::string rows;
        for (std::size_t i = 0; i < gen.rows.size(); ++i)
        {
            const TxCyclic& r = gen.rows[i];
            rows += std::format("{}{{\"index\":{},\"name\":{},\"id\":{},\"iface\":{},\"interval_ms\":{},\"enabled\":{},\"data\":{}}}",
                                rows.empty() ? "" : ",", i, json_string(r.name), r.msg.id,
                                json_string(r.msg.iface < app.ifaces.size() ? app.ifaces[r.msg.iface].info.name : ""),
                                r.interval_ms, r.enabled, json_string(hex_bytes(r.msg)));
        }
        return std::format("{{\"tab\":{},\"rows\":[{}]}}", json_string(tab->title), rows);
    }
    // The other routes act on one row: "index", or for /signal the first row of the signal's message.
    std::optional<int> index = json_int(body, "index");
    std::optional<SignalEntry> sig;
    if (sub == "signal")
    {
        const std::string name = json_get(body, "signal");
        sig = find_signal(app.setup, name);
        if (!sig || sig->can_sig == nullptr)
        {
            return error(status, 400, std::format("unknown or ambiguous CAN signal \"{}\" (\"Message.Signal\")", name));
        }
        if (!index)
        {
            const auto it = std::ranges::find(gen.rows, sig->can_msg, &TxCyclic::db);
            index = it == gen.rows.end() ? std::nullopt : std::optional<int>(static_cast<int>(it - gen.rows.begin()));
        }
    }
    if (!index || *index < 0 || *index >= static_cast<int>(gen.rows.size()))
    {
        return error(status, 400, "no such row (index, see GET /generator)");
    }
    TxCyclic& row = gen.rows[static_cast<std::size_t>(*index)];
    if (sub == "remove")
    {
        gen.rows.erase(gen.rows.begin() + *index);
        gen.current = -1;
        gen.edit_row = -1;
        gen.rx_dirty = true;
    }
    else if (sub == "enable")
    {
        const bool on = json_get(body, "enabled") != "false";
        if (const auto ms = json_int(body, "interval_ms"))
        {
            row.interval_ms = std::clamp(*ms, 1, 60000);
        }
        // As the row's ▶: cyclic sending needs a measurement and a timed trigger.
        if (on && (!app.measuring || row.trigger == TxTrigger::Manual))
        {
            return error(status, 409, app.measuring ? "a Manual row only sends with its Send button" : "start the measurement first");
        }
        if (on && !row.enabled)
        {
            row.next_due = {};
            row.pending = false;
        }
        row.enabled = on;
    }
    else if (sub == "signal")
    {
        const std::string v = json_get(body, "value");
        double value = 0.0;
        if (row.db != sig->can_msg || v.empty() || float_from_chars(v.data(), v.data() + v.size(), value).ec != std::errc{})
        {
            return error(status, 400, row.db != sig->can_msg ? "the row is not that signal's message" : "value missing");
        }
        tx_signal_set(*sig->can_sig, row.msg, value);
    }
    else
    {
        return error(status, 404, "no such route");
    }
    gen.changed = true;
    gen.wake.notify_all();
    return std::format("{{\"index\":{},\"rows\":{}}}", *index, gen.rows.size());
}

// ---- Macros ----

std::string macros_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    if (sub.empty())
    {
        std::string out;
        for (const Macro& m : app.macros.items)
        {
            out += std::format("{}{{\"name\":{},\"steps\":{},\"running\":{}}}", out.empty() ? "" : ",", json_string(m.name),
                               m.steps.size(), m.running->load());
        }
        return std::format("{{\"macros\":[{}]}}", out);
    }
    const std::string name = json_get(body, "name");
    const auto m = std::ranges::find(app.macros.items, name, &Macro::name);
    if (m == app.macros.items.end())
    {
        return error(status, 400, "no such macro (see GET /macros)");
    }
    if (sub == "run")
    {
        return macro_start(app, *m) ? "{\"running\":true}" : error(status, 409, "the macro is running");
    }
    if (sub == "stop")
    {
        macro_stop(*m);
        return "{\"running\":false}";
    }
    return error(status, 404, "no such route");
}

// ---- Python script ----

std::string script_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    PyState& py = app.python;
    if (sub == "stop")
    {
        python_stop(py);
        return "{\"stopping\":true}";
    }
    if (sub == "output")
    {
        std::string text;
        uint64_t total = 0;
        {
            std::scoped_lock lock(py.mutex);
            for (const PyConsoleRun& r : py.console)
            {
                text += r.text;
            }
            total = py.console_total;
        }
        return std::format("{{\"running\":{},\"file\":{},\"appends\":{},\"output\":{}}}", py.running.load(),
                           json_string(app.script.file_path), total, json_string(text));
    }
    if (sub == "file")
    {
        const std::string path = json_get(body, "path");
        if (path.empty() || !std::filesystem::is_regular_file(path))
        {
            return error(status, 400, "path: no such file");
        }
        if (py.running)
        {
            return error(status, 409, "a script is running");
        }
        if (!script_window_load_file(app.script, path))
        {
            return error(status, 400, "cannot read the file");
        }
        if (json_get(body, "run") == "false")
        {
            return "{\"loaded\":true,\"running\":false}";
        }
        script_window_run(app, app.script);
        return py.running ? "{\"loaded\":true,\"running\":true}" : error(status, 409, "the script did not start (see GET /script/output)");
    }
    return error(status, 404, "no such route");
}

// ---- Workspace and tabs ----

std::string tabs_json(const App& app)
{
    std::string out;
    for (const WorkspaceTab& t : app.workspace.tabs)
    {
        out += (out.empty() ? "" : ",") + json_string(t.title);
    }
    return std::format("{{\"tabs\":[{}],\"current\":{},\"workspace\":{}}}", out, app.workspace.current,
                       json_string(app.settings.workspace_path));
}

std::string workspace_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    const std::string path = json_get(body, "path");
    if (sub == "open")
    {
        if (path.empty() || !std::filesystem::is_regular_file(path))
        {
            return error(status, 400, "path: no such file");
        }
        // Replaces the dock layout, so it loads before the next frame (app_before_frame), not here.
        app.settings.pending_open = path;
        return std::format("{{\"opening\":{}}}", json_string(path));
    }
    if (sub == "save")
    {
        const std::string file = path.empty() ? app.settings.workspace_path : path;
        if (file.empty())
        {
            return error(status, 400, "path missing (never saved)");
        }
        return workspace_save(app, file) ? std::format("{{\"saved\":{}}}", json_string(file))
                                         : error(status, 500, "cannot write " + file);
    }
    return error(status, 404, "no such route");
}

std::string tab_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    WorkspaceTabs& ws = app.workspace;
    if (sub == "new")
    {
        const std::string title = json_get(body, "title");
        workspace_add_tab(ws, title.empty() ? workspace_unique_title(ws) : title);
        return tabs_json(app);
    }
    if (!sub.empty())
    {
        // "title" or "index" names the tab.
        const std::string title = json_get(body, "title");
        const std::optional<int> index = json_int(body, "index");
        int i = index.value_or(-1);
        if (!title.empty())
        {
            const auto it = std::ranges::find(ws.tabs, title, &WorkspaceTab::title);
            i = it == ws.tabs.end() ? -1 : static_cast<int>(it - ws.tabs.begin());
        }
        if (i < 0 || i >= static_cast<int>(ws.tabs.size()))
        {
            return error(status, 400, "no such tab (title or index, see GET /tab)");
        }
        WorkspaceTab& tab = ws.tabs[static_cast<std::size_t>(i)];
        if (sub == "switch")
        {
            ws.current = i;
            ws.select_current = true;
        }
        else if (sub == "rename")
        {
            const std::string name = json_get(body, "name");
            if (name.empty())
            {
                return error(status, 400, "name missing");
            }
            tab.title = name;
        }
        else if (sub == "close")
        {
            if (ws.tabs.size() < 2)
            {
                return error(status, 409, "the last tab cannot be closed");
            }
            workspace_close_tab(app, tab.uid);
        }
        else
        {
            return error(status, 404, "no such route");
        }
    }
    return tabs_json(app);
}

// ---- Graph export: {"graph":"Graph 2","path":"x.csv|x.png"} ----

std::string graph_export_json(App& app, std::string_view body, int& status)
{
    WorkspaceTab* tab = find_tab(app, body);
    const std::string name = json_get(body, "graph");
    const std::string path = json_get(body, "path");
    if (tab == nullptr)
    {
        return error(status, 400, "no such tab");
    }
    const auto g = std::ranges::find_if(tab->graphs, [&](const GraphState& gs)
    {
        return (gs.id == 0 ? std::string("Graph") : std::format("Graph {}", gs.id)) == (name.empty() ? "Graph" : name);
    });
    if (g == tab->graphs.end())
    {
        return error(status, 400, "no such graph (\"Graph\", \"Graph 2\", ...)");
    }
    if (path.empty())
    {
        return error(status, 400, "path missing (.csv or .png)");
    }
    if (iequals(std::filesystem::path(path).extension().string(), ".png"))
    {
        // Drawn off screen at the end of the graph window, rendered by main.cpp after the frame.
        g->png_path = g->png_last = path;
        return std::format("{{\"png\":{},\"pending\":true}}", json_string(path));
    }
    graph_export_csv(*g, path);
    return std::filesystem::exists(path) ? std::format("{{\"csv\":{}}}", json_string(path)) : error(status, 500, "cannot write " + path);
}

// ---- Convert ----

std::string convert_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    ConvertState& s = app.convert;
    const bool busy = s.worker.joinable() && s.log_result.valid();
    if (busy && s.log_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        s.log_status = s.log_result.get(); // as the Convert window does, when it is closed
        s.worker = {};
    }
    const std::string in = json_get(body, "in");
    const std::string out = json_get(body, "out");
    if (sub == "database" || sub == "log")
    {
        if (in.empty() || out.empty())
        {
            return error(status, 400, "in and out missing");
        }
    }
    if (sub == "database")
    {
        const std::string r = convert_database(in, out);
        return r.starts_with("Error") ? error(status, 400, r) : std::format("{{\"status\":{}}}", json_string(r));
    }
    if (sub == "log")
    {
        if (s.worker.joinable())
        {
            return error(status, 409, "a conversion is running");
        }
        const auto format = trace_format_from_path(out);
        const auto it = format ? std::ranges::find(trace_file_formats, *format) : trace_file_formats.end();
        if (it == trace_file_formats.end())
        {
            return error(status, 400, "unknown output format (by out's extension: .asc, .blf, .log, .mf4, .pcapng, .trc, ...)");
        }
        // Blocking: on the worker thread, as the Convert window's button.
        s.log_in = in;
        s.log_out = out;
        s.log_format = static_cast<int>(it - trace_file_formats.begin());
        s.log_status.clear();
        std::promise<std::string> done;
        s.log_result = done.get_future();
        s.worker = std::jthread([&s, in, out, f = s.log_format, done = std::move(done)](const std::stop_token& stop) mutable
        {
            done.set_value(convert_log(in, out, f, s, stop));
        });
        return "{\"started\":true}";
    }
    if (sub == "status")
    {
        return std::format("{{\"busy\":{},\"read_frames\":{},\"written\":{},\"total\":{},\"status\":{},\"database\":{}}}",
                           s.worker.joinable(), s.read_frames.load(), s.written.load(), s.total.load(),
                           json_string(s.log_status), json_string(s.db_status));
    }
    if (sub == "cancel")
    {
        s.worker.request_stop();
        return "{\"cancelling\":true}";
    }
    return error(status, 404, "no such route");
}

// ---- Watch: GET /watch, POST /watch/add {"signals":"A, B"}, /watch/reset ----

std::string watch_json(App& app, std::string_view sub, std::string_view body, int& status)
{
    WorkspaceTab* tab = find_tab(app, body);
    if (tab == nullptr)
    {
        return error(status, 400, "no such tab");
    }
    WatchWindow& w = app.watch_windows[tab->uid];
    if (sub == "add")
    {
        const std::string list = json_get(body, "signals");
        for (std::string_view rest = list; !rest.empty();)
        {
            const std::size_t comma = rest.find(',');
            const std::string_view name = trim(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            if (name.empty())
            {
                continue;
            }
            const auto e = find_signal(app.setup, name);
            if (!e || e->can_sig == nullptr)
            {
                return error(status, 400, std::format("unknown or ambiguous CAN signal \"{}\" (\"Message.Signal\")", name));
            }
            watch_add(w, app.setup, *e);
        }
        w.open = true;
    }
    else if (sub == "reset")
    {
        watch_reset(w);
    }
    else if (!sub.empty())
    {
        return error(status, 404, "no such route");
    }
    std::string out;
    for (const WatchItem& i : w.items)
    {
        out += std::format("{}{{\"signal\":{},\"has_value\":{},\"value\":{},\"min\":{},\"max\":{},\"count\":{}}}",
                           out.empty() ? "" : ",", json_string(i.msg != nullptr ? i.msg->name + "." + i.signal : i.signal),
                           i.has_value, i.value, i.min, i.max, i.count);
    }
    return std::format("{{\"items\":[{}]}}", out);
}

// ---- Settings: theme and font scale ----

std::string settings_json(App& app, std::string_view method, std::string_view body, int& status)
{
    constexpr std::array<std::string_view, 3> themes{"system", "light", "dark"};
    if (method == "POST")
    {
        const std::string theme = json_get(body, "theme");
        const auto t = std::ranges::find(themes, theme);
        if (!theme.empty() && t == themes.end())
        {
            return error(status, 400, "theme: system, light or dark");
        }
        const std::optional<int> scale = json_int(body, "font_scale");
        if (!json_get(body, "font_scale").empty() && (!scale || *scale < 100 || *scale > 175))
        {
            return error(status, 400, "font_scale: 100..175 (percent)");
        }
        if (t != themes.end())
        {
            app.settings.theme = static_cast<ThemeMode>(t - themes.begin());
        }
        app.settings.font_scale_pct = scale.value_or(app.settings.font_scale_pct);
        if (ImGui::GetCurrentContext() != nullptr)
        {
            settings_apply_theme(app);
        }
        settings_save(app);
    }
    return std::format("{{\"theme\":{},\"font_scale\":{}}}", json_string(themes[static_cast<std::size_t>(app.settings.theme)]),
                       app.settings.font_scale_pct);
}

// "/a/b" under prefix "/a": "b"; "/a": ""; anything else: nullopt.
std::optional<std::string_view> under(std::string_view path, std::string_view prefix)
{
    if (path == prefix)
    {
        return std::string_view{};
    }
    if (path.starts_with(prefix) && path.size() > prefix.size() + 1 && path[prefix.size()] == '/')
    {
        return path.substr(prefix.size() + 1);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> rest_control_routes(App& app, std::string_view method, std::string_view path,
                                               std::string_view body, int& status)
{
    const bool get = method == "GET";
    const bool post = method == "POST";
    // GET for the bare listing, POST for every action below it.
    const auto route = [&](std::string_view prefix) -> std::optional<std::string_view>
    {
        const auto sub = under(path, prefix);
        return sub && (sub->empty() ? get : post) ? sub : std::nullopt;
    };
    if (const auto sub = route("/generator"))
    {
        return generator_json(app, *sub, body, status);
    }
    if (const auto sub = route("/macros"))
    {
        return macros_json(app, *sub, body, status);
    }
    if (post && (path == "/script/stop" || path == "/script/file"))
    {
        return script_json(app, path.substr(8), body, status);
    }
    if (get && path == "/script/output")
    {
        return script_json(app, "output", body, status);
    }
    if (post && (path == "/workspace/open" || path == "/workspace/save"))
    {
        return workspace_json(app, path.substr(11), body, status);
    }
    if (const auto sub = route("/tab"))
    {
        return tab_json(app, *sub, body, status);
    }
    if (post && path == "/graph/export")
    {
        return graph_export_json(app, body, status);
    }
    if ((post && (path == "/convert/database" || path == "/convert/log" || path == "/convert/cancel"))
        || (get && path == "/convert/status"))
    {
        return convert_json(app, path.substr(9), body, status);
    }
    if (const auto sub = route("/watch"))
    {
        return watch_json(app, *sub, body, status);
    }
    if ((get || post) && path == "/settings")
    {
        return settings_json(app, method, body, status);
    }
    return std::nullopt;
}
