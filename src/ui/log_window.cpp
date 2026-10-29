#include "ui/log_window.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <span>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "core/fuzzy.h"
#include "core/local_time.h"
#include "core/log.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/vim_nav.h"
#include "ui/workspace_tabs.h"

namespace
{

std::string time_text(std::chrono::system_clock::time_point t)
{
    return std::format("{:%H:%M:%S}", to_local_time(std::chrono::floor<std::chrono::seconds>(t)));
}

LogFilterLevel filter_level(LogLevel level)
{
    return static_cast<LogFilterLevel>(std::min(level, LogLevel::Error)); // same order, Error and above -> Error
}

// Warnings in brass (the Kraken button rim), errors in coral (the Stop button outline).
ImVec4 level_color(LogFilterLevel level)
{
    switch (level)
    {
    case LogFilterLevel::Debug:
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    case LogFilterLevel::Warning:
        return ImGui::ColorConvertU32ToFloat4(theme_u32(theme_kraken_button().border));
    case LogFilterLevel::Error:
        return ImGui::ColorConvertU32ToFloat4(theme_u32(theme_stop_button().border));
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

bool line_passes(const LogWindowState& s, LogFilterLevel level, const LogEntry& e)
{
    return s.show[static_cast<size_t>(level)] && smart_find(e.text, s.query) != std::string_view::npos;
}

// text with the bytes at pos (ascending) in hi. ponytail: splits at byte positions, so a
// non-ASCII query can cut a UTF-8 sequence; snap segment ends to code points if that matters.
void text_highlighted(std::string_view text, std::span<const int> pos, const ImVec4& hi)
{
    if (pos.empty())
    {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        return;
    }
    size_t k = 0;
    for (size_t i = 0; i < text.size();)
    {
        const bool matched = k < pos.size() && static_cast<size_t>(pos[k]) == i;
        size_t j = i;
        while (j < text.size() && (k < pos.size() && static_cast<size_t>(pos[k]) == j) == matched)
        {
            k += matched ? 1 : 0;
            ++j;
        }
        if (i != 0)
        {
            ImGui::SameLine(0.0f, 0.0f);
        }
        if (matched)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, hi);
        }
        ImGui::TextUnformatted(text.data() + i, text.data() + j);
        if (matched)
        {
            ImGui::PopStyleColor();
        }
        i = j;
    }
}

void export_log(const std::string& path)
{
    std::ofstream out(path);
    if (!out)
    {
        log_error(std::format("Cannot write {}", path));
        return;
    }
    out << "Time\tLevel\tText\n------------------------------------------------------------\n";
    LogState& log = log_state();
    const std::lock_guard lock(log.mutex);
    for (size_t i = 0; i < log.entries.size(); ++i)
    {
        const LogEntry& e = log_entry_at(log, i);
        out << time_text(e.time) << '\t' << log_level_name(e.level) << '\t' << e.text << '\n';
    }
}

} // namespace

void log_window_update(LogWindowState& s, const LogState& log)
{
    constexpr size_t cap = LogState::capacity;
    s.level_ring.resize(cap);
    const uint64_t base = log.total - log.entries.size();
    // Lines that left the ring (wrapped out or cleared) leave the counts and rows.
    if (base >= s.done)
    {
        s.counts = {};
    }
    else
    {
        for (uint64_t a = s.base; a < base; ++a)
        {
            --s.counts[s.level_ring[a % cap]];
        }
    }
    while (!s.rows.empty() && s.rows.front() < base)
    {
        s.rows.pop_front();
    }
    s.base = base;
    if (s.query != s.applied_query || s.show != s.applied_show)
    {
        s.rows.clear();
        for (uint64_t a = base; a < s.done; ++a)
        {
            const LogEntry& e = log_entry_at(log, a - base);
            if (line_passes(s, filter_level(e.level), e))
            {
                s.rows.push_back(a);
            }
        }
        s.applied_query = s.query;
        s.applied_show = s.show;
    }
    for (uint64_t a = std::max(s.done, base); a < log.total; ++a)
    {
        const LogEntry& e = log_entry_at(log, a - base);
        const LogFilterLevel level = filter_level(e.level);
        s.level_ring[a % cap] = static_cast<uint8_t>(level);
        ++s.counts[static_cast<size_t>(level)];
        if (line_passes(s, level, e))
        {
            s.rows.push_back(a);
        }
    }
    s.done = log.total;
}

void draw_log_window(LogWindowState& s, const WorkspaceTab& tab)
{
    if (!ImGui::Begin(workspace_window_name(tab, "Log").c_str()))
    {
        ImGui::End();
        return;
    }
    LogState& log = log_state();
    {
        const std::lock_guard lock(log.mutex);
        log_window_update(s, log);
    }
    if (icon_text_button("Clear", Icon::EditClear))
    {
        log_clear();
    }
    ImGui::SetItemTooltip("Clear the log");
    same_line_or_wrap(icon_text_button_width("Export..."));
    const bool export_clicked = icon_text_button("Export...", Icon::DocumentSaveAs);
    ImGui::SetItemTooltip("Save the log to a file");
    if (export_clicked)
    {
        file_dialog_open(s.export_dialog, FileDialogMode::Save, "Export Logs", "",
                         {{"Log Files (*.log)", "*.log"}, {"Text Files (*.txt)", "*.txt"}, {"All Files", "*"}});
    }
    for (const auto& path : file_dialog_draw(s.export_dialog))
    {
        export_log(path);
    }

    static constexpr std::array<const char*, log_filter_levels> level_names{"Debug", "Info", "Warn", "Error"};
    for (size_t i = 0; i < log_filter_levels; ++i)
    {
        const std::string label = std::format("{} {}##lvl{}", level_names[i], s.counts[i], i);
        ImGui::SameLine();
        const bool on = s.show[i];
        ImGui::PushStyleColor(ImGuiCol_Text, on ? level_color(static_cast<LogFilterLevel>(i))
                                                : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(on ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBg));
        if (ImGui::Button(label.c_str()))
        {
            s.show[i] = !on;
        }
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s %s lines", on ? "Hide" : "Show", level_names[i]);
    }
    ImGui::SameLine();
    if (s.focus_search)
    {
        ImGui::SetKeyboardFocusHere();
        s.focus_search = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search (/)", &s.query);

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##log", 3, flags))
    {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        const ImVec4 highlight = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        const std::lock_guard lock(log.mutex); // log calls from other threads wait one draw
        log_window_update(s, log);             // Clear may have run since the update above
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(s.rows.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const LogEntry& e = log_entry_at(log, s.rows[static_cast<size_t>(row)] - s.base);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(time_text(e.time).c_str());
                ImGui::TableNextColumn();
                const std::string_view level = log_level_name(e.level);
                ImGui::TextColored(level_color(filter_level(e.level)), "%.*s", static_cast<int>(level.size()), level.data());
                ImGui::TableNextColumn();
                if (!s.query.empty())
                {
                    s.positions.clear();
                    const std::size_t at = smart_find(e.text, s.query);
                    for (std::size_t k = 0; at != std::string_view::npos && k < s.query.size(); ++k)
                    {
                        s.positions.push_back(static_cast<int>(at + k));
                    }
                }
                text_highlighted(e.text, s.query.empty() ? std::span<const int>{} : s.positions, highlight);
            }
        }
        const float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
        int line = static_cast<int>(ImGui::GetScrollY() / row_h); // top visible line
        int h = 0;
        if (vim_nav(s.vim, line, static_cast<int>(s.rows.size()), static_cast<int>(ImGui::GetWindowHeight() / row_h),
                    s.focus_search, h))
        {
            ImGui::SetScrollY(static_cast<float>(line) * row_h);
        }
        if (h != 0)
        {
            ImGui::SetScrollX(ImGui::GetScrollX() + static_cast<float>(h) * em * 4.0f);
        }
        if (log.total != s.seen_total)
        {
            s.seen_total = log.total;
            if (at_bottom)
            {
                ImGui::SetScrollHereY(1.0f); // follow new lines unless scrolled up
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
