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

#include "ui/macros.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <pugixml.hpp>

#include "app.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/tasks.h"
#include "drivers/driver.h"
#include "ui/raw_tx.h"
#include "ui/script_window.h"
#include "ui/theme.h"

namespace
{

constexpr std::array<const char*, static_cast<std::size_t>(MacroStepKind::Count)> kind_names = {"send", "wait", "command", "script"};
constexpr std::array<const char*, static_cast<std::size_t>(MacroStepKind::Count)> kind_labels = {"Send", "Wait", "Command", "Script"};

std::string_view plain_label(Command cmd)
{
    const std::string_view label = command_label(cmd);
    return label.substr(0, label.find("##"));
}

std::string frame_text(const App& app, const BusMessage& m)
{
    std::string out = std::format("0x{:X}  [{}]  ", m.id, m.len);
    append_hex_bytes(out, std::span(m.data).first(m.len), " ");
    out += m.iface < app.ifaces.size() ? std::format("  on {}", app.ifaces[m.iface].info.name) : std::string("  (no interface)");
    return out;
}

void draw_step(App& app, MacroStep& step)
{
    switch (step.kind)
    {
    case MacroStepKind::Send:
        if (ImGui::Button(frame_text(app, step.frame).c_str(), ImVec2(-FLT_MIN, 0.0f)))
        {
            ImGui::OpenPopup("##frame");
        }
        if (ImGui::BeginPopup("##frame"))
        {
            draw_raw_tx(app, step.frame, nullptr);
            ImGui::EndPopup();
        }
        break;
    case MacroStepKind::Wait:
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::InputInt("ms", &step.wait_ms, 10, 100);
        step.wait_ms = std::clamp(step.wait_ms, 0, 3'600'000);
        break;
    case MacroStepKind::Command:
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##command", std::string(plain_label(step.command)).c_str()))
        {
            for (int c = 0; c < static_cast<int>(Command::Count); ++c)
            {
                const auto cmd = static_cast<Command>(c);
                if (cmd != Command::WorkspaceOpenRecent // needs a path
                    && ImGui::Selectable(std::format("{}##{}", plain_label(cmd), c).c_str(), cmd == step.command))
                {
                    step.command = cmd;
                }
            }
            ImGui::EndCombo();
        }
        break;
    case MacroStepKind::Script:
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##script", "path of a Python script", &step.script);
        ImGui::SetItemTooltip("Loaded into the Python Script window and run there (one script at a time)");
        break;
    case MacroStepKind::Count:
        break;
    }
}

void draw_macro(App& app, Macros& macros, Macro& m)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##name", "Name", &m.name);
    ImGui::SameLine();
    if (ImGui::Button(macros.capture ? "Press keys..." : (m.chord != 0 ? chord_name(m.chord).c_str() : "No key"), ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f)))
    {
        macros.capture = !macros.capture;
    }
    ImGui::SetItemTooltip("The key that runs this macro, e.g. F7 or Ctrl+Shift+1.\nClick, then press it; Backspace = none, Esc = keep.");
    if (macros.capture)
    {
        if (const int chord = chord_capture(); chord != chord_capture_waiting)
        {
            m.chord = chord == chord_capture_cancelled ? m.chord : chord;
            macros.capture = false;
        }
    }
    ImGui::SameLine();
    const bool running = *m.running;
    if (ImGui::Button(running ? "Stop" : "Run"))
    {
        running ? macro_stop(m) : static_cast<void>(macro_start(app, m));
    }
    if (m.chord != 0)
    {
        for (int c = 0; c < static_cast<int>(Command::Count); ++c)
        {
            if (command_chord(app.menu, static_cast<Command>(c)) == m.chord)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::warn)), "also runs \"%s\"",
                                   std::string(plain_label(static_cast<Command>(c))).c_str());
            }
        }
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    const float footer = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("##steps", 4, flags, ImVec2(0.0f, -footer)))
    {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, em * 1.5f);
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
        ImGui::TableHeadersRow();
        std::size_t remove = SIZE_MAX;
        std::size_t up = SIZE_MAX;
        for (std::size_t i = 0; i < m.steps.size(); ++i)
        {
            MacroStep& step = m.steps[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            int kind = static_cast<int>(step.kind);
            if (ImGui::Combo("##kind", &kind, kind_labels.data(), static_cast<int>(kind_labels.size())))
            {
                step.kind = static_cast<MacroStepKind>(kind);
            }
            ImGui::TableNextColumn();
            draw_step(app, step);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up))
            {
                up = i;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == m.steps.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down))
            {
                up = i + 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("x"))
            {
                remove = i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (up != SIZE_MAX)
        {
            std::swap(m.steps[up], m.steps[up - 1]);
        }
        if (remove != SIZE_MAX)
        {
            m.steps.erase(m.steps.begin() + static_cast<std::ptrdiff_t>(remove));
        }
    }
    for (int k = 0; k < static_cast<int>(MacroStepKind::Count); ++k)
    {
        if (k > 0)
        {
            ImGui::SameLine();
        }
        if (ImGui::Button(std::format("+ {}", kind_labels[static_cast<std::size_t>(k)]).c_str()))
        {
            MacroStep step{.kind = static_cast<MacroStepKind>(k)};
            // A new frame goes where the one before it went.
            const auto prev = std::ranges::find(m.steps.rbegin(), m.steps.rend(), MacroStepKind::Send, &MacroStep::kind);
            step.frame.iface = prev != m.steps.rend() ? prev->frame.iface : (app.ifaces.empty() ? UINT16_MAX : uint16_t{0});
            m.steps.push_back(std::move(step));
        }
    }
}

} // namespace

void macro_run(std::stop_token stop, std::vector<MacroStep> steps, std::deque<Iface>& ifaces, Tasks& tasks,
               std::shared_ptr<std::atomic<bool>> running)
{
    std::mutex mutex;
    std::condition_variable_any cv; // only to sleep until the wait is over or the stop request
    for (const MacroStep& step : steps)
    {
        if (stop.stop_requested())
        {
            break;
        }
        switch (step.kind)
        {
        case MacroStepKind::Send:
            if (step.frame.iface >= ifaces.size() || !iface_send(ifaces[step.frame.iface], step.frame))
            {
                log_warning(std::format("Macro: frame 0x{:X} not sent (no open interface)", step.frame.id));
            }
            break;
        case MacroStepKind::Wait:
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, stop, std::chrono::milliseconds(step.wait_ms), [] { return false; });
            break;
        }
        case MacroStepKind::Command:
            tasks_post(tasks, [cmd = step.command](App& app) { menu_run(app, cmd); });
            break;
        case MacroStepKind::Script:
            tasks_post(tasks, [path = step.script](App& app)
            {
                if (script_window_load_file(app.script, path))
                {
                    script_window_run(app, app.script);
                }
            });
            break;
        case MacroStepKind::Count:
            break;
        }
    }
    *running = false;
    if (tasks.wake != nullptr)
    {
        tasks.wake(); // the Run button comes back
    }
}

bool macro_start(App& app, Macro& m)
{
    if (*m.running)
    {
        return false;
    }
    *m.running = true;
    platform_fine_timers();
    m.worker = std::jthread(macro_run, m.steps, std::ref(app.ifaces), std::ref(app.tasks), m.running);
    return true;
}

void macro_stop(Macro& m)
{
    m.worker = {}; // request_stop + join
}

void macros_frame(App& app, Macros& macros)
{
    if (menu_take(app.menu, Command::Macros))
    {
        macros.open = true;
    }
    macros.capture = macros.capture && macros.open;
    if (!macros.capture && !app.menu.capturing_shortcut)
    {
        for (Macro& m : macros.items)
        {
            if (m.chord != 0 && ImGui::Shortcut(m.chord, ImGuiInputFlags_RouteGlobal))
            {
                macro_start(app, m);
            }
        }
    }
    if (!macros.open)
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(760.0f * px, 420.0f * px), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Macros", &macros.open))
    {
        const float em = ImGui::GetFontSize();
        ImGui::BeginChild("##list", ImVec2(em * 13.0f, 0.0f), ImGuiChildFlags_Borders);
        if (ImGui::Button("New"))
        {
            macros.items.push_back({.name = std::format("Macro {}", macros.items.size() + 1)});
            macros.selected = static_cast<int>(macros.items.size()) - 1;
            macros.capture = false;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(macros.selected < 0 || macros.selected >= static_cast<int>(macros.items.size()));
        if (ImGui::Button("Delete"))
        {
            macros.items.erase(macros.items.begin() + macros.selected); // joins a run in progress
            macros.selected = std::min(macros.selected, static_cast<int>(macros.items.size()) - 1);
            macros.capture = false;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(macros.items.size()); ++i)
        {
            const Macro& m = macros.items[static_cast<std::size_t>(i)];
            const std::string label = std::format("{}{}{}##{}", *m.running ? "> " : "", m.name,
                                                  m.chord != 0 ? std::format("  [{}]", chord_name(m.chord)) : std::string(), i);
            if (ImGui::Selectable(label.c_str(), i == macros.selected))
            {
                macros.selected = i;
                macros.capture = false;
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##macro");
        if (macros.selected >= 0 && macros.selected < static_cast<int>(macros.items.size()))
        {
            draw_macro(app, macros, macros.items[static_cast<std::size_t>(macros.selected)]);
        }
        else
        {
            ImGui::TextDisabled("A macro is a list of steps run by a key:\nsend a frame, wait, a menu command, a Python script.\nSaved with the workspace.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void macros_save_xml(const Macros& macros, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    for (const Macro& m : macros.items)
    {
        pugi::xml_node mn = el.append_child("macro");
        mn.append_attribute("name") = m.name.c_str();
        mn.append_attribute("key") = chord_name(m.chord).c_str();
        for (const MacroStep& step : m.steps)
        {
            pugi::xml_node s = mn.append_child("step");
            s.append_attribute("kind") = kind_names[static_cast<std::size_t>(step.kind)];
            switch (step.kind)
            {
            case MacroStepKind::Send:
            {
                const BusMessage& f = step.frame;
                std::string data;
                append_hex_bytes(data, std::span(f.data).first(f.len), " ");
                s.append_attribute("id") = std::format("0x{:X}", f.id).c_str();
                s.append_attribute("dlc") = f.len;
                s.append_attribute("extended") = has_flag(f, bus_flag::extended) ? 1 : 0;
                s.append_attribute("rtr") = has_flag(f, bus_flag::rtr) ? 1 : 0;
                s.append_attribute("fd") = has_flag(f, bus_flag::fd) ? 1 : 0;
                s.append_attribute("brs") = has_flag(f, bus_flag::brs) ? 1 : 0;
                s.append_attribute("interface") = f.iface < ifaces.size() ? ifaces[f.iface].info.name.c_str() : "";
                s.append_attribute("driver") = f.iface < ifaces.size() && ifaces[f.iface].ops != nullptr ? ifaces[f.iface].ops->name : "";
                s.append_attribute("data") = data.c_str();
                break;
            }
            case MacroStepKind::Wait:
                s.append_attribute("ms") = step.wait_ms;
                break;
            case MacroStepKind::Command:
                s.append_attribute("command") = std::string(plain_label(step.command)).c_str();
                break;
            case MacroStepKind::Script:
                s.append_attribute("path") = step.script.c_str();
                break;
            case MacroStepKind::Count:
                break;
            }
        }
    }
}

void macros_load_xml(Macros& macros, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    macros.items.clear(); // joins runs in progress
    macros.selected = -1;
    macros.capture = false;
    for (const pugi::xml_node mn : el.children("macro"))
    {
        Macro m{.name = mn.attribute("name").as_string(), .chord = std::max(0, chord_parse(mn.attribute("key").as_string()))};
        for (const pugi::xml_node s : mn.children("step"))
        {
            const auto k = std::ranges::find(kind_names, std::string_view(s.attribute("kind").as_string()));
            if (k == kind_names.end())
            {
                continue; // a step kind of a newer version
            }
            MacroStep step{.kind = static_cast<MacroStepKind>(k - kind_names.begin())};
            BusMessage& f = step.frame;
            f.id = static_cast<uint32_t>(std::strtoul(s.attribute("id").as_string(), nullptr, 16)) & can_id_mask_extended;
            f.flags = static_cast<uint16_t>((s.attribute("extended").as_int() != 0 ? bus_flag::extended : 0)
                                            | (s.attribute("rtr").as_int() != 0 ? bus_flag::rtr : 0)
                                            | (s.attribute("fd").as_int() != 0 ? bus_flag::fd : 0)
                                            | (s.attribute("brs").as_int() != 0 ? bus_flag::brs : 0));
            set_length(f, std::clamp(s.attribute("dlc").as_int(), 0, bus_max_data_bytes));
            std::string_view data = s.attribute("data").as_string();
            for (std::size_t i = 0; i < f.len && !data.empty(); ++i)
            {
                const auto sp = data.find(' ');
                f.data[i] = static_cast<uint8_t>(std::strtoul(std::string(data.substr(0, sp)).c_str(), nullptr, 16));
                data = sp == std::string_view::npos ? std::string_view{} : data.substr(sp + 1);
            }
            const int index = ifaces_find(ifaces, s.attribute("driver").as_string(), s.attribute("interface").as_string());
            f.iface = static_cast<uint16_t>(index >= 0 ? index : UINT16_MAX);
            step.wait_ms = std::clamp(s.attribute("ms").as_int(100), 0, 3'600'000);
            const Command cmd = command_by_label(s.attribute("command").as_string());
            step.command = cmd != Command::Count ? cmd : Command::MeasurementStart;
            step.script = s.attribute("path").as_string();
            m.steps.push_back(std::move(step));
        }
        macros.items.push_back(std::move(m));
    }
}
