#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/python_engine.h"
#include "core/setup.h"
#include "core/tasks.h"
#include "core/trace.h"
#include "core/trace_recorder.h"
#include "drivers/driver.h"
#include "ui/can_status.h"
#include "ui/conditional_logging.h"
#include "ui/dbc_editor.h"
#include "ui/lin_control.h"
#include "ui/log_window.h"
#include "ui/macros.h"
#include "ui/instrument_panel.h"
#include "ui/main_menu.h"
#include "ui/convert.h"
#include "ui/recording_dialog.h"
#include "ui/file_dialog.h"
#include "ui/frame_cache.h"
#include "ui/replay.h"
#include "ui/script_window.h"
#include "ui/settings.h"
#include "ui/settings_dialog.h"
#include "ui/setup_dialog.h"
#include "ui/status_bar.h"
#include "ui/theme.h"
#include "ui/tx_generator.h"
#include "ui/ink.h"
#include "ui/value_search.h"
#include "ui/watch_window.h"
#include "ui/vim_nav.h"
#include "ui/trace_window.h"
#include "ui/workspace_tabs.h"

// All application state. Grows as the port proceeds (trace, setup, interfaces, ...).
struct ImDrawList;

struct App
{
    bool quit = false;
    bool measuring = false; // measurement running; set by measurement start/stop (T16)
    MainMenu menu;          // menu/control-bar commands, recent files, record toggle
    InkState ink;           // the "ink" easter egg (ui/ink.h)
    Settings settings;      // ini-backed settings + current workspace file
    WorkspaceTabs workspace; // bottom tabs, one dockspace each
    ThemeFonts fonts; // fonts.mono for hex/data columns
    Tasks tasks;      // work posted from other threads; tasks.wake doubles as the RX wake-up
    Trace trace;
    std::shared_ptr<const FrameCache> trace_file; // keeps the mapping of trace.file (a loaded file) alive
    std::unordered_map<unsigned, TraceWindowState> trace_windows; // per workspace tab uid
    std::unordered_map<unsigned, ValueSearch> value_searches;     // per workspace tab uid
    Recorder recorder; // follows menu.record_armed; not movable, so App stays put
    std::chrono::steady_clock::time_point recorder_drained{};
    LogWindowState log_window;       // shared by every tab's Log window
    CanStatusState can_status;       // counters polled while measuring
    VimNav vim_windows;              // Ctrl+w window moves (ui/vim_nav)
    RecordingDialogState recording_dialog;
    ConvertState convert;
    SettingsDialogState settings_dialog;
    StatusBarState status_bar;       // bottom status line + menu bar connection corner
    Setup setup;
    SetupDialogState setup_dialog; // Measurement > Setup; edits a copy of setup
    std::deque<Iface> ifaces;              // every enumerated channel, index = BusMessage::iface
    std::vector<RxConsumer> rx_consumers;  // run on the RX threads, changed only while they are stopped
    std::vector<BusMessage> rx_scratch;
    std::map<unsigned, TxGenerator> tx_generators; // key: WorkspaceTab::uid; after ifaces (its thread sends on them)
    std::map<unsigned, Replay> replays;      // key: WorkspaceTab::uid; after ifaces (its thread sends on them)
    FileDialog trace_file_dialog;            // Trace > Save Trace to file / Export full trace / Import full trace
    // Graph > Export to PNG: main.cpp reads this rectangle (ImGui screen coordinates, main
    // viewport only) from the framebuffer after the frame is rendered and writes the file.
    // Graph "Export to PNG": a window drawn off screen this frame (ui/graph); main.cpp takes its
    // draw lists out of the main viewport and renders them into a framebuffer of w x h.
    struct PngExport
    {
        std::vector<ImDrawList*> lists; // the window's and its children's (the plot is a child window)
        float x = 0.0f, y = 0.0f;       // the window's position (the draw lists' origin)
        int w = 0, h = 0;
        bool transparent = false;
        std::string path;
    };
    std::optional<PngExport> png_export;
    std::map<unsigned, LinControl> lin_controls; // key: WorkspaceTab::uid
    std::map<unsigned, InstrumentPanel> instrument_panels; // key: WorkspaceTab::uid; after ifaces (sends on them)
    std::map<unsigned, WatchWindow> watch_windows;         // key: WorkspaceTab::uid
    Macros macros;                                         // of the workspace; after ifaces and tasks (their runs send and post)
    std::map<unsigned, DbcEditorState> dbc_editors;        // key: WorkspaceTab::uid; edits a copy of a DBC
    ConditionalLogging conditional_logging;  // fed from the trace every frame
    PyState python;                          // an RX consumer; one interpreter, one script at a time
    ScriptWindowState script;                // the "Python Script" window, shared by every tab
};

// Enumerates the drivers and, if the setup is empty, fills it with one network per interface.
void app_init_interfaces(App& app);
// Opens every enabled setup interface and starts its listener thread.
void app_measurement_start(App& app);
// Joins the listener threads and closes the interfaces. Call before glfwTerminate.
void app_measurement_stop(App& app);

// Work that must not run inside a frame (loading a workspace replaces the dock layout).
// Called right before ImGui::NewFrame().
void app_before_frame(App& app);

// Draws one frame of the UI. Called between ImGui::NewFrame() and ImGui::Render().
// Writes every frame of the trace store to path, format from its extension; errors go to the log.
void app_trace_save(App& app, const std::string& path);
void app_frame(App& app);
