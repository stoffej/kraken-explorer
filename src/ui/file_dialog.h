#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ui/vim_nav.h"

// In-app file chooser (replaces portable-file-dialogs): an ImGui modal in the app theme.
// Asynchronous: file_dialog_open() once, then file_dialog_draw() every frame from the same
// place (inside the parent popup when there is one). draw returns the chosen absolute
// paths on the frame the user confirms, empty otherwise (also on cancel).
//
//   if (ImGui::Button("Load")) file_dialog_open(s.dialog, FileDialogMode::Open, "Load Trace", s.path, {{"ASC", "*.asc"}});
//   for (const auto& path : file_dialog_draw(s.dialog)) load(path);
//
// No <imgui.h> here: app.h (and through it the drivers) may include this header.

enum class FileDialogMode
{
    Open,         // one existing file
    OpenMultiple, // one or more existing files (Ctrl+click)
    Save,         // file name; asks before replacing, appends the filter's extension
    SelectFolder, // a directory
};

// One entry of the type combo. patterns: space-separated globs ("*.asc *.log"), "*" = all.
struct FileFilter
{
    std::string name;
    std::string patterns;
};

// Trace files the replay parsers read (also Trace > Import full trace).
inline const std::vector<FileFilter> trace_read_filters = {
    {"All Supported", "*.asc *.blf *.candump *.log *.mf4 *.mdf *.pcap *.pcapng *.trc"}, {"Vector ASC", "*.asc"},
    {"Vector BLF", "*.blf"},                {"ASAM MDF4", "*.mf4 *.mdf"},
    {"Linux candump", "*.candump *.log"},   {"PCAP", "*.pcap"},          {"PCAPng", "*.pcapng"},
    {"PEAK PCAN trace", "*.trc"},           {"All Files", "*"}};

// CAN databases the setup loads (Setup dialog, Replay's prompt after a trace).
inline const std::vector<FileFilter> can_db_read_filters = {{"CAN Databases (*.dbc *.dbf *.sym)", "*.dbc *.dbf *.sym"},
                                                            {"All Files", "*"}};

struct FileEntry
{
    std::string name;
    bool is_dir = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
};

enum class FileSortColumn
{
    Name,
    Size,
    Modified,
};

struct FileDialog
{
    FileDialogMode mode = FileDialogMode::Open;
    std::string title;
    std::vector<FileFilter> filters; // empty = all files
    int filter = 0;

    bool visible = false;      // between open and OK/Cancel
    bool open_request = false; // OpenPopup on the next draw
    std::filesystem::path dir;
    std::string location;      // editable path field
    std::string file_name;     // name field (Save, or typed name for Open)
    std::vector<FileEntry> entries;
    std::vector<std::string> selected; // names in dir
    std::string error;         // listing / validation error shown under the list
    bool unreadable = false;   // listing dir failed: "Cannot read folder" instead of "Empty folder"
    std::string confirm_path;  // Save: waiting for "Replace?"
    FileSortColumn sort = FileSortColumn::Name;
    bool ascending = true;
    bool show_hidden = false;
    VimNav vim;              // j/k/gg/G/Ctrl+d.. in the list, h = up, l = into dir, Enter = open
    bool focus_name = false; // '/': keyboard focus to the name (or folder path) field next frame
};

// start_path: a directory, a file (its directory, and its name for Save) or a bare name
// ("graph.csv"); empty or missing starts in the last used directory, else $HOME.
void file_dialog_open(FileDialog& d, FileDialogMode mode, std::string title, const std::string& start_path,
                      std::vector<FileFilter> filters = {});
[[nodiscard]] std::vector<std::string> file_dialog_draw(FileDialog& d);

// Logic behind the dialog (testable without ImGui).
// Case-insensitive glob (* and ?) match of name against any of the space-separated patterns.
[[nodiscard]] bool file_filter_match(std::string_view patterns, std::string_view name);
// Directory contents without "." / ".."; hidden (dot) entries only when show_hidden.
// Sets error (and returns empty) when the directory cannot be read.
[[nodiscard]] std::vector<FileEntry> file_dialog_list(const std::filesystem::path& dir, bool show_hidden,
                                                      std::string& error);
// Directories first, then by column (name ties broken by name, case-insensitive).
void file_dialog_sort(std::vector<FileEntry>& entries, FileSortColumn column, bool ascending);
// Save target: dir / name, plus the extension of the first "*.ext" pattern when name has none.
[[nodiscard]] std::filesystem::path file_dialog_save_path(const std::filesystem::path& dir, const std::string& name,
                                                          std::string_view patterns);
// Save name after picking another file type: the extension becomes the type's first "*.ext"
// ("trace.asc" + "*.candump *.log" -> "trace.candump"); unchanged for an all-files type.
[[nodiscard]] std::string file_dialog_retype(const std::string& name, std::string_view patterns);
