#include "ui/instrument_panel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <numeric>
#include <string>
#include <string_view>

#include <imgui.h>
#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>
#include <pugixml.hpp>

#include "app.h"
#include "core/setup.h"
#include "core/trace.h"
#include "db/model/can_db.h"
#include "drivers/driver.h"
#include "ui/depth_gauge.h"
#include "ui/raw_tx.h"
#include "ui/workspace_tabs.h"

namespace
{

constexpr std::array<const char*, static_cast<std::size_t>(InstrumentKind::Count)> kind_names = {
    "Gauge", "Bar", "LED", "Numeric", "Text", "Trend", "Button", "Slider", "Checkbox", "Knob"};
constexpr std::array<const char*, 3> led_names = {"nonzero", "above", "below"};

constexpr ImU32 red_zone = IM_COL32(0xe0, 0x40, 0x40, 255);
// Instrument plate: deep-sea petrol, lighter at the top; sea-foam text.
constexpr ImU32 plate_top = IM_COL32(0x0e, 0x2e, 0x36, 255);
constexpr ImU32 plate_bottom = IM_COL32(0x06, 0x16, 0x1b, 255);
constexpr ImU32 plate_well = IM_COL32(0x03, 0x0c, 0x10, 255); // bar / LED background
constexpr ImU32 plate_text = IM_COL32(0xe6, 0xf4, 0xf1, 255);
constexpr ImU32 plate_text_dim = IM_COL32(0x8f, 0xb5, 0xb0, 255);

float fraction(const Instrument& inst, double v)
{
    return inst.max > inst.min ? static_cast<float>(std::clamp((v - inst.min) / (inst.max - inst.min), 0.0, 1.0)) : 0.0f;
}

const std::string& title(const Instrument& inst)
{
    return inst.label.empty() ? inst.signal : inst.label;
}

std::string value_text(const Instrument& inst)
{
    if (!inst.has_value)
    {
        return "-";
    }
    const std::string_view unit = inst.sig != nullptr ? std::string_view(inst.sig->unit) : std::string_view{};
    const std::string v = inst.sig != nullptr ? can_signal_format(*inst.sig, inst.value) : std::format("{:.6g}", inst.value);
    return unit.empty() ? v : std::format("{} {}", v, unit);
}

bool led_lit(const Instrument& inst)
{
    switch (inst.led)
    {
    case LedCondition::Above:
        return inst.value > inst.threshold;
    case LedCondition::Below:
        return inst.value < inst.threshold;
    default:
        return inst.value != 0.0;
    }
}

void push_trend(Instrument& inst, double v)
{
    const auto len = static_cast<std::size_t>(std::max(inst.trend_len, 2));
    if (inst.trend.size() < len)
    {
        inst.trend.push_back(static_cast<float>(v));
        return;
    }
    inst.trend.resize(len); // trend_len may have shrunk
    inst.trend_head %= len;
    inst.trend[inst.trend_head] = static_cast<float>(v);
    inst.trend_head = (inst.trend_head + 1) % len;
}

// Round gauge: brass bezel, 270 degree scale min..max with a red zone, needle, value below the centre.
void draw_gauge(ImDrawList* dl, ImVec2 p0, ImVec2 size, const Instrument& inst)
{
    const float fs = ImGui::GetFontSize();
    const std::string v = value_text(inst);
    const std::string lo = std::format("{:g}", inst.min);
    const std::string hi = std::format("{:g}", inst.max);
    const GaugeLayout g = instrument_gauge_layout(size.x, size.y, fs, ImGui::CalcTextSize(v.c_str()).x,
                                                  ImGui::CalcTextSize(lo.c_str()).x, ImGui::CalcTextSize(hi.c_str()).x);
    const float r = g.r;
    const ImVec2 c{p0.x + g.cx, p0.y + g.cy};
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dial_face(dl, c, r);
    if (inst.red_from < inst.max)
    {
        dl->PathArcTo(c, r * 0.88f, dial_start + dial_span * fraction(inst, inst.red_from), dial_start + dial_span, 24);
        dl->PathStroke(red_zone, 0, r * 0.08f);
    }
    dial_ticks(dl, c, r, 0.93f);
    if (g.scale_labels) // outside the dial: the theme's text colour
    {
        dl->AddText({p0.x + g.lo.x0, p0.y + g.lo.y0}, text, lo.c_str());
        dl->AddText({p0.x + g.hi.x0, p0.y + g.hi.y0}, text, hi.c_str());
    }
    if (inst.has_value)
    {
        const float a = dial_start + dial_span * fraction(inst, inst.value);
        const bool red = inst.red_from < inst.max && inst.value >= inst.red_from;
        dl->PathArcTo(c, r * 0.64f, dial_start, a, 32); // water line up to the needle
        dl->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark, 0.45f), 0, std::max(2.0f, r * 0.04f));
        dial_needle(dl, c, r * 0.82f, a, red ? red_zone : accent);
    }
    dl->AddCircleFilled(c, r * 0.08f, dial_brass);
    dl->AddText(ImGui::GetFont(), fs * g.value_scale, {p0.x + g.value.x0, p0.y + g.value.y0}, IM_COL32_WHITE, v.c_str());
}

// Title line; an unbound widget shows the binding hint there instead.
void draw_title(const Instrument& inst)
{
    if (inst.sig == nullptr)
    {
        ImGui::TextDisabled("%s (right-click to bind)", instrument_kind_name(inst.kind));
    }
    else
    {
        ImGui::TextUnformatted(title(inst).c_str());
    }
}

// Rotary knob: drag vertically or turn the wheel; sends the snapped position whenever it changes.
void draw_knob(App& app, Instrument& inst, ImVec2 size)
{
    const float fs = ImGui::GetFontSize();
    draw_title(inst);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = std::max(size.y - 2.0f * (fs + ImGui::GetStyle().ItemSpacing.y), 8.0f);
    ImGui::InvisibleButton("##knob", {std::max(size.x, 8.0f), h});
    const float range_px = fs * 12.0f;
    double v = inst.input;
    if (ImGui::IsItemActivated())
    {
        inst.knob_drag = inst.input; // drag start
    }
    if (ImGui::IsItemActive())
    {
        v = instrument_knob_drag(inst, inst.knob_drag, ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y, range_px);
    }
    else if (ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY) && ImGui::GetIO().MouseWheel != 0.0f)
    {
        const double res = inst.sig != nullptr && inst.sig->value_type == SignalValueType::integer ? std::abs(inst.sig->factor) : 0.0;
        v = instrument_snap(inst, inst.input + ImGui::GetIO().MouseWheel * std::max((inst.max - inst.min) / 100.0, res));
    }
    if (v != inst.input)
    {
        inst.input = v;
        instrument_send(app, inst, v);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::max(std::min(size.x, h) * 0.5f - 3.0f, 4.0f);
    const ImVec2 c{p0.x + size.x * 0.5f, p0.y + h * 0.5f};
    const float a = dial_start + dial_span * fraction(inst, inst.input);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    dial_face(dl, c, r);
    dl->PathArcTo(c, r * 0.8f, dial_start, a, 24);
    dl->PathStroke(accent, 0, r * 0.1f);
    dl->AddLine({c.x + std::cos(a) * r * 0.3f, c.y + std::sin(a) * r * 0.3f}, {c.x + std::cos(a) * r * 0.7f, c.y + std::sin(a) * r * 0.7f},
                IM_COL32_WHITE, std::max(2.0f, r * 0.06f));
    ImGui::TextDisabled("%s", value_text(inst).c_str());
}

// Text as large as `fit` allows (never below the UI font, at most 8x), centred in `box`.
void draw_fitted(ImDrawList* dl, ImVec2 p0, ImVec2 fit, ImVec2 box, ImU32 col, const std::string& text)
{
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float scale = std::clamp(std::min(fit.x / std::max(ts.x, 1.0f), fit.y / std::max(ts.y, 1.0f)), 1.0f, 8.0f);
    dl->AddText(ImGui::GetFont(), fs * scale, {p0.x + (box.x - ts.x * scale) * 0.5f, p0.y + (box.y - ts.y * scale) * 0.5f},
                col, text.c_str());
}

void draw_bar(ImDrawList* dl, ImVec2 p0, ImVec2 size, const Instrument& inst)
{
    const float f = inst.has_value ? fraction(inst, inst.value) : 0.0f;
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImVec2 p1{p0.x + size.x, p0.y + size.y};
    dl->AddRectFilled(p0, p1, plate_well, 3.0f);
    if (inst.vertical)
    {
        dl->AddRectFilled({p0.x, p1.y - size.y * f}, p1, accent, 3.0f);
    }
    else
    {
        dl->AddRectFilled(p0, {p0.x + size.x * f, p1.y}, accent, 3.0f);
    }
    draw_fitted(dl, p0, {size.x * 0.9f, size.y * 0.8f}, size, ImGui::GetColorU32(ImGuiCol_Text), value_text(inst));
}

void draw_led(ImDrawList* dl, ImVec2 p0, ImVec2 size, const Instrument& inst)
{
    const float r = std::max(std::min(size.x, size.y) * 0.35f, 3.0f);
    const ImVec2 c{p0.x + size.x * 0.5f, p0.y + size.y * 0.5f};
    const bool on = inst.has_value && led_lit(inst);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    if (on)
    {
        dl->AddCircleFilled(c, r * 1.3f, ImGui::GetColorU32(ImGuiCol_CheckMark, 0.25f), 32); // glow
    }
    dl->AddCircleFilled(c, r, on ? accent : plate_well, 32);
}

// Filterable combo of every CAN signal in the setup's DBCs. Returns true when a signal was picked.
bool draw_signal_picker(App& app, InstrumentPanel& p, Instrument& inst)
{
    const std::string preview = inst.signal.empty() ? std::string("(none)") : std::format("{}.{}", inst.msg ? inst.msg->name : "?", inst.signal);
    bool picked = false;
    if (!ImGui::BeginCombo("Signal", preview.c_str(), ImGuiComboFlags_HeightLarge))
    {
        return false;
    }
    const bool appearing = ImGui::IsWindowAppearing();
    if (appearing)
    {
        p.search.clear(); // one filter for every widget's combo: a stale one would re-bind on Enter
    }
    const SignalEntry* hit = signal_search_input(p.picker, app.setup, "Filter...", p.search, appearing, &app.signal_seen);
    for (int row = 0; row < static_cast<int>(p.picker.hits.size()); ++row)
    {
        const SignalEntry& e = p.picker.entries[static_cast<std::size_t>(p.picker.hits[static_cast<std::size_t>(row)].entry)];
        ImGui::PushID(row);
        const std::string label = std::format("{}  ({})", e.label, app.setup.networks[e.network].name);
        if (ImGui::Selectable(label.c_str(), inst.sig == e.can_sig || (!p.search.empty() && row == p.picker.selected)))
        {
            hit = &e;
        }
        ImGui::PopID();
    }
    if (hit != nullptr)
    {
        const CanDbSignal& sig = *hit->can_sig;
        inst.network = app.setup.networks[hit->network].name;
        inst.raw_id = hit->raw_id;
        inst.signal = sig.name;
        inst.msg = hit->can_msg;
        inst.sig = &sig;
        inst.has_value = false;
        inst.has_frame = false;
        inst.trend.clear();
        if (sig.max > sig.min)
        {
            inst.min = sig.min;
            inst.max = sig.max;
            inst.red_from = sig.max;
        }
        picked = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
    return picked;
}

// Right-click menu of one instrument: properties inline, Remove at the bottom. True = remove.
bool draw_properties(App& app, InstrumentPanel& p, Instrument& inst)
{
    const float w = ImGui::GetFontSize() * 14.0f;
    ImGui::SetNextItemWidth(w);
    int kind = static_cast<int>(inst.kind);
    if (ImGui::Combo("Type", &kind, kind_names.data(), static_cast<int>(kind_names.size())))
    {
        inst.kind = static_cast<InstrumentKind>(kind);
    }
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("Label", inst.signal.c_str(), &inst.label);
    ImGui::SetNextItemWidth(w);
    draw_signal_picker(app, p, inst);
    if (instrument_is_input(inst.kind))
    {
        ImGui::SetNextItemWidth(w);
        draw_iface_combo("Interface", app, inst.iface, BusType::CAN);
    }
    ImGui::SetNextItemWidth(w);
    int grid[4] = {inst.col, inst.row, inst.col_span, inst.row_span};
    if (ImGui::InputInt4("Col Row W H", grid)) // fallback for dragging in the panel
    {
        inst.col = std::clamp(grid[0], 0, 63);
        inst.row = std::clamp(grid[1], 0, 255);
        instrument_resize(inst, grid[2], grid[3]);
        instrument_panel_settle(p, static_cast<std::size_t>(&inst - p.items.data()));
    }
    switch (inst.kind)
    {
    case InstrumentKind::Gauge:
    case InstrumentKind::Bar:
    case InstrumentKind::Slider:
    case InstrumentKind::Knob:
    {
        ImGui::SetNextItemWidth(w);
        double range[2] = {inst.min, inst.max};
        if (ImGui::InputScalarN("Min / Max", ImGuiDataType_Double, range, 2))
        {
            inst.min = range[0];
            inst.max = range[1];
        }
        if (inst.kind == InstrumentKind::Gauge)
        {
            ImGui::SetNextItemWidth(w);
            ImGui::InputDouble("Red zone from", &inst.red_from);
        }
        if (inst.kind == InstrumentKind::Bar)
        {
            ImGui::Checkbox("Vertical", &inst.vertical);
        }
        break;
    }
    case InstrumentKind::Led:
    {
        ImGui::SetNextItemWidth(w);
        constexpr const char* conds[] = {"value != 0", "value > threshold", "value < threshold"};
        int led = static_cast<int>(inst.led);
        if (ImGui::Combo("On when", &led, conds, 3))
        {
            inst.led = static_cast<LedCondition>(led);
        }
        if (inst.led != LedCondition::NonZero)
        {
            ImGui::SetNextItemWidth(w);
            ImGui::InputDouble("Threshold", &inst.threshold);
        }
        break;
    }
    case InstrumentKind::Trend:
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("Samples", &inst.trend_len))
        {
            inst.trend_len = std::clamp(inst.trend_len, 2, 10000);
            inst.trend.clear();
            inst.trend_head = 0;
        }
        break;
    case InstrumentKind::Button:
        ImGui::Checkbox("Toggle (latching)", &inst.toggle);
        [[fallthrough]];
    case InstrumentKind::Checkbox:
    {
        ImGui::SetNextItemWidth(w);
        double vals[2] = {inst.on_value, inst.off_value};
        if (ImGui::InputScalarN("On / Off value", ImGuiDataType_Double, vals, 2))
        {
            inst.on_value = vals[0];
            inst.off_value = vals[1];
        }
        break;
    }
    default:
        break;
    }
    ImGui::Separator();
    return ImGui::MenuItem("Remove");
}

void draw_input(App& app, const InstrumentPanel& p, Instrument& inst, ImVec2 size)
{
    ImGui::BeginDisabled(p.edit || inst.sig == nullptr);
    switch (inst.kind)
    {
    case InstrumentKind::Button:
    {
        const bool on = inst.toggle && inst.input == inst.on_value;
        if (on)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_CheckMark));
        }
        const bool clicked = ImGui::Button(title(inst).c_str(), size);
        if (on)
        {
            ImGui::PopStyleColor();
        }
        if (inst.toggle && clicked)
        {
            inst.input = on ? inst.off_value : inst.on_value;
            instrument_send(app, inst, inst.input);
        }
        else if (!inst.toggle && ImGui::IsItemActivated())
        {
            instrument_send(app, inst, inst.on_value);
        }
        else if (!inst.toggle && ImGui::IsItemDeactivated())
        {
            instrument_send(app, inst, inst.off_value);
        }
        break;
    }
    case InstrumentKind::Slider:
        draw_title(inst);
        ImGui::SetNextItemWidth(size.x);
        if (ImGui::SliderScalar("##v", ImGuiDataType_Double, &inst.input, &inst.min, &inst.max, "%g"))
        {
            inst.input = instrument_snap(inst, inst.input); // show what is sent
            instrument_send(app, inst, inst.input);
        }
        ImGui::TextDisabled("%s", value_text(inst).c_str());
        break;
    case InstrumentKind::Checkbox:
    {
        bool on = inst.input == inst.on_value;
        if (ImGui::Checkbox(title(inst).c_str(), &on))
        {
            inst.input = on ? inst.on_value : inst.off_value;
            instrument_send(app, inst, inst.input);
        }
        ImGui::TextDisabled("%s", value_text(inst).c_str());
        break;
    }
    case InstrumentKind::Knob:
        draw_knob(app, inst, size);
        break;
    default:
        break;
    }
    ImGui::EndDisabled();
}

void draw_display(Instrument& inst, ImVec2 size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    draw_title(inst);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    size.y = std::max(size.y - fs - ImGui::GetStyle().ItemSpacing.y, 4.0f);
    switch (inst.kind)
    {
    case InstrumentKind::Gauge:
        draw_gauge(dl, p0, size, inst);
        break;
    case InstrumentKind::Bar:
        draw_bar(dl, p0, inst.vertical ? size : ImVec2{size.x, std::min(size.y, fs * 4.0f)}, inst);
        break;
    case InstrumentKind::Led:
        draw_led(dl, p0, size, inst);
        break;
    case InstrumentKind::Numeric:
    {
        draw_fitted(dl, p0, {size.x * 0.95f, size.y * 0.9f}, size, ImGui::GetColorU32(ImGuiCol_CheckMark), value_text(inst));
        break;
    }
    case InstrumentKind::Text:
    {
        const std::string_view name = inst.has_value && inst.sig ? can_signal_value_name(*inst.sig, inst.raw) : std::string_view{};
        const std::string v = name.empty() ? value_text(inst) : std::string(name);
        draw_fitted(dl, p0, {size.x * 0.95f, size.y * 0.9f}, size, ImGui::GetColorU32(ImGuiCol_Text), v);
        break;
    }
    case InstrumentKind::Trend:
    {
        ImGui::TextDisabled("%s", value_text(inst).c_str());
        const ImVec2 plot{size.x, std::max(size.y - fs - ImGui::GetStyle().ItemSpacing.y, 4.0f)};
        if (ImPlot::BeginPlot("##trend", plot, ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs | ImPlotFlags_NoFrame))
        {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit,
                              ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit);
            ImPlotSpec spec;
            spec.LineColor = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            const bool full = inst.trend.size() >= static_cast<std::size_t>(inst.trend_len);
            spec.Offset = full ? static_cast<int>(inst.trend_head) : 0;
            ImPlot::PlotLine("##v", inst.trend.data(), static_cast<int>(inst.trend.size()), 1.0, 0.0, spec);
            ImPlot::EndPlot();
        }
        return;
    }
    default:
        break;
    }
    ImGui::Dummy(size);
}

} // namespace

const char* instrument_kind_name(InstrumentKind kind) noexcept
{
    return kind < InstrumentKind::Count ? kind_names[static_cast<std::size_t>(kind)] : "";
}

void instrument_panel_resolve(InstrumentPanel& p, const Setup& setup)
{
    for (Instrument& inst : p.items)
    {
        inst.msg = nullptr;
        inst.sig = nullptr;
        for (const SetupNetwork& net : setup.networks)
        {
            if (net.name != inst.network)
            {
                continue;
            }
            for (const auto& db : net.can_dbs)
            {
                if (const CanDbMessage* msg = can_db_find_message(*db, inst.raw_id))
                {
                    if (const CanDbSignal* sig = can_db_find_signal(*msg, inst.signal); sig && !inst.sig)
                    {
                        inst.msg = msg;
                        inst.sig = sig;
                    }
                }
            }
        }
    }
}

void instrument_panel_ingest(InstrumentPanel& p, const Setup& setup, const Trace& trace)
{
    if (p.trace_clears != trace.clears)
    {
        p.trace_clears = trace.clears;
        for (Instrument& inst : p.items)
        {
            inst.has_value = false;
            inst.trend.clear();
            inst.trend_head = 0;
        }
    }
    if (p.setup_generation != setup.generation)
    {
        p.setup_generation = setup.generation;
        instrument_panel_resolve(p, setup);
    }
    const uint64_t first = trace.file.empty() ? std::max(p.next_index, trace.begin) : trace.end; // a file view is not live
    p.next_index = trace.end;
    if (p.items.empty())
    {
        return;
    }
    for (uint64_t i = first; i < trace.end; ++i)
    {
        const BusMessage& m = trace_at(trace, i);
        const CanDbMessage* msg = m.type == BusType::CAN && m.errors == 0 ? setup_find_can_message(setup, m) : nullptr;
        if (msg == nullptr)
        {
            continue;
        }
        for (Instrument& inst : p.items)
        {
            if (inst.msg != msg)
            {
                continue;
            }
            inst.frame = m;
            inst.has_frame = true;
            if (!can_signal_present(*msg, *inst.sig, m))
            {
                continue;
            }
            inst.raw = can_signal_extract_raw(*inst.sig, m);
            inst.value = can_signal_raw_to_physical(*inst.sig, inst.raw);
            inst.has_value = true;
            if (inst.kind == InstrumentKind::Trend)
            {
                push_trend(inst, inst.value);
            }
        }
    }
}

BusMessage instrument_encode(const Instrument& inst, double physical)
{
    BusMessage m{};
    if (inst.has_frame)
    {
        m = inst.frame;
    }
    else
    {
        m.id = inst.msg->raw_id & can_id_mask_extended;
        m.flags = (inst.msg->raw_id & 0x80000000u) != 0 ? bus_flag::extended : 0;
        set_length(m, inst.msg->dlc);
        if (m.len > 8)
        {
            m.flags |= bus_flag::fd;
        }
    }
    m.flags &= static_cast<uint16_t>(~bus_flag::tx);
    m.iface = inst.iface;
    m.ts_ns = 0;
    // ponytail: a muxed signal is sent in the last seen frame's mux slot; set the muxer too if panels need it.
    can_signal_inject_physical(*inst.sig, m, physical);
    return m;
}

double instrument_snap(const Instrument& inst, double physical)
{
    double v = std::clamp(physical, std::min(inst.min, inst.max), std::max(inst.min, inst.max));
    if (inst.sig != nullptr && inst.sig->value_type == SignalValueType::integer && inst.sig->factor != 0.0)
    {
        v = inst.sig->offset + std::round((v - inst.sig->offset) / inst.sig->factor) * inst.sig->factor;
    }
    return v;
}

double instrument_knob_drag(const Instrument& inst, double from, float dy_px, float px_per_range)
{
    return instrument_snap(inst, from - static_cast<double>(dy_px / std::max(px_per_range, 1.0f)) * (inst.max - inst.min));
}

void instrument_panel_move(InstrumentPanel& p, std::size_t i, int col, int row)
{
    Instrument& a = p.items[i];
    col = std::clamp(col, 0, 63);
    row = std::clamp(row, 0, 255);
    for (std::size_t j = 0; j < p.items.size(); ++j)
    {
        Instrument& b = p.items[j];
        if (j != i && col >= b.col && col < b.col + b.col_span && row >= b.row && row < b.row + b.row_span)
        {
            b.col = a.col;
            b.row = a.row;
            break;
        }
    }
    a.col = col;
    a.row = row;
    instrument_panel_settle(p, i);
}

void instrument_panel_settle(InstrumentPanel& p, std::size_t keep)
{
    const auto overlap = [](const Instrument& a, const Instrument& b)
    {
        return a.col < b.col + b.col_span && b.col < a.col + a.col_span && a.row < b.row + b.row_span && b.row < a.row + a.row_span;
    };
    std::vector<std::size_t> order(p.items.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (keep < order.size())
    {
        std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(keep), order.begin() + static_cast<std::ptrdiff_t>(keep) + 1);
    }
    for (std::size_t k = 0; k < order.size(); ++k)
    {
        Instrument& a = p.items[order[k]];
        a.col_span = std::min(a.col_span, p.columns);
        a.col = std::clamp(a.col, 0, p.columns - a.col_span);
        while (std::any_of(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(k),
                           [&](std::size_t j) { return overlap(a, p.items[j]); }))
        {
            ++a.row;
        }
    }
}

void instrument_resize(Instrument& inst, int col_span, int row_span)
{
    inst.col_span = std::clamp(col_span, 1, 16);
    inst.row_span = std::clamp(row_span, 1, 16);
}

GaugeLayout instrument_gauge_layout(float w, float h, float fs, float value_w, float lo_w, float hi_w)
{
    constexpr float pad = 3.0f;
    constexpr float d = 0.7071068f + 0.05f; // arc end at 45 degrees, plus half the bezel width
    GaugeLayout g;
    const float r_plain = std::max(std::min(w * 0.5f, (h - pad) * 0.5f) - pad, 4.0f);
    // Labels hang outside the arc ends, below-left and below-right of the dial.
    const float r_labels = std::min({(w * 0.5f - std::max(lo_w, hi_w) - 2.0f) / d, (h - pad - fs - 2.0f) / (1.0f + d), r_plain});
    g.scale_labels = r_labels >= std::max(r_plain * 0.6f, fs);
    g.r = g.scale_labels ? r_labels : r_plain;
    g.cx = w * 0.5f;
    g.cy = pad + g.r;
    if (g.scale_labels)
    {
        const float x = g.r * d + 2.0f;
        const float y = g.cy + g.r * d + 2.0f;
        g.lo = {g.cx - x - lo_w, y, g.cx - x, y + fs};
        g.hi = {g.cx + x, y, g.cx + x + hi_w, y + fs};
    }
    // In the wedge under the scale's ends, where the needle never points: top 0.45r, height <= 0.3r,
    // half width <= 0.45r. Grows with the dial.
    g.value_scale = std::min(g.r * 0.3f / fs, g.r * 0.9f / std::max(value_w, 1.0f));
    float vy = g.cy + g.r * 0.45f;
    if (g.value_scale < 0.8f) // too small to read inside: under a smaller dial at the UI size
    {
        g.value_scale = std::min(1.0f, (w - 2.0f * pad) / std::max(value_w, 1.0f));
        g.scale_labels = false;
        g.r = std::max(std::min(w * 0.5f - pad, (h - 3.0f * pad - fs * g.value_scale) * 0.5f), 2.0f);
        g.cy = pad + g.r;
        vy = g.cy + g.r + pad;
    }
    const float vw = value_w * g.value_scale;
    g.value = {g.cx - vw * 0.5f, vy, g.cx + vw * 0.5f, vy + fs * g.value_scale};
    return g;
}

bool instrument_send(App& app, Instrument& inst, double physical)
{
    if (inst.sig == nullptr || inst.iface >= app.ifaces.size())
    {
        return false;
    }
    const BusMessage m = instrument_encode(inst, physical);
    inst.frame = m; // the next press builds on this, even before the TX echo arrives
    inst.has_frame = true;
    return iface_send(app.ifaces[inst.iface], m);
}

void draw_instrument_panel(App& app, const WorkspaceTab& tab, InstrumentPanel& p)
{
    if (!p.open)
    {
        return;
    }
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 40.0f, ImGui::GetFontSize() * 26.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(workspace_window_name(tab, "Instrument Panel").c_str(), &p.open))
    {
        ImGui::End();
        return;
    }
    const float fs = ImGui::GetFontSize();
    if (ImGui::RadioButton("Edit", p.edit))
    {
        p.edit = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Run", !p.edit))
    {
        p.edit = false;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!p.edit);
    if (ImGui::Button("Add..."))
    {
        ImGui::OpenPopup("add");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 6.0f);
    if (ImGui::InputInt("Columns", &p.columns))
    {
        p.columns = std::clamp(p.columns, 1, 64);
        instrument_panel_settle(p);
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("add"))
    {
        for (std::size_t k = 0; k < kind_names.size(); ++k)
        {
            if (ImGui::MenuItem(kind_names[k]))
            {
                int row = 0;
                for (const Instrument& inst : p.items)
                {
                    row = std::max(row, inst.row + inst.row_span);
                }
                p.items.push_back({.kind = static_cast<InstrumentKind>(k), .row = row});
            }
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float cell_w = std::max(ImGui::GetContentRegionAvail().x / static_cast<float>(p.columns), fs * 3.0f);
    const float cell_h = fs * 7.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x * 0.5f;
    ImVec2 extent{0.0f, 0.0f};
    int remove = -1;
    // Edit-mode drag of one widget: move by its frame, resize by its bottom-right corner.
    struct Drag
    {
        int item = -1;
        bool resize = false;
        bool drop = false;
        int a = 0; // col / col_span
        int b = 0; // row / row_span
    } drag;
    for (std::size_t i = 0; i < p.items.size(); ++i)
    {
        Instrument& inst = p.items[i];
        const ImVec2 pos{origin.x + static_cast<float>(inst.col) * cell_w, origin.y + static_cast<float>(inst.row) * cell_h};
        const ImVec2 size{static_cast<float>(inst.col_span) * cell_w - gap, static_cast<float>(inst.row_span) * cell_h - gap};
        extent = {std::max(extent.x, pos.x + size.x - origin.x), std::max(extent.y, pos.y + size.y - origin.y)};
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(static_cast<int>(i));
        // A bulkhead plate, the same in both themes, so its text is light in both. Rimless; the
        // accent rim only marks the cells in Edit mode.
        // The theme's colours, for the properties popup and the tooltip (plain theme windows).
        const std::array<ImVec4, 4> theme = {ImGui::GetStyleColorVec4(ImGuiCol_Border), ImGui::GetStyleColorVec4(ImGuiCol_ChildBg),
                                             ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)};
        const auto push_theme = [&]
        {
            constexpr std::array cols{ImGuiCol_Border, ImGuiCol_ChildBg, ImGuiCol_Text, ImGuiCol_TextDisabled};
            for (std::size_t k = 0; k < cols.size(); ++k)
            {
                ImGui::PushStyleColor(cols[k], theme[k]);
            }
        };
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetColorU32(ImGuiCol_CheckMark));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, plate_bottom);
        ImGui::PushStyleColor(ImGuiCol_Text, plate_text);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, plate_text_dim);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, std::max(1.5f, fs / 10.0f));
        if (ImGui::BeginChild("cell", size, p.edit ? ImGuiChildFlags_Borders : ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor({pos.x + 2.0f, pos.y + 2.0f}, {pos.x + size.x - 2.0f, pos.y + size.y - 2.0f},
                                                                plate_top, plate_top, plate_bottom, plate_bottom);
            if (p.edit)
            {
                // Submitted before the content so they own the hover; the grip first, it sits on top.
                const float grip = fs * 0.8f;
                const ImVec2 corner{pos.x + size.x, pos.y + size.y};
                for (const bool resize : {true, false})
                {
                    ImGui::SetCursorScreenPos(resize ? ImVec2{corner.x - grip, corner.y - grip} : pos);
                    ImGui::InvisibleButton(resize ? "##resize" : "##move", resize ? ImVec2{grip, grip} : size);
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    {
                        ImGui::SetMouseCursor(resize ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeAll);
                    }
                    if (ImGui::IsItemActive() || ImGui::IsItemDeactivated())
                    {
                        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                        const int dc = static_cast<int>(std::lround(d.x / cell_w));
                        const int dr = static_cast<int>(std::lround(d.y / cell_h));
                        drag = {static_cast<int>(i), resize, ImGui::IsItemDeactivated(),
                                resize ? inst.col_span + dc : inst.col + dc, resize ? inst.row_span + dr : inst.row + dr};
                    }
                }
                ImGui::GetWindowDrawList()->AddTriangleFilled({corner.x - grip, corner.y - 1.0f}, {corner.x - 1.0f, corner.y - grip},
                                                              {corner.x - 1.0f, corner.y - 1.0f}, dial_brass);
                ImGui::SetCursorPos(ImGui::GetStyle().WindowPadding);
            }
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            if (instrument_is_input(inst.kind))
            {
                const bool fill = inst.kind == InstrumentKind::Button || inst.kind == InstrumentKind::Knob;
                draw_input(app, p, inst, {avail.x, fill ? avail.y : 0.0f});
            }
            else
            {
                draw_display(inst, avail);
            }
            if (inst.sig == nullptr && (inst.kind == InstrumentKind::Button || inst.kind == InstrumentKind::Checkbox))
            {
                ImGui::SetCursorPos({ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().WindowPadding.y + fs * 1.2f});
                ImGui::TextDisabled("%s (right-click to bind)", instrument_kind_name(inst.kind));
            }
            push_theme(); // the plate's light text would vanish on a light-theme popup
            if (ImGui::BeginPopupContextWindow("props"))
            {
                if (draw_properties(app, p, inst))
                {
                    remove = static_cast<int>(i);
                }
                ImGui::EndPopup();
            }
            if (inst.has_value && ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive())
            {
                ImGui::SetTooltip("%s.%s = %s", inst.msg ? inst.msg->name.c_str() : "?", inst.signal.c_str(), value_text(inst).c_str());
            }
            ImGui::PopStyleColor(4);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
    }
    if (drag.item >= 0)
    {
        const auto di = static_cast<std::size_t>(drag.item);
        Instrument t = p.items[di]; // drop target, for the highlight
        if (drag.resize)
        {
            instrument_resize(t, drag.a, drag.b);
        }
        else
        {
            t.col = std::clamp(drag.a, 0, 63);
            t.row = std::clamp(drag.b, 0, 255);
        }
        const ImVec2 t0{origin.x + static_cast<float>(t.col) * cell_w, origin.y + static_cast<float>(t.row) * cell_h};
        const ImVec2 t1{t0.x + static_cast<float>(t.col_span) * cell_w - gap, t0.y + static_cast<float>(t.row_span) * cell_h - gap};
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(t0, t1, ImGui::GetColorU32(ImGuiCol_CheckMark, 0.2f), 4.0f);
        fg->AddRect(t0, t1, ImGui::GetColorU32(ImGuiCol_CheckMark), 4.0f, 0, 2.0f);
        if (drag.drop && drag.resize)
        {
            instrument_resize(p.items[di], drag.a, drag.b);
            instrument_panel_settle(p, di);
        }
        else if (drag.drop)
        {
            instrument_panel_move(p, di, drag.a, drag.b);
        }
    }
    if (remove >= 0)
    {
        p.items.erase(p.items.begin() + remove);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(extent); // the window scrolls to the lowest widget
    if (p.items.empty())
    {
        ImGui::TextDisabled("Add a widget, then right-click it to bind a signal.");
    }
    ImGui::End();
}

void instrument_panel_save_xml(const InstrumentPanel& p, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    el.append_attribute("columns") = p.columns;
    el.append_attribute("edit") = p.edit;
    for (const Instrument& inst : p.items)
    {
        pugi::xml_node n = el.append_child("instrument");
        n.append_attribute("kind") = instrument_kind_name(inst.kind);
        n.append_attribute("label") = inst.label.c_str();
        n.append_attribute("col") = inst.col;
        n.append_attribute("row") = inst.row;
        n.append_attribute("colspan") = inst.col_span;
        n.append_attribute("rowspan") = inst.row_span;
        n.append_attribute("network") = inst.network.c_str();
        n.append_attribute("id") = inst.raw_id;
        n.append_attribute("signal") = inst.signal.c_str();
        if (inst.iface < ifaces.size())
        {
            n.append_attribute("driver") = ifaces[inst.iface].ops ? ifaces[inst.iface].ops->name : "";
            n.append_attribute("interface") = ifaces[inst.iface].info.name.c_str();
        }
        n.append_attribute("min") = inst.min;
        n.append_attribute("max") = inst.max;
        n.append_attribute("red") = inst.red_from;
        n.append_attribute("vertical") = inst.vertical;
        n.append_attribute("led") = led_names[static_cast<std::size_t>(inst.led)];
        n.append_attribute("threshold") = inst.threshold;
        n.append_attribute("toggle") = inst.toggle;
        n.append_attribute("on") = inst.on_value;
        n.append_attribute("off") = inst.off_value;
        n.append_attribute("samples") = inst.trend_len;
    }
}

void instrument_panel_load_xml(InstrumentPanel& p, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    p.columns = std::clamp(el.attribute("columns").as_int(4), 1, 64);
    p.edit = el.attribute("edit").as_bool(true);
    p.items.clear();
    p.setup_generation = UINT64_MAX;
    for (const pugi::xml_node n : el.children("instrument"))
    {
        const std::string_view kind = n.attribute("kind").as_string();
        const auto k = std::ranges::find(kind_names, kind);
        if (k == kind_names.end())
        {
            continue; // unknown widget from a newer version
        }
        const std::string_view led = n.attribute("led").as_string();
        const auto l = std::ranges::find(led_names, led);
        const int index = ifaces_find(ifaces, n.attribute("driver").as_string(), n.attribute("interface").as_string());
        p.items.push_back({
            .kind = static_cast<InstrumentKind>(k - kind_names.begin()),
            .label = n.attribute("label").as_string(),
            .col = std::clamp(n.attribute("col").as_int(), 0, 63),
            .row = std::clamp(n.attribute("row").as_int(), 0, 255),
            .col_span = std::clamp(n.attribute("colspan").as_int(1), 1, 16),
            .row_span = std::clamp(n.attribute("rowspan").as_int(1), 1, 16),
            .network = n.attribute("network").as_string(),
            .raw_id = n.attribute("id").as_uint(),
            .signal = n.attribute("signal").as_string(),
            .iface = static_cast<uint16_t>(index >= 0 ? index : UINT16_MAX),
            .min = n.attribute("min").as_double(0.0),
            .max = n.attribute("max").as_double(100.0),
            .red_from = n.attribute("red").as_double(80.0),
            .vertical = n.attribute("vertical").as_bool(),
            .led = l != led_names.end() ? static_cast<LedCondition>(l - led_names.begin()) : LedCondition::NonZero,
            .threshold = n.attribute("threshold").as_double(),
            .toggle = n.attribute("toggle").as_bool(),
            .on_value = n.attribute("on").as_double(1.0),
            .off_value = n.attribute("off").as_double(0.0),
            .trend_len = std::clamp(n.attribute("samples").as_int(100), 2, 10000),
        });
    }
}
