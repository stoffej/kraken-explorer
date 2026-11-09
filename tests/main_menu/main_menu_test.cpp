#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, HoveredIdPreviousFrame

#include "app.h"
#include "ui/main_menu.h"
#include "ui_test.h"

TEST_CASE("open recent keeps 8 unique paths, newest first")
{
    MainMenu menu;
    for (int i = 0; i < 10; ++i)
    {
        menu_add_recent(menu, "/w/" + std::to_string(i) + ".kraken");
    }
    menu_add_recent(menu, "/w/5.kraken");
    REQUIRE(menu.recent_files.size() == max_recent_files);
    CHECK(menu.recent_files[0] == "/w/5.kraken");
    CHECK(menu.recent_files[1] == "/w/9.kraken");
    CHECK(menu.recent_files.back() == "/w/2.kraken"); // 0 and 1 fell off
}

static void frame(App& app)
{
    ImGui::NewFrame();
    draw_main_menu(app);
    ImGui::EndFrame();
}

// Presses chord for one frame (routes are resolved from the previous frame, so warm up first).
static void press(App& app, ImGuiKeyChord chord)
{
    ImGuiIO& io = ImGui::GetIO();
    const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    for (bool down : {true, false})
    {
        for (ImGuiKey mod : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt})
        {
            if (chord & mod)
            {
                io.AddKeyEvent(mod, down);
            }
        }
        io.AddKeyEvent(key, down);
        frame(app);
    }
}

TEST_CASE("shortcuts of mainwindow.cpp trigger their commands")
{
    const UiTest ui;
    {
        App app;
        frame(app);

        press(app, ImGuiKey_F5);
        CHECK(menu_take(app.menu, Command::MeasurementStart));
        CHECK_FALSE(menu_take(app.menu, Command::MeasurementStart)); // taken once

        app.measuring = true; // Start and Setup are disabled while measuring
        press(app, ImGuiKey_F5);
        press(app, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S);
        CHECK_FALSE(menu_take(app.menu, Command::MeasurementStart));
        CHECK_FALSE(menu_take(app.menu, Command::Setup));
        press(app, ImGuiMod_Shift | ImGuiKey_F5);
        CHECK(menu_take(app.menu, Command::MeasurementStop));

        press(app, ImGuiMod_Ctrl | ImGuiKey_R);
        CHECK(app.menu.record_armed);
        press(app, ImGuiMod_Ctrl | ImGuiKey_R);
        CHECK_FALSE(app.menu.record_armed);

        press(app, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);
        CHECK(menu_take(app.menu, Command::WorkspaceSaveAs));
        CHECK_FALSE(menu_take(app.menu, Command::WorkspaceSave));
        press(app, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B);
        CHECK(menu_take(app.menu, Command::StandaloneGraph));

        const BusMessage m{.id = 0x123};
        trace_append(app.trace, {&m, 1});
        press(app, ImGuiKey_Escape);
        CHECK(trace_size(app.trace) == 0);
        CHECK(menu_take(app.menu, Command::TraceClear));

        press(app, ImGuiMod_Alt | ImGuiKey_F4);
        CHECK(app.quit);
    }
}

// Moves the mouse along the control bar until the item with id is hovered, then clicks it.
// Disabled items are hovered too (ImGui sets HoveredId for their tooltips) but ignore the click.
static bool click_control_bar(App& app, const char* id, const char* inner = nullptr)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* bar = ImGui::FindWindowByName("##control_bar");
    REQUIRE(bar != nullptr);
    const ImGuiID target = inner == nullptr ? bar->GetID(id) : ImHashStr(inner, 0, bar->GetID(id));
    for (float y = bar->Pos.y + 2.0f; y < bar->Pos.y + bar->Size.y; y += 8.0f)
    {
        for (float x = bar->Pos.x + 2.0f; x < bar->Pos.x + bar->Size.x; x += 6.0f)
        {
            io.AddMousePosEvent(x, y);
            frame(app);
            if (ImGui::GetCurrentContext()->HoveredIdPreviousFrame == target)
            {
                io.AddMouseButtonEvent(0, true);
                frame(app);
                io.AddMouseButtonEvent(0, false);
                frame(app);
                return true;
            }
        }
    }
    return false;
}

TEST_CASE("control bar buttons trigger their commands, also when it wraps")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    for (float width : {1280.0f, 400.0f})
    {
        CAPTURE(width);
        io.DisplaySize = {width, 800};
        App app;
        frame(app);
        frame(app); // the bar takes last frame's content height

        const ImGuiWindow* bar = ImGui::FindWindowByName("##control_bar");
        const float height = bar->Size.y;
        if (width < 500.0f)
        {
            CHECK(height > ImGui::GetFrameHeight() * 2.0f); // wrapped onto more rows
        }

        // Icon + text buttons: the id is "##icon_text" under the label.
        REQUIRE(click_control_bar(app, "Open", "##icon_text"));
        CHECK(menu_take(app.menu, Command::WorkspaceOpen));
        CHECK(app.menu.pending.none()); // nothing else fired while the mouse moved
        REQUIRE(click_control_bar(app, "Save", "##icon_text"));
        CHECK(menu_take(app.menu, Command::WorkspaceSave));

        // The Start pill is taller than a normal frame, so the bar is too.
        CHECK(height > ImGui::GetFrameHeight() + 8.0f);
        REQUIRE(click_control_bar(app, "Release the Kraken", "##icon_text"));
        CHECK(menu_take(app.menu, Command::MeasurementStart));

        REQUIRE(click_control_bar(app, "Record", "##icon_text"));
        CHECK(app.menu.record_armed);
        CHECK(menu_take(app.menu, Command::Record));

        REQUIRE(click_control_bar(app, "Setup Interface...", "##icon_text"));
        CHECK(menu_take(app.menu, Command::Setup));
        REQUIRE(click_control_bar(app, "Replay", "##icon_text"));
        CHECK(menu_take(app.menu, Command::NewReplayView));
        REQUIRE(click_control_bar(app, "DBC Editor", "##icon_text"));
        CHECK(menu_take(app.menu, Command::NewDbcEditor));
        REQUIRE(click_control_bar(app, "Convert", "##icon_text"));
        CHECK(menu_take(app.menu, Command::Convert));
        app.measuring = true; // disabled like the menu item while measuring (still hovered)
        REQUIRE(click_control_bar(app, "Setup Interface...", "##icon_text"));
        CHECK_FALSE(menu_take(app.menu, Command::Setup));
    }
}


TEST_CASE("shortcuts: chord names parse back, user overrides win, labels find commands")
{
    const UiTest ui;
    CHECK(chord_parse("Ctrl+Shift+T") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T));
    CHECK(chord_parse("F5") == ImGuiKey_F5);
    CHECK(chord_parse("None") == ImGuiKey_None);
    CHECK(chord_parse("") == ImGuiKey_None);
    CHECK(chord_parse("Ctrl+Nope") == -1);
    CHECK(chord_parse("Bogus+T") == -1);
    for (const int chord : {ImGuiMod_Ctrl | ImGuiKey_R, ImGuiMod_Alt | ImGuiKey_F4, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S, int{ImGuiKey_Escape}})
    {
        CHECK(chord_parse(chord_name(chord)) == chord);
    }
    MainMenu menu;
    CHECK(command_chord(menu, Command::NewTraceView) == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T));
    menu.chords[static_cast<std::size_t>(Command::NewTraceView)] = ImGuiMod_Ctrl | ImGuiKey_K;
    CHECK(command_chord(menu, Command::NewTraceView) == (ImGuiMod_Ctrl | ImGuiKey_K));
    CHECK(std::string(command_shortcut(menu, Command::NewTraceView)) == "Ctrl+K");
    menu.chords[static_cast<std::size_t>(Command::NewTraceView)] = ImGuiKey_None;
    CHECK(std::string(command_shortcut(menu, Command::NewTraceView)).empty());
    CHECK(command_by_label("Tab") == Command::NewTraceView);
    CHECK(command_by_label("Graph View") == Command::NewGraphView); // not the "##widget" one
    CHECK(command_by_label("Nope") == Command::Count);
}

TEST_CASE("shortcuts: every capturable key round-trips through chord_name / chord_parse with any modifiers")
{
    const UiTest ui;
    int checked = 0;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
    {
        const auto key = static_cast<ImGuiKey>(k);
        const bool modifier = (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) || key >= ImGuiKey_ReservedForModCtrl;
        const bool mouse = key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY;
        if (modifier || mouse) // chord_capture never returns these
        {
            continue;
        }
        for (const int mods : {0, int{ImGuiMod_Ctrl}, ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiMod_Alt | ImGuiMod_Super,
                               ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super})
        {
            const int chord = mods | k;
            const std::string name = chord_name(chord);
            INFO("key ", k, " name '", name, "'");
            CHECK(chord_parse(name) == chord); // what Settings writes to the ini must read back
            ++checked;
        }
    }
    CHECK(checked > 400);
    CHECK(chord_parse("Shift+Ctrl+T") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T)); // any modifier order
    CHECK(chord_parse("Ctrl+") == -1);
    CHECK(chord_parse("+T") == -1);
    CHECK(chord_parse("Ctrl+Ctrl+T") == (ImGuiMod_Ctrl | ImGuiKey_T));
    CHECK(chord_parse("ctrl+t") == -1); // case matters, as GetKeyName spells them
}
