#include "ui/help_overlay.h"

#include <algorithm>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // BeginPopupEx, ImHashStr, FindRenderedTextEnd

#include "ui/main_menu.h"
#include "ui/vim_nav.h"

namespace
{

// Fixed popup id, so help_overlay_open() works from any window's id stack (the Help menu).
ImGuiID popup_id()
{
    return ImHashStr("help_overlay");
}

bool typed_question_mark()
{
    const ImGuiIO& io = ImGui::GetIO();
    return !io.WantTextInput && std::find(io.InputQueueCharacters.begin(), io.InputQueueCharacters.end(), ImWchar('?')) != io.InputQueueCharacters.end();
}

void row(const char* keys, const char* action)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(keys);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(action, ImGui::FindRenderedTextEnd(action)); // "Graph View##widget"
    vim_yank_table_row({{.label = "Key", .text = keys}, {.label = "Action", .text = std::string(action, ImGui::FindRenderedTextEnd(action))}});
}

bool begin_group(const char* title)
{
    ImGui::SeparatorText(title);
    // NoHostExtendX: inside an auto-resizing popup a table would otherwise be clamped to the host
    // width, which is measured from the table: neither can grow and the text gets clipped.
    if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX))
    {
        return false;
    }
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("Action");
    return true;
}

// Commands [first, last] that have a shortcut; nothing when none has one.
void command_group(const MainMenu& menu, const char* title, Command first, Command last)
{
    bool any = false;
    for (auto c = static_cast<int>(first); c <= static_cast<int>(last) && !any; ++c)
    {
        any = *command_shortcut(menu, static_cast<Command>(c)) != '\0';
    }
    if (!any || !begin_group(title))
    {
        return;
    }
    for (auto c = static_cast<int>(first); c <= static_cast<int>(last); ++c)
    {
        const auto cmd = static_cast<Command>(c);
        const char* keys = command_shortcut(menu, cmd);
        if (*keys != '\0')
        {
            row(keys, command_label(cmd)); // keys is used before the next command_shortcut call
        }
    }
    vim_yank_end_table();
}

} // namespace

void help_overlay_open()
{
    ImGui::OpenPopup(popup_id());
}

bool help_overlay_is_open()
{
    return ImGui::IsPopupOpen(popup_id(), ImGuiPopupFlags_None);
}

void draw_help_overlay(const MainMenu& menu)
{
    const bool open = help_overlay_is_open();
    if (!open)
    {
        if (!typed_question_mark() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        {
            return;
        }
        help_overlay_open();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::BeginPopupEx(popup_id(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove))
    {
        return;
    }
    // The global Esc (Trace Clear) is skipped while a popup is open, so Esc only closes this.
    if (open && (typed_question_mark() || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::TextUnformatted("Keyboard Shortcuts");
    if (ImGui::BeginTable("##help_columns", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoHostExtendX))
    {
        ImGui::TableNextColumn();
        command_group(menu, "Measurement", Command::MeasurementStart, Command::ReloadInterfaces);
        command_group(menu, "Trace", Command::TraceClear, Command::TraceImportFull);
        command_group(menu, "Workspace", Command::WorkspaceNew, Command::Exit);
        command_group(menu, "Windows", Command::NewTraceView, Command::ConditionalLogging);
        command_group(menu, "Graph", Command::FindSignal, Command::FindSignal);
        if (begin_group("Help"))
        {
            row("?", "Show / hide this overlay");
            vim_yank_end_table();
        }
        ImGui::TableNextColumn();
        if (begin_group("Navigation (vim)"))
        {
            for (const VimKey& k : vim_keys)
            {
                row(k.keys, k.action);
            }
            vim_yank_end_table();
        }
        vim_yank_end_table();
    }
    ImGui::EndPopup();
}
