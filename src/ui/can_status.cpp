#include "ui/can_status.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <utility>

#include <imgui.h>

#include "app.h"
#include "core/trace.h"
#include "ui/theme.h"

namespace
{

constexpr auto poll_interval = std::chrono::milliseconds(100); // the old QTimer
constexpr auto load_interval = std::chrono::milliseconds(500); // load averaged for stability
constexpr auto link_interval = std::chrono::seconds(1);        // sysfs link state refresh

bool is_socketcan(const Iface& iface)
{
    return iface.ops && std::strcmp(iface.ops->name, "SocketCAN") == 0;
}

constexpr const char* no_bitrate = "\u2014";

void append_rate(std::string& out, unsigned bps, bool custom = false)
{
    if (custom)
    {
        out += "custom";
    }
    else if (bps >= 1000000)
    {
        std::format_to(std::back_inserter(out), "{:g}M", bps / 1e6);
    }
    else
    {
        std::format_to(std::back_inserter(out), "{:g}k", bps / 1e3);
    }
}

// The controller's actual bitrate when the driver reports one (SocketCAN bit timing), else the
// configured one from the interface's Setup entry ("500k", "500k / 2M", LIN "19.2k");
// no_bitrate for vcan, interfaces the OS configures and ones not in the setup.
std::string bitrate_text(const Setup& setup, const Iface& iface, const IfaceStats& stats)
{
    if (!iface.ops || iface.info.details == "vcan")
    {
        return no_bitrate;
    }
    if (stats.bitrate)
    {
        std::string out;
        append_rate(out, stats.bitrate);
        if (stats.data_bitrate)
        {
            out += " / ";
            append_rate(out, stats.data_bitrate);
        }
        return out;
    }
    for (const auto& net : setup.networks)
    {
        for (const auto& si : net.interfaces)
        {
            if (si.driver != iface.ops->name || si.name != iface.info.name)
            {
                continue;
            }
            if (!si.configure)
            {
                return no_bitrate;
            }
            std::string out;
            if (si.bus_type == BusType::LIN)
            {
                append_rate(out, si.lin_baudrate);
                return out;
            }
            append_rate(out, si.bitrate, si.is_custom_bitrate);
            if (si.can_fd)
            {
                out += " / ";
                append_rate(out, si.fd_bitrate, si.is_custom_fd_bitrate);
            }
            return out;
        }
    }
    return no_bitrate;
}

// Theme colour of a bus state: accent teal, brass, coral, red-coral, disabled text.
ImVec4 state_color(IfaceState state)
{
    switch (state)
    {
    case IfaceState::Ok:
        return ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    case IfaceState::Warning:
        return ImGui::ColorConvertU32ToFloat4(theme_u32(theme_kraken_button().border));
    case IfaceState::Passive:
        return ImGui::ColorConvertU32ToFloat4(theme_u32(theme_stop_button().text));
    case IfaceState::BusOff:
        return ImGui::ColorConvertU32ToFloat4(theme_u32(theme_stop_button().border));
    case IfaceState::Stopped:
    case IfaceState::Unknown:
        break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

// Picks up link changes made outside Kraken Explorer (`ip link` in a terminal) and after our commands.
void refresh_links(App& app, CanStatusState& s)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - s.link_polled < link_interval)
    {
        return;
    }
    s.link_polled = now;
    s.link_exists.resize(app.ifaces.size(), 1);
    s.bitrate.resize(app.ifaces.size());
    s.sort_dirty = true; // names change on a reload
    for (auto& iface : app.ifaces)
    {
        s.bitrate[iface.index] = bitrate_text(app.setup, iface, iface.index < s.rows.size() ? s.rows[iface.index].stats : IfaceStats{});
        if (is_socketcan(iface))
        {
            s.link_exists[iface.index] = socketcan_link_exists(iface.info.name);
            iface.info.up = socketcan_link_up(iface.info.name);
        }
    }
}

// Bitrate the setup opens the interface with (LIN: baud rate), 0 = unknown.
unsigned setup_bitrate(const Setup& setup, int index)
{
    for (const auto& net : setup.networks)
    {
        for (const auto& si : net.interfaces)
        {
            if (si.iface == index)
            {
                return si.bus_type == BusType::LIN ? si.lin_baudrate : si.bitrate;
            }
        }
    }
    return 0;
}

SetupInterface* setup_socketcan(Setup& setup, const std::string& name)
{
    for (auto& net : setup.networks)
    {
        for (auto& si : net.interfaces)
        {
            if (si.driver == "SocketCAN" && si.name == name)
            {
                return &si;
            }
        }
    }
    return nullptr;
}

// Bit timing for `ip link set ... up`: the setup's, else the SetupInterface defaults (500k / 87.5%).
IfaceConfig setup_timing(Setup& setup, const std::string& name)
{
    const SetupInterface* si = setup_socketcan(setup, name);
    return si ? *si : IfaceConfig{};
}

void poll(App& app, CanStatusState& s)
{
    const auto now = std::chrono::steady_clock::now();
    s.rows.resize(app.ifaces.size());
    s.sort_dirty = true;
    for (auto& iface : app.ifaces)
    {
        CanStatusRow& row = s.rows[iface.index];
        if (iface.failed) // link down or device gone: the listener reopens it when it is back
        {
            row = {.stats = {.state = IfaceState::Stopped}, .seen = true}; // counters from zero again then
            continue;
        }
        IfaceStats stats;
        if (!iface_stats(iface, stats))
        {
            continue; // closed: keep the last values
        }
        const uint64_t bits = iface.total_bits.load(std::memory_order_relaxed);
        if (!row.seen || row.load_time == std::chrono::steady_clock::time_point{}) // first poll after the (re)open
        {
            row = {.base = stats, .bits = bits, .load_bits = bits, .load_time = now, .seen = true};
        }
        row.stats = stats;
        row.stats.rx_frames -= row.base.rx_frames; // the cumulative ones; rx/tx_errors are the controller's counters
        row.stats.tx_frames -= row.base.tx_frames;
        row.stats.rx_overruns -= row.base.rx_overruns;
        row.stats.tx_dropped -= row.base.tx_dropped;
        row.bits = bits;
        if (row.load_time == now)
        {
            continue;
        }
        const auto dt = std::chrono::duration<double>(now - row.load_time).count();
        if (now - row.load_time < load_interval)
        {
            continue;
        }
        const unsigned bitrate = row.stats.bitrate ? row.stats.bitrate : setup_bitrate(app.setup, iface.index);
        row.load = bitrate > 0 ? std::format("{:.1f}%", std::min(100.0, static_cast<double>(row.bits - row.load_bits) / bitrate / dt * 100.0))
                               : "---";
        row.load_bits = row.bits;
        row.load_time = now;
    }
}

// Status text for a failed `ip` call; "short: detail" is drawn short with detail as tooltip.
std::string ip_error_text(IpResult r, const std::string& name)
{
    switch (r)
    {
    case IpResult::ok:
        return "";
    case IpResult::denied:
        return "Authorization cancelled";
    case IpResult::no_agent:
        return "No polkit agent: start one (polkit-gnome) or install packaging/10-kraken-explorer-socketcan.rules";
    case IpResult::failed:
        break;
    }
    return std::format("ip failed on {}, see Log", name);
}

// Draws text up to its first ": " (the whole text as tooltip) or all of it.
void draw_status_text(const std::string& text, const ImVec4* color = nullptr)
{
    const std::size_t cut = text.find(": ");
    const std::string shown = text.substr(0, cut);
    if (color)
    {
        ImGui::TextColored(*color, "%s", shown.c_str());
    }
    else
    {
        ImGui::TextUnformatted(shown.c_str());
    }
    if (cut != std::string::npos)
    {
        ImGui::SetItemTooltip("%s", text.c_str());
    }
}

// Main thread, after a link worker: re-enable the buttons, refresh the link state now, reload.
void link_done(App& app)
{
    app.can_status.link_busy = false;
    app.can_status.link_polled = {};
    app.menu.pending.set(static_cast<std::size_t>(Command::ReloadInterfaces));
}

// Starts work on the link worker thread; false (nothing started) while another command runs.
// The worker ends with link_done posted to the main thread.
bool link_start(CanStatusState& s, auto&& work)
{
    if (s.link_busy)
    {
        return false;
    }
    s.link_busy = true;
    s.link_worker = std::jthread(std::forward<decltype(work)>(work));
    return true;
}

// Runs `[pkexec] ip` for op on a worker thread, then reloads the interfaces (on the main
// thread, Command::ReloadInterfaces). Ignored while another command runs. timing: see ip_link_args.
void link_command(App& app, LinkOp op, const std::string& name, const IfaceConfig* timing = nullptr)
{
    link_start(app.can_status,
        [&tasks = app.tasks, args = ip_link_args(op, name, timing), name]
        {
            const IpResult r = socketcan_run_ip(args); // logs stderr on failure
            tasks_post(tasks,
                       [r, name](App& app)
                       {
                           app.can_status.link_error = ip_error_text(r, name);
                           link_done(app);
                       });
        });
}

// Scans the bitrate of a physical SocketCAN interface listen-only on a worker thread; a hit is
// stored in the interface's Setup entry. Ignored while another link command runs.
void autobaud_command(App& app, const std::string& name)
{
    CanStatusState& s = app.can_status;
    const bool started = link_start(s,
        [&tasks = app.tasks, name, timing = setup_timing(app.setup, name)]
        {
            const AutobaudResult res = socketcan_autobaud(name, timing);
            tasks_post(tasks,
                       [bitrate = res.bitrate, ip = res.ip, name](App& app)
                       {
                           app.can_status.autobaud_result = bitrate ? std::format("{:g} kbit/s", *bitrate / 1000.0)
                                                            : ip != IpResult::ok ? ip_error_text(ip, name)
                                                                                 : "no match";
                           if (SetupInterface* si = bitrate ? setup_socketcan(app.setup, name) : nullptr)
                           {
                               si->bitrate = *bitrate; // Start and Up use it from now on
                           }
                           link_done(app);
                       });
        });
    if (started)
    {
        s.autobaud_iface = name;
        s.autobaud_result = "Auto-baud...";
    }
}

// The Up/Down button (and Enter/Space on the selected row): physical CAN needs bit timing to
// come up; vcan has none.
void toggle_link(App& app, const Iface& iface)
{
    const IfaceConfig timing = setup_timing(app.setup, iface.info.name);
    link_command(app, iface.info.up ? LinkOp::Down : LinkOp::Up, iface.info.name,
                 iface.info.details == "vcan" ? nullptr : &timing);
}

// Whether toggle_link may run: a SocketCAN link that exists, no command running, not measuring.
bool can_toggle(const App& app, const Iface& iface)
{
    const CanStatusState& s = app.can_status;
    return is_socketcan(iface) && !(iface.index < s.link_exists.size() && !s.link_exists[iface.index]) && !s.link_busy
        && !app.measuring;
}

} // namespace

void can_status_sort(CanStatusState& s, const std::deque<Iface>& ifaces)
{
    s.order.resize(ifaces.size());
    for (std::size_t i = 0; i < ifaces.size(); ++i)
    {
        s.order[i] = static_cast<uint16_t>(i);
    }
    const auto stats = [&s](uint16_t i) -> IfaceStats
    { return i < s.rows.size() && s.rows[i].seen ? s.rows[i].stats : IfaceStats{}; };
    const auto key = [&](uint16_t i)
    {
        const IfaceStats st = stats(i);
        switch (s.sort_key)
        {
        case CanStatusSort::State:
            return static_cast<uint64_t>(st.state);
        case CanStatusSort::RxFrames:
            return st.rx_frames;
        case CanStatusSort::RxErrors:
            return st.rx_errors;
        case CanStatusSort::TxFrames:
            return st.tx_frames;
        case CanStatusSort::TxErrors:
            return st.tx_errors;
        case CanStatusSort::Interface:
            break;
        }
        return uint64_t{0};
    };
    std::stable_sort(s.order.begin(), s.order.end(),
                     [&](uint16_t a, uint16_t b)
                     {
                         if (s.sort_desc)
                         {
                             std::swap(a, b);
                         }
                         if (s.sort_key == CanStatusSort::Interface)
                         {
                             return ifaces[a].info.name < ifaces[b].info.name;
                         }
                         return key(a) < key(b);
                     });
    s.sort_dirty = false;
}

void draw_link_buttons(App& app, const Iface& iface)
{
    if (!is_socketcan(iface))
    {
        return;
    }
    const CanStatusState& s = app.can_status;
    if (iface.index < s.link_exists.size() && !s.link_exists[iface.index])
    {
        ImGui::TextDisabled("removed");
        return;
    }
    ImGui::PushID(iface.index);
    // The measurement owns open interfaces; a reload is refused while it runs anyway.
    ImGui::BeginDisabled(s.link_busy || app.measuring);
    if (ImGui::SmallButton(iface.info.up ? "Down" : "Up"))
    {
        toggle_link(app, iface);
    }
    if (iface.info.details == "vcan")
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
        {
            ImGui::OpenPopup("confirm_delete"); // right next to Down, and vcan may be shared
        }
    }
    else
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto-baud"))
        {
            autobaud_command(app, iface.info.name);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("confirm_delete"))
    {
        ImGui::Text("Delete %s?", iface.info.name.c_str());
        if (ImGui::Button("Delete"))
        {
            link_command(app, LinkOp::Delete, iface.info.name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImGui::GetStyleColorVec4(iface.info.up ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled), "%s",
                       iface.info.up ? "UP" : "DOWN");
    if (s.autobaud_iface == iface.info.name)
    {
        ImGui::SameLine();
        draw_status_text(s.autobaud_result);
    }
    ImGui::PopID();
}

void draw_new_vcan_button(App& app)
{
    if (!socketcan_available)
    {
        return;
    }
    ImGui::BeginDisabled(app.can_status.link_busy || app.measuring);
    if (ImGui::Button("New vcan"))
    {
        link_command(app, LinkOp::AddVcan,
                     next_vcan_name([](const std::string& name) { return socketcan_link_exists(name); }));
    }
    ImGui::EndDisabled();
    if (app.can_status.link_busy)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("running ip...");
    }
    else if (!app.can_status.link_error.empty())
    {
        ImGui::SameLine();
        const ImVec4 red = ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error));
        draw_status_text(app.can_status.link_error, &red);
    }
}

void draw_can_status(App& app, CanStatusState& s, const WorkspaceTab& tab)
{
    // Rows are rebuilt when a measurement starts and updated while it runs, even when hidden.
    if (app.measuring && !s.was_measuring)
    {
        s.rows.clear();
        s.trace_done = app.trace.end;
    }
    const auto now = std::chrono::steady_clock::now();
    if (app.measuring || s.was_measuring) // one last poll right after the stop
    {
        if (now - s.polled >= poll_interval || !app.measuring)
        {
            s.polled = now;
            poll(app, s);
        }
    }
    s.was_measuring = app.measuring;
    // Error frames: only the trace rows appended since the last call.
    // A file view (Replay load) is not live: its error frames are not this measurement's.
    for (uint64_t i = app.trace.file.empty() ? std::max(s.trace_done, app.trace.begin) : app.trace.end; i < app.trace.end; ++i)
    {
        const BusMessage& m = trace_at(app.trace, i);
        if (m.errors != 0 && m.iface < s.rows.size())
        {
            ++s.rows[m.iface].error_frames;
        }
    }
    s.trace_done = app.trace.end;
    refresh_links(app, s);

    if (!ImGui::Begin(workspace_window_name(tab, "CAN Status").c_str()))
    {
        ImGui::End();
        return;
    }
    draw_new_vcan_button(app);
    struct Column
    {
        const char* name;
        float width; // em
        int sort;    // CanStatusSort, -1 = not sortable
    };
    constexpr Column columns[] = {
        {"Driver", 6.0f, -1},
        {"Interface", 6.0f, static_cast<int>(CanStatusSort::Interface)},
        {"Link", 16.0f, -1},
        {"Bitrate", 6.0f, -1},
        {"State", 6.0f, static_cast<int>(CanStatusSort::State)},
        {"Rx Frames", 6.0f, static_cast<int>(CanStatusSort::RxFrames)},
        {"Rx Errors", 6.0f, static_cast<int>(CanStatusSort::RxErrors)},
        {"Rx Overrun", 6.0f, -1},
        {"Error Frames", 7.0f, -1},
        {"Tx Frames", 6.0f, static_cast<int>(CanStatusSort::TxFrames)},
        {"Tx Errors", 6.0f, static_cast<int>(CanStatusSort::TxErrors)},
        {"Tx Dropped", 6.0f, -1},
        {"Load (%)", 6.0f, -1},
        {"Bits", 6.0f, -1},
    };
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;
    {
        // The width every column needs, for the startup size-to-fit of the left dock column.
        const ImGuiStyle& st = ImGui::GetStyle();
        float w = st.WindowPadding.x * 2.0f + st.ScrollbarSize;
        for (const Column& c : columns)
        {
            w += c.width * ImGui::GetFontSize() + st.CellPadding.x * 2.0f;
        }
        s.fit_width = w;
    }
    if (ImGui::BeginTable("##can_status", IM_ARRAYSIZE(columns), flags))
    {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const Column& c : columns)
        {
            ImGui::TableSetupColumn(c.name, ImGuiTableColumnFlags_WidthFixed | (c.sort < 0 ? ImGuiTableColumnFlags_NoSort : 0),
                                    em * c.width, static_cast<ImGuiID>(c.sort));
        }
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
        {
            if (specs->SpecsCount > 0)
            {
                s.sort_key = static_cast<CanStatusSort>(specs->Specs[0].ColumnUserID);
                s.sort_desc = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            }
            specs->SpecsDirty = false;
            s.sort_dirty = true;
        }
        if (s.sort_dirty || s.order.size() != app.ifaces.size())
        {
            can_status_sort(s, app.ifaces);
        }

        // j/k/gg/G on the display order; Enter/Space toggles the selected link like its Up/Down.
        const int count = static_cast<int>(s.order.size());
        int cur = static_cast<int>(std::find(s.order.begin(), s.order.end(), s.selected) - s.order.begin());
        cur = cur < count ? cur : -1;
        bool focus_search = false;
        int h_delta = 0;
        const int page = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().y / ImGui::GetTextLineHeightWithSpacing()));
        const bool moved = vim_nav(s.vim, cur, count, page, focus_search, h_delta);
        if (moved)
        {
            s.selected = s.order[static_cast<std::size_t>(cur)];
        }
        if (cur >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy) && !ImGui::GetIO().WantTextInput
            && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)
                || ImGui::IsKeyPressed(ImGuiKey_Space, false)))
        {
            const Iface& iface = app.ifaces[s.order[static_cast<std::size_t>(cur)]];
            if (can_toggle(app, iface))
            {
                toggle_link(app, iface);
            }
        }
        if (cur >= 0 && vim_yank())
        {
            const uint16_t index = s.order[static_cast<std::size_t>(cur)];
            const Iface& iface = app.ifaces[index];
            std::vector<VimYankItem>& items = s.yank_items;
            items.assign({{.label = "Row"},
                          {.label = "Driver", .text = iface.ops ? iface.ops->name : ""},
                          {.label = "Interface", .text = iface.info.name},
                          {.label = "Bitrate", .text = index < s.bitrate.size() ? s.bitrate[index] : no_bitrate}});
            if (index < s.rows.size() && s.rows[index].seen)
            {
                const CanStatusRow& row = s.rows[index];
                items.push_back({.label = "State", .text = iface_state_name(row.stats.state)});
                const std::pair<const char*, uint64_t> counters[] = {
                    {"Rx Frames", row.stats.rx_frames}, {"Rx Errors", row.stats.rx_errors},
                    {"Rx Overrun", row.stats.rx_overruns}, {"Error Frames", row.error_frames},
                    {"Tx Frames", row.stats.tx_frames}, {"Tx Errors", row.stats.tx_errors},
                    {"Tx Dropped", row.stats.tx_dropped}};
                for (const auto& [label, value] : counters)
                {
                    items.push_back({.label = label, .text = std::to_string(value)});
                }
                items.push_back({.label = "Load (%)", .text = row.load});
                items.push_back({.label = "Bits", .text = std::to_string(row.bits)});
            }
            for (std::size_t i = 1; i < items.size(); ++i)
            {
                items[0].text += (i > 1 ? "\t" : "") + items[i].text;
            }
            s.vim.yank_menu = 1;
        }
        vim_yank_menu(s.vim, s.yank_items);

        ImGui::TableHeadersRow();
        const auto right = [](const std::string& text)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x);
            ImGui::TextUnformatted(text.c_str());
        };
        // Every interface is listed (for its link buttons); counters only once it was measured.
        for (const uint16_t index : s.order)
        {
            const Iface& iface = app.ifaces[index];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(index);
            if (ImGui::Selectable("##row", s.selected == index,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                s.selected = index;
            }
            if (moved && s.selected == index)
            {
                ImGui::SetScrollHereY();
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextUnformatted(iface.ops ? iface.ops->name : "");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(iface.info.name.c_str());
            if (ImGui::IsItemHovered() && (!iface.info.details.empty() || !iface.info.version.empty()))
            {
                ImGui::SetTooltip("%s%s%s", iface.info.details.c_str(), iface.info.version.empty() ? "" : "\nVersion: ",
                                  iface.info.version.c_str());
            }
            ImGui::TableNextColumn();
            draw_link_buttons(app, iface);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(index < s.bitrate.size() ? s.bitrate[index].c_str() : no_bitrate);
            if (index >= s.rows.size() || !s.rows[index].seen)
            {
                continue;
            }
            const CanStatusRow& row = s.rows[index];
            ImGui::TableNextColumn();
            ImGui::TextColored(state_color(row.stats.state), "%s", iface_state_name(row.stats.state));
            for (const uint64_t v : {row.stats.rx_frames, row.stats.rx_errors, row.stats.rx_overruns, row.error_frames,
                                     row.stats.tx_frames, row.stats.tx_errors, row.stats.tx_dropped})
            {
                ImGui::TableNextColumn();
                right(std::to_string(v));
            }
            ImGui::TableNextColumn();
            right(row.load);
            ImGui::TableNextColumn();
            right(std::to_string(row.bits));
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
