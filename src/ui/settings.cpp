#include "ui/settings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#include <imgui_internal.h> // ImGuiSettingsHandler, ImHashStr
#include <pugixml.hpp>

#include "app.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/text.h"

namespace
{

constexpr int workspace_version = 2;
constexpr std::string_view ini_type = "Kraken";

constexpr std::string_view theme_names[] = {"system", "light", "dark"};

void* ini_read_open(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
{
    return std::string_view(name) == "Settings" ? handler->UserData : nullptr;
}

void ini_read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
{
    settings_ini_read_line(*static_cast<App*>(entry), line);
}

void ini_write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    std::string body;
    settings_ini_write(*static_cast<const App*>(handler->UserData), body);
    buf->appendf("[%s][Settings]\n", ini_type.data());
    buf->append(body.c_str());
    buf->append("\n");
}

std::filesystem::path config_dir()
{
    const std::filesystem::path dir = platform_config_dir();
    return dir.empty() ? dir : dir / "kraken-explorer";
}

} // namespace

void settings_ini_write(const App& app, std::string& out)
{
    const auto line = [&out](std::string_view key, const auto& value) { out += std::format("{}={}\n", key, value); };
    line("ui/theme", theme_names[static_cast<int>(app.settings.theme)]);
    line("ui/font_scale", app.settings.font_scale_pct);
    line("mainWindow/CANblaster", app.menu.canblaster ? 1 : 0);
    for (std::size_t i = 0; i < app.menu.chords.size(); ++i)
    {
        if (app.menu.chords[i] != chord_default)
        {
            const std::string_view label = command_label(static_cast<Command>(i));
            line(std::format("shortcut/{}", label.substr(0, label.find("##"))), chord_name(app.menu.chords[i]));
        }
    }
    line("trace/maxSize", app.trace.max_size);
    const RecordingConfig& rec = app.recorder.config;
    line("recording/folder", rec.folder);
    line("recording/fileNamePattern", rec.file_name_pattern);
    line("recording/format", trace_format_name(rec.format));
    line("recording/splitSizeMb", rec.split_size_mb);
    line("recording/stayArmed", rec.stay_armed ? 1 : 0);
    for (const auto& path : app.menu.recent_files)
    {
        line("recentFiles/list", path); // repeated, newest first
    }
    // Tabs of the last session, so the dock layout in the same ini finds its dockspaces again.
    for (const auto& tab : app.workspace.tabs)
    {
        line("workspace/tab", std::format("{} {}", tab.uid, tab.title));
    }
    line("workspace/currentTab", app.workspace.current);
}

void settings_ini_read_line(App& app, std::string_view line)
{
    const auto eq = line.find('=');
    if (eq == std::string_view::npos)
    {
        return;
    }
    const std::string_view key = line.substr(0, eq);
    const std::string_view value = line.substr(eq + 1);
    RecordingConfig& rec = app.recorder.config;
    int n = 0;
    if (key == "ui/theme")
    {
        if (const auto* it = std::ranges::find(theme_names, value); it != std::end(theme_names))
        {
            app.settings.theme = static_cast<ThemeMode>(it - std::begin(theme_names));
        }
    }
    else if (key == "ui/font_scale" && parse_number(value, n))
    {
        app.settings.font_scale_pct = std::clamp(n, 50, 300);
    }
    else if (key == "mainWindow/CANblaster")
    {
        app.menu.canblaster = value == "1";
    }
    else if (key.starts_with("shortcut/"))
    {
        const Command cmd = command_by_label(key.substr(9));
        if (const int chord = chord_parse(value); cmd != Command::Count && chord >= 0)
        {
            app.menu.chords[static_cast<std::size_t>(cmd)] = chord;
        }
    }
    else if (key == "trace/maxSize" && parse_number(value, n) && n >= 1000)
    {
        app.trace.max_size = static_cast<uint64_t>(std::min(n, 10000000));
    }
    else if (key == "recording/folder")
    {
        rec.folder = value;
    }
    else if (key == "recording/fileNamePattern" && !value.empty())
    {
        rec.file_name_pattern = value;
    }
    else if (key == "recording/format")
    {
        rec.format = trace_format_from_name(value).value_or(rec.format);
    }
    else if (key == "recording/splitSizeMb" && parse_number(value, n) && n >= 0)
    {
        rec.split_size_mb = n;
    }
    else if (key == "recording/stayArmed")
    {
        rec.stay_armed = value == "1";
    }
    else if (key == "recentFiles/list" && !value.empty() && app.menu.recent_files.size() < max_recent_files)
    {
        app.menu.recent_files.emplace_back(value);
    }
    else if (key == "workspace/tab")
    {
        unsigned uid = 0;
        const auto sp = value.find(' ');
        const auto dup = [&] { return std::ranges::any_of(app.workspace.tabs, [&](const WorkspaceTab& t) { return t.uid == uid; }); };
        if (sp != std::string_view::npos && parse_number(value.substr(0, sp), uid) && uid > 0 && !dup())
        {
            workspace_add_tab(app.workspace, std::string(value.substr(sp + 1)), uid);
        }
    }
    else if (key == "workspace/currentTab" && parse_number(value, n) && n >= 0
             && n < static_cast<int>(app.workspace.tabs.size()))
    {
        app.workspace.current = n;
    }
}

std::string settings_strip_ini(std::string_view ini)
{
    std::string out;
    bool skip = false;
    while (!ini.empty())
    {
        const auto nl = ini.find('\n');
        const std::string_view line = ini.substr(0, nl == std::string_view::npos ? ini.size() : nl + 1);
        ini.remove_prefix(line.size());
        if (line.starts_with('['))
        {
            skip = line.starts_with(std::format("[{}]", ini_type));
        }
        if (!skip)
        {
            out += line;
        }
    }
    return out;
}

void settings_init(App& app)
{
    ImGuiSettingsHandler handler;
    handler.TypeName = ini_type.data();
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = ini_read_open;
    handler.ReadLineFn = ini_read_line;
    handler.WriteAllFn = ini_write_all;
    handler.UserData = &app;
    ImGui::AddSettingsHandler(&handler);

    app.settings.os_dark = theme_os_prefers_dark();
    const std::filesystem::path dir = config_dir();
    std::error_code ec;
    if (dir.empty() || (std::filesystem::create_directories(dir, ec), ec))
    {
        log_warning(std::format("No config directory ({}), settings are not saved", ec.message()));
        return;
    }
    app.settings.ini_path = (dir / "kraken-explorer.ini").string();
    ImGui::LoadIniSettingsFromDisk(app.settings.ini_path.c_str()); // no-op when the file is missing
}

void settings_save(App& app)
{
    if (app.settings.ini_path.empty())
    {
        return;
    }
    // Write + rename: a crash or a second instance never leaves a half-written ini behind.
    const std::string tmp = app.settings.ini_path + ".tmp";
    std::error_code ec;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << ImGui::SaveIniSettingsToMemory(); // also clears io.WantSaveIniSettings
        if (!f.flush())
        {
            ec = std::make_error_code(std::errc::io_error);
        }
    }
    if (!ec)
    {
        std::filesystem::rename(tmp, app.settings.ini_path, ec);
    }
    if (ec)
    {
        log_warning(std::format("Cannot save settings to {}: {}", app.settings.ini_path, ec.message()));
        std::filesystem::remove(tmp, ec);
    }
}

void settings_save_if_wanted(App& app)
{
    if (ImGui::GetIO().WantSaveIniSettings)
    {
        settings_save(app);
        ImGui::GetIO().WantSaveIniSettings = false; // also when not persisted
    }
}

void settings_apply_theme(App& app)
{
    const ThemeMode mode = app.settings.theme;
    theme_apply(mode == ThemeMode::Dark || (mode == ThemeMode::System && app.settings.os_dark));
    // Platform windows: square corners and an opaque background, as imgui's examples do.
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;
    // imgui 1.92 rasterises glyphs at the scaled size, so this stays sharp (no FontGlobalScale blur).
    ImGui::GetStyle().FontScaleMain = static_cast<float>(app.settings.font_scale_pct) / 100.0f;
}

namespace
{

// <kraken-workspace> with <tabs> and <setup>; workspace_save adds <layout>.
void workspace_xml(const App& app, pugi::xml_document& doc)
{
    pugi::xml_node root = doc.append_child("kraken-workspace");
    root.append_attribute("kraken-version") = VERSION_STRING;
    root.append_attribute("workspace-version") = workspace_version;
    pugi::xml_node tabs = root.append_child("tabs");
    for (const auto& tab : app.workspace.tabs)
    {
        pugi::xml_node el = tabs.append_child("tab");
        el.append_attribute("title") = tab.title.c_str();
        el.append_attribute("uid") = tab.uid; // keys the dockspace ids in <layout>
        if (const auto gen = app.tx_generators.find(tab.uid); gen != app.tx_generators.end())
        {
            tx_generator_save_xml(gen->second, app.ifaces, el.append_child("txgeneratorwindow"));
        }
        if (const auto lin = app.lin_controls.find(tab.uid); lin != app.lin_controls.end() && lin->second.open)
        {
            lin_control_save_xml(lin->second, app.ifaces, el.append_child("lincontrolwindow"));
        }
        if (const auto ip = app.instrument_panels.find(tab.uid); ip != app.instrument_panels.end() && ip->second.open)
        {
            instrument_panel_save_xml(ip->second, app.ifaces, el.append_child("instrumentpanel"));
        }
        if (const auto ww = app.watch_windows.find(tab.uid); ww != app.watch_windows.end() && ww->second.open)
        {
            watch_save_xml(ww->second, el.append_child("watchwindow"));
        }
        if (const auto tw = app.trace_windows.find(tab.uid); tw != app.trace_windows.end())
        {
            trace_window_save_xml(tw->second, app.ifaces, el.append_child("tracewindow"));
        }
        graph_save_xml(tab.graphs, app.ifaces, el);
    }
    if (!app.macros.items.empty())
    {
        macros_save_xml(app.macros, app.ifaces, root.append_child("macros"));
    }
    pugi::xml_node setup = root.append_child("setup");
    setup_save_xml(app.setup, setup);
}

} // namespace

std::string workspace_snapshot(const App& app)
{
    pugi::xml_document doc;
    workspace_xml(app, doc);
    std::ostringstream out;
    doc.save(out, "", pugi::format_raw);
    return std::move(out).str();
}

bool workspace_dirty(const App& app)
{
    return !app.settings.saved_snapshot.empty() && workspace_snapshot(app) != app.settings.saved_snapshot;
}

bool workspace_save(App& app, const std::string& path)
{
    pugi::xml_document doc;
    workspace_xml(app, doc);
    pugi::xml_node root = doc.child("kraken-workspace");
    root.append_child("layout").append_child(pugi::node_cdata).set_value(
        settings_strip_ini(ImGui::SaveIniSettingsToMemory()).c_str());
    ImGui::GetIO().WantSaveIniSettings = true; // SaveIniSettingsToMemory cleared it
    if (!doc.save_file(path.c_str(), "  "))
    {
        log_error(std::format("Cannot open workspace file for writing: {}", path));
        return false;
    }
    app.settings.workspace_path = path;
    app.settings.saved_snapshot = workspace_snapshot(app);
    menu_add_recent(app.menu, path);
    log_info(std::format("Saved workspace settings to file: {}", path));
    return true;
}

bool workspace_load(App& app, const std::string& path)
{
    pugi::xml_document doc;
    if (const pugi::xml_parse_result res = doc.load_file(path.c_str()); !res)
    {
        log_error(std::format("Cannot load workspace file {}: {}", path, res.description()));
        return false;
    }
    const pugi::xml_node root = doc.child("kraken-workspace");
    if (!root)
    {
        log_error(std::format("Invalid workspace file format: {}", path));
        return false;
    }
    if (const int version = root.attribute("workspace-version").as_int(1); version != workspace_version)
    {
        log_error(std::format("Unsupported workspace version {} (expected {}): {}", version, workspace_version, path));
        return false;
    }

    Setup setup;
    setup_load_xml(setup, root.child("setup"));
    app.setup = std::move(setup);

    WorkspaceTabs& ws = app.workspace;
    ws.tabs.clear();
    app.tx_generators.clear(); // joins their sender threads
    app.lin_controls.clear();
    app.instrument_panels.clear();
    app.watch_windows.clear();
    app.trace_windows.clear(); // the views rebuild from the trace
    const unsigned next_uid = ws.next_uid;
    for (const pugi::xml_node el : root.child("tabs").children("tab"))
    {
        const unsigned uid = el.attribute("uid").as_uint();
        const bool dup = std::ranges::any_of(ws.tabs, [uid](const WorkspaceTab& t) { return t.uid == uid; });
        WorkspaceTab& tab = workspace_add_tab(ws, el.attribute("title").as_string("Trace"), uid > 0 && !dup ? uid : 0);
        graph_load_xml(tab.graphs, app.setup, app.ifaces, el); // none in older workspaces: the default graph
        if (const pugi::xml_node tw = el.child("tracewindow"); tw)
        {
            trace_window_load_xml(app.trace_windows[tab.uid], app.ifaces, tw);
        }
        if (const pugi::xml_node gen = el.child("txgeneratorwindow"); gen)
        {
            tx_generator_load_xml(app.tx_generators[tab.uid], app.ifaces, gen); // ifaces enumerated by now
        }
        if (const pugi::xml_node lin = el.child("lincontrolwindow"); lin)
        {
            LinControl& lc = app.lin_controls[tab.uid];
            lc.open = true;
            lin_control_load_xml(lc, app.ifaces, lin);
        }
        if (const pugi::xml_node ip = el.child("instrumentpanel"); ip)
        {
            InstrumentPanel& panel = app.instrument_panels[tab.uid];
            panel.open = true;
            instrument_panel_load_xml(panel, app.ifaces, ip);
        }
        if (const pugi::xml_node ww = el.child("watchwindow"); ww)
        {
            WatchWindow& watch = app.watch_windows[tab.uid];
            watch.open = true;
            watch_load_xml(watch, ww);
        }
    }
    ws.next_uid = std::max(ws.next_uid, next_uid);
    ws.current = 0;
    // The dock nodes are cleared and rebuilt from here; tabs without a node get the default layout.
    macros_load_xml(app.macros, app.ifaces, root.child("macros"));
    if (const pugi::xml_node layout = root.child("layout"); layout)
    {
        const std::string ini = settings_strip_ini(layout.text().get());
        ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    }

    app.settings.workspace_path = path;
    menu_add_recent(app.menu, path);
    ImGui::MarkIniSettingsDirty();
    app.settings.snapshot_in = 2; // after a frame has created the tabs' default window state
    log_info(std::format("Loaded workspace: {}", path));
    return true;
}

void workspace_open_dialog(App& app)
{
    file_dialog_open(app.settings.workspace_dialog, FileDialogMode::Open, "Open workspace configuration",
                     app.settings.workspace_path, {{"Workspace config files", "*.kraken"}, {"All Files", "*"}});
}

void workspace_save_dialog(App& app)
{
    // The dialog appends ".kraken" to a name without extension.
    file_dialog_open(app.settings.workspace_dialog, FileDialogMode::Save, "Save workspace configuration",
                     app.settings.workspace_path, {{"Workspace config files", "*.kraken"}});
}

void draw_workspace_dialog(App& app)
{
    for (const auto& path : file_dialog_draw(app.settings.workspace_dialog))
    {
        if (app.settings.workspace_dialog.mode == FileDialogMode::Save)
        {
            workspace_save(app, path);
        }
        else
        {
            app.settings.pending_open = path;
        }
    }
}
