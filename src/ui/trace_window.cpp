#include "ui/trace_window.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <charconv>
#include <format>
#include <iterator>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // TableFindByID: last frame's column state for the ScrollX fallback
#include <misc/cpp/imgui_stdlib.h>
#include <pugixml.hpp>

#include "app.h"
#include "core/setup.h"
#include "core/fuzzy.h"
#include "core/text.h"
#include "db/model/can_db.h"
#include "db/model/lin_db.h"
#include "drivers/driver.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/vim_nav.h"

namespace
{

using namespace std::chrono_literals;

constexpr auto filter_debounce = 250ms;
// ponytail: fixed row cap per UI frame for a filter change (a 23M-row trace takes ~46 frames),
// make it time-based if one slice ever stalls a frame.
constexpr uint64_t refilter_rows_per_frame = 500'000;
// ponytail: aggregated rows sorted on a changing value (Time, Data, Cycle, ...) re-sort at most
// this often; the row set and sort spec changes re-sort at once. Lower agg_resort_s if the lag is visible.
constexpr double agg_resort_s = 0.25;

enum Column
{
    col_index,
    col_time,
    col_channel,
    col_direction,
    col_type,
    col_id,
    col_sender,
    col_name,
    col_dlc,
    col_data,
    col_comment,
    col_count, // columns of every table; the cycle columns below exist in the aggregated one only
    col_cycle_min = col_count,
    col_cycle_max,
    col_cycle_mean,
    col_cycle_median,
    col_agg_count,
};

struct ColumnDef
{
    const char* label;
    const char* sample; // widest typical value: sizes a fixed column; nullptr for the text columns
    float weight;       // stretch weight of the text columns
    bool hidden;        // off by default (header context menu turns it on), so the table fits ~500 px
};

constexpr ColumnDef columns[col_agg_count] = {
    {"Index", "000 000 000", 0, false},
    {"Time", "00:00:00.000", 0, false},
    {"Channel", "vcan0", 0, true},
    {"RX/TX", "TX", 0, true},
    {"Type", "FD.EXT.BRS", 0, true},
    {"ID", "0x1FFFFFFF", 0, false},
    {"Sender", nullptr, 150, true},
    {"Name", nullptr, 150, false},
    {"DLC", "64", 0, false},
    {"Data", nullptr, 260, false},
    {"Comment", nullptr, 120, true},
    {"Cycle min", "1000.000", 0, true},
    {"Cycle max", "1000.000", 0, true},
    {"Cycle", "1000.000", 0, false}, // mean
    {"Cycle median", "1000.000", 0, true},
};

// Free-text columns share the width left by the fixed ones, weighted by their weight above.
constexpr bool stretch_column(int i) noexcept
{
    return columns[i].sample == nullptr;
}
constexpr float stretch_min_width = 40; // logical px; below the sum of minimums the table scrolls

// Fixed column widths from the current font: max(label + sort arrow, sample); recomputed per font size.
const float* fixed_widths()
{
    static float widths[col_agg_count] = {};
    static float font_size = 0.0f;
    if (font_size != ImGui::GetFontSize())
    {
        font_size = ImGui::GetFontSize();
        const float arrow = ImFloor(font_size * 0.65f + ImGui::GetStyle().FramePadding.x); // as TableHeader
        for (int i = 0; i < col_agg_count; ++i)
        {
            widths[i] = stretch_column(i) ? 0.0f
                                          : std::max(ImGui::CalcTextSize(columns[i].label).x + arrow,
                                                     ImGui::CalcTextSize(columns[i].sample).x);
        }
    }
    return widths;
}

constexpr const char* tab_labels[] = {"Monitor", "UDS Protocol", "J1939"};

// --- keys ---------------------------------------------------------------------------------

// UnifiedTraceViewModel::makeDeltaKey: interface, direction, extended, bus type, id.
uint64_t delta_key(const BusMessage& m) noexcept
{
    return static_cast<uint64_t>(m.iface) << 33 | static_cast<uint64_t>(!has_flag(m, bus_flag::tx)) << 32
           | static_cast<uint64_t>(has_flag(m, bus_flag::extended)) << 31
           | static_cast<uint64_t>(m.type) << 29 | (m.id & can_id_mask_extended);
}

// AggregatedTraceViewModel::makeUniqueKey: error frames (id 0) stay apart from a real 0x000.
uint64_t agg_key(const BusMessage& m) noexcept
{
    return static_cast<uint64_t>(!has_flag(m, bus_flag::tx)) << 63 | static_cast<uint64_t>(is_error_frame(m)) << 62
           | static_cast<uint64_t>(m.iface) << 32 | static_cast<uint64_t>(m.type) << 30
           | (static_cast<uint64_t>(m.id) & 0x3FFFFFFFull);
}

bool is_protocol(const ProtocolMessage& pm, std::string_view name)
{
    return iequals(pm.protocol, name);
}

uint64_t proto_agg_key(const ProtocolMessage& pm)
{
    const BusMessage* f = pm.raw_frames.empty() ? nullptr : &pm.raw_frames.front();
    const uint64_t iface = f != nullptr ? f->iface : 0;
    if (is_protocol(pm, "J1939"))
    {
        const auto sa = pm.metadata.find("Source Address");
        return iface << 32 | static_cast<uint64_t>(pm.id) << 8 | (sa != pm.metadata.end() ? sa->second & 0xFF : 0);
    }
    return iface << 48 | static_cast<uint64_t>(f != nullptr ? f->id : 0) << 16 | (pm.id & 0xFFFF);
}

// --- text ---------------------------------------------------------------------------------

std::string_view iface_name(const App& app, uint16_t i)
{
    return i < app.ifaces.size() ? std::string_view(app.ifaces[i].info.name) : std::string_view{};
}

bool icontains(std::string_view hay, std::string_view needle)
{
    return needle.empty() || !std::ranges::search(hay, needle, {}, ascii_lower, ascii_lower).empty();
}

// Names, senders and channels: ripgrep smart case. Ids stay case-insensitive (hex is not text).
bool smart_contains(std::string_view hay, std::string_view needle)
{
    return smart_find(hay, needle) != std::string_view::npos;
}

void append_type(std::string& out, const BusMessage& m)
{
    if (m.type == BusType::LIN)
    {
        out += has_flag(m, bus_flag::lin_sleep) ? "LIN.SLP" : has_flag(m, bus_flag::lin_wakeup) ? "LIN.WUP" : "LIN";
        return;
    }
    if (is_error_frame(m))
    {
        out += "ERR";
        return;
    }
    out += has_flag(m, bus_flag::fd) ? "FD." : "";
    out += has_flag(m, bus_flag::extended) ? "EXT" : "STD";
    out += has_flag(m, bus_flag::rtr) ? ".RTR" : "";
    out += has_flag(m, bus_flag::brs) ? ".BRS" : "";
}

// A CAN error frame carries error classes instead of an id; a LIN error frame is a real frame.
void append_frame_id(std::string& out, const BusMessage& m)
{
    if (is_error_frame(m) && m.type == BusType::CAN)
    {
        out += '-';
        return;
    }
    append_id(out, m);
}

// Decimal bytes "18 171 0", separated like append_bytes' hex.
void append_dec_bytes(std::string& out, std::span<const uint8_t> bytes)
{
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        char t[3];
        out.append(i > 0 ? " " : "");
        out.append(t, std::to_chars(t, t + sizeof t, bytes[i]).ptr);
    }
}

struct DbText
{
    std::string_view name, sender, comment;
};

DbText db_text(const App& app, const BusMessage& m)
{
    if (m.type == BusType::LIN)
    {
        if (has_flag(m, bus_flag::lin_sleep))
        {
            return {.name = "Sleep"};
        }
        if (has_flag(m, bus_flag::lin_wakeup))
        {
            return {.name = "Wakeup"};
        }
        const LinFrame* f = setup_find_lin_frame(app.setup, m);
        return f != nullptr ? DbText{.name = f->name, .sender = f->publisher} : DbText{};
    }
    const CanDbMessage* d = setup_find_can_message(app.setup, m);
    return d != nullptr ? DbText{.name = d->name, .sender = d->sender, .comment = d->comment} : DbText{};
}

const std::chrono::time_zone* local_zone()
{
    static const std::chrono::time_zone* tz = []() -> const std::chrono::time_zone*
    {
        try
        {
            return std::chrono::current_zone();
        }
        catch (const std::exception&)
        {
            return nullptr; // no tzdata: absolute times fall back to UTC
        }
    }();
    return tz;
}

void append_time(std::string& out, TimestampMode mode, int64_t ts, int64_t prev_ts, int64_t first_ts)
{
    using namespace std::chrono;
    const auto sys = floor<milliseconds>(sys_time<nanoseconds>(nanoseconds(ts)));
    switch (mode)
    {
    case TimestampMode::Absolute:
        if (const time_zone* tz = local_zone())
        {
            std::format_to(std::back_inserter(out), "{:%H:%M:%S}", zoned_time(tz, sys).get_local_time());
            return;
        }
        [[fallthrough]];
    case TimestampMode::AbsoluteUtc:
        std::format_to(std::back_inserter(out), "{:%H:%M:%S}", sys);
        return;
    case TimestampMode::Relative: // seconds, or m:ss / h:mm:ss / Nd hh:mm:ss once that long
        append_duration(out, ts >= first_ts ? static_cast<double>(ts - first_ts) / 1e9 : 0.0);
        return;
    case TimestampMode::Delta:
        append_duration(out, prev_ts > 0 && ts >= prev_ts ? static_cast<double>(ts - prev_ts) / 1e9 : 0.0);
        return;
    }
}

// --- filter -------------------------------------------------------------------------------

bool dialog_accepts(const TraceFilter& f, const BusMessage& m)
{
    if (has_flag(m, bus_flag::tx) ? !f.show_tx : !f.show_rx)
    {
        return false;
    }
    if (m.type == BusType::LIN ? f.hidden_lin_ids.contains(m.id) : f.hidden_ids.contains(can_id(m)))
    {
        return false;
    }
    return !f.hidden_ifaces.contains(m.iface);
}

// Columns the old TraceFilterModel searched: ID (as shown: hex or decimal), name, channel, sender, type.
bool text_matches(const App& app, std::string_view text, const BusMessage& m, bool decimal)
{
    static std::string buf; // main thread only
    buf.clear();
    trace_append_id(buf, m, decimal);
    buf += '\n';
    append_type(buf, m);
    const DbText db = db_text(app, m);
    return icontains(buf, text) || smart_contains(db.name, text) || smart_contains(db.sender, text)
           || smart_contains(iface_name(app, m.iface), text);
}

bool proto_accepts(const TraceWindowState& s, const App& app, const ProtocolMessage& pm)
{
    if (pm.raw_frames.empty())
    {
        return s.filter.text.empty();
    }
    const BusMessage& f = pm.raw_frames.front();
    return dialog_accepts(s.filter, f)
           && (s.filter.text.empty() || smart_contains(pm.name, s.filter.text) || text_matches(app, s.filter.text, f, s.decimal));
}

// --- model update -------------------------------------------------------------------------

void monitor_add(TraceWindowState& s, const App& app, uint64_t index)
{
    const BusMessage& m = trace_at(app.trace, index);
    auto [it, inserted] = s.last_by_key.try_emplace(delta_key(m), index);
    const uint64_t prev = inserted ? UINT64_MAX : std::exchange(it->second, index);
    if (trace_filter_accepts(s, app, m))
    {
        s.rolling.push_back({.index = index, .prev = prev});
        s.scroll_pending = true;
    }
}

void proto_rebuild_visible(const TraceWindowState& s, const App& app, ProtoView& v)
{
    v.visible.clear();
    for (uint32_t i = 0; i < v.rolling.size(); ++i)
    {
        if (proto_accepts(s, app, v.rolling[i].msg))
        {
            v.visible.push_back(i);
        }
    }
}

void proto_add(TraceWindowState& s, const App& app, ProtoView& v, ProtocolMessage&& pm)
{
    ProtoRow row{.order = v.next_order++};
    const uint64_t dkey = pm.raw_frames.empty() ? 0 : delta_key(pm.raw_frames.front());
    auto [ts_it, first] = v.last_ts.try_emplace(dkey, pm.ts_ns);
    row.prev_ts_ns = first ? 0 : std::exchange(ts_it->second, pm.ts_ns);
    row.msg = std::move(pm);

    auto [agg_it, inserted] = v.agg_index.try_emplace(proto_agg_key(row.msg), static_cast<uint32_t>(v.aggregated.size()));
    if (inserted)
    {
        v.aggregated.push_back(row);
    }
    else
    {
        ProtoRow& a = v.aggregated[agg_it->second];
        a.prev_ts_ns = a.msg.ts_ns;
        a.msg = row.msg;
    }

    v.rolling.push_back(std::move(row));
    if (proto_accepts(s, app, v.rolling.back().msg))
    {
        v.visible.push_back(static_cast<uint32_t>(v.rolling.size() - 1));
        s.scroll_pending = true;
    }
    if (v.rolling.size() > s.max_proto_rows)
    {
        v.rolling.erase(v.rolling.begin(), v.rolling.begin() + static_cast<std::ptrdiff_t>(s.max_proto_rows / 5));
        proto_rebuild_visible(s, app, v);
    }
}

void process_frame(TraceWindowState& s, const App& app, uint64_t index)
{
    const BusMessage& m = trace_at(app.trace, index);
    if (s.first_ts_ns == 0)
    {
        s.first_ts_ns = m.ts_ns;
    }
    monitor_add(s, app, index);

    auto [it, inserted] = s.agg_index.try_emplace(agg_key(m), static_cast<uint32_t>(s.agg.size()));
    if (inserted)
    {
        s.agg.push_back({.last = m, .order = static_cast<uint32_t>(s.agg.size() + 1)});
        s.agg_dirty = true;
    }
    else
    {
        AggRow& row = s.agg[it->second];
        row.prev = row.last;
        row.last = m;
        row.has_prev = true;
        if (m.ts_ns >= row.prev.ts_ns && (row.prev.ts_ns >= s.cycle_cut_ns || m.ts_ns < s.cycle_cut_ns)) // not across a stop
        {
            cycle_stats_add(row.cycle, m.ts_ns - row.prev.ts_ns);
        }
    }

    ProtocolMessage pm;
    if (protocol_decode(s.decoder, m, pm) == DecodeStatus::Completed)
    {
        if (is_protocol(pm, "uds"))
        {
            proto_add(s, app, s.uds, std::move(pm));
        }
        else if (is_protocol(pm, "J1939"))
        {
            proto_add(s, app, s.j1939, std::move(pm));
        }
    }
}

void reset_views(TraceWindowState& s, uint64_t begin)
{
    s.processed = begin;
    s.index_base = begin;
    s.first_ts_ns = 0;
    s.rolling.clear();
    s.last_by_key.clear();
    s.agg.clear();
    s.agg_index.clear();
    s.agg_order.clear();
    s.agg_dirty = true;
    protocol_reset(s.decoder);
    s.uds = {};
    s.j1939 = {};
}

// Any filter set (text, TX/RX, hidden ids or interfaces)?
bool trace_filter_active(const TraceFilter& f) noexcept
{
    return !f.text.empty() || !f.show_tx || !f.show_rx || !f.hidden_ids.empty() || !f.hidden_lin_ids.empty()
           || !f.hidden_ifaces.empty();
}

// File view: one aggregated row per (channel, id) of the cache index, with its last two frames and
// the cycle stats the build summed up (the median window from the tail of the row's frame list).
// Indexing the whole file through process_frame would take seconds.
void file_view_agg(TraceWindowState& s, const App& app)
{
    const Trace& t = app.trace;
    s.first_ts_ns = trace_at(t, t.begin).ts_ns;
    if (app.trace_file == nullptr)
    {
        return;
    }
    const FrameCache& c = *app.trace_file;
    for (const FrameCacheRow& r : c.rows)
    {
        AggRow row{.last = frame_cache_frame(c, c.row_frames[r.first + r.count - 1]), .order = static_cast<uint32_t>(s.agg.size() + 1)};
        if (r.count > 1)
        {
            row.prev = frame_cache_frame(c, c.row_frames[r.first + r.count - 2]);
            row.has_prev = true;
            row.cycle = {.min_ns = r.cycle_min_ns, .max_ns = r.cycle_max_ns, .sum_ns = r.cycle_sum_ns, .count = r.count - 1};
            const uint64_t n = std::min<uint64_t>(row.cycle.count, CycleStats::window);
            for (uint64_t j = 0, k = r.first + r.count - n; j < n; ++j, ++k)
            {
                row.cycle.recent[j] = c.recs[c.row_frames[k]].ts_ns - c.recs[c.row_frames[k - 1]].ts_ns;
            }
        }
        s.agg_index.try_emplace(agg_key(row.last), static_cast<uint32_t>(s.agg.size()));
        s.agg.push_back(row);
    }
    s.agg_dirty = true;
}

// Drops the filtered list of a file view and a merge still running for it (stopped and joined).
void file_filter_cancel(TraceWindowState& s)
{
    s.file_filtered.clear();
    s.file_filter_result = {};
    s.file_filter_worker = {};
}

// File view with a filter: the accepted frames in time order, from the cache's per-id index. The
// filter looks at id, name, sender, channel and type, all fixed per (channel, id) row, so each row
// is decided once (here) and the accepted rows' frame lists are merged on a worker (~1 s per 100M
// frames); only the direction is per frame. 4 bytes per accepted frame and no pass over the file.
void file_view_filter(TraceWindowState& s, const App& app)
{
    file_filter_cancel(s);
    if (app.trace_file == nullptr)
    {
        return;
    }
    const FrameCache& c = *app.trace_file;
    std::vector<std::span<const uint32_t>> lists;
    std::size_t total = 0;
    for (const FrameCacheRow& r : c.rows)
    {
        BusMessage m = frame_cache_frame(c, c.row_frames[r.first]);
        m.flags = static_cast<uint16_t>((m.flags & ~bus_flag::tx) | (s.filter.show_rx ? 0 : bus_flag::tx));
        if (trace_filter_accepts(s, app, m))
        {
            lists.push_back(c.row_frames.subspan(r.first, r.count));
            total += r.count;
        }
    }
    s.file_filter_total = total;
    s.file_filter_done = 0;
    // The spans point into the mapping, kept alive by the captured cache. A newer filter stops
    // this merge (checked every 1M frames) and its partial list is dropped.
    std::packaged_task<std::vector<uint32_t>(std::stop_token)> merge(
        [cache = app.trace_file, lists = std::move(lists), total, show_tx = s.filter.show_tx, show_rx = s.filter.show_rx,
         done = &s.file_filter_done, wake = app.tasks.wake](const std::stop_token& stop) -> std::vector<uint32_t>
        {
            std::vector<uint32_t> out;
            out.reserve(total);
            const bool by_direction = !show_tx || !show_rx;
            using Head = std::pair<uint32_t, std::size_t>; // (frame, list)
            std::priority_queue<Head, std::vector<Head>, std::greater<>> heap;
            std::vector<std::size_t> next(lists.size(), 1);
            for (std::size_t i = 0; i < lists.size(); ++i)
            {
                heap.emplace(lists[i][0], i);
            }
            for (uint64_t n = 0; !heap.empty(); ++n)
            {
                if (n % (uint64_t{1} << 20) == 0 && n > 0)
                {
                    if (stop.stop_requested())
                    {
                        return {};
                    }
                    done->store(n);
                    if (wake != nullptr)
                    {
                        wake(); // the toolbar's "Filtering... N %" in an event-driven loop
                    }
                }
                const auto [frame, i] = heap.top();
                heap.pop();
                if (!by_direction || ((cache->recs[frame].flags & bus_flag::tx) != 0 ? show_tx : show_rx))
                {
                    out.push_back(frame);
                }
                if (next[i] < lists[i].size())
                {
                    heap.emplace(lists[i][next[i]++], i);
                }
            }
            done->store(total);
            if (wake != nullptr)
            {
                wake(); // the next frame's trace_window_update takes the list
            }
            return out;
        });
    s.file_filter_result = merge.get_future();
    s.file_filter_worker = std::jthread(std::move(merge));
}

} // namespace

void trace_window_select_frame(TraceWindowState& s, const Trace& t, uint64_t index, uint64_t context_rows)
{
    s.tab_goto = static_cast<int>(TraceTab::Monitor);
    s.autoscroll = false;
    if (!t.file.empty())
    {
        // The Log's position, also behind the aggregated view: the graph follows it, and a
        // switch to the Log view lands on the frame.
        trace_window_goto(s, t, index - t.begin, context_rows);
    }
    if (s.modes[static_cast<int>(TraceTab::Monitor)] == TraceViewMode::Aggregated)
    {
        // The aggregated row of this frame's id, where it is in the sorted display order (a row
        // the filter hides is left alone).
        const auto it = s.agg_index.find(agg_key(trace_at(t, index)));
        const auto pos = it != s.agg_index.end() ? std::ranges::find(s.agg_order, it->second) : s.agg_order.end();
        if (pos != s.agg_order.end())
        {
            s.selected = static_cast<int>(pos - s.agg_order.begin());
            s.nav_scroll = true;
        }
        return;
    }
    if (!t.file.empty())
    {
        return;
    }
    const auto it = std::ranges::lower_bound(s.rolling, index, {}, &TraceRow::index);
    s.selected = static_cast<int>(it - s.rolling.begin());
    s.nav_scroll = true;
}

void trace_window_goto(TraceWindowState& s, const Trace& t, uint64_t file_index, uint64_t context_rows)
{
    const bool filtered = !s.file_filtered.empty();
    const uint64_t n = filtered ? s.file_filtered.size() : t.file.size();
    uint64_t row = filtered ? static_cast<uint64_t>(std::ranges::lower_bound(s.file_filtered, static_cast<uint32_t>(file_index))
                                                     - s.file_filtered.begin())
                            : file_index;
    row = std::min(row, n > 0 ? n - 1 : 0);
    if (s.modes[static_cast<int>(TraceTab::Monitor)] == TraceViewMode::Rolling)
    {
        s.selected = static_cast<int>(std::min<uint64_t>(row, INT32_MAX)); // a Log row; the aggregated view has its own
    }
    s.file_top = row > context_rows ? row - context_rows : 0;
    s.autoscroll = false;
}

void cycle_stats_add(CycleStats& c, int64_t cycle_ns) noexcept
{
    c.recent[c.count % CycleStats::window] = cycle_ns;
    c.min_ns = std::min(c.min_ns, cycle_ns);
    c.max_ns = std::max(c.max_ns, cycle_ns);
    c.sum_ns += cycle_ns;
    ++c.count;
}

double cycle_stats_median(const CycleStats& c)
{
    const std::size_t n = std::min<uint64_t>(c.count, CycleStats::window);
    if (n == 0)
    {
        return 0.0;
    }
    std::array<int64_t, CycleStats::window> v = c.recent;
    const auto first = v.begin();
    const auto mid = first + static_cast<std::ptrdiff_t>(n / 2);
    std::nth_element(first, mid, first + static_cast<std::ptrdiff_t>(n));
    if (n % 2 != 0)
    {
        return static_cast<double>(*mid);
    }
    return (static_cast<double>(*std::max_element(first, mid)) + static_cast<double>(*mid)) / 2.0;
}

bool trace_filter_accepts(const TraceWindowState& s, const App& app, const BusMessage& m)
{
    return dialog_accepts(s.filter, m) && (s.filter.text.empty() || text_matches(app, s.filter.text, m, s.decimal));
}

std::vector<uint16_t> trace_filter_ifaces(const TraceWindowState& s, const App& app)
{
    std::vector<uint16_t> out;
    for (const Iface& i : app.ifaces)
    {
        const bool in_setup = i.ops != nullptr && std::ranges::any_of(app.setup.networks, [&i](const SetupNetwork& net)
        {
            return std::ranges::any_of(net.interfaces, [&i](const SetupInterface& si)
            {
                return si.enabled && si.driver == i.ops->name && si.name == i.info.name;
            });
        });
        if (in_setup || std::ranges::any_of(s.agg, [&i](const AggRow& r) { return r.last.iface == i.index; }))
        {
            out.push_back(i.index);
        }
    }
    return out;
}

uint64_t trace_changed_mask(const BusMessage& cur, const BusMessage& prev) noexcept
{
    if (prev.len == 0 || is_error_frame(cur) || is_error_frame(prev))
    {
        return 0; // error frames show flags, not bytes
    }
    uint64_t mask = 0;
    for (int i = 0; i < cur.len; ++i)
    {
        mask |= static_cast<uint64_t>(i >= prev.len || cur.data[i] != prev.data[i]) << i;
    }
    return mask;
}

void trace_window_update(TraceWindowState& s, const App& app)
{
    const Trace& t = app.trace;
    if (s.filter_edited != std::chrono::steady_clock::time_point{}
        && std::chrono::steady_clock::now() - s.filter_edited >= filter_debounce)
    {
        s.filter_edited = {};
        s.filter.text = s.filter_edit;
        s.filter_dirty = true;
    }
    if (t.clears != s.clears)
    {
        s.clears = t.clears;
        reset_views(s, t.begin);
        s.file_top = 0;
    }
    if (s.was_measuring && !app.measuring)
    {
        // Every frame of the stopped measurement is in the trace by now (stop joins the RX threads).
        // ponytail: a stop and restart while this window is not drawn (its tab hidden) is not seen;
        // a measurement counter in App would catch that too.
        s.cycle_cut_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
    }
    s.was_measuring = app.measuring;
    if (s.processed < t.begin)
    {
        s.processed = t.begin; // pruned before this window saw it (e.g. its tab was hidden)
    }
    // Pruning drops whole chunks from the front; the rolling rows are sorted by index.
    const auto pruned = std::ranges::lower_bound(s.rolling, t.begin, {}, &TraceRow::index);
    s.rolling.erase(s.rolling.begin(), pruned);

    const bool file_view = !t.file.empty();
    if (s.file_filter_result.valid()
        && (!file_view || s.file_filter_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
    {
        // The merge is done (or the file view ended, live frames came in: not wanted any more).
        s.file_filtered = file_view ? s.file_filter_result.get() : std::vector<uint32_t>{};
        s.file_filter_result = {};
        s.file_filter_worker = {};
    }
    if (file_view && s.processed < t.end)
    {
        file_view_agg(s, app);
        s.processed = t.end;
        s.filter_dirty = true; // the filtered Log of a file view is built by the refilter below
    }
    if (s.filter_dirty)
    {
        s.filter_dirty = false;
        s.rolling.clear();
        s.last_by_key.clear();
        // A file view draws its rows straight from the trace, or from file_filtered (draw_file_rolling).
        s.refilter = file_view ? UINT64_MAX : t.begin;
        file_filter_cancel(s);
        if (file_view && trace_filter_active(s.filter))
        {
            file_view_filter(s, app);
        }
        s.agg_dirty = true;
        proto_rebuild_visible(s, app, s.uds);
        proto_rebuild_visible(s, app, s.j1939);
        s.scroll_pending = true;
    }
    if (s.refilter != UINT64_MAX)
    {
        // Rebuild the rolling log in slices. New frames wait in the trace until it has caught
        // up, so rows stay sorted by index and last_by_key stays in order.
        s.refilter = std::max(s.refilter, t.begin);
        const uint64_t refilter_stop = std::min(s.processed, s.refilter + refilter_rows_per_frame);
        for (; s.refilter < refilter_stop; ++s.refilter)
        {
            monitor_add(s, app, s.refilter);
        }
        if (s.refilter < s.processed)
        {
            return;
        }
        s.refilter = UINT64_MAX;
    }
    // Bounded per frame: a window catching up on millions of frames (tab shown again during a
    // flood) would otherwise stall the UI for seconds. Steady state is ~5k frames per UI frame.
    // ponytail: fixed cap, make it time-based if 100k frames ever take too long per UI frame.
    const uint64_t stop = std::min(t.end, s.processed + 100'000);
    for (; s.processed < stop; ++s.processed)
    {
        process_frame(s, app, s.processed);
    }
}

// --- drawing ------------------------------------------------------------------------------

namespace
{

struct Colors
{
    ImVec4 text, error, changed, faded, request, positive, negative;
};

Colors theme_colors()
{
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const bool dark = bg.x + bg.y + bg.z < 1.5f;
    const auto rgb = [](unsigned hex) { return ImGui::ColorConvertU32ToFloat4(theme_u32(hex)); };
    // faded = the theme's hint colour (>= 4.5:1); changed bytes >= 4.5:1 on the alternate row too.
    const ImVec4 faded = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    return dark ? Colors{ImGui::GetStyleColorVec4(ImGuiCol_Text), rgb(0xff6464), rgb(0xd27800),
                         faded, rgb(0x64b4ff), rgb(0x78ff78), rgb(0xff7878)}
                : Colors{ImGui::GetStyleColorVec4(ImGuiCol_Text), rgb(0xff0000), rgb(0x9a4c00),
                         faded, rgb(0x00008b), rgb(0x006400), rgb(0x8b0000)};
}

enum class Align
{
    Left,
    Center,
    Right,
};

constexpr Align column_align(int col)
{
    switch (col)
    {
    case col_time:
    case col_cycle_min:
    case col_cycle_max:
    case col_cycle_mean:
    case col_cycle_median: return Align::Right;
    case col_name:
    case col_data:
    case col_comment: return Align::Left;
    default: return Align::Center;
    }
}

// 'y' capture: while set, every drawn cell of the selected row is appended here (tab-separated).
// ponytail: cells of columns clipped out of view are skipped by TableSetColumnIndex, so they are
// not copied either; format the row from data instead if that ever matters.
std::string* g_yank = nullptr;
std::vector<VimYankItem>* g_yank_cells = nullptr; // the row's own cells, one item each (column name, text)
bool g_yank_child = false;                        // in a child row: its cells only go to the line

void yank_cell(std::string_view text)
{
    if (g_yank != nullptr)
    {
        if (!g_yank->empty() && g_yank->back() != '\n')
        {
            *g_yank += '\t';
        }
        *g_yank += text;
        if (!g_yank_child)
        {
            g_yank_cells->push_back({.label = ImGui::TableGetColumnName(), .text = std::string(text)});
        }
    }
}

void cell(int col, std::string_view text)
{
    if (!ImGui::TableSetColumnIndex(col) || text.empty())
    {
        return;
    }
    yank_cell(text);
    if (const Align a = column_align(col); a != Align::Left)
    {
        const float w = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
        const float free = ImGui::GetContentRegionAvail().x - w;
        if (free > 0.0f)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (a == Align::Center ? free * 0.5f : free));
        }
    }
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

// Per-frame drawing context.
struct Ctx
{
    App& app;
    TraceWindowState& s;
    Colors colors;
    std::string buf;
    std::string name_buf; // name column override, kept apart from buf which frame_cells reuses
    std::string yank_line;
    int64_t now_ns = 0;
};

// Ends a 'y' capture: the row's text goes to the clipboard. An empty capture (the selected row
// was scrolled into view but its cells not drawn yet) keeps yank_pending for the next frame.
void yank_finish(Ctx& c)
{
    if (g_yank == nullptr)
    {
        return;
    }
    if (!c.yank_line.empty())
    {
        c.s.yank_items[0].text = c.yank_line;
        c.s.vim.yank_menu = 1; // drawn by draw_trace_window
        c.s.yank_pending = false;
    }
    g_yank = nullptr;
    g_yank_cells = nullptr;
}

// Data column in the mono font; bytes set in `changed` in orange (DataColumnDelegate).
void data_cell(Ctx& c, std::string_view text, uint64_t changed, float alpha)
{
    if (!ImGui::TableSetColumnIndex(col_data) || text.empty())
    {
        return;
    }
    yank_cell(text);
    if (c.app.fonts.mono != nullptr)
    {
        ImGui::PushFont(c.app.fonts.mono, 0.0f);
    }
    if (changed == 0)
    {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    }
    else
    {
        // One token per byte ("AB " hex, "A " ascii); mono font, so x = chars * glyph width.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float cw = ImGui::CalcTextSize("0").x;
        ImVec4 hi = c.colors.changed;
        hi.w *= alpha;
        const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 orange = ImGui::GetColorU32(hi);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        std::size_t p = 0;
        for (int byte = 0; p < text.size(); ++byte)
        {
            std::size_t e = text.find(' ', p);
            e = e == std::string_view::npos ? text.size() : e;
            const bool hit = byte < 64 && ((changed >> byte) & 1) != 0;
            dl->AddText(ImVec2(pos.x + static_cast<float>(p) * cw, pos.y), hit ? orange : normal, text.data() + p,
                        text.data() + e);
            p = e + 1;
        }
        ImGui::Dummy(ImVec2(static_cast<float>(text.size()) * cw, ImGui::GetTextLineHeight()));
    }
    if (c.app.fonts.mono != nullptr)
    {
        ImGui::PopFont();
    }
}

// Row color: red for error frames, alpha for the aggregated view's stale fade.
void push_row_color(const Ctx& c, const BusMessage& m, float alpha)
{
    ImVec4 col = is_error_frame(m) ? c.colors.error : c.colors.text;
    col.w *= alpha;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
}

// Every column of a frame except Index. prev_ts: for Delta; prev: for changed bytes (or null).
void frame_cells(Ctx& c, const BusMessage& m, int64_t prev_ts, const BusMessage* prev, float alpha,
                 std::string_view name_override = {})
{
    std::string& b = c.buf;
    b.clear();
    append_time(b, c.s.ts_mode, m.ts_ns, prev_ts, c.s.first_ts_ns);
    cell(col_time, b);
    cell(col_channel, iface_name(c.app, m.iface));
    cell(col_direction, has_flag(m, bus_flag::tx) ? "TX" : "RX");
    b.clear();
    append_type(b, m);
    cell(col_type, b);
    b.clear();
    trace_append_id(b, m, c.s.decimal);
    cell(col_id, b);
    const DbText db = db_text(c.app, m);
    cell(col_sender, db.sender);
    cell(col_name, name_override.empty() ? db.name : name_override);
    b.clear();
    std::format_to(std::back_inserter(b), "{}", m.len);
    cell(col_dlc, b);
    b.clear();
    if (c.s.decimal || has_flag(m, bus_flag::rtr))
    {
        trace_append_data(b, m, true); // an RTR's DLC is only the requested length: no bytes
    }
    else
    {
        append_data(b, m, c.s.ascii);
    }
    if (b.empty() && is_error_frame(m))
    {
        append_error_flags(b, m.errors); // append_data writes nothing for an empty error frame
    }
    data_cell(c, b, prev != nullptr ? trace_changed_mask(m, *prev) : 0, alpha);
    std::string_view comment = db.comment;
    cell(col_comment, comment.substr(0, comment.find('\n')));
}

// Cycle min/max/mean/median in ms, 3 decimals like the Time column; blank before two frames.
void cycle_cells(Ctx& c, const CycleStats& st)
{
    if (st.count == 0)
    {
        return;
    }
    const auto ms_cell = [&](int col, double ns)
    {
        c.buf.clear();
        std::format_to(std::back_inserter(c.buf), "{:.3f}", ns / 1e6);
        cell(col, c.buf);
    };
    ms_cell(col_cycle_min, static_cast<double>(st.min_ns));
    ms_cell(col_cycle_max, static_cast<double>(st.max_ns));
    ms_cell(col_cycle_mean, static_cast<double>(st.sum_ns) / static_cast<double>(st.count));
    if (ImGui::TableGetColumnFlags(col_cycle_median) & ImGuiTableColumnFlags_IsEnabled) // hidden: skip nth_element
    {
        ms_cell(col_cycle_median, cycle_stats_median(st));
    }
}

void index_cell(Ctx& c, uint64_t n)
{
    c.buf.clear();
    append_grouped(c.buf, n); // "182 764 569"
    cell(col_index, c.buf);
}

void setup_columns(bool sortable, int count)
{
    const float* widths = fixed_widths();
    for (int i = 0; i < count; ++i)
    {
        ImGuiTableColumnFlags f = stretch_column(i) ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
        if (sortable && i == col_id)
        {
            f |= ImGuiTableColumnFlags_DefaultSort;
        }
        if (!sortable || i == col_cycle_median) // median sorting would nth_element per compare
        {
            f |= ImGuiTableColumnFlags_NoSort;
        }
        if (i == col_index)
        {
            f |= ImGuiTableColumnFlags_NoHide;
        }
        if (columns[i].hidden)
        {
            f |= ImGuiTableColumnFlags_DefaultHide;
        }
        ImGui::TableSetupColumn(columns[i].label, f, stretch_column(i) ? columns[i].weight : widths[i],
                                static_cast<ImGuiID>(i));
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
}

// width: 0 = the whole content region, negative = all but that much (ImGui item width rules).
bool begin_table(const char* id, float px, bool sortable, bool decimal, int count = col_count, float width = 0.0f)
{
    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable
                            | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
                            | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter
                            | ImGuiTableFlags_SizingFixedFit;
    if (sortable)
    {
        flags |= ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    }
    // Columns fit the window; ScrollX only once the enabled columns' minimum widths (fixed ones as
    // resized, from last frame) no longer fit.
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiTable* prev = ImGui::TableFindByID(ImGui::GetID(id));
    const float* widths = fixed_widths();
    float min_width = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        const bool known = prev != nullptr && i < prev->ColumnsCount;
        if (known ? !prev->Columns[i].IsEnabled : columns[i].hidden)
        {
            continue;
        }
        const float w = i == col_data && decimal ? stretch_min_width * px * 4.0f / 3.0f // "255 " vs "FF "
                        : stretch_column(i)       ? stretch_min_width * px
                        : known && prev->Columns[i].WidthRequest > 0.0f ? prev->Columns[i].WidthRequest
                                                                        : widths[i];
        min_width += w + style.CellPadding.x * 2.0f + 1.0f; // + padding and the vertical border
    }
    const float inner_width = min_width > ImGui::GetContentRegionAvail().x ? min_width : 0.0f;
    if (!ImGui::BeginTable(id, count, flags, ImVec2(width, 0.0f), inner_width))
    {
        return false;
    }
    setup_columns(sortable, count);
    return true;
}

// j/k/gg/G/Ctrl+d/u/f/b select a row of the current table (count rows in display order), / focuses
// the Filter field, h/l switch the Monitor/UDS/J1939 tabs. Call inside the table.
void table_nav(Ctx& c, int count, bool rolling)
{
    TraceWindowState& s = c.s;
    int h = 0;
    const int page = static_cast<int>(ImGui::GetWindowHeight() / ImGui::GetTextLineHeightWithSpacing());
    if (vim_nav(s.vim, s.selected, count, page, s.focus_filter, h))
    {
        s.nav_scroll = true;
        if (rolling)
        {
            s.autoscroll = s.selected == count - 1; // reading back through the log stops following it
        }
    }
    if (s.selected >= 0 && vim_yank())
    {
        s.yank_pending = true;
        s.nav_scroll = true; // bring the row into the clipper so its cells get drawn
    }
    // Held until the tab bar has switched (SetSelected lands a frame later, maybe on an earlier tab).
    if (const int to = std::clamp(static_cast<int>(s.tab) + h, 0, static_cast<int>(TraceTab::Count) - 1);
        to != static_cast<int>(s.tab))
    {
        s.tab_goto = to;
    }
}

// 'y' capture of row r. Call before its first cell: the selected row's cells (and child rows) are
// all drawn once the next row starts.
void yank_row(Ctx& c, int r)
{
    if (r != c.s.selected)
    {
        yank_finish(c);
        return;
    }
    if (c.s.yank_pending)
    {
        c.yank_line.clear();
        g_yank = &c.yank_line;
        c.s.yank_items.assign({{.label = "Row"}});
        g_yank_cells = &c.s.yank_items;
        g_yank_child = false;
    }
}

// A child row (signal, metadata, raw frame) under the row being captured: its own line.
void child_row()
{
    ImGui::TableNextRow();
    if (g_yank != nullptr)
    {
        *g_yank += '\n';
        g_yank_child = true;
    }
}

// Highlights row r when selected and scrolls to it after a move. Call after its first cell.
void nav_row(Ctx& c, int r)
{
    if (r != c.s.selected)
    {
        return;
    }
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
    if (c.s.nav_scroll)
    {
        ImGui::SetScrollHereY();
        c.s.nav_scroll = false;
    }
}

void autoscroll(Ctx& c)
{
    if (c.s.autoscroll && c.s.scroll_pending)
    {
        ImGui::SetScrollHereY(1.0f);
    }
}

void append_signal_value(std::string& b, uint64_t raw, std::string_view value_name, double phys,
                         std::string_view unit, bool float32)
{
    if (!value_name.empty())
    {
        std::format_to(std::back_inserter(b), "{} - {}", raw, value_name);
        return;
    }
    if (float32)
    {
        std::format_to(std::back_inserter(b), "{}", static_cast<float>(phys)); // shortest float round-trip
    }
    else
    {
        std::format_to(std::back_inserter(b), "{:.15g}", phys);
    }
    if (!unit.empty())
    {
        b += ' ';
        b += unit;
    }
}

// Signal child rows under an aggregated frame (DBC / LDF), faded when muxed out.
void signal_rows(Ctx& c, const BusMessage& m, float alpha)
{
    std::string& b = c.buf;
    if (m.type == BusType::LIN)
    {
        const LinFrame* f = setup_find_lin_frame(c.app.setup, m);
        if (f == nullptr)
        {
            return;
        }
        for (const LinSignal& sig : f->signals)
        {
            child_row();
            cell(col_name, sig.name);
            const uint64_t raw = lin_signal_extract_raw(sig, std::span(m.data.data(), m.len));
            b.clear();
            append_signal_value(b, raw, lin_signal_value_name(sig, raw), lin_signal_raw_to_physical(sig, raw), sig.unit,
                                false);
            data_cell(c, b, 0, alpha);
        }
        return;
    }
    const CanDbMessage* d = setup_find_can_message(c.app.setup, m);
    if (d == nullptr)
    {
        return;
    }
    for (const CanDbSignal& sig : d->signals)
    {
        child_row();
        // ponytail: muxed-out signals are left blank; the old mux cache (last value per mux) is not ported. Port it if blanks are a complaint.
        const bool present = can_signal_present(*d, sig, m);
        ImVec4 faded = c.colors.faded;
        faded.w *= alpha;
        if (!present)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, faded);
        }
        cell(col_name, sig.name);
        if (present)
        {
            const uint64_t raw = can_signal_extract_raw(sig, m);
            b.clear();
            trace_append_signal_value(b, sig, raw);
            data_cell(c, b, 0, alpha);
        }
        std::string_view comment = sig.comment;
        cell(col_comment, comment.substr(0, comment.find('\n')));
        if (!present)
        {
            ImGui::PopStyleColor();
        }
    }
}

bool has_signals(const App& app, const BusMessage& m)
{
    if (m.type == BusType::LIN)
    {
        const LinFrame* f = setup_find_lin_frame(app.setup, m);
        return f != nullptr && !f->signals.empty();
    }
    const CanDbMessage* d = setup_find_can_message(app.setup, m);
    return d != nullptr && !d->signals.empty();
}

// Index cell with a tree node when the row has children; returns whether it is open.
// id: the row's order of appearance; a pointer into the row vector dies when it grows.
bool tree_index_cell(Ctx& c, uint64_t n, bool has_children, uint32_t id)
{
    if (!has_children)
    {
        index_cell(c, n);
        return false;
    }
    ImGui::TableSetColumnIndex(col_index);
    c.buf.clear();
    append_grouped(c.buf, n);
    yank_cell(c.buf);
    return ImGui::TreeNodeEx(reinterpret_cast<const void*>(static_cast<uintptr_t>(id)), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s",
                             c.buf.c_str());
}

bool agg_less(const App& app, const AggRow& a, const AggRow& b, int col)
{
    const BusMessage& x = a.last;
    const BusMessage& y = b.last;
    switch (col)
    {
    case col_index: return a.order < b.order;
    case col_time: return x.ts_ns < y.ts_ns;
    case col_channel: return x.iface < y.iface;
    case col_direction: return has_flag(x, bus_flag::tx) < has_flag(y, bus_flag::tx);
    case col_type: return x.flags < y.flags;
    case col_sender: return db_text(app, x).sender < db_text(app, y).sender;
    case col_name: return db_text(app, x).name < db_text(app, y).name;
    case col_dlc: return x.len < y.len;
    case col_data: return std::ranges::lexicographical_compare(std::span(x.data.data(), x.len), std::span(y.data.data(), y.len));
    case col_cycle_min: return a.cycle.min_ns < b.cycle.min_ns;
    case col_cycle_max: return a.cycle.max_ns < b.cycle.max_ns;
    case col_cycle_mean:
        return static_cast<double>(a.cycle.sum_ns) * static_cast<double>(b.cycle.count)
               < static_cast<double>(b.cycle.sum_ns) * static_cast<double>(a.cycle.count);
    default: return std::pair(x.type, can_id(x)) < std::pair(y.type, can_id(y));
    }
}

void draw_monitor_aggregated(Ctx& c, float px)
{
    TraceWindowState& s = c.s;
    if (!begin_table("##agg", px, true, s.decimal, col_agg_count))
    {
        return;
    }
    // Refilter and sort only when the row set, filter, setup or sort spec changed, or (sorted on a
    // value that new frames change) every agg_resort_s: the sort cost ~0.8 % of a flood per frame.
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    const ImGuiTableColumnSortSpecs* spec = specs != nullptr && specs->SpecsCount > 0 ? &specs->Specs[0] : nullptr;
    const bool live_key = spec != nullptr && spec->ColumnIndex != col_index && spec->ColumnIndex != col_channel
                          && spec->ColumnIndex != col_direction && spec->ColumnIndex != col_id;
    const double now = ImGui::GetTime();
    if (s.agg_dirty || s.agg_setup_gen != c.app.setup.generation || (specs != nullptr && specs->SpecsDirty)
        || (live_key && now - s.agg_sorted_time >= agg_resort_s))
    {
        s.agg_dirty = false;
        s.agg_setup_gen = c.app.setup.generation;
        s.agg_sorted_time = now;
        ++s.agg_sorts;
        if (specs != nullptr)
        {
            specs->SpecsDirty = false;
        }
        s.agg_order.clear();
        for (uint32_t i = 0; i < s.agg.size(); ++i)
        {
            if (trace_filter_accepts(s, c.app, s.agg[i].last))
            {
                s.agg_order.push_back(i);
            }
        }
        if (spec != nullptr)
        {
            const bool desc = spec->SortDirection == ImGuiSortDirection_Descending;
            std::ranges::stable_sort(s.agg_order, [&](uint32_t a, uint32_t b)
                                     {
                                         const AggRow& x = s.agg[desc ? b : a];
                                         const AggRow& y = s.agg[desc ? a : b];
                                         return agg_less(c.app, x, y, spec->ColumnIndex);
                                     });
        }
    }
    // ponytail: the selection is a display position, so a re-sort can put another row under it;
    // track the agg index if that ever matters.
    table_nav(c, static_cast<int>(s.agg_order.size()), false);
    for (int r = 0; r < static_cast<int>(s.agg_order.size()); ++r)
    {
        const uint32_t i = s.agg_order[static_cast<std::size_t>(r)];
        const AggRow& row = s.agg[i];
        // Stale rows fade while measuring (58 alpha steps per second, floor 80), as before.
        float alpha = 1.0f;
        if (c.app.measuring)
        {
            const double age = static_cast<double>(c.now_ns - row.last.ts_ns) / 1e9;
            alpha = static_cast<float>(std::clamp(255.0 - age * 58.0, 160.0, 255.0) / 255.0); // stale rows stay readable
        }
        ImGui::TableNextRow();
        push_row_color(c, row.last, alpha);
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableSetColumnIndex(col_index); // cursor at this row's first cell, even when hidden
        const float y = ImGui::GetCursorScreenPos().y;
        const bool visible = y + ImGui::GetTextLineHeightWithSpacing() >= ImGui::GetWindowPos().y
                             && y <= ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
        yank_row(c, r);
        const bool open = tree_index_cell(c, row.order, has_signals(c.app, row.last), row.order);
        nav_row(c, r);
        // Off-screen rows keep only their index cell (one line, so the scroll height stays right):
        // formatting every cell of thousands of ids cost ~35 % of the main thread under a flood.
        if (open || visible)
        {
            frame_cells(c, row.last, row.has_prev ? row.prev.ts_ns : 0, row.has_prev ? &row.prev : nullptr, alpha);
            cycle_cells(c, row.cycle);
        }
        if (open)
        {
            signal_rows(c, row.last, alpha);
        }
        ImGui::PopID();
        ImGui::PopStyleColor();
    }
    ImGui::EndTable();
    yank_finish(c);
}

void draw_monitor_rolling(Ctx& c, float px)
{
    TraceWindowState& s = c.s;
    if (!begin_table("##rolling", px, false, c.s.decimal))
    {
        return;
    }
    const Trace& t = c.app.trace;
    table_nav(c, static_cast<int>(s.rolling.size()), true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(s.rolling.size()));
    if (s.nav_scroll && s.selected >= 0)
    {
        clipper.IncludeItemByIndex(s.selected);
    }
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const TraceRow& row = s.rolling[static_cast<std::size_t>(r)];
            const BusMessage m = trace_at(t, row.index);
            const bool has_prev = row.prev != UINT64_MAX && row.prev >= t.begin;
            const BusMessage prev = has_prev ? trace_at(t, row.prev) : BusMessage{};
            ImGui::TableNextRow();
            push_row_color(c, m, 1.0f);
            yank_row(c, r);
            index_cell(c, row.index - s.index_base + 1);
            nav_row(c, r);
            frame_cells(c, m, has_prev ? prev.ts_ns : 0, has_prev ? &prev : nullptr, 1.0f);
            ImGui::PopStyleColor();
        }
    }
    autoscroll(c);
    ImGui::EndTable();
    yank_finish(c);
}

// Previous frame with m's delta key (Delta time, changed bytes): the cache's per-id index gives the
// row's frames in time order, so it is two binary searches, exact for any id rate.
std::optional<BusMessage> file_prev(const App& app, uint64_t index, const BusMessage& m)
{
    const FrameCache* c = app.trace_file.get();
    if (c == nullptr)
    {
        return std::nullopt;
    }
    const uint32_t id = is_error_frame(m) ? replay_error_id : m.id;
    const auto row = std::ranges::lower_bound(c->rows, std::pair{m.iface, id}, {},
                                              [](const FrameCacheRow& r) { return std::pair{r.channel, r.id}; });
    if (row == c->rows.end() || row->channel != m.iface || row->id != id)
    {
        return std::nullopt;
    }
    const auto list = c->row_frames.subspan(row->first, row->count);
    const uint64_t key = delta_key(m);
    for (auto it = std::ranges::lower_bound(list, static_cast<uint32_t>(index - app.trace.begin)); it != list.begin();)
    {
        if (const BusMessage p = frame_cache_frame(*c, *--it); delta_key(p) == key) // the row mixes RX and TX
        {
            return p;
        }
    }
    return std::nullopt;
}

// Vertical scroll indicator right of the file view's Log table, linear in rows: ImGui's scrollbar
// widget with 64-bit positions (its window scrolling is float pixels, too coarse for 100M rows).
void file_scrollbar(TraceWindowState& s, uint64_t n, uint64_t page, float width)
{
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImRect bb(p0, ImVec2(p0.x + width, p0.y + ImGui::GetItemRectSize().y));
    ImS64 top = static_cast<ImS64>(s.file_top);
    if (ImGui::ScrollbarEx(bb, ImGui::GetID("##file_scroll"), ImGuiAxis_Y, &top, static_cast<ImS64>(page), static_cast<ImS64>(n),
                           ImDrawFlags_RoundCornersAll))
    {
        s.file_top = static_cast<uint64_t>(top);
        s.autoscroll = false;
    }
    ImGui::Dummy(bb.GetSize());
}

// Unfiltered Log of a file view: the page of rows at file_top, read from the mapping. Wheel,
// vim keys, the position slider and "Go to" move file_top; Autoscroll pins it to the end.
void draw_file_rolling(Ctx& c, float px)
{
    TraceWindowState& s = c.s;
    const Trace& t = c.app.trace;
    if (s.file_filter_result.valid())
    {
        return; // the merge runs: the toolbar shows "Filtering... N %", the list lands in trace_window_update
    }
    const std::vector<uint32_t>* list = trace_filter_active(s.filter) ? &s.file_filtered : nullptr;
    const uint64_t n = list != nullptr ? list->size() : trace_size(t);
    const auto frame_of = [&](uint64_t r) { return list != nullptr ? (*list)[r] : r; }; // row -> file index
    if (n == 0)
    {
        ImGui::TextDisabled("No frames match the filter");
        return;
    }
    const int64_t first_ts = trace_at(t, t.begin).ts_ns;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Go to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f * px);
    if (ImGui::InputTextWithHint("##goto", "d h:m:s", &s.file_goto, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        const std::optional<double> at = parse_duration(s.file_goto);
        if (!at)
        {
            ImGui::SetKeyboardFocusHere(-1); // keep editing a malformed time
            return;
        }
        s.file_goto = format_duration(*at);
        const auto ts = first_ts + static_cast<int64_t>(*at * 1e9);
        trace_window_goto(s, t, static_cast<uint64_t>(std::ranges::lower_bound(t.file, ts, {}, &FrameCacheRec::ts_ns) - t.file.begin()), 0);
    }
    ImGui::SetItemTooltip("Time since the first frame (90, 1:30, 1:02:03.5, 2d 1:02:03), Enter jumps there");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const uint64_t zero = 0;
    const uint64_t last = n - 1;
    const double top_s = static_cast<double>(trace_at(t, t.begin + frame_of(std::min(s.file_top, last))).ts_ns - first_ts) / 1e9;
    std::string position = "row ";
    append_grouped(position, s.file_top + 1);
    position += "  at  " + format_duration(top_s); // shown as is: no printf specifier in it
    if (ImGui::SliderScalar("##position", ImGuiDataType_U64, &s.file_top, &zero, &last, position.c_str()))
    {
        s.autoscroll = false;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float row_h = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
    const uint64_t page = static_cast<uint64_t>(std::max(1.0f, ImGui::GetContentRegionAvail().y / row_h - 2.0f)); // - header
    const float bar_w = style.ScrollbarSize;
    if (!begin_table("##file_rolling", px, false, s.decimal, col_count, -bar_w))
    {
        return;
    }
    // ponytail: vim selection is an int, so rows past 2^31 are reached with the slider only.
    const int count = static_cast<int>(std::min<uint64_t>(n, INT32_MAX));
    table_nav(c, count, true);
    if (s.nav_scroll && s.selected >= 0) // keep the moved selection on the page
    {
        const auto sel = static_cast<uint64_t>(s.selected);
        s.file_top = sel < s.file_top ? sel : sel >= s.file_top + page ? sel - page + 1 : s.file_top;
    }
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
    {
        const auto step = static_cast<int64_t>(-ImGui::GetIO().MouseWheel * 3.0f);
        s.file_top = static_cast<uint64_t>(std::max<int64_t>(0, static_cast<int64_t>(s.file_top) + step));
        s.autoscroll = false;
    }
    if (s.autoscroll)
    {
        s.file_top = n > page ? n - page : 0;
    }
    s.file_top = std::min(s.file_top, n > page ? n - page : 0);
    for (uint64_t r = s.file_top; r < std::min(n, s.file_top + page); ++r)
    {
        const uint64_t index = t.begin + frame_of(r);
        const BusMessage m = trace_at(t, index);
        const std::optional<BusMessage> prev = file_prev(c.app, index, m);
        ImGui::TableNextRow();
        push_row_color(c, m, 1.0f);
        yank_row(c, static_cast<int>(r));
        index_cell(c, index - t.begin + 1);
        nav_row(c, static_cast<int>(r));
        frame_cells(c, m, prev ? prev->ts_ns : 0, prev ? &*prev : nullptr, 1.0f);
        ImGui::PopStyleColor();
    }
    ImGui::EndTable();
    yank_finish(c);
    file_scrollbar(s, n, page, bar_w);
}

const char* metadata_abbrev(std::string_view name)
{
    if (name == "Priority") return "P";
    if (name == "Reserved") return "R";
    if (name == "Data Page") return "DP";
    if (name == "PDU Format") return "PF";
    if (name == "PDU Specific") return "PS";
    if (name == "Source Address") return "SA";
    return "";
}

// ISO-TP frame kind of a raw UDS frame (child rows of a protocol message).
void append_tp_name(std::string& out, const BusMessage& m)
{
    const uint8_t pci = m.len > 0 ? m.data[0] : 0;
    switch (pci >> 4)
    {
    case 0x0: out += "[tp] Single Frame"; break;
    case 0x1: out += "[tp] First Frame"; break;
    case 0x2: std::format_to(std::back_inserter(out), "[tp] Consecutive Frame (SN: {})", pci & 0x0F); break;
    case 0x3: out += "[tp] Flow Control"; break;
    default: break;
    }
}

void proto_cells(Ctx& c, const ProtoRow& row)
{
    const ProtocolMessage& pm = row.msg;
    const BusMessage* f = pm.raw_frames.empty() ? nullptr : &pm.raw_frames.front();
    const bool j1939 = is_protocol(pm, "J1939");
    std::string& b = c.buf;
    b.clear();
    append_time(b, c.s.ts_mode, pm.ts_ns, row.prev_ts_ns, c.s.first_ts_ns);
    cell(col_time, b);
    if (f != nullptr)
    {
        cell(col_channel, iface_name(c.app, f->iface));
        cell(col_direction, has_flag(*f, bus_flag::tx) ? "TX" : "RX");
    }
    cell(col_type, j1939 ? (((pm.id >> 8) & 0xFF) < 240 ? "PDU1" : "PDU2") : "UDS");
    b.clear();
    const uint32_t raw_id = f != nullptr ? f->id : 0;
    if (j1939)
    {
        std::format_to(std::back_inserter(b), "0x{:X} (PGN:{:X})", raw_id, pm.id);
    }
    else
    {
        std::format_to(std::back_inserter(b), "0x{:X} (SID:{:02X})", raw_id, pm.id);
    }
    cell(col_id, b);
    if (!j1939)
    {
        cell(col_sender, pm.type == MessageType::Request ? "Tester" : "ECU");
    }
    cell(col_name, pm.name);
    b.clear();
    std::format_to(std::back_inserter(b), "{}", pm.payload.size());
    cell(col_dlc, b);
    b.clear();
    if (c.s.decimal)
    {
        append_dec_bytes(b, pm.payload);
    }
    else
    {
        append_bytes(b, pm.payload, c.s.ascii);
    }
    data_cell(c, b, 0, 1.0f);
    cell(col_comment, pm.description);
}

ImVec4 proto_color(const Ctx& c, const ProtocolMessage& pm)
{
    switch (pm.type)
    {
    case MessageType::Request: return c.colors.request;
    case MessageType::PositiveResponse: return c.colors.positive;
    case MessageType::NegativeResponse: return c.colors.negative;
    default: return c.colors.text;
    }
}

void draw_proto_rolling(Ctx& c, ProtoView& v, float px)
{
    if (!begin_table("##proto_rolling", px, false, c.s.decimal))
    {
        return;
    }
    table_nav(c, static_cast<int>(v.visible.size()), true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(v.visible.size()));
    if (c.s.nav_scroll && c.s.selected >= 0)
    {
        clipper.IncludeItemByIndex(c.s.selected);
    }
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const ProtoRow& row = v.rolling[v.visible[static_cast<std::size_t>(r)]];
            ImGui::TableNextRow();
            ImGui::PushStyleColor(ImGuiCol_Text, proto_color(c, row.msg));
            yank_row(c, r);
            index_cell(c, row.order);
            nav_row(c, r);
            proto_cells(c, row);
            ImGui::PopStyleColor();
        }
    }
    autoscroll(c);
    ImGui::EndTable();
    yank_finish(c);
}

// Aggregated protocol rows (one per UDS id/SID or J1939 PGN/SA) with metadata and raw frames
// as children.
void draw_proto_aggregated(Ctx& c, ProtoView& v, float px)
{
    if (!begin_table("##proto_agg", px, false, c.s.decimal))
    {
        return;
    }
    int shown = 0; // rows passing the filter; the row count for table_nav after the loop
    for (std::size_t i = 0; i < v.aggregated.size(); ++i)
    {
        const ProtoRow& row = v.aggregated[i];
        const ProtocolMessage& pm = row.msg;
        if (!proto_accepts(c.s, c.app, pm))
        {
            continue;
        }
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, proto_color(c, pm));
        yank_row(c, shown);
        const bool open = tree_index_cell(c, row.order, !pm.metadata.empty() || !pm.raw_frames.empty(), row.order);
        nav_row(c, shown++);
        proto_cells(c, row);
        ImGui::PopStyleColor();
        if (open)
        {
            for (const auto& [name, value] : pm.metadata)
            {
                child_row();
                cell(col_type, metadata_abbrev(name));
                cell(col_name, name);
                c.buf.clear();
                std::format_to(std::back_inserter(c.buf), "{}", value);
                data_cell(c, c.buf, 0, 1.0f);
            }
            const bool uds = is_protocol(pm, "uds");
            int64_t prev_ts = pm.ts_ns;
            for (const BusMessage& f : pm.raw_frames)
            {
                child_row();
                push_row_color(c, f, 1.0f);
                c.name_buf.clear();
                if (uds)
                {
                    append_tp_name(c.name_buf, f);
                }
                frame_cells(c, f, prev_ts, nullptr, 1.0f, c.name_buf);
                prev_ts = f.ts_ns;
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopID();
    }
    table_nav(c, shown, false); // a move shows (and scrolls) from the next frame
    ImGui::EndTable();
    yank_finish(c);
}

void draw_filter_popup(Ctx& c)
{
    if (!ImGui::BeginPopup("##trace_filter"))
    {
        return;
    }
    TraceFilter& f = c.s.filter;
    bool changed = ImGui::Checkbox("Show TX", &f.show_tx);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Show RX", &f.show_rx);

    ImGui::SeparatorText("Interfaces");
    for (const uint16_t i : trace_filter_ifaces(c.s, c.app))
    {
        const Iface& iface = c.app.ifaces[i];
        bool shown = !f.hidden_ifaces.contains(i);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Checkbox(iface.info.name.c_str(), &shown))
        {
            if (shown)
            {
                f.hidden_ifaces.erase(i);
            }
            else
            {
                f.hidden_ifaces.insert(i);
            }
            changed = true;
        }
        ImGui::PopID();
    }

    // ponytail: lists the ids seen in this trace, not every DBC/LDF message. Merge the setup's messages in if wanted.
    ImGui::SeparatorText("Messages");
    std::set<std::pair<bool, uint32_t>> seen; // (LIN, id)
    for (const AggRow& row : c.s.agg)
    {
        if (!is_error_frame(row.last) || row.last.type == BusType::LIN)
        {
            seen.emplace(row.last.type == BusType::LIN, row.last.type == BusType::LIN ? row.last.id : can_id(row.last));
        }
    }
    const bool show_all = ImGui::Button("Show all");
    ImGui::SameLine();
    const bool hide_all = ImGui::Button("Hide all");
    if (show_all || hide_all)
    {
        f.hidden_ids.clear();
        f.hidden_lin_ids.clear();
        for (const auto& [lin, id] : seen)
        {
            if (hide_all)
            {
                (lin ? f.hidden_lin_ids : f.hidden_ids).insert(id);
            }
        }
        changed = true;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    if (ImGui::BeginChild("##ids", ImVec2(320.0f * px, std::min(300.0f * px, (static_cast<float>(seen.size()) + 1.0f) * ImGui::GetFrameHeightWithSpacing())),
                          ImGuiChildFlags_Borders))
    {
        for (const auto& [lin, id] : seen)
        {
            auto& hidden = lin ? f.hidden_lin_ids : f.hidden_ids;
            bool shown = !hidden.contains(id);
            BusMessage probe{.id = id, .flags = id > can_id_mask_standard ? bus_flag::extended : uint16_t{0},
                             .type = lin ? BusType::LIN : BusType::CAN};
            c.buf.clear();
            append_id(c.buf, probe);
            if (const DbText db = db_text(c.app, probe); !db.name.empty())
            {
                c.buf += "  ";
                c.buf += db.name;
            }
            c.buf += lin ? "  (LIN)" : "";
            ImGui::PushID(static_cast<int>(id | (lin ? 0x80000000u : 0u)));
            if (ImGui::Checkbox(c.buf.c_str(), &shown))
            {
                if (shown)
                {
                    hidden.erase(id);
                }
                else
                {
                    hidden.insert(id);
                }
                changed = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (changed)
    {
        c.s.filter_dirty = true;
    }
    ImGui::EndPopup();
}

void draw_toolbar(Ctx& c, float px)
{
    TraceWindowState& s = c.s;
    TraceViewMode& mode = s.modes[static_cast<int>(s.tab)];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("View");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f * px);
    int m = static_cast<int>(mode);
    if (ImGui::Combo("##view", &m, "Monitor\0Log\0"))
    {
        mode = static_cast<TraceViewMode>(m);
        s.scroll_pending = true;
        s.selected = -1;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    // Label and combo wrap together when the window gets narrow (T48).
    same_line_or_wrap(ImGui::CalcTextSize("Timestamps").x + style.ItemSpacing.x + 130.0f * px);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Timestamps");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f * px);
    // Combo order of the old window; values are TimestampMode.
    constexpr TimestampMode ts_items[] = {TimestampMode::Absolute, TimestampMode::AbsoluteUtc, TimestampMode::Relative,
                                          TimestampMode::Delta};
    constexpr const char* ts_labels[] = {"Absolute", "Absolute (UTC)", "Relative", "Delta"};
    const auto cur = std::ranges::find(ts_items, s.ts_mode) - std::begin(ts_items);
    if (ImGui::BeginCombo("##timestamps", ts_labels[cur < 4 ? cur : 0]))
    {
        for (int i = 0; i < 4; ++i)
        {
            if (ImGui::Selectable(ts_labels[i], i == cur))
            {
                s.ts_mode = ts_items[i];
            }
        }
        ImGui::EndCombo();
    }
    same_line_or_wrap(ImGui::CalcTextSize("Hex").x + style.FramePadding.x * 2.0f);
    if (ImGui::Button(s.decimal ? "Dec###radix" : "Hex###radix"))
    {
        s.decimal = !s.decimal;
        s.filter_dirty = s.filter_dirty || !s.filter.text.empty(); // the ID text the filter matches changed
    }
    ImGui::SetItemTooltip("Data and ID columns in hex or decimal");
    same_line_or_wrap(ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Autoscroll").x);
    ImGui::Checkbox("Autoscroll", &s.autoscroll);
    same_line_or_wrap(icon_text_button_width("Clear"));
    command_button(c.app, Command::TraceClear, Icon::EditClear, "Clear"); // clears the trace, tooltip with Esc
    const float filter_btn = icon_text_button_width("Filter") + style.ItemSpacing.x;
    same_line_or_wrap(80.0f * px + filter_btn);
    ImGui::SetNextItemWidth(std::max(80.0f * px, ImGui::GetContentRegionAvail().x - filter_btn));
    if (s.focus_filter)
    {
        ImGui::SetKeyboardFocusHere();
        s.focus_filter = false;
    }
    if (ImGui::InputTextWithHint("##filter", "Filter", &s.filter_edit))
    {
        s.filter_edited = std::chrono::steady_clock::now();
    }
    ImGui::SameLine();
    if (icon_text_button("Filter", Icon::Filter))
    {
        ImGui::OpenPopup("##trace_filter");
    }
    int pct = -1; // a running refilter (live log) or file view merge (worker)
    if (s.refilter != UINT64_MAX && s.processed > c.app.trace.begin)
    {
        const uint64_t begin = c.app.trace.begin;
        pct = static_cast<int>((std::max(s.refilter, begin) - begin) * 100 / (s.processed - begin));
    }
    else if (s.file_filter_result.valid())
    {
        pct = s.file_filter_total == 0 ? 100 : static_cast<int>(s.file_filter_done.load() * 100 / s.file_filter_total);
    }
    if (pct >= 0)
    {
        same_line_or_wrap(ImGui::CalcTextSize("Filtering... 100 %").x);
        ImGui::TextDisabled("Filtering... %d %%", pct);
    }
    draw_filter_popup(c);
}

} // namespace

void trace_append_data(std::string& out, const BusMessage& m, bool decimal)
{
    if (is_error_frame(m))
    {
        append_error_flags(out, m.errors);
        return;
    }
    if (has_flag(m, bus_flag::rtr))
    {
        return; // len is the requested DLC, the frame carries no data
    }
    if (!decimal)
    {
        append_data(out, m, false);
        return;
    }
    append_dec_bytes(out, std::span(m.data).first(m.len));
}

void trace_append_id(std::string& out, const BusMessage& m, bool decimal)
{
    if (!decimal || (is_error_frame(m) && m.type == BusType::CAN))
    {
        append_frame_id(out, m);
        return;
    }
    char t[10];
    out.append(t, std::to_chars(t, t + sizeof t, m.type == BusType::LIN ? m.id & 0x3F : can_id(m)).ptr);
}

void trace_append_signal_value(std::string& out, const CanDbSignal& sig, uint64_t raw)
{
    append_signal_value(out, raw, can_signal_value_name(sig, raw), can_signal_raw_to_physical(sig, raw), sig.unit,
                        sig.value_type == SignalValueType::float32 && sig.length == 32);
}

void draw_trace_window(App& app, TraceWindowState& s, const WorkspaceTab& tab)
{
    trace_window_update(s, app);
    const std::string title = workspace_window_name(tab, "Trace");
    if (!ImGui::Begin(title.c_str()))
    {
        ImGui::End();
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    Ctx c{.app = app,
          .s = s,
          .colors = theme_colors(),
          .now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()};
    draw_toolbar(c, px);
    if (s.clears != app.trace.clears)
    {
        // The toolbar's Clear is the only trace_clear that runs mid-draw (menu, Esc and control
        // bar run before the windows): drop the rows it just invalidated before drawing them.
        trace_window_update(s, app);
    }
    if (ImGui::BeginTabBar("##trace_tabs"))
    {
        for (int i = 0; i < static_cast<int>(TraceTab::Count); ++i)
        {
            if (!ImGui::BeginTabItem(tab_labels[i], nullptr, i == s.tab_goto ? ImGuiTabItemFlags_SetSelected : 0))
            {
                continue;
            }
            if (s.tab != static_cast<TraceTab>(i))
            {
                s.tab = static_cast<TraceTab>(i);
                s.scroll_pending = true;
                if (s.tab_goto != i || !s.nav_scroll)
                {
                    s.selected = -1; // a goto (Value Search hit) selected the row it lands on: kept
                }
                s.tab_goto = -1;
            }
            const bool rolling = s.modes[i] == TraceViewMode::Rolling;
            ImGui::PushID(i);
            switch (s.tab)
            {
            case TraceTab::Monitor:
                if (rolling && !app.trace.file.empty())
                {
                    draw_file_rolling(c, px);
                }
                else
                {
                    rolling ? draw_monitor_rolling(c, px) : draw_monitor_aggregated(c, px);
                }
                break;
            case TraceTab::Uds: rolling ? draw_proto_rolling(c, s.uds, px) : draw_proto_aggregated(c, s.uds, px); break;
            default: rolling ? draw_proto_rolling(c, s.j1939, px) : draw_proto_aggregated(c, s.j1939, px); break;
            }
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    vim_yank_menu(s.vim, s.yank_items);
    s.scroll_pending = false;
    ImGui::End();
}

namespace
{

constexpr const char* view_attrs[] = {"monitor-view", "uds-view", "j1939-view"};

} // namespace

void trace_window_save_xml(const TraceWindowState& s, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    for (int i = 0; i < static_cast<int>(TraceTab::Count); ++i)
    {
        el.append_attribute(view_attrs[i]) = s.modes[i] == TraceViewMode::Rolling ? "log" : "monitor";
    }
    el.append_attribute("timestamps") = static_cast<int>(s.ts_mode);
    el.append_attribute("decimal") = s.decimal;
    el.append_attribute("filter") = s.filter.text.c_str();
    el.append_attribute("show-tx") = s.filter.show_tx;
    el.append_attribute("show-rx") = s.filter.show_rx;
    for (const uint16_t i : s.filter.hidden_ifaces)
    {
        if (i < ifaces.size() && ifaces[i].ops != nullptr)
        {
            pugi::xml_node h = el.append_child("hidden-interface");
            h.append_attribute("driver") = ifaces[i].ops->name;
            h.append_attribute("interface") = ifaces[i].info.name.c_str();
        }
    }
}

void trace_window_load_xml(TraceWindowState& s, const std::deque<Iface>& ifaces, pugi::xml_node el)
{
    for (int i = 0; i < static_cast<int>(TraceTab::Count); ++i)
    {
        if (const pugi::xml_attribute a = el.attribute(view_attrs[i]); a)
        {
            s.modes[i] = std::string_view(a.as_string()) == "log" ? TraceViewMode::Rolling : TraceViewMode::Aggregated;
        }
    }
    const int ts = el.attribute("timestamps").as_int(static_cast<int>(s.ts_mode));
    if (ts >= 0 && ts <= static_cast<int>(TimestampMode::AbsoluteUtc))
    {
        s.ts_mode = static_cast<TimestampMode>(ts);
    }
    s.decimal = el.attribute("decimal").as_bool(s.decimal);
    s.filter.text = el.attribute("filter").as_string();
    s.filter_edit = s.filter.text;
    s.filter.show_tx = el.attribute("show-tx").as_bool(true);
    s.filter.show_rx = el.attribute("show-rx").as_bool(true);
    s.filter.hidden_ifaces.clear();
    for (const pugi::xml_node h : el.children("hidden-interface"))
    {
        if (const int i = ifaces_find(ifaces, h.attribute("driver").as_string(), h.attribute("interface").as_string()); i >= 0)
        {
            s.filter.hidden_ifaces.insert(static_cast<uint16_t>(i));
        }
    }
    s.filter_dirty = true;
}
