#include "ui/dbc_editor.h"

#include <algorithm>
#include <cfloat>
#include <format>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // ScrollToItem
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"

#include "core/fuzzy.h"
#include "core/log.h"
#include "core/setup.h"
#include "core/text.h"
#include "db/dbc/dbc_check.h"
#include "db/dbc/dbc_writer.h"
#include "ui/workspace_tabs.h"
#include "ui/vim_nav.h"

namespace
{

constexpr uint32_t extended_bit = 0x80000000u;
constexpr uint32_t no_selection = UINT32_MAX; // sel_id: bit 31 + every id bit, never a real message
const std::vector<FileFilter> dbc_filters = {{"DBC", "*.dbc"}, {"All Files", "*"}};

std::string file_name(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string id_label(uint32_t raw_id)
{
    const uint32_t id = raw_id & ~extended_bit;
    return (raw_id & extended_bit) != 0 ? std::format("0x{:08X}", id) : std::format("0x{:03X}", id);
}

// Best score of the query against the name and the id spelled "0x123", "123" (hex) and "291".
int message_score(std::string_view query, const CanDbMessage& m)
{
    const uint32_t id = m.raw_id & ~extended_bit;
    int best = fuzzy_score(query, m.name);
    for (const std::string& text : {std::format("0x{:X}", id), std::format("{:X}", id), std::format("{}", id)})
    {
        best = std::max(best, fuzzy_score(query, text));
    }
    return best;
}

int row_count(const DbcEditorState& s)
{
    return static_cast<int>(s.rows.size());
}

void set_mux(CanDbMessage& m, int index, int mode) // 0 none, 1 multiplexer, 2 multiplexed
{
    CanDbSignal& sig = m.signals[static_cast<std::size_t>(index)];
    sig.is_muxer = mode == 1;
    sig.is_muxed = mode == 2;
    if (mode == 1)
    {
        for (CanDbSignal& other : m.signals)
        {
            other.is_muxer = &other == &sig;
        }
        m.muxer = index;
    }
    else if (m.muxer == index)
    {
        m.muxer = -1;
    }
}

void draw_toolbar(App& app, DbcEditorState& s)
{
    if (ImGui::Button("Open..."))
    {
        file_dialog_open(s.dialog, FileDialogMode::Open, "Open DBC", s.path, dbc_filters);
    }
    ImGui::SameLine();
    if (ImGui::Button("From setup"))
    {
        ImGui::OpenPopup("from_setup");
    }
    if (ImGui::BeginPopup("from_setup"))
    {
        bool any = false;
        for (const SetupNetwork& net : app.setup.networks)
        {
            for (const auto& db : net.can_dbs)
            {
                any = true;
                ImGui::PushID(db.get());
                if (ImGui::MenuItem(std::format("{}: {}", net.name, file_name(db->path)).c_str()))
                {
                    dbc_editor_load(s, *db, db->path); // a copy: the setup's database is never edited live
                }
                ImGui::PopID();
            }
        }
        if (!any)
        {
            ImGui::TextDisabled("No DBC in the setup");
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.path.empty());
    if (ImGui::Button("Save"))
    {
        static_cast<void>(dbc_editor_save(app, s, s.path));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save As..."))
    {
        file_dialog_open(s.dialog, FileDialogMode::Save, "Save DBC As", s.path.empty() ? "database.dbc" : s.path, dbc_filters);
    }
    ImGui::SameLine();
    if (ImGui::Button("Check"))
    {
        dbc_editor_check(s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add message"))
    {
        static_cast<void>(dbc_editor_add_message(s));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(dbc_editor_selected_message(s) == nullptr);
    if (ImGui::Button("Add signal"))
    {
        static_cast<void>(dbc_editor_add_signal(s));
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
    {
        static_cast<void>(dbc_editor_delete_selected(s));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", s.path.empty() ? "unsaved" : file_name(s.path).c_str(), s.dirty ? " *" : "");

    for (const auto& path : file_dialog_draw(s.dialog))
    {
        if (s.dialog.mode == FileDialogMode::Save)
        {
            static_cast<void>(dbc_editor_save(app, s, path));
        }
        else
        {
            static_cast<void>(dbc_editor_open_file(s, path));
        }
    }
}

void draw_list(DbcEditorState& s)
{
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const int n = row_count(s);
    int sel = s.selected;
    int h_delta = 0;
    if (vim_nav(s.vim, sel, n, static_cast<int>(ImGui::GetWindowHeight() / row_h), s.focus_search, h_delta))
    {
        dbc_editor_select(s, sel);
        s.scroll_to_selected = true;
    }
    if (n == 0)
    {
        ImGui::TextDisabled(s.db.messages.empty() ? "No messages" : "No match");
        return;
    }
    if (s.selected < 0)
    {
        s.scroll_to_selected = false;
    }
    ImGuiListClipper clipper;
    clipper.Begin(n, row_h);
    if (s.scroll_to_selected)
    {
        clipper.IncludeItemByIndex(s.selected); // submitted even off-screen, so it can be scrolled to
    }
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            ImGui::PushID(row);
            if (ImGui::Selectable(s.rows[static_cast<std::size_t>(row)].label.c_str(), row == s.selected))
            {
                dbc_editor_select(s, row);
            }
            ImGui::PopID();
            if (row == s.selected && std::exchange(s.scroll_to_selected, false))
            {
                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            }
        }
    }
}

void draw_message_form(DbcEditorState& s, CanDbMessage& m)
{
    const float fs = ImGui::GetFontSize();
    bool changed = false;
    ImGui::SeparatorText("Message");
    ImGui::PushItemWidth(fs * 16.0f);
    changed |= ImGui::InputText("Name", &m.name);
    if (s.id_text_for != m.raw_id)
    {
        s.id_text = std::format("{:X}", m.raw_id & ~extended_bit);
        s.id_text_for = m.raw_id;
        s.id_conflict = false;
    }
    bool extended = (m.raw_id & extended_bit) != 0;
    ImGui::InputText("Id (hex)", &s.id_text, ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::IsItemDeactivatedAfterEdit()) // Enter or leaving the field; not on every prefix typed
    {
        uint32_t id = 0;
        const bool valid = parse_number(s.id_text, id, 16) && id <= (extended ? 0x1FFFFFFFu : 0x7FFu);
        const uint32_t raw = id | (extended ? extended_bit : 0u);
        // The message object keeps its address across the re-key (map node handle), so `m` stays valid.
        s.id_conflict = !valid || !dbc_editor_set_message_id(s, m.raw_id, raw);
        if (!s.id_conflict)
        {
            s.id_text_for = raw;
        }
    }
    if (s.id_conflict)
    {
        ImGui::SameLine();
        ImGui::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "invalid or in use");
    }
    if (ImGui::Checkbox("Extended", &extended))
    {
        const uint32_t raw = (m.raw_id & ~extended_bit) | (extended ? extended_bit : 0u);
        s.id_conflict = !dbc_editor_set_message_id(s, m.raw_id, raw);
        s.id_text_for = no_selection; // re-derive the text (also on a conflict: nothing changed)
    }
    int dlc = m.dlc;
    if (ImGui::InputInt("DLC", &dlc))
    {
        m.dlc = static_cast<uint8_t>(std::clamp(dlc, 1, 64));
        changed = true;
    }
    changed |= ImGui::InputText("Sender", &m.sender);
    ImGui::PopItemWidth();
    changed |= ImGui::InputTextMultiline("Comment", &m.comment, {-FLT_MIN, fs * 5.0f});
    ImGui::TextDisabled("%zu signals", m.signals.size());
    if (changed)
    {
        dbc_editor_mark(s);
    }
}

void draw_signal_form(DbcEditorState& s, CanDbMessage& m, int index)
{
    CanDbSignal& sig = m.signals[static_cast<std::size_t>(index)];
    const float fs = ImGui::GetFontSize();
    bool changed = false;
    ImGui::SeparatorText(std::format("Signal of {}", m.name).c_str());
    ImGui::PushItemWidth(fs * 16.0f);
    changed |= ImGui::InputText("Name", &sig.name);
    int start = static_cast<int>(dbc_start_bit(sig)); // DBC numbering, not the parser's Motorola index
    if (ImGui::InputInt("Start bit", &start))
    {
        dbc_set_start_bit(sig, static_cast<unsigned>(std::clamp(start, 0, 511)));
        changed = true;
    }
    int length = sig.length;
    if (ImGui::InputInt("Length", &length))
    {
        sig.length = static_cast<uint16_t>(std::clamp(length, 1, 64));
        changed = true;
    }
    int order = sig.big_endian ? 1 : 0;
    if (ImGui::Combo("Byte order", &order, "Intel (little endian)\0Motorola (big endian)\0"))
    {
        const unsigned dbc_bit = dbc_start_bit(sig);
        sig.big_endian = order == 1;
        dbc_set_start_bit(sig, dbc_bit); // the shown start bit stays
        changed = true;
    }
    bool is_signed = !sig.is_unsigned;
    if (ImGui::Checkbox("Signed", &is_signed))
    {
        sig.is_unsigned = !is_signed;
        changed = true;
    }
    int value_type = static_cast<int>(sig.value_type);
    if (ImGui::Combo("Value type", &value_type, "Integer\0Float32\0Float64\0"))
    {
        sig.value_type = static_cast<SignalValueType>(value_type);
        changed = true;
    }
    changed |= ImGui::InputDouble("Factor", &sig.factor, 0.0, 0.0, "%g");
    changed |= ImGui::InputDouble("Offset", &sig.offset, 0.0, 0.0, "%g");
    changed |= ImGui::InputDouble("Min", &sig.min, 0.0, 0.0, "%g");
    changed |= ImGui::InputDouble("Max", &sig.max, 0.0, 0.0, "%g");
    changed |= ImGui::InputText("Unit", &sig.unit);
    int mux = sig.is_muxer ? 1 : (sig.is_muxed ? 2 : 0);
    if (ImGui::Combo("Multiplex", &mux, "None\0Multiplexer\0Multiplexed\0"))
    {
        set_mux(m, index, mux);
        changed = true;
    }
    if (sig.is_muxed)
    {
        int mux_value = static_cast<int>(sig.mux_value);
        if (ImGui::InputInt("Mux value", &mux_value))
        {
            sig.mux_value = static_cast<uint32_t>(std::max(mux_value, 0));
            changed = true;
        }
    }
    ImGui::PopItemWidth();
    changed |= ImGui::InputTextMultiline("Comment", &sig.comment, {-FLT_MIN, fs * 4.0f});

    ImGui::SeparatorText("Value table");
    if (ImGui::BeginTable("value_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, fs * 7.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
        ImGui::TableHeadersRow();
        bool has_remove = false;
        uint64_t remove_value = 0;
        for (auto& [value, name] : sig.value_table)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(std::format("{}", value).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(&name); // map nodes keep their address
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::InputText("##name", &name);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x"))
            {
                has_remove = true;
                remove_value = value;
            }
            vim_yank_table_row({{.label = "Value", .text = std::format("{}", value)}, {.label = "Name", .text = name}});
            ImGui::PopID();
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputScalar("##vt_value", ImGuiDataType_U64, &s.vt_value);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##vt_name", "new value name", &s.vt_name);
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Add"))
        {
            sig.value_table[s.vt_value] = s.vt_name;
            s.vt_name.clear();
            ++s.vt_value;
            changed = true;
        }
        vim_yank_end_table();
        if (has_remove)
        {
            sig.value_table.erase(remove_value);
            changed = true;
        }
    }
    if (changed)
    {
        dbc_editor_mark(s);
    }
}

void draw_form(DbcEditorState& s)
{
    CanDbMessage* m = dbc_editor_selected_message(s);
    if (m == nullptr)
    {
        ImGui::TextDisabled("Select a message or a signal");
        return;
    }
    if (dbc_editor_selected_signal(s) != nullptr)
    {
        draw_signal_form(s, *m, s.rows[static_cast<std::size_t>(s.selected)].signal);
    }
    else
    {
        draw_message_form(s, *m);
    }
}

void draw_problems(const DbcEditorState& s)
{
    const int parse = static_cast<int>(s.parse_errors.size());
    const int n = parse + static_cast<int>(s.check_errors.size());
    ImGui::TextUnformatted(std::format("Problems ({})", n).c_str());
    ImGui::Separator();
    if (n == 0)
    {
        ImGui::TextDisabled("No problems");
        return;
    }
    ImGuiListClipper clipper;
    clipper.Begin(n);
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const DbcError& e = i < parse ? s.parse_errors[static_cast<std::size_t>(i)] : s.check_errors[static_cast<std::size_t>(i - parse)];
            if (e.line > 0)
            {
                ImGui::TextUnformatted(std::format("line {}: {}", e.line, e.message).c_str());
            }
            else
            {
                ImGui::TextUnformatted(e.message.c_str());
            }
        }
    }
}

} // namespace

void dbc_editor_load(DbcEditorState& s, const CanDb& db, std::string path)
{
    s.db = db;
    s.db.path = path;
    s.path = std::move(path);
    s.dirty = false;
    s.parse_errors.clear();
    s.check_errors = dbc_check(s.db);
    s.sel_id = no_selection;
    s.sel_signal = -1;
    s.id_text_for = no_selection;
    dbc_editor_rebuild_rows(s);
}

bool dbc_editor_open_file(DbcEditorState& s, const std::filesystem::path& path)
{
    CanDb fresh;
    std::vector<DbcError> errors;
    const bool ok = dbc_parse_file(path, fresh, &errors);
    dbc_editor_load(s, fresh, path.string()); // whatever parsed, so a broken file can still be fixed here
    s.parse_errors = std::move(errors);
    if (!ok)
    {
        log_warning(std::format("DBC Editor: {} has {} syntax problem(s)", path.string(), s.parse_errors.size()));
    }
    return ok;
}

void dbc_editor_check(DbcEditorState& s)
{
    s.parse_errors.clear();
    if (!s.path.empty())
    {
        CanDb scratch;
        static_cast<void>(dbc_parse_file(s.path, scratch, &s.parse_errors));
    }
    s.check_errors = dbc_check(s.db);
}

void dbc_editor_mark(DbcEditorState& s)
{
    s.dirty = true;
    s.check_errors = dbc_check(s.db);
    dbc_editor_rebuild_rows(s); // names / ids in the labels may have changed
}

void dbc_editor_rebuild_rows(DbcEditorState& s)
{
    struct Group
    {
        int score = 0;
        std::vector<DbcEditorRow> rows;
    };
    const bool all = s.query.empty();
    const auto by_score = [](const auto& a, const auto& b) { return a.score > b.score; };
    std::vector<Group> groups;
    for (const auto& [raw_id, m] : s.db.messages)
    {
        Group g{.score = all ? 0 : message_score(s.query, m)};
        g.rows.push_back({.raw_id = raw_id, .signal = -1, .score = g.score,
                          .label = std::format("{}  {}  [{}]", id_label(raw_id), m.name, m.dlc)});
        for (int i = 0; i < static_cast<int>(m.signals.size()); ++i)
        {
            const CanDbSignal& sig = m.signals[static_cast<std::size_t>(i)];
            int score = 0;
            if (!all)
            {
                score = fuzzy_score(s.query, m.name + '.' + sig.name);
                if (score < 0)
                {
                    continue;
                }
            }
            g.score = std::max(g.score, score);
            g.rows.push_back({.raw_id = raw_id, .signal = i, .score = score, .label = "    " + sig.name});
        }
        if (g.score < 0)
        {
            continue; // neither the message nor one of its signals matched
        }
        g.rows.front().score = g.score;
        if (!all)
        {
            std::stable_sort(g.rows.begin() + 1, g.rows.end(), by_score);
        }
        groups.push_back(std::move(g));
    }
    if (!all)
    {
        std::stable_sort(groups.begin(), groups.end(), by_score); // equal scores keep id order
    }
    s.rows.clear();
    for (Group& g : groups)
    {
        s.rows.insert(s.rows.end(), std::move_iterator(g.rows.begin()), std::move_iterator(g.rows.end()));
    }
    s.applied_query = s.query;
    s.selected = -1;
    for (int i = 0; i < row_count(s); ++i)
    {
        const DbcEditorRow& r = s.rows[static_cast<std::size_t>(i)];
        if (r.raw_id == s.sel_id && r.signal == s.sel_signal)
        {
            s.selected = i;
            break;
        }
    }
}

void dbc_editor_select(DbcEditorState& s, int row)
{
    if (row < 0 || row >= row_count(s))
    {
        s.selected = -1;
        s.sel_id = no_selection;
        s.sel_signal = -1;
        return;
    }
    s.selected = row;
    s.sel_id = s.rows[static_cast<std::size_t>(row)].raw_id;
    s.sel_signal = s.rows[static_cast<std::size_t>(row)].signal;
}

CanDbMessage* dbc_editor_selected_message(DbcEditorState& s)
{
    if (s.selected < 0 || s.selected >= row_count(s))
    {
        return nullptr;
    }
    return can_db_find_message(s.db, s.rows[static_cast<std::size_t>(s.selected)].raw_id);
}

CanDbSignal* dbc_editor_selected_signal(DbcEditorState& s)
{
    CanDbMessage* m = dbc_editor_selected_message(s);
    if (m == nullptr)
    {
        return nullptr;
    }
    const int i = s.rows[static_cast<std::size_t>(s.selected)].signal;
    return i >= 0 && i < static_cast<int>(m->signals.size()) ? &m->signals[static_cast<std::size_t>(i)] : nullptr;
}

bool dbc_editor_set_message_id(DbcEditorState& s, uint32_t old_raw, uint32_t new_raw)
{
    if (old_raw == new_raw)
    {
        return true;
    }
    const auto it = s.db.messages.find(old_raw);
    if (it == s.db.messages.end() || s.db.messages.contains(new_raw))
    {
        return false;
    }
    auto node = s.db.messages.extract(it); // the CanDbMessage keeps its address
    node.key() = new_raw;
    node.mapped().raw_id = new_raw;
    s.db.messages.insert(std::move(node));
    if (s.sel_id == old_raw)
    {
        s.sel_id = new_raw;
    }
    dbc_editor_mark(s);
    return true;
}

uint32_t dbc_editor_add_message(DbcEditorState& s)
{
    uint32_t id = 0;
    while (s.db.messages.contains(id))
    {
        ++id;
    }
    s.db.messages[id] = {.name = "NewMessage", .raw_id = id, .dlc = 8};
    s.query.clear(); // so the new message is listed
    s.sel_id = id;
    s.sel_signal = -1;
    s.scroll_to_selected = true;
    dbc_editor_mark(s);
    return id;
}

bool dbc_editor_add_signal(DbcEditorState& s)
{
    CanDbMessage* m = dbc_editor_selected_message(s);
    if (m == nullptr)
    {
        return false;
    }
    m->signals.push_back({.name = "NewSignal", .length = 8, .is_unsigned = true});
    s.query.clear();
    s.sel_id = m->raw_id;
    s.sel_signal = static_cast<int>(m->signals.size()) - 1;
    s.scroll_to_selected = true;
    dbc_editor_mark(s);
    return true;
}

bool dbc_editor_delete_selected(DbcEditorState& s)
{
    CanDbMessage* m = dbc_editor_selected_message(s);
    if (m == nullptr)
    {
        return false;
    }
    const int sig = s.rows[static_cast<std::size_t>(s.selected)].signal;
    if (sig >= 0 && sig < static_cast<int>(m->signals.size()))
    {
        m->signals.erase(m->signals.begin() + sig);
        if (m->muxer == sig)
        {
            m->muxer = -1;
        }
        else if (m->muxer > sig)
        {
            --m->muxer;
        }
        s.sel_signal = -1; // the message stays selected
    }
    else
    {
        s.db.messages.erase(m->raw_id);
        s.sel_id = no_selection;
    }
    dbc_editor_mark(s);
    return true;
}

bool dbc_editor_save(App& app, DbcEditorState& s, const std::filesystem::path& path)
{
    std::string error;
    if (!dbc_write_file(s.db, path, &error))
    {
        log_error(std::format("DBC Editor: cannot write {}: {}", path.string(), error));
        return false;
    }
    s.path = path.string();
    s.db.path = s.path;
    s.dirty = false;
    // ponytail: paths are compared as stored; a setup that references the file through another spelling
    // (relative, symlink) is not reloaded. Upgrade: std::filesystem::equivalent.
    const bool referenced = std::ranges::any_of(app.setup.networks, [&](const SetupNetwork& net)
    {
        return std::ranges::any_of(net.can_dbs, [&](const auto& db) { return db->path == s.path; });
    });
    if (referenced)
    {
        std::vector<std::string> errors;
        if (!setup_reload_databases(app.setup, &errors))
        {
            for (const std::string& e : errors)
            {
                log_error(std::format("DBC Editor: reload after save: {}", e));
            }
        }
    }
    log_info(std::format("DBC Editor: saved {}", s.path));
    return true;
}

void draw_dbc_editor(App& app, DbcEditorState& s, const WorkspaceTab& tab)
{
    if (!s.open)
    {
        return;
    }
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 60.0f, ImGui::GetFontSize() * 40.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(workspace_window_name(tab, "DBC Editor").c_str(), &s.open))
    {
        ImGui::End();
        return;
    }
    const float fs = ImGui::GetFontSize();
    draw_toolbar(app, s);
    if (s.focus_search)
    {
        ImGui::SetKeyboardFocusHere();
        s.focus_search = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search messages / signals: name, 0x123, 291, Message.Signal (/)", &s.query);
    if (s.query != s.applied_query)
    {
        dbc_editor_rebuild_rows(s);
        s.scroll_to_selected = true;
    }
    const float problems_h = fs * 8.0f;
    if (ImGui::BeginChild("list", {fs * 24.0f, -problems_h}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
    {
        draw_list(s);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("form", {0.0f, -problems_h}, ImGuiChildFlags_Borders))
    {
        draw_form(s);
    }
    ImGui::EndChild();
    if (ImGui::BeginChild("problems", {0.0f, 0.0f}, ImGuiChildFlags_Borders))
    {
        draw_problems(s);
    }
    ImGui::EndChild();
    ImGui::End();
}
