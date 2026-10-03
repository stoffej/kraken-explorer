#include "ui/setup_dialog.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "db/sym/sym_parser.h"
#include "drivers/driver.h"
#include "ui/theme.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{

// Sorted unique values of the timings matching `keep`.
template <class Keep, class Value>
std::vector<unsigned> timing_values(const std::vector<CanTiming>& timings, Keep keep, Value value)
{
    std::vector<unsigned> out;
    for (const auto& t : timings)
    {
        if (keep(t))
        {
            out.push_back(value(t));
        }
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool contains(const std::vector<unsigned>& v, unsigned x)
{
    return std::ranges::find(v, x) != v.end();
}

void draw_can_setup_page(const IfaceInfo& info, SetupInterface& intf);
void draw_lin_setup_page(const IfaceInfo& info, const SetupNetwork& net, SetupInterface& intf,
                         LinFrameDefaultsState& fd);
void draw_lin_frame_defaults(const LinDb& db, SetupInterface& intf, LinFrameDefaultsState& fd);

// Combo over `values`; label via fmt(value). Returns true when the selection changed.
template <class Fmt>
bool value_combo(const char* id, unsigned& current, const std::vector<unsigned>& values, Fmt fmt)
{
    bool changed = false;
    if (ImGui::BeginCombo(id, values.empty() ? "" : fmt(current).c_str()))
    {
        for (unsigned v : values)
        {
            if (ImGui::Selectable(fmt(v).c_str(), v == current))
            {
                current = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string bitrate_str(unsigned v) { return std::to_string(v); }
std::string sample_point_str(unsigned sp) { return std::format("{:.1f}%", sp / 10.0); }

bool has(uint32_t caps, uint32_t bit) { return (caps & bit) != 0; }

void interface_header(const IfaceInfo& info, const SetupInterface& intf)
{
    ImGui::TextDisabled("Driver:");
    ImGui::SameLine(140);
    ImGui::TextUnformatted(intf.driver.c_str());
    ImGui::TextDisabled("Interface:");
    ImGui::SameLine(140);
    ImGui::TextUnformatted(intf.name.c_str());
    ImGui::TextDisabled("Interface Details:");
    ImGui::SameLine(140);
    ImGui::TextWrapped("%s", info.details.c_str());
    ImGui::Separator();
}

std::string file_name(const std::string& path) { return std::filesystem::path(path).filename().string(); }
std::string directory(const std::string& path) { return std::filesystem::path(path).parent_path().string(); }

// Hex field for a Div+Seg1+Seg2 value; clamped when editing ends.
void custom_field(const char* id, uint32_t& v, uint32_t (*clamp)(uint32_t) noexcept)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputScalar(id, ImGuiDataType_U32, &v, nullptr, nullptr, "%06X", ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        v = clamp(v);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Div+Seg1+Seg2 (Hex)");
}

// Opens the file dialog; draw_setup_dialog hands the chosen files to add_databases.
void choose_databases(SetupDialogState& s, int net_index)
{
    const SetupNetwork& net = s.work.networks[static_cast<std::size_t>(net_index)];
    s.db_net = net_index;
    s.db_lin = std::ranges::any_of(net.interfaces, [](const SetupInterface& i) { return i.bus_type == BusType::LIN; });
    if (s.db_lin)
    {
        file_dialog_open(s.db_dialog, FileDialogMode::OpenMultiple, "Load LIN Databases", "",
                         {{"LIN Description Files (*.ldf)", "*.ldf"}, {"All Files", "*"}});
    }
    else
    {
        file_dialog_open(s.db_dialog, FileDialogMode::OpenMultiple, "Load CAN Databases", "",
                         {{"CAN Databases (*.dbc *.dbf *.sym)", "*.dbc *.dbf *.sym"}, {"All Files", "*"}});
    }
}

void add_databases(SetupDialogState& s, SetupNetwork& net, const std::vector<std::string>& files)
{
    const bool lin = s.db_lin;
    for (const auto& path : files)
    {
        if (lin)
        {
            auto db = std::make_shared<LinDb>();
            if (!lin_db_load(*db, path))
            {
                s.message = std::format("Failed to load LDF file {}: {}", path, db->last_error);
                continue;
            }
            // A duplicate is reloaded: the fresh copy replaces the old one.
            std::erase_if(net.lin_dbs, [&](const auto& d) { return d->path == path; });
            // Configure LIN interfaces that have no LDF yet, so starting works without visiting each page.
            for (auto& intf : net.interfaces)
            {
                if (intf.bus_type == BusType::LIN && intf.lin_ldf_path.empty())
                {
                    lin_setup_apply_ldf(*db, intf);
                }
            }
            net.lin_dbs.push_back(std::move(db));
            continue;
        }
        CanDb fresh;
        if (!can_db_parse_file(path, fresh))
        {
            s.message = std::format("Failed to load DBC file {}", path);
            continue;
        }
        fresh.path = path;
        const auto dup = std::ranges::find_if(net.can_dbs, [&](const auto& d) { return d->path == path; });
        if (dup != net.can_dbs.end())
        {
            can_db_update_from(**dup, fresh); // in place: held CanDbMessage*/CanDbSignal* survive
        }
        else
        {
            net.can_dbs.push_back(std::make_shared<CanDb>(std::move(fresh)));
        }
    }
    setup_rebuild_cache(s.work);
}

void select(SetupDialogState& s, SetupSel sel, int net, int item = -1)
{
    s.sel = sel;
    s.net = net;
    s.item = item;
    s.row = -1;
}

void draw_tree(App& app, SetupDialogState& s)
{
    enum class Action { None, AddInterface, DeleteInterface, AddDb, DeleteCanDb, DeleteLinDb, ReloadDbs };
    Action action = Action::None;
    int action_net = -1;
    int action_item = -1;
    const auto leaf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    const auto branch = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    const auto sel_flag = [&](SetupSel sel, int n, int item = -1)
    { return s.sel == sel && s.net == n && s.item == item ? ImGuiTreeNodeFlags_Selected : 0; };
    const auto clicked = [&](SetupSel sel, int n, int item = -1)
    {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            select(s, sel, n, item);
        }
    };
    const auto menu = [&](const char* label, Action a, int n, int item = -1)
    {
        if (ImGui::MenuItem(label))
        {
            action = a;
            action_net = n;
            action_item = item;
        }
    };

    for (int n = 0; n < static_cast<int>(s.work.networks.size()); ++n)
    {
        SetupNetwork& net = s.work.networks[static_cast<std::size_t>(n)];
        ImGui::PushID(n);
        const bool net_open = ImGui::TreeNodeEx("##net", branch | sel_flag(SetupSel::Network, n), "%s", net.name.c_str());
        clicked(SetupSel::Network, n);
        if (net_open)
        {
            const bool if_open = ImGui::TreeNodeEx("##ifs", branch | sel_flag(SetupSel::Interfaces, n), "Interfaces");
            clicked(SetupSel::Interfaces, n);
            if (ImGui::BeginPopupContextItem())
            {
                menu("Add...", Action::AddInterface, n);
                ImGui::EndPopup();
            }
            if (if_open)
            {
                for (int i = 0; i < static_cast<int>(net.interfaces.size()); ++i)
                {
                    const auto& intf = net.interfaces[static_cast<std::size_t>(i)];
                    const bool known = ifaces_find(app.ifaces, intf.driver, intf.name) >= 0;
                    ImGui::PushID(i);
                    ImGui::TreeNodeEx("##if", leaf | sel_flag(SetupSel::Interface, n, i), "%s%s",
                                      known ? "" : "(unavailable) ", intf.name.c_str());
                    clicked(SetupSel::Interface, n, i);
                    if (ImGui::BeginPopupContextItem())
                    {
                        menu("Delete", Action::DeleteInterface, n, i);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            const bool db_open = ImGui::TreeNodeEx("##dbs", branch | sel_flag(SetupSel::Databases, n), "Database");
            clicked(SetupSel::Databases, n);
            if (ImGui::BeginPopupContextItem())
            {
                menu("Add...", Action::AddDb, n);
                ImGui::EndPopup();
            }
            if (db_open)
            {
                for (int i = 0; i < static_cast<int>(net.can_dbs.size()); ++i)
                {
                    ImGui::PushID(i);
                    ImGui::TreeNodeEx("##can", leaf | sel_flag(SetupSel::CanDb, n, i), "%s",
                                      file_name(net.can_dbs[static_cast<std::size_t>(i)]->path).c_str());
                    clicked(SetupSel::CanDb, n, i);
                    if (ImGui::BeginPopupContextItem())
                    {
                        menu("Delete", Action::DeleteCanDb, n, i);
                        menu("Reload", Action::ReloadDbs, n, i);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                for (int i = 0; i < static_cast<int>(net.lin_dbs.size()); ++i)
                {
                    ImGui::PushID(1000 + i);
                    ImGui::TreeNodeEx("##lin", leaf | sel_flag(SetupSel::LinDb, n, i), "%s",
                                      file_name(net.lin_dbs[static_cast<std::size_t>(i)]->path).c_str());
                    clicked(SetupSel::LinDb, n, i);
                    if (ImGui::BeginPopupContextItem())
                    {
                        menu("Delete", Action::DeleteLinDb, n, i);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // Applied after the loop, so no vector changes while it is being drawn.
    if (action == Action::None)
    {
        return;
    }
    SetupNetwork& net = s.work.networks[static_cast<std::size_t>(action_net)];
    const auto item = static_cast<std::size_t>(action_item);
    switch (action)
    {
    case Action::AddInterface:
        select(s, SetupSel::Interfaces, action_net);
        s.add_request = true; // opened by the page, where the popup lives
        break;
    case Action::DeleteInterface:
        net.interfaces.erase(net.interfaces.begin() + static_cast<std::ptrdiff_t>(item));
        select(s, SetupSel::Interfaces, action_net);
        break;
    case Action::AddDb:
        select(s, SetupSel::Databases, action_net);
        choose_databases(s, action_net);
        break;
    case Action::DeleteCanDb:
        net.can_dbs.erase(net.can_dbs.begin() + static_cast<std::ptrdiff_t>(item));
        select(s, SetupSel::Databases, action_net);
        setup_rebuild_cache(s.work);
        break;
    case Action::DeleteLinDb:
        net.lin_dbs.erase(net.lin_dbs.begin() + static_cast<std::ptrdiff_t>(item));
        select(s, SetupSel::Databases, action_net);
        setup_rebuild_cache(s.work);
        break;
    case Action::ReloadDbs:
    {
        std::vector<std::string> errors;
        if (!setup_reload_databases(s.work, &errors))
        {
            s.message = "Failed to reload: " + errors.front();
        }
        select(s, SetupSel::Databases, action_net);
        break;
    }
    case Action::None:
        break;
    }
}

// Multi-select list of the enumerated interfaces not yet in any network.
void draw_add_interfaces_popup(App& app, SetupDialogState& s, SetupNetwork& net)
{
    bool open = true;
    if (!ImGui::BeginPopupModal("Add Interfaces", &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (!open) // the title bar's X = Cancel
        {
            s.add_candidates.clear();
            s.add_checked.clear();
        }
        return;
    }
    if (s.add_candidates.empty() && s.add_checked.empty())
    {
        for (int i = 0; i < static_cast<int>(app.ifaces.size()); ++i)
        {
            const Iface& f = app.ifaces[static_cast<std::size_t>(i)];
            if (!setup_interface_used(s.work, f.ops->name, f.info.name))
            {
                s.add_candidates.push_back(i);
            }
        }
        s.add_checked.assign(s.add_candidates.size(), 0);
    }
    if (ImGui::BeginTable("##cand", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 14)))
    {
        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("Driver");
        ImGui::TableSetupColumn("Description");
        ImGui::TableHeadersRow();
        for (std::size_t k = 0; k < s.add_candidates.size(); ++k)
        {
            const Iface& f = app.ifaces[static_cast<std::size_t>(s.add_candidates[k])];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(k));
            if (ImGui::Selectable(f.info.name.c_str(), s.add_checked[k] != 0, ImGuiSelectableFlags_SpanAllColumns))
            {
                s.add_checked[k] = s.add_checked[k] ? 0 : 1;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.ops->name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.info.details.c_str());
        }
        ImGui::EndTable();
    }
    const bool ok = ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6, 0));
    ImGui::SameLine();
    if (ok || ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::Shortcut(ImGuiKey_Escape))
    {
        for (std::size_t k = 0; ok && k < s.add_candidates.size(); ++k)
        {
            if (s.add_checked[k])
            {
                const Iface& f = app.ifaces[static_cast<std::size_t>(s.add_candidates[k])];
                net.interfaces.push_back({.driver = f.ops->name, .name = f.info.name, .bus_type = f.info.bus_type});
            }
        }
        s.add_candidates.clear();
        s.add_checked.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_interfaces_page(App& app, SetupDialogState& s, SetupNetwork& net)
{
    ImGui::TextUnformatted("Interfaces assigned to this network:");
    if (ImGui::BeginTable("##ifs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing())))
    {
        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("Driver");
        ImGui::TableSetupColumn("Bitrate");
        ImGui::TableSetupColumn("Link");
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(net.interfaces.size()); ++i)
        {
            const auto& intf = net.interfaces[static_cast<std::size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(intf.name.c_str(), s.row == i, ImGuiSelectableFlags_SpanAllColumns))
            {
                s.row = i;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(intf.driver.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", intf.bus_type == BusType::LIN ? intf.lin_baudrate : intf.bitrate);
            ImGui::TableNextColumn();
            if (const int idx = ifaces_find(app.ifaces, intf.driver, intf.name); idx >= 0)
            {
                draw_link_buttons(app, app.ifaces[static_cast<std::size_t>(idx)]);
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add Interface...") || s.add_request)
    {
        s.add_request = false;
        s.add_candidates.clear();
        s.add_checked.clear();
        ImGui::OpenPopup("Add Interfaces");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.row < 0 || s.row >= static_cast<int>(net.interfaces.size()));
    if (ImGui::Button("Remove Interface"))
    {
        net.interfaces.erase(net.interfaces.begin() + s.row);
        s.row = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    draw_new_vcan_button(app); // then "Add Interface..." picks it up
    draw_add_interfaces_popup(app, s, net);
}

void draw_databases_page(SetupDialogState& s, SetupNetwork& net)
{
    const int can_count = static_cast<int>(net.can_dbs.size());
    const int total = can_count + static_cast<int>(net.lin_dbs.size());
    if (ImGui::BeginTable("##dbs", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing())))
    {
        ImGui::TableSetupColumn("Filename");
        ImGui::TableSetupColumn("Path");
        ImGui::TableHeadersRow();
        for (int i = 0; i < total; ++i)
        {
            const std::string& path = i < can_count ? net.can_dbs[static_cast<std::size_t>(i)]->path
                                                    : net.lin_dbs[static_cast<std::size_t>(i - can_count)]->path;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(file_name(path).c_str(), s.row == i, ImGuiSelectableFlags_SpanAllColumns))
            {
                s.row = i;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(directory(path).c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add Database..."))
    {
        choose_databases(s, s.net);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.row < 0 || s.row >= total);
    if (ImGui::Button("Remove Database"))
    {
        if (s.row < can_count)
        {
            net.can_dbs.erase(net.can_dbs.begin() + s.row);
        }
        else
        {
            net.lin_dbs.erase(net.lin_dbs.begin() + (s.row - can_count));
        }
        s.row = -1;
        setup_rebuild_cache(s.work);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload DBC"))
    {
        std::vector<std::string> errors;
        if (!setup_reload_databases(s.work, &errors))
        {
            s.message = "Failed to reload: " + errors.front();
        }
    }
}

void draw_page_widgets(App& app, SetupDialogState& s, SetupNetwork& net)
{
    switch (s.sel)
    {
    case SetupSel::Network:
        ImGui::TextUnformatted("Network name:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", &net.name);
        break;
    case SetupSel::Interfaces:
        draw_interfaces_page(app, s, net);
        break;
    case SetupSel::Databases:
    case SetupSel::CanDb:
    case SetupSel::LinDb:
        draw_databases_page(s, net);
        break;
    case SetupSel::Interface:
    {
        if (s.item < 0 || s.item >= static_cast<int>(net.interfaces.size()))
        {
            break;
        }
        SetupInterface& intf = net.interfaces[static_cast<std::size_t>(s.item)];
        const int idx = ifaces_find(app.ifaces, intf.driver, intf.name);
        static const IfaceInfo unavailable{.details = "(interface not available)"};
        const Iface* iface = idx >= 0 ? &app.ifaces[static_cast<std::size_t>(idx)] : nullptr;
        const IfaceInfo& info = iface ? iface->info : unavailable;
        if (intf.bus_type == BusType::LIN)
        {
            draw_lin_setup_page(info, net, intf, s.frame_defaults);
        }
        else
        {
            draw_can_setup_page(info, intf);
        }
        break;
    }
    case SetupSel::None:
        break;
    }
}

void draw_page(App& app, SetupDialogState& s)
{
    if (s.net < 0 || s.net >= static_cast<int>(s.work.networks.size()))
    {
        return;
    }
    SetupNetwork& net = s.work.networks[static_cast<std::size_t>(s.net)];
    // Ids per network and item: a field still active when the tree selects another network
    // would otherwise write its buffer into that one (same "##name" id).
    ImGui::PushID(s.net);
    ImGui::PushID(s.item);
    draw_page_widgets(app, s, net);
    ImGui::PopID();
    ImGui::PopID();
}

} // namespace

void setup_dialog_open(App& app, SetupDialogState& s)
{
    s.work = app.setup;
#ifndef _WIN32 // no SocketCAN there
    // Without root SocketCAN interfaces default to "configured by OS", as in the Qt build.
    if (geteuid() != 0)
    {
        for (auto& net : s.work.networks)
        {
            for (auto& intf : net.interfaces)
            {
                if (intf.driver == "SocketCAN")
                {
                    intf.configure = false;
                }
            }
        }
    }
#endif
    select(s, s.work.networks.empty() ? SetupSel::None : SetupSel::Network, s.work.networks.empty() ? -1 : 0);
    s.message.clear();
    s.open_request = true;
}

void draw_setup_dialog(App& app, SetupDialogState& s)
{
    if (s.open_request)
    {
        ImGui::OpenPopup("Measurement Setup");
        s.open_request = false;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 60, em * 36), ImGuiCond_Appearing);
    bool open = true; // the title bar's X = Cancel; s.work is copied afresh on the next open
    if (!ImGui::BeginPopupModal("Measurement Setup", &open))
    {
        return;
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2;
    ImGui::BeginChild("##left", ImVec2(em * 16, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    draw_tree(app, s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    draw_page(app, s);
    ImGui::EndChild();
    if (const auto files = file_dialog_draw(s.db_dialog);
        !files.empty() && s.db_net >= 0 && s.db_net < static_cast<int>(s.work.networks.size()))
    {
        add_databases(s, s.work.networks[static_cast<std::size_t>(s.db_net)], files);
    }

    if (ImGui::Button("Add Network"))
    {
        std::string name;
        for (int i = 1; name.empty() || setup_find_network(s.work, name); ++i)
        {
            name = std::format("Network {}", i);
        }
        s.work.networks.push_back({.name = name});
        select(s, SetupSel::Network, static_cast<int>(s.work.networks.size()) - 1);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.sel != SetupSel::Network);
    if (ImGui::Button("Remove Network"))
    {
        s.work.networks.erase(s.work.networks.begin() + s.net);
        setup_rebuild_cache(s.work);
        select(s, SetupSel::None, -1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        ifaces_default_setup(app.ifaces, s.work); // the Qt "refresh networks": one network per interface
        select(s, s.work.networks.empty() ? SetupSel::None : SetupSel::Network, s.work.networks.empty() ? -1 : 0);
    }
    if (!s.message.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error)), "%s", s.message.c_str());
    }

    const float bw = em * 6;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("OK", ImVec2(bw, 0)))
    {
        app.setup = std::move(s.work);
        setup_rebuild_cache(app.setup); // bumps generation past the copy's: consumers re-resolve
        s.work = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::Shortcut(ImGuiKey_Escape))
    {
        s.work = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

CanTimingLists can_setup_snap(const IfaceInfo& info, SetupInterface& intf)
{
    CanTimingLists lists;
    const auto& t = info.bitrates;
    if (t.empty())
    {
        return lists;
    }
    const auto snap = [](unsigned& v, const std::vector<unsigned>& values)
    {
        if (!values.empty() && !contains(values, v))
        {
            v = values.front();
        }
    };
    lists.bitrates = timing_values(t, [](const CanTiming&) { return true; }, [](const CanTiming& c) { return c.bitrate; });
    snap(intf.bitrate, lists.bitrates);
    lists.sample_points = timing_values(t, [&](const CanTiming& c) { return c.bitrate == intf.bitrate; },
                                        [](const CanTiming& c) { return c.sample_point; });
    snap(intf.sample_point, lists.sample_points);
    lists.fd_bitrates = timing_values(t, [&](const CanTiming& c) { return c.bitrate == intf.bitrate && c.bitrate_fd > 0; },
                                      [](const CanTiming& c) { return c.bitrate_fd; });
    if (!contains(lists.fd_bitrates, intf.fd_bitrate))
    {
        intf.fd_bitrate = lists.fd_bitrates.empty() ? 0 : lists.fd_bitrates.front();
    }
    lists.fd_sample_points = timing_values(t, [&](const CanTiming& c) { return c.bitrate_fd == intf.fd_bitrate; },
                                           [](const CanTiming& c) { return c.sample_point_fd; });
    snap(intf.fd_sample_point, lists.fd_sample_points);
    intf.can_fd = intf.fd_bitrate > 0 || intf.is_custom_fd_bitrate;
    return lists;
}

uint32_t custom_bitrate_clamp(uint32_t v) noexcept
{
    const uint32_t div = std::max<uint32_t>((v >> 16) & 0xFF, 1);
    const uint32_t seg1 = std::max<uint32_t>((v >> 8) & 0xFF, 2);
    const uint32_t seg2 = std::clamp<uint32_t>(v & 0xFF, 2, 128);
    return div << 16 | seg1 << 8 | seg2;
}

uint32_t custom_fd_bitrate_clamp(uint32_t v) noexcept
{
    const uint32_t div = std::clamp<uint32_t>((v >> 16) & 0xFF, 1, 32);
    const uint32_t seg1 = std::clamp<uint32_t>((v >> 8) & 0xFF, 1, 32);
    const uint32_t seg2 = std::clamp<uint32_t>(v & 0xFF, 1, 16);
    return div << 16 | seg1 << 8 | seg2;
}

namespace
{

void draw_can_setup_page(const IfaceInfo& info, SetupInterface& intf)
{
    const CanTimingLists lists = can_setup_snap(info, intf);
    interface_header(info, intf);
    const uint32_t caps = info.capabilities;

    ImGui::SeparatorText("CAN Setting / CAN FD Arbitration Phase Setting");
    ImGui::BeginDisabled(intf.is_custom_bitrate);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    value_combo("Bitrate", intf.bitrate, lists.bitrates, bitrate_str);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    value_combo("Sample Point", intf.sample_point, lists.sample_points, sample_point_str);
    ImGui::EndDisabled();

    ImGui::SeparatorText("CAN FD Data Phase Setting");
    ImGui::BeginDisabled(intf.is_custom_fd_bitrate || !has(caps, iface_cap::canfd));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    value_combo("Data Bitrate", intf.fd_bitrate, lists.fd_bitrates, bitrate_str);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    value_combo("Data Sample Point", intf.fd_sample_point, lists.fd_sample_points, sample_point_str);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Options");
    bool by_os = !intf.configure;
    ImGui::BeginDisabled(!has(caps, iface_cap::config_os));
    if (ImGui::Checkbox("Configured by OS", &by_os))
    {
        intf.configure = !by_os;
    }
    ImGui::EndDisabled();
    const auto option = [&](const char* label, bool& v, uint32_t cap)
    {
        ImGui::BeginDisabled(!intf.configure || !has(caps, cap));
        ImGui::Checkbox(label, &v);
        ImGui::EndDisabled();
    };
    option("Bus Monitoring Mode (Listen Only)", intf.listen_only, iface_cap::listen_only);
    option("Auto-Restart on Bus-Off Condition", intf.auto_restart, iface_cap::auto_restart);
    option("Custom Bitrate + Sample Point", intf.is_custom_bitrate, iface_cap::custom_bitrate);
    ImGui::BeginDisabled(!intf.is_custom_bitrate);
    custom_field("##custom", intf.custom_bitrate, custom_bitrate_clamp);
    ImGui::EndDisabled();
    option("Custom CAN FD Data Bitrate + Sample Point", intf.is_custom_fd_bitrate, iface_cap::custom_canfd_bitrate);
    ImGui::BeginDisabled(!intf.is_custom_fd_bitrate);
    custom_field("##customfd", intf.custom_fd_bitrate, custom_fd_bitrate_clamp);
    ImGui::EndDisabled();
    intf.can_fd = intf.fd_bitrate > 0 || intf.is_custom_fd_bitrate;
}

} // namespace

void lin_setup_apply_ldf(const LinDb& db, SetupInterface& intf)
{
    static constexpr std::pair<std::string_view, LinProtocolVersion> versions[] = {
        {"1.3", LinProtocolVersion::V1_3}, {"2.0", LinProtocolVersion::V2_0}, {"2.1", LinProtocolVersion::V2_1},
        {"2.2", LinProtocolVersion::V2_2}, {"2.2A", LinProtocolVersion::V2_2A},
    };
    intf.lin_ldf_path = db.path;
    intf.lin_baudrate = static_cast<unsigned>(db.speed_bps);
    intf.lin_timebase_ms = static_cast<uint8_t>(std::clamp(db.master_timebase_ms, 0.0, 255.0));
    intf.lin_jitter_us = static_cast<uint16_t>(std::clamp(db.master_jitter_ms * 1000.0, 0.0, 65535.0));
    for (const auto& [name, v] : versions)
    {
        if (db.protocol_version == name)
        {
            intf.lin_protocol = v;
        }
    }
}

namespace
{

void draw_lin_setup_page(const IfaceInfo& info, const SetupNetwork& net, SetupInterface& intf, LinFrameDefaultsState& fd)
{
    interface_header(info, intf);
    const bool cap_master = has(info.capabilities, iface_cap::lin_master);
    const bool cap_slave = has(info.capabilities, iface_cap::lin_slave);

    ImGui::SeparatorText("Options");
    ImGui::Checkbox("Use Classic Checksum (LIN 1.x)", &intf.lin_checksum_classic);
    if (cap_slave)
    {
        ImGui::Checkbox("Listen Only (Slave)", &intf.lin_listen_only);
    }
    else
    {
        intf.lin_listen_only = false;
    }

    ImGui::SeparatorText("LIN Configuration");
    const bool has_ldfs = !net.lin_dbs.empty();
    ImGui::BeginDisabled(!has_ldfs);
    int ldf = 0;
    for (int i = 0; i < static_cast<int>(net.lin_dbs.size()); ++i)
    {
        if (net.lin_dbs[static_cast<std::size_t>(i)]->path == intf.lin_ldf_path)
        {
            ldf = i;
        }
    }
    const LinDb* db = has_ldfs ? net.lin_dbs[static_cast<std::size_t>(ldf)].get() : nullptr;
    if (db && intf.lin_ldf_path != db->path)
    {
        lin_setup_apply_ldf(*db, intf); // first visit or a stale path: take the LDF's settings
    }
    const float w = ImGui::GetFontSize() * 12;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("LDF File", db ? file_name(db->path).c_str() : ""))
    {
        for (int i = 0; i < static_cast<int>(net.lin_dbs.size()); ++i)
        {
            const LinDb& d = *net.lin_dbs[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            if (ImGui::Selectable(file_name(d.path).c_str(), i == ldf) && i != ldf)
            {
                lin_setup_apply_ldf(d, intf);
                db = &d;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    std::vector<unsigned> bauds = {1000, 2400, 4800, 9600, 19200, 20000};
    if (!contains(bauds, intf.lin_baudrate))
    {
        bauds.push_back(intf.lin_baudrate);
    }
    ImGui::SetNextItemWidth(w);
    value_combo("Baud Rate", intf.lin_baudrate, bauds, bitrate_str);

    static constexpr const char* protocols[] = {"LIN 1.3", "LIN 2.0", "LIN 2.1", "LIN 2.2", "LIN 2.2A"};
    int proto = std::clamp(static_cast<int>(intf.lin_protocol), 0, 4);
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Protocol Version", &proto, protocols, 5))
    {
        intf.lin_protocol = static_cast<LinProtocolVersion>(proto);
    }

    // Schedule table by name; falls back to the first table.
    if (db && !db->schedule_tables.empty())
    {
        const auto& tables = db->schedule_tables;
        auto it = std::ranges::find(tables, intf.lin_schedule_table, &LinScheduleTable::name);
        if (it == tables.end())
        {
            it = tables.begin();
        }
        intf.lin_schedule_table = it->name;
        intf.lin_schedule_table_index = static_cast<uint8_t>(it - tables.begin());
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Schedule Table", intf.lin_schedule_table.c_str()))
    {
        for (std::size_t i = 0; db && i < db->schedule_tables.size(); ++i)
        {
            const auto& name = db->schedule_tables[i].name;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(name.c_str(), name == intf.lin_schedule_table))
            {
                intf.lin_schedule_table = name;
                intf.lin_schedule_table_index = static_cast<uint8_t>(i);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Node mode: forced when the device supports only one, or by listen-only.
    if (db)
    {
        if (cap_master && !cap_slave)
        {
            intf.lin_node_mode = LinNodeMode::Master;
        }
        else if ((cap_slave && !cap_master) || intf.lin_listen_only)
        {
            intf.lin_node_mode = LinNodeMode::Slave;
        }
        else if (intf.lin_node_mode == LinNodeMode::Monitor)
        {
            intf.lin_node_mode = LinNodeMode::Master;
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Node Mode:");
    ImGui::SameLine();
    ImGui::BeginDisabled(!cap_master || intf.lin_listen_only);
    if (ImGui::RadioButton("Master", intf.lin_node_mode == LinNodeMode::Master))
    {
        intf.lin_node_mode = LinNodeMode::Master;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!cap_slave && cap_master);
    if (ImGui::RadioButton("Slave", intf.lin_node_mode == LinNodeMode::Slave))
    {
        intf.lin_node_mode = LinNodeMode::Slave;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool slave = intf.lin_node_mode == LinNodeMode::Slave;
    if (db && slave && !db->slave_nodes.empty() && std::ranges::find(db->slave_nodes, intf.lin_slave_node) == db->slave_nodes.end())
    {
        intf.lin_slave_node = db->slave_nodes.front();
    }
    if (!slave)
    {
        intf.lin_slave_node.clear();
    }
    ImGui::BeginDisabled(!slave);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::BeginCombo("##slave", intf.lin_slave_node.c_str()))
    {
        for (std::size_t i = 0; db && i < db->slave_nodes.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(db->slave_nodes[i].c_str(), db->slave_nodes[i] == intf.lin_slave_node))
            {
                intf.lin_slave_node = db->slave_nodes[i];
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("Timebase:");
    ImGui::SameLine();
    ImGui::TextUnformatted(db ? std::format("{} ms", db->master_timebase_ms).c_str() : "-");
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::TextDisabled("Jitter:");
    ImGui::SameLine();
    ImGui::TextUnformatted(db ? std::format("{} ms", db->master_jitter_ms).c_str() : "-");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Frame Defaults:");
    ImGui::SameLine();
    if (ImGui::Button("Configure...") && db)
    {
        fd.node = slave ? intf.lin_slave_node : db->master_node;
        fd.frame = 0;
        fd.open_request = true;
    }
    ImGui::EndDisabled();
    if (db)
    {
        draw_lin_frame_defaults(*db, intf, fd);
    }
}

} // namespace

void lin_signal_write_raw(std::span<uint8_t> data, const LinSignal& sig, uint64_t raw) noexcept
{
    for (unsigned i = 0; i < sig.bit_length && i < 64; ++i)
    {
        const unsigned pos = sig.bit_offset + i;
        if (pos / 8 >= data.size())
        {
            break;
        }
        const auto mask = static_cast<uint8_t>(1u << (pos % 8));
        data[pos / 8] = static_cast<uint8_t>(((raw >> i) & 1u) ? data[pos / 8] | mask : data[pos / 8] & ~mask);
    }
}

std::vector<uint8_t> lin_frame_init_data(const LinFrame& frame)
{
    std::vector<uint8_t> data(frame.length, 0);
    for (const auto& sig : frame.signals)
    {
        lin_signal_write_raw(data, sig, sig.init_value);
    }
    return data;
}

namespace
{

void draw_lin_frame_defaults(const LinDb& db, SetupInterface& intf, LinFrameDefaultsState& fd)
{
    if (fd.open_request)
    {
        ImGui::OpenPopup("LIN Frame Default Data");
        fd.open_request = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36, 0), ImGuiCond_Appearing);
    bool open = true; // edits apply live, so X and Esc are OK
    if (!ImGui::BeginPopupModal("LIN Frame Default Data", &open))
    {
        return;
    }
    std::vector<const LinFrame*> frames;
    for (const auto& [id, f] : db.frames)
    {
        if (fd.node.empty() || f.publisher == fd.node)
        {
            frames.push_back(&f);
        }
    }
    fd.frame = frames.empty() ? 0 : std::clamp(fd.frame, 0, static_cast<int>(frames.size()) - 1);
    const auto label = [](const LinFrame& f) { return std::format("{} (0x{:02X})", f.name, f.id); };
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##frame", frames.empty() ? "" : label(*frames[static_cast<std::size_t>(fd.frame)]).c_str()))
    {
        for (int i = 0; i < static_cast<int>(frames.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(label(*frames[static_cast<std::size_t>(i)]).c_str(), i == fd.frame))
            {
                fd.frame = i;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (!frames.empty())
    {
        const LinFrame& frame = *frames[static_cast<std::size_t>(fd.frame)];
        auto& data = intf.lin_frame_defaults[frame.id];
        if (data.size() != frame.length)
        {
            data.assign(frame.length, 0);
        }
        ImGui::SeparatorText("Bytes (hex)");
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (i > 0)
            {
                ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 2);
            ImGui::InputScalar("##b", ImGuiDataType_U8, &data[i], nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::PopID();
        }
        ImGui::SeparatorText("Signals");
        if (ImGui::BeginTable("##sigs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Raw Value");
            ImGui::TableSetupColumn("Physical");
            ImGui::TableSetupColumn("Unit");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < frame.signals.size(); ++i)
            {
                const LinSignal& sig = frame.signals[i];
                uint64_t raw = lin_signal_extract_raw(sig, data);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sig.name.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
                if (ImGui::InputScalar("##raw", ImGuiDataType_U64, &raw))
                {
                    const uint64_t max = sig.bit_length >= 64 ? ~0ull : (1ull << sig.bit_length) - 1;
                    lin_signal_write_raw(data, sig, std::min(raw, max));
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", lin_signal_raw_to_physical(sig, lin_signal_extract_raw(sig, data)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sig.unit.c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset Values"))
        {
            data = lin_frame_init_data(frame);
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::Shortcut(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

bool setup_interface_used(const Setup& setup, std::string_view driver, std::string_view name)
{
    return std::ranges::any_of(setup.networks, [&](const SetupNetwork& n)
                               { return std::ranges::any_of(n.interfaces, [&](const SetupInterface& si)
                                                            { return si.driver == driver && si.name == name; }); });
}
