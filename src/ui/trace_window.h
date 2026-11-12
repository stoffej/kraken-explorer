#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/bus_message.h"
#include "decoders/decoders.h"
#include "ui/file_dialog.h"
#include "ui/vim_nav.h"

// No <imgui.h> here: app.h includes this header and is included by targets without imgui.

struct App;
struct Trace;
struct CanDbSignal;
struct Iface;
struct WorkspaceTab;

namespace pugi
{
class xml_node;
}

// Values match the old timestamp_mode_t, so workspace files keep their meaning.
enum class TimestampMode
{
    Absolute = 0,
    Relative = 1, // since the first frame the window saw after a clear
    Delta = 2,    // since the previous frame with the same id/interface/direction
    AbsoluteUtc = 3,
};

enum class TraceViewMode
{
    Aggregated,
    Rolling,
};

enum class TraceTab
{
    Monitor,
    Uds,
    J1939,
    N2k,
    Count,
};

// The Filter dialog plus the text filter (substring of ID and type case-insensitive, of name,
// channel and sender with ripgrep smart case).
struct TraceFilter
{
    std::string text;
    bool show_tx = true;
    bool show_rx = true;
    std::set<uint32_t> hidden_ids;     // CAN id (can_id())
    std::set<uint32_t> hidden_lin_ids; // LIN frame id
    std::set<uint16_t> hidden_ifaces;
    std::set<std::string, std::less<>> hidden_nodes; // DBC sender / LDF publisher names
};

// Monitor, rolling log: one frame that passed the filter.
struct TraceRow
{
    uint64_t index = 0; // trace index
    uint64_t prev = UINT64_MAX; // trace index of the previous frame with the same delta key
};

// Cycle time statistics of one aggregated row: min/max/mean over every cycle, median over
// the newest cycles only.
struct CycleStats
{
    // ponytail: median over the last 256 cycles (2 KiB per id), a streaming quantile sketch if
    // the whole history is ever needed.
    static constexpr std::size_t window = 256;
    std::array<int64_t, window> recent{}; // ring buffer of cycle times in ns
    int64_t min_ns = INT64_MAX;
    int64_t max_ns = 0;
    int64_t sum_ns = 0;
    uint64_t count = 0;
};

void cycle_stats_add(CycleStats& c, int64_t cycle_ns) noexcept;
// Median of the recent window in ns (mean of the two middle values for an even count); 0 if empty.
[[nodiscard]] double cycle_stats_median(const CycleStats& c);

// Monitor, aggregated: the newest frame per (direction, error, interface, bus, id).
struct AggRow
{
    BusMessage last{};
    BusMessage prev{}; // previous frame of this row; len == 0 before the second one
    uint32_t order = 0; // 1-based order of appearance (Index column)
    uint64_t count = 1; // frames received (Count column)
    bool has_prev = false;
    CycleStats cycle;
};

struct ProtoRow
{
    ProtocolMessage msg;
    int64_t prev_ts_ns = 0; // previous message from the same raw frame key (Delta column)
    uint32_t order = 0;
};

// One protocol tab (UDS, J1939 or NMEA 2000): both the rolling list and the aggregated rows.
struct ProtoView
{
    std::vector<ProtoRow> rolling;
    std::vector<uint32_t> visible; // indices into rolling that pass the filter
    std::vector<ProtoRow> aggregated;
    std::unordered_map<uint64_t, uint32_t> agg_index; // key -> index into aggregated
    std::unordered_map<uint64_t, int64_t> last_ts;    // delta key -> last timestamp
    uint32_t next_order = 1;
};

struct TraceWindowState
{
    TraceTab tab = TraceTab::Monitor;
    TraceViewMode modes[static_cast<int>(TraceTab::Count)] = {TraceViewMode::Aggregated, TraceViewMode::Rolling,
                                                              TraceViewMode::Aggregated, TraceViewMode::Aggregated};
    TimestampMode ts_mode = TimestampMode::Delta;
    bool autoscroll = true;
    bool ascii = false; // data column as ASCII instead of hex (tracewindow/dataAsciiMode)
    bool decimal = false; // Data and ID columns in decimal instead of hex
    TraceFilter filter;
    FileDialog dbc_dialog;
    std::vector<std::string> dbc_paths; // waiting for a network choice

    // Everything below is derived from the trace by trace_window_update().
    std::string filter_edit; // text box contents, applied to filter.text after a short pause
    std::chrono::steady_clock::time_point filter_edited{};
    bool filter_dirty = true; // rebuild the filtered lists on the next update
    uint64_t setup_gen = 0;   // Setup::generation used by the filtered lists
    uint64_t processed = 0;   // next trace index to look at
    uint64_t refilter = UINT64_MAX; // next index of a running rolling-log refilter, UINT64_MAX = none
    uint64_t index_base = 0;  // trace index shown as Index 1 (the trace begin at the last clear)
    uint64_t clears = 0;      // Trace::clears last seen
    bool was_measuring = false;
    int64_t cycle_cut_ns = 0; // measurement stop seen at this time: no cycle spans it
    int64_t first_ts_ns = 0;
    std::vector<TraceRow> rolling;
    std::unordered_map<uint64_t, uint64_t> last_by_key; // delta key -> trace index
    std::vector<AggRow> agg;
    std::unordered_map<uint64_t, uint32_t> agg_index; // aggregation key -> index into agg
    std::vector<uint32_t> agg_order; // agg indices in display (sort) order
    bool agg_dirty = true;           // row set or filter changed: rebuild agg_order on the next draw
    uint64_t agg_setup_gen = 0;      // Setup::generation at the last rebuild (names, senders)
    double agg_sorted_time = -1.0;   // ImGui::GetTime() of the last rebuild
    uint64_t agg_sorts = 0;          // agg_order rebuilds (for tests)
    ProtocolDecoder decoder;
    ProtoView uds;
    ProtoView j1939;
    ProtoView n2k;
    std::size_t max_proto_rows = 100000; // oldest 20% dropped beyond this
    bool scroll_pending = false;

    // Vim motions over the rows of the current table (position in display order, -1 = none).
    VimNav vim;
    int selected = -1;
    bool nav_scroll = false;   // scroll to the selected row when it is drawn next
    bool yank_pending = false; // 'y': copy the selected row's cells when it is drawn next
    std::vector<VimYankItem> yank_items; // the captured row for vim_yank_menu: [0] whole row, then one per column
    bool focus_filter = false; // '/' pressed: focus the Filter field next frame
    int tab_goto = -1;         // h/l: TraceTab to switch to, -1 = none

    // File view (a loaded file, Trace::file): the Log draws the visible page straight from the
    // mapping, no row list. ImGui scrolls in float pixels, which cannot address 100M+ rows, so
    // the page starts at file_top (a row number) instead.
    uint64_t file_top = 0;
    std::string file_goto; // "Go to" field: time since the first frame, "90", "1:30", "2d 1:02:03"
    std::vector<uint32_t> file_filtered; // with a filter: the accepted frames (file indices), in time order
    // The merge that fills file_filtered runs on a worker (file_view_filter): file_filter_result is
    // valid while it runs (the Log shows "Filtering..."), trace_window_update takes the list when ready.
    std::atomic<uint64_t> file_filter_done{0}; // frames merged so far, of file_filter_total
    uint64_t file_filter_total = 0;
    std::future<std::vector<uint32_t>> file_filter_result;
    std::jthread file_filter_worker; // last: stopped and joined before the members it uses go away
};

// Pulls new frames from app.trace into the window's lists: incremental filter, aggregation
// and protocol decoding. Applies a pending filter change or trace clear first. No ImGui.
void trace_window_update(TraceWindowState& s, const App& app);

// network == -1 creates a database-only CAN network. Failures are logged and shown in the status bar.
bool trace_window_add_databases(App& app, int network, const std::vector<std::string>& paths);
// OS file drop in ImGui screen coordinates; only the visible Trace window under the pointer accepts it.
void trace_window_drop_files(App& app, const std::vector<std::string>& paths, float x, float y);

// Shows trace index `index` in the Monitor tab as it is: the Log row (file view: with
// `context_rows` above it) or, in the aggregated Monitor, the row of that frame's id.
void trace_window_select_frame(TraceWindowState& s, const Trace& t, uint64_t index, uint64_t context_rows);

// File view: puts the Log on frame `file_index` (an index into Trace::file): the row is the frame
// itself, or the first filtered row at or after it; `context_rows` rows stay visible above it.
void trace_window_goto(TraceWindowState& s, const Trace& t, uint64_t file_index, uint64_t context_rows);

// Filter predicate used for all lists (exposed for tests).
[[nodiscard]] bool trace_filter_accepts(const TraceWindowState& s, const App& app, const BusMessage& m);

// Interfaces the Filter popup lists: those in the measurement setup (driver + name) plus any
// with frames in the trace. Returns Iface::index values.
[[nodiscard]] std::vector<uint16_t> trace_filter_ifaces(const TraceWindowState& s, const App& app);

// Changed-byte mask of cur against prev (bit i = byte i); 0 for error frames or no prev.
[[nodiscard]] uint64_t trace_changed_mask(const BusMessage& cur, const BusMessage& prev) noexcept;

// Draws the Trace window of a workspace tab (update included). When loading a filter,
// set filter_edit = filter.text too (the text box edits filter_edit).
// Data / ID column text: hex ("12 AB", "0x123") or decimal ("18 171", "291"); a CAN error frame
// shows its error flags / "-" either way.
void trace_append_data(std::string& out, const BusMessage& m, bool decimal);
void trace_append_id(std::string& out, const BusMessage& m, bool decimal);
// Value of an expanded signal row: "raw - value name", else the physical value (+ unit); integer
// signals with the decimals of factor and offset (can_signal_format), so the decimal point stays
// put, left-padded to width characters; float32 signals at float precision, float64 15 digits.
void trace_append_signal_value(std::string& out, const CanDbSignal& sig, uint64_t raw, std::size_t width = 0);
// The whole trace as CSV with the DBC columns (sender, name, decoded signals) next to the raw
// data. Returns the number of frames written, -1 when the file cannot be opened.
int64_t trace_write_csv(const App& app, const std::string& path);

void draw_trace_window(App& app, TraceWindowState& s, const WorkspaceTab& tab);

// <tracewindow> of a workspace tab: view modes, timestamps, hex/dec, filter text, TX/RX and
// hidden interfaces (by driver + name).
void trace_window_save_xml(const TraceWindowState& s, const std::deque<Iface>& ifaces, pugi::xml_node el);
void trace_window_load_xml(TraceWindowState& s, const std::deque<Iface>& ifaces, pugi::xml_node el);
