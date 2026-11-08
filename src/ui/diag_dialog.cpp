#include "ui/diag_dialog.h"

#include <cfloat>
#include <format>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "drivers/slcan_codec.h"
#include "ui/lin_control.h"
#include "ui/theme.h"
#include "ui/vim_nav.h"

namespace
{

// Label cell of a two-column form table, the value cell follows.
void form_label(const char* text)
{
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableNextColumn();
}

} // namespace

std::optional<std::vector<uint8_t>> parse_hex_bytes(std::string_view text)
{
    std::vector<uint8_t> out;
    int pending = -1;
    for (const char c : text)
    {
        if (c == ' ' || c == '\t')
        {
            continue;
        }
        const int v = slcan::from_hex_nibble(c);
        if (v < 0)
        {
            return std::nullopt;
        }
        if (pending < 0)
        {
            pending = v;
        }
        else
        {
            out.push_back(static_cast<uint8_t>(pending << 4 | v));
            pending = -1;
        }
    }
    if (pending >= 0)
    {
        return std::nullopt;
    }
    return out;
}

std::string format_hex_bytes(std::span<const uint8_t> data)
{
    std::string out;
    for (const uint8_t b : data)
    {
        out += std::format("{}{:02X}", out.empty() ? "" : " ", b);
    }
    return out;
}

void diag_dialog_open(DiagDialogState& d, const LinDiagRequest* existing)
{
    d.editing = existing != nullptr;
    d.name = existing ? existing->name : std::string{};
    d.iface = existing ? existing->iface : UINT16_MAX;
    d.nad = existing ? existing->nad : 0;
    d.data_hex = existing ? format_hex_bytes(existing->data) : std::string{};
    d.error.clear();
    d.open = true;
}

bool draw_diag_dialog(App& app, DiagDialogState& d, LinDiagRequest& out)
{
    const char* title = d.editing ? "Edit Diagnostic Request" : "Add Diagnostic Request";
    if (!d.open)
    {
        return false;
    }
    if (!ImGui::IsPopupOpen(title))
    {
        ImGui::OpenPopup(title);
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(380.0f * px, 0.0f), ImGuiCond_Appearing);
    bool accepted = false;
    if (!ImGui::BeginPopupModal(title, &d.open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return false;
    }
    if (ImGui::BeginTable("##form", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        form_label("Name:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##name", "e.g. Read Product ID", &d.name);
        vim_yank_table_row({{.label = "Name", .text = d.name}});
        form_label("Interface:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        draw_lin_iface_combo("##iface", app, d.iface);
        form_label("NAD:");
        ImGui::SetNextItemWidth(80.0f * px);
        uint8_t nad = static_cast<uint8_t>(d.nad);
        if (ImGui::InputScalar("##nad", ImGuiDataType_U8, &nad, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal))
        {
            d.nad = nad;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d dec)", d.nad);
        vim_yank_table_row({{.label = "NAD", .text = std::format("{:02X}", d.nad)}});
        form_label("Data (SID + params):");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##data", "SID + params (no PCI), e.g. 22 F1 90", &d.data_hex);
        ImGui::SetItemTooltip("Service data bytes starting with the SID (max 6 bytes).\n"
                              "The PCI byte is added automatically - do not include it.");
        vim_yank_table_row({{.label = "Data", .text = d.data_hex}});
        vim_yank_end_table();
    }
    if (!d.error.empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error)), "%s", d.error.c_str());
    }
    if (ImGui::Button("OK", ImVec2(80.0f * px, 0.0f)))
    {
        const auto data = parse_hex_bytes(d.data_hex);
        if (d.name.find_first_not_of(" \t") == std::string::npos)
        {
            d.error = "Please enter a name.";
        }
        else if (d.iface >= app.ifaces.size())
        {
            d.error = "No LIN interface available.";
        }
        else if (!data)
        {
            d.error = "Data must be hex byte pairs.";
        }
        else if (data->empty())
        {
            d.error = "Please enter at least one data byte.";
        }
        else
        {
            out = {.name = d.name, .iface = d.iface, .nad = static_cast<uint8_t>(d.nad), .data = *data};
            accepted = true;
            d.open = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80.0f * px, 0.0f)) || ImGui::Shortcut(ImGuiKey_Escape))
    {
        d.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return accepted;
}
