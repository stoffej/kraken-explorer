// ui/trace_window + ui/value_search over a loaded file: the Log's file view (vim keys, wheel and
// "Go to" move the page of a mapped frame cache) and Value Search (stretches of a signal's value
// range, a click on one syncs the Log and the Graph). Headless ImGui, no display.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_env.h"

#include <chrono>
#include <thread>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

#include <imgui.h>
#include <imgui_internal.h> // ActivateItemByID, FindWindowByName, ImHash*

#include "app.h"
#include "core/text.h"
#include "db/dbc/dbc_parser.h"
#include "ui_test.h"
#include "ui/frame_cache.h"
#include "ui/graph.h"
#include "ui/trace_window.h"
#include "ui/value_search.h"
#include "ui/workspace_tabs.h"

namespace
{

constexpr int frame_count = 2000;      // 0x111 and 0x222 alternate, 10 ms apart: 20 s
constexpr uint32_t angle_id = 0x111;   // Tentacle1 in the DBC below
constexpr int angle_frames = frame_count / 2;

// A candump log whose Tentacle1.Angle (16-bit signed, 0.01 deg, bytes 0-1) is a triangle over
// its 1000 frames: raw 0..499 then 500..1, so a value range holds two stretches of the file.
std::string candump_log()
{
    std::string s;
    for (int i = 0; i < frame_count; ++i)
    {
        const double t = static_cast<double>(i) * 0.01;
        if (i % 2 == 0)
        {
            const int k = i / 2;
            const int raw = k < 500 ? k : 1000 - k;
            s += std::format("({:.6f}) vcan0 {:03X}#{:02X}{:02X}000000000000\n", t, angle_id, raw & 0xFF, raw >> 8);
        }
        else
        {
            s += std::format("({:.6f}) vcan0 222#{:02X}\n", t, i & 0xFF);
        }
    }
    return s;
}

constexpr std::string_view tentacle_dbc = R"(VERSION ""

NS_:

BS_:

BU_: Kraken

BO_ 273 Tentacle1: 8 Kraken
 SG_ Angle : 0|16@1- (0.01,0) [-180|180] "deg" Kraken
 SG_ Curl : 16|8@1+ (0.5,0) [0|100] "%" Kraken
)";

// The log, its frame cache (built under XDG_CACHE_HOME = the scratch dir, as the app would) and
// an App showing the file, with the DBC in one setup network and one Graph in the tab.
struct FileView
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("kraken_file_view_{}", getpid());
    FrameCache cache;
    App app;
    WorkspaceTab tab{.title = "Trace", .uid = 1};
    const CanDbMessage* msg = nullptr;
    const CanDbSignal* angle = nullptr;

    FileView()
    {
        std::filesystem::create_directories(dir);
        test_setenv("XDG_CACHE_HOME", dir);
        const std::filesystem::path src = dir / "two_ids.log";
        std::ofstream(src, std::ios::binary) << candump_log();
        const std::filesystem::path kfc = frame_cache_path(src);
        REQUIRE(kfc.string().starts_with(dir.string()));
        {
            auto built = frame_cache_build(src, kfc, TraceFileFormat::CanDump);
            REQUIRE(built.has_value());
            frame_cache_close(*built);
            frame_cache_wait_saved(); // tests reopen it from disk
        }
        auto c = frame_cache_open(src, kfc);
        REQUIRE(c.has_value());
        cache = *c;
        app.trace_file = std::make_shared<FrameCache>(cache); // the views, unmapped by hand below
        trace_open_file(app.trace, app.trace_file->recs, app.trace_file->overflow);
        REQUIRE(trace_size(app.trace) == frame_count);

        auto db = std::make_shared<CanDb>();
        REQUIRE(dbc_parse(tentacle_dbc, *db));
        SetupNetwork net{.name = "Kraken"};
        net.can_dbs.push_back(db);
        app.setup.networks.push_back(std::move(net));
        setup_rebuild_cache(app.setup);
        msg = &db->messages.at(angle_id);
        angle = can_db_find_signal(*msg, "Angle");
        REQUIRE(angle != nullptr);
        tab.graphs.emplace_back();
    }
    ~FileView()
    {
        app.trace_file.reset();
        frame_cache_close(cache);
        std::filesystem::remove_all(dir);
    }

    TraceWindowState& log() { return app.trace_windows[tab.uid]; }
    ValueSearch& search() { return app.value_searches[tab.uid]; }
    SignalEntry entry() const
    {
        return {.label = "Tentacle1.Angle", .network = 0, .raw_id = angle_id, .can_msg = msg, .can_sig = angle};
    }

    // One headless frame: Trace (focused on request) above Value Search, the Graph ingesting as
    // draw_graph would.
    void frame(bool focus_trace = false)
    {
        ImGui::NewFrame();
        if (focus_trace)
        {
            ImGui::SetNextWindowFocus();
        }
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({1000.0f, 600.0f});
        draw_trace_window(app, log(), tab);
        ImGui::SetNextWindowPos({0.0f, 600.0f});
        ImGui::SetNextWindowSize({1000.0f, 300.0f});
        draw_value_search(app, search(), tab);
        graph_ingest(tab.graphs.front(), app);
        ImGui::EndFrame();
    }
    void type(std::string_view chars)
    {
        for (const char c : chars)
        {
            ImGui::GetIO().AddInputCharacter(static_cast<ImWchar>(c));
            frame();
        }
    }
    void press(ImGuiKey key, bool ctrl = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (ctrl)
        {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
        }
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        if (ctrl)
        {
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        }
        frame();
    }
    ImGuiWindow* window(const char* title) const
    {
        ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(tab, title).c_str());
        REQUIRE(w != nullptr);
        return w;
    }
};

} // namespace

TEST_CASE("file view Log: vim keys, the wheel and Go to move the page")
{
    const UiTest ui({1600, 900}, true, true);
    FileView f;
    TraceWindowState& s = f.log();
    s.modes[static_cast<int>(TraceTab::Monitor)] = TraceViewMode::Rolling;
    f.frame();
    f.frame(true); // both windows exist; the Trace window takes the keys from here on

    // Autoscroll (the default) pins the page to the end of the file.
    REQUIRE(s.autoscroll);
    REQUIRE(s.file_top > 0);
    REQUIRE(s.file_top < frame_count);
    const uint64_t end_top = s.file_top;

    f.type("j"); // first row selected: the page goes to it, reading back stops following
    CHECK(s.selected == 0);
    CHECK(s.file_top == 0);
    CHECK_FALSE(s.autoscroll);

    f.press(ImGuiKey_D, true); // half a page down: the selection moves, the page keeps it visible
    CHECK(s.selected > 0);
    CHECK(s.file_top <= static_cast<uint64_t>(s.selected));
    CHECK(static_cast<uint64_t>(s.selected) < s.file_top + end_top); // within a page below the top

    f.type("G"); // last row: autoscroll again, the page at the end
    CHECK(s.selected == frame_count - 1);
    CHECK(s.autoscroll);
    CHECK(s.file_top == end_top);

    f.type("gg");
    CHECK(s.selected == 0);
    CHECK(s.file_top == 0);
    CHECK_FALSE(s.autoscroll);

    // One wheel notch over the table scrolls three rows.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(500.0f, 400.0f);
    f.frame();
    io.AddMouseWheelEvent(0.0f, -1.0f);
    f.frame();
    CHECK(s.file_top == 3);

    // "Go to": the InputText "##goto" sits under the Monitor tab item (PushID 0) of "##trace_tabs".
    ImGuiWindow* w = f.window("Trace");
    const ImGuiID tab_bar = ImHashStr("##trace_tabs", 0, w->ID);
    const ImGuiID monitor = ImHashStr("Monitor", 0, tab_bar);
    const int zero = 0;
    const ImGuiID goto_id = ImHashStr("##goto", 0, ImHashData(&zero, sizeof zero, monitor));
    ImGui::ActivateItemByID(goto_id);
    f.frame();
    REQUIRE(ImGui::GetActiveID() == goto_id);
    f.type("0:10"); // m:ss, 10 s = frame 1000 exactly
    f.press(ImGuiKey_Enter);
    CHECK(s.file_goto == "10.000"); // normalised
    CHECK(s.selected == 1000);
    CHECK(s.file_top == 1000);
    CHECK_FALSE(s.autoscroll);
    CHECK(ImGui::GetActiveID() != goto_id); // Enter leaves the field

    // Between two frames: the first frame at or after the time. A malformed time keeps the field.
    ImGui::ActivateItemByID(goto_id);
    f.frame();
    s.file_goto.clear();
    f.type("5.005");
    f.press(ImGuiKey_Enter);
    CHECK(s.selected == 501);
    CHECK(s.file_top == 501);
    ImGui::ActivateItemByID(goto_id);
    f.frame();
    s.file_goto.clear();
    f.type("1:2:3:4");
    f.press(ImGuiKey_Enter);
    CHECK(s.file_goto == "1:2:3:4");
    CHECK(s.selected == 501);
    CHECK(ImGui::GetActiveID() == goto_id); // still editing
}

TEST_CASE("Value Search: the stretches of a value range, a click syncs the Log and the Graph")
{
    const UiTest ui({1600, 900}, true, true);
    FileView f;
    const int64_t first_ts = trace_at(f.app.trace, f.app.trace.begin).ts_ns;

    // The search itself, through the cache's per-id index: the triangle crosses [2, 3] twice,
    // raw 200..300 on the way up (frames 400..600) and 300..200 on the way down (1400..1600).
    // 2..3 deg: raw 200..300 going up (frames 400..600) and 300..200 going down (1400..1600):
    // one hit per sample, in time order.
    const ValueResult found = value_search_run(f.app, *f.msg, *f.angle, 2.0, 3.0);
    const std::vector<ValueHit>& hits = found.hits;
    CHECK_FALSE(found.truncated);
    REQUIRE(hits.size() == 202);
    CHECK(hits[0].index == f.app.trace.begin + 400);
    CHECK(hits[0].ts_ns == first_ts + 4'000'000'000);
    CHECK(hits[0].value == doctest::Approx(2.0));
    CHECK(hits[100].index == f.app.trace.begin + 600);
    CHECK(hits[100].value == doctest::Approx(3.0));
    CHECK(hits[101].index == f.app.trace.begin + 1400);
    CHECK(hits[101].ts_ns == first_ts + 14'000'000'000);
    CHECK(value_search_run(f.app, *f.msg, *f.angle, -1e9, 1e9).hits.size() == 1000);
    CHECK(value_search_run(f.app, *f.msg, *f.angle, 100.0, 200.0).hits.empty());

    // The window: signal picked, bounds typed, Find pressed (activated by id, as a click would).
    ValueSearch& v = f.search();
    f.frame();
    v.signal = f.entry();
    v.setup_generation = f.app.setup.generation; // the pick happened after the setup was built
    v.from = "2.99"; // raw 299..300 going up (frames 598, 600) and down (1400, 1402): four rows, all drawn
    v.to = "3";
    ImGuiWindow* w = f.window("Value Search");
    ImGui::ActivateItemByID(ImHashStr("Find", 0, w->ID));
    f.frame();
    // A file view scans on the worker: frames until the result landed.
    for (int i = 0; i < 500 && v.job.valid(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        f.frame();
    }
    REQUIRE(v.hits.size() == 4);
    CHECK(v.status.starts_with("4 hits"));
    CHECK(v.selected == -1);
    f.frame(); // the hits table is drawn with its rows now

    // Click hit 2 (the first sample of the way down): its Selectable is "<time>" under PushID(row).
    GraphState& g = f.tab.graphs.front();
    REQUIRE(g.start_ns == first_ts); // ingested from the file, so graph_show_range has a time base
    f.log().modes[static_cast<int>(TraceTab::Monitor)] = TraceViewMode::Rolling; // the Log view
    const int row = 2;
    const ImGuiID table = ImHashStr("##hits", 0, w->ID);
    const std::string start = format_duration(static_cast<double>(v.hits[2].ts_ns - first_ts) / 1e9);
    CHECK(start == "14.000");
    ImGui::ActivateItemByID(ImHashStr(start.c_str(), 0, ImHashData(&row, sizeof row, table)));
    f.frame();
    REQUIRE(v.selected == 2);

    // The Log: the frame selected with three rows of context above it, no longer following the end.
    const TraceWindowState& s = f.log();
    CHECK(s.modes[static_cast<int>(TraceTab::Monitor)] == TraceViewMode::Rolling);
    CHECK(s.tab_goto == static_cast<int>(TraceTab::Monitor));
    CHECK(s.selected == 1400);
    CHECK(s.file_top == 1397);
    CHECK_FALSE(s.autoscroll);

    // The Graph: the signal added, the window around the sample, both cursors on it.
    REQUIRE(g.signals.size() == 1);
    CHECK(g.signals[0].can_sig == f.angle);
    CHECK_FALSE(g.follow);
    CHECK(g.cursor_on);
    CHECK(g.cursor_a == doctest::Approx(14.0));
    CHECK(g.cursor_b == doctest::Approx(14.0));
    CHECK(g.x_min == doctest::Approx(13.5));
    CHECK(g.x_max == doctest::Approx(14.5));
    // The pyramid is built off the main thread: draw frames until it landed (ASan builds take a
    // few hundred ms), then the visible window is a slice of it: only those samples (plus a
    // neighbour each side).
    for (int i = 0; i < 500 && g.signals[0].lod == nullptr; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        f.frame();
    }
    f.frame();
    const GraphSignal& sig = g.signals[0];
    REQUIRE_FALSE(sig.t.empty());
    CHECK(sig.t.size() < 400);
    CHECK(sig.t.front() >= 13.4);
    CHECK(sig.t.back() <= 14.6);
    CHECK(graph_value_at(sig.t, sig.v, 14.005) == doctest::Approx(3.0)); // frame 1400: raw 1000 - 700
}
