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

// Macros: named step lists (send a frame, wait, run a menu command, run a Python script), each
// started with its own key chord or the Run button. They belong to the workspace file, so every
// project has its own keys. Plain data plus free functions. No <imgui.h> here: app.h includes this.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/bus_message.h"
#include "ui/main_menu.h"

struct App;
struct Iface;
struct Tasks;
namespace pugi
{
class xml_node;
}

enum class MacroStepKind : uint8_t { Send, Wait, Command, Script, Count };

struct MacroStep
{
    MacroStepKind kind = MacroStepKind::Send;
    BusMessage frame{.iface = UINT16_MAX};        // Send: frame.iface is the App::ifaces index
    int wait_ms = 100;                            // Wait
    Command command = Command::MeasurementStart;  // Command: as if picked from the menu
    std::string script;                           // Script: a Python file, run in the Script window
};

struct Macro
{
    std::string name;
    int chord = 0; // ImGuiKeyChord, 0 = none
    std::vector<MacroStep> steps;
    // The run in progress: the worker plays a copy of the steps, so editing meanwhile is safe.
    std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(false);
    std::jthread worker; // last: joined before `running` goes away
};

struct Macros
{
    bool open = false;
    std::vector<Macro> items;
    int selected = -1;
    bool capture = false; // recording the selected macro's chord: global shortcuts pause
};

// Worker thread body: the steps in order. Frames go out through iface_send, commands and scripts
// through tasks (main thread); a stop request ends a wait at once. Clears running.
void macro_run(std::stop_token stop, std::vector<MacroStep> steps, std::deque<Iface>& ifaces, Tasks& tasks,
               std::shared_ptr<std::atomic<bool>> running);

// Starts the macro unless it is still running (false). macro_stop joins it.
bool macro_start(App& app, Macro& m);
void macro_stop(Macro& m);

// Every frame: starts the macros whose chord was pressed and draws the "Macros" window while open.
void macros_frame(App& app, Macros& macros);

// Workspace persistence; a frame's interface is saved as driver + name, a command by its label.
void macros_save_xml(const Macros& macros, const std::deque<Iface>& ifaces, pugi::xml_node el);
void macros_load_xml(Macros& macros, const std::deque<Iface>& ifaces, pugi::xml_node el);
