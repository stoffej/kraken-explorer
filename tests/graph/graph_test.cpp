#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

#include "ui/graph.h"

namespace
{

GraphSignal sig(const char* unit, int axis = -1, double min = 0.0, double max = 0.0)
{
    GraphSignal s;
    s.unit = unit;
    s.axis = axis;
    s.min = min;
    s.max = max;
    return s;
}

} // namespace

TEST_CASE("slots: one per unit, manual axis wins and is skipped by automatic ones")
{
    const std::vector<GraphSignal> s{sig("V"), sig("A"), sig("V"), sig("rpm", 0), sig("%"), sig("C"), sig("bar")};
    // slot 0 is taken by the manual rpm -> V=1, A=2; Y1..Y3 are then full and no range is known,
    // so %, C and bar join the first slot. Manual slots beyond Y3 still make a second plot.
    CHECK(graph_assign_slots(s) == std::vector<int>{1, 2, 1, 0, 0, 0, 0});
    CHECK(graph_assign_slots(std::vector<GraphSignal>{sig("V"), sig("A", 4)}) == std::vector<int>{0, 4});
}

TEST_CASE("slots: automatic axis by unit and range, at most Y1..Y3, then nearest range")
{
    // rpm 0..8000, degC -40..150, bar 0..10: three units -> three axes; the second degC shares.
    CHECK(graph_assign_slots(std::vector<GraphSignal>{sig("rpm", -1, 0, 8000), sig("degC", -1, -40, 150), sig("bar", -1, 0, 10),
                                                      sig("degC", -1, 0, 120)})
          == std::vector<int>{0, 1, 2, 1});
    // Same unit, ranges 100x apart: own axis.
    CHECK(graph_assign_slots(std::vector<GraphSignal>{sig("V", -1, 0, 5), sig("V", -1, 0, 500)}) == std::vector<int>{0, 1});
    // Same unit within 10x: shared.
    CHECK(graph_assign_slots(std::vector<GraphSignal>{sig("V", -1, 0, 5), sig("V", -1, 0, 40)}) == std::vector<int>{0, 0});
    // Full: a 4th unit (0..9000) goes to the rpm axis (0..8000), the nearest range.
    CHECK(graph_assign_slots(std::vector<GraphSignal>{sig("rpm", -1, 0, 8000), sig("degC", -1, -40, 150), sig("bar", -1, 0, 10),
                                                      sig("m", -1, 0, 9000)})
          == std::vector<int>{0, 1, 2, 0});
}

TEST_CASE("cursors: sample-and-hold value at A and B, B - A, delta-t text")
{
    GraphSignal s;
    s.t = {1.0, 2.0, 3.0};
    s.v = {10.0, 20.0, 35.0};
    CHECK(std::isnan(graph_value_at(s.t, s.v, 0.5))); // before the first sample: none
    CHECK(graph_value_at(s.t, s.v, 1.0) == 10.0);      // exactly on a sample
    CHECK(graph_value_at(s.t, s.v, 2.9) == 20.0);      // held until the next one
    CHECK(graph_value_at(s.t, s.v, 99.0) == 35.0);
    CHECK(std::isnan(graph_value_at({}, {}, 1.0)));
    CHECK(graph_cursor_values(s, 1.5, 3.0) == std::array<double, 3>{10.0, 35.0, 25.0});
    CHECK(graph_cursor_values(s, 3.0, 1.5) == std::array<double, 3>{35.0, 10.0, -25.0});
    // Cached until a cursor moves or samples are added.
    s.v[2] = 99.0;
    CHECK(graph_cursor_values(s, 3.0, 1.5)[0] == 35.0);
    s.t.push_back(4.0);
    s.v.push_back(40.0);
    CHECK(graph_cursor_values(s, 3.0, 1.5)[0] == 99.0);
    const auto none = graph_cursor_values(s, 0.0, 2.0);
    CHECK(std::isnan(none[0]));
    CHECK(none[1] == 20.0);
    CHECK(std::isnan(none[2]));

    CHECK(graph_delta_t_text(1.0, 1.0125) == "\u0394t = 12.5 ms  (1/\u0394t = 80 Hz)");
    CHECK(graph_delta_t_text(2.0, 1.5) == "\u0394t = -500 ms  (1/\u0394t = 2 Hz)");
    CHECK(graph_delta_t_text(3.0, 3.0) == "\u0394t = 0 ms  (1/\u0394t = - Hz)");
}

TEST_CASE("decimate: few points pass through with one neighbour each side")
{
    const std::vector<double> t{0, 1, 2, 3, 4, 5};
    const std::vector<double> v{0, 10, 20, 30, 40, 50};
    std::vector<double> ot, ov;
    graph_decimate(t, v, 2.0, 3.0, 100, ot, ov);
    CHECK(ot == std::vector<double>{1, 2, 3, 4});
    CHECK(ov == std::vector<double>{10, 20, 30, 40});
}

TEST_CASE("decimate: many points keep min and max per bucket in time order")
{
    std::vector<double> t, v;
    for (int i = 0; i < 1000; ++i)
    {
        t.push_back(i * 0.01);         // 0 .. 9.99
        v.push_back(i == 123 ? 99.0 : i == 456 ? -99.0 : 0.0);
    }
    std::vector<double> ot, ov;
    graph_decimate(t, v, 0.0, 10.0, 10, ot, ov);
    CHECK(ot.size() <= 2 * 12);
    CHECK(std::is_sorted(ot.begin(), ot.end()));
    CHECK(std::count(ov.begin(), ov.end(), 99.0) == 1);
    CHECK(std::count(ov.begin(), ov.end(), -99.0) == 1);
}

TEST_CASE("lod: a window at every level keeps a spike's min and max; the level obeys 4 x pixels")
{
    // 64 * 64 * 8 samples at 1 ms with one spike up and one down; top = 4 gives three levels.
    const std::size_t n = graph_lod_fan * graph_lod_fan * 8;
    std::vector<double> t(n), v(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        t[i] = static_cast<double>(i) * 1e-3;
        v[i] = std::sin(static_cast<double>(i) * 0.01);
    }
    v[12345] = 50.0;
    v[23456] = -50.0;
    const GraphLod lod = graph_lod_build(t, v, 4);
    REQUIRE(lod.t.size() == 3);
    CHECK(lod.t[1].size() == 2 * n / graph_lod_fan);             // 512 buckets, two points each
    CHECK(lod.t[2].size() == 2 * lod.t[1].size() / graph_lod_fan); // 16 buckets
    std::vector<double> ot, ov;
    for (std::size_t level = 0; level < lod.t.size(); ++level)
    {
        graph_lod_window(lod, level, 10.0, 30.0, ot, ov); // both spikes (12.345 s, 23.456 s) inside
        CHECK(std::is_sorted(ot.begin(), ot.end()));
        CHECK(ot.front() <= 10.0);
        CHECK(ot.back() >= 30.0);
        CHECK(*std::max_element(ov.begin(), ov.end()) == 50.0);
        CHECK(*std::min_element(ov.begin(), ov.end()) == -50.0);
        graph_lod_window(lod, level, 13.0, 20.0, ot, ov); // neither (but the neighbour before 13 s may be the 12.345 s spike)
        for (std::size_t i = 0; i < ot.size(); ++i)
        {
            CHECK((ot[i] < 13.0 || ot[i] > 20.0 || std::abs(ov[i]) < 50.0));
        }
        CHECK(ot.front() <= 13.0);
        CHECK(ot.back() >= 20.0);
    }
    // Level 2 has 16 buckets over the whole series: enough for 4 pixels, not 5; level 1 has 512.
    CHECK(graph_lod_level(lod, 0.0, 40.0, 4) == 2);
    CHECK(graph_lod_level(lod, 0.0, 40.0, 5) == 1);
    CHECK(graph_lod_level(lod, 0.0, 40.0, 129) == 0);
    const auto buckets = [&](std::size_t level, double x0, double x1)
    {
        const std::vector<double>& lt = lod.t[level];
        const auto lo = std::lower_bound(lt.begin(), lt.end(), x0);
        const auto hi = std::upper_bound(lo, lt.end(), x1);
        return static_cast<std::size_t>(hi - lo) / (level == 0 ? 1 : 2);
    };
    for (const int px : {1, 2, 3, 4, 5, 50, 77, 78, 79, 100, 2000, 10000})
    {
        const std::size_t level = graph_lod_level(lod, 5.0, 25.0, px);
        CHECK((level == 0 || buckets(level, 5.0, 25.0) >= 4 * static_cast<std::size_t>(px)));
        CHECK((level + 1 == lod.t.size() || buckets(level + 1, 5.0, 25.0) < 4 * static_cast<std::size_t>(px)));
    }
    CHECK(graph_lod_build({}, {}).t.size() == 1); // an empty series is one empty level
}

TEST_CASE("xy pair: each Y takes the latest X at or before its time (sample-and-hold)")
{
    // X: 0,1,2,3 at 0,10,20,30 ms; Y: 10,11,12 at 5,15,25 ms -> (0,10), (1,11), (2,12) by hand
    const std::vector<double> xt{0.000, 0.010, 0.020, 0.030};
    const std::vector<double> xv{0, 1, 2, 3};
    const std::vector<double> yt{0.005, 0.015, 0.025};
    const std::vector<double> yv{10, 11, 12};
    std::vector<double> ox, oy;
    graph_xy_pair(xt, xv, yt, yv, 0.0, 1.0, ox, oy);
    CHECK(ox == std::vector<double>{0, 1, 2});
    CHECK(oy == std::vector<double>{10, 11, 12});

    // Y at exactly an X time pairs with that X; Y before the first X is dropped; window clips Y.
    const std::vector<double> yt2{-0.001, 0.010, 0.030, 0.040};
    const std::vector<double> yv2{9, 20, 30, 40};
    graph_xy_pair(xt, xv, yt2, yv2, -1.0, 0.035, ox, oy);
    CHECK(ox == std::vector<double>{1, 3});
    CHECK(oy == std::vector<double>{20, 30});
    graph_xy_pair(xt, xv, yt, yv, 0.012, 0.030, ox, oy); // window starts mid-series
    CHECK(ox == std::vector<double>{1, 2});
    CHECK(oy == std::vector<double>{11, 12});
}

// Headless frames of the real window: 4 units -> 2 subplots (linked X), Y1..Y3 on the first.
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include "app.h"
#include "ui/theme.h"
#include "ui/workspace_tabs.h"

TEST_CASE("graph window draws three Y axes plus a second subplot without asserting")
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1400, 900};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);

    App app;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        WorkspaceTab* tab = draw_workspace(app);
        draw_graph_windows(app, tab);
        ImGui::EndFrame();
        app.menu.pending.reset();
    };
    frame(); // creates the tab and its docked "Graph"
    REQUIRE(app.workspace.tabs.size() == 1);
    REQUIRE(app.workspace.tabs[0].graphs.size() == 1);
    GraphState& g = app.workspace.tabs[0].graphs[0];
    for (const char* unit : {"V", "A", "rpm", "%", "V"})
    {
        GraphSignal s = sig(unit, std::string_view(unit) == "%" ? 3 : -1); // % on Y1 of a second plot
        s.name = std::string("sig_") + unit + std::to_string(g.signals.size());
        s.kind = GraphSignalKind::BusLoad; // no DBC needed: ingest only matches iface -1
        s.iface = -1;
        for (int i = 0; i < 5000; ++i)
        {
            s.t.push_back(i * 0.01);
            s.v.push_back(std::sin(i * 0.05) * (1.0 + static_cast<double>(g.signals.size())));
        }
        g.signals.push_back(std::move(s));
    }
    g.cursor_on = true;
    g.cursor_a = 25.0;
    g.cursor_b = 30.0;
    g.statistics = true;
    g.downsample = 2;
    for (int i = 0; i < 6; ++i)
    {
        frame();
    }
    CHECK(graph_assign_slots(g.signals) == std::vector<int>{0, 1, 2, 3, 0});
    const ImPlotContext* ctx = ImPlot::GetCurrentContext();
    CHECK(ctx->Subplots.GetBufSize() == 1);
    CHECK(ctx->Plots.GetBufSize() >= 2);
    CHECK(ImGui::FindWindowByName(workspace_window_name(app.workspace.tabs[0], "Graph").c_str()) != nullptr);
    for (GraphView view : {GraphView::XY, GraphView::Text, GraphView::Gauge})
    {
        g.view = view;
        frame();
    }
    // PNG export: every view and style draws a 4K window off screen; its draw lists (the plot is a
    // child window) go to main.cpp, the Y ranges are the ones on screen.
    g.view = GraphView::TimeSeries;
    frame();
    REQUIRE(g.y_shown.size() >= 4);
    CHECK(g.y_shown[0][1] > g.y_shown[0][0]);
    g.png.size = 4; // 3840 x 2160
    for (GraphView view : {GraphView::TimeSeries, GraphView::XY, GraphView::Text, GraphView::Gauge})
    {
        for (GraphPngStyle style : {GraphPngStyle::Kraken, GraphPngStyle::Light, GraphPngStyle::Print, GraphPngStyle::Transparent})
        {
            INFO(static_cast<int>(view), " ", static_cast<int>(style));
            g.view = view;
            g.png.style = style;
            g.png_path = "/tmp/x.png";
            frame();
            REQUIRE(app.png_export.has_value());
            CHECK(app.png_export->w == 3840);
            CHECK(app.png_export->h == 2160);
            CHECK(app.png_export->transparent == (style == GraphPngStyle::Transparent));
            CHECK(app.png_export->lists.size() >= 2); // the window and the plot's child
            int vertices = 0;
            for (const ImDrawList* l : app.png_export->lists)
            {
                vertices += l->VtxBuffer.Size;
            }
            CHECK(vertices > (view == GraphView::Text ? 200 : 1000)); // Text: value cards only
            CHECK(g.png_path.empty());
            app.png_export.reset();
        }
    }
    CHECK(graph_png_scale(g.png, 2160) == doctest::Approx(2.4f));
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

TEST_CASE("statistics: window min/max/mean/median, cached until range or data change, max every 250 ms")
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1400, 900};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);

    App app;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        WorkspaceTab* tab = draw_workspace(app);
        draw_graph_windows(app, tab);
        ImGui::EndFrame();
        app.menu.pending.reset();
    };
    frame();
    REQUIRE(app.workspace.tabs.size() == 1);
    GraphState& g = app.workspace.tabs[0].graphs[0];
    GraphSignal s = sig("V");
    s.kind = GraphSignalKind::BusLoad;
    s.name = "stat";
    s.t = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    s.v = {5, 1, 4, 2, 3, 10, 20, 30, 40, 50};
    g.signals.push_back(std::move(s));
    g.statistics = true;
    g.follow = false;
    g.x_min = 0.0;
    g.x_max = 4.0;
    frame();
    const GraphSignal& r = g.signals[0];
    REQUIRE(g.x_max == 4.0);
    CHECK(std::array{r.stats[0], r.stats[1], r.stats[2], r.stats[3]} == std::array<double, 4>{1, 5, 3, 3}); // {5,1,4,2,3} by hand
    CHECK(r.stats[4] == doctest::Approx(1.4142136)); // std dev: sqrt((4 + 4 + 1 + 1 + 0) / 5)

    g.x_max = 9.0;
    frame(); // range changed, but the last recompute was < 250 ms ago: still the old window
    CHECK(std::array{r.stats[0], r.stats[1], r.stats[2], r.stats[3]} == std::array<double, 4>{1, 5, 3, 3});
    g.signals[0].stats_wall = -1e9; // as if 250 ms passed
    frame();
    CHECK(std::array{r.stats[0], r.stats[1], r.stats[2], r.stats[3]} == std::array<double, 4>{1, 50, 16.5, 7.5}); // mean 165/10, median (5+10)/2

    // Cursors on: the statistics cover A..B (either order), not the visible window.
    g.cursor_on = true;
    g.cursor_a = 9.0;
    g.cursor_b = 5.0;
    g.signals[0].stats_wall = -1e9;
    frame();
    CHECK(std::array{r.stats[0], r.stats[1], r.stats[2], r.stats[3]} == std::array<double, 4>{10, 50, 30, 30}); // {10,20,30,40,50}
    g.cursor_on = false;

    g.x_min = 20.0;
    g.x_max = 30.0;
    g.signals[0].stats_wall = -1e9;
    frame();
    CHECK(std::isnan(r.stats[0])); // empty window
    CHECK(std::isnan(r.stats[4]));
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

TEST_CASE("a drag-drop payload over a plot with unused Y2/Y3 does not assert")
{
    // Moving a floating window (a docking payload) over the plot asserted 'id != 0' in
    // BeginDragDropTargetCustom: the drop target was asked for Y2/Y3 that were never set up.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1400, 900};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);

    App app;
    bool drag = false;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
        {
            ImGui::SetDragDropPayload("TEST_PAYLOAD", nullptr, 0);
            ImGui::EndDragDropSource();
        }
        WorkspaceTab* tab = draw_workspace(app);
        draw_graph_windows(app, tab);
        ImGui::EndFrame();
        app.menu.pending.reset();
    };
    frame();
    REQUIRE(app.workspace.tabs.size() == 1);
    GraphState& g = app.workspace.tabs[0].graphs[0];
    GraphSignal s = sig("V");
    s.kind = GraphSignalKind::BusLoad;
    s.t = {0.0, 1.0};
    s.v = {0.0, 1.0};
    g.signals.push_back(std::move(s)); // Y1 only
    frame();
    const ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(app.workspace.tabs[0], "Graph").c_str());
    REQUIRE(w != nullptr);
    io.AddMousePosEvent(w->Pos.x + w->Size.x * 0.75f, w->Pos.y + w->Size.y * 0.5f); // over the plot
    drag = true;
    for (int i = 0; i < 4; ++i)
    {
        frame();
    }
    CHECK(ImGui::GetDragDropPayload() != nullptr);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

#include <deque>
#include <memory>
#include <string_view>

#include <pugixml.hpp>

#include "db/model/can_db.h"

namespace
{

const DriverOps fake_ops{.name = "Fake"};

// Net A: Engine (0x100) with Rpm + Temp, interface fake0 on the Fake driver at 250 kbit/s.
void make_setup(Setup& setup, std::deque<Iface>& ifaces, bool with_temp = true)
{
    auto db = std::make_shared<CanDb>();
    CanDbMessage& msg = db->messages[0x100];
    msg.name = "Engine";
    msg.raw_id = 0x100;
    msg.signals.push_back({.name = "Rpm", .length = 16, .unit = "rpm"});
    if (with_temp)
    {
        msg.signals.push_back({.name = "Temp", .start_bit = 16, .length = 8, .unit = "C"});
    }
    SetupNetwork net{.name = "Net A"};
    net.interfaces.push_back({.driver = "Fake", .name = "fake0", .bitrate = 250000});
    net.can_dbs.push_back(db);
    setup.networks.push_back(std::move(net));
    Iface& i = ifaces.emplace_back();
    i.ops = &fake_ops;
    i.info.name = "fake0";
}

GraphSignal can_sig_of(const Setup& setup, const char* name, uint32_t color)
{
    const CanDbMessage& msg = setup.networks[0].can_dbs[0]->messages.at(0x100);
    GraphSignal s;
    s.kind = GraphSignalKind::Can;
    s.network = "Net A";
    s.can_msg = &msg;
    s.can_sig = can_db_find_signal(msg, name);
    s.can_raw_id = 0x100;
    s.name = name;
    s.parent = "Engine";
    s.color = color;
    return s;
}

} // namespace

TEST_CASE("workspace XML: graph config round trip, missing signals dropped")
{
    Setup setup;
    std::deque<Iface> ifaces;
    make_setup(setup, ifaces);
    std::vector<GraphState> graphs(2);
    graphs[1].id = 3;
    graphs[1].standalone = true;
    GraphState& g = graphs[0];
    g.view = GraphView::XY;
    g.x_signal = 1;
    g.statistics = true;
    g.cursor_on = true;
    g.cursor_a = 1.25;
    g.cursor_b = -3.5e-4;
    g.duration = 4;
    g.downsample = 5;
    g.columns = 3;
    g.signals.push_back(can_sig_of(setup, "Rpm", 0xFF0000FF));
    g.signals.push_back(can_sig_of(setup, "Temp", 0xFF00FF00));
    g.signals.back().axis = 4;
    g.signals.back().hidden = true;
    GraphSignal load;
    load.kind = GraphSignalKind::BusLoad;
    load.iface = 0;
    load.name = "Bus Load - fake0";
    load.color = 0x80FFFFFF;
    g.signals.push_back(load);

    pugi::xml_document doc;
    graph_save_xml(graphs, ifaces, doc.append_child("tab"));
    std::vector<GraphState> back;
    graph_load_xml(back, setup, ifaces, doc.child("tab"));
    REQUIRE(back.size() == 2);
    CHECK(back[1].id == 3);
    CHECK(back[1].standalone);
    CHECK(back[1].view == GraphView::TimeSeries);
    CHECK(back[1].signals.empty());
    const GraphState& b = back[0];
    CHECK(b.id == 0);
    CHECK_FALSE(b.standalone);
    CHECK(b.view == GraphView::XY);
    CHECK(b.x_signal == 1);
    CHECK(b.statistics);
    CHECK(b.cursor_on);
    CHECK(b.cursor_a == 1.25);
    CHECK(b.cursor_b == -3.5e-4);
    CHECK(back[1].cursor_a == 0.0);
    CHECK(b.duration == 4);
    CHECK(b.downsample == 5);
    CHECK(b.columns == 3);
    REQUIRE(b.signals.size() == 3);
    const CanDbMessage& msg = setup.networks[0].can_dbs[0]->messages.at(0x100);
    CHECK(b.signals[0].kind == GraphSignalKind::Can);
    CHECK(b.signals[0].can_sig == &msg.signals[0]);
    CHECK(b.signals[0].can_msg == &msg);
    CHECK(b.signals[0].network == "Net A");
    CHECK(b.signals[0].unit == "rpm");
    CHECK(b.signals[0].color == 0xFF0000FF);
    CHECK(b.signals[0].axis == -1);
    CHECK_FALSE(b.signals[0].hidden);
    CHECK(b.signals[1].can_sig == &msg.signals[1]);
    CHECK(b.signals[1].axis == 4);
    CHECK(b.signals[1].hidden);
    CHECK(b.signals[2].kind == GraphSignalKind::BusLoad);
    CHECK(b.signals[2].iface == 0);
    CHECK(b.signals[2].bitrate == 250000);
    CHECK(b.signals[2].name == "Bus Load - fake0");
    CHECK(b.signals[2].color == 0x80FFFFFF);

    // Same file against a setup without Temp: Temp (the X signal) is dropped with a warning.
    Setup other;
    std::deque<Iface> other_ifaces;
    make_setup(other, other_ifaces, false);
    std::vector<GraphState> partial;
    graph_load_xml(partial, other, other_ifaces, doc.child("tab"));
    REQUIRE(partial.size() == 2);
    REQUIRE(partial[0].signals.size() == 2);
    CHECK(partial[0].signals[0].name == "Rpm");
    CHECK(partial[0].signals[1].kind == GraphSignalKind::BusLoad);
    CHECK(partial[0].x_signal == 1); // the dropped X (index 1) falls back onto the next signal

    // A tab without <graph> (older workspaces) adds nothing.
    std::vector<GraphState> none;
    graph_load_xml(none, setup, ifaces, doc.append_child("tab2"));
    CHECK(none.empty());
}

TEST_CASE("value text: float32 at float precision, integer signals at the decimals of their factor")
{
    CanDbSignal f32{.value_type = SignalValueType::float32};
    CanDbSignal f64{.value_type = SignalValueType::float64};
    CanDbSignal i32{.factor = 0.5};
    GraphSignal s;
    s.can_sig = &f32;
    CHECK(graph_format_value(s, static_cast<double>(0.1f)) == "0.1");
    CHECK(graph_format_value(s, static_cast<double>(123456.7f)) == "123456.7");
    CHECK(graph_format_value(s, static_cast<double>(1.5e-8f)) == "1.5e-08");
    s.can_sig = &f64;
    CHECK(graph_format_value(s, 0.1 + 0.2) == "0.3");
    CHECK(graph_format_value(s, 123456.789012) == "123456.789012");
    CHECK(graph_format_value(s, std::nan("")) == "-");
    s.can_sig = &i32;
    CHECK(graph_format_value(s, 1234.5) == "1234.5");
    // T87b F1: %.6g cut a 1e-07 latitude to 89.909 and 0.01 m depth to 11000
    CanDbSignal lat{.length = 32, .factor = 1e-07};
    s.can_sig = &lat;
    CHECK(graph_format_value(s, can_signal_raw_to_physical(lat, 899089582)) == "89.9089582");
    CanDbSignal depth{.length = 32, .is_unsigned = true, .factor = 0.01};
    s.can_sig = &depth;
    CHECK(graph_format_value(s, can_signal_raw_to_physical(depth, 1099999)) == "10999.99");
    // computed values (cursor B - A, mean) without float noise: "2.83", not "2.83000000000000"
    CHECK(graph_format_value(s, can_signal_raw_to_physical(depth, 1099999) - can_signal_raw_to_physical(depth, 1099716)) == "2.83");
    CHECK(graph_format_value(s, -0.001) == "0.00");
    CanDbSignal deg{.length = 16, .factor = 0.0057};
    s.can_sig = &deg;
    CHECK(graph_format_value(s, 5.1 - 0.0000000001) == "5.1000");
    s.kind = GraphSignalKind::BusLoad;
    s.can_sig = nullptr;
    CHECK(graph_format_value(s, 42.0) == "42");
}

TEST_CASE("signal list: j/k select a row, Space toggles its visibility")
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1400, 900};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);

    App app;
    ImGuiWindow* list = nullptr;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        if (list != nullptr)
        {
            ImGui::FocusWindow(list);
        }
        WorkspaceTab* tab = draw_workspace(app);
        draw_graph_windows(app, tab);
        ImGui::EndFrame();
        app.menu.pending.reset();
    };
    frame();
    REQUIRE(app.workspace.tabs.size() == 1);
    GraphState& g = app.workspace.tabs[0].graphs[0];
    for (const char* name : {"a", "b", "c"})
    {
        GraphSignal s = sig("V");
        s.kind = GraphSignalKind::BusLoad;
        s.name = name;
        g.signals.push_back(std::move(s));
    }
    frame();
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    {
        if (std::string_view(w->Name).find("/##list_") != std::string_view::npos && w->ParentWindow != nullptr
            && std::string_view(w->ParentWindow->Name).find("/##side_") != std::string_view::npos)
        {
            list = w;
        }
    }
    REQUIRE(list != nullptr);
    const auto type = [&](char c)
    {
        io.AddInputCharacter(static_cast<unsigned>(c));
        frame();
    };
    CHECK(g.selected == -1);
    type('j');
    CHECK(g.selected == 0);
    type('j');
    type('j');
    type('j'); // clamped at the last row
    CHECK(g.selected == 2);
    type('k');
    CHECK(g.selected == 1);
    io.AddKeyEvent(ImGuiKey_Space, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Space, false);
    frame();
    CHECK(g.signals[1].hidden);
    CHECK_FALSE(g.signals[0].hidden);
    CHECK_FALSE(g.signals[2].hidden);
    io.AddKeyEvent(ImGuiKey_Space, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Space, false);
    frame();
    CHECK_FALSE(g.signals[1].hidden);

    // Unfocused list: the keys belong to someone else.
    list = nullptr;
    ImGui::NewFrame();
    ImGui::FocusWindow(nullptr);
    ImGui::EndFrame();
    type('k');
    CHECK(g.selected == 1);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

TEST_CASE("remove keeps the XY X signal; a new signal gets an unused colour; Text view follows the window")
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    GraphState g;
    for (int k = 0; k < 4; ++k)
    {
        GraphSignal s = sig("V");
        s.name = std::string(1, static_cast<char>('a' + k));
        s.color = graph_next_color(g);
        g.signals.push_back(std::move(s));
    }
    g.x_signal = 2; // "c"
    graph_remove_signal(g, 0);
    REQUIRE(g.signals.size() == 3);
    CHECK(g.signals[static_cast<std::size_t>(g.x_signal)].name == "c");
    graph_remove_signal(g, 2); // after X: X stays
    CHECK(g.signals[static_cast<std::size_t>(g.x_signal)].name == "c");

    // "a" (colour 0) and "d" (colour 3) are gone: the next two take exactly those colours.
    const uint32_t c0 = ImGui::ColorConvertFloat4ToU32(ImPlot::GetColormapColor(0));
    CHECK(graph_next_color(g) == c0);
    for (const GraphSignal& s : g.signals)
    {
        CHECK(s.color != c0);
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    // Finding 2: the time window also advances in Text view (statistics use it).
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1400, 900};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);
    App app;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        WorkspaceTab* tab = draw_workspace(app);
        draw_graph_windows(app, tab);
        ImGui::EndFrame();
        app.menu.pending.reset();
    };
    frame();
    GraphState& main = app.workspace.tabs[0].graphs[0];
    main.view = GraphView::Text;
    main.follow = true;
    main.duration = 0; // All: [0, max(now, 1)]
    main.x_min = 5.0;
    main.x_max = 60.0;
    frame();
    CHECK(main.x_min == 0.0);
    CHECK(main.x_max == 1.0);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <unistd.h>

#include "db/dbc/dbc_parser.h"
#include "ui/frame_cache.h"

TEST_CASE("conditional logging: no live frames are taken while armed, the graph starts when triggered")
{
    App app;
    SetupNetwork& net = app.setup.networks.emplace_back();
    net.name = "net";
    net.can_dbs.push_back(std::make_shared<CanDb>());
    REQUIRE(dbc_parse("BO_ 273 Msg: 1 ECU\n SG_ Sig : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n", *net.can_dbs[0]));
    setup_rebuild_cache(app.setup);
    GraphState g;
    GraphSignal& s = g.signals.emplace_back();
    s.network = "net";
    s.name = "Sig";
    s.can_raw_id = 0x111;
    const auto push = [&](int64_t ns, uint8_t v)
    {
        BusMessage m{.id = 0x111, .ts_ns = ns};
        set_length(m, 1);
        m.data[0] = v;
        trace_append(app.trace, {&m, 1});
    };
    app.conditional_logging.config.enabled = true; // armed: no condition holds yet
    push(1'000'000, 1);
    graph_ingest(g, app);
    CHECK(s.t.empty());
    app.conditional_logging.condition_met = true;
    push(2'000'000, 2);
    graph_ingest(g, app);
    REQUIRE(s.v.size() == 1);
    CHECK(s.v[0] == 2.0);
}

TEST_CASE("file view: a signal's pyramid is built off the main thread; a zoomed-out window keeps the spike")
{
    // 64 * 4096 frames of 0x111 at 1 ms, byte 0 = a sawtooth with one 255 spike: level 1 gets 4096 buckets.
    const std::size_t n = graph_lod_fan * graph_lod_top;
    const std::size_t spike = 123'456;
    const auto src = std::filesystem::temp_directory_path() / std::format("kraken_graph_{}.log", getpid());
    const auto cache = std::filesystem::temp_directory_path() / std::format("kraken_graph_{}.kfc", getpid());
    {
        std::ofstream out(src, std::ios::binary);
        for (std::size_t i = 0; i < n; ++i)
        {
            out << std::format("({:.6f}) vcan0 111#{:02X}\n", static_cast<double>(i) * 1e-3, i == spike ? 255 : i % 100);
        }
    }
    {
        auto built = frame_cache_build(src, cache, TraceFileFormat::CanDump);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    auto c = frame_cache_open(src, cache);
    REQUIRE(c.has_value());
    App app;
    app.trace_file = std::make_shared<FrameCache>(*c); // closed by hand below
    trace_open_file(app.trace, app.trace_file->recs, app.trace_file->overflow);
    SetupNetwork& net = app.setup.networks.emplace_back();
    net.name = "net";
    net.can_dbs.push_back(std::make_shared<CanDb>());
    REQUIRE(dbc_parse("BO_ 273 Msg: 1 ECU\n SG_ Sig : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n", *net.can_dbs[0]));
    setup_rebuild_cache(app.setup);

    GraphState g;
    GraphSignal& s = g.signals.emplace_back();
    s.network = "net";
    s.name = "Sig";
    s.can_raw_id = 0x111;
    g.plot_px = 100; // wants 400 buckets: the whole file fits level 1 (4096), a 1 s window does not
    const auto settle = [&]
    {
        for (int i = 0; i < 1000 && (s.lod == nullptr || s.t.empty()); ++i)
        {
            graph_ingest(g, app);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    settle();
    REQUIRE(s.lod != nullptr);
    REQUIRE(s.lod->t.size() == 2);
    CHECK(s.lod->t[0].size() == n);
    CHECK(g.x_min == 0.0);
    CHECK(g.x_max == doctest::Approx(static_cast<double>(n - 1) * 1e-3));
    CHECK(s.t.size() == s.lod->t[1].size()); // the whole file: level 1
    CHECK(std::count(s.v.begin(), s.v.end(), 255.0) == 1);
    CHECK(s.seen_min == 0.0);
    CHECK(s.seen_max == 255.0);

    g.x_min = 123.0; // 1 s around the spike: level 0, every sample
    g.x_max = 124.0;
    graph_ingest(g, app);
    CHECK((s.t.size() >= 1001 && s.t.size() <= 1003)); // 1 s of 1 ms samples plus a neighbour each side
    CHECK(s.v[static_cast<std::size_t>(std::lower_bound(s.t.begin(), s.t.end(), 123.456) - s.t.begin())] == 255.0);
    g.x_min = 200.0; // away from it
    g.x_max = 201.0;
    graph_ingest(g, app);
    CHECK(std::count(s.v.begin(), s.v.end(), 255.0) == 0);

    app.setup.generation++; // a setup change rebuilds the pyramid
    s.t.clear();
    settle();
    CHECK(s.lod_generation == app.setup.generation);
    CHECK((s.t.size() >= 1001 && s.t.size() <= 1003)); // 1 s of 1 ms samples plus a neighbour each side

    g.signals.clear(); // drops the pyramid (and would stop a running job) before the mapping goes
    app.trace_file.reset();
    frame_cache_close(*c);
    std::filesystem::remove(src);
    std::filesystem::remove(cache);
}
