#include "ui/graph.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>
#include <pugixml.hpp>

#include "app.h"
#include "core/log.h"
#include "db/model/can_db.h"
#include "db/model/lin_db.h"
#include "core/stats.h"
#include "core/text.h"
#include "ui/depth_gauge.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace
{

constexpr const char* view_names[] = {"Time Series", "XY", "Text", "Gauge"};
constexpr const char* view_keys[] = {"timeseries", "xy", "text", "gauge"};  // workspace XML, GraphView order
constexpr const char* kind_keys[] = {"can", "lin", "busload"};             // GraphSignalKind order
constexpr const char* duration_names[] = {"All", "1 min", "5 min", "10 min", "15 min", "30 min"};
constexpr double duration_secs[] = {0.0, 60.0, 300.0, 600.0, 900.0, 1800.0};
constexpr double bus_load_window_s = 1.0;  // rolling window, as the Qt decoder worker
constexpr double bus_load_sample_s = 0.05;
// ponytail: plain vectors capped by dropping the oldest half; a real ring buffer if the copy shows up.
constexpr std::size_t max_points = std::size_t{2} << 20;
constexpr const char* drag_payload = "GRAPH_SIGNAL";

double wall_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// X axis ticks and mouse readout: seconds below a minute ("12.5"), then m:ss, h:mm:ss, Nd hh:mm:ss.
// user_data: the GraphState. A window shorter than a day drops the day from every tick ("19:38:50",
// not "12d 19:38:50"): ImPlot spaces ticks without measuring them, and the long labels overlapped.
int format_time_tick(double value, char* buf, int size, void* user_data)
{
    if (std::abs(value) < 60.0)
    {
        return std::snprintf(buf, static_cast<std::size_t>(size), "%g", value);
    }
    const auto* g = static_cast<const GraphState*>(user_data);
    if (g != nullptr && g->x_max - g->x_min < 86400.0 && value >= 86400.0)
    {
        value = std::fmod(value, 86400.0);
    }
    const bool whole = std::abs(value - std::round(value)) < 1e-6;
    const std::string text = format_duration(value, whole ? 0 : 3);
    return std::snprintf(buf, static_cast<std::size_t>(size), "%s", text.c_str());
}

void push_sample(GraphSignal& s, double t, double v)
{
    if (s.t.size() >= max_points)
    {
        s.t.erase(s.t.begin(), s.t.begin() + static_cast<std::ptrdiff_t>(max_points / 2));
        s.v.erase(s.v.begin(), s.v.begin() + static_cast<std::ptrdiff_t>(max_points / 2));
    }
    s.seen_min = s.t.empty() ? v : std::min(s.seen_min, v);
    s.seen_max = s.t.empty() ? v : std::max(s.seen_max, v);
    s.t.push_back(t);
    s.v.push_back(v);
}

// Ages the rolling window up to t and emits a sample every bus_load_sample_s.
void sample_bus_load(GraphSignal& s, double t)
{
    while (!s.load_window.empty() && s.load_window.front().first < t - bus_load_window_s)
    {
        s.load_bits -= s.load_window.front().second;
        s.load_window.pop_front();
    }
    if (s.bitrate == 0 || (s.load_emitted >= 0.0 && t - s.load_emitted < bus_load_sample_s))
    {
        return;
    }
    s.load_emitted = t;
    const double load = static_cast<double>(s.load_bits) / (s.bitrate * bus_load_window_s) * 100.0;
    push_sample(s, t, std::min(load, 100.0));
}

void clear_samples(GraphState& g)
{
    for (auto& s : g.signals)
    {
        s.t.clear();
        s.v.clear();
        s.load_window.clear();
        s.load_bits = 0;
        s.load_emitted = -1.0;
        s.lod.reset(); // a new trace: the pyramid of the old file would only hold memory
        s.lod_job.reset();
        s.lod_file.reset();
    }
    g.start_ns = -1;
    g.last_t = 0.0;
    g.follow = true;
    g.file_key = {};
}

// Newest time on the X axis: while measuring it keeps running between frames.
double graph_now(const GraphState& g, bool measuring)
{
    return measuring && g.start_ns >= 0 ? g.last_t + (wall_seconds() - g.last_wall) : g.last_t;
}

// Cursor A: the theme's teal accent; B: its warning brass.
ImVec4 cursor_color(int k)
{
    return k == 0 ? ImGui::GetStyleColorVec4(ImGuiCol_CheckMark) : ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::warn));
}

// A signal colour as the PNG export draws it: the app's palette is made for a dark background, so
// the light styles darken it (yellow and light teal on white were hard to read).
ImVec4 png_color(const GraphPngOptions* png, ImVec4 c)
{
    if (png != nullptr && (png->style == GraphPngStyle::Light || png->style == GraphPngStyle::Print))
    {
        return ImVec4(c.x * 0.72f, c.y * 0.72f, c.z * 0.72f, c.w);
    }
    return c;
}

void update_cursor_values(GraphState& g)
{
    if (g.cursor_on)
    {
        for (auto& s : g.signals)
        {
            graph_cursor_values(s, g.cursor_a, g.cursor_b);
        }
    }
}

const std::vector<int>& graph_slots(GraphState& g)
{
    if (g.slots_dirty || g.slots.size() != g.signals.size())
    {
        g.slots = graph_assign_slots(g.signals);
        g.slots_dirty = false;
    }
    return g.slots;
}

// Re-resolves CAN signal pointers after a setup change; signals whose database is gone go too.
void resolve_signals(GraphState& g, const Setup& setup)
{
    g.slots_dirty = true;
    std::erase_if(g.signals, [&](GraphSignal& s)
    {
        if (s.kind != GraphSignalKind::Can)
        {
            return false;
        }
        for (const auto& net : setup.networks)
        {
            if (net.name != s.network)
            {
                continue;
            }
            for (const auto& db : net.can_dbs)
            {
                if (CanDbMessage* msg = can_db_find_message(*db, s.can_raw_id))
                {
                    if (const CanDbSignal* sig = can_db_find_signal(*msg, s.name))
                    {
                        s.can_msg = msg;
                        s.can_sig = sig;
                        return false;
                    }
                }
            }
        }
        return true;
    });
}

} // namespace

// The first colormap colour no signal uses yet, so a removal does not make the next one repeat a curve.
uint32_t graph_next_color(const GraphState& g)
{
    const int n = ImPlot::GetColormapSize();
    for (int k = 0; k < n; ++k)
    {
        const uint32_t c = ImGui::ColorConvertFloat4ToU32(ImPlot::GetColormapColor(k));
        if (std::ranges::none_of(g.signals, [&](const GraphSignal& s) { return s.color == c; }))
        {
            return c;
        }
    }
    return ImGui::ColorConvertFloat4ToU32(ImPlot::GetColormapColor(static_cast<int>(g.signals.size())));
}

// Removes signal i and keeps the XY X signal pointing at the same signal.
void graph_remove_signal(GraphState& g, std::size_t i)
{
    const int k = static_cast<int>(i);
    g.signals.erase(g.signals.begin() + k);
    g.x_signal -= k < g.x_signal ? 1 : 0;
    g.slots_dirty = true;
}

namespace
{

// Checkbox that adds/removes one signal of the tree; `match` finds it in g.signals.
template <class Match, class Make>
void signal_checkbox(GraphState& g, const char* label, Match match, Make make)
{
    auto it = std::find_if(g.signals.begin(), g.signals.end(), match);
    bool on = it != g.signals.end();
    if (ImGui::Checkbox(label, &on))
    {
        if (on)
        {
            GraphSignal s = make();
            s.color = graph_next_color(g);
            g.signals.push_back(std::move(s));
            g.slots_dirty = true;
        }
        else
        {
            graph_remove_signal(g, static_cast<std::size_t>(it - g.signals.begin()));
        }
    }
}

GraphSignal make_can_signal(const SetupNetwork& net, uint32_t raw_id, const CanDbMessage& msg, const CanDbSignal& sig)
{
    GraphSignal s;
    s.kind = GraphSignalKind::Can;
    s.network = net.name;
    s.can_msg = &msg;
    s.can_sig = &sig;
    s.can_raw_id = raw_id;
    s.name = sig.name;
    s.parent = msg.name;
    s.unit = sig.unit;
    s.min = sig.min;
    s.max = sig.max;
    return s;
}

GraphSignal make_lin_signal(const SetupNetwork& net, uint8_t id, const LinFrame& frame, const LinSignal& sig)
{
    GraphSignal s;
    s.kind = GraphSignalKind::Lin;
    s.network = net.name;
    s.lin_id = id;
    s.name = sig.name;
    s.parent = frame.name;
    s.unit = sig.unit;
    s.min = sig.min;
    s.max = sig.max;
    return s;
}

GraphSignal make_bus_load_signal(int index, const SetupInterface& si)
{
    GraphSignal s;
    s.kind = GraphSignalKind::BusLoad;
    s.iface = index;
    s.bitrate = si.bus_type == BusType::LIN ? si.lin_baudrate : si.bitrate;
    s.name = std::format("Bus Load - {}", si.name);
    s.parent = "Bus Load";
    s.unit = "%";
    s.max = 100.0;
    return s;
}

bool is_lin_signal(const GraphSignal& s, const SetupNetwork& net, uint8_t id, const LinSignal& sig)
{
    return s.kind == GraphSignalKind::Lin && s.lin_id == id && s.name == sig.name && s.network == net.name;
}

// Adds a search / palette hit unless the graph already has it.
void add_search_hit(GraphState& g, const Setup& setup, const SignalEntry& e)
{
    const SetupNetwork& net = setup.networks[e.network];
    const auto id = static_cast<uint8_t>(e.raw_id);
    const bool present = std::any_of(g.signals.begin(), g.signals.end(), [&](const GraphSignal& s)
    {
        return e.can_sig != nullptr ? s.kind == GraphSignalKind::Can && s.can_sig == e.can_sig : is_lin_signal(s, net, id, *e.lin_sig);
    });
    if (present)
    {
        return;
    }
    GraphSignal s = e.can_sig != nullptr ? make_can_signal(net, e.raw_id, *e.can_msg, *e.can_sig) : make_lin_signal(net, id, *e.lin_frame, *e.lin_sig);
    s.color = graph_next_color(g);
    g.signals.push_back(std::move(s));
}

void draw_signal_tree(App& app, GraphState& g)
{
    for (std::size_t ni = 0; ni < app.setup.networks.size(); ++ni)
    {
        const SetupNetwork& net = app.setup.networks[ni];
        ImGui::PushID(static_cast<int>(ni));
        if (!net.can_dbs.empty())
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode("can", "CAN - %s", net.name.c_str()))
            {
                for (const auto& db : net.can_dbs)
                {
                    for (const auto& [raw_id, msg] : db->messages)
                    {
                        if (!ImGui::TreeNode(&msg, "%s (0x%X)", msg.name.c_str(), raw_id & can_id_mask_extended))
                        {
                            continue;
                        }
                        for (const auto& sig : msg.signals)
                        {
                            ImGui::PushID(&sig);
                            signal_checkbox(g, sig.name.c_str(),
                                [&](const GraphSignal& s) { return s.kind == GraphSignalKind::Can && s.can_sig == &sig; },
                                [&] { return make_can_signal(net, raw_id, msg, sig); });
                            ImGui::SetItemTooltip("%s  [%g .. %g] %s\n%s", sig.is_unsigned ? "Unsigned" : "Signed", sig.min,
                                                  sig.max, sig.unit.c_str(), sig.comment.c_str());
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
        if (!net.lin_dbs.empty())
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode("lin", "LIN - %s", net.name.c_str()))
            {
                for (const auto& db : net.lin_dbs)
                {
                    for (const auto& [id, frame] : db->frames)
                    {
                        if (!ImGui::TreeNode(&frame, "%s (0x%02X)", frame.name.c_str(), id))
                        {
                            continue;
                        }
                        for (const auto& sig : frame.signals)
                        {
                            ImGui::PushID(&sig);
                            signal_checkbox(g, sig.name.c_str(),
                                [&](const GraphSignal& s) { return is_lin_signal(s, net, id, sig); },
                                [&] { return make_lin_signal(net, id, frame, sig); });
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
    if (ImGui::TreeNode("Virtual"))
    {
        for (const auto& net : app.setup.networks)
        {
            for (const auto& si : net.interfaces)
            {
                const int index = ifaces_find(app.ifaces, si.driver, si.name);
                if (index < 0)
                {
                    continue;
                }
                const std::string label = std::format("Bus Load - {}", si.name);
                ImGui::PushID(index);
                signal_checkbox(g, label.c_str(),
                    [&](const GraphSignal& s) { return s.kind == GraphSignalKind::BusLoad && s.iface == index; },
                    [&] { return make_bus_load_signal(index, si); });
                ImGui::SetItemTooltip("0..100 %%, 1 s rolling window");
                ImGui::PopID();
            }
        }
        ImGui::TreePop();
    }
}

// ponytail: recomputed when (x range, samples, newest t) changed, at most every 250 ms,
// so a following window under a flood costs one O(visible points) pass per 250 ms,
// not per frame. Incremental min/max/mean + a two-heap median if that is still too slow.
// Between the cursors when they are on (MCUViewer's "select range"), else the visible window.
void update_stats(const GraphState& g, GraphSignal& s)
{
    const double from = g.cursor_on ? std::min(g.cursor_a, g.cursor_b) : g.x_min;
    const double to = g.cursor_on ? std::max(g.cursor_a, g.cursor_b) : g.x_max;
    const std::array<double, 4> key{from, to, static_cast<double>(s.t.size()), s.t.empty() ? 0.0 : s.t.back()};
    if (const double now = wall_seconds(); key != s.stats_key && now - s.stats_wall >= 0.25)
    {
        static std::vector<double> scratch; // main thread only, reused so it stops growing
        const auto lo = std::lower_bound(s.t.begin(), s.t.end(), from);
        const auto hi = std::upper_bound(lo, s.t.end(), to);
        const auto first = s.v.begin() + (lo - s.t.begin());
        scratch.assign(first, first + (hi - lo));
        const Stats st = stats_of(scratch);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        s.stats = scratch.empty() ? std::array{nan, nan, nan, nan, nan}
                                  : std::array{st.min, st.max, st.mean, st.median, st.stddev};
        s.stats_key = key;
        s.stats_wall = now;
    }
}

// The active signals: colour, value (A, B and B - A when the cursors are on), plot slot, with the
// selected signal's statistics below. Rows drag onto a Y axis; right-click for the axis, visibility
// and removal.
void draw_signal_list(GraphState& g, std::span<const int> slots)
{
    const int slot_count = slots.empty() ? 1 : *std::max_element(slots.begin(), slots.end()) + 1;
    // j/k/gg/G pick a row, Space toggles its visibility; only while the list itself has focus,
    // so the fuzzy search list above keeps its own j/k.
    const int count = static_cast<int>(g.signals.size());
    g.selected = std::min(g.selected, count - 1);
    bool moved = false;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
    {
        bool focus_search = false;
        int h_delta = 0;
        const int page = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().y / ImGui::GetFrameHeightWithSpacing()));
        moved = vim_nav(g.vim, g.selected, count, page, focus_search, h_delta);
        if (g.selected >= 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        {
            g.signals[static_cast<std::size_t>(g.selected)].hidden ^= true;
        }
    }
    if (g.cursor_on)
    {
        update_cursor_values(g);
        ImGui::TextUnformatted(graph_delta_t_text(g.cursor_a, g.cursor_b).c_str());
    }
    if (g.cursor_y_on)
    {
        ImGui::TextUnformatted(std::format("\u0394Y = {:.6g}  (Y1 {:.6g}, Y2 {:.6g})", g.cursor_y2 - g.cursor_y1, g.cursor_y1, g.cursor_y2).c_str());
    }
    // No ScrollY: the ##list child scrolls the table and the statistics below it together.
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    // Different ids per column count: ImGui keeps column state per table id.
    const int columns = 3 + (g.cursor_on ? 2 : 0);
    const bool stats_on = g.statistics && count > 0;
    const int stats_sel = stats_on ? std::max(g.selected, 0) : -1; // none picked: the first signal
    if (!ImGui::BeginTable(std::format("##active{}", columns).c_str(), columns, flags))
    {
        return;
    }
    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    if (g.cursor_on)
    {
        ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("\u0394 (B-A)", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    }
    else
    {
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    }
    ImGui::TableSetupColumn("Axis", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    int remove = -1;
    for (int i = 0; i < static_cast<int>(g.signals.size()); ++i)
    {
        GraphSignal& s = g.signals[static_cast<std::size_t>(i)];
        const int slot = slots[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool shown = !s.hidden;
        if (ImGui::Checkbox("##shown", &shown))
        {
            s.hidden = !shown;
        }
        ImGui::SameLine();
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(s.color);
        if (ImGui::ColorEdit4("##color", &col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        {
            s.color = ImGui::ColorConvertFloat4ToU32(col);
        }
        ImGui::SameLine();
        if (s.hidden)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        if (ImGui::Selectable(s.name.c_str(), i == g.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
        {
            g.selected = i;
        }
        if (moved && i == g.selected)
        {
            ImGui::SetScrollHereY();
        }
        if (s.hidden)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SetItemTooltip("%s / %s", s.parent.c_str(), s.name.c_str());
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(drag_payload, &i, sizeof i);
            ImGui::Text("%s -> drop on a Y axis", s.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##signal_menu"))
        {
            if (ImGui::MenuItem("Auto (by unit and range)", nullptr, s.axis < 0))
            {
                s.axis = -1;
                g.slots_dirty = true;
            }
            for (int k = 0; k <= slot_count && k < 4 * graph_axes_per_plot; ++k)
            {
                const std::string label = std::format("Y{} of plot {}", k % graph_axes_per_plot + 1, k / graph_axes_per_plot + 1);
                if (ImGui::MenuItem(label.c_str(), nullptr, s.axis == k))
                {
                    s.axis = k;
                    g.slots_dirty = true;
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("Hidden", nullptr, &s.hidden);
            if (ImGui::MenuItem("Remove"))
            {
                remove = i;
            }
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        if (g.cursor_on)
        {
            ImGui::Text("%s %s", graph_format_value(s, s.at_cursor[0]).c_str(), s.unit.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s %s", graph_format_value(s, s.at_cursor[1]).c_str(), s.unit.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s %s", graph_format_value(s, s.at_cursor[2]).c_str(), s.unit.c_str());
        }
        else
        {
            const double v = s.v.empty() ? std::numeric_limits<double>::quiet_NaN() : s.v.back();
            ImGui::Text("%s %s", graph_format_value(s, v).c_str(), s.unit.c_str());
        }
        ImGui::TableNextColumn();
        if (g.view == GraphView::XY)
        {
            ImGui::RadioButton("X##x", &g.x_signal, i);
            ImGui::SetItemTooltip("Use as the X axis; the other signals are plotted against it");
        }
        else
        {
            ImGui::Text("%sY%d/%d", s.axis < 0 ? "" : "*", slot % graph_axes_per_plot + 1, slot / graph_axes_per_plot + 1);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (stats_on)
    {
        GraphSignal& s = g.signals[static_cast<std::size_t>(stats_sel)];
        update_stats(g, s);
        // One row per value: 5 stat columns next to the signals made the list too wide.
        ImGui::TextUnformatted(std::format("Statistics: {}", s.name).c_str());
        if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("##stat", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            constexpr std::array labels{"Min", "Max", "Mean", "Median", "Std dev"};
            for (std::size_t k = 0; k < labels.size(); ++k)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(labels[k]);
                ImGui::TableNextColumn();
                ImGui::Text("%s %s", graph_format_value(s, s.stats[k]).c_str(), s.unit.c_str());
            }
            ImGui::EndTable();
        }
    }
    if (remove >= 0)
    {
        graph_remove_signal(g, static_cast<std::size_t>(remove));
    }
}

// Time window [x_min, x_max] follows the newest data while `follow` is on.
void follow_window(const App& app, GraphState& g)
{
    if (g.follow)
    {
        const double d = duration_secs[g.duration];
        g.x_max = std::max(graph_now(g, app.measuring), d > 0.0 ? 0.0 : 1.0);
        g.x_min = d > 0.0 ? g.x_max - d : 0.0;
    }
}

// Every g.downsample-th sample of s (the Downsample field), or s itself; the spans stay valid
// until the next call.
std::pair<std::span<const double>, std::span<const double>> stride(GraphState& g, const GraphSignal& s)
{
    if (g.downsample <= 1)
    {
        return {s.t, s.v};
    }
    g.stride_t.clear();
    g.stride_v.clear();
    for (std::size_t k = 0; k < s.t.size(); k += static_cast<std::size_t>(g.downsample))
    {
        g.stride_t.push_back(s.t[k]);
        g.stride_v.push_back(s.v[k]);
    }
    return {g.stride_t, g.stride_v};
}

// The user pans or zooms the current plot (not by dragging a cursor): stop following the newest data.
void stop_follow_on_user_zoom(GraphState& g, bool cursor_held)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (ImPlot::IsPlotHovered() && !cursor_held
        && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right) || io.MouseWheel != 0.0f))
    {
        g.follow = false;
    }
}

// Y signals against the X signal over the time window, e.g. lat/lon track or RPM vs speed.
void draw_xy(App& app, GraphState& g, const GraphPngOptions* png = nullptr)
{
    if (g.signals.size() < 2)
    {
        ImGui::TextDisabled("XY needs two signals: pick one as X in the Axis column, the others are Y.");
        return;
    }
    g.x_signal = std::clamp(g.x_signal, 0, static_cast<int>(g.signals.size()) - 1);
    const GraphSignal& xs = g.signals[static_cast<std::size_t>(g.x_signal)];
    const auto with_unit = [](const GraphSignal& s) { return s.unit.empty() ? s.name : std::format("{} [{}]", s.name, s.unit); };
    std::string y_label;
    for (std::size_t i = 0; i < g.signals.size(); ++i)
    {
        if (static_cast<int>(i) != g.x_signal && !g.signals[i].hidden)
        {
            y_label += (y_label.empty() ? "" : ", ") + with_unit(g.signals[i]);
        }
    }
    if (!ImPlot::BeginPlot("##xy", ImGui::GetContentRegionAvail(), png != nullptr && !png->legend ? ImPlotFlags_NoLegend : ImPlotFlags_None))
    {
        return;
    }
    const std::string x_label = with_unit(xs);
    // Fit while following; a zoom or pan stops following (Reset Zoom resumes), as in the time series.
    // The PNG export shows the range on screen.
    const ImPlotAxisFlags fit = g.follow && png == nullptr ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
    ImPlot::SetupAxes(x_label.c_str(), y_label.c_str(), fit, fit);
    if (png != nullptr && g.xy_shown[1] > g.xy_shown[0] && g.xy_shown[3] > g.xy_shown[2])
    {
        ImPlot::SetupAxesLimits(g.xy_shown[0], g.xy_shown[1], g.xy_shown[2], g.xy_shown[3], ImPlotCond_Always);
    }
    if (png == nullptr)
    {
        stop_follow_on_user_zoom(g, false);
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    update_cursor_values(g);
    for (std::size_t i = 0; i < g.signals.size(); ++i)
    {
        const GraphSignal& s = g.signals[i];
        if (static_cast<int>(i) == g.x_signal || s.hidden)
        {
            continue;
        }
        const auto [t, v] = stride(g, s);
        graph_xy_pair(xs.t, xs.v, t, v, g.x_min, g.x_max, g.scratch_t, g.scratch_v);
        const int n = static_cast<int>(g.scratch_t.size());
        if (n == 0)
        {
            continue;
        }
        const ImVec4 color = png_color(png, ImGui::ColorConvertU32ToFloat4(s.color));
        const std::string label = std::format("{}##{}", s.name, i);
        ImPlotSpec spec;
        spec.LineColor = color;
        spec.FillColor = color;
        spec.LineWeight = 2.0f * px;
        ImPlot::PlotLine(label.c_str(), g.scratch_t.data(), g.scratch_v.data(), n, spec);
        spec.Marker = ImPlotMarker_Circle;
        if (g.dots) // the latest point
        {
            spec.MarkerSize = 5.0f * px;
            ImPlot::PlotScatter(label.c_str(), &g.scratch_t.back(), &g.scratch_v.back(), 1, spec);
        }
        for (int k = 0; g.cursor_on && (png == nullptr || png->cursors) && k < 2; ++k) // the curve's point at cursor time A / B
        {
            const double x = xs.at_cursor[static_cast<std::size_t>(k)];
            const double y = s.at_cursor[static_cast<std::size_t>(k)];
            if (!std::isnan(x) && !std::isnan(y))
            {
                spec.LineColor = spec.FillColor = cursor_color(k);
                spec.MarkerSize = 6.0f * px;
                ImPlot::PlotScatter(label.c_str(), &x, &y, 1, spec);
                ImPlot::Annotation(x, y, cursor_color(k), ImVec2(8.0f * px, -8.0f * px), true, "%s", k == 0 ? "A" : "B");
            }
        }
    }
    if (png == nullptr)
    {
        const ImPlotRect r = ImPlot::GetPlotLimits();
        g.xy_shown = {r.X.Min, r.X.Max, r.Y.Min, r.Y.Max};
    }
    ImPlot::EndPlot();
}

// png: drawn for the PNG export (off screen, no input): the Y ranges on screen, its own decimation,
// cursors as static lines, legend and cursor values as chosen.
void draw_plots(App& app, GraphState& g, std::span<const int> slots, const GraphPngOptions* png = nullptr)
{
    const int slot_count = slots.empty() ? 1 : *std::max_element(slots.begin(), slots.end()) + 1;
    const int rows = (slot_count + graph_axes_per_plot - 1) / graph_axes_per_plot;
    update_cursor_values(g);

    // Title of a subplot: the units on it, as the screenshot's per-plot titles.
    const auto row_title = [&](int row)
    {
        std::string title;
        for (std::size_t i = 0; i < g.signals.size(); ++i)
        {
            const std::string& u = g.signals[i].unit.empty() ? g.signals[i].parent : g.signals[i].unit;
            if (slots[i] / graph_axes_per_plot == row && title.find(u) == std::string::npos)
            {
                title += title.empty() ? u : ", " + u;
            }
        }
        return std::format("{}##row{}", title.empty() ? "Graph" : title, row);
    };

    if (g.y_shown.size() < static_cast<std::size_t>(rows * graph_axes_per_plot))
    {
        g.y_shown.resize(static_cast<std::size_t>(rows * graph_axes_per_plot), {0.0, 0.0});
    }
    const float line_scale = png != nullptr && png->style == GraphPngStyle::Print ? 1.4f : 1.0f; // print: bolder curves
    const auto plot_row = [&](int row)
    {
        ImPlot::SetupAxis(ImAxis_X1, row == rows - 1 ? "time" : nullptr);
        ImPlot::SetupAxisFormat(ImAxis_X1, format_time_tick, &g);
        ImPlot::SetupAxisLinks(ImAxis_X1, &g.x_min, &g.x_max);
        bool used[graph_axes_per_plot] = {};
        for (int k = 0; k < graph_axes_per_plot; ++k)
        {
            const int slot = row * graph_axes_per_plot + k;
            // Label: the units on the axis, "name [unit]" when one signal is alone on it; text in
            // the colour of its first signal so each curve finds its axis.
            std::string label;
            const GraphSignal* first = nullptr;
            int on_axis = 0;
            for (std::size_t i = 0; i < g.signals.size(); ++i)
            {
                if (slots[i] != slot)
                {
                    continue;
                }
                first = first != nullptr ? first : &g.signals[i];
                ++on_axis;
                if (label.find(g.signals[i].unit) == std::string::npos)
                {
                    label += label.empty() ? g.signals[i].unit : ", " + g.signals[i].unit;
                }
            }
            if (first == nullptr && k > 0)
            {
                continue;
            }
            if (on_axis == 1)
            {
                label = first->unit.empty() ? first->name : std::format("{} [{}]", first->name, first->unit);
            }
            ImPlotAxisFlags flags = k > 0 ? ImPlotAxisFlags_AuxDefault : ImPlotAxisFlags_None;
            // A file view keeps fitting Y to the visible window: its samples are that window only.
            flags |= png == nullptr && (g.follow || !app.trace.file.empty()) ? ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit
                                                                              : ImPlotAxisFlags_None;
            if (first != nullptr)
            {
                ImPlot::PushStyleColor(ImPlotCol_AxisText, png_color(png, ImGui::ColorConvertU32ToFloat4(first->color)));
            }
            ImPlot::SetupAxis(ImAxis_Y1 + k, label.empty() ? nullptr : label.c_str(), flags);
            const std::array<double, 2>& shown = g.y_shown[static_cast<std::size_t>(slot)];
            if (png != nullptr && shown[1] > shown[0])
            {
                ImPlot::SetupAxisLimits(ImAxis_Y1 + k, shown[0], shown[1], ImPlotCond_Always);
            }
            if (first != nullptr)
            {
                ImPlot::PopStyleColor();
            }
            used[k] = true;
        }
        ImPlot::SetupFinish();
        for (int k = 0; png == nullptr && k < graph_axes_per_plot; ++k)
        {
            if (used[k])
            {
                const ImPlotRange y = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1 + k).Y;
                g.y_shown[static_cast<std::size_t>(row * graph_axes_per_plot + k)] = {y.Min, y.Max};
            }
        }
        if (g.log_t >= 0.0) // the Log's position (file view)
        {
            ImPlotSpec line;
            line.LineColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            line.LineWeight = 1.0f;
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::PlotInfLines("##log", &g.log_t, 1, line);
        }
        if (png == nullptr && ImPlot::IsPlotHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
            && ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 9.0f) // a click, not a pan
        {
            g.click_t = ImPlot::GetPlotMousePos(ImAxis_X1).x;
        }

        const int buckets = std::max(1, static_cast<int>(ImPlot::GetPlotSize().x));
        if (png == nullptr)
        {
            g.plot_px = buckets; // for the next file-view window (one frame late is fine)
        }
        std::vector<double> png_t; // the export's own decimation: the screen's cache stays as it is
        std::vector<double> png_v;
        const float px = ImGui::GetFontSize() / 15.0f;
        for (std::size_t i = 0; i < g.signals.size(); ++i)
        {
            GraphSignal& s = g.signals[i];
            if (slots[i] / graph_axes_per_plot != row || s.hidden)
            {
                continue;
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1 + slots[i] % graph_axes_per_plot);
            const auto [t, v] = stride(g, s);
            // Decimated once per change of window / width / samples, not per frame (perf: 2-4 % of the
            // main thread went to re-decimating an unchanged view on every redraw).
            const std::array<double, 5> key{g.x_min, g.x_max, static_cast<double>(buckets), static_cast<double>(t.size()),
                                            static_cast<double>(g.downsample)};
            if (png != nullptr)
            {
                graph_decimate(t, v, g.x_min, g.x_max, buckets, png_t, png_v);
            }
            else if (key != s.dec_key)
            {
                s.dec_key = key;
                graph_decimate(t, v, g.x_min, g.x_max, buckets, s.dec_t, s.dec_v);
            }
            const std::vector<double>& dt = png != nullptr ? png_t : s.dec_t;
            const std::vector<double>& dv = png != nullptr ? png_v : s.dec_v;
            const ImVec4 color = png_color(png, ImGui::ColorConvertU32ToFloat4(s.color));
            ImPlotSpec spec;
            spec.LineColor = color;
            spec.FillColor = color;
            // Dots only while the samples are sparse: after min/max decimation there are two points
            // per pixel column and markers would only thicken the line (and double the vertex count).
            if (g.dots && dt.size() < static_cast<std::size_t>(buckets) / 2)
            {
                // A marker shape per signal, so series are not told apart by hue alone (ui-ux-pro-max:
                // charts, accessibility); the shape cycles with the slot's colour index.
                constexpr ImPlotMarker shapes[] = {ImPlotMarker_Circle, ImPlotMarker_Square, ImPlotMarker_Diamond,
                                                   ImPlotMarker_Up, ImPlotMarker_Down, ImPlotMarker_Cross};
                spec.Marker = shapes[i % std::size(shapes)];
                spec.MarkerSize = 2.5f * px;
            }
            spec.LineWeight = 2.5f * px * line_scale;
            const std::string label = std::format("{}##{}", s.name, i);
            ImPlot::PlotLine(label.c_str(), dt.data(), dv.data(), static_cast<int>(dt.size()), spec);
            for (int k = 0; g.cursor_on && (png == nullptr || png->cursors) && k < 2; ++k)
            {
                const double y = s.at_cursor[static_cast<std::size_t>(k)];
                if (!std::isnan(y))
                {
                    ImPlot::Annotation(k == 0 ? g.cursor_a : g.cursor_b, y, color, ImVec2(8.0f * px, -8.0f * px), true,
                                       "%s: %s %s", s.name.c_str(), graph_format_value(s, y).c_str(), s.unit.c_str());
                }
            }
        }

        const auto accept = [&](int slot)
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(drag_payload))
            {
                const int i = *static_cast<const int*>(p->Data);
                if (i >= 0 && i < static_cast<int>(g.signals.size()))
                {
                    g.signals[static_cast<std::size_t>(i)].axis = slot;
                    g.slots_dirty = true;
                }
            }
            ImGui::EndDragDropTarget();
        };
        if (png != nullptr)
        {
            // The export: cursors as plain lines with their tags, nothing to drag.
            for (int k = 0; g.cursor_on && png->cursors && k < 2; ++k)
            {
                const double x = k == 0 ? g.cursor_a : g.cursor_b;
                ImPlotSpec line;
                line.LineColor = cursor_color(k);
                line.LineWeight = 1.5f * px;
                ImPlot::PlotInfLines(k == 0 ? "##A" : "##B", &x, 1, line);
                ImPlot::TagX(x, cursor_color(k), "%s %s%s", k == 0 ? "A" : "B", format_duration(x, 3).c_str(), std::abs(x) < 60.0 ? " s" : "");
            }
            return;
        }
        for (int k = 0; k < graph_axes_per_plot; ++k)
        {
            // Only axes set up above: an unused Y2/Y3 has id 0 and asserts in BeginDragDropTargetCustom.
            if (k > 0 && std::find(slots.begin(), slots.end(), row * graph_axes_per_plot + k) == slots.end())
            {
                continue;
            }
            if (ImPlot::BeginDragDropTargetAxis(ImAxis_Y1 + k))
            {
                accept(row * graph_axes_per_plot + k);
            }
        }
        if (ImPlot::BeginDragDropTargetPlot()) // the plot area: a new slot, i.e. a new axis or subplot
        {
            accept(slot_count);
        }

        if (g.place_cursor_y && row == 0)
        {
            const ImPlotRange y = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1).Y;
            g.cursor_y1 = y.Min + y.Size() / 3.0;
            g.cursor_y2 = y.Min + y.Size() * 2.0 / 3.0;
            g.place_cursor_y = false;
        }
        bool cursor_held = false;
        for (int k = 0; g.cursor_on && k < 2; ++k)
        {
            double& x = k == 0 ? g.cursor_a : g.cursor_b;
            bool held = false;
            ImPlot::DragLineX(k, &x, cursor_color(k), 1.5f, ImPlotDragToolFlags_None, nullptr, nullptr, &held);
            ImPlot::TagX(x, cursor_color(k), "%s %s%s", k == 0 ? "A" : "B", format_duration(x, 3).c_str(), std::abs(x) < 60.0 ? " s" : "");
            cursor_held |= held;
        }
        for (int k = 0; g.cursor_y_on && row == 0 && k < 2; ++k)
        {
            double& y = k == 0 ? g.cursor_y1 : g.cursor_y2;
            bool held = false;
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::DragLineY(2 + k, &y, cursor_color(k), 1.5f, ImPlotDragToolFlags_None, nullptr, nullptr, &held);
            ImPlot::TagY(y, cursor_color(k), "%s %.6g", k == 0 ? "Y1" : "Y2", y);
            cursor_held |= held;
        }
        const ImGuiIO& io = ImGui::GetIO();
        // a / b over the plot put cursor A / B at the mouse (typed characters, as vim_nav: any layout;
        // vim_nav has no a/b and only acts on the focused list).
        if (ImPlot::IsPlotHovered() && !io.WantTextInput)
        {
            for (const ImWchar c : io.InputQueueCharacters)
            {
                if (c == 'a' || c == 'b')
                {
                    (c == 'a' ? g.cursor_a : g.cursor_b) = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x;
                    g.cursor_on = true;
                }
            }
        }
        stop_follow_on_user_zoom(g, cursor_held);
    };

    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImPlotFlags plot_flags = png != nullptr && !png->legend ? ImPlotFlags_NoLegend : ImPlotFlags_None;
    if (rows == 1)
    {
        if (ImPlot::BeginPlot(row_title(0).c_str(), size, plot_flags))
        {
            plot_row(0);
            ImPlot::EndPlot();
        }
    }
    else if (ImPlot::BeginSubplots("##graphs", rows, 1, size, ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoTitle))
    {
        for (int row = 0; row < rows; ++row)
        {
            if (ImPlot::BeginPlot(row_title(row).c_str(), ImVec2(-1, 0), plot_flags))
            {
                plot_row(row);
                ImPlot::EndPlot();
            }
        }
        ImPlot::EndSubplots();
    }
}

// Long format: signal, t, value. One row per sample of every visible signal.
void export_csv(const GraphState& g, const std::string& path)
{
    std::ofstream out(path, std::ios::binary);
    out << "signal,unit,time_s,value\n";
    for (const auto& s : g.signals)
    {
        if (s.hidden)
        {
            continue;
        }
        for (std::size_t i = 0; i < s.t.size(); ++i)
        {
            out << s.name << ',' << s.unit << ',' << std::format("{:.6f},{:.9g}", s.t[i], s.v[i]) << '\n';
        }
    }
    if (!out)
    {
        log_error(std::format("Graph: could not write {}", path));
    }
}

// Text cards and gauges share the grid; `cell` draws one signal.
template <class Cell>
void draw_grid(GraphState& g, Cell cell)
{
    if (!ImGui::BeginTable("##grid", std::max(1, g.columns), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY))
    {
        return;
    }
    for (std::size_t i = 0; i < g.signals.size(); ++i)
    {
        if (g.signals[i].hidden)
        {
            continue;
        }
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        cell(g.signals[i]);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_text_card(const GraphSignal& s)
{
    const float font = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Border, s.color);
    if (ImGui::BeginChild("##card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
    {
        ImGui::TextDisabled("%s", s.parent.c_str());
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::PushFont(nullptr, font * 2.0f);
        const std::string value = graph_format_value(s, s.v.empty() ? std::numeric_limits<double>::quiet_NaN() : s.v.back());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s.color), "%s", value.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(s.unit.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Porthole dial as the instrument panel's gauge, with the signal's colour as the water line up to
// the needle. DBC min/max, or the seen range when the DBC has none. Text grows with the dial.
void draw_gauge(const GraphSignal& s)
{
    const float font = ImGui::GetFontSize();
    const float w = ImGui::GetContentRegionAvail().x;
    const float r = std::clamp(w * 0.42f, font * 3.0f, font * 14.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + w * 0.5f, p.y + r + 4.0f);
    double lo = s.min;
    double hi = s.max;
    if (hi <= lo)
    {
        lo = s.seen_min;
        hi = s.seen_max > s.seen_min ? s.seen_max : s.seen_min + 1.0;
    }
    const double v = s.v.empty() ? lo : s.v.back();
    const float frac = static_cast<float>(std::clamp((v - lo) / (hi - lo), 0.0, 1.0));
    const float a = dial_start + frac * dial_span;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* face = ImGui::GetFont();
    const auto text_at = [&](float size, float x_anchor, float align, float y, ImU32 col, const std::string& text)
    {
        const float tw = face->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
        dl->AddText(face, size, ImVec2(x_anchor - tw * align, y), col, text.c_str());
    };
    dial_face(dl, c, r);
    dial_ticks(dl, c, r, 0.80f);
    if (!s.v.empty())
    {
        dl->PathArcTo(c, r * 0.88f, dial_start, a, 48);
        dl->PathStroke(s.color, ImDrawFlags_None, r * 0.08f);
        dial_needle(dl, c, r * 0.78f, a, IM_COL32(0xe6, 0xf4, 0xf1, 255));
    }
    dl->AddCircleFilled(c, r * 0.08f, dial_brass);

    // Value inside the dial below the centre, as wide as the dial allows.
    const std::string value = std::format("{} {}", graph_format_value(s, s.v.empty() ? std::nan("") : v), s.unit);
    const float value_w = std::max(face->CalcTextSizeA(font, FLT_MAX, 0.0f, value.c_str()).x, 1.0f);
    // In the wedge under the scale's ends, where the needle never points.
    const float value_size = std::max(font * 0.8f, std::min(r * 0.3f, font * r * 0.9f / value_w)); // readable on a small dial
    text_at(value_size, c.x, 0.5f, c.y + r * 0.45f, IM_COL32_WHITE, value);

    // Scale ends and name outside the dial, in the theme's text colours.
    const float small = std::clamp(r * 0.16f, font, font * 1.6f);
    const float name_size = std::clamp(r * 0.2f, font, font * 2.0f);
    const float bottom = c.y + r * 0.82f;
    text_at(small, c.x - r, 0.0f, bottom, ImGui::GetColorU32(ImGuiCol_TextDisabled), graph_format_value(s, lo));
    text_at(small, c.x + r, 1.0f, bottom, ImGui::GetColorU32(ImGuiCol_TextDisabled), graph_format_value(s, hi));
    text_at(name_size, c.x, 0.5f, c.y + r + small * 0.4f, ImGui::GetColorU32(ImGuiCol_Text), s.name);
    ImGui::Dummy(ImVec2(w, c.y + r + small * 0.4f + name_size * 1.4f - p.y));
}

struct PngSize
{
    const char* label;
    int w, h;
};
constexpr PngSize png_sizes[] = {{"As on screen", 0, 0},          {"1280 x 720", 1280, 720},  {"1920 x 1080 (Full HD)", 1920, 1080},
                                 {"2560 x 1440", 2560, 1440},     {"3840 x 2160 (4K)", 3840, 2160}, {"Custom", 0, 0}};
constexpr const char* png_scales[] = {"Auto", "1x", "1.5x", "2x", "3x"};
constexpr float png_scale_values[] = {0.0f, 1.0f, 1.5f, 2.0f, 3.0f};
constexpr int png_max_side = 8192; // within any GL_MAX_TEXTURE_SIZE a desktop GPU has

// The visible signals' names, the export's default title.
std::string png_auto_title(const GraphState& g)
{
    std::string t;
    for (const GraphSignal& s : g.signals)
    {
        if (!s.hidden)
        {
            t += (t.empty() ? "" : ", ") + s.name;
        }
    }
    return t.empty() ? std::string("Graph") : t;
}

void draw_png_dialog(GraphState& g)
{
    constexpr const char* popup = "Export Graph as PNG";
    if (g.png_dialog)
    {
        ImGui::OpenPopup(popup);
        g.png_dialog = false;
    }
    if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    GraphPngOptions& o = g.png;
    const float em = ImGui::GetFontSize();
    ImGui::SeparatorText("Image");
    ImGui::SetNextItemWidth(em * 14.0f);
    ImGui::Combo("Size", &o.size, [](void*, int i) { return png_sizes[i].label; }, nullptr, static_cast<int>(std::size(png_sizes)));
    if (o.size == static_cast<int>(std::size(png_sizes)) - 1)
    {
        ImGui::SetNextItemWidth(em * 6.0f);
        ImGui::InputInt("##w", &o.width, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 6.0f);
        ImGui::InputInt("##h", &o.height, 0);
        o.width = std::clamp(o.width, 64, png_max_side);
        o.height = std::clamp(o.height, 64, png_max_side);
    }
    ImGui::SetNextItemWidth(em * 14.0f);
    ImGui::Combo("Text and lines", &o.scale, png_scales, static_cast<int>(std::size(png_scales)));
    ImGui::SetItemTooltip("Auto grows with the height: 1x up to about 900 px, 2.4x at 4K");
    ImGui::SeparatorText("Style");
    int style = static_cast<int>(o.style);
    ImGui::RadioButton("Kraken", &style, 0);
    ImGui::SetItemTooltip("The app's current theme");
    ImGui::SameLine();
    ImGui::RadioButton("Light", &style, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Print", &style, 2);
    ImGui::SetItemTooltip("White, black text, bolder curves");
    ImGui::SameLine();
    ImGui::RadioButton("Transparent", &style, 3);
    ImGui::SetItemTooltip("No background, for slides");
    o.style = static_cast<GraphPngStyle>(style);
    ImGui::SeparatorText("Contents");
    ImGui::SetNextItemWidth(em * 22.0f);
    ImGui::InputTextWithHint("Title", png_auto_title(g).c_str(), &o.title);
    ImGui::Checkbox("Time range under the title", &o.subtitle);
    ImGui::Checkbox("Legend", &o.legend);
    ImGui::BeginDisabled(!g.cursor_on);
    ImGui::Checkbox("Cursors and their values", &o.cursors);
    ImGui::EndDisabled();
    if (!g.cursor_on)
    {
        ImGui::SetItemTooltip("Turn the cursors on in the graph first");
    }
    ImGui::Checkbox("Kraken Explorer watermark", &o.watermark);
    const auto [w, h] = graph_png_size(o, g.view_w, g.view_h);
    ImGui::TextDisabled("%d x %d px, text and lines %.1fx", w, h, graph_png_scale(o, h));
    ImGui::Separator();
    if (ImGui::Button("Save..."))
    {
        file_dialog_open(g.export_dialog, FileDialogMode::Save, "Export plot as PNG", g.png_last, {{"PNG image", "*.png"}});
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The graph once more, in a window of the export's size that never reaches the screen: main.cpp
// renders its draw list into a framebuffer (App::PngExport). Same data, axes and cursors as on
// screen; the style's colours, a title, the scale's font size and line widths.
void draw_png_export(App& app, GraphState& g)
{
    const GraphPngOptions& o = g.png;
    const auto [w, h] = graph_png_size(o, g.view_w, g.view_h);
    const float scale = graph_png_scale(o, h);
    const bool light = o.style == GraphPngStyle::Light || o.style == GraphPngStyle::Print;
    const bool transparent = o.style == GraphPngStyle::Transparent;
    const ImVec4 ink = light ? ImVec4(0.09f, 0.14f, 0.16f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 dim = light ? ImVec4(0.33f, 0.40f, 0.43f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec4 paper = o.style == GraphPngStyle::Print ? ImVec4(1, 1, 1, 1)
                         : light                         ? ImVec4(0.97f, 0.98f, 0.98f, 1.0f)
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    // A window's clip rect is cut to its viewport: for this one Begin the main viewport is as big as
    // the image, so a 4K export from a smaller screen keeps all of it.
    const ImVec2 screen = vp->Size;
    vp->Size = ImVec2(std::max(vp->Size.x, static_cast<float>(w)), std::max(vp->Size.y, static_cast<float>(h)));
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)));
    ImGui::SetNextWindowViewport(vp->ID);
    const float pad = 28.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    const float base = ImGui::GetStyle().FontSizeBase * scale;
    ImGui::PushFont(nullptr, base);
    ImPlot::PushStyleColor(ImPlotCol_FrameBg, ImVec4(0, 0, 0, 0));
    if (light || transparent)
    {
        ImPlot::PushStyleColor(ImPlotCol_PlotBg, transparent ? ImVec4(0, 0, 0, 0) : ImVec4(1, 1, 1, light ? 0.0f : 1.0f));
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, light ? ImVec4(0, 0, 0, 0.10f) : ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid]);
        ImPlot::PushStyleColor(ImPlotCol_AxisText, ink);
        ImPlot::PushStyleColor(ImPlotCol_TitleText, ink);
        ImPlot::PushStyleColor(ImPlotCol_LegendText, ink);
        ImPlot::PushStyleColor(ImPlotCol_LegendBg, light ? ImVec4(1, 1, 1, 0.85f) : ImVec4(0, 0, 0, 0.35f));
        ImPlot::PushStyleColor(ImPlotCol_LegendBorder, light ? ImVec4(0, 0, 0, 0.15f) : ImVec4(1, 1, 1, 0.2f));
        ImPlot::PushStyleColor(ImPlotCol_PlotBorder, light ? ImVec4(0, 0, 0, 0.20f) : ImVec4(1, 1, 1, 0.25f));
    }
    const auto scaled = [scale](auto v)
    {
        if constexpr (std::is_same_v<decltype(v), ImVec2>)
        {
            return ImVec2(v.x * scale, v.y * scale);
        }
        else
        {
            return v * scale;
        }
    };
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, scaled(ImPlot::GetStyle().PlotPadding));
    ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, scaled(ImPlot::GetStyle().LabelPadding));
    ImPlot::PushStyleVar(ImPlotStyleVar_LegendPadding, scaled(ImPlot::GetStyle().LegendPadding));
    ImPlot::PushStyleVar(ImPlotStyleVar_LegendInnerPadding, scaled(ImPlot::GetStyle().LegendInnerPadding));
    ImPlot::PushStyleVar(ImPlotStyleVar_LegendSpacing, scaled(ImPlot::GetStyle().LegendSpacing));
    ImPlot::PushStyleVar(ImPlotStyleVar_MajorTickLen, scaled(ImPlot::GetStyle().MajorTickLen));
    ImPlot::PushStyleVar(ImPlotStyleVar_MinorTickLen, scaled(ImPlot::GetStyle().MinorTickLen));
    ImPlot::PushStyleVar(ImPlotStyleVar_MajorTickSize, scaled(ImPlot::GetStyle().MajorTickSize));
    ImPlot::PushStyleVar(ImPlotStyleVar_MinorTickSize, scaled(ImPlot::GetStyle().MinorTickSize));
    ImPlot::PushStyleVar(ImPlotStyleVar_MajorGridSize, scaled(ImPlot::GetStyle().MajorGridSize));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, scaled(ImPlot::GetStyle().PlotBorderSize));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
                                       | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing
                                       | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##graph_png", nullptr, flags))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + static_cast<float>(w), p0.y + static_cast<float>(h));
        if (!transparent)
        {
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(paper));
            if (!light) // a deep-sea glow from the bottom, as the app's chrome
            {
                const ImU32 glow = ImGui::GetColorU32(ImVec4(0.05f, 0.45f, 0.50f, 0.18f));
                dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - h * 0.45f), p1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), glow, glow);
            }
        }
        ImGui::PushFont(nullptr, base * 1.7f);
        ImGui::TextUnformatted(o.title.empty() ? png_auto_title(g).c_str() : o.title.c_str());
        ImGui::PopFont();
        if (o.subtitle)
        {
            const double span = g.x_max - g.x_min;
            int shown = 0;
            for (const GraphSignal& s : g.signals)
            {
                shown += s.hidden ? 0 : 1;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, dim);
            ImGui::Text("%s - %s  (%s%s)  ·  %d signal%s", format_duration(g.x_min, 3).c_str(), format_duration(g.x_max, 3).c_str(),
                        format_duration(span, 3).c_str(), span < 60.0 ? " s" : "", shown, shown == 1 ? "" : "s");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));
        const float mark_h = o.watermark ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
        if (ImGui::BeginChild("##png_plot", ImVec2(0.0f, -mark_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            switch (g.view)
            {
            case GraphView::TimeSeries: draw_plots(app, g, graph_slots(g), &o); break;
            case GraphView::XY: draw_xy(app, g, &o); break;
            case GraphView::Text: draw_grid(g, draw_text_card); break;
            case GraphView::Gauge: draw_grid(g, draw_gauge); break;
            }
        }
        ImGui::EndChild();
        if (o.watermark)
        {
            char squid[5] = {};
            ImTextCharToUtf8(squid, theme_squid_codepoint);
            const std::string mark = std::string(squid) + " Kraken Explorer";
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(dim.x, dim.y, dim.z, 0.8f));
            ImGui::SetCursorPosX(static_cast<float>(w) - pad - ImGui::CalcTextSize(mark.c_str()).x);
            ImGui::TextUnformatted(mark.c_str());
            ImGui::PopStyleColor();
        }
        app.png_export = App::PngExport{.x = p0.x, .y = p0.y, .w = w, .h = h, .transparent = transparent, .path = g.png_path};
    }
    const ImGuiWindow* root = ImGui::GetCurrentWindow();
    ImGui::End();
    if (app.png_export)
    {
        for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows) // the export window and its children, drawn this frame
        {
            if (win->RootWindow == root && win->Active)
            {
                app.png_export->lists.push_back(win->DrawList);
            }
        }
    }
    ImPlot::PopStyleVar(11);
    ImPlot::PopStyleColor(light || transparent ? 9 : 1);
    ImGui::PopFont();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    vp->Size = screen;
    g.png_path.clear();
}

void draw_toolbar(App& app, GraphState& g)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    int view = static_cast<int>(g.view);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("View Type:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f * px);
    if (ImGui::Combo("##view", &view, view_names, IM_ARRAYSIZE(view_names)))
    {
        g.view = static_cast<GraphView>(view);
    }
    ImGui::SameLine();
    const bool plot = g.view == GraphView::TimeSeries || g.view == GraphView::XY;
    if (plot)
    {
        ImGui::TextUnformatted("Duration:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f * px);
        if (ImGui::Combo("##duration", &g.duration, duration_names, IM_ARRAYSIZE(duration_names)))
        {
            g.follow = true;
        }
        ImGui::SameLine();
        const double mid = (g.x_min + g.x_max) * 0.5;
        const double half = (g.x_max - g.x_min) * 0.5;
        if (ImGui::Button("Zoom +"))
        {
            g.follow = false;
            g.x_min = mid - half * 0.5;
            g.x_max = mid + half * 0.5;
        }
        ImGui::SameLine();
        if (ImGui::Button("Zoom -"))
        {
            g.follow = false;
            g.x_min = mid - half * 2.0;
            g.x_max = mid + half * 2.0;
        }
        same_line_or_wrap(icon_text_button_width("Reset Zoom"));
        if (icon_text_button("Reset Zoom", Icon::ViewRefresh))
        {
            g.follow = true;
        }
        ImGui::SetItemTooltip("Follow the newest data and fit the Y axes");
    }
    else
    {
        ImGui::TextUnformatted("Columns:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f * px);
        ImGui::SliderInt("##columns", &g.columns, 1, 4);
    }
    same_line_or_wrap(icon_text_button_width("Clear"));
    if (icon_text_button("Clear", Icon::EditClear))
    {
        clear_samples(g);
        g.next_index = app.trace.end;
    }
}

namespace
{

// File view: the graph keeps to the Log. The Log's top row is a line in the plot; when the Log
// moves outside the visible window the window follows (same zoom), and a click in the plot puts
// the Log on that time. ponytail: file view only, the live Log scrolls in pixels.
void sync_with_log(App& app, const WorkspaceTab& tab, GraphState& g)
{
    g.log_t = -1.0;
    const Trace& t = app.trace;
    const auto it = app.trace_windows.find(tab.uid);
    if (t.file.empty() || g.start_ns < 0 || it == app.trace_windows.end())
    {
        g.click_t = -1.0;
        return;
    }
    TraceWindowState& s = it->second;
    const bool filtered = !s.file_filtered.empty();
    const uint64_t n = filtered ? s.file_filtered.size() : t.file.size();
    if (g.click_t >= 0.0)
    {
        const int64_t ts = g.start_ns + static_cast<int64_t>(g.click_t * 1e9);
        trace_window_goto(s, t, static_cast<uint64_t>(std::ranges::lower_bound(t.file, ts, {}, &FrameCacheRec::ts_ns) - t.file.begin()), 0);
        g.click_t = -1.0;
    }
    if (n == 0)
    {
        return;
    }
    const uint64_t top = std::min(s.file_top, n - 1);
    g.log_t = static_cast<double>(t.file[filtered ? s.file_filtered[top] : top].ts_ns - g.start_ns) * 1e-9;
    if (g.log_t < g.x_min || g.log_t > g.x_max)
    {
        const double half = (g.x_max - g.x_min) * 0.5;
        g.follow = false;
        g.x_min = g.log_t - half;
        g.x_max = g.log_t + half;
    }
}

} // namespace

void draw_graph(App& app, const WorkspaceTab& tab, GraphState& g)
{
    for (GraphSignal& s : g.signals) // palette colours follow a theme switch (dark ones vanish on light)
    {
        s.color = theme_signal_color_remap(s.color);
    }
    const std::string title = g.id == 0 ? std::string("Graph") : std::format("Graph {}", g.id);
    const float px = ImGui::GetFontSize() / 15.0f;
    if (g.id != 0)
    {
        ImGui::SetNextWindowSize(ImVec2(900.0f * px, 550.0f * px), ImGuiCond_FirstUseEver);
    }
    if (g.standalone)
    {
        ImGuiWindowClass wc;
        wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge; // its own OS window
        ImGui::SetNextWindowClass(&wc);
    }
    if (g.dock_into != 0)
    {
        ImGui::SetNextWindowDockID(g.dock_into, ImGuiCond_Always);
        g.dock_into = 0;
    }
    const bool visible = ImGui::Begin(workspace_window_name(tab, title.c_str()).c_str(), g.id == 0 ? nullptr : &g.open);
    if (visible)
    {
        draw_toolbar(app, g);
        if (const TriggerState trig = conditional_logging_state(app.conditional_logging); trig != TriggerState::Off)
        {
            const unsigned c = theme_text(trig == TriggerState::Armed ? ThemeText::warn : ThemeText::rec);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(c), "%s%s", trigger_state_label(trig),
                               trig == TriggerState::Armed ? " (the graph starts when the condition is met)" : "");
        }
        follow_window(app, g); // every view: the statistics use the window in Text / Gauge too
        sync_with_log(app, tab, g);
        if (ImGui::BeginChild("##side", ImVec2(280.0f * px, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders))
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            const SignalEntry* hit = signal_search_input(g.finder, app.setup, "Search signals...", g.search);
            if (ImGui::BeginChild("##tree", ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.45f),
                                  ImGuiChildFlags_ResizeY | ImGuiChildFlags_Borders))
            {
                if (g.search.empty())
                {
                    draw_signal_tree(app, g);
                }
                else if (const SignalEntry* e = signal_search_list(g.finder))
                {
                    hit = e;
                }
            }
            ImGui::EndChild();
            if (hit != nullptr)
            {
                add_search_hit(g, app.setup, *hit);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Downsample:");
            ImGui::SameLine(110.0f * px);
            ImGui::SetNextItemWidth(80.0f * px);
            if (ImGui::InputInt("##downsample", &g.downsample))
            {
                g.downsample = std::max(1, g.downsample);
            }
            ImGui::SetItemTooltip("Keep every Nth sample before drawing");
            ImGui::TextUnformatted("Cursors:");
            ImGui::SameLine(110.0f * px);
            if (ImGui::Checkbox("##cursors", &g.cursor_on) && g.cursor_on)
            {
                g.cursor_a = g.x_min + (g.x_max - g.x_min) / 3.0;
                g.cursor_b = g.x_min + (g.x_max - g.x_min) * 2.0 / 3.0;
            }
            ImGui::SetItemTooltip("Cursors A and B: drag them, or press a / b over the time series plot.\n"
                                  "XY shows each curve's point at the cursor times.");
            ImGui::TextUnformatted("Y cursors:");
            ImGui::SameLine(110.0f * px);
            if (ImGui::Checkbox("##cursors_y", &g.cursor_y_on) && g.cursor_y_on)
            {
                g.place_cursor_y = true; // at 1/3 and 2/3 of the first plot's Y1 range, known while plotting
            }
            ImGui::SetItemTooltip("Horizontal cursors Y1 and Y2 on the first plot's left axis: drag them to read a level\n"
                                  "and the difference between two levels.");
            ImGui::TextUnformatted("Statistics:");
            ImGui::SameLine(110.0f * px);
            ImGui::Checkbox("##statistics", &g.statistics);
            ImGui::SetItemTooltip("Min / max / mean / median / std dev per signal:\nbetween cursors A and B when they are on, else of the visible window.");
            ImGui::TextUnformatted("Dots:");
            ImGui::SameLine(110.0f * px);
            ImGui::Checkbox("##dots", &g.dots);
            ImGui::SetItemTooltip("Mark every sample on the curves");
            const float button_h = ImGui::GetFrameHeightWithSpacing() * 2.0f; // CSV + PNG export
            if (ImGui::BeginChild("##list", ImVec2(0.0f, -button_h)))
            {
                // After the tree: a checkbox there may have added or removed a signal this frame.
                draw_signal_list(g, graph_slots(g));
            }
            ImGui::EndChild();
            if (ImGui::Button("Export plot to *.csv", ImVec2(-FLT_MIN, 0.0f)))
            {
                file_dialog_open(g.export_dialog, FileDialogMode::Save, "Export plot", "graph.csv", {{"CSV files", "*.csv"}});
            }
            if (ImGui::Button("Export to PNG...", ImVec2(-FLT_MIN, 0.0f)))
            {
                g.png_dialog = true;
            }
            draw_png_dialog(g);
            for (const auto& path : file_dialog_draw(g.export_dialog))
            {
                if (path.ends_with(".png"))
                {
                    g.png_path = g.png_last = path; // drawn off screen at the end of this window, rendered by main.cpp
                }
                else
                {
                    export_csv(g, path);
                }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##view"))
        {
            g.view_w = ImGui::GetWindowSize().x;
            g.view_h = ImGui::GetWindowSize().y;
            // The list may have removed a signal this frame; the slots must match again.
            const std::vector<int>& now_slots = graph_slots(g);
            switch (g.view)
            {
            case GraphView::TimeSeries:
                draw_plots(app, g, now_slots);
                break;
            case GraphView::XY:
                draw_xy(app, g);
                break;
            case GraphView::Text:
                draw_grid(g, draw_text_card);
                break;
            case GraphView::Gauge:
                draw_grid(g, draw_gauge);
                break;
            }
            if (!g.png_path.empty())
            {
                draw_png_export(app, g);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// Ctrl+P: centred "Find signal" popup; Enter / double-click adds the hit to the tab's default graph.
void draw_find_signal(App& app, const WorkspaceTab& tab, GraphState& g)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * px, 380.0f * px), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Find signal"))
    {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) // one Esc closes, even from inside the search field
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::TextUnformatted("Find signal");
    ImGui::SetNextItemWidth(-FLT_MIN);
    const SignalEntry* hit = signal_search_input(g.palette, app.setup, "Message.Signal...", g.palette_query, ImGui::IsWindowAppearing());
    if (ImGui::BeginChild("##hits", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
    {
        if (const SignalEntry* e = signal_search_list(g.palette))
        {
            hit = e;
        }
    }
    ImGui::EndChild();
    if (hit != nullptr)
    {
        add_search_hit(g, app.setup, *hit);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (hit != nullptr)
    {
        ImGui::SetWindowFocus(workspace_window_name(tab, "Graph").c_str());
    }
}

// A <signal> of a saved graph back to a GraphSignal of the current setup; false when it is gone.
bool load_signal(const Setup& setup, const std::deque<Iface>& ifaces, pugi::xml_node el, GraphSignal& out)
{
    const std::string_view kind = el.attribute("kind").as_string();
    if (kind == "busload")
    {
        const std::string_view driver = el.attribute("driver").as_string();
        const std::string_view iface = el.attribute("iface").as_string();
        const int index = ifaces_find(ifaces, driver, iface);
        for (const auto& net : setup.networks)
        {
            for (const auto& si : net.interfaces)
            {
                if (index >= 0 && si.name == iface && si.driver == driver)
                {
                    out = make_bus_load_signal(index, si);
                    return true;
                }
            }
        }
        return false;
    }
    const std::string_view parent = el.attribute("message").as_string();
    const std::string_view name = el.attribute("name").as_string();
    for (const auto& net : setup.networks)
    {
        if (net.name != el.attribute("network").as_string())
        {
            continue;
        }
        for (const auto& db : net.can_dbs)
        {
            for (const auto& [raw_id, msg] : db->messages)
            {
                const CanDbSignal* sig = kind == "can" && msg.name == parent ? can_db_find_signal(msg, name) : nullptr;
                if (sig != nullptr)
                {
                    out = make_can_signal(net, raw_id, msg, *sig);
                    return true;
                }
            }
        }
        for (const auto& db : net.lin_dbs)
        {
            for (const auto& [id, frame] : db->frames)
            {
                if (kind != "lin" || frame.name != parent)
                {
                    continue;
                }
                const auto sig = std::find_if(frame.signals.begin(), frame.signals.end(), [&](const LinSignal& ls) { return ls.name == name; });
                if (sig != frame.signals.end())
                {
                    out = make_lin_signal(net, id, frame, *sig);
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

std::pair<int, int> graph_png_size(const GraphPngOptions& o, float screen_w, float screen_h)
{
    const int last = static_cast<int>(std::size(png_sizes)) - 1;
    const int i = std::clamp(o.size, 0, last);
    if (i == 0)
    {
        return {std::clamp(static_cast<int>(screen_w), 64, png_max_side), std::clamp(static_cast<int>(screen_h), 64, png_max_side)};
    }
    if (i == last)
    {
        return {std::clamp(o.width, 64, png_max_side), std::clamp(o.height, 64, png_max_side)};
    }
    return {png_sizes[i].w, png_sizes[i].h};
}

float graph_png_scale(const GraphPngOptions& o, int h)
{
    const int i = std::clamp(o.scale, 0, static_cast<int>(std::size(png_scales)) - 1);
    return i == 0 ? std::clamp(static_cast<float>(h) / 900.0f, 1.0f, 4.0f) : png_scale_values[i];
}

double graph_value_at(std::span<const double> t, std::span<const double> v, double time)
{
    const auto it = std::upper_bound(t.begin(), t.end(), time);
    return it == t.begin() ? std::numeric_limits<double>::quiet_NaN() : v[static_cast<std::size_t>(it - t.begin() - 1)];
}

const std::array<double, 3>& graph_cursor_values(GraphSignal& s, double a, double b)
{
    const std::array<double, 4> key{a, b, static_cast<double>(s.t.size()), s.t.empty() ? 0.0 : s.t.back()};
    if (key != s.cursor_key)
    {
        const double va = graph_value_at(s.t, s.v, a);
        const double vb = graph_value_at(s.t, s.v, b);
        s.at_cursor = {va, vb, vb - va};
        s.cursor_key = key;
    }
    return s.at_cursor;
}

std::string graph_delta_t_text(double a, double b)
{
    const double dt = b - a;
    return std::format("\u0394t = {:.6g} ms  (1/\u0394t = {} Hz)", dt * 1e3,
                       dt == 0.0 ? std::string("-") : std::format("{:.6g}", 1.0 / std::abs(dt)));
}

std::string graph_format_value(const GraphSignal& s, double v)
{
    if (std::isnan(v))
    {
        return "-";
    }
    return s.kind == GraphSignalKind::Can && s.can_sig != nullptr ? can_signal_format(*s.can_sig, v) : std::format("{:.6g}", v);
}

void graph_save_xml(std::span<const GraphState> graphs, const std::deque<Iface>& ifaces, pugi::xml_node tab)
{
    for (const GraphState& g : graphs)
    {
        if (!g.open)
        {
            continue;
        }
        pugi::xml_node el = tab.append_child("graph");
        el.append_attribute("id") = g.id;
        el.append_attribute("standalone") = g.standalone;
        el.append_attribute("view") = view_keys[static_cast<int>(g.view)];
        el.append_attribute("duration") = g.duration;
        el.append_attribute("columns") = g.columns;
        el.append_attribute("x-signal") = g.x_signal;
        el.append_attribute("downsample") = g.downsample;
        el.append_attribute("cursors") = g.cursor_on;
        el.append_attribute("cursor-a") = g.cursor_a; // seconds since the measurement's first frame
        el.append_attribute("cursor-b") = g.cursor_b;
        el.append_attribute("statistics") = g.statistics;
        el.append_attribute("dots") = g.dots;
        el.append_attribute("cursors-y") = g.cursor_y_on;
        el.append_attribute("cursor-y1") = g.cursor_y1;
        el.append_attribute("cursor-y2") = g.cursor_y2;
        for (const GraphSignal& s : g.signals)
        {
            pugi::xml_node se = el.append_child("signal");
            se.append_attribute("kind") = kind_keys[static_cast<int>(s.kind)];
            if (s.kind == GraphSignalKind::BusLoad)
            {
                const bool known = s.iface >= 0 && static_cast<std::size_t>(s.iface) < ifaces.size();
                const Iface* iface = known ? &ifaces[static_cast<std::size_t>(s.iface)] : nullptr;
                se.append_attribute("driver") = iface != nullptr && iface->ops != nullptr ? iface->ops->name : "";
                se.append_attribute("iface") = iface != nullptr ? iface->info.name.c_str() : "";
            }
            else
            {
                se.append_attribute("network") = s.network.c_str();
                se.append_attribute("message") = s.parent.c_str();
                se.append_attribute("name") = s.name.c_str();
            }
            se.append_attribute("color") = std::format("0x{:08X}", s.color).c_str();
            se.append_attribute("axis") = s.axis;
            se.append_attribute("hidden") = s.hidden;
        }
    }
}

void graph_load_xml(std::vector<GraphState>& graphs, const Setup& setup, const std::deque<Iface>& ifaces,
                    pugi::xml_node tab)
{
    constexpr int duration_count = static_cast<int>(std::size(duration_names));
    for (const pugi::xml_node el : tab.children("graph"))
    {
        GraphState& g = graphs.emplace_back();
        g.id = el.attribute("id").as_uint();
        g.standalone = el.attribute("standalone").as_bool();
        const std::string_view view = el.attribute("view").as_string();
        const auto* v = std::find(std::begin(view_keys), std::end(view_keys), view);
        g.view = v == std::end(view_keys) ? GraphView::TimeSeries : static_cast<GraphView>(v - std::begin(view_keys));
        g.duration = std::clamp(el.attribute("duration").as_int(1), 0, duration_count - 1);
        g.columns = std::clamp(el.attribute("columns").as_int(2), 1, 4);
        g.downsample = std::max(1, el.attribute("downsample").as_int(1));
        g.cursor_on = el.attribute("cursors").as_bool();
        g.cursor_a = el.attribute("cursor-a").as_double();
        g.cursor_b = el.attribute("cursor-b").as_double();
        g.statistics = el.attribute("statistics").as_bool();
        g.dots = el.attribute("dots").as_bool(true);
        g.cursor_y_on = el.attribute("cursors-y").as_bool();
        g.cursor_y1 = el.attribute("cursor-y1").as_double();
        g.cursor_y2 = el.attribute("cursor-y2").as_double();
        int x_signal = el.attribute("x-signal").as_int();
        int index = 0;
        for (const pugi::xml_node se : el.children("signal"))
        {
            GraphSignal s;
            if (!load_signal(setup, ifaces, se, s))
            {
                log_warning(std::format("Graph: signal {} {}.{} not in the setup, dropped", se.attribute("kind").as_string(),
                                        se.attribute("message").as_string(se.attribute("iface").as_string()),
                                        se.attribute("name").as_string()));
                x_signal -= index < x_signal ? 1 : 0;
                ++index;
                continue;
            }
            s.color = se.attribute("color").as_uint(0xFFFFFFFF);
            s.axis = std::clamp(se.attribute("axis").as_int(-1), -1, 4 * graph_axes_per_plot - 1);
            s.hidden = se.attribute("hidden").as_bool();
            g.signals.push_back(std::move(s));
            ++index;
        }
        g.x_signal = std::clamp(x_signal, 0, std::max(0, static_cast<int>(g.signals.size()) - 1));
    }
}

std::vector<int> graph_assign_slots(std::span<const GraphSignal> signals)
{
    // Per slot: unit and range of its first signal, and whether automatic signals may join it.
    struct Slot
    {
        std::string_view unit;
        double span = 0.0; // max - min, 0 = unknown (fits any)
        bool automatic = false;
        bool used = false;
    };
    std::vector<Slot> slot_info(4 * graph_axes_per_plot);
    const auto span_of = [](const GraphSignal& s) { return s.max > s.min ? s.max - s.min : 0.0; };
    // |log10| of the range ratio, 0 when either range is unknown.
    const auto distance = [](double a, double b) { return a > 0.0 && b > 0.0 ? std::abs(std::log10(a / b)) : 0.0; };
    std::vector<int> slots(signals.size(), -1);
    for (std::size_t i = 0; i < signals.size(); ++i)
    {
        if (const auto axis = static_cast<std::size_t>(signals[i].axis); signals[i].axis >= 0 && axis < slot_info.size()
                                                                          && !slot_info[axis].used)
        {
            slot_info[axis] = {signals[i].unit, span_of(signals[i]), false, true};
        }
        slots[i] = signals[i].axis;
    }
    for (std::size_t i = 0; i < signals.size(); ++i)
    {
        if (slots[i] >= 0)
        {
            continue;
        }
        const double span = span_of(signals[i]);
        int best = -1;
        for (int k = 0; k < graph_axes_per_plot && best < 0; ++k) // same unit, range within 10x
        {
            const Slot& sl = slot_info[static_cast<std::size_t>(k)];
            best = sl.automatic && sl.unit == signals[i].unit && distance(sl.span, span) <= 1.0 ? k : -1;
        }
        for (int k = 0; k < graph_axes_per_plot && best < 0; ++k) // next free axis
        {
            if (!slot_info[static_cast<std::size_t>(k)].used)
            {
                best = k;
                slot_info[static_cast<std::size_t>(k)] = {signals[i].unit, span, true, true};
            }
        }
        if (best < 0) // Y1..Y3 all taken: nearest range
        {
            best = 0;
            for (int k = 1; k < graph_axes_per_plot; ++k)
            {
                if (distance(slot_info[static_cast<std::size_t>(k)].span, span) < distance(slot_info[static_cast<std::size_t>(best)].span, span))
                {
                    best = k;
                }
            }
        }
        slots[i] = best;
    }
    return slots;
}

void graph_decimate(std::span<const double> t, std::span<const double> v, double x0, double x1, int buckets,
                    std::vector<double>& out_t, std::vector<double>& out_v)
{
    out_t.clear();
    out_v.clear();
    auto lo = std::lower_bound(t.begin(), t.end(), x0);
    auto hi = std::upper_bound(lo, t.end(), x1);
    lo = lo == t.begin() ? lo : lo - 1; // one neighbour each side so the line reaches the edges
    hi = hi == t.end() ? hi : hi + 1;
    const auto i0 = static_cast<std::size_t>(lo - t.begin());
    const auto i1 = static_cast<std::size_t>(hi - t.begin());
    buckets = std::max(buckets, 1);
    if (i1 - i0 <= 2 * static_cast<std::size_t>(buckets))
    {
        out_t.assign(t.begin() + static_cast<std::ptrdiff_t>(i0), t.begin() + static_cast<std::ptrdiff_t>(i1));
        out_v.assign(v.begin() + static_cast<std::ptrdiff_t>(i0), v.begin() + static_cast<std::ptrdiff_t>(i1));
        return;
    }
    const double w = (x1 - x0) / buckets;
    const auto bucket = [&](double x) { return std::clamp(static_cast<long>(std::floor((x - x0) / w)), -1L, static_cast<long>(buckets)); };
    for (std::size_t i = i0; i < i1;)
    {
        const long b = bucket(t[i]);
        std::size_t mn = i;
        std::size_t mx = i;
        std::size_t j = i + 1;
        for (; j < i1 && bucket(t[j]) == b; ++j)
        {
            mn = v[j] < v[mn] ? j : mn;
            mx = v[j] > v[mx] ? j : mx;
        }
        for (std::size_t k : {std::min(mn, mx), std::max(mn, mx)})
        {
            out_t.push_back(t[k]);
            out_v.push_back(v[k]);
            if (mn == mx)
            {
                break;
            }
        }
        i = j;
    }
}

GraphLod graph_lod_build(std::vector<double> t, std::vector<double> v, std::size_t top)
{
    GraphLod lod;
    lod.t.push_back(std::move(t));
    lod.v.push_back(std::move(v));
    while (lod.t.back().size() / graph_lod_fan >= top)
    {
        const std::vector<double>& pt = lod.t.back();
        const std::vector<double>& pv = lod.v.back();
        std::vector<double> nt;
        std::vector<double> nv;
        nt.reserve(2 * (pt.size() / graph_lod_fan + 1));
        nv.reserve(nt.capacity());
        for (std::size_t i = 0; i < pt.size(); i += graph_lod_fan)
        {
            const std::size_t end = std::min(i + graph_lod_fan, pt.size());
            std::size_t mn = i;
            std::size_t mx = i;
            for (std::size_t j = i + 1; j < end; ++j)
            {
                mn = pv[j] < pv[mn] ? j : mn;
                mx = pv[j] > pv[mx] ? j : mx;
            }
            for (std::size_t k : {std::min(mn, mx), std::max(mn, mx)}) // always two: buckets = points / 2
            {
                nt.push_back(pt[k]);
                nv.push_back(pv[k]);
            }
        }
        lod.t.push_back(std::move(nt));
        lod.v.push_back(std::move(nv));
    }
    return lod;
}

std::size_t graph_lod_level(const GraphLod& lod, double x0, double x1, int pixels)
{
    const std::size_t need = 4 * static_cast<std::size_t>(std::max(pixels, 1));
    for (std::size_t level = lod.t.size(); level-- > 1;)
    {
        const std::vector<double>& t = lod.t[level];
        const auto lo = std::lower_bound(t.begin(), t.end(), x0);
        const auto hi = std::upper_bound(lo, t.end(), x1);
        if (static_cast<std::size_t>(hi - lo) / 2 >= need)
        {
            return level;
        }
    }
    return 0;
}

void graph_lod_window(const GraphLod& lod, std::size_t level, double x0, double x1, std::vector<double>& out_t,
                      std::vector<double>& out_v)
{
    const std::vector<double>& t = lod.t[level];
    const std::vector<double>& v = lod.v[level];
    auto lo = std::lower_bound(t.begin(), t.end(), x0);
    auto hi = std::upper_bound(lo, t.end(), x1);
    lo = lo == t.begin() ? lo : lo - 1; // one neighbour each side so the line reaches the edges
    hi = hi == t.end() ? hi : hi + 1;
    out_t.assign(lo, hi);
    out_v.assign(v.begin() + (lo - t.begin()), v.begin() + (hi - t.begin()));
}

void graph_xy_pair(std::span<const double> x_t, std::span<const double> x_v, std::span<const double> y_t,
                   std::span<const double> y_v, double t0, double t1, std::vector<double>& out_x,
                   std::vector<double>& out_y)
{
    out_x.clear();
    out_y.clear();
    const auto lo = std::lower_bound(y_t.begin(), y_t.end(), t0);
    const auto hi = std::upper_bound(lo, y_t.end(), t1);
    // Two-pointer merge: one binary search to the window start, then xi only moves forward.
    auto xi = lo == hi ? x_t.end() : std::upper_bound(x_t.begin(), x_t.end(), *lo);
    for (auto it = lo; it != hi; ++it)
    {
        while (xi != x_t.end() && *xi <= *it) // xi = first X after this Y; the one before it holds
        {
            ++xi;
        }
        if (xi == x_t.begin())
        {
            continue;
        }
        out_x.push_back(x_v[static_cast<std::size_t>(xi - x_t.begin() - 1)]);
        out_y.push_back(y_v[static_cast<std::size_t>(it - y_t.begin())]);
    }
}

namespace
{

// Decodes s over the whole file into a pyramid on its own thread: the frame cache's per-id index
// gives the message's frames, the mapping stays alive through the shared_ptr, and the DB entries
// are copied so a DBC reload meanwhile cannot pull them away. The job is dropped (stop + join)
// with the signal, so the loop checks the stop token.
std::shared_ptr<GraphLodJob> start_lod_job(const App& app, const GraphSignal& s, int64_t start_ns)
{
    const FrameCache& c = *app.trace_file;
    std::vector<uint32_t> list = frame_cache_message_frames(c, app.setup, s.can_msg);
    auto job = std::make_shared<GraphLodJob>();
    job->thread = std::jthread([job = job.get(), keep = app.trace_file, list = std::move(list), msg = *s.can_msg, name = s.name, start_ns,
                                wake = app.tasks.wake](const std::stop_token& stop)
    {
        const CanDbSignal* sig = can_db_find_signal(msg, name);
        std::vector<double> t;
        std::vector<double> v;
        for (std::size_t k = 0; sig != nullptr && k < list.size(); ++k)
        {
            if ((k & 0xFFFF) == 0 && stop.stop_requested())
            {
                return;
            }
            const BusMessage m = frame_cache_frame(*keep, list[k]);
            if (can_signal_present(msg, *sig, m))
            {
                t.push_back(static_cast<double>(m.ts_ns - start_ns) * 1e-9);
                v.push_back(can_signal_extract_physical(*sig, m));
            }
        }
        job->result = std::make_shared<const GraphLod>(graph_lod_build(std::move(t), std::move(v)));
        job->done.store(true, std::memory_order_release);
        if (wake != nullptr)
        {
            wake(); // the main loop is event-driven: draw the finished signal now
        }
    });
    return job;
}

// File view (a loaded file in the trace): every CAN signal gets a pyramid of the whole file
// (start_lod_job) and shows nothing until it is built; then each window change copies the
// coarsest level still finer than a quarter pixel into s.t / s.v, which graph_decimate reduces
// per pixel as in live mode. Redone when the window, the plot width, the signal set or the setup
// changes. ponytail: statistics and cursor values read that window, so zoomed out they are of
// the bucket extremes, exact once a level-0 window fits.
void graph_file_window(GraphState& g, const App& app)
{
    if (app.trace_file == nullptr)
    {
        return;
    }
    const FrameCache& c = *app.trace_file;
    if (g.start_ns < 0)
    {
        g.start_ns = c.recs.front().ts_ns;
        g.last_t = static_cast<double>(c.recs.back().ts_ns - g.start_ns) * 1e-9;
        g.last_wall = wall_seconds();
        g.follow = false; // open on the whole file
        g.x_min = 0.0;
        g.x_max = std::max(g.last_t, 1e-3);
    }
    bool arrived = false;
    for (GraphSignal& s : g.signals)
    {
        if (s.kind != GraphSignalKind::Can || s.can_msg == nullptr)
        {
            continue; // ponytail: LIN and bus load are live-only for now
        }
        if (s.lod_file.lock() != app.trace_file || s.lod_generation != app.setup.generation)
        {
            s.lod_file = app.trace_file;
            s.lod_generation = app.setup.generation;
            s.lod.reset();
            s.lod_job = start_lod_job(app, s, g.start_ns);
        }
        if (s.lod_job != nullptr && s.lod_job->done.load(std::memory_order_acquire))
        {
            s.lod = s.lod_job->result;
            s.lod_job.reset();
            const std::vector<double>& top = s.lod->v.back(); // holds the extremes of the whole file
            s.seen_min = top.empty() ? 0.0 : *std::ranges::min_element(top);
            s.seen_max = top.empty() ? 0.0 : *std::ranges::max_element(top);
            arrived = true;
        }
    }
    const std::array<double, 5> key{g.x_min, g.x_max, static_cast<double>(g.signals.size()),
                                    static_cast<double>(app.setup.generation), static_cast<double>(g.plot_px)};
    if (key == g.file_key && !arrived)
    {
        return;
    }
    g.file_key = key;
    for (GraphSignal& s : g.signals)
    {
        s.t.clear();
        s.v.clear();
        if (s.lod != nullptr)
        {
            graph_lod_window(*s.lod, graph_lod_level(*s.lod, g.x_min, g.x_max, g.plot_px), g.x_min, g.x_max, s.t, s.v);
        }
    }
}

} // namespace

void graph_ingest(GraphState& g, const App& app)
{
    const Trace& tr = app.trace;
    if (g.trace_clears != tr.clears)
    {
        g.trace_clears = tr.clears;
        clear_samples(g);
    }
    if (g.setup_generation != app.setup.generation)
    {
        g.setup_generation = app.setup.generation;
        resolve_signals(g, app.setup);
    }
    if (!tr.file.empty())
    {
        graph_file_window(g, app);
        g.next_index = tr.end;
        return;
    }
    if (g.signals.empty())
    {
        g.next_index = tr.end;
        return;
    }
    if (conditional_logging_state(app.conditional_logging) == TriggerState::Armed)
    {
        // The graph starts with the trigger: frames that arrive while armed are skipped.
        // ponytail: per app frame (condition_met after the newest frame), not per sample.
        g.next_index = tr.end;
        return;
    }
    const uint64_t first = std::max(g.next_index, tr.begin);
    if (g.start_ns < 0 && first < tr.end)
    {
        // t0 = earliest frame of the first batch, so no sample lands at a negative time.
        g.start_ns = trace_at(tr, first).ts_ns;
        for (uint64_t i = first + 1; i < tr.end; ++i)
        {
            g.start_ns = std::min(g.start_ns, trace_at(tr, i).ts_ns);
        }
    }
    for (uint64_t i = first; i < tr.end; ++i)
    {
        const BusMessage& m = trace_at(tr, i);
        const double t = static_cast<double>(m.ts_ns - g.start_ns) * 1e-9;
        // Resolved in the DBC/LDF of m.iface's network, so a signal only takes its own
        // network's frames (interfaces in no network search every network).
        const CanDbMessage* can_db = setup_find_can_message(app.setup, m);
        const LinFrame* lin_frame = setup_find_lin_frame(app.setup, m);
        const int net = setup_network_of(app.setup, m.iface);
        const auto in_network = [&](const GraphSignal& s)
        { return net < 0 || app.setup.networks[static_cast<std::size_t>(net)].name == s.network; };
        g.last_t = std::max(g.last_t, t);
        for (auto& s : g.signals)
        {
            switch (s.kind)
            {
            case GraphSignalKind::Can:
                if (can_db != nullptr && can_db == s.can_msg && can_signal_present(*s.can_msg, *s.can_sig, m))
                {
                    push_sample(s, t, can_signal_extract_physical(*s.can_sig, m));
                }
                break;
            case GraphSignalKind::Lin:
                if (lin_frame != nullptr && lin_frame->id == s.lin_id && in_network(s))
                {
                    const LinSignal* sig = lin_frame_find_signal(*lin_frame, s.name);
                    if (sig != nullptr)
                    {
                        push_sample(s, t, lin_signal_extract_physical(*sig, {m.data.data(), m.len}));
                    }
                }
                break;
            case GraphSignalKind::BusLoad:
                if (m.iface == s.iface)
                {
                    const uint32_t bits = bus_frame_bits(m);
                    s.load_window.emplace_back(t, bits);
                    s.load_bits += bits;
                    sample_bus_load(s, t);
                }
                break;
            }
        }
    }
    if (first < tr.end)
    {
        g.last_wall = wall_seconds();
    }
    g.next_index = tr.end;
    // A quiet bus gets no frames of its own: age the bus-load windows so the curve falls to 0.
    if (app.measuring && g.start_ns >= 0)
    {
        const double now = graph_now(g, true);
        for (auto& s : g.signals)
        {
            if (s.kind == GraphSignalKind::BusLoad)
            {
                sample_bus_load(s, now);
            }
        }
    }
}

namespace
{

// "+" after the last tab of the dock node that holds the docked "Graph": returns that node's id
// when clicked. Drawn into the node's host window, which owns the tab bar.
ImGuiID graph_tab_add_button(const App& app, const WorkspaceTab& tab)
{
    const ImGuiWindow* graph = ImGui::FindWindowByName(workspace_window_name(tab, "Graph").c_str());
    const ImGuiDockNode* node = graph != nullptr ? graph->DockNode : nullptr;
    if (node == nullptr || node->TabBar == nullptr || node->TabBar->Tabs.empty() || node->HostWindow == nullptr
        || !node->HostWindow->WasActive)
    {
        return 0;
    }
    const ImGuiTabBar& bar = *node->TabBar;
    const ImGuiTabItem& last = bar.Tabs.back();
    const ImVec2 pos(bar.BarRect.Min.x + last.Offset + last.Width - bar.ScrollingAnim, bar.BarRect.Min.y);
    ImGui::Begin(node->HostWindow->Name);
    ImGui::PushClipRect(bar.BarRect.Min, bar.BarRect.Max, false);
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool clicked = ImGui::Button("+##graph_add", ImVec2(bar.BarRect.GetHeight(), bar.BarRect.GetHeight()));
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("New graph  %s", command_shortcut(app.menu, Command::NewGraphView));
    ImGui::PopClipRect();
    ImGui::End();
    return clicked ? node->ID : 0;
}

} // namespace

void draw_graph_windows(App& app, WorkspaceTab* current)
{
    const bool view = menu_take(app.menu, Command::NewGraphView);
    const bool widget = menu_take(app.menu, Command::NewGraphWidget);
    const bool standalone = menu_take(app.menu, Command::StandaloneGraph);
    const ImGuiID dock_into = current != nullptr ? graph_tab_add_button(app, *current) : 0;
    if (current != nullptr && (view || widget || standalone || dock_into != 0))
    {
        unsigned id = 1;
        for (const auto& g : current->graphs)
        {
            id = std::max(id, g.id + 1);
        }
        GraphState& g = current->graphs.emplace_back();
        g.id = id;
        g.standalone = standalone;
        g.dock_into = dock_into;
    }
    for (auto& tab : app.workspace.tabs)
    {
        if (std::none_of(tab.graphs.begin(), tab.graphs.end(), [](const GraphState& g) { return g.id == 0; }))
        {
            tab.graphs.emplace(tab.graphs.begin()); // the docked default "Graph"
        }
        for (auto& g : tab.graphs)
        {
            graph_ingest(g, app);
        }
    }
    const bool find = menu_take(app.menu, Command::FindSignal);
    if (current == nullptr)
    {
        return;
    }
    for (auto& g : current->graphs)
    {
        draw_graph(app, *current, g);
    }
    GraphState& main = *std::find_if(current->graphs.begin(), current->graphs.end(), [](const GraphState& g) { return g.id == 0; });
    if (find)
    {
        main.palette_query.clear();
        ImGui::OpenPopup("Find signal");
    }
    draw_find_signal(app, *current, main);
    std::erase_if(current->graphs, [](const GraphState& g) { return !g.open; });
}

void graph_show_range(GraphState& g, const Setup& setup, const SignalEntry& e, int64_t t0_ns, int64_t t1_ns)
{
    add_search_hit(g, setup, e);
    if (g.start_ns < 0)
    {
        return; // no time base yet: nothing ingested
    }
    const double a = static_cast<double>(t0_ns - g.start_ns) * 1e-9;
    const double b = static_cast<double>(t1_ns - g.start_ns) * 1e-9;
    const double pad = std::max((b - a) * 0.25, 0.5);
    g.follow = false;
    g.x_min = a - pad;
    g.x_max = b + pad;
    g.cursor_on = true;
    g.cursor_a = a;
    g.cursor_b = b;
}
