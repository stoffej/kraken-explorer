// Instrument Panel window: display and input widgets bound to DBC signals, placed on a grid.
// Plain data plus free functions. No <imgui.h> here: app.h includes this header.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/bus_message.h"
#include "ui/signal_search.h"

struct App;
struct CanDbMessage;
struct CanDbSignal;
struct Iface;
struct Setup;
struct Trace;
struct WorkspaceTab;
namespace pugi
{
class xml_node;
}

// Display widgets first, then input widgets (instrument_is_input). Saved by name, so the order may change.
enum class InstrumentKind : uint8_t { Gauge, Bar, Led, Numeric, Text, Trend, Button, Slider, Checkbox, Knob, Count };
// LED lit when: value != 0, value > threshold, value < threshold.
enum class LedCondition : uint8_t { NonZero, Above, Below };

[[nodiscard]] const char* instrument_kind_name(InstrumentKind kind) noexcept;
[[nodiscard]] constexpr bool instrument_is_input(InstrumentKind kind) noexcept
{
    return kind >= InstrumentKind::Button && kind < InstrumentKind::Count;
}

struct Instrument
{
    InstrumentKind kind = InstrumentKind::Gauge;
    std::string label;          // empty: the signal name
    int col = 0;                // grid cell of the top-left corner
    int row = 0;
    int col_span = 1;
    int row_span = 1;
    // Binding: network + DBC raw id + signal name, re-resolved when Setup::generation changes.
    std::string network;
    uint32_t raw_id = 0;
    std::string signal;
    const CanDbMessage* msg = nullptr;
    const CanDbSignal* sig = nullptr;
    uint16_t iface = UINT16_MAX; // input widgets: App::ifaces index to send on
    double min = 0.0;            // gauge / bar / slider scale
    double max = 100.0;
    double red_from = 80.0;      // gauge red zone [red_from, max]; >= max = none
    bool vertical = false;       // bar
    LedCondition led = LedCondition::NonZero;
    double threshold = 0.0;      // LED Above / Below
    bool toggle = false;         // button: latching instead of momentary
    double on_value = 1.0;       // button / checkbox physical values sent
    double off_value = 0.0;
    int trend_len = 100;         // trend: samples kept
    // Runtime, not saved.
    bool has_value = false;
    double value = 0.0;          // last physical value
    uint64_t raw = 0;            // its raw value (value-table lookup)
    BusMessage frame{};          // last frame of the bound message: base for sending (keeps the other signals)
    bool has_frame = false;
    double input = 0.0;          // slider / knob position, button + checkbox state
    double knob_drag = 0.0;      // knob: unsnapped position while dragging
    std::vector<float> trend;    // ring buffer of trend_len values
    std::size_t trend_head = 0;
};

struct InstrumentPanel
{
    bool open = false;
    bool edit = true;            // Edit: layout + properties, inputs do not send. Run: inputs send.
    int columns = 4;
    std::vector<Instrument> items;
    std::string search;          // signal picker filter
    SignalSearch picker{.can_only = true, .list_all = true}; // fuzzy ranking of `search`; widgets bind signals that may not be on the bus yet
    uint64_t next_index = 0;     // next trace index to decode
    uint64_t trace_clears = 0;
    uint64_t setup_generation = UINT64_MAX; // forces a resolve on the first ingest
};

// Clamps `physical` to [min, max] and, for an integer signal, rounds it to the signal's resolution
// (offset + n * factor).
[[nodiscard]] double instrument_snap(const Instrument& inst, double physical);
// Knob position after dragging `dy_px` pixels (up = negative = more) from `from`, where
// `px_per_range` pixels turn the knob through min..max. Snapped.
[[nodiscard]] double instrument_knob_drag(const Instrument& inst, double from, float dy_px, float px_per_range);
// Edit-mode layout: moves item `i` to cell (col, row); an item covering that cell takes i's old place.
void instrument_panel_move(InstrumentPanel& p, std::size_t i, int col, int row);
// No widget overlaps another or sticks out past `columns`: item `keep` stays (clamped into the
// columns), the others in order move down to the first free row. Run after every move / resize.
void instrument_panel_settle(InstrumentPanel& p, std::size_t keep = SIZE_MAX);
void instrument_resize(Instrument& inst, int col_span, int row_span);

// Gauge layout inside a w x h area (below the title line), relative to its top-left. Text widths
// are measured at font size fs. The value sits below the dial centre inside the dial, scaled by
// value_scale; the min / max labels sit outside the arc ends and are dropped when they don't fit.
struct InstrumentRect
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};
struct GaugeLayout
{
    float cx = 0.0f;
    float cy = 0.0f;
    float r = 0.0f;
    float value_scale = 1.0f; // >= 0.8 (or the cell width): inside the dial, or under it on a small one
    InstrumentRect value;
    bool scale_labels = false;
    InstrumentRect lo;
    InstrumentRect hi;
};
[[nodiscard]] GaugeLayout instrument_gauge_layout(float w, float h, float fs, float value_w, float lo_w, float hi_w);

// Re-resolves every binding against `setup`; unresolved instruments stay (shown unbound).
void instrument_panel_resolve(InstrumentPanel& p, const Setup& setup);
// Decodes the trace frames appended since the last call into the instruments' last values.
// O(new frames x instruments); drawing never touches the trace.
void instrument_panel_ingest(InstrumentPanel& p, const Setup& setup, const Trace& trace);
// Frame that sets the instrument's signal to `physical`: the last seen frame of its message,
// otherwise a zeroed frame of the DBC length. Requires a resolved binding.
[[nodiscard]] BusMessage instrument_encode(const Instrument& inst, double physical);
// Encodes and sends on inst.iface. False when unbound, no interface, or iface_send fails.
bool instrument_send(App& app, Instrument& inst, double physical);

// The "Instrument Panel" window of `tab` while p.open.
void draw_instrument_panel(App& app, const WorkspaceTab& tab, InstrumentPanel& p);

// Workspace persistence; the interface is saved as driver + name and resolved on load.
void instrument_panel_save_xml(const InstrumentPanel& p, const std::deque<Iface>& ifaces, pugi::xml_node el);
void instrument_panel_load_xml(InstrumentPanel& p, const std::deque<Iface>& ifaces, pugi::xml_node el);
