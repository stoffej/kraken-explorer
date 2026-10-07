#include "ui/bit_matrix.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <format>
#include <string>

#include <imgui.h>

#include "db/model/can_db.h"
#include "ui/theme.h"

namespace
{

[[nodiscard]] bool is_bright(ImU32 c)
{
    const unsigned r = c & 0xFF;
    const unsigned g = (c >> 8) & 0xFF;
    const unsigned b = (c >> 16) & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 > 128;
}

[[nodiscard]] int byte_rows(const CanDbMessage* msg)
{
    return msg != nullptr ? std::clamp<int>(msg->dlc, 8, 64) : 8;
}

// Signal index owning each payload bit (byte * 8 + bit, bit 0 = LSB), -1 = unused.
// Overlapping (multiplexed) signals: the first one in the message wins.
using BitOwners = std::array<int16_t, 64 * 8>;

void bit_matrix_owners(const CanDbMessage& msg, BitOwners& out)
{
    out.fill(-1);
    const auto claim = [&](int bit, int16_t owner)
    {
        if (bit >= 0 && bit < static_cast<int>(out.size()) && out[static_cast<std::size_t>(bit)] < 0)
        {
            out[static_cast<std::size_t>(bit)] = owner;
        }
    };
    int16_t index = 0;
    for (const CanDbSignal& sig : msg.signals)
    {
        if (!sig.big_endian)
        {
            for (int i = 0; i < sig.length; ++i)
            {
                claim(sig.start_bit + i, index);
            }
        }
        else
        {
            // start_bit is the parser's sequential index (msb_byte * 8 + 7 - msb_bit); walk
            // from the MSB towards the LSB, wrapping from bit 0 to bit 7 of the next byte.
            int bit = (sig.start_bit / 8) * 8 + (7 - sig.start_bit % 8);
            for (int i = 0; i < sig.length; ++i)
            {
                claim(bit, index);
                bit = bit % 8 == 0 ? bit + 15 : bit - 1;
            }
        }
        ++index;
    }
}

} // namespace

void draw_bit_matrix(const CanDbMessage* msg, float cell_size)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    const float cell_h = cell_size * px;
    const float cell_w = cell_h * 1.8f;
    const float label = 40.0f * px;
    const float margin = 10.0f * px;
    const int rows = byte_rows(msg);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(margin * 2 + label + 8 * cell_w, margin * 2 + label + rows * cell_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 grid(origin.x + margin + label, origin.y + margin + label);
    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);

    // Header: bit numbers 7..0 on top, byte numbers on the left.
    for (int col = 0; col < 8; ++col)
    {
        const std::string s = std::to_string(7 - col);
        const ImVec2 sz = ImGui::CalcTextSize(s.c_str());
        dl->AddText(ImVec2(grid.x + col * cell_w + (cell_w - sz.x) * 0.5f, origin.y + margin + (label - sz.y) * 0.5f),
                    text_col, s.c_str());
    }
    for (int row = 0; row < rows; ++row)
    {
        const std::string s = std::to_string(row);
        const ImVec2 sz = ImGui::CalcTextSize(s.c_str());
        dl->AddText(ImVec2(origin.x + margin + (label - sz.x) * 0.5f, grid.y + row * cell_h + (cell_h - sz.y) * 0.5f),
                    text_col, s.c_str());
    }

    const ImU32 grid_col = ImGui::GetColorU32(ImGuiCol_Text, 40.0f / 255.0f);
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < 8; ++col)
        {
            const ImVec2 a(grid.x + col * cell_w, grid.y + row * cell_h);
            dl->AddRect(a, ImVec2(a.x + cell_w, a.y + cell_h), grid_col);
        }
    }
    if (msg == nullptr)
    {
        return;
    }

    BitOwners owners;
    bit_matrix_owners(*msg, owners);
    ImFont* font = ImGui::GetFont();
    const float font_size = std::max(ImGui::GetFontSize(), cell_h * 0.3f); // the UI's text size, larger when zoomed in
    int index = 0;
    for (const CanDbSignal& sig : msg->signals)
    {
        const ImU32 bg = theme_signal_color(static_cast<unsigned>(index));
        const bool bright = is_bright(bg);
        for (int row = 0; row < rows; ++row)
        {
            // Visual span of the signal in this byte: leftmost (bit 7 side) to rightmost bit.
            int left = -1;
            int right = 8;
            for (int b = 7; b >= 0; --b)
            {
                if (owners[static_cast<std::size_t>(row * 8 + b)] == index)
                {
                    left = std::max(left, b);
                    right = std::min(right, b);
                }
            }
            if (left < 0)
            {
                continue;
            }
            const int wide = left - right + 1;
            const ImVec2 a(grid.x + (7 - left) * cell_w, grid.y + row * cell_h);
            const ImVec2 b(a.x + wide * cell_w, a.y + cell_h);
            dl->AddRectFilled(a, b, bg);
            const ImU32 sep = bright ? IM_COL32(0, 0, 0, 40) : IM_COL32(255, 255, 255, 40);
            for (int i = 1; i < wide; ++i)
            {
                dl->AddLine(ImVec2(a.x + i * cell_w, a.y), ImVec2(a.x + i * cell_w, b.y), sep);
            }
            dl->AddRect(a, b, bright ? IM_COL32(0, 0, 0, 80) : IM_COL32(255, 255, 255, 80));

            // Arrows only when there is plenty of room, otherwise just the name (wrapped).
            const float box_w = b.x - a.x;
            std::string text = std::format("<-- {} -->", sig.name);
            if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()).x >= box_w * 0.9f)
            {
                text = sig.name;
            }
            const ImVec2 sz = font->CalcTextSizeA(font_size, FLT_MAX, box_w, text.c_str());
            const ImVec4 clip(a.x, a.y, b.x, b.y);
            dl->AddText(font, font_size, ImVec2(a.x + std::max(0.0f, (box_w - sz.x) * 0.5f), a.y + std::max(0.0f, (cell_h - sz.y) * 0.5f)),
                        bright ? IM_COL32_BLACK : IM_COL32_WHITE, text.c_str(), nullptr, box_w, &clip);
        }
        ++index;
    }
}
