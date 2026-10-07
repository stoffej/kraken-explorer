// Graph window (the Qt GraphWindow): time series, XY, text cards and gauges of
// CAN/LIN signals and bus load, drawn with ImPlot. Plain data plus free functions.
// No <imgui.h> here: workspace_tabs.h (and so app.h) includes this header.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ui/file_dialog.h"
#include "ui/signal_search.h"
#include "ui/vim_nav.h"

namespace pugi
{
class xml_node;
}

struct App;
struct CanDbMessage;
struct CanDbSignal;
struct Iface;
struct Setup;
struct WorkspaceTab;

// Saved by name in the workspace (graph_save_xml), so the order is free.
enum class GraphView : uint8_t { TimeSeries, XY, Text, Gauge };
enum class GraphSignalKind : uint8_t { Can, Lin, BusLoad };

// Plot slots: slot / graph_axes_per_plot = subplot row, slot % graph_axes_per_plot = Y1..Y3.
inline constexpr int graph_axes_per_plot = 3;

// File view: min/max pyramid of a signal's whole series, so a window at any zoom costs
// O(pixels) and no spike is lost. Level 0 is the series; level L > 0 holds, per bucket of
// graph_lod_fan consecutive points of level L - 1, that bucket's minimum and maximum sample
// (their own timestamps, in time order), so a window read at a level keeps every extreme as
// long as its buckets are narrower than a pixel.
// ponytail: level 0 is doubles (88 MB for a 5.5 M-sample signal); float32 values, or dropping
// level 0 and decoding fine zooms from the frame cache, when memory shows up.
inline constexpr std::size_t graph_lod_fan = 64;
inline constexpr std::size_t graph_lod_top = 4096; // no level above one with fewer buckets than this

struct GraphLod
{
    std::vector<std::vector<double>> t; // per level, time order
    std::vector<std::vector<double>> v;
};

// Level 0 = (t, v), then levels of graph_lod_fan while the next one would have >= top buckets.
[[nodiscard]] GraphLod graph_lod_build(std::vector<double> t, std::vector<double> v, std::size_t top = graph_lod_top);
// Coarsest level with at least 4 * pixels buckets (level 0: samples) inside [x0, x1], else 0.
[[nodiscard]] std::size_t graph_lod_level(const GraphLod& lod, double x0, double x1, int pixels);
// Points of `level` in [x0, x1] plus one neighbour each side, into out_* (as graph_decimate).
void graph_lod_window(const GraphLod& lod, std::size_t level, double x0, double x1, std::vector<double>& out_t,
                      std::vector<double>& out_v);

struct FrameCache;

// A pyramid being decoded off the main thread; `result` is set before `done`.
struct GraphLodJob
{
    std::shared_ptr<const GraphLod> result;
    std::atomic<bool> done{false};
    std::jthread thread; // last member: joined before the others go
};

struct GraphSignal
{
    GraphSignalKind kind = GraphSignalKind::Can;
    std::string network;                 // frames of other networks' interfaces are ignored
    const CanDbMessage* can_msg = nullptr; // Can; re-resolved when Setup::generation changes
    const CanDbSignal* can_sig = nullptr;
    uint32_t can_raw_id = 0;             // Can: key for re-resolving
    uint8_t lin_id = 0;                  // Lin: LinFrame* dies on reload, so id + name
    int iface = -1;                      // BusLoad: App::ifaces index
    unsigned bitrate = 0;                // BusLoad
    std::string name;
    std::string parent;                  // message / frame name, "Bus Load"
    std::string unit;
    double min = 0.0;
    double max = 0.0;
    uint32_t color = 0xFFFFFFFF;         // ImU32
    int axis = -1;                       // plot slot, -1 = automatic by unit
    bool hidden = false;
    std::vector<double> t;               // seconds since GraphState::start_ns
    std::vector<double> v;               // physical value
    double seen_min = 0.0;               // range of v, gauge scale when the DBC has none
    double seen_max = 0.0;
    std::deque<std::pair<double, uint32_t>> load_window; // BusLoad: (t, bits) of the last second
    uint64_t load_bits = 0;
    double load_emitted = -1.0;
    std::array<double, 5> stats{};       // Statistics columns: min, max, mean, median, std dev (NaN = none)
    std::array<double, 4> stats_key{0.0, 0.0, -1.0, 0.0}; // (x_min, x_max, samples, newest t) of `stats`
    double stats_wall = -1e9;            // wall_seconds() of the last recompute
    std::vector<double> dec_t;           // decimated points of the last plot (graph_decimate), reused while
    std::vector<double> dec_v;           // the window, the pixel width and the samples are unchanged
    std::array<double, 5> dec_key{};     // (x_min, x_max, buckets, samples, downsample) of dec_*
    std::array<double, 3> at_cursor{};   // value at cursor A, at B, B - A (NaN = none); graph_cursor_values
    std::array<double, 4> cursor_key{0.0, 0.0, -1.0, 0.0}; // (A, B, samples, newest t) of `at_cursor`
    std::shared_ptr<const GraphLod> lod; // file view: pyramid of the whole file, t and v are a window of it
    std::shared_ptr<GraphLodJob> lod_job; // the pyramid being built; nothing is drawn meanwhile
    std::weak_ptr<const FrameCache> lod_file; // the file and setup generation `lod` / `lod_job` are of
    uint64_t lod_generation = 0;
};

// "Export to PNG" options (the graph's export dialog).
enum class GraphPngStyle : uint8_t { Kraken, Light, Print, Transparent };
struct GraphPngOptions
{
    int size = 0;         // index in graph_png_sizes: 0 = as on screen, last = custom
    int width = 1920;     // custom size
    int height = 1080;
    int scale = 0;        // index in graph_png_scales: 0 = auto (by the height)
    GraphPngStyle style = GraphPngStyle::Kraken;
    std::string title;    // empty = the visible signals' names
    bool subtitle = true; // time range and signal count
    bool legend = true;
    bool cursors = true;  // cursor lines and values, when the cursors are on
    bool watermark = true;
};

// Width x height of size index i (0: the plot as on screen, w / h; last: the custom size).
[[nodiscard]] std::pair<int, int> graph_png_size(const GraphPngOptions& o, float screen_w, float screen_h);
// Text and line scale for o at height h (auto: 1x per ~900 px, 1..4).
[[nodiscard]] float graph_png_scale(const GraphPngOptions& o, int h);

struct GraphState
{
    unsigned id = 0;          // 0 = the docked default "Graph"; names "Graph <id>" otherwise
    bool open = true;
    bool standalone = false;  // own OS window (Window > Standalone Graph)
    unsigned dock_into = 0;   // dock node to appear in as a tab ("+" beside the Graph tab), 0 = none
    GraphView view = GraphView::TimeSeries;
    int duration = 1;         // index into the duration combo (All, 1 min, ...)
    int columns = 2;          // text / gauge grid
    int x_signal = 0;         // XY: index into signals of the X signal, the others are Y
    std::vector<GraphSignal> signals;
    int selected = -1;        // row of the signal list (j/k, Space toggles its visibility)
    VimNav vim;
    std::string search;       // non-empty: fuzzy ranked list instead of the tree
    SignalSearch finder;      // ranking of `search`
    std::string palette_query; // Ctrl+P "Find signal"; only the tab's default graph uses these two
    SignalSearch palette;
    uint64_t next_index = 0;  // next trace index to decode
    uint64_t trace_clears = 0;
    uint64_t setup_generation = 0;
    int64_t start_ns = -1;    // time base, earliest frame of the first batch
    double last_t = 0.0;      // newest frame time
    double last_wall = 0.0;   // steady-clock seconds when last_t was seen
    double x_min = 0.0;
    double x_max = 60.0;
    bool follow = true;       // X follows the newest data; off after user zoom/pan
    bool cursor_on = false;   // "Cursors": drag lines A and B, value per signal at each and the difference
    double cursor_a = 0.0;    // seconds since start_ns, as the X axis
    double cursor_b = 0.0;
    bool statistics = false;  // min / max / mean / median / std dev per signal: between the cursors when they
                              // are on, else of the visible window
    bool cursor_y_on = false; // "Y cursors": horizontal drag lines 1 and 2 on the first plot's Y1 axis
    double cursor_y1 = 0.0;
    double cursor_y2 = 0.0;
    bool place_cursor_y = false; // the checkbox was just ticked: put Y1 / Y2 into the visible range
    bool dots = true;         // sample markers on the curves (XY: the latest point)
    int downsample = 1;       // keep every Nth sample before decimation
    std::vector<int> slots;   // graph_assign_slots(signals), recomputed when the signal set or an axis changes
    bool slots_dirty = true;
    std::vector<double> stride_t; // downsampled points, reused every frame
    std::vector<double> stride_v;
    std::array<double, 5> file_key{}; // file view: (x_min, x_max, signals, setup generation, plot_px) of the decoded window
    int plot_px = 1024;       // file view: plot width in pixels at the last draw, picks the pyramid level
    double log_t = -1.0;      // the Log's selected row (file view: else its top row) on the X axis (a line), < 0 = none
    uint64_t log_sel = UINT64_MAX; // trace index of the Log's selected row last frame: the window centres when it changes
    double click_t = -1.0;    // file view: a click in the plot at this X moves the Log there, < 0 = none
    std::vector<double> scratch_t; // decimated points, reused every frame
    std::vector<double> scratch_v;
    FileDialog export_dialog; // "Export plot to *.csv" / "Export to PNG" (by extension)
    float view_w = 0.0f, view_h = 0.0f; // the plot area on screen ("as on screen" export size)
    GraphPngOptions png;
    bool png_dialog = false;  // open the options popup next frame
    std::string png_path;     // chosen file: drawn off screen this frame, rendered by main.cpp
    std::string png_last = "graph.png"; // the last export's path: the next one starts there
    std::vector<std::array<double, 2>> y_shown; // per slot: the Y range on screen, so the export matches it
    std::array<double, 4> xy_shown{};           // XY view: x min / max, y min / max on screen
};

// Plot slot per signal (same order): manual `axis` wins. Automatic ones share a slot when they
// share a unit and their DBC ranges (max - min) differ by at most 10x; otherwise they take the
// next free Y axis of the first plot, and once Y1..Y3 are taken the slot whose range is nearest.
[[nodiscard]] std::vector<int> graph_assign_slots(std::span<const GraphSignal> signals);

// Points of (t, v) visible in [x0, x1], plus one neighbour on each side, into out_*.
// More than 2 * buckets points are reduced to min and max per bucket, in time order.
void graph_decimate(std::span<const double> t, std::span<const double> v, double x0, double x1, int buckets,
                    std::vector<double>& out_t, std::vector<double>& out_v);

// XY pairing, sample-and-hold: for each Y sample with time in [t0, t1], the latest X value at
// or before that time. Y samples before the first X sample are skipped. Both t spans sorted.
void graph_xy_pair(std::span<const double> x_t, std::span<const double> x_v, std::span<const double> y_t,
                   std::span<const double> y_v, double t0, double t1, std::vector<double>& out_x,
                   std::vector<double>& out_y);

// Sample-and-hold value at `time`: the latest sample at or before it, NaN before the first.
[[nodiscard]] double graph_value_at(std::span<const double> t, std::span<const double> v, double time);

// s.at_cursor for cursors a and b, recomputed only when a cursor moved or samples were added.
const std::array<double, 3>& graph_cursor_values(GraphSignal& s, double a, double b);

// "Δt = 12.5 ms  (1/Δt = 80 Hz)" for cursors a and b; "-" as frequency when they coincide.
[[nodiscard]] std::string graph_delta_t_text(double a, double b);

// Value text: CAN signals at their resolution (can_signal_format), others %.6g; NaN = "-".
[[nodiscard]] std::string graph_format_value(const GraphSignal& s, double v);
// First colormap colour no signal of g uses (needs an ImPlot context).
[[nodiscard]] uint32_t graph_next_color(const GraphState& g);
// Removes signal i; the XY X signal keeps pointing at the same signal.
void graph_remove_signal(GraphState& g, std::size_t i);

// Adds e (unless present) and shows [t0, t1] (absolute ns) with some margin, cursors A and B on
// its ends: Value Search hits.
void graph_show_range(GraphState& g, const Setup& setup, const SignalEntry& e, int64_t t0_ns, int64_t t1_ns);

// One <graph> child of the workspace <tab> per open graph: view settings and the signals by
// name (network + message + signal, bus load by driver + interface), not their samples.
void graph_save_xml(std::span<const GraphState> graphs, const std::deque<Iface>& ifaces, pugi::xml_node tab);
// Appends the <graph> children of tab; signals no longer in setup / ifaces are dropped with a warning.
void graph_load_xml(std::vector<GraphState>& graphs, const Setup& setup, const std::deque<Iface>& ifaces,
                    pugi::xml_node tab);

// Decodes the trace frames appended since the last call into the graph's signals.
void graph_ingest(GraphState& g, const App& app);

// Handles New Graph View / Graph Widget / Standalone Graph, feeds every graph of every tab
// and draws the graphs of `current`. Call after draw_workspace().
void draw_graph_windows(App& app, WorkspaceTab* current);
