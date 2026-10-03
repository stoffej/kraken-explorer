#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct App;
enum class Icon;

// Everything the main menu, its shortcuts and the control bar can trigger. Exit, Trace Clear,
// Record and About are handled in place; the rest is left in MainMenu::pending for the owning
// module (workspace, measurement, windows, ...) to pick up with menu_take() in the same frame.
enum class Command
{
    WorkspaceNew,
    WorkspaceOpen,
    WorkspaceOpenRecent, // path in MainMenu::recent_path
    WorkspaceSave,
    WorkspaceSaveAs,
    Settings,
    Exit,
    MeasurementStart,
    MeasurementStop,
    Record, // toggles MainMenu::record_armed
    RecordingOptions,
    RecordingOpenFolder,
    Setup,
    ReloadInterfaces,
    TraceClear,
    TraceSave,
    TraceExportFull,
    TraceImportFull,
    NewTraceView,
    NewGraphView,
    NewGraphWidget,
    NewReplayView,
    NewLinControl,
    NewInstrumentPanel,
    NewWatchWindow,
    NewDbcEditor,
    StandaloneGraph,
    ConditionalLogging,
    Macros,     // Measurement > Macros..., ui/macros
    FindSignal, // Ctrl+P palette, handled by ui/graph
    Convert,    // File > Convert..., ui/convert
    About,
    Count
};

inline constexpr std::size_t max_recent_files = 8;
inline constexpr int chord_default = -1; // MainMenu::chords entry: use the command table's shortcut

struct MainMenu
{
    std::bitset<static_cast<std::size_t>(Command::Count)> pending; // cleared at the end of every frame
    std::string recent_path;                                       // for Command::WorkspaceOpenRecent
    std::vector<std::string> recent_files;                         // newest first, at most max_recent_files
    bool record_armed = false;                                     // Ctrl+R; the recorder (T09) follows it
    std::string record_status;                                     // REC line, filled by the recorder (T09)
    bool canblaster = false;                                       // Measurement > Driver > CANblaster
    // User shortcuts (Settings > Shortcuts, ini "shortcut/<label>"): ImGuiKeyChord as int, 0 = none,
    // chord_default = the command table's.
    std::array<int, static_cast<std::size_t>(Command::Count)> chords = []
    {
        std::array<int, static_cast<std::size_t>(Command::Count)> a{};
        a.fill(chord_default);
        return a;
    }();
    bool capturing_shortcut = false; // the settings dialog is recording a key: global shortcuts pause
};

// The effective shortcut of cmd (user override or the command table's), 0 = none.
[[nodiscard]] int command_chord(const MainMenu& menu, Command cmd) noexcept;
// "Ctrl+Shift+T" <-> chord. chord_parse: 0 for "None"/empty, -1 for text it does not understand.
[[nodiscard]] std::string chord_name(int chord);
[[nodiscard]] int chord_parse(std::string_view text);
// Recording a chord (Settings > Shortcuts, Macros): the chord pressed this frame (0 for Backspace =
// none), chord_capture_cancelled for Esc, chord_capture_waiting while no key came.
inline constexpr int chord_capture_waiting = -1;
inline constexpr int chord_capture_cancelled = -2;
[[nodiscard]] int chord_capture();
// The command whose menu label (without any "##" suffix) is `label`, Command::Count when none.
[[nodiscard]] Command command_by_label(std::string_view label) noexcept;

// Triggers cmd as its menu item does; nothing when the command is disabled now (a macro step).
void menu_run(App& app, Command cmd);
// Returns true once if cmd was triggered this frame.
[[nodiscard]] bool menu_take(MainMenu& menu, Command cmd) noexcept;

// Moves path to the front of the Open Recent list.
void menu_add_recent(MainMenu& menu, std::string path);

// Toolbar icon + label button that triggers cmd like its menu item: same enabled state, tooltip =
// menu label + shortcut.
void command_button(App& app, Command cmd, Icon icon, const char* label);
[[nodiscard]] float command_button_width(const char* label);

// Menu label (may carry a "##" suffix) and effective shortcut name ("Ctrl+R", "" when none) of
// cmd (help overlay). The shortcut string is valid until the next call.
[[nodiscard]] const char* command_label(Command cmd) noexcept;
[[nodiscard]] const char* command_shortcut(const MainMenu& menu, Command cmd);

// Main menu bar, global shortcuts and the control bar below it (Start/Stop pills, Setup
// Interface..., Graph, Record). Call before the dockspace so it gets the rest.
void draw_main_menu(App& app);
