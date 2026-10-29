#include "ui/recording_dialog.h"

#include <algorithm>
#include <chrono>
#include <format>

#include <imgui.h>
#include <imgui_internal.h> // MarkIniSettingsDirty
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "core/local_time.h"
#include "ui/theme.h"

namespace
{

constexpr const char* popup = "Record Trace";

struct FormatItem
{
    TraceFileFormat format;
    const char* label;
};
constexpr FormatItem formats[] = {
    {TraceFileFormat::VectorAsc, "Vector ASC (*.asc)"},
    {TraceFileFormat::CanDump, "Linux candump (*.candump)"},
    {TraceFileFormat::PcapNg, "PCAPng (*.pcapng)"},
    {TraceFileFormat::Trc, "PEAK PCAN trace (*.trc)"},
};

void draw_status(const Recorder& r)
{
    ImGui::SeparatorText("Status");
    if (r.recording)
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::rec)), "Recording");
        ImGui::TextWrapped("File: %s", r.file_path.c_str());
        ImGui::Text("%llu frames, %.1f MB, part %d", static_cast<unsigned long long>(r.frames_written),
                    static_cast<double>(r.total_bytes) / (1024.0 * 1024.0), r.file_index);
    }
    else
    {
        ImGui::TextUnformatted(r.armed ? "Armed, starts with the next measurement" : "Not armed (Ctrl+R to arm)");
    }
    if (r.dropped > 0)
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::warn)), "%llu frames dropped (queue full)",
                           static_cast<unsigned long long>(r.dropped));
    }
    if (!r.last_error.empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error)), "%s", r.last_error.c_str());
    }
}

} // namespace

void draw_recording_dialog(App& app, RecordingDialogState& s)
{
    if (menu_take(app.menu, Command::RecordingOptions))
    {
        s.edit = app.recorder.config;
        s.split = s.edit.split_size_mb > 0;
        s.split_mb = s.split ? s.edit.split_size_mb : 100;
        ImGui::OpenPopup(popup);
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 34.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true; // the title bar's X = Cancel
    if (!ImGui::BeginPopupModal(popup, &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::SeparatorText("Output");
    ImGui::SetNextItemWidth(em * 22.0f);
    ImGui::InputText("##folder", &s.edit.folder);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        file_dialog_open(s.folder_dialog, FileDialogMode::SelectFolder, "Recording Folder", s.edit.folder);
    }
    for (const auto& folder : file_dialog_draw(s.folder_dialog))
    {
        s.edit.folder = folder;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Folder");
    ImGui::SetNextItemWidth(em * 22.0f);
    ImGui::InputText("File name", &s.edit.file_name_pattern);
    ImGui::SetItemTooltip("Placeholders: {date}, {time} (recording start) and {index} (file number when splitting)");
    const auto* current = std::ranges::find(formats, s.edit.format, &FormatItem::format);
    ImGui::SetNextItemWidth(em * 22.0f);
    if (ImGui::BeginCombo("Format", current != std::end(formats) ? current->label : formats[0].label))
    {
        for (const auto& f : formats)
        {
            if (ImGui::Selectable(f.label, f.format == s.edit.format))
            {
                s.edit.format = f.format;
            }
        }
        ImGui::EndCombo();
    }
    const auto now = to_local_time(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    RecordingConfig example = s.edit; // the split checkbox below only reaches s.edit on OK
    example.split_size_mb = s.split ? s.split_mb : 0;
    ImGui::TextDisabled("Example: %s", recorder_file_name(example, now, 1).c_str());

    ImGui::SeparatorText("Split Files");
    ImGui::Checkbox("Start a new file every", &s.split);
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.split);
    ImGui::SetNextItemWidth(em * 8.0f);
    ImGui::InputInt("MB", &s.split_mb);
    s.split_mb = std::clamp(s.split_mb, 1, 100000);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Start");
    ImGui::Checkbox("Record every measurement (stay armed after stop)", &s.edit.stay_armed);
    ImGui::PushTextWrapPos(em * 34.0f);
    ImGui::TextDisabled("Recording starts with the next measurement, or immediately if one is running. "
                        "Changed options apply from the next recording.");
    ImGui::PopTextWrapPos();

    draw_status(app.recorder);

    ImGui::Separator();
    ImGui::BeginDisabled(s.edit.folder.empty());
    if (ImGui::Button("OK", ImVec2(em * 6.0f, 0.0f)))
    {
        s.edit.split_size_mb = s.split ? s.split_mb : 0;
        app.recorder.config = s.edit;
        ImGui::MarkIniSettingsDirty(); // recording/* is saved in the ini (settings.cpp)
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 6.0f, 0.0f)) || ImGui::Shortcut(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
