#include "app.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <utility>

#include "core/log.h"
#include "core/platform.h"
#include "core/trace_file_writer.h"
#include "drivers/grip.h"
#include "ui/help_overlay.h"

namespace
{

void app_workspace_run(App& app, Command cmd, const std::string& recent_path)
{
    if (cmd == Command::WorkspaceNew)
    {
        app_measurement_stop(app);
        trace_clear(app.trace);
        app.workspace.tabs.clear(); // draw_workspace adds a fresh default tab
        app.tx_generators.clear();  // else orphaned generators keep their rows (and threads)
        for (auto& [uid, r] : app.replays)
        {
            replay_stop(r); // else the player keeps filling the new trace, with no window to stop it
        }
        app.replays.clear();
        app.trace_windows.clear();
        app.lin_controls.clear();
        app.value_searches.clear();
        app.instrument_panels.clear();
        app.watch_windows.clear();
        app.macros.items.clear();
        app.dbc_editors.clear();
        app.setup = {};
        ifaces_default_setup(app.ifaces, app.setup);
        app.settings.workspace_path.clear();
        app.settings.snapshot_in = 2;
    }
    else if (cmd == Command::WorkspaceOpen)
    {
        workspace_open_dialog(app);
    }
    else
    {
        app.settings.pending_open = recent_path;
    }
}

void app_workspace_commands(App& app)
{
    constexpr const char* unsaved_popup = "Save changes?##workspace";
    if (app.settings.snapshot_in > 0 && --app.settings.snapshot_in == 0)
    {
        app.settings.saved_snapshot = workspace_snapshot(app);
    }
    for (const Command cmd : {Command::WorkspaceNew, Command::WorkspaceOpen, Command::WorkspaceOpenRecent})
    {
        if (!menu_take(app.menu, cmd))
        {
            continue;
        }
        if (workspace_dirty(app))
        {
            app.settings.unsaved_command = cmd;
            app.settings.unsaved_path = app.menu.recent_path;
            ImGui::OpenPopup(unsaved_popup);
        }
        else
        {
            app_workspace_run(app, cmd, app.menu.recent_path);
        }
    }
    bool unsaved_open = true; // the title bar's X = Cancel
    if (ImGui::BeginPopupModal(unsaved_popup, &unsaved_open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("The workspace has unsaved changes. Save them first?");
        const float bw = ImGui::GetFontSize() * 6.0f;
        int choice = 0; // 1 Save, 2 Discard, 3 Cancel
        choice = ImGui::Button("Save", ImVec2(bw, 0.0f)) ? 1 : choice;
        ImGui::SameLine();
        choice = ImGui::Button("Discard", ImVec2(bw, 0.0f)) ? 2 : choice;
        ImGui::SameLine();
        choice = ImGui::Button("Cancel", ImVec2(bw, 0.0f)) || ImGui::Shortcut(ImGuiKey_Escape) ? 3 : choice;
        if (choice != 0)
        {
            ImGui::CloseCurrentPopup();
        }
        if (choice == 1 && app.settings.workspace_path.empty())
        {
            // ponytail: never saved -> Save As, and the New/Open is dropped; the user repeats it. Chain it after the save if that annoys.
            workspace_save_dialog(app);
        }
        else if (choice == 2 || (choice == 1 && workspace_save(app, app.settings.workspace_path)))
        {
            app_workspace_run(app, app.settings.unsaved_command, app.settings.unsaved_path);
        }
        ImGui::EndPopup();
    }
    const bool save_as = menu_take(app.menu, Command::WorkspaceSaveAs);
    if (menu_take(app.menu, Command::WorkspaceSave) || save_as)
    {
        if (save_as || app.settings.workspace_path.empty())
        {
            workspace_save_dialog(app);
        }
        else
        {
            workspace_save(app, app.settings.workspace_path);
        }
    }
    draw_workspace_dialog(app);
}

} // namespace

void app_before_frame(App& app)
{
    if (app.settings.pending_open.empty())
    {
        return;
    }
    const std::string path = std::exchange(app.settings.pending_open, {});
    app_measurement_stop(app);
    trace_clear(app.trace);
    workspace_load(app, path);
}

void app_trace_save(App& app, const std::string& path)
{
    const auto format = trace_format_from_path(path);
    if (!format)
    {
        log_error(std::format("Unknown trace format for {} (use .asc, .blf, .candump, .log, .mf4, .pcap, .pcapng or .trc)", path));
        return;
    }
    // ponytail: the whole trace in RAM (88 B per frame, a file view decoded); a chunked writer if
    // exports of 100M-frame files matter.
    const std::vector<BusMessage> msgs = trace_copy(app.trace);
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out)
    {
        log_error(std::format("Cannot write {}", path));
        return;
    }
    write_trace_file(out, *format, msgs, [&app](uint16_t iface)
    {
        return iface < app.ifaces.size() ? app.ifaces[iface].info.name : std::to_string(iface);
    });
    log_info(std::format("Saved {} frames to {}", msgs.size(), path));
}

void app_frame(App& app)
{
    tasks_drain(app.tasks, app);
    for (auto& iface : app.ifaces)
    {
        inbox_take(iface.inbox, app.rx_scratch);
    }
    for (auto& [uid, gen] : app.tx_generators)
    {
        tx_generator_on_rx(gen, app.rx_scratch); // Generator rows triggered "On receive"
    }
    trace_append_sorted(app.trace, app.rx_scratch);
    for (auto& [uid, panel] : app.instrument_panels)
    {
        instrument_panel_ingest(panel, app.setup, app.trace); // only the frames appended since last time
    }
    for (auto& [uid, watch] : app.watch_windows)
    {
        watch_ingest(watch, app.setup, app.trace);
    }
    draw_main_menu(app);
    app_workspace_commands(app);
    if (menu_take(app.menu, Command::MeasurementStart))
    {
        app_measurement_start(app);
    }
    if (menu_take(app.menu, Command::MeasurementStop))
    {
        app_measurement_stop(app);
    }
    if (menu_take(app.menu, Command::Record))
    {
        recorder_set_armed(app.recorder, app.menu.record_armed, app.measuring);
    }
    if (const auto now = std::chrono::steady_clock::now(); now - app.recorder_drained >= Recorder::drain_interval)
    {
        app.recorder_drained = now;
        recorder_drain(app.recorder);
    }
    // The recorder disarms itself on write errors and, without stay_armed, after a measurement.
    app.menu.record_armed = app.recorder.armed;
    app.menu.record_status = app.recorder.recording
        ? std::format("REC {} ({} frames)", app.recorder.file_path, app.recorder.frames_written)
        : std::string{};
    if (menu_take(app.menu, Command::ReloadInterfaces) && !app.measuring)
    {
        canblast_enabled = app.menu.canblaster; // Driver > CANblaster applies on reload
        ifaces_enumerate(app.ifaces);
    }
    if (menu_take(app.menu, Command::Setup) && !app.measuring)
    {
        setup_dialog_open(app, app.setup_dialog);
    }
    draw_status_bar(app, app.status_bar);
    if (WorkspaceTab* tab = draw_workspace(app))
    {
        vim_window_nav(app.vim_windows, tab->uid); // Ctrl+w h/j/k/l/w, before the windows take the keys
        draw_trace_window(app, app.trace_windows[tab->uid], *tab);
        draw_log_window(app.log_window, *tab);
        draw_can_status(app, app.can_status, *tab);
        draw_tx_generator(app, *tab, app.tx_generators[tab->uid]);
        draw_script_window(app, app.script, *tab);
        draw_value_search(app, app.value_searches[tab->uid], *tab);
        if (menu_take(app.menu, Command::NewReplayView))
        {
            app.replays[tab->uid].open = true;
        }
        // Trace > Save / Export write the whole trace store (the same thing: the view keeps no
        // separate filtered copy); Import opens the file in this tab's Replay view.
        if (menu_take(app.menu, Command::TraceSave) || menu_take(app.menu, Command::TraceExportFull))
        {
            file_dialog_open(app.trace_file_dialog, FileDialogMode::Save, "Save Trace", app.recorder.config.folder + "/trace.asc",
                             {{"Vector ASC", "*.asc"}, {"Linux candump", "*.candump *.log"}, {"MDF4", "*.mf4"},
                              {"PCAP", "*.pcap"}, {"PCAPng", "*.pcapng"}, {"PEAK PCAN trace", "*.trc"}});
        }
        if (menu_take(app.menu, Command::TraceImportFull))
        {
            file_dialog_open(app.trace_file_dialog, FileDialogMode::Open, "Import Trace", app.recorder.config.folder, trace_read_filters);
        }
        for (const auto& path : file_dialog_draw(app.trace_file_dialog))
        {
            if (app.trace_file_dialog.mode == FileDialogMode::Save)
            {
                app_trace_save(app, path);
            }
            else
            {
                Replay& r = app.replays[tab->uid];
                r.open = true;
                replay_load(app, r, path);
            }
        }
        if (menu_take(app.menu, Command::NewLinControl))
        {
            app.lin_controls[tab->uid].open = true;
        }
        if (const auto it = app.lin_controls.find(tab->uid); it != app.lin_controls.end())
        {
            draw_lin_control(app, *tab, it->second);
        }
        if (menu_take(app.menu, Command::NewInstrumentPanel))
        {
            app.instrument_panels[tab->uid].open = true;
        }
        if (const auto it = app.instrument_panels.find(tab->uid); it != app.instrument_panels.end())
        {
            draw_instrument_panel(app, *tab, it->second);
        }
        if (menu_take(app.menu, Command::NewWatchWindow))
        {
            app.watch_windows[tab->uid].open = true;
        }
        if (const auto it = app.watch_windows.find(tab->uid); it != app.watch_windows.end())
        {
            draw_watch_window(app, *tab, it->second);
        }
        if (menu_take(app.menu, Command::NewDbcEditor))
        {
            DbcEditorState& ed = app.dbc_editors[tab->uid];
            ed.open = true;
            // An empty editor starts on the setup's active DBC (the first; "From setup" picks another).
            if (ed.path.empty() && ed.db.messages.empty() && !ed.dirty)
            {
                for (const SetupNetwork& net : app.setup.networks)
                {
                    if (!net.can_dbs.empty())
                    {
                        dbc_editor_load(ed, *net.can_dbs.front(), net.can_dbs.front()->path);
                        break;
                    }
                }
            }
        }
        if (const auto it = app.dbc_editors.find(tab->uid); it != app.dbc_editors.end())
        {
            draw_dbc_editor(app, it->second, *tab);
        }
    }
    if (menu_take(app.menu, Command::ConditionalLogging))
    {
        conditional_logging_open(app.conditional_logging);
    }
    macros_frame(app, app.macros);
    conditional_logging_frame(app, app.conditional_logging);
    draw_conditional_logging(app, app.conditional_logging);
    draw_graph_windows(app, workspace_current(app.workspace));
    draw_setup_dialog(app, app.setup_dialog);
    draw_recording_dialog(app, app.recording_dialog);
    draw_convert(app, app.convert);
    draw_settings_dialog(app, app.settings_dialog);
    for (auto& [uid, replay] : app.replays)
    {
        if (const auto it = std::ranges::find(app.workspace.tabs, uid, &WorkspaceTab::uid); it != app.workspace.tabs.end())
        {
            draw_replay(app, *it, replay); // also runs autoplay for tabs not shown
        }
    }
    draw_help_overlay(app.menu); // "?"
    ink_frame(app.ink);          // "ink" typed: ink over everything
    // Handlers in this frame take their commands with menu_take(); whatever nobody took expires here.
    app.menu.pending.reset();
}

void app_init_interfaces(App& app)
{
    grip_attach(app.tasks, app.ifaces); // GrIP GPIO reports / capability updates
    app.recorder.iface_name = [&app](uint16_t i) { return i < app.ifaces.size() ? app.ifaces[i].info.name : std::string{}; };
    if (app.recorder.config.folder.empty()) // not in the settings ini yet
    {
        const std::filesystem::path home = platform_home_dir();
        app.recorder.config.folder = ((home.empty() ? "." : home) / "Documents" / "KrakenExplorer").string();
    }
    app.rx_consumers.push_back({.fn = recorder_rx_consumer, .user = &app.recorder});
    app.rx_consumers.push_back({.fn = python_rx_consumer, .user = &app.python});
    canblast_enabled = app.menu.canblaster;
    ifaces_enumerate(app.ifaces);
    if (app.setup.networks.empty())
    {
        ifaces_default_setup(app.ifaces, app.setup);
    }
}

void app_measurement_start(App& app)
{
    if (app.measuring)
    {
        return;
    }
    log_info("Starting measurement");
    recorder_measurement_starting(app.recorder); // before the RX threads, so no first frame is missed
    ifaces_start(app.ifaces, app.setup, app.rx_consumers, app.tasks.wake);
    std::string down;
    int enabled = 0;
    for (const auto& net : app.setup.networks)
    {
        for (const auto& si : net.interfaces)
        {
            enabled += si.enabled;
            if (si.enabled && (si.iface < 0 || !app.ifaces[static_cast<size_t>(si.iface)].open))
            {
                down += std::format("{}{}/{}", down.empty() ? "" : ", ", si.driver, si.name);
            }
        }
    }
    if (enabled == 0)
    {
        down = "No interface selected: Setup Interfaces... to add one";
        status_bar_notice(app.status_bar, down);
        log_warning(down);
    }
    else if (!down.empty())
    {
        down = std::format("Measurement started without: {}", down);
        status_bar_notice(app.status_bar, down);
        log_warning(down);
    }
    app.measuring = true;
}

void app_measurement_stop(App& app)
{
    if (!app.measuring)
    {
        return;
    }
    for (auto& [uid, gen] : app.tx_generators)
    {
        tx_generator_stop_all(gen); // cyclic rows off before their interfaces close
    }
    ifaces_stop(app.ifaces);
    for (auto& iface : app.ifaces)
    {
        inbox_take(iface.inbox, app.rx_scratch); // the last batches after the join
    }
    trace_append_sorted(app.trace, app.rx_scratch);
    app.measuring = false;
    recorder_measurement_stopped(app.recorder); // after the join: flushes the queue, writes the footer
    log_info(std::format("Measurement stopped, {} frames in trace", trace_size(app.trace)));
}
