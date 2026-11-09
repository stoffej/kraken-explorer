#include "ui/file_dialog.h"

#include "core/platform.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <ranges>

#include <imgui.h>
#include <imgui_internal.h> // SetKeyOwner, NavActivateId
#include <misc/cpp/imgui_stdlib.h>

#include "core/local_time.h"
#include "core/text.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/vim_nav.h"

namespace fs = std::filesystem;

namespace
{

std::vector<std::string> g_recent_dirs;
constexpr std::size_t max_recent = 8;

// Iterative glob with backtracking to the last '*'.
bool glob_match(std::string_view p, std::string_view s)
{
    std::size_t pi = 0;
    std::size_t si = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (si < s.size())
    {
        if (pi < p.size() && (p[pi] == '?' || ascii_lower(p[pi]) == ascii_lower(s[si])))
        {
            ++pi;
            ++si;
        }
        else if (pi < p.size() && p[pi] == '*')
        {
            star = pi++;
            mark = si;
        }
        else if (star != std::string_view::npos)
        {
            pi = star + 1;
            si = ++mark;
        }
        else
        {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*')
    {
        ++pi;
    }
    return pi == p.size();
}

bool less_ci(const std::string& a, const std::string& b)
{
    return std::ranges::lexicographical_compare(a, b, {}, ascii_lower, ascii_lower);
}

fs::path home_dir()
{
    if (fs::path home = platform_home_dir(); !home.empty())
    {
        return home;
    }
    std::error_code ec;
    return fs::current_path(ec);
}

bool is_dir(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

const std::string& current_patterns(const FileDialog& d)
{
    static const std::string all = "*";
    return d.filters.empty() ? all : d.filters[static_cast<std::size_t>(d.filter)].patterns;
}

void navigate(FileDialog& d, const fs::path& target)
{
    std::error_code ec;
    fs::path p = fs::absolute(target, ec).lexically_normal();
    if (!p.has_filename() && p != p.root_path())
    {
        p = p.parent_path(); // "/home/x/" -> "/home/x"
    }
    d.dir = p;
    d.location = p.string();
    d.selected.clear();
    d.confirm_path.clear();
    d.error.clear();
    d.entries = file_dialog_list(p, d.show_hidden, d.error);
    d.unreadable = !d.error.empty();
    file_dialog_sort(d.entries, d.sort, d.ascending);
}

void add_recent(const std::string& dir)
{
    std::erase(g_recent_dirs, dir);
    g_recent_dirs.insert(g_recent_dirs.begin(), dir);
    if (g_recent_dirs.size() > max_recent)
    {
        g_recent_dirs.resize(max_recent);
    }
}

std::string size_text(std::uintmax_t n)
{
    if (n < 1024)
    {
        return std::format("{} B", n);
    }
    const char* units[] = {"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n) / 1024.0;
    int u = 0;
    for (; v >= 1024.0 && u < 3; ++u)
    {
        v /= 1024.0;
    }
    return std::format("{:.1f} {}", v, units[u]);
}

std::string time_text(fs::file_time_type t)
{
    try
    {
        const auto sys = std::chrono::floor<std::chrono::minutes>(fs::file_time_type::clock::to_sys(t));
        return std::format("{:%Y-%m-%d %H:%M}", to_local_time(sys));
    }
    catch (const std::exception&)
    {
        return {};
    }
}

bool visible_entry(const FileDialog& d, const FileEntry& e)
{
    if (e.is_dir)
    {
        return true;
    }
    return d.mode != FileDialogMode::SelectFolder && file_filter_match(current_patterns(d), e.name);
}

// A pasted path as other programs hand it over: "C:\\x y" with quotes (Explorer's Copy as path),
// a trailing newline from a terminal.
std::string clean_path(std::string_view text)
{
    text = trim(text);
    while (!text.empty() && (text.front() == '"' || text.front() == '\''))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == '"' || text.back() == '\''))
    {
        text.remove_suffix(1);
    }
    text = trim(text);
    if (text == "~") // GTK and macOS: ~ is the home folder
    {
        return home_dir().string();
    }
    if (text.starts_with("~/"))
    {
        return (home_dir() / std::string(text.substr(2))).string();
    }
    return std::string(text);
}

// OK / Enter / double click. Returns the chosen paths, or navigates / reports an error.
std::vector<std::string> accept(FileDialog& d)
{
    const auto full = [&](const std::string& name) { return (d.dir / name).lexically_normal(); };
    if (d.mode == FileDialogMode::SelectFolder)
    {
        const bool dir_selected = d.selected.size() == 1 && is_dir(full(d.selected.front()));
        return {dir_selected ? full(d.selected.front()).string() : d.dir.string()};
    }
    d.file_name = clean_path(d.file_name); // a full path pasted into Name, as Explorer and GTK take it
    if (d.selected.size() == 1 && is_dir(full(d.selected.front())))
    {
        navigate(d, full(d.selected.front()));
        return {};
    }
    if (d.mode == FileDialogMode::Save)
    {
        if (d.file_name.empty())
        {
            return {};
        }
        if (const fs::path typed = full(d.file_name); is_dir(typed)) // a folder, before the type's extension is added
        {
            navigate(d, typed);
            return {};
        }
        const fs::path p = file_dialog_save_path(d.dir, d.file_name, current_patterns(d)).lexically_normal();
        std::error_code ec;
        if (!is_dir(p.parent_path()))
        {
            d.error = std::format("Folder {} does not exist", p.parent_path().string());
            return {};
        }
        if (fs::exists(p, ec) && d.confirm_path != p.string())
        {
            d.confirm_path = p.string();
            return {};
        }
        return {p.string()};
    }
    std::vector<std::string> out;
    if (d.selected.empty() && !d.file_name.empty())
    {
        const fs::path p = full(d.file_name);
        if (is_dir(p))
        {
            navigate(d, p);
            return {};
        }
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))
        {
            d.error = std::format("{} not found", p.string());
            return {};
        }
        out.push_back(p.string());
    }
    for (const auto& name : d.selected)
    {
        if (!is_dir(full(name)))
        {
            out.push_back(full(name).string());
        }
    }
    return out;
}

void finish(FileDialog& d, const std::vector<std::string>& result)
{
    add_recent(d.mode == FileDialogMode::SelectFolder ? result.front() : d.dir.string());
    d.visible = false;
    ImGui::CloseCurrentPopup();
}

void place(const char* label, const fs::path& p, FileDialog& d, std::optional<fs::path>& go)
{
    if (ImGui::Selectable(label, d.dir == p))
    {
        go = p;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        ImGui::SetTooltip("%s", p.string().c_str());
    }
}

void draw_places(FileDialog& d, std::optional<fs::path>& go)
{
    const fs::path home = home_dir();
    ImGui::SeparatorText("Places");
    ImGui::PushID("places");
    place("Home", home, d, go);
    if (is_dir(home / "Documents"))
    {
        place("Documents", home / "Documents", d, go);
    }
    for (const fs::path& root : platform_roots())
    {
        place(root == "/" ? "File System" : root.string().c_str(), root, d, go);
    }
    ImGui::PopID();
    if (!g_recent_dirs.empty())
    {
        ImGui::SeparatorText("Recent");
        for (std::size_t i = 0; i < g_recent_dirs.size(); ++i)
        {
            const fs::path p = g_recent_dirs[i];
            const std::string label = std::format("{}##recent{}", p.has_filename() ? p.filename().string() : p.string(), i);
            place(label.c_str(), p, d, go);
        }
    }
}

// Enter in the address bar: a folder is entered, a file (not when choosing a folder) is chosen
// at once, as in Explorer and the GTK chooser.
void go_location(FileDialog& d, std::optional<fs::path>& go, bool& accept_now)
{
    fs::path p = clean_path(d.location);
    if (p.empty()) // cleared and entered: back to the shown folder
    {
        d.location = d.dir.string();
        d.error.clear();
        return;
    }
    if (p.is_relative())
    {
        p = d.dir / p; // "sub" or "../x": from the shown folder, not the process's
    }
    if (is_dir(p))
    {
        go = p;
        return;
    }
    if (d.mode == FileDialogMode::SelectFolder)
    {
        d.error = std::format("{} is not a folder", p.string());
        return;
    }
    std::error_code ec;
    const bool exists = fs::is_regular_file(p, ec);
    if (exists || (d.mode == FileDialogMode::Save && is_dir(p.parent_path())))
    {
        navigate(d, p.parent_path()); // Save: a new file's path puts its name in the Name field
        d.file_name = p.filename().string();
        d.selected = exists ? std::vector<std::string>{d.file_name} : std::vector<std::string>{};
        accept_now = exists;
        return;
    }
    d.error = std::format("{} not found", p.string());
}

// Address bar: the breadcrumb buttons, or, after a click on their empty end or Ctrl+L, a text
// field with the whole path selected so it can be copied, cut or replaced by a pasted one; a
// typed '/' / '~' or Ctrl+V with nothing focused starts it with that text. Enter goes there, Esc
// or a click elsewhere restores the breadcrumbs.
void draw_breadcrumbs(FileDialog& d, std::optional<fs::path>& go, bool& accept_now)
{
    if (icon_button("##up", Icon::GoUp))
    {
        go = d.dir.parent_path();
    }
    ImGui::SetItemTooltip("Up");
    ImGui::SameLine();
    if (icon_button("##home", Icon::GoHome))
    {
        go = home_dir();
    }
    ImGui::SetItemTooltip("Home");
    ImGui::SameLine();
    // Ctrl+L (GTK, Explorer), Alt+D (Explorer), Ctrl+Shift+G (macOS "Go to Folder" is Cmd+Shift+G:
    // Cmd works too there, as the text fields' Cmd+C / Cmd+V do; ImGui does not swap Ctrl for Cmd
    // in chords).
    ImGuiIO& io = ImGui::GetIO();
    const auto chord = [&](ImGuiKeyChord mods_and_key)
    {
        return ImGui::Shortcut(ImGuiMod_Ctrl | mods_and_key)
               || (io.ConfigMacOSXBehaviors && ImGui::Shortcut(ImGuiMod_Super | mods_and_key));
    };
    if (chord(ImGuiKey_L) || ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_D) || chord(ImGuiMod_Shift | ImGuiKey_G))
    {
        d.edit_location = d.focus_location = true;
    }
    // With nothing focused, a typed "/" or "~" starts a path (GTK, macOS) and Ctrl+V puts the
    // clipboard's path in the field (macOS "Go to Folder" with a pasted path); Enter then goes.
    std::string prefill;
    if (!d.edit_location && !io.WantTextInput && !ImGui::IsAnyItemActive())
    {
        ImVector<ImWchar>& queue = io.InputQueueCharacters;
        for (int i = 0; i < queue.Size; ++i)
        {
            if (queue[i] == '/' || queue[i] == '~') // this and what was typed after it in the frame
            {
                char utf8[5];
                for (; i < queue.Size; ++i)
                {
                    prefill.append(utf8, static_cast<std::size_t>(ImTextCharToUtf8(utf8, queue[i])));
                }
                queue.resize(0); // not the list's '/' (vim search)
            }
        }
        if (prefill.empty() && chord(ImGuiKey_V))
        {
            const char* clip = ImGui::GetClipboardText();
            prefill = clip != nullptr ? clean_path(clip) : "";
            if (prefill.contains('\n')) // not a path
            {
                prefill.clear();
            }
        }
        if (!prefill.empty())
        {
            d.location = prefill;
            d.edit_location = d.focus_location = true;
        }
    }
    // A click on the empty end of the row (where it was last frame): the field drawn now takes
    // this very click, so it is active with everything selected before the frame ends. Handing
    // the focus over on the next frame lost the first keys at the 4 Hz idle frame rate.
    const ImVec2 mouse = ImGui::GetMousePos();
    if (!d.edit_location && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()
        && mouse.x >= d.address_rest[0] && mouse.y >= d.address_rest[1] && mouse.x < d.address_rest[2]
        && mouse.y < d.address_rest[3])
    {
        d.edit_location = true;
    }
    if (d.edit_location)
    {
        if (d.focus_location)
        {
            // Activated in this frame, not SetKeyboardFocusHere's two to three frames later: at
            // the idle frame rate a Ctrl+V right after Ctrl+L would otherwise arrive before the
            // field is active and be lost. The field reads these two like a nav activation.
            ImGuiContext& g = *ImGui::GetCurrentContext();
            g.NavActivateId = ImGui::GetID("##location");
            g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
            d.focus_location = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool entered = ImGui::InputTextWithHint("##location", "Folder or file path", &d.location,
                                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (!prefill.empty()) // typed or pasted: the caret goes after it instead of selecting it
        {
            if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID()); state != nullptr)
            {
                state->ReloadUserBufAndMoveToEnd();
            }
        }
        if (entered)
        {
            go_location(d, go, accept_now);
        }
        if (ImGui::IsItemDeactivated()) // Enter, Esc or a click elsewhere
        {
            d.edit_location = false;
            if (d.error.empty())
            {
                d.location = d.dir.string(); // a rejected path stays for the next Ctrl+L
            }
        }
        return;
    }
    fs::path acc;
    int i = 0;
    for (const auto& part : d.dir)
    {
        acc /= part;
        const std::string label = part.string();
        if (label.empty())
        {
            continue;
        }
        ImGui::PushID(i++);
        same_line_or_wrap(button_width(label.c_str()));
        if (ImGui::Button(label.c_str()))
        {
            go = acc;
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    const float rest = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    ImGui::InvisibleButton("##address", ImVec2(rest, ImGui::GetFrameHeight()));
    d.address_rest[0] = ImGui::GetItemRectMin().x;
    d.address_rest[1] = ImGui::GetItemRectMin().y;
    d.address_rest[2] = ImGui::GetItemRectMax().x;
    d.address_rest[3] = ImGui::GetItemRectMax().y;
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        ImGui::SetTooltip("Click to type or paste a path (Ctrl+L)");
    }
}

void draw_list(FileDialog& d, float height, std::optional<fs::path>& go, bool& accept_now)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##files", 3, flags, ImVec2(0.0f, height)))
    {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            d.sort = static_cast<FileSortColumn>(specs->Specs[0].ColumnIndex);
            d.ascending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
            file_dialog_sort(d.entries, d.sort, d.ascending);
        }
        specs->SpecsDirty = false;
    }

    std::vector<int> rows;
    int cur = -1; // row of the single selected entry, for the vim motions
    for (int i = 0; i < static_cast<int>(d.entries.size()); ++i)
    {
        if (visible_entry(d, d.entries[static_cast<std::size_t>(i)]))
        {
            if (d.selected.size() == 1 && d.selected.front() == d.entries[static_cast<std::size_t>(i)].name)
            {
                cur = static_cast<int>(rows.size());
            }
            rows.push_back(i);
        }
    }
    int h = 0;
    const int page = static_cast<int>(height / ImGui::GetTextLineHeightWithSpacing());
    bool unused_search = false; // '/' is the address bar's, taken before the list sees it
    const bool moved = vim_nav(d.vim, cur, static_cast<int>(rows.size()), page, unused_search, h);
    const FileEntry* cur_entry = cur >= 0 ? &d.entries[static_cast<std::size_t>(rows[static_cast<std::size_t>(cur)])] : nullptr;
    if (moved)
    {
        d.selected = {cur_entry->name};
        if (!cur_entry->is_dir)
        {
            d.file_name = cur_entry->name;
        }
        d.confirm_path.clear();
        d.error.clear();
    }
    if (h < 0)
    {
        go = d.dir.parent_path();
    }
    else if (h > 0 && cur_entry != nullptr && cur_entry->is_dir)
    {
        go = d.dir / cur_entry->name;
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    if (moved)
    {
        clipper.IncludeItemByIndex(cur);
    }
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const FileEntry& e = d.entries[static_cast<std::size_t>(rows[static_cast<std::size_t>(r)])];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(r);
            icon_image(e.is_dir ? Icon::Folder : Icon::File);
            ImGui::SameLine();
            const bool selected = std::ranges::contains(d.selected, e.name);
            if (moved && r == cur)
            {
                ImGui::SetScrollHereY();
                ImGui::SetKeyboardFocusHere(); // Enter on the nav cursor then opens this row
            }
            if (ImGui::Selectable(e.name.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                d.confirm_path.clear();
                d.error.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)
                    || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
                {
                    if (e.is_dir)
                    {
                        go = d.dir / e.name;
                    }
                    else
                    {
                        d.selected = {e.name};
                        accept_now = true;
                    }
                }
                else if (ImGui::GetIO().KeyCtrl && d.mode == FileDialogMode::OpenMultiple && !e.is_dir)
                {
                    if (selected)
                    {
                        std::erase(d.selected, e.name);
                    }
                    else
                    {
                        std::erase_if(d.selected, [&](const std::string& n) { return is_dir(d.dir / n); });
                        d.selected.push_back(e.name);
                    }
                }
                else
                {
                    d.selected = {e.name};
                    if (!e.is_dir)
                    {
                        d.file_name = e.name;
                    }
                }
            }
            vim_yank_table_row({{.label = "Name", .text = e.name},
                                {.label = "Path", .text = (d.dir / e.name).string()},
                                {.label = "Size", .text = e.is_dir ? std::string() : size_text(e.size)},
                                {.label = "Modified", .text = time_text(e.modified)}},
                               true);
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (!e.is_dir)
            {
                ImGui::TextUnformatted(size_text(e.size).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(time_text(e.modified).c_str());
        }
    }
    if (rows.empty())
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled(d.unreadable ? "Cannot read folder" : "Empty folder"); // error may be a validation message
    }
    vim_yank_end_table();
}

} // namespace

bool file_filter_match(std::string_view patterns, std::string_view name)
{
    bool any = false;
    for (const auto part : patterns | std::views::split(' '))
    {
        const std::string_view p(part.begin(), part.end());
        if (p.empty())
        {
            continue;
        }
        any = true;
        if (glob_match(p, name))
        {
            return true;
        }
    }
    return !any;
}

std::vector<FileEntry> file_dialog_list(const fs::path& dir, bool show_hidden, std::string& error)
{
    std::vector<FileEntry> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        error = std::format("Cannot open {}: {}", dir.string(), ec.message());
        return out;
    }
    for (; it != fs::directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            error = std::format("Cannot read {}: {}", dir.string(), ec.message());
            break;
        }
        FileEntry e{.name = it->path().filename().string()};
        if (!show_hidden && e.name.starts_with('.'))
        {
            continue;
        }
        std::error_code entry_ec; // a broken symlink or unreadable entry still gets listed
        e.is_dir = it->is_directory(entry_ec);
        if (!e.is_dir)
        {
            e.size = it->file_size(entry_ec);
            if (entry_ec)
            {
                e.size = 0;
            }
        }
        e.modified = it->last_write_time(entry_ec);
        out.push_back(std::move(e));
    }
    return out;
}

void file_dialog_sort(std::vector<FileEntry>& entries, FileSortColumn column, bool ascending)
{
    std::ranges::stable_sort(entries, [&](const FileEntry& a, const FileEntry& b) {
        if (a.is_dir != b.is_dir)
        {
            return a.is_dir;
        }
        const auto by_name = [&] { return ascending ? less_ci(a.name, b.name) : less_ci(b.name, a.name); };
        switch (column)
        {
        case FileSortColumn::Size:
            if (a.size != b.size)
            {
                return ascending ? a.size < b.size : a.size > b.size;
            }
            return by_name();
        case FileSortColumn::Modified:
            if (a.modified != b.modified)
            {
                return ascending ? a.modified < b.modified : a.modified > b.modified;
            }
            return by_name();
        case FileSortColumn::Name:
            break;
        }
        return by_name();
    });
}

fs::path file_dialog_save_path(const fs::path& dir, const std::string& name, std::string_view patterns)
{
    fs::path p = dir / name; // an absolute name replaces dir
    if (p.has_extension())
    {
        return p;
    }
    for (const auto part : patterns | std::views::split(' '))
    {
        const std::string_view pat(part.begin(), part.end());
        if (pat.starts_with("*.") && pat.find_first_of("*?", 2) == std::string_view::npos)
        {
            p += std::string(pat.substr(1));
            break;
        }
    }
    return p;
}

std::string file_dialog_retype(const std::string& name, std::string_view patterns)
{
    if (name.empty())
    {
        return name;
    }
    const fs::path typed = file_dialog_save_path({}, fs::path(name).replace_extension().string(), patterns);
    return typed.has_extension() ? typed.string() : name;
}

void file_dialog_open(FileDialog& d, FileDialogMode mode, std::string title, const std::string& start_path,
                      std::vector<FileFilter> filters)
{
    const bool show_hidden = d.show_hidden; // viewer preference survives reopening
    d = FileDialog{.mode = mode, .title = std::move(title), .filters = std::move(filters)};
    d.show_hidden = show_hidden;
    d.visible = true;
    d.open_request = true;

    fs::path dir = g_recent_dirs.empty() ? home_dir() : fs::path(g_recent_dirs.front());
    const fs::path start(start_path);
    if (!start_path.empty() && is_dir(start))
    {
        dir = start;
    }
    else if (!start_path.empty())
    {
        if (start.has_parent_path() && is_dir(start.parent_path()))
        {
            dir = start.parent_path();
        }
        if (mode == FileDialogMode::Save)
        {
            d.file_name = start.filename().string();
        }
    }
    navigate(d, dir);
}

std::vector<std::string> file_dialog_draw(FileDialog& d)
{
    std::vector<std::string> result;
    if (!d.visible)
    {
        return result;
    }
    const std::string id = d.title + "###file_dialog";
    if (d.open_request)
    {
        ImGui::OpenPopup(id.c_str());
        d.open_request = false;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 50.0f, em * 32.0f), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(id.c_str(), &open))
    {
        d.visible = false; // closed with the title bar button
        return result;
    }
    const bool editing = ImGui::IsAnyItemActive(); // Esc belongs to the text field then
    std::optional<fs::path> go;
    bool accept_now = false;

    draw_breadcrumbs(d, go, accept_now);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + style.ItemSpacing.y;
    const float body = std::max(ImGui::GetContentRegionAvail().y - footer, em * 4.0f);
    ImGui::BeginChild("##places", ImVec2(em * 10.0f, body), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    draw_places(d, go);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    draw_list(d, body, go, accept_now);
    ImGui::EndGroup();

    // Footer row 1: name + type.
    const bool has_filters = !d.filters.empty() && d.mode != FileDialogMode::SelectFolder;
    const float combo_w = has_filters ? em * 14.0f : 0.0f;
    if (d.mode == FileDialogMode::SelectFolder)
    {
        const std::string target =
            d.selected.size() == 1 && is_dir(d.dir / d.selected.front()) ? (d.dir / d.selected.front()).string() : d.dir.string();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Folder: %s", target.c_str());
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(has_filters ? -(combo_w + style.ItemSpacing.x) : -FLT_MIN);
        const std::string before = d.file_name;
        if (ImGui::InputText("##name", &d.file_name, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            accept_now = true;
        }
        if (d.file_name != before)
        {
            d.selected.clear(); // typed name wins over the list selection
            d.confirm_path.clear();
        }
    }
    if (has_filters)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::BeginCombo("##type", d.filters[static_cast<std::size_t>(d.filter)].name.c_str()))
        {
            for (int i = 0; i < static_cast<int>(d.filters.size()); ++i)
            {
                if (ImGui::Selectable(d.filters[static_cast<std::size_t>(i)].name.c_str(), i == d.filter))
                {
                    d.filter = i;
                    d.selected.clear();
                    if (d.mode == FileDialogMode::Save) // the type picks the format: trace.asc -> trace.pcapng
                    {
                        d.file_name = file_dialog_retype(d.file_name, d.filters[static_cast<std::size_t>(i)].patterns);
                        d.confirm_path.clear();
                    }
                }
            }
            ImGui::EndCombo();
        }
    }

    // Footer row 2: hidden toggle, message, buttons (or the replace question).
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (ImGui::Checkbox("Show hidden", &d.show_hidden))
    {
        go = d.dir;
    }
    const float bw = em * 6.0f;
    const float buttons = bw * 2.0f + style.ItemSpacing.x;
    const std::string& message = d.confirm_path.empty() ? d.error
                                                        : std::format("{} already exists. Replace it?",
                                                                      fs::path(d.confirm_path).filename().string());
    if (!message.empty())
    {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        const ImVec4 color = d.confirm_path.empty() ? ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error)) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushTextWrapPos(right - buttons - style.ItemSpacing.x);
        ImGui::TextColored(color, "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::SameLine(right - buttons);
    const char* ok_label = !d.confirm_path.empty()                   ? "Replace"
                           : d.mode == FileDialogMode::Save         ? "Save"
                           : d.mode == FileDialogMode::SelectFolder ? "Select"
                                                                    : "Open";
    if (ImGui::Button(ok_label, ImVec2(bw, 0.0f)))
    {
        accept_now = true;
    }
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(bw, 0.0f));

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        if (!editing)
        {
            if (d.confirm_path.empty())
            {
                cancel = true;
            }
            d.confirm_path.clear();
        }
        // The parent popup (setup / recording dialog) must not close on the same key press.
        ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##esc"), ImGuiInputFlags_LockThisFrame);
    }

    // Enter in the list (vim motions) when no field or button took it.
    if (!accept_now && !cancel && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()
        && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
    {
        accept_now = true;
    }
    if (go)
    {
        navigate(d, *go);
    }
    else if (accept_now)
    {
        d.error.clear();
        result = accept(d);
        if (!result.empty())
        {
            finish(d, result);
        }
    }
    if (cancel && d.visible)
    {
        d.visible = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return result;
}
