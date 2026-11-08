#include "ui/lin_control.h"

#include <cfloat>
#include <format>
#include <shared_mutex>
#include <string>

#include <imgui.h>
#include <pugixml.hpp>

#include "app.h"
#include "ui/raw_tx.h"
#include "ui/vim_nav.h"

namespace
{

struct LinRow
{
    int index;                 // App::ifaces
    SetupNetwork* net;
    SetupInterface* intf;
    std::string label;         // "Network: name"
};

std::vector<LinRow> lin_rows(App& app)
{
    std::vector<LinRow> out;
    for (SetupNetwork& net : app.setup.networks)
    {
        for (SetupInterface& si : net.interfaces)
        {
            const int index = ifaces_find(app.ifaces, si.driver, si.name);
            if (si.bus_type == BusType::LIN && index >= 0)
            {
                out.push_back({index, &net, &si, std::format("{}: {}", net.name, si.name)});
            }
        }
    }
    return out;
}

std::string request_label(const App& app, const LinDiagRequest& req)
{
    const std::string iface = req.iface < app.ifaces.size() ? app.ifaces[req.iface].info.name : std::string{};
    return iface.empty() ? req.name : std::format("[{}]   {}", iface, req.name);
}

// Schedule tables of the network's LDFs (index = SetupInterface::lin_schedule_table_index).
std::vector<const LinScheduleTable*> schedule_tables(const SetupNetwork& net)
{
    std::vector<const LinScheduleTable*> out;
    for (const auto& db : net.lin_dbs)
    {
        for (const LinScheduleTable& t : db->schedule_tables)
        {
            out.push_back(&t);
        }
    }
    return out;
}

void draw_interface_rows(App& app)
{
    const std::vector<LinRow> rows = lin_rows(app);
    if (rows.empty())
    {
        ImGui::TextDisabled("No LIN interfaces configured");
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginDisabled(!app.measuring);
    for (const LinRow& row : rows)
    {
        ImGui::PushID(row.index);
        Iface& iface = app.ifaces[static_cast<std::size_t>(row.index)];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row.label.c_str());
        const std::vector<const LinScheduleTable*> tables = schedule_tables(*row.net);
        if (!tables.empty())
        {
            ImGui::SameLine(160.0f * px);
            const std::size_t current = row.intf->lin_schedule_table_index;
            const char* preview = current < tables.size() ? tables[current]->name.c_str() : "-";
            ImGui::SetNextItemWidth(150.0f * px);
            if (ImGui::BeginCombo("##table", preview))
            {
                for (std::size_t i = 0; i < tables.size(); ++i)
                {
                    if (ImGui::Selectable(tables[i]->name.c_str(), i == current))
                    {
                        row.intf->lin_schedule_table = tables[i]->name;
                        row.intf->lin_schedule_table_index = static_cast<uint8_t>(i);
                        lin_send_set_schedule(iface, static_cast<uint8_t>(i));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Active schedule table");
        }
        const float buttons = (ImGui::CalcTextSize("Wakeup").x + ImGui::CalcTextSize("Sleep").x)
                              + ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - buttons);
        if (ImGui::Button("Sleep"))
        {
            lin_send_sleep_wakeup(iface, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Wakeup"))
        {
            lin_send_sleep_wakeup(iface, true);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}

void send_request(App& app, const LinDiagRequest& req)
{
    if (req.iface < app.ifaces.size())
    {
        lin_send_diag_request(app.ifaces[req.iface], req.nad, req.data);
    }
}

void draw_requests(App& app, LinControl& lc)
{
    ImGui::TextUnformatted("Diagnostic Requests");
    const float px = ImGui::GetFontSize() / 15.0f;
    const float button_w = 90.0f * px;
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##requests", 1, flags, ImVec2(-button_w - ImGui::GetStyle().ItemSpacing.x, 0.0f)))
    {
        for (int i = 0; i < static_cast<int>(lc.requests.size()); ++i)
        {
            const LinDiagRequest& req = lc.requests[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(request_label(app, req).c_str(), lc.selected == i, ImGuiSelectableFlags_AllowDoubleClick))
            {
                lc.selected = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    send_request(app, req);
                }
            }
            ImGui::SetItemTooltip("NAD 0x%02X: %s (double-click to send)", req.nad, format_hex_bytes(req.data).c_str());
            vim_yank_table_row({{.label = "Request", .text = request_label(app, req)},
                                {.label = "NAD", .text = std::format("{:02X}", req.nad)},
                                {.label = "Data", .text = format_hex_bytes(req.data)}},
                               true);
            ImGui::PopID();
        }
        vim_yank_end_table();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button("Add...", ImVec2(button_w, 0.0f)))
    {
        diag_dialog_open(lc.dialog, nullptr);
    }
    const bool has_selection = lc.selected >= 0 && lc.selected < static_cast<int>(lc.requests.size());
    ImGui::BeginDisabled(!has_selection);
    if (ImGui::Button("Edit...", ImVec2(button_w, 0.0f)))
    {
        diag_dialog_open(lc.dialog, &lc.requests[static_cast<std::size_t>(lc.selected)]);
    }
    if (ImGui::Button("Remove", ImVec2(button_w, 0.0f)))
    {
        lc.requests.erase(lc.requests.begin() + lc.selected);
        lc.selected = -1;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();

    LinDiagRequest result;
    if (draw_diag_dialog(app, lc.dialog, result))
    {
        if (lc.dialog.editing && has_selection)
        {
            lc.requests[static_cast<std::size_t>(lc.selected)] = std::move(result);
        }
        else
        {
            lc.requests.push_back(std::move(result));
        }
    }
}

} // namespace

bool lin_send_sleep_wakeup(Iface& iface, bool wakeup)
{
    std::shared_lock lock(iface.io_mutex);
    if (!iface.open || iface.ops == nullptr || iface.ops->lin_sleep_wakeup == nullptr)
    {
        return false;
    }
    iface.ops->lin_sleep_wakeup(iface, wakeup);
    return true;
}

bool lin_send_set_schedule(Iface& iface, uint8_t table)
{
    std::shared_lock lock(iface.io_mutex);
    if (!iface.open || iface.ops == nullptr || iface.ops->lin_set_schedule == nullptr)
    {
        return false;
    }
    iface.ops->lin_set_schedule(iface, table);
    return true;
}

bool lin_send_diag_request(Iface& iface, uint8_t nad, std::span<const uint8_t> data)
{
    std::shared_lock lock(iface.io_mutex);
    if (!iface.open || iface.ops == nullptr || iface.ops->lin_diag_request == nullptr || data.empty())
    {
        return false;
    }
    iface.ops->lin_diag_request(iface, nad, data);
    return true;
}

bool draw_lin_iface_combo(const char* id, App& app, uint16_t& iface)
{
    if (iface >= app.ifaces.size())
    {
        const std::vector<LinRow> rows = lin_rows(app);
        if (!rows.empty())
        {
            iface = static_cast<uint16_t>(rows.front().index); // as the Qt combo: first entry preselected
        }
    }
    return draw_iface_combo(id, app, iface, BusType::LIN);
}

void draw_lin_control(App& app, const WorkspaceTab& tab, LinControl& lc)
{
    if (!lc.open)
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(480.0f * px, 360.0f * px), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(workspace_window_name(tab, "LIN Control").c_str(), &lc.open))
    {
        const float rows_h = ImGui::GetContentRegionAvail().y * 0.4f;
        if (ImGui::BeginChild("##rows", ImVec2(0.0f, rows_h), ImGuiChildFlags_None))
        {
            draw_interface_rows(app);
        }
        ImGui::EndChild();
        ImGui::Separator();
        draw_requests(app, lc);
    }
    ImGui::End();
}

void lin_control_save_xml(const LinControl& lc, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    for (const LinDiagRequest& req : lc.requests)
    {
        pugi::xml_node node = el.append_child("DiagRequest");
        node.append_attribute("name") = req.name.c_str();
        if (req.iface < ifaces.size())
        {
            node.append_attribute("driver") = ifaces[req.iface].ops ? ifaces[req.iface].ops->name : "";
            node.append_attribute("interface") = ifaces[req.iface].info.name.c_str();
        }
        node.append_attribute("nad") = req.nad;
        node.append_attribute("data") = format_hex_bytes(req.data).c_str();
    }
}

void lin_control_load_xml(LinControl& lc, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    lc.requests.clear();
    lc.selected = -1;
    for (pugi::xml_node node : el.children("DiagRequest"))
    {
        const int index = ifaces_find(ifaces, node.attribute("driver").as_string(), node.attribute("interface").as_string());
        lc.requests.push_back({
            .name = node.attribute("name").as_string(),
            .iface = static_cast<uint16_t>(index >= 0 ? index : UINT16_MAX),
            .nad = static_cast<uint8_t>(node.attribute("nad").as_uint()),
            .data = parse_hex_bytes(node.attribute("data").as_string()).value_or(std::vector<uint8_t>{}),
        });
    }
}
