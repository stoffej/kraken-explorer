#include "ui/script_window.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/python_engine.h"
#include "ui/theme.h"
#include "ui/workspace_tabs.h"

namespace
{

// The bundled example scripts: examples/ of a source-tree build (build/src/../..), examples/
// next to the executable (the Windows zip, and a Windows build tree: the sources are elsewhere),
// /usr/share/kraken-explorer/examples when installed; empty (last used dir) when none exists.
std::string examples_dir()
{
    std::error_code ec;
    const std::filesystem::path exe = platform_exe_path();
    for (const std::filesystem::path dir : {exe.parent_path() / "../../examples", exe.parent_path() / "examples", std::filesystem::path("/usr/share/kraken-explorer/examples")})
    {
        if (std::filesystem::is_directory(dir, ec))
        {
            return std::filesystem::weakly_canonical(dir, ec).string();
        }
    }
    return {};
}

void reload_if_modified(ScriptWindowState& s)
{
    if (s.file_path.empty())
    {
        return;
    }
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(s.file_path, ec);
    if (!ec && modified > s.loaded_time)
    {
        script_window_load_file(s, s.file_path);
    }
}

std::vector<FileFilter> script_filters()
{
    return {{"Python Files (*.py)", "*.py"}, {"All Files", "*"}};
}

void save_as(ScriptWindowState& s, const std::string& path)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        log_error(std::format("Cannot write {}", path));
        return;
    }
    out << s.code;
    s.file_path = path;
    std::error_code ec;
    s.loaded_time = std::filesystem::last_write_time(path, ec);
}

void draw_console(App& app, ScriptWindowState& s, ImVec2 size)
{
    PyState& py = app.python;
    ImGui::BeginChild("##console", size, ImGuiChildFlags_Borders);
    ImGui::PushFont(app.fonts.mono, 0.0f);
    ImGui::PushTextWrapPos(0.0f); // long tracebacks wrap at the console's edge
    {
        const std::lock_guard lock(py.mutex); // the script's print() waits one draw
        for (const PyConsoleRun& run : py.console)
        {
            if (run.error)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme_text(ThemeText::error));
            }
            ImGui::TextUnformatted(run.text.data(), run.text.data() + run.text.size());
            if (run.error)
            {
                ImGui::PopStyleColor();
            }
        }
        if (py.console_total != s.console_seen)
        {
            s.console_seen = py.console_total;
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
}

} // namespace

bool script_window_load_file(ScriptWindowState& s, const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        log_error(std::format("Cannot read script {}", path));
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    s.code = text.str();
    s.file_path = path;
    std::error_code ec;
    s.loaded_time = std::filesystem::last_write_time(path, ec);
    return true;
}

void script_window_run(App& app, ScriptWindowState& s)
{
    reload_if_modified(s);
    python_run(app, app.python, s.code, s.file_path.empty() ? "<script>" : std::filesystem::path(s.file_path).filename().string());
}

void draw_script_window(App& app, ScriptWindowState& s, const WorkspaceTab& tab)
{
    PyState& py = app.python;
    python_poll(py);
    if (app.measuring != s.was_measuring)
    {
        s.was_measuring = app.measuring;
        if (app.measuring && s.autorun && !py.running)
        {
            script_window_run(app, s);
        }
        else if (!app.measuring && py.running)
        {
            python_stop(py);
        }
    }

    if (!ImGui::Begin(workspace_window_name(tab, "Python Script").c_str()))
    {
        ImGui::End();
        return;
    }
    const bool running = py.running;
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Run"))
    {
        script_window_run(app, s);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running || py.stop_requested);
    if (ImGui::Button("Stop"))
    {
        python_stop(py);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("AutoRun", &s.autorun);
    ImGui::SetItemTooltip("Start script with measurement");
    // Load / Save / Clear right-aligned, as the old toolbar stretch.
    const float right = ImGui::CalcTextSize("Load").x + ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("Clear").x
                        + 6.0f * ImGui::GetStyle().FramePadding.x + 2.0f * ImGui::GetStyle().ItemSpacing.x;
    // The cursor is already on the next line here, so measure AutoRun's end; too narrow (large text) wraps.
    const float after = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + ImGui::GetStyle().ItemSpacing.x;
    const float target = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - right;
    if (target >= after)
    {
        ImGui::SameLine(target);
    }
    if (ImGui::Button("Load"))
    {
        file_dialog_open(s.file_dialog, FileDialogMode::Open, "Load Python Script",
                         s.file_path.empty() ? examples_dir() : s.file_path, script_filters());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        file_dialog_open(s.file_dialog, FileDialogMode::Save, "Save Python Script", s.file_path, script_filters());
    }
    for (const auto& path : file_dialog_draw(s.file_dialog))
    {
        if (s.file_dialog.mode == FileDialogMode::Save)
        {
            save_as(s, path);
        }
        else
        {
            script_window_load_file(s, path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        python_console_clear(py);
    }
    ImGui::TextDisabled("%s", s.file_path.empty() ? "No script loaded" : s.file_path.c_str());

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float body = avail.y - ImGui::GetFrameHeightWithSpacing();
    ImGui::PushFont(app.fonts.mono, 0.0f);
    ImGui::InputTextMultiline("##editor", &s.code, ImVec2(avail.x * 0.6f, body),
                              running ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    ImGui::PopFont();
    ImGui::SameLine();
    draw_console(app, s, ImVec2(0.0f, body));

    ImGui::BeginDisabled(!running);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushFont(app.fonts.mono, 0.0f);
    if (ImGui::InputTextWithHint("##input", "Script input (press Enter to send)...", &s.input,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
    {
        python_console_append(py, "> " + s.input + "\n", false);
        python_input(py, s.input);
        s.input.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopFont();
    ImGui::EndDisabled();
    ImGui::End();
}
