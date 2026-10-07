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


#include "ui/value_search.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <cfloat>
#include <format>
#include <limits>

#include <imgui.h>
#include <imgui_internal.h> // dock builder, window settings lookup
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "core/text.h"
#include "db/model/can_db.h"
#include "ui/icons.h"
#include "ui/workspace_tabs.h"

namespace
{

double parse_bound(const std::string& text, double open)
{
    double v = 0.0;
    return parse_number(trim(text), v) ? v : open;
}

// Shows trace index `index` in this tab's Trace window, in whatever Monitor mode it has.
void log_goto(App& app, const WorkspaceTab& tab, uint64_t index)
{
    trace_window_select_frame(app.trace_windows[tab.uid], app.trace, index, 3); // a few rows of context above
}

// Folds one sample into the result: the seen range, and a hit when the value is in range.
struct Fold
{
    double lo;
    double hi;
    std::size_t max_hits;
    ValueResult& r;
};

void fold(Fold& f, uint64_t index, const BusMessage& m, double v)
{
    ValueResult& r = f.r;
    if (r.samples++ == 0)
    {
        r.seen_min = r.seen_max = v;
    }
    r.seen_min = std::min(r.seen_min, v);
    r.seen_max = std::max(r.seen_max, v);
    if (v < f.lo || v > f.hi)
    {
        return;
    }
    if (r.hits.size() == f.max_hits)
    {
        r.truncated = true;
        return;
    }
    r.hits.push_back({.index = index, .ts_ns = m.ts_ns, .value = v});
}

std::string status_of(const ValueResult& r)
{
    std::string count;
    append_grouped(count, r.hits.size());
    return std::format("{} hits{}", count, r.truncated ? std::format(" (first {})", value_search_max_hits) : "");
}

} // namespace

ValueResult value_search_scan(const FrameCache& c, std::span<const uint32_t> list, const CanDbMessage& msg, const CanDbSignal& sig,
                              double lo, double hi, std::size_t max_hits, std::stop_token stop, std::atomic<uint64_t>* progress)
{
    ValueResult r;
    Fold f{.lo = lo, .hi = hi, .max_hits = max_hits, .r = r};
    for (std::size_t k = 0; k < list.size(); ++k)
    {
        if (k % 65536 == 0 && k > 0)
        {
            if (stop.stop_requested())
            {
                return r;
            }
            if (progress != nullptr)
            {
                progress->store(k);
            }
        }
        const BusMessage m = frame_cache_frame(c, list[k]);
        if (can_signal_present(msg, sig, m))
        {
            fold(f, list[k], m, can_signal_extract_physical(sig, m));
        }
    }
    if (progress != nullptr)
    {
        progress->store(list.size());
    }
    return r;
}

ValueResult value_search_run(const App& app, const CanDbMessage& msg, const CanDbSignal& sig, double lo, double hi,
                             std::size_t max_hits)
{
    const Trace& t = app.trace;
    if (!t.file.empty() && app.trace_file != nullptr)
    {
        ValueResult r = value_search_scan(*app.trace_file, frame_cache_message_frames(*app.trace_file, app.setup, &msg), msg, sig,
                                          lo, hi, max_hits);
        for (ValueHit& h : r.hits)
        {
            h.index += t.begin; // trace index, as the live path reports
        }
        return r;
    }
    ValueResult r;
    Fold f{.lo = lo, .hi = hi, .max_hits = max_hits, .r = r};
    for (uint64_t i = t.begin; i < t.end; ++i)
    {
        const BusMessage m = trace_at(t, i);
        if (m.type == BusType::CAN && setup_find_can_message(app.setup, m) == &msg && can_signal_present(msg, sig, m))
        {
            fold(f, i, m, can_signal_extract_physical(sig, m));
        }
    }
    return r;
}

std::optional<RawPattern> raw_pattern_parse(std::string_view id_text, std::string_view data_text)
{
    RawPattern p;
    if (const std::string_view id = trim(id_text); !id.empty())
    {
        uint32_t v = 0;
        if (!parse_number(id.starts_with("0x") || id.starts_with("0X") ? id.substr(2) : id, v, 16))
        {
            return std::nullopt;
        }
        p.id = v;
    }
    // Hex pairs, spaces optional: "01 ?? FF", "01??FF"; ?? / XX / .. match any byte.
    std::string compact;
    for (const char c : data_text)
    {
        if (c != ' ' && c != '\t')
        {
            compact += c;
        }
    }
    if (compact.size() % 2 != 0 || compact.size() / 2 > bus_max_data_bytes)
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < compact.size(); i += 2)
    {
        const std::string_view pair(compact.data() + i, 2);
        if (pair == "??" || pair == "xx" || pair == "XX" || pair == "..")
        {
            p.bytes.push_back(std::nullopt);
            continue;
        }
        uint8_t b = 0;
        if (!parse_number(pair, b, 16))
        {
            return std::nullopt;
        }
        p.bytes.push_back(b);
    }
    return p;
}

bool raw_pattern_matches(const RawPattern& p, const BusMessage& m) noexcept
{
    if (p.id && can_id(m) != *p.id)
    {
        return false;
    }
    for (std::size_t i = 0; i < p.bytes.size(); ++i)
    {
        if (p.bytes[i] && (i >= m.len || m.data[i] != *p.bytes[i]))
        {
            return false;
        }
    }
    return true;
}

ValueResult raw_search_scan(const FrameCache& c, std::span<const uint32_t> list, bool all, const RawPattern& pattern,
                            std::size_t max_hits, std::stop_token stop, std::atomic<uint64_t>* progress)
{
    ValueResult r;
    const std::size_t n = all ? c.recs.size() : list.size();
    for (std::size_t k = 0; k < n; ++k)
    {
        if (k % 65536 == 0 && k > 0)
        {
            if (stop.stop_requested())
            {
                return r;
            }
            if (progress != nullptr)
            {
                progress->store(k);
            }
        }
        const uint32_t i = all ? static_cast<uint32_t>(k) : list[k];
        const BusMessage m = frame_cache_frame(c, i);
        ++r.samples;
        if (!raw_pattern_matches(pattern, m))
        {
            continue;
        }
        if (r.hits.size() == max_hits)
        {
            r.truncated = true;
            break;
        }
        r.hits.push_back({.index = i, .ts_ns = m.ts_ns, .value = 0.0});
    }
    if (progress != nullptr)
    {
        progress->store(n);
    }
    return r;
}

namespace
{

// File view: the frames to look at for a raw pattern: the id's rows (every channel), or everything.
std::vector<uint32_t> raw_candidates(const FrameCache& c, const RawPattern& p)
{
    std::vector<uint32_t> out;
    if (!p.id)
    {
        return out; // empty = all of the cache
    }
    std::size_t rows = 0;
    for (const FrameCacheRow& r : c.rows)
    {
        if (r.id == *p.id || (r.id & can_id_mask_extended) == *p.id)
        {
            const auto list = c.row_frames.subspan(r.first, r.count);
            out.insert(out.end(), list.begin(), list.end());
            ++rows;
        }
    }
    if (rows > 1)
    {
        std::ranges::sort(out);
    }
    if (out.empty())
    {
        out.push_back(UINT32_MAX); // no such id: scan nothing (an empty list would mean everything)
    }
    return out;
}

} // namespace

ValueResult raw_search_run(const App& app, const RawPattern& pattern, std::size_t max_hits)
{
    const Trace& t = app.trace;
    if (!t.file.empty() && app.trace_file != nullptr)
    {
        std::vector<uint32_t> list = raw_candidates(*app.trace_file, pattern);
        if (list.size() == 1 && list[0] == UINT32_MAX)
        {
            return {};
        }
        ValueResult r = raw_search_scan(*app.trace_file, list, list.empty(), pattern, max_hits);
        for (ValueHit& h : r.hits)
        {
            h.index += t.begin;
        }
        return r;
    }
    ValueResult r;
    for (uint64_t i = t.begin; i < t.end; ++i)
    {
        const BusMessage m = trace_at(t, i);
        ++r.samples;
        if (!raw_pattern_matches(pattern, m))
        {
            continue;
        }
        if (r.hits.size() == max_hits)
        {
            r.truncated = true;
            break;
        }
        r.hits.push_back({.index = i, .ts_ns = m.ts_ns, .value = 0.0});
    }
    return r;
}

namespace
{

// A finished scan into v: the bounds after a pick, else the table.
void take_result(ValueSearch& v, ValueResult r)
{
    if (v.range_only)
    {
        if (r.samples == 0)
        {
            v.status = "no samples in the trace";
            return;
        }
        v.from = std::format("{}", r.seen_min);
        v.to = std::format("{}", r.seen_max);
        v.status = std::format("in the trace: {} .. {}", v.from, v.to);
        return;
    }
    v.truncated = r.truncated;
    v.status = status_of(r);
    v.hits = std::move(r.hits);
    v.selected = -1;
}

} // namespace

void value_search_start(App& app, ValueSearch& v, double lo, double hi, bool range_only)
{
    v.worker = {}; // a running scan is stopped and joined
    v.job = {};
    v.range_only = range_only;
    if (!v.signal || v.signal->can_sig == nullptr)
    {
        return;
    }
    const CanDbMessage& msg = *v.signal->can_msg;
    const CanDbSignal& sig = *v.signal->can_sig;
    if (app.trace.file.empty() || app.trace_file == nullptr)
    {
        take_result(v, value_search_run(app, msg, sig, lo, hi)); // live trace: main thread only
        return;
    }
    std::vector<uint32_t> list = frame_cache_message_frames(*app.trace_file, app.setup, &msg);
    v.to_scan = list.size();
    v.scanned = 0;
    v.status = "Scanning...";
    // Copies of the DBC objects: a reload on the main thread must not pull them from under the scan.
    std::packaged_task<ValueResult(std::stop_token)> task(
        [cache = app.trace_file, list = std::move(list), msg = msg, sig = sig, lo, hi, progress = &v.scanned, begin = app.trace.begin,
         wake = app.tasks.wake](std::stop_token stop)
        {
            ValueResult r = value_search_scan(*cache, list, msg, sig, lo, hi, value_search_max_hits, stop, progress);
            for (ValueHit& h : r.hits)
            {
                h.index += begin;
            }
            if (wake != nullptr)
            {
                wake(); // the next frame polls the result
            }
            return r;
        });
    v.job = task.get_future();
    v.worker = std::jthread(std::move(task));
}

void raw_search_start(App& app, ValueSearch& v)
{
    v.worker = {};
    v.job = {};
    v.range_only = false;
    const auto pattern = raw_pattern_parse(v.raw_id, v.raw_data);
    if (!pattern)
    {
        v.status = "ID: hex; Data: hex pairs, ?? for any byte";
        return;
    }
    if (app.trace.file.empty() || app.trace_file == nullptr)
    {
        take_result(v, raw_search_run(app, *pattern));
        return;
    }
    std::vector<uint32_t> list = raw_candidates(*app.trace_file, *pattern);
    if (list.size() == 1 && list[0] == UINT32_MAX)
    {
        take_result(v, {});
        return;
    }
    const bool all = list.empty();
    v.to_scan = all ? app.trace_file->recs.size() : list.size();
    v.scanned = 0;
    v.status = "Scanning...";
    std::packaged_task<ValueResult(std::stop_token)> task(
        [cache = app.trace_file, list = std::move(list), all, pattern = *pattern, progress = &v.scanned, begin = app.trace.begin,
         wake = app.tasks.wake](std::stop_token stop)
        {
            ValueResult r = raw_search_scan(*cache, list, all, pattern, value_search_max_hits, stop, progress);
            for (ValueHit& h : r.hits)
            {
                h.index += begin;
            }
            if (wake != nullptr)
            {
                wake();
            }
            return r;
        });
    v.job = task.get_future();
    v.worker = std::jthread(std::move(task));
}

bool value_search_poll(ValueSearch& v)
{
    if (!v.job.valid() || v.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }
    take_result(v, v.job.get());
    v.worker = {};
    return true;
}

namespace
{

// The hits table: Time, then the value (signal) or the frame's id and data (raw), and the Index as
// the Log shows it. A click shows the frame in the Trace window and the Graph around it.
void draw_hits(App& app, ValueSearch& v, WorkspaceTab& tab)
{
    // Hits name frames that are gone: all of them when the trace was cleared or left its file
    // view (begin jumps past every old index), the oldest when a live trace pruned a chunk.
    if (!v.hits.empty() && (v.hits.front().index < app.trace.begin || v.hits.back().index >= app.trace.end))
    {
        const auto first = v.hits.back().index < app.trace.end
                               ? std::ranges::lower_bound(v.hits, app.trace.begin, {}, &ValueHit::index)
                               : v.hits.end();
        const auto dropped = static_cast<int>(first - v.hits.begin());
        v.hits.erase(v.hits.begin(), first);
        v.selected = v.selected >= dropped ? v.selected - dropped : -1;
        if (v.hits.empty())
        {
            v.status = "the trace changed: search again";
        }
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter
                                      | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##hits", v.raw ? 4 : 3, flags))
    {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time");
    if (v.raw)
    {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Data");
    }
    else
    {
        ImGui::TableSetupColumn("Value");
    }
    ImGui::TableSetupColumn("Index");
    ImGui::TableHeadersRow();
    const int64_t first_ts = trace_size(app.trace) > 0 ? trace_at(app.trace, app.trace.begin).ts_ns : 0;
    std::string b;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(v.hits.size()));
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const ValueHit& h = v.hits[static_cast<std::size_t>(r)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            b = format_duration(static_cast<double>(h.ts_ns - first_ts) / 1e9);
            ImGui::PushID(r);
            if (ImGui::Selectable(b.c_str(), r == v.selected, ImGuiSelectableFlags_SpanAllColumns))
            {
                v.selected = r;
                // Log on the frame, Graph around it (both cursors on the sample).
                log_goto(app, tab, h.index);
                if (!tab.graphs.empty() && !v.raw && v.signal)
                {
                    graph_show_range(tab.graphs.front(), app.setup, *v.signal, h.ts_ns, h.ts_ns);
                }
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (v.raw)
            {
                const BusMessage m = trace_at(app.trace, h.index);
                b.clear();
                append_id(b, m);
                ImGui::TextUnformatted(b.c_str());
                ImGui::TableNextColumn();
                b.clear();
                append_data(b, m, false);
                ImGui::TextUnformatted(b.c_str());
            }
            else
            {
                ImGui::Text("%g", h.value);
            }
            ImGui::TableNextColumn();
            b.clear();
            append_grouped(b, h.index - app.trace.begin + 1); // as the Log's Index column
            ImGui::TextUnformatted(b.c_str());
        }
    }
    ImGui::EndTable();
}

// Raw mode: an id and / or a data byte pattern.
void draw_raw_search(App& app, ValueSearch& v)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    value_search_poll(v);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("ID");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * px);
    bool run = ImGui::InputTextWithHint("##raw_id", "any (hex)", &v.raw_id, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextUnformatted("Data");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f * px);
    run |= ImGui::InputTextWithHint("##raw_data", "01 ?? FF (?? = any byte)", &v.raw_data, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemTooltip("Hex byte pairs from byte 0, ?? matches any value; shorter patterns leave the rest free");
    same_line_or_wrap(ImGui::CalcTextSize("Find").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    const bool scanning = v.job.valid();
    ImGui::BeginDisabled(scanning);
    run |= ImGui::Button("Find");
    ImGui::EndDisabled();
    if (run && !scanning)
    {
        raw_search_start(app, v);
    }
    if (scanning)
    {
        const uint64_t done = v.scanned;
        const std::string text = std::format("Scanning... {} %", v.to_scan == 0 ? 100 : static_cast<int>(done * 100 / v.to_scan));
        same_line_or_wrap(ImGui::CalcTextSize(text.c_str()).x);
        ImGui::TextDisabled("%s", text.c_str());
    }
    else if (!v.status.empty())
    {
        same_line_or_wrap(ImGui::CalcTextSize(v.status.c_str()).x);
        ImGui::TextDisabled("%s", v.status.c_str());
    }
}

} // namespace

void draw_value_search(App& app, ValueSearch& v, WorkspaceTab& tab)
{
    const std::string name = workspace_window_name(tab, "Value Search");
    // Value Search is a tab with Log and Python Script. A layout saved before it existed has no
    // place for it, and one saved before it moved has it split off to the right of Python Script:
    // put it in Python Script's node once, as the default layout does. A spot the user picked stays.
    if (!v.dock_checked)
    {
        const ImGuiWindow* py = ImGui::FindWindowByName(workspace_window_name(tab, "Python Script").c_str());
        if (py != nullptr && py->DockId != 0)
        {
            v.dock_checked = true;
            // Before its first Begin the window only exists as saved settings.
            const ImGuiWindow* self = ImGui::FindWindowByName(name.c_str());
            const ImGuiWindowSettings* saved = ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()));
            const ImGuiID own_id = self != nullptr ? self->DockId : saved != nullptr ? saved->DockId : 0;
            const ImGuiDockNode* own = ImGui::DockBuilderGetNode(own_id);
            const ImGuiDockNode* pyn = ImGui::DockBuilderGetNode(py->DockId);
            const bool unplaced = self == nullptr && saved == nullptr;
            const bool old_split = own != nullptr && pyn != nullptr && own != pyn && own->ParentNode != nullptr
                && own->ParentNode == pyn->ParentNode && own->Windows.Size <= 1;
            if (unplaced || old_split)
                ImGui::DockBuilderDockWindow(name.c_str(), py->DockId);
        }
    }
    if (!ImGui::Begin(name.c_str()))
    {
        ImGui::End();
        return;
    }
    if (v.setup_generation != app.setup.generation)
    {
        v.setup_generation = app.setup.generation;
        v.signal.reset(); // its pointers die with the old setup
        v.hits.clear();
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    // Two ways to search: a signal's value, or the raw frame (id, data bytes).
    if (ImGui::RadioButton("Signal", !v.raw))
    {
        v.raw = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Raw", v.raw))
    {
        v.raw = true;
    }
    if (v.raw)
    {
        draw_raw_search(app, v);
        draw_hits(app, v, tab);
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const SignalEntry* picked = signal_search_input(v.finder, app.setup, "Find signal (fuzzy)...", v.query);
    if (!v.query.empty())
    {
        if (ImGui::BeginChild("##signals", ImVec2(0.0f, 6.5f * ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_Borders))
        {
            if (const SignalEntry* e = signal_search_list(v.finder))
            {
                picked = e;
            }
        }
        ImGui::EndChild();
    }
    if (picked != nullptr)
    {
        v.signal = *picked;
        v.query.clear();
        v.hits.clear();
        v.status.clear();
        v.from.clear();
        v.to.clear();
        // The range the signal really takes in this trace, as the start.
        value_search_start(app, v, -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), true);
    }
    value_search_poll(v);

    const bool can = v.signal && v.signal->can_sig != nullptr;
    ImGui::AlignTextToFramePadding();
    if (v.signal)
    {
        ImGui::Text("Signal: %s", v.signal->label.c_str());
        if (v.signal->can_sig != nullptr && v.signal->can_sig->max > v.signal->can_sig->min)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(DBC %g .. %g)", v.signal->can_sig->min, v.signal->can_sig->max);
        }
        if (!can)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(LIN signals are not searched yet)");
        }
    }
    else
    {
        ImGui::TextDisabled("Pick a signal above");
    }
    ImGui::BeginDisabled(!can);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Value from");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f * px);
    bool run = ImGui::InputTextWithHint("##from", "-inf", &v.from, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f * px);
    run |= ImGui::InputTextWithHint("##to", "+inf", &v.to, ImGuiInputTextFlags_EnterReturnsTrue);
    if (can && !v.signal->can_sig->unit.empty())
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(v.signal->can_sig->unit.c_str());
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    same_line_or_wrap(ImGui::CalcTextSize("Find").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    const bool scanning = v.job.valid();
    ImGui::BeginDisabled(scanning);
    run |= ImGui::Button("Find");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (run && can && !scanning)
    {
        value_search_start(app, v, parse_bound(v.from, -std::numeric_limits<double>::infinity()),
                           parse_bound(v.to, std::numeric_limits<double>::infinity()), false);
    }
    if (scanning)
    {
        const uint64_t done = v.scanned;
        const std::string text = std::format("Scanning... {} %", v.to_scan == 0 ? 100 : static_cast<int>(done * 100 / v.to_scan));
        same_line_or_wrap(ImGui::CalcTextSize(text.c_str()).x);
        ImGui::TextDisabled("%s", text.c_str());
    }
    else if (!v.status.empty())
    {
        same_line_or_wrap(ImGui::CalcTextSize(v.status.c_str()).x);
        ImGui::TextDisabled("%s", v.status.c_str());
    }

    draw_hits(app, v, tab);
    ImGui::End();
}

