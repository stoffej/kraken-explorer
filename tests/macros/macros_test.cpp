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

// ui/macros: the steps in order on the worker (frames through iface_send, commands through
// tasks, waits that a stop ends at once), no second run while one is in progress, and the
// workspace XML round trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <pugixml.hpp>

#include "app.h"

#include "drivers/driver.h"
#include "ui/macros.h"
#include "ui/theme.h"

namespace
{

std::vector<BusMessage> sent;

int fake_read(Iface&, BusMessage*, int, int timeout_ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
    return 0;
}

const DriverOps fake_can = {
    .name = "FakeCan",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return true; },
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage& m)
    {
        sent.push_back(m);
        return true;
    },
    .read = fake_read,
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

MacroStep send(uint32_t id, uint8_t byte)
{
    MacroStep s{.kind = MacroStepKind::Send};
    s.frame.id = id;
    s.frame.iface = 0;
    set_length(s.frame, 1);
    s.frame.data[0] = byte;
    return s;
}

bool wait_done(const Macro& m)
{
    for (int t = 0; t < 1000 && *m.running; ++t)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !*m.running;
}

} // namespace

TEST_CASE("a macro sends its frames in order with the wait between them and posts its command")
{
    App app;
    Iface& can = app.ifaces.emplace_back();
    can.ops = &fake_can;
    can.info.name = "can0";
    app.setup.networks.push_back({.name = "Net"});
    app.setup.networks[0].interfaces.push_back({.driver = "FakeCan", .name = "can0"});
    REQUIRE(ifaces_start(app.ifaces, app.setup, {}, nullptr) == 1);
    sent.clear();

    Macro m{.name = "wake"};
    m.steps = {send(0x100, 1), {.kind = MacroStepKind::Wait, .wait_ms = 60}, send(0x200, 2),
               {.kind = MacroStepKind::Command, .command = Command::Record}};
    const auto t = std::chrono::steady_clock::now();
    REQUIRE(macro_start(app, m));
    CHECK_FALSE(macro_start(app, m)); // still running: the key does nothing
    REQUIRE(wait_done(m));
    CHECK(std::chrono::steady_clock::now() - t >= std::chrono::milliseconds(60));
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].id == 0x100);
    CHECK(sent[1].data[0] == 2);
    CHECK_FALSE(app.menu.record_armed);
    tasks_drain(app.tasks, app); // the command runs on the main thread, as from the menu
    CHECK(app.menu.record_armed);
    CHECK(macro_start(app, m)); // finished: runs again
    REQUIRE(wait_done(m));
    CHECK(sent.size() == 4);
    macro_stop(m);
    ifaces_stop(app.ifaces);
}

TEST_CASE("stop ends a long wait at once; a frame without an open interface is skipped")
{
    App app;
    sent.clear();
    Macro m;
    m.steps = {send(0x100, 1), {.kind = MacroStepKind::Wait, .wait_ms = 60000}, send(0x200, 2)};
    REQUIRE(macro_start(app, m));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto t = std::chrono::steady_clock::now();
    macro_stop(m);
    CHECK(std::chrono::steady_clock::now() - t < std::chrono::milliseconds(500));
    CHECK_FALSE(*m.running);
    CHECK(sent.empty());
}

TEST_CASE("workspace XML round trip keeps names, keys and every step kind")
{
    ImGui::CreateContext(); // chord names
    {
        App app;
        Iface& can = app.ifaces.emplace_back();
        can.ops = &fake_can;
        can.info.name = "can0";
        Macros macros;
        Macro& m = macros.items.emplace_back();
        m.name = "Wake up";
        m.chord = ImGuiMod_Ctrl | ImGuiKey_F7;
        MacroStep frame = send(0x18FF0001, 0xAB);
        frame.frame.flags = bus_flag::extended | bus_flag::fd;
        set_length(frame.frame, 12);
        frame.frame.data[11] = 0xCD;
        m.steps = {frame, {.kind = MacroStepKind::Wait, .wait_ms = 250}, {.kind = MacroStepKind::Command, .command = Command::MeasurementStop},
                   {.kind = MacroStepKind::Script, .script = "C:/scripts/reset.py"}};
        macros.items.emplace_back().name = "Empty";

        pugi::xml_document doc;
        pugi::xml_node el = doc.append_child("macros");
        macros_save_xml(macros, app.ifaces, el);
        Macros q;
        macros_load_xml(q, app.ifaces, el);
        REQUIRE(q.items.size() == 2);
        CHECK(q.items[0].name == "Wake up");
        CHECK(q.items[0].chord == (ImGuiMod_Ctrl | ImGuiKey_F7));
        REQUIRE(q.items[0].steps.size() == 4);
        const BusMessage& f = q.items[0].steps[0].frame;
        CHECK(f.id == 0x18FF0001);
        CHECK(f.flags == (bus_flag::extended | bus_flag::fd));
        CHECK(f.len == 12);
        CHECK(f.data[0] == 0xAB);
        CHECK(f.data[11] == 0xCD);
        CHECK(f.iface == 0);
        CHECK(q.items[0].steps[1].wait_ms == 250);
        CHECK(q.items[0].steps[2].command == Command::MeasurementStop);
        CHECK(q.items[0].steps[3].script == "C:/scripts/reset.py");
        CHECK(q.items[1].chord == 0);
        CHECK(q.items[1].steps.empty());
    }
    ImGui::DestroyContext();
}

TEST_CASE("the Macros window draws headless and the macro's key starts it")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1280, 800};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);
    {
        App app;
        app.macros.open = true;
        Macro& m = app.macros.items.emplace_back();
        m.name = "Record";
        m.chord = ImGuiKey_F7;
        m.steps = {{.kind = MacroStepKind::Command, .command = Command::Record}, send(0x1, 0), {.kind = MacroStepKind::Wait},
                   {.kind = MacroStepKind::Script}};
        m.steps[2].wait_ms = 0;
        m.steps[3].kind = MacroStepKind::Wait;
        m.steps[3].wait_ms = 0;
        app.macros.selected = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (i == 2)
            {
                io.AddKeyEvent(ImGuiKey_F7, true);
            }
            ImGui::NewFrame();
            macros_frame(app, app.macros);
            ImGui::EndFrame();
        }
        io.AddKeyEvent(ImGuiKey_F7, false);
        REQUIRE(wait_done(m));
        tasks_drain(app.tasks, app);
        CHECK(app.menu.record_armed);
        CHECK(ImGui::FindWindowByName("Macros") != nullptr);
    }
    ImGui::DestroyContext();
}
