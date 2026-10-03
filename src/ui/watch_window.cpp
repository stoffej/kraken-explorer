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

#include "ui/watch_window.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include <imgui.h>
#include <pugixml.hpp>

#include "app.h"
#include "core/setup.h"
#include "core/text.h"
#include "core/trace.h"
#include "db/model/can_db.h"
#include "ui/theme.h"
#include "ui/workspace_tabs.h"

namespace
{

void resolve(WatchWindow& w, const Setup& setup)
{
    for (WatchItem& item : w.items)
    {
        item.msg = nullptr;
        item.sig = nullptr;
        for (const SetupNetwork& net : setup.networks)
        {
            if (net.name != item.network)
            {
                continue;
            }
            for (const auto& db : net.can_dbs)
            {
                if (const CanDbMessage* msg = can_db_find_message(*db, item.raw_id))
                {
                    if (const CanDbSignal* sig = can_db_find_signal(*msg, item.signal); sig && !item.sig)
                    {
                        item.msg = msg;
                        item.sig = sig;
                    }
                }
            }
        }
    }
}

// The value as the trace shows it: the value table's name when the raw value has one.
std::string value_text(const WatchItem& item)
{
    if (const std::string_view name = can_signal_value_name(*item.sig, item.raw); !name.empty())
    {
        return std::format("{} ({})", name, can_signal_format(*item.sig, item.value));
    }
    return can_signal_format(*item.sig, item.value);
}

// "Add signal..." combo: the fuzzy finder over every CAN signal of the setup.
void draw_add_combo(App& app, WatchWindow& w)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (!ImGui::BeginCombo("##add", "Add signal...", ImGuiComboFlags_HeightLarge))
    {
        return;
    }
    w.picker.can_only = true;
    const bool appearing = ImGui::IsWindowAppearing();
    if (appearing)
    {
        w.search.clear();
    }
    const SignalEntry* hit = signal_search_input(w.picker, app.setup, "Filter...", w.search, appearing);
    for (int row = 0; row < static_cast<int>(w.picker.hits.size()); ++row)
    {
        const SignalEntry& e = w.picker.entries[static_cast<std::size_t>(w.picker.hits[static_cast<std::size_t>(row)].entry)];
        ImGui::PushID(row);
        const std::string label = std::format("{}  ({})", e.label, app.setup.networks[e.network].name);
        // The combo stays open: several signals in one go.
        if (ImGui::Selectable(label.c_str(), !w.search.empty() && row == w.picker.selected, ImGuiSelectableFlags_NoAutoClosePopups))
        {
            hit = &e;
        }
        ImGui::PopID();
    }
    if (hit != nullptr)
    {
        watch_add(w, app.setup, *hit);
    }
    ImGui::EndCombo();
}

} // namespace

bool watch_add(WatchWindow& w, const Setup& setup, const SignalEntry& entry)
{
    if (entry.can_sig == nullptr || entry.network >= setup.networks.size())
    {
        return false;
    }
    const std::string& network = setup.networks[entry.network].name;
    if (std::ranges::any_of(w.items, [&](const WatchItem& i)
                            { return i.network == network && i.raw_id == entry.raw_id && i.signal == entry.can_sig->name; }))
    {
        return false;
    }
    w.items.push_back({.network = network, .raw_id = entry.raw_id, .signal = entry.can_sig->name, .msg = entry.can_msg, .sig = entry.can_sig});
    return true;
}

void watch_reset(WatchWindow& w)
{
    for (WatchItem& item : w.items)
    {
        item.has_value = false;
        item.changed = false;
        item.count = 0;
    }
}

void watch_ingest(WatchWindow& w, const Setup& setup, const Trace& trace)
{
    if (w.trace_clears != trace.clears)
    {
        w.trace_clears = trace.clears;
        watch_reset(w);
    }
    if (w.setup_generation != setup.generation)
    {
        w.setup_generation = setup.generation;
        resolve(w, setup);
    }
    const uint64_t first = trace.file.empty() ? std::max(w.next_index, trace.begin) : trace.end; // a file view is not live
    w.next_index = trace.end;
    if (w.items.empty())
    {
        return;
    }
    for (uint64_t i = first; i < trace.end; ++i)
    {
        const BusMessage& m = trace_at(trace, i);
        const CanDbMessage* msg = m.type == BusType::CAN && m.errors == 0 ? setup_find_can_message(setup, m) : nullptr;
        if (msg == nullptr)
        {
            continue;
        }
        for (WatchItem& item : w.items)
        {
            if (item.msg != msg || !can_signal_present(*msg, *item.sig, m))
            {
                continue;
            }
            const uint64_t raw = can_signal_extract_raw(*item.sig, m);
            const double value = can_signal_raw_to_physical(*item.sig, raw);
            item.changed = item.has_value && raw != item.raw;
            item.min = item.has_value ? std::min(item.min, value) : value;
            item.max = item.has_value ? std::max(item.max, value) : value;
            item.raw = raw;
            item.value = value;
            item.has_value = true;
            ++item.count;
        }
    }
}

void draw_watch_window(App& app, const WorkspaceTab& tab, WatchWindow& w)
{
    const WorkspaceTab* current = workspace_current(app.workspace);
    if (!w.open || current == nullptr || current->uid != tab.uid)
    {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(workspace_window_name(tab, "Watch").c_str(), &w.open))
    {
        draw_add_combo(app, w);
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
        {
            watch_reset(w);
        }
        ImGui::SetItemTooltip("Forget the values, min / max and counts");
        if (w.items.empty())
        {
            ImGui::TextDisabled("No signals: add the ones to keep an eye on.");
        }
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                          | ImGuiTableFlags_Resizable;
        if (!w.items.empty() && ImGui::BeginTable("##watch", 6, flags))
        {
            const float px = ImGui::GetFontSize() / 15.0f;
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 130.0f * px);
            ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, 50.0f * px);
            ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 80.0f * px);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 80.0f * px);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 70.0f * px);
            ImGui::TableHeadersRow();
            std::size_t remove = SIZE_MAX;
            std::pair<std::size_t, std::size_t> swap{SIZE_MAX, SIZE_MAX};
            for (std::size_t i = 0; i < w.items.size(); ++i)
            {
                const WatchItem& item = w.items[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string name = std::format("{}.{}", item.msg != nullptr ? item.msg->name : "?", item.signal);
                ImGui::BeginDisabled(item.sig == nullptr); // not in the loaded databases (any more)
                ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                ImGui::EndDisabled();
                if (ImGui::BeginPopupContextItem("##row"))
                {
                    if (ImGui::MenuItem("Remove"))
                    {
                        remove = i;
                    }
                    if (ImGui::MenuItem("Move up", nullptr, false, i > 0))
                    {
                        swap = {i, i - 1};
                    }
                    if (ImGui::MenuItem("Move down", nullptr, false, i + 1 < w.items.size()))
                    {
                        swap = {i, i + 1};
                    }
                    ImGui::EndPopup();
                }
                if (item.sig != nullptr && item.has_value)
                {
                    ImGui::TableNextColumn();
                    if (item.changed) // since the reception before: stands out like changed bytes in the trace
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, theme_text(ThemeText::warn));
                    }
                    ImGui::TextUnformatted(value_text(item).c_str());
                    if (item.changed)
                    {
                        ImGui::PopStyleColor();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(item.sig->unit.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(can_signal_format(*item.sig, item.min).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(can_signal_format(*item.sig, item.max).c_str());
                    ImGui::TableNextColumn();
                    std::string count;
                    append_grouped(count, item.count);
                    ImGui::TextUnformatted(count.c_str());
                }
                else
                {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", item.sig == nullptr ? "not in the databases" : "-");
                }
                ImGui::PopID();
            }
            if (remove != SIZE_MAX)
            {
                w.items.erase(w.items.begin() + static_cast<std::ptrdiff_t>(remove));
            }
            if (swap.first != SIZE_MAX)
            {
                std::swap(w.items[swap.first], w.items[swap.second]);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void watch_save_xml(const WatchWindow& w, pugi::xml_node el)
{
    for (const WatchItem& item : w.items)
    {
        pugi::xml_node n = el.append_child("signal");
        n.append_attribute("network") = item.network.c_str();
        n.append_attribute("id") = item.raw_id;
        n.append_attribute("signal") = item.signal.c_str();
    }
}

void watch_load_xml(WatchWindow& w, pugi::xml_node el)
{
    w.items.clear();
    w.setup_generation = UINT64_MAX;
    for (const pugi::xml_node n : el.children("signal"))
    {
        w.items.push_back({.network = n.attribute("network").as_string(), .raw_id = n.attribute("id").as_uint(), .signal = n.attribute("signal").as_string()});
    }
}
