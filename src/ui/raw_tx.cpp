#include "ui/raw_tx.h"

#include <algorithm>
#include <cfloat>
#include <format>
#include <string>

#include <imgui.h>

#include "app.h"
#include "db/model/can_db.h"
#include "ui/tx_generator.h"
#include "ui/vim_nav.h"

namespace
{

// Toggles one bus_flag bit with a checkbox.
bool flag_checkbox(const char* label, BusMessage& msg, uint16_t flag)
{
    bool on = has_flag(msg, flag);
    if (!ImGui::Checkbox(label, &on))
    {
        return false;
    }
    msg.flags = static_cast<uint16_t>(on ? msg.flags | flag : msg.flags & ~flag);
    return true;
}

} // namespace

bool draw_iface_combo(const char* id, App& app, uint16_t& iface, std::optional<BusType> bus)
{
    const std::string current = iface < app.ifaces.size() ? app.ifaces[iface].info.name : std::string{};
    bool changed = false;
    if (ImGui::BeginCombo(id, current.c_str()))
    {
        for (const SetupNetwork& net : app.setup.networks)
        {
            for (const SetupInterface& si : net.interfaces)
            {
                const int index = ifaces_find(app.ifaces, si.driver, si.name);
                if (index < 0 || (bus && si.bus_type != *bus))
                {
                    continue;
                }
                const std::string label = std::format("{}: {}##{}", net.name, si.name, index);
                if (ImGui::Selectable(label.c_str(), index == iface))
                {
                    changed = iface != index;
                    iface = static_cast<uint16_t>(index);
                }
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool draw_raw_tx(App& app, BusMessage& msg, const CanDbMessage* db)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    bool changed = false;
    const bool canfd = msg.iface < app.ifaces.size() && (app.ifaces[msg.iface].info.capabilities & iface_cap::canfd) != 0;

    // --- Header: ID, DLC, flags, interface ---
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("ID (Hex):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * px);
    uint32_t id = msg.id;
    if (ImGui::InputScalar("##id", ImGuiDataType_U32, &id, nullptr, nullptr, "%X",
                           ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase))
    {
        msg.id = std::min(id, can_id_mask_extended);
        if (msg.id > can_id_mask_standard)
        {
            msg.flags |= bus_flag::extended;
        }
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("DLC:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f * px);
    if (ImGui::BeginCombo("##dlc", std::to_string(msg.len).c_str()))
    {
        const int options = canfd || has_flag(msg, bus_flag::fd) ? 16 : 9;
        for (int d = 0; d < options; ++d)
        {
            const int len = bus_dlc_lengths[static_cast<std::size_t>(d)];
            if (ImGui::Selectable(std::to_string(len).c_str(), len == msg.len))
            {
                set_length(msg, len);
                if (len > 8)
                {
                    msg.flags = static_cast<uint16_t>((msg.flags | bus_flag::fd) & ~bus_flag::rtr);
                }
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // An id above 0x7FF only exists as extended: unticking would send a different (masked) id.
    ImGui::BeginDisabled(msg.id > can_id_mask_standard);
    changed |= flag_checkbox("Extended", msg, bus_flag::extended);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ids above 7FF are always extended");
    ImGui::SameLine();
    const bool fd = has_flag(msg, bus_flag::fd);
    ImGui::BeginDisabled(fd);
    changed |= flag_checkbox("RTR", msg, bus_flag::rtr);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canfd || msg.len > 8); // more than 8 bytes forces FD
    if (flag_checkbox("FD", msg, bus_flag::fd))
    {
        msg.flags = static_cast<uint16_t>(has_flag(msg, bus_flag::fd) ? msg.flags & ~bus_flag::rtr : msg.flags & ~bus_flag::brs);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_flag(msg, bus_flag::fd));
    changed |= flag_checkbox("BRS", msg, bus_flag::brs);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("Interface:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f * px);
    changed |= draw_iface_combo("##iface", app, msg.iface);

    // --- Data hex grid: 8 bytes per row, row header = offset ---
    const int rows = std::max(1, (msg.len + 7) / 8);
    constexpr ImGuiTableFlags grid_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    ImGui::PushFont(app.fonts.mono, 0.0f);
    if (ImGui::BeginTable("##data", 9, grid_flags))
    {
        const float cell_w = ImGui::CalcTextSize("000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00").x);
        for (int c = 0; c < 8; ++c)
        {
            ImGui::TableSetupColumn(std::to_string(c).c_str(), ImGuiTableColumnFlags_WidthFixed, cell_w);
        }
        ImGui::TableHeadersRow();
        for (int r = 0; r < rows; ++r)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", r * 8);
            for (int c = 0; c < 8; ++c)
            {
                ImGui::TableNextColumn();
                const int i = r * 8 + c;
                ImGui::PushID(i);
                ImGui::BeginDisabled(i >= msg.len);
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= ImGui::InputScalar("##b", ImGuiDataType_U8, &msg.data[static_cast<std::size_t>(i)], nullptr,
                                              nullptr, "%02X",
                                              ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();

    // --- Signal table (only with a DBC message) ---
    if (db != nullptr && !db->signals.empty())
    {
        constexpr ImGuiTableFlags sig_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
                                              | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##signals", 3, sig_flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 120.0f * px);
            ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            int i = 0;
            for (const CanDbSignal& sig : db->signals)
            {
                ImGui::PushID(i++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(sig.name.c_str());
                ImGui::TableNextColumn();
                double value = can_signal_extract_physical(sig, msg);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputDouble("##v", &value, 0.0, 0.0, can_signal_printf(sig).c_str())) // 1e-07 scales stay visible
                {
                    tx_signal_set(sig, msg, value); // clamped to the DBC range, as the Generator row editor
                    changed = true;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sig.unit.c_str());
                vim_yank_table_row({{.label = "Signal", .text = sig.name},
                                    {.label = "Value", .text = can_signal_format(sig, can_signal_extract_physical(sig, msg))},
                                    {.label = "Unit", .text = sig.unit}});
                ImGui::PopID();
            }
            vim_yank_end_table();
        }
    }
    return changed;
}
