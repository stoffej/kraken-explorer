// Settings in the ImGui ini ([Kraken][Settings]) and workspace .kraken v2 round trips.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_env.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include <imgui.h>

#include "app.h"
#include "ui_test.h"

namespace
{

std::filesystem::path temp_dir()
{
    auto dir = std::filesystem::temp_directory_path() / "kraken_settings_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void fill(App& app)
{
    app.settings.theme = ThemeMode::Dark;
    app.menu.canblaster = true;
    app.trace.max_size = 123456;
    app.menu.recent_files = {"/a/one.kraken", "/b/two.kraken"};
    app.recorder.config.folder = "/rec";
    app.recorder.config.file_name_pattern = "x_{date}";
    app.recorder.config.format = TraceFileFormat::PcapNg;
    app.recorder.config.split_size_mb = 7;
    app.recorder.config.stay_armed = false;
    workspace_add_tab(app.workspace, "Trace", 3);
    workspace_add_tab(app.workspace, "My tab", 5);
    app.workspace.current = 1;
}

void check(const App& app)
{
    CHECK(app.settings.theme == ThemeMode::Dark);
    CHECK(app.menu.canblaster);
    CHECK(app.trace.max_size == 123456);
    CHECK(app.menu.recent_files == std::vector<std::string>{"/a/one.kraken", "/b/two.kraken"});
    CHECK(app.recorder.config.folder == "/rec");
    CHECK(app.recorder.config.file_name_pattern == "x_{date}");
    CHECK(app.recorder.config.format == TraceFileFormat::PcapNg);
    CHECK(app.recorder.config.split_size_mb == 7);
    CHECK_FALSE(app.recorder.config.stay_armed);
    REQUIRE(app.workspace.tabs.size() == 2);
    CHECK(app.workspace.tabs[0].uid == 3);
    CHECK(app.workspace.tabs[1].uid == 5);
    CHECK(app.workspace.tabs[1].title == "My tab");
    CHECK(app.workspace.current == 1);
    CHECK(app.workspace.next_uid == 6);
}

} // namespace

TEST_CASE("ini section round trip")
{
    App a;
    fill(a);
    std::string ini;
    settings_ini_write(a, ini);

    App b;
    size_t pos = 0;
    while (pos < ini.size())
    {
        const size_t nl = ini.find('\n', pos);
        settings_ini_read_line(b, std::string_view(ini).substr(pos, nl - pos));
        pos = nl + 1;
    }
    check(b);
}

TEST_CASE("text size: default 110 %, round trip")
{
    App a;
    CHECK(a.settings.font_scale_pct == 110);
    a.settings.font_scale_pct = 150;
    std::string ini;
    settings_ini_write(a, ini);
    CHECK(ini.find("ui/font_scale=150\n") != std::string::npos);
    App b;
    settings_ini_read_line(b, "ui/font_scale=150");
    CHECK(b.settings.font_scale_pct == 150);
    settings_ini_read_line(b, "ui/font_scale=big");
    CHECK(b.settings.font_scale_pct == 150);
}

TEST_CASE("malformed ini lines are ignored")
{
    App app;
    for (const char* line : {"ui/theme=purple", "workspace/tab=abc Trace", "workspace/tab=0 Zero", "workspace/tab=4",
                             "recording/splitSizeMb=-3", "recording/format=wav", "workspace/currentTab=9", "garbage"})
    {
        settings_ini_read_line(app, line);
    }
    CHECK(app.settings.theme == ThemeMode::System);
    CHECK(app.workspace.tabs.empty());
    CHECK(app.recorder.config.split_size_mb == 0);
    CHECK(app.recorder.config.format == TraceFileFormat::VectorAsc);
    settings_ini_read_line(app, "workspace/tab=2 A");
    settings_ini_read_line(app, "workspace/tab=2 Duplicate");
    CHECK(app.workspace.tabs.size() == 1);
}

TEST_CASE("strip removes only the Kraken sections")
{
    const std::string ini = "[Window][A]\nPos=1,2\n\n[Kraken][Settings]\nui/theme=dark\n\n[Docking][Data]\nDockSpace ID=0x1\n";
    CHECK(settings_strip_ini(ini) == "[Window][A]\nPos=1,2\n\n[Docking][Data]\nDockSpace ID=0x1\n");
}

TEST_CASE("settings_init + settings_save persist through the ini handler")
{
    const auto dir = temp_dir();
    test_setenv("XDG_CONFIG_HOME", dir);
    {
        ImGui::CreateContext();
        auto a = std::make_unique<App>();
        settings_init(*a);
        CHECK(a->settings.ini_path == (dir / "kraken-explorer" / "kraken-explorer.ini").string());
        fill(*a);
        settings_save(*a);
        ImGui::DestroyContext(); // before the App: the handler points at it
    }
    ImGui::CreateContext();
    auto b = std::make_unique<App>();
    settings_init(*b);
    check(*b);
    ImGui::DestroyContext();
}

// A docked layout in ImGui's own .ini format, exactly as SaveIniSettingsToMemory writes it
// back without a frame (no host Window= on the dockspace).
constexpr const char* layout_ini = "[Window][Trace@3]\n"
                                   "Pos=0,19\n"
                                   "Size=1280,500\n"
                                   "Collapsed=0\n"
                                   "DockId=0x00000001,0\n"
                                   "\n"
                                   "[Docking][Data]\n"
                                   "DockSpace   ID=0x8B93E3BD Pos=0,19 Size=1280,781 Split=Y\n"
                                   "  DockNode  ID=0x00000001 Parent=0x8B93E3BD SizeRef=1280,500 Selected=0x1C33C293\n"
                                   "  DockNode  ID=0x00000002 Parent=0x8B93E3BD SizeRef=1280,279\n"
                                   "\n";

TEST_CASE("workspace v2 save + load")
{
    const auto dir = temp_dir();
    const std::string path = (dir / "ws.kraken").string();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable; // else [Docking] is neither read nor written
    {
        App a;
        fill(a);
        SetupInterface intf{.driver = "SocketCAN", .name = "vcan0", .bitrate = 250000};
        a.setup.networks.push_back({.name = "Net A", .interfaces = {intf}});
        TxCyclic row{.name = "Ping", .interval_ms = 250};
        row.msg.id = 0x123;
        row.msg.len = 2;
        row.msg.data[1] = 0xAB;
        a.tx_generators[5].rows.push_back(row);
        ImGui::LoadIniSettingsFromMemory(layout_ini); // as a running app would have it
        REQUIRE(workspace_save(a, path));
        CHECK(a.settings.workspace_path == path);
        CHECK(a.menu.recent_files.front() == path);
    }
    std::ifstream f(path);
    const std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(xml.find("workspace-version=\"2\"") != std::string::npos);
    CHECK(xml.find("<layout>") != std::string::npos);
    CHECK(xml.find("[Kraken]") == std::string::npos);
    CHECK(xml.find(layout_ini) != std::string::npos);

    ImGui::DestroyContext(); // the load must bring the layout back into a fresh context
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    App b;
    b.workspace.next_uid = 9;
    REQUIRE(workspace_load(b, path));
    REQUIRE(b.setup.networks.size() == 1);
    CHECK(b.setup.networks[0].name == "Net A");
    REQUIRE(b.setup.networks[0].interfaces.size() == 1);
    CHECK(b.setup.networks[0].interfaces[0].name == "vcan0");
    CHECK(b.setup.networks[0].interfaces[0].bitrate == 250000);
    REQUIRE(b.workspace.tabs.size() == 2);
    CHECK(b.workspace.tabs[1].uid == 5);
    CHECK(b.workspace.tabs[1].title == "My tab");
    CHECK(b.workspace.current == 0);
    CHECK(b.workspace.next_uid == 9);
    CHECK_FALSE(b.tx_generators.contains(3)); // no generator saved for that tab
    REQUIRE(b.tx_generators[5].rows.size() == 1);
    CHECK(b.tx_generators[5].rows[0].name == "Ping");
    CHECK(b.tx_generators[5].rows[0].interval_ms == 250);
    CHECK(b.tx_generators[5].rows[0].msg.id == 0x123);
    CHECK(b.tx_generators[5].rows[0].msg.data[1] == 0xAB);
    CHECK(b.settings.workspace_path == path);

    // Save again after the load: byte-identical file (setup, tabs and layout ini).
    const std::string path2 = (dir / "ws2.kraken").string();
    REQUIRE(workspace_save(b, path2));
    std::ifstream f2(path2);
    const std::string xml2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    CHECK(xml2 == xml);

    std::ofstream(dir / "v1.kraken") << "<kraken-workspace workspace-version=\"1\"><setup/></kraken-workspace>";
    CHECK_FALSE(workspace_load(b, (dir / "v1.kraken").string()));
    CHECK_FALSE(workspace_load(b, (dir / "missing.kraken").string()));
    CHECK(b.setup.networks.size() == 1); // untouched by the failed loads
    ImGui::DestroyContext();
}

TEST_CASE("settings dialog: Cancel reverts the live theme and text size")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    App app;
    app.settings.theme = ThemeMode::Light;
    app.settings.font_scale_pct = 110;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        draw_settings_dialog(app, app.settings_dialog);
        ImGui::EndFrame();
        app.menu.pending.reset();
    };
    frame(); // closed: the live theme is left alone (it was reverted to the defaults every frame)
    frame();
    CHECK(app.settings.theme == ThemeMode::Light);
    CHECK(app.settings.font_scale_pct == 110);
    app.menu.pending.set(static_cast<std::size_t>(Command::Settings));
    frame();
    frame();
    app.settings.theme = ThemeMode::Dark; // as the combos set them
    app.settings.font_scale_pct = 150;
    settings_apply_theme(app);
    frame();
    CHECK(ImGui::GetStyle().FontScaleMain == doctest::Approx(1.5f));

    io.AddKeyEvent(ImGuiKey_Escape, true); // Cancel
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    CHECK(app.settings.theme == ThemeMode::Light);
    CHECK(app.settings.font_scale_pct == 110);
    CHECK(ImGui::GetStyle().FontScaleMain == doctest::Approx(1.1f));
    ImGui::NewFrame();
    CHECK_FALSE(ImGui::IsPopupOpen("Settings")); // needs a current window
    ImGui::EndFrame();

}

TEST_CASE("workspace dirty: tabs or setup differ from the snapshot")
{
    ImGui::CreateContext();
    App app;
    fill(app);
    CHECK_FALSE(workspace_dirty(app)); // no snapshot taken yet
    app.settings.saved_snapshot = workspace_snapshot(app);
    CHECK_FALSE(workspace_dirty(app));
    app.workspace.tabs[1].title = "Renamed";
    CHECK(workspace_dirty(app));
    app.workspace.tabs[1].title = "My tab";
    CHECK_FALSE(workspace_dirty(app));
    app.setup.networks.push_back({.name = "Net B"});
    CHECK(workspace_dirty(app));
    ImGui::DestroyContext();
}

TEST_CASE("workspace dirty: a signal added to a graph")
{
    ImGui::CreateContext();
    App app;
    fill(app);
    app.workspace.tabs[1].graphs.emplace_back();
    app.settings.saved_snapshot = workspace_snapshot(app);
    CHECK_FALSE(workspace_dirty(app));
    GraphSignal s;
    s.kind = GraphSignalKind::BusLoad;
    app.workspace.tabs[1].graphs[0].signals.push_back(s);
    CHECK(workspace_dirty(app));
    app.workspace.tabs[1].graphs[0].signals.clear();
    CHECK_FALSE(workspace_dirty(app));
    app.workspace.tabs[1].graphs[0].statistics = true;
    CHECK(workspace_dirty(app));
    ImGui::DestroyContext();
}

TEST_CASE("workspace graphs: saved per tab, older workspaces without <graph> load as before")
{
    const auto dir = temp_dir();
    ImGui::CreateContext();
    {
        App a;
        fill(a);
        GraphState& g = a.workspace.tabs[1].graphs.emplace_back();
        g.view = GraphView::Gauge;
        g.statistics = true;
        a.workspace.tabs[1].graphs.emplace_back().id = 2;
        REQUIRE(workspace_save(a, (dir / "g.kraken").string()));
    }
    App b;
    REQUIRE(workspace_load(b, (dir / "g.kraken").string()));
    REQUIRE(b.workspace.tabs.size() == 2);
    CHECK(b.workspace.tabs[0].graphs.empty());
    REQUIRE(b.workspace.tabs[1].graphs.size() == 2);
    CHECK(b.workspace.tabs[1].graphs[0].view == GraphView::Gauge);
    CHECK(b.workspace.tabs[1].graphs[0].statistics);
    CHECK(b.workspace.tabs[1].graphs[1].id == 2);

    std::ofstream(dir / "old.kraken") << "<kraken-workspace workspace-version=\"2\"><tabs><tab title=\"Old\" uid=\"4\"/>"
                                           "</tabs><setup/></kraken-workspace>";
    App c;
    REQUIRE(workspace_load(c, (dir / "old.kraken").string()));
    REQUIRE(c.workspace.tabs.size() == 1);
    CHECK(c.workspace.tabs[0].title == "Old");
    CHECK(c.workspace.tabs[0].graphs.empty()); // the frame adds the default graph, as before
    ImGui::DestroyContext();
}
