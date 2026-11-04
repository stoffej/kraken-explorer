#pragma once

#include <string>
#include <string_view>

#include "ui/file_dialog.h"
#include "ui/main_menu.h"

struct App;

enum class ThemeMode
{
    System,
    Light,
    Dark
};

// Application settings (the old QSettings keys) and the workspace file. Settings live in the
// ImGui .ini under $XDG_CONFIG_HOME/kraken-explorer/, in one "[Kraken][Settings]" section next to
// the layout. Keys keep the old QSettings names (ui/theme, recording/folder, ...). State that
// already has an owner (menu.recent_files, menu.canblaster, recorder.config, workspace tabs)
// is read and written in place; only settings without another home are fields here.
struct Settings
{
    ThemeMode theme = ThemeMode::System; // ui/theme; applied by settings_apply_theme()
    int font_scale_pct = 110;            // ui/font_scale, 100..175; style.FontScaleMain
    bool os_dark = false;                // desktop preference, queried once at start
    bool update_check = true;            // ui/updater: look for a new release at start and daily
    bool uds_addr_ext = false;           // uds/addr_ext: the bus uses extended (address-byte) ISO-TP

    std::string ini_path;       // empty = nothing is persisted
    std::string workspace_path; // current .kraken file, empty = never saved
    std::string pending_open;   // workspace to load before the next frame (app_before_frame)
    FileDialog workspace_dialog; // Open / Save As; its mode tells which
    std::string saved_snapshot; // workspace_snapshot() at the last save / load; empty = not taken yet
    int snapshot_in = 2;        // frames until saved_snapshot is taken (after start, load, New)
    Command unsaved_command = Command::WorkspaceNew; // New / Open / Open Recent held by "Save changes?"
    std::string unsaved_path;   // its recent path
};

// Registers the ini handler and loads the ini. After ImGui::CreateContext(), before
// app_init_interfaces() and the first frame. Leaves io.IniFilename null: saving goes
// through settings_save() so the file is replaced atomically.
void settings_init(App& app);
// Writes the ini when ImGui (or a settings change via ImGui::MarkIniSettingsDirty) asks for it.
void settings_save_if_wanted(App& app);
void settings_save(App& app);

// Applies settings.theme (System follows the desktop) and settings.font_scale_pct. Also sets the platform-window tweaks.
void settings_apply_theme(App& app);

// The [Kraken][Settings] section body, one "key=value" per line (exposed for tests).
void settings_ini_write(const App& app, std::string& out);
void settings_ini_read_line(App& app, std::string_view line);
// `ini` without its [Kraken] sections: the layout part stored in a workspace.
[[nodiscard]] std::string settings_strip_ini(std::string_view ini);

// Workspace .kraken v2: <kraken-workspace workspace-version="2"> with <tabs> (per-tab
// window settings go into each <tab>), <setup> and the ImGui layout ini in <layout>.
// Version 1 (Qt) files are rejected. Callers stop the measurement before loading.
bool workspace_save(App& app, const std::string& path);
// <tabs> + <setup> as saved, without the layout (dock sizes are not a change worth asking about).
[[nodiscard]] std::string workspace_snapshot(const App& app);
// Tabs or setup differ from the last save / load. Only call it on a command: it serializes.
[[nodiscard]] bool workspace_dirty(const App& app);
// Replaces setup, tabs and layout. Loads the layout ini, so call it outside a frame.
bool workspace_load(App& app, const std::string& path);
// Open / Save As through the in-app file dialog. draw_workspace_dialog() (every frame) sets
// pending_open or calls workspace_save() once the user confirms.
void workspace_open_dialog(App& app);
void workspace_save_dialog(App& app);
void draw_workspace_dialog(App& app);
