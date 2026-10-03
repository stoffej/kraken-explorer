#include "ui/tx_generator.h"

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <pugixml.hpp>

#include "app.h"
#include "core/fuzzy.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/text.h"
#include "db/model/can_db.h"
#include "ui/bit_matrix.h"
#include "ui/icons.h"
#include "ui/raw_tx.h"
#include "ui/theme.h"

namespace
{

using TxClock = std::chrono::steady_clock;
constexpr std::chrono::seconds max_rx_age{1};

[[nodiscard]] std::string iface_label(const std::deque<Iface>& ifaces, uint16_t index)
{
    return index < ifaces.size() ? ifaces[index].info.name : std::format("#{}", index);
}

// Caller holds gen.mutex. Starting needs a running measurement and a timed trigger (a Manual row
// only sends on its Send button); stopping always works.
void set_enabled(TxGenerator& gen, TxCyclic& row, bool enabled, bool measuring)
{
    if (enabled && (!measuring || row.trigger == TxTrigger::Manual))
    {
        return;
    }
    if (enabled && !row.enabled)
    {
        row.next_due = {}; // fresh schedule, first frame at once
        row.pending = false;
    }
    row.enabled = enabled;
    gen.changed = true;
}

// Caller holds gen.mutex. A shorter cycle takes effect now, not after the old deadline.
void set_interval(TxGenerator& gen, TxCyclic& row, int interval_ms)
{
    row.interval_ms = std::clamp(interval_ms, 1, 60000); // as the Interval field under the table
    const auto latest = TxClock::now() + std::chrono::milliseconds(row.interval_ms);
    if (row.next_due > latest)
    {
        row.next_due = latest;
    }
    gen.changed = true;
}

void send_once(App& app, const TxCyclic& row)
{
    if (row.msg.iface >= app.ifaces.size() || !iface_send(app.ifaces[row.msg.iface], row.msg))
    {
        log_error(std::format("TxGenerator: Interface {} is not open.", iface_label(app.ifaces, row.msg.iface)));
    }
}

// The row's ▶/■ (and Space on it): applies to the whole selection when the row is part of it.
void toggle_run(TxGenerator& gen, TxCyclic& row, bool measuring)
{
    const bool target = !row.enabled;
    if (row.selected)
    {
        for (TxCyclic& r : gen.rows)
        {
            if (r.selected)
            {
                set_enabled(gen, r, target, measuring);
            }
        }
    }
    else
    {
        set_enabled(gen, row, target, measuring);
    }
}

[[nodiscard]] bool enter_pressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
}

// Rows an action applies to: the selected ones, else the current one.
template <class F>
void for_targets(TxGenerator& gen, F&& fn)
{
    bool any = false;
    for (TxCyclic& row : gen.rows)
    {
        if (row.selected)
        {
            fn(row);
            any = true;
        }
    }
    if (!any && gen.current >= 0 && gen.current < static_cast<int>(gen.rows.size()))
    {
        fn(gen.rows[static_cast<std::size_t>(gen.current)]);
    }
}

// A button in a theme_*_button() style; disabled (on == false) it is the theme's normal disabled button.
bool themed_button(const char* label, const ThemeButton& c, bool on = true)
{
    if (!on)
    {
        return ImGui::Button(label);
    }
    theme_push_button(c);
    const bool pressed = ImGui::Button(label);
    theme_pop_button();
    return pressed;
}

// Outlined ▶ (Kraken style, start) / ■ (Stop style) toggle, filled on hover like the Qt buttons.
bool status_button(bool running)
{
    const float h = ImGui::GetFrameHeight();
    const ImVec2 size(h * 1.6f, h);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##status", size);
    const bool hovered = ImGui::IsItemHovered(); // false while disabled
    const ThemeButton t = running ? theme_stop_button() : theme_kraken_button();
    const auto col = [alpha = ImGui::GetStyle().Alpha](unsigned hex)
    {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(theme_u32(hex));
        c.w = alpha;
        return ImGui::ColorConvertFloat4ToU32(c);
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (hovered)
    {
        dl->AddRectFilled(a, b, col(t.hover), rounding);
    }
    dl->AddRect(a, b, col(t.border), rounding);
    const ImU32 glyph = col(hovered ? t.text : t.border);
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float r = h * 0.22f;
    if (running)
    {
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), glyph);
    }
    else
    {
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 1.15f), ImVec2(c.x - r, c.y + r * 1.15f), ImVec2(c.x + r * 1.2f, c.y), glyph);
    }
    ImGui::SetItemTooltip(running ? "Stop" : "Start");
    return pressed;
}

constexpr const char* trigger_names[] = {"Cyclic", "Manual", "On receive"};

// One signal's physical value: a combo for value tables and multiplexers, else an input
// clamped to the DBC range. Encodes into msg only when the value changes.
void draw_signal_value(const CanDbMessage& db, const CanDbSignal& sig, BusMessage& msg)
{
    const uint64_t raw = can_signal_extract_raw(sig, msg);
    char buf[32];
    const auto fmt = [&buf](uint64_t v)
    {
        *std::format_to_n(buf, sizeof(buf) - 1, "{}", v).out = '\0';
        return buf;
    };
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!sig.value_table.empty())
    {
        const auto it = sig.value_table.find(raw);
        if (ImGui::BeginCombo("##v", it != sig.value_table.end() ? it->second.c_str() : fmt(raw)))
        {
            for (const auto& [value, name] : sig.value_table)
            {
                if (ImGui::Selectable(name.c_str(), value == raw) && value != raw)
                {
                    can_signal_inject_raw(sig, msg, value);
                }
            }
            ImGui::EndCombo();
        }
        return;
    }
    if (sig.is_muxer)
    {
        if (ImGui::BeginCombo("##v", fmt(raw)))
        {
            for (auto s = db.signals.begin(); s != db.signals.end(); ++s)
            {
                const bool first = s->is_muxed && std::none_of(db.signals.begin(), s, [&](const CanDbSignal& o)
                                                                { return o.is_muxed && o.mux_value == s->mux_value; });
                if (first && ImGui::Selectable(fmt(s->mux_value), s->mux_value == raw) && s->mux_value != raw)
                {
                    can_signal_inject_raw(sig, msg, s->mux_value);
                }
            }
            ImGui::EndCombo();
        }
        return;
    }
    double value = can_signal_raw_to_physical(sig, raw);
    if (ImGui::InputDouble("##v", &value, 0.0, 0.0, "%g"))
    {
        tx_signal_set(sig, msg, value);
    }
}

// Expanded row: OnReceive settings and the signals of the DBC message (only the ones the
// current multiplexer value selects).
void draw_row_details(App& app, TxGenerator& gen, TxCyclic& row)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    if (row.trigger == TxTrigger::OnReceive)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("On");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f * px);
        gen.rx_dirty |= draw_iface_combo("##rx_iface", app, row.rx_iface);
        same_line_or_wrap(90.0f * px);
        ImGui::SetNextItemWidth(90.0f * px);
        if (ImGui::InputScalar("##rx_id", ImGuiDataType_U32, &row.rx_id, nullptr, nullptr, "%03X",
                               ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase))
        {
            row.rx_id &= row.rx_extended ? can_id_mask_extended : can_id_mask_standard;
            gen.rx_dirty = true;
        }
        ImGui::SetItemTooltip("Received ID (hex)");
        ImGui::SameLine();
        if (ImGui::Checkbox("Ext", &row.rx_extended))
        {
            row.rx_id &= row.rx_extended ? can_id_mask_extended : can_id_mask_standard;
            gen.rx_dirty = true;
        }
        same_line_or_wrap(ImGui::CalcTextSize("Delay (ms)").x + 90.0f * px);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Delay (ms)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f * px);
        if (ImGui::InputInt("##delay", &row.delay_ms, 0))
        {
            row.delay_ms = std::clamp(row.delay_ms, 0, 600000);
        }
    }
    if (row.db == nullptr || row.db->signals.empty())
    {
        ImGui::TextDisabled("No DBC signals: edit the bytes in Message View (double-click the row).");
        return;
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##signals", 3, flags))
    {
        ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        int i = 0;
        for (const CanDbSignal& sig : row.db->signals)
        {
            ++i;
            if (!can_signal_present(*row.db, sig, row.msg)) // inactive mux branch, or beyond the DLC
            {
                continue;
            }
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(sig.name.c_str());
            if (!sig.comment.empty())
            {
                ImGui::SetItemTooltip("%s", sig.comment.c_str());
            }
            ImGui::TableNextColumn();
            draw_signal_value(*row.db, sig, row.msg);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(sig.unit.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

TxCyclic row_from_db(const CanDbMessage& db, uint16_t iface)
{
    TxCyclic row{.name = db.name, .db = &db};
    row.msg.id = db.raw_id & can_id_mask_extended;
    row.msg.flags = (db.raw_id & 0x80000000u) != 0 ? bus_flag::extended : 0;
    set_length(row.msg, db.dlc);
    if (row.msg.len > 8)
    {
        row.msg.flags |= bus_flag::fd;
    }
    row.msg.iface = iface;
    return row;
}

void draw_available(App& app, TxGenerator& gen)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    ImGui::SeparatorText("Available Messages (DBC)");

    // DBC messages of the networks that use the selected interface; when none does (no interface
    // yet, or a DBC in a network without interfaces) every network's, so there is something to
    // add and edit in Message View.
    const auto uses_iface = [&](const SetupNetwork& net)
    {
        return std::ranges::any_of(net.interfaces, [&](const SetupInterface& si)
                                   { return ifaces_find(app.ifaces, si.driver, si.name) == gen.iface; });
    };
    const bool any_network = std::ranges::any_of(app.setup.networks, uses_iface);
    std::vector<const CanDbMessage*> messages;
    for (const SetupNetwork& net : app.setup.networks)
    {
        if (any_network && !uses_iface(net))
        {
            continue;
        }
        for (const auto& db : net.can_dbs)
        {
            for (const auto& [raw_id, msg] : db->messages)
            {
                messages.push_back(&msg);
            }
        }
    }
    std::erase_if(gen.avail_selected, [&](const CanDbMessage* m) { return std::ranges::find(messages, m) == messages.end(); });
    if (std::ranges::find(messages, gen.layout_msg) == messages.end())
    {
        gen.layout_msg = nullptr;
    }

    // The controls below wrap on narrow windows, so leave room for last frame's height.
    const float footer = gen.avail_footer > 0.0f ? gen.avail_footer : ImGui::GetFrameHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##avail_tabs"))
    {
        if (ImGui::BeginTabItem("Message List"))
        {
            if (std::exchange(gen.focus_search, false))
            {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##search", "Message Name or ID...", gen.search, sizeof(gen.search));
            const std::string_view needle = gen.search;
            std::vector<const CanDbMessage*> shown; // the rows that pass the search, for vim_nav's count
            shown.reserve(messages.size());
            std::string hay;
            std::vector<std::pair<int, const CanDbMessage*>> scored; // fzf-style, as the signal search
            for (const CanDbMessage* m : messages)
            {
                hay.clear();
                std::format_to(std::back_inserter(hay), "0x{:03x} {}", m->raw_id & can_id_mask_extended, m->name);
                if (const int score = fuzzy_score(needle, hay); score >= 0)
                {
                    scored.emplace_back(score, m);
                }
            }
            // Best match first; equal scores (and an empty search) keep the database order.
            std::ranges::stable_sort(scored, std::greater{}, &std::pair<int, const CanDbMessage*>::first);
            for (const auto& [score, m] : scored)
            {
                shown.push_back(m);
            }
            // j/k/gg/G move the cursor (layout_msg) and select it alone; Enter adds it.
            const int count = static_cast<int>(shown.size());
            int cur = static_cast<int>(std::ranges::find(shown, gen.layout_msg) - shown.begin());
            cur = cur < count ? cur : -1;
            bool focus_search = false;
            int h_delta = 0;
            const int page = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().y / ImGui::GetTextLineHeightWithSpacing()));
            const bool moved = focused && vim_nav(gen.vim_avail, cur, count, page, focus_search, h_delta);
            gen.focus_search |= focus_search;
            if (moved)
            {
                gen.layout_msg = shown[static_cast<std::size_t>(cur)];
                gen.avail_selected.assign(1, gen.layout_msg);
            }
            const CanDbMessage* add = nullptr;
            if (focused && cur >= 0 && !ImGui::GetIO().WantTextInput && enter_pressed())
            {
                add = shown[static_cast<std::size_t>(cur)];
            }
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                              | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##avail", 2, flags, ImVec2(0.0f, -footer)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 90.0f * px);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const CanDbMessage* m : shown)
                {
                    const std::string id = std::format("0x{:03X}", m->raw_id & can_id_mask_extended);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool selected = std::ranges::find(gen.avail_selected, m) != gen.avail_selected.end();
                    ImGui::PushID(m);
                    if (ImGui::Selectable(id.c_str(), selected,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        if (ImGui::GetIO().KeyCtrl)
                        {
                            if (selected)
                            {
                                std::erase(gen.avail_selected, m);
                            }
                            else
                            {
                                gen.avail_selected.push_back(m);
                            }
                        }
                        else
                        {
                            gen.avail_selected.assign(1, m);
                        }
                        gen.layout_msg = m;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            add = m;
                        }
                    }
                    if (moved && m == gen.layout_msg)
                    {
                        ImGui::SetScrollHereY();
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(m->name.c_str());
                }
                ImGui::EndTable();
                if (add != nullptr)
                {
                    gen.rows.push_back(row_from_db(*add, gen.iface));
                    gen.rx_dirty = true;
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layout View"))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Zoom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f * px);
            ImGui::SliderInt("##zoom", &gen.zoom, 30, 120);
            if (ImGui::BeginChild("##layout", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
            {
                draw_bit_matrix(gen.layout_msg, static_cast<float>(gen.zoom));
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    const float footer_top = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(gen.avail_selected.empty());
    if (ImGui::Button("Add DBC Message"))
    {
        for (const CanDbMessage* m : gen.avail_selected)
        {
            gen.rows.push_back(row_from_db(*m, gen.iface));
        }
        gen.rx_dirty = true;
    }
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("ID (Hex):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * px);
    ImGui::InputScalar("##manual_id", ImGuiDataType_U32, &gen.manual_id, nullptr, nullptr, "%03X",
                       ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
    gen.manual_id = std::min(gen.manual_id, can_id_mask_extended);
    const ImGuiStyle& style = ImGui::GetStyle();
    same_line_or_wrap(ImGui::CalcTextSize("DLC:").x + style.ItemSpacing.x + 90.0f * px);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DLC:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * px);
    ImGui::InputInt("##manual_dlc", &gen.manual_dlc);
    gen.manual_dlc = std::clamp(gen.manual_dlc, 0, bus_max_data_bytes);
    same_line_or_wrap(button_width("Add Manual"));
    if (ImGui::Button("Add Manual"))
    {
        TxCyclic row{.name = "Manual", .interval_ms = gen.interval_ms};
        row.msg.id = gen.manual_id;
        row.msg.flags = static_cast<uint16_t>((gen.manual_id > can_id_mask_standard ? bus_flag::extended : 0)
                                              | (gen.manual_dlc > 8 ? bus_flag::fd : 0));
        // Round up to a valid CAN FD length (e.g. 10 -> 12).
        set_length(row.msg, bus_dlc_lengths[bus_length_to_dlc(gen.manual_dlc)]);
        row.msg.iface = gen.iface;
        gen.rows.push_back(row);
        gen.rx_dirty = true;
    }
    gen.avail_footer = ImGui::GetCursorPosY() - footer_top;
}

void draw_active(App& app, TxGenerator& gen, bool root_focused)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SeparatorText("Active Cyclic Transmissions");
    // Keys go here when this box or the bare Generator window (e.g. after Ctrl+w) has focus:
    // j/k/gg/G move the current row (selected alone), Enter expands it, Space is its ▶/■.
    const bool focused = root_focused || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    const int count = static_cast<int>(gen.rows.size());
    int cur = gen.current < count ? gen.current : -1;
    bool focus_search = false;
    int h_delta = 0;
    const int page = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().y / ImGui::GetFrameHeightWithSpacing()));
    const bool moved = focused && vim_nav(gen.vim_active, cur, count, page, focus_search, h_delta);
    gen.focus_search |= focus_search;
    if (moved)
    {
        for (TxCyclic& r : gen.rows)
        {
            r.selected = false;
        }
        TxCyclic& row = gen.rows[static_cast<std::size_t>(cur)];
        row.selected = true;
        gen.current = cur;
        gen.interval_ms = row.interval_ms;
    }
    if (focused && cur >= 0 && !ImGui::GetIO().WantTextInput)
    {
        TxCyclic& row = gen.rows[static_cast<std::size_t>(cur)];
        if (enter_pressed())
        {
            row.expanded = !row.expanded;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && row.trigger != TxTrigger::Manual)
        {
            toggle_run(gen, row, app.measuring);
        }
    }
    bool open_edit = false; // OpenPopup must run in the popup's ID scope, outside the table
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_Resizable;
    const float footer = gen.active_footer > 0.0f ? gen.active_footer : ImGui::GetFrameHeightWithSpacing();
    const float table_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x; // for the details rows
    if (ImGui::BeginTable("##active", 7, flags, ImVec2(0.0f, -footer)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 50.0f * px);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 90.0f * px);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthFixed, 110.0f * px);
        ImGui::TableSetupColumn("DLC", ImGuiTableColumnFlags_WidthFixed, 40.0f * px);
        ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, 100.0f * px);
        ImGui::TableSetupColumn("Interval (ms)", ImGuiTableColumnFlags_WidthFixed, 100.0f * px);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < gen.rows.size(); ++i)
        {
            TxCyclic& row = gen.rows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (row.trigger == TxTrigger::Manual)
            {
                ImGui::BeginDisabled(!app.measuring);
                if (ImGui::Button("Send"))
                {
                    send_once(app, row);
                }
                ImGui::EndDisabled();
            }
            ImGui::BeginDisabled(!row.enabled && !app.measuring);
            if (row.trigger != TxTrigger::Manual && status_button(row.enabled))
            {
                toggle_run(gen, row, app.measuring);
            }
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            const std::string id = std::format("0x{:03X}", row.msg.id);
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(id.c_str(), row.selected,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
                                      | ImGuiSelectableFlags_AllowDoubleClick))
            {
                // Click selects one row, Ctrl+click toggles (ExtendedSelection without Shift ranges).
                if (ImGui::GetIO().KeyCtrl)
                {
                    row.selected = !row.selected;
                }
                else
                {
                    for (TxCyclic& other : gen.rows)
                    {
                        other.selected = &other == &row;
                    }
                }
                gen.current = static_cast<int>(i);
                gen.interval_ms = row.interval_ms;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    gen.edit_row = static_cast<int>(i);
                    gen.edit_msg = row.msg;
                    open_edit = true;
                }
            }
            if (moved && static_cast<int>(i) == gen.current)
            {
                ImGui::SetScrollHereY();
            }
            ImGui::TableNextColumn();
            if (ImGui::ArrowButton("##expand", row.expanded ? ImGuiDir_Down : ImGuiDir_Right))
            {
                row.expanded = !row.expanded;
            }
            ImGui::SetItemTooltip("Signals and trigger settings");
            ImGui::SameLine();
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.msg.iface < app.ifaces.size() ? app.ifaces[row.msg.iface].info.name.c_str() : "Unknown");
            ImGui::TableNextColumn();
            ImGui::Text("%u", row.msg.len);
            ImGui::TableNextColumn();
            int trigger = static_cast<int>(row.trigger);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##trigger", &trigger, trigger_names, IM_ARRAYSIZE(trigger_names)))
            {
                row.trigger = static_cast<TxTrigger>(trigger);
                row.pending = false;
                if (row.trigger == TxTrigger::OnReceive)
                {
                    row.expanded = true; // its settings are there
                    row.rx_iface = row.rx_iface == UINT16_MAX ? row.msg.iface : row.rx_iface;
                }
                row.enabled = false; // a new trigger starts stopped, only RUN / ▶ send
                gen.changed = true;
                gen.rx_dirty = true;
            }
            ImGui::TableNextColumn();
            int interval = row.interval_ms;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(row.trigger != TxTrigger::Cyclic);
            const bool interval_edited = ImGui::InputInt("##interval", &interval, 0);
            ImGui::EndDisabled();
            if (interval_edited && interval > 0)
            {
                // Applies to the whole selection when the edited row is part of it.
                if (row.selected)
                {
                    for (TxCyclic& r : gen.rows)
                    {
                        if (r.selected)
                        {
                            set_interval(gen, r, interval);
                        }
                    }
                }
                else
                {
                    set_interval(gen, row, interval);
                }
            }
            if (row.expanded)
            {
                // The details span the whole row (tables have no column span): a child from the
                // first cell to the table's right edge, with the cell clipping widened to match.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float width = std::max(table_right - at.x - ImGui::GetStyle().CellPadding.x * 2.0f, 1.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImGui::PushClipRect({at.x, dl->GetClipRectMin().y}, {at.x + width, dl->GetClipRectMax().y}, false);
                if (ImGui::BeginChild("##details", {width, 0.0f}, ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground))
                {
                    draw_row_details(app, gen, row);
                }
                ImGui::EndChild();
                ImGui::PopClipRect();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Edit dialog (double-click on a row): the Message View editor on a copy, OK applies it.
    const float h = ImGui::GetFrameHeightWithSpacing();
    if (open_edit)
    {
        ImGui::OpenPopup("Edit Message###edit");
    }
    ImGui::SetNextWindowSize(ImVec2(750.0f * px, 480.0f * px), ImGuiCond_Appearing);
    bool edit_open = true; // the title bar's X = Cancel
    if (ImGui::BeginPopupModal("Edit Message###edit", &edit_open))
    {
        const bool valid = gen.edit_row >= 0 && gen.edit_row < static_cast<int>(gen.rows.size());
        if (valid)
        {
            const TxCyclic& row = gen.rows[static_cast<std::size_t>(gen.edit_row)];
            ImGui::Text("Edit Message: 0x%03X - %s", row.msg.id, row.name.c_str());
            if (ImGui::BeginChild("##editor", ImVec2(0.0f, -h)))
            {
                draw_raw_tx(app, gen.edit_msg, row.db);
            }
            ImGui::EndChild();
        }
        if (ImGui::Button("OK") && valid)
        {
            gen.rows[static_cast<std::size_t>(gen.edit_row)].msg = gen.edit_msg;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::Shortcut(ImGuiKey_Escape) || !valid)
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Controls row, wrapped on narrow windows
    const float footer_top = ImGui::GetCursorPosY();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::BeginDisabled(!app.measuring);
    if (themed_button("RUN", theme_kraken_button(), app.measuring))
    {
        for_targets(gen, [&](TxCyclic& r) { set_enabled(gen, r, true, app.measuring); });
    }
    ImGui::EndDisabled();
    same_line_or_wrap(button_width("STOP"));
    if (themed_button("STOP", theme_stop_button()))
    {
        for_targets(gen, [&](TxCyclic& r) { set_enabled(gen, r, false, app.measuring); });
    }
    same_line_or_wrap(button_width("Randomize Data"));
    if (ImGui::Button("Randomize Data"))
    {
        static std::mt19937 rng{std::random_device{}()};
        for_targets(gen, [&](TxCyclic& r) { std::generate_n(r.msg.data.begin(), r.msg.len, [&] { return static_cast<uint8_t>(rng()); }); });
    }
    ImGui::SetItemTooltip("Randomize data bytes for selected messages");
    same_line_or_wrap(ImGui::CalcTextSize("Interval:").x + style.ItemSpacing.x + 110.0f * px);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Interval:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f * px);
    if (ImGui::InputInt("##interval_all", &gen.interval_ms, 10, 100))
    {
        gen.interval_ms = std::clamp(gen.interval_ms, 1, 60000);
        for_targets(gen, [&](TxCyclic& r) { set_interval(gen, r, gen.interval_ms); });
    }
    same_line_or_wrap(button_width("Send"));
    ImGui::BeginDisabled(!app.measuring);
    if (ImGui::Button("Send"))
    {
        for_targets(gen, [&](TxCyclic& r) { send_once(app, r); });
    }
    ImGui::EndDisabled();
    same_line_or_wrap(button_width("Select All Rows"));
    if (ImGui::Button("Select All Rows"))
    {
        std::ranges::for_each(gen.rows, [](TxCyclic& r) { r.selected = true; });
    }
    same_line_or_wrap(button_width("Deselect All"));
    if (ImGui::Button("Deselect All"))
    {
        std::ranges::for_each(gen.rows, [](TxCyclic& r) { r.selected = false; });
    }
    same_line_or_wrap(button_width("Remove"));
    if (ImGui::Button("Remove"))
    {
        const bool any = std::ranges::any_of(gen.rows, [](const TxCyclic& r) { return r.selected; });
        if (any)
        {
            std::erase_if(gen.rows, [](const TxCyclic& r) { return r.selected; });
        }
        else if (gen.current >= 0 && gen.current < static_cast<int>(gen.rows.size()))
        {
            gen.rows.erase(gen.rows.begin() + gen.current);
        }
        gen.current = -1;
        gen.edit_row = -1;
        gen.changed = true;
        gen.rx_dirty = true;
    }
    gen.active_footer = ImGui::GetCursorPosY() - footer_top;
}

} // namespace

void tx_signal_set(const CanDbSignal& sig, BusMessage& msg, double physical) noexcept
{
    if (sig.max > sig.min)
    {
        physical = std::clamp(physical, sig.min, sig.max);
    }
    can_signal_inject_physical(sig, msg, physical);
}

void tx_generator_on_rx(TxGenerator& gen, std::span<const BusMessage> msgs, TxClock::time_point now, int64_t wall_now_ns)
{
    if (msgs.empty())
    {
        return;
    }
    std::scoped_lock lock(gen.mutex);
    if (gen.rx_dirty)
    {
        gen.rx_dirty = false;
        gen.rx_index.clear(); // keeps its buckets
        for (std::size_t i = 0; i < gen.rows.size(); ++i)
        {
            const TxCyclic& row = gen.rows[i];
            if (row.trigger == TxTrigger::OnReceive)
            {
                gen.rx_index.emplace(tx_rx_key(row.rx_iface, row.rx_id, row.rx_extended), i);
            }
        }
    }
    if (gen.rx_index.empty())
    {
        return;
    }
    bool armed = false;
    for (const BusMessage& m : msgs)
    {
        if (has_flag(m, bus_flag::tx) || is_error_frame(m))
        {
            continue; // our own sends must not re-trigger
        }
        const auto [first, last] = gen.rx_index.equal_range(tx_rx_key(m.iface, m.id, has_flag(m, bus_flag::extended)));
        for (auto it = first; it != last; ++it)
        {
            TxCyclic& row = gen.rows[it->second];
            // ponytail: one pending send per row, frames arriving within the delay merge into it;
            // a small queue of deadlines if every trigger must answer.
            if (row.enabled && !row.pending)
            {
                row.pending = true;
                // The delay runs from reception, not from this UI frame (up to a frame later).
                // An age beyond max_rx_age means the timestamp is not live (replayed file): ignore it.
                std::chrono::nanoseconds age{wall_now_ns - m.ts_ns};
                if (age < std::chrono::nanoseconds::zero() || age > max_rx_age)
                {
                    age = {};
                }
                row.next_due = now + std::chrono::milliseconds(row.delay_ms)
                               - std::chrono::duration_cast<TxClock::duration>(age);
                armed = true;
            }
        }
    }
    if (armed)
    {
        gen.changed = true;
        gen.wake.notify_one();
    }
}

void tx_sender_loop(std::stop_token stop, TxGenerator& gen)
{
    std::unique_lock lock(gen.mutex);
    while (!stop.stop_requested())
    {
        const auto now = TxClock::now();
        std::optional<TxClock::time_point> earliest;
        for (TxCyclic& row : gen.rows)
        {
            if (!row.enabled || row.trigger == TxTrigger::Manual || (row.trigger == TxTrigger::OnReceive && !row.pending))
            {
                continue;
            }
            const auto period = std::chrono::milliseconds(std::max(1, row.interval_ms));
            if (row.next_due == TxClock::time_point{})
            {
                row.next_due = now;
            }
            if (row.next_due <= now)
            {
                Iface* iface = gen.ifaces != nullptr && row.msg.iface < gen.ifaces->size() ? &(*gen.ifaces)[row.msg.iface] : nullptr;
                if (iface == nullptr || !iface_send(*iface, row.msg))
                {
                    // Disabled to avoid error spam; the user has to start it again.
                    row.enabled = false;
                    log_error(std::format("TxGenerator: Interface {} is not open.",
                                          iface != nullptr ? iface->info.name : std::to_string(row.msg.iface)));
                    continue;
                }
                if (row.trigger == TxTrigger::OnReceive)
                {
                    row.pending = false; // one shot per received frame
                    continue;
                }
                // Advance from the deadline, not from the send time, so latency cannot drift
                // the cycle; more than a period behind: skip the missed slots, no burst.
                row.next_due += period;
                if (row.next_due <= now)
                {
                    row.next_due += period * ((now - row.next_due) / period + 1);
                }
            }
            if (!earliest || row.next_due < *earliest)
            {
                earliest = row.next_due;
            }
        }
        gen.changed = false;
        const auto woken = [&gen] { return gen.changed; };
        if (earliest)
        {
            gen.wake.wait_until(lock, stop, *earliest, woken);
        }
        else
        {
            gen.wake.wait(lock, stop, woken);
        }
    }
}

void tx_generator_start(TxGenerator& gen, std::deque<Iface>& ifaces)
{
    if (!gen.sender.joinable())
    {
        gen.ifaces = &ifaces;
        platform_fine_timers();
        gen.sender = std::jthread([&gen](std::stop_token stop) { tx_sender_loop(stop, gen); });
    }
}

void tx_generator_stop_all(TxGenerator& gen)
{
    std::scoped_lock lock(gen.mutex);
    for (TxCyclic& row : gen.rows)
    {
        row.enabled = false;
    }
    gen.changed = true;
    gen.wake.notify_one();
}

void draw_tx_generator(App& app, const WorkspaceTab& tab, TxGenerator& gen)
{
    tx_generator_start(gen, app.ifaces);
    // ponytail: the rows stay locked while both windows are drawn, which can delay a due
    // frame by one table draw (well under a millisecond); copy the rows out if that shows.
    std::scoped_lock lock(gen.mutex);

    // Re-linked every frame: a replaced setup (workspace load, setup dialog) frees the old
    // CanDbMessage objects, and an edited ID should show its own layout (Qt: resolveDbMessages).
    for (TxCyclic& row : gen.rows)
    {
        row.db = setup_find_can_message(app.setup, row.msg);
    }

    if (gen.iface >= app.ifaces.size())
    {
        for (const SetupNetwork& net : app.setup.networks)
        {
            for (const SetupInterface& si : net.interfaces)
            {
                if (const int index = ifaces_find(app.ifaces, si.driver, si.name); index >= 0 && gen.iface >= app.ifaces.size())
                {
                    gen.iface = static_cast<uint16_t>(index);
                }
            }
        }
    }

    if (ImGui::Begin(workspace_window_name(tab, "Generator View").c_str()))
    {
        const float px = ImGui::GetFontSize() / 15.0f;
        const bool root_focused = ImGui::IsWindowFocused(); // the window itself, none of its children
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Interface:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::min(220.0f * px, ImGui::GetContentRegionAvail().x));
        draw_iface_combo("##gen_iface", app, gen.iface);

        // Side by side like the Qt window; stacked when too narrow for both control rows (T48),
        // the list then gets at least room for a few rows above its wrapped controls.
        // Separate ids: the resizable side-by-side child keeps its first size per id.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool stacked = avail.x < 700.0f * px;
        const float list_h = std::max(avail.y * 0.45f, ImGui::GetFrameHeightWithSpacing() * 8.0f + gen.avail_footer);
        const bool open = stacked ? ImGui::BeginChild("##available_v", ImVec2(0.0f, list_h), ImGuiChildFlags_Borders)
                                  : ImGui::BeginChild("##available", ImVec2(avail.x * 0.4f, 0.0f),
                                                      ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        if (open)
        {
            draw_available(app, gen);
        }
        ImGui::EndChild();
        if (!stacked)
        {
            ImGui::SameLine();
        }
        if (ImGui::BeginChild("##active_box", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
        {
            draw_active(app, gen, root_focused);
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // Message View: the RawTx editor on the generator's current row, edited live.
    if (ImGui::Begin(workspace_window_name(tab, "Message View").c_str()))
    {
        const bool valid = gen.current >= 0 && gen.current < static_cast<int>(gen.rows.size());
        BusMessage none;
        ImGui::BeginDisabled(!valid);
        if (valid)
        {
            TxCyclic& row = gen.rows[static_cast<std::size_t>(gen.current)];
            draw_raw_tx(app, row.msg, row.db);
        }
        else
        {
            draw_raw_tx(app, none, nullptr);
        }
        ImGui::EndDisabled();
    }
    ImGui::End();

    if (gen.changed)
    {
        gen.wake.notify_one();
    }
}

void tx_generator_save_xml(const TxGenerator& gen, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    for (const TxCyclic& row : gen.rows)
    {
        const BusMessage& m = row.msg;
        std::string data;
        append_hex_bytes(data, std::span(m.data).first(m.len), " ");
        pugi::xml_node f = el.append_child("frame");
        f.append_attribute("id") = std::format("0x{:X}", m.id).c_str();
        f.append_attribute("name") = row.name.c_str();
        f.append_attribute("dlc") = m.len;
        f.append_attribute("extended") = has_flag(m, bus_flag::extended) ? 1 : 0;
        f.append_attribute("fd") = has_flag(m, bus_flag::fd) ? 1 : 0;
        f.append_attribute("brs") = has_flag(m, bus_flag::brs) ? 1 : 0;
        f.append_attribute("interval") = row.interval_ms;
        f.append_attribute("interface") = m.iface < ifaces.size() ? ifaces[m.iface].info.name.c_str() : "";
        f.append_attribute("driver") = m.iface < ifaces.size() && ifaces[m.iface].ops != nullptr ? ifaces[m.iface].ops->name : "";
        f.append_attribute("data") = data.c_str();
        if (row.trigger != TxTrigger::Cyclic)
        {
            f.append_attribute("trigger") = row.trigger == TxTrigger::Manual ? "manual" : "receive";
        }
        if (row.trigger == TxTrigger::OnReceive)
        {
            f.append_attribute("rx_id") = std::format("0x{:X}", row.rx_id).c_str();
            f.append_attribute("rx_extended") = row.rx_extended ? 1 : 0;
            f.append_attribute("rx_interface") = row.rx_iface < ifaces.size() ? ifaces[row.rx_iface].info.name.c_str() : "";
            f.append_attribute("rx_driver") = row.rx_iface < ifaces.size() && ifaces[row.rx_iface].ops != nullptr ? ifaces[row.rx_iface].ops->name : "";
            f.append_attribute("delay") = row.delay_ms;
        }
    }
}

void tx_generator_load_xml(TxGenerator& gen, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    std::scoped_lock lock(gen.mutex);
    gen.rows.clear();
    gen.current = -1;
    gen.edit_row = -1;
    for (const pugi::xml_node f : el.children("frame"))
    {
        TxCyclic row{.name = f.attribute("name").as_string(), .interval_ms = std::max(1, f.attribute("interval").as_int(100))};
        BusMessage& m = row.msg;
        m.id = static_cast<uint32_t>(std::strtoul(f.attribute("id").as_string(), nullptr, 16)) & can_id_mask_extended;
        m.flags = static_cast<uint16_t>((f.attribute("extended").as_int() != 0 ? bus_flag::extended : 0)
                                        | (f.attribute("fd").as_int() != 0 ? bus_flag::fd : 0)
                                        | (f.attribute("brs").as_int() != 0 ? bus_flag::brs : 0));
        set_length(m, std::clamp(f.attribute("dlc").as_int(), 0, bus_max_data_bytes));
        // Qt files have no driver attribute: match the interface name on any driver then.
        // Unresolved = UINT16_MAX: sending logs an error and stops the row.
        const auto resolve = [&ifaces](std::string_view name, std::string_view driver)
        {
            for (std::size_t i = 0; i < ifaces.size(); ++i)
            {
                if (ifaces[i].info.name == name && (driver.empty() || (ifaces[i].ops != nullptr && driver == ifaces[i].ops->name)))
                {
                    return static_cast<uint16_t>(i);
                }
            }
            return static_cast<uint16_t>(UINT16_MAX);
        };
        m.iface = resolve(f.attribute("interface").as_string(), f.attribute("driver").as_string());
        const std::string_view trigger = f.attribute("trigger").as_string();
        row.trigger = trigger == "manual" ? TxTrigger::Manual : trigger == "receive" ? TxTrigger::OnReceive : TxTrigger::Cyclic;
        row.rx_extended = f.attribute("rx_extended").as_int() != 0;
        row.rx_id = static_cast<uint32_t>(std::strtoul(f.attribute("rx_id").as_string(), nullptr, 16))
                    & (row.rx_extended ? can_id_mask_extended : can_id_mask_standard);
        row.rx_iface = row.trigger == TxTrigger::OnReceive
                           ? resolve(f.attribute("rx_interface").as_string(), f.attribute("rx_driver").as_string())
                           : UINT16_MAX;
        row.delay_ms = std::clamp(f.attribute("delay").as_int(), 0, 600000);
        std::string_view data = f.attribute("data").as_string();
        for (std::size_t i = 0; i < m.len && !data.empty(); ++i)
        {
            const auto sp = data.find(' ');
            m.data[i] = static_cast<uint8_t>(std::strtoul(std::string(data.substr(0, sp)).c_str(), nullptr, 16));
            data = sp == std::string_view::npos ? std::string_view{} : data.substr(sp + 1);
        }
        gen.rows.push_back(row); // enabled = false: never auto-start on load
    }
    gen.changed = true;
    gen.rx_dirty = true;
    gen.wake.notify_one();
}
