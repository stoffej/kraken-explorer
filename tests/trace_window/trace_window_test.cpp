#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <initializer_list>
#include <string>
#include <tuple>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, HoveredIdPreviousFrame
#include <pugixml.hpp>

#include "app.h"
#include "db/dbc/dbc_parser.h"
#include "db/model/can_db.h"
#include "drivers/driver.h"
#include "ui_test.h"
#include "ui/trace_window.h"
#include "ui/workspace_tabs.h"

static BusMessage frame(uint32_t id, std::initializer_list<uint8_t> bytes, uint16_t flags = 0, int64_t ts = 1)
{
    BusMessage m{.id = id, .flags = flags, .ts_ns = ts};
    for (uint8_t b : bytes)
    {
        m.data[m.len++] = b;
    }
    return m;
}

TEST_CASE("changed mask marks differing and new bytes, never error frames")
{
    const BusMessage a = frame(0x100, {1, 2, 3});
    CHECK(trace_changed_mask(frame(0x100, {1, 9, 3, 4}), a) == 0b1010);
    CHECK(trace_changed_mask(a, a) == 0);
    CHECK(trace_changed_mask(a, BusMessage{}) == 0); // no previous frame
    BusMessage err = frame(0, {1});
    err.errors = bus_error::ack;
    CHECK(trace_changed_mask(err, a) == 0);
}

TEST_CASE("data and id columns render in hex or decimal")
{
    const BusMessage m = frame(0x123, {0x12, 0xAB, 0x00, 0xFF, 0x07});
    std::string out;
    trace_append_data(out, m, false);
    CHECK(out == "12 AB 00 FF 07");
    out.clear();
    trace_append_data(out, m, true);
    CHECK(out == "18 171 0 255 7");
    out.clear();
    trace_append_id(out, m, false);
    CHECK(out == "0x123");
    out.clear();
    trace_append_id(out, m, true);
    CHECK(out == "291");

    // A remote frame: DLC 3 requested, no payload (candump 124#R3). Nothing in the Data column.
    BusMessage rtr = frame(0x124, {}, bus_flag::rtr);
    set_length(rtr, 3);
    for (const bool dec : {false, true})
    {
        out.clear();
        trace_append_data(out, rtr, dec);
        CHECK(out.empty());
    }

    const BusMessage ext = frame(0x18FEF100, {}, bus_flag::extended);
    out.clear();
    trace_append_id(out, ext, true);
    CHECK(out == "419361024");
    out.clear();
    trace_append_data(out, ext, true);
    CHECK(out.empty());

    BusMessage err = frame(0, {});
    err.errors = bus_error::ack;
    out.clear();
    trace_append_id(out, err, true);
    trace_append_data(out, err, true);
    CHECK(out == "-ERROR: ACK");
}

TEST_CASE("update aggregates per id, filters and follows a clear")
{
    App app;
    TraceWindowState s;
    const BusMessage msgs[] = {frame(0x100, {1}, 0, 10), frame(0x200, {2}, 0, 20), frame(0x100, {3}, 0, 30),
                               frame(0x100, {3}, bus_flag::tx, 40)};
    trace_append(app.trace, msgs);
    trace_window_update(s, app);
    CHECK(s.rolling.size() == 4);
    REQUIRE(s.agg.size() == 3); // TX 0x100 is its own row
    CHECK(s.agg[0].last.data[0] == 3);
    CHECK(s.agg[0].prev.data[0] == 1);
    CHECK(s.rolling[2].prev == 0); // previous 0x100 RX frame is trace index 0
    CHECK(s.rolling[3].prev == UINT64_MAX);

    s.filter.show_tx = false;
    s.filter.hidden_ids.insert(0x200);
    s.filter_dirty = true;
    trace_window_update(s, app);
    CHECK(s.rolling.size() == 2);

    trace_clear(app.trace);
    trace_window_update(s, app);
    CHECK(s.rolling.empty());
    CHECK(s.agg.empty());
}

TEST_CASE("aggregated rows keep cycle time min, max, mean and median")
{
    App app;
    TraceWindowState s;
    // Cycles 10, 20, 30, 40 ms: min 10, max 40, mean 100/4 = 25, median (20+30)/2 = 25.
    constexpr int64_t ms = 1'000'000;
    const BusMessage msgs[] = {frame(0x123, {1}, 0, 1000 * ms), frame(0x123, {1}, 0, 1010 * ms),
                               frame(0x123, {1}, 0, 1030 * ms), frame(0x123, {1}, 0, 1060 * ms),
                               frame(0x123, {1}, 0, 1100 * ms)};
    trace_append(app.trace, msgs);
    trace_window_update(s, app);
    REQUIRE(s.agg.size() == 1);
    const CycleStats& c = s.agg[0].cycle;
    CHECK(c.count == 4);
    CHECK(c.min_ns == 10 * ms);
    CHECK(c.max_ns == 40 * ms);
    CHECK(static_cast<double>(c.sum_ns) / static_cast<double>(c.count) == doctest::Approx(25.0 * ms));
    CHECK(cycle_stats_median(c) == doctest::Approx(25.0 * ms));

    // The median only sees the newest 256 cycles: 256 more of 5 ms push the old ones out.
    CycleStats w = c;
    for (int i = 0; i < 256; ++i)
    {
        cycle_stats_add(w, 5 * ms);
    }
    CHECK(cycle_stats_median(w) == doctest::Approx(5.0 * ms));
    CHECK(w.max_ns == 40 * ms);

    trace_clear(app.trace);
    trace_window_update(s, app);
    CHECK(s.agg.empty());
}

TEST_CASE("filter popup lists setup interfaces and interfaces with frames only")
{
    static const DriverOps fake_ops{.name = "Fake"};
    App app;
    for (const char* name : {"vcan0", "vcan1", "vcan2", "can0"})
    {
        Iface& f = app.ifaces.emplace_back();
        f.ops = &fake_ops;
        f.info.name = name;
        f.index = static_cast<uint16_t>(app.ifaces.size() - 1);
    }
    app.setup.networks.push_back({.name = "n", .interfaces = {{.driver = "Fake", .name = "vcan1"},
                                                              {.driver = "Other", .name = "vcan2"},
                                                              {.driver = "Fake", .name = "can0", .enabled = false}}});
    TraceWindowState s;
    CHECK(trace_filter_ifaces(s, app) == std::vector<uint16_t>{1});

    BusMessage m = frame(0x100, {1});
    m.iface = 3; // not enabled in the setup, but it has frames
    trace_append(app.trace, {&m, 1});
    trace_window_update(s, app);
    CHECK(trace_filter_ifaces(s, app) == std::vector<uint16_t>{1, 3});
}

TEST_CASE("refilter runs in slices and ends with the same rows as a full scan")
{
    App app;
    app.trace.max_size = 3'000'000;
    constexpr uint32_t total = 2'000'000;
    std::vector<BusMessage> msgs;
    msgs.reserve(total);
    for (uint32_t i = 0; i < total; ++i)
    {
        msgs.push_back(frame(0x100 + i % 8, {static_cast<uint8_t>(i)}, i % 5 == 0 ? bus_flag::tx : uint16_t{0},
                             static_cast<int64_t>(i) + 1));
    }
    trace_append(app.trace, msgs);

    TraceWindowState s;
    while (s.processed < app.trace.end)
    {
        trace_window_update(s, app);
    }
    REQUIRE(s.rolling.size() == total);

    s.filter.show_tx = false;
    s.filter.hidden_ids.insert(0x103);
    s.filter_dirty = true;
    trace_window_update(s, app);
    CHECK(s.refilter == 500'000); // one slice per update
    CHECK(s.rolling.size() < total / 4);

    // Frames arriving meanwhile wait until the refilter has caught up.
    const BusMessage late[] = {frame(0x101, {1}, 0, total + 1), frame(0x103, {1}, 0, total + 2)};
    trace_append(app.trace, late);
    int updates = 1;
    while (s.refilter != UINT64_MAX || s.processed < app.trace.end)
    {
        trace_window_update(s, app);
        ++updates;
    }
    CHECK(updates >= 4);

    TraceWindowState full;
    full.filter = s.filter;
    while (full.processed < app.trace.end)
    {
        trace_window_update(full, app);
    }
    REQUIRE(s.rolling.size() == full.rolling.size());
    bool same = true;
    for (std::size_t i = 0; i < s.rolling.size(); ++i)
    {
        same = same && s.rolling[i].index == full.rolling[i].index && s.rolling[i].prev == full.rolling[i].prev;
    }
    CHECK(same);
}

static void draw_frame(App& app, TraceWindowState& s, const WorkspaceTab& tab, float width = 1000.0f,
                       bool focus = false)
{
    ImGui::NewFrame();
    if (focus)
    {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width, 600});
    draw_trace_window(app, s, tab);
    ImGui::EndFrame();
    app.menu.pending.reset();
}

TEST_CASE("toolbar Clear in the rolling log drops the rows in the same frame")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    {
        App app;
        TraceWindowState s;
        s.modes[static_cast<int>(TraceTab::Monitor)] = TraceViewMode::Rolling;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        for (int i = 0; i < 100; ++i)
        {
            const BusMessage m = frame(0x100 + static_cast<uint32_t>(i % 4), {static_cast<uint8_t>(i)}, 0, i + 1);
            trace_append(app.trace, {&m, 1});
        }
        draw_frame(app, s, tab);
        draw_frame(app, s, tab);
        REQUIRE(s.rolling.size() == 100);

        // Hover along the toolbar until the Clear button is hot, then click it (press on release).
        ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(tab, "Trace").c_str());
        REQUIRE(w != nullptr);
        const ImGuiID clear = ImHashStr("##icon_text", 0, w->GetID("Clear"));
        bool clicked = false;
        for (float y = w->Pos.y + 2.0f; !clicked && y < w->Pos.y + ImGui::GetFrameHeight() * 4.0f; y += 6.0f)
        {
            for (float x = w->Pos.x + 2.0f; !clicked && x < w->Pos.x + w->Size.x; x += 6.0f)
            {
                io.AddMousePosEvent(x, y);
                draw_frame(app, s, tab);
                if (ImGui::GetCurrentContext()->HoveredIdPreviousFrame == clear)
                {
                    io.AddMouseButtonEvent(0, true);
                    draw_frame(app, s, tab);
                    io.AddMouseButtonEvent(0, false);
                    draw_frame(app, s, tab); // trace_clear runs here, mid-draw; used to SIGSEGV in trace_at
                    clicked = true;
                }
            }
        }
        REQUIRE(clicked);
        CHECK(trace_size(app.trace) == 0);
        CHECK(s.rolling.empty()); // already in the clicking frame, not only on the next update

        draw_frame(app, s, tab);
        CHECK(s.rolling.empty());

        // New traffic after the clear: fresh rows, Index restarts at 1.
        const BusMessage m = frame(0x123, {1}, 0, 1000);
        trace_append(app.trace, {&m, 1});
        draw_frame(app, s, tab);
        REQUIRE(s.rolling.size() == 1);
        CHECK(s.rolling[0].index == 100);
        CHECK(s.index_base == 100);
    }
}

// The aggregated table (the only one with the cycle columns) as ImGui laid it out last frame.
static const ImGuiTable* agg_table()
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.Tables.GetMapSize(); ++i)
    {
        if (const ImGuiTable* t = g.Tables.TryGetMapData(i); t != nullptr && t->ColumnsCount == 15)
        {
            return t;
        }
    }
    return nullptr;
}

TEST_CASE("monitor default columns fit a narrow dock, ScrollX only below the minimum widths")
{
    const UiTest ui({2000, 800});
    {
        App app;
        TraceWindowState s;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        const BusMessage m = frame(0x123, {1, 2, 3, 4, 5, 6, 7, 8}, 0, 1);
        trace_append(app.trace, {&m, 1});

        // Default-visible: Index, Time, ID, Name, DLC, Data, Cycle (mean).
        constexpr bool shown[15] = {true, true, false, false, false, true, false, true,
                                    true, true, false, false, false, true, false};
        struct Layout
        {
            float total, outer;
            bool scroll, data_widest;
            float data;
        };
        const auto layout = [&](float width)
        {
            for (int i = 0; i < 3; ++i)
            {
                draw_frame(app, s, tab, width);
            }
            const ImGuiTable* t = agg_table();
            REQUIRE(t != nullptr);
            Layout l{0.0f, t->OuterRect.GetWidth(), t->InnerWindow->ScrollbarX, true, t->Columns[9].WidthGiven};
            for (int c = 0; c < t->ColumnsCount; ++c)
            {
                CHECK(t->Columns[c].IsEnabled == shown[c]);
                if (t->Columns[c].IsEnabled)
                {
                    l.total += t->Columns[c].WidthGiven;
                    l.data_widest = l.data_widest && (c == 9 || t->Columns[c].WidthGiven < l.data);
                }
            }
            return l;
        };

        const Layout wide = layout(1600.0f);
        CHECK_FALSE(wide.scroll);
        CHECK(wide.total <= wide.outer + 1.0f);
        CHECK(wide.total > wide.outer - 7 * 2 * ImGui::GetStyle().CellPadding.x - 20.0f); // fills the width
        CHECK(wide.data_widest);

        // The default Trace dock: everything fits, Data still gets the most room.
        const Layout dock = layout(600.0f);
        CHECK_FALSE(dock.scroll);
        CHECK(dock.total <= dock.outer + 1.0f);
        CHECK(dock.data_widest);

        // 300 px is below the sum of minimum widths: the text columns shrink and ScrollX takes over.
        const Layout narrow = layout(300.0f);
        CHECK(narrow.scroll);
        CHECK(narrow.total > narrow.outer);
        CHECK(narrow.data < dock.data);

        // Back to wide: the scrollbar goes away again.
        CHECK_FALSE(layout(1600.0f).scroll);
    }
}

TEST_CASE("signal rows print physical values without float noise")
{
    const auto text = [](const CanDbSignal& sig, uint64_t raw)
    {
        std::string out;
        trace_append_signal_value(out, sig, raw);
        return out;
    };
    CHECK(text({.length = 8, .is_unsigned = true, .factor = 0.1}, 3) == "0.3");
    CHECK(text({.length = 32, .is_unsigned = true}, 1234567) == "1234567");
    CHECK(text({.length = 8}, 0xFF) == "-1");
    CHECK(text({.length = 8, .is_unsigned = true, .value_table = {{2, "On"}}}, 2) == "2 - On");
    const CanDbSignal f32{.length = 32, .value_type = SignalValueType::float32};
    CHECK(text(f32, 0x40490FDB) == "3.1415927");
    CHECK(text(f32, 0xC0000000) == "-2");
    CanDbSignal volts = f32;
    volts.unit = "V";
    CHECK(text(volts, 0x3DCCCCCD) == "0.1 V");
    const CanDbSignal f64{.length = 64, .value_type = SignalValueType::float64};
    CHECK(text(f64, 0x3FB999999999999A) == "0.1");
    CHECK(text(f64, 0x3E112E0BE826D695) == "1e-09");
    CanDbSignal speed = f64;
    speed.factor = 2.0;
    speed.unit = "km/h";
    CHECK(text(speed, 0x3FF8000000000000) == "3 km/h");
}

// What a click on a header does: set the aggregated table's sort column (the table of last frame).
static void sort_agg(int column, ImGuiSortDirection dir)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiTable* prev = g.CurrentTable;
    g.CurrentTable = const_cast<ImGuiTable*>(agg_table());
    REQUIRE(g.CurrentTable != nullptr);
    ImGui::TableSetColumnSortDirection(column, dir, false);
    g.CurrentTable = prev;
}

TEST_CASE("aggregated order is re-sorted on spec or row set changes, a live key every 250 ms")
{
    const UiTest ui;
    {
        App app;
        TraceWindowState s;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        const BusMessage msgs[] = {frame(0x300, {1}, 0, 1), frame(0x100, {1}, 0, 2), frame(0x200, {1}, 0, 3)};
        trace_append(app.trace, msgs);
        draw_frame(app, s, tab);
        draw_frame(app, s, tab);
        CHECK(s.agg_order == std::vector<uint32_t>{1, 2, 0}); // default: ID ascending

        const uint64_t sorts = s.agg_sorts;
        for (int i = 0; i < 30; ++i)
        {
            draw_frame(app, s, tab);
        }
        CHECK(s.agg_sorts == sorts); // nothing changed, ID never changes: no sort

        const BusMessage added = frame(0x050, {1}, 0, 4);
        trace_append(app.trace, {&added, 1});
        draw_frame(app, s, tab);
        CHECK(s.agg_sorts == sorts + 1);
        CHECK(s.agg_order == std::vector<uint32_t>{3, 1, 2, 0});

        const BusMessage update = frame(0x300, {2}, 0, 5);
        trace_append(app.trace, {&update, 1});
        draw_frame(app, s, tab);
        CHECK(s.agg_sorts == sorts + 1); // a new value of an existing row, sorted on ID

        sort_agg(1, ImGuiSortDirection_Descending); // Time, newest first
        draw_frame(app, s, tab);
        CHECK(s.agg_sorts == sorts + 2);
        CHECK(s.agg_order == std::vector<uint32_t>{0, 3, 2, 1});

        const BusMessage newest = frame(0x100, {2}, 0, 6);
        trace_append(app.trace, {&newest, 1});
        draw_frame(app, s, tab);
        CHECK(s.agg_order == std::vector<uint32_t>{0, 3, 2, 1}); // within 250 ms: not yet
        for (int i = 0; i < 20; ++i) // 21 frames at 60 Hz = 350 ms
        {
            draw_frame(app, s, tab);
        }
        CHECK(s.agg_sorts == sorts + 3);
        CHECK(s.agg_order == std::vector<uint32_t>{1, 0, 3, 2});
    }
}

TEST_CASE("vim keys select aggregated rows, / focuses the filter, h/l switch tabs")
{
    const UiTest ui;
    {
        App app;
        TraceWindowState s;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        const BusMessage msgs[] = {frame(0x100, {1}, 0, 1), frame(0x200, {1}, 0, 2), frame(0x300, {1}, 0, 3)};
        trace_append(app.trace, msgs);
        draw_frame(app, s, tab, 1000.0f, true);
        draw_frame(app, s, tab);
        ImGuiIO& io = ImGui::GetIO();
        const auto keys = [&](const char* typed)
        {
            io.AddInputCharactersUTF8(typed);
            draw_frame(app, s, tab);
        };
        CHECK(s.selected == -1);
        keys("j");
        CHECK(s.selected == 0);
        keys("j");
        CHECK(s.selected == 1);
        keys("k");
        CHECK(s.selected == 0);
        keys("G");
        CHECK(s.selected == 2);
        keys("gg");
        CHECK(s.selected == 0);
        keys("kkk");
        CHECK(s.selected == 0); // clamped

        keys("l");
        draw_frame(app, s, tab);
        CHECK(s.tab == TraceTab::Uds);
        CHECK(s.selected == -1);
        keys("h");
        draw_frame(app, s, tab);
        draw_frame(app, s, tab); // an earlier tab: its SetSelected is only submitted next frame
        CHECK(s.tab == TraceTab::Monitor);

        ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(tab, "Trace").c_str());
        REQUIRE(w != nullptr);
        keys("/");
        draw_frame(app, s, tab);
        draw_frame(app, s, tab);
        CHECK(ImGui::GetCurrentContext()->ActiveId == w->GetID("##filter"));
        keys("j"); // typed into the filter now, not a motion
        CHECK(s.selected == -1);
        CHECK(s.filter_edit == "j");
    }
}

TEST_CASE("y copies the selected row's cells to the clipboard, tab-separated")
{
    const UiTest ui;
    {
        App app;
        TraceWindowState s;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        const BusMessage msgs[] = {frame(0x100, {1}, 0, 1), frame(0x200, {0xAB, 0xCD}, 0, 2)};
        trace_append(app.trace, msgs);
        draw_frame(app, s, tab, 1000.0f, true);
        draw_frame(app, s, tab);
        ImGuiIO& io = ImGui::GetIO();
        const auto keys = [&](const char* typed)
        {
            io.AddInputCharactersUTF8(typed);
            draw_frame(app, s, tab);
        };
        ImGui::SetClipboardText("");
        keys("y"); // nothing selected: nothing copied
        CHECK(std::string(ImGui::GetClipboardText()).empty());
        keys("jj");
        REQUIRE(s.selected == 1);
        keys("y");
        for (int i = 0; i < 3 && s.yank_pending; ++i)
        {
            draw_frame(app, s, tab); // the row is drawn (and captured) once it is in view
        }
        ImGui::GetIO().AddInputCharacter('y'); // the copy menu is open: y = the whole row
        draw_frame(app, s, tab);
        const std::string line = ImGui::GetClipboardText();
        CHECK(line.find("0x200") != std::string::npos);
        CHECK(line.find("AB CD") != std::string::npos);
        CHECK(line.find('\t') != std::string::npos);
        CHECK_FALSE(s.yank_pending);
    }
}

TEST_CASE("Filter text matches the ID as shown: decimal in Dec mode")
{
    App app;
    TraceWindowState s;
    const BusMessage msgs[] = {frame(0x555, {1}, 0, 10), frame(0x100, {2}, 0, 20)};
    trace_append(app.trace, msgs);
    s.filter.text = "1365"; // 0x555
    s.decimal = true;
    trace_window_update(s, app);
    REQUIRE(s.rolling.size() == 1);
    CHECK(s.rolling[0].index == 0);
    s.decimal = false;
    s.filter_dirty = true;
    while (s.refilter != UINT64_MAX || s.filter_dirty)
    {
        trace_window_update(s, app);
    }
    CHECK(s.rolling.empty());
    s.filter.text = "555";
    s.filter_dirty = true;
    while (s.refilter != UINT64_MAX || s.filter_dirty)
    {
        trace_window_update(s, app);
    }
    CHECK(s.rolling.size() == 1);
}

TEST_CASE("cycle statistics leave out the stopped time between two measurements")
{
    App app;
    TraceWindowState s;
    constexpr int64_t ms = 1'000'000;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    app.measuring = true;
    const BusMessage first[] = {frame(0x123, {1}, 0, now - 300 * ms), frame(0x123, {1}, 0, now - 200 * ms)};
    trace_append(app.trace, first);
    trace_window_update(s, app);
    app.measuring = false; // stopped ...
    trace_window_update(s, app);
    app.measuring = true; // ... and started again 10 s later
    const BusMessage second[] = {frame(0x123, {1}, 0, now + 10'000 * ms), frame(0x123, {1}, 0, now + 10'100 * ms)};
    trace_append(app.trace, second);
    trace_window_update(s, app);
    REQUIRE(s.agg.size() == 1);
    const CycleStats& c = s.agg[0].cycle;
    CHECK(c.count == 2); // 100 ms before the stop, 100 ms after the start
    CHECK(c.max_ns == 100 * ms);
}

namespace
{

// Clipboard text after 'y' on the row `downs` j presses down (vim selection), in the current view.
std::string yank_after(App& app, TraceWindowState& s, const WorkspaceTab& tab, int downs)
{
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < downs; ++i)
    {
        io.AddInputCharactersUTF8("j");
        draw_frame(app, s, tab);
    }
    ImGui::SetClipboardText("");
    io.AddInputCharactersUTF8("y");
    draw_frame(app, s, tab);
    for (int i = 0; i < 3 && s.yank_pending; ++i)
    {
        draw_frame(app, s, tab);
    }
    ImGui::GetIO().AddInputCharacter('y'); // the copy menu is open: y = the whole row
    draw_frame(app, s, tab);
    return ImGui::GetClipboardText();
}

} // namespace

TEST_CASE("y copies the selected row's own Index, not the next row's")
{
    const UiTest ui;
    for (const TraceViewMode mode : {TraceViewMode::Aggregated, TraceViewMode::Rolling})
    {
        CAPTURE(static_cast<int>(mode));
        App app;
        TraceWindowState s;
        s.modes[0] = mode;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        const BusMessage msgs[] = {frame(0x100, {1}, 0, 1), frame(0x200, {0xAB}, 0, 2), frame(0x300, {3}, 0, 3)};
        trace_append(app.trace, msgs);
        draw_frame(app, s, tab, 1000.0f, true);
        draw_frame(app, s, tab);
        const std::string line = yank_after(app, s, tab, 2); // row 2 of 3: 0x200
        CHECK(line.starts_with("2\t"));
        CHECK(line.find("0x200") != std::string::npos);
        CHECK(line.find("AB") != std::string::npos);
        CHECK_FALSE(line.ends_with("\t3"));
        CHECK(line.find('\n') == std::string::npos);
    }
}

TEST_CASE("y on a Monitor row with DBC signals keeps its Index; expanded signal rows are lines of their own")
{
    const UiTest ui;
    {
        App app;
        auto db = std::make_shared<CanDb>();
        REQUIRE(dbc_parse("VERSION \"\"\n\nBU_: ECU\n\nBO_ 256 Engine: 2 ECU\n"
                          " SG_ Speed : 0|8@1+ (1,0) [0|255] \"rpm\" ECU\n"
                          " SG_ Temp : 8|8@1+ (1,-40) [-40|215] \"degC\" ECU\n",
                          *db));
        app.setup.networks.push_back({.name = "Net", .can_dbs = {db}});
        setup_rebuild_cache(app.setup);
        TraceWindowState s;
        const WorkspaceTab tab{.title = "Trace", .uid = 1};
        const BusMessage msgs[] = {frame(0x100, {7, 60}, 0, 1), frame(0x200, {1}, 0, 2)};
        trace_append(app.trace, msgs);
        draw_frame(app, s, tab, 1000.0f, true);
        draw_frame(app, s, tab);
        std::string line = yank_after(app, s, tab, 1);
        CHECK(line.starts_with("1\t"));
        CHECK(line.find("Engine") != std::string::npos);
        CHECK_FALSE(line.ends_with("\t2"));

        // Expand row 0 (tree node id: table id -> PushID(agg index) -> the row's order as a
        // pointer, in the table's scroll window).
        const ImGuiTable* t = agg_table();
        REQUIRE(t != nullptr);
        const int i0 = 0;
        const ImGuiID row_seed = ImHashData(&i0, sizeof(int), t->ID);
        const void* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(s.agg[0].order));
        t->InnerWindow->StateStorage.SetInt(ImHashData(&ptr, sizeof(void*), row_seed), 1);
        draw_frame(app, s, tab);
        ImGui::SetClipboardText("");
        ImGui::GetIO().AddInputCharactersUTF8("y");
        draw_frame(app, s, tab);
        for (int i = 0; i < 3 && s.yank_pending; ++i)
        {
            draw_frame(app, s, tab);
        }
        ImGui::GetIO().AddInputCharacter('y'); // the copy menu is open: y = the whole row
        draw_frame(app, s, tab);
        line = ImGui::GetClipboardText();
        CHECK(line.starts_with("1\t"));
        CHECK(line.find("\nSpeed\t7 rpm") != std::string::npos);
        CHECK(line.find("\nTemp\t20 degC") != std::string::npos);
        CHECK(line.find("0x200") == std::string::npos);
    }
}

TEST_CASE("trace window settings round-trip through the workspace XML")
{
    std::deque<Iface> ifaces;
    static const DriverOps fake = {.name = "Fake"};
    for (const char* name : {"fake0", "fake1"})
    {
        Iface& i = ifaces.emplace_back();
        i.ops = &fake;
        i.info.name = name;
    }
    TraceWindowState a;
    a.modes[0] = TraceViewMode::Rolling;
    a.modes[2] = TraceViewMode::Rolling;
    a.ts_mode = TimestampMode::AbsoluteUtc;
    a.decimal = true;
    a.filter.text = "Engine";
    a.filter.show_tx = false;
    a.filter.hidden_ifaces = {1};
    pugi::xml_document doc;
    trace_window_save_xml(a, ifaces, doc.append_child("tracewindow"));
    const pugi::xml_node el = doc.child("tracewindow");
    CHECK(std::string(el.attribute("monitor-view").as_string()) == "log");
    CHECK(std::string(el.child("hidden-interface").attribute("interface").as_string()) == "fake1");

    TraceWindowState b;
    trace_window_load_xml(b, ifaces, el);
    CHECK(b.modes[0] == TraceViewMode::Rolling);
    CHECK(b.modes[1] == TraceViewMode::Rolling); // the UDS default, saved as "log"
    CHECK(b.modes[2] == TraceViewMode::Rolling);
    CHECK(b.ts_mode == TimestampMode::AbsoluteUtc);
    CHECK(b.decimal);
    CHECK(b.filter.text == "Engine");
    CHECK(b.filter_edit == "Engine");
    CHECK_FALSE(b.filter.show_tx);
    CHECK(b.filter.show_rx);
    CHECK(b.filter.hidden_ifaces == std::set<uint16_t>{1});
}
