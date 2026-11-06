// REST API routes of Replay, the Trace window and Value Search (core/rest_api_replay.cpp).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include <unistd.h>

#include "app.h"
#include "core/rest_api.h"
#include "test_env.h"
#include "ui/workspace_tabs.h"

namespace
{

// rest_api_handle without [[nodiscard]]: many checks only look at the status.
std::string api(App& app, std::string_view method, std::string_view path, std::string_view body, int& status)
{
    return rest_api_handle(app, method, path, body, status);
}

bool has(const std::string& body, std::string_view part)
{
    return body.find(part) != std::string::npos;
}

std::filesystem::path write_candump()
{
    test_setenv("XDG_CACHE_HOME", std::filesystem::temp_directory_path()); // frame caches go there
    const auto path = std::filesystem::temp_directory_path()
        / std::format("rest_replay_{}_{:08x}.candump", getpid(), std::random_device{}());
    std::ofstream(path) << "(1700000000.000000) vcan0 123#0102\n"
                           "(1700000000.100000) vcan0 456#AABBCC\n"
                           "(1700000000.200000) vcan1 123#0103\n"
                           "(1700000000.300000) vcan0 7E8#01FF\n";
    return path;
}

// GET /replay until the load has landed.
std::string load(App& app, const std::filesystem::path& path)
{
    int status = 0;
    std::string body = api(app, "POST", "/replay/load", std::format(R"({{"path":{}}})", json_string(path.string())), status);
    REQUIRE(status == 200);
    const auto t0 = std::chrono::steady_clock::now();
    while (has(body, "\"loading\":true") && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        body = api(app, "GET", "/replay", "", status);
    }
    return body;
}

} // namespace

TEST_CASE("replay routes: errors without a tab, a file or a load")
{
    App app;
    int status = 0;
    api(app, "GET", "/replay", "", status);
    CHECK(status == 409); // no workspace tab
    workspace_add_tab(app.workspace);
    std::string body = api(app, "GET", "/replay", "", status);
    CHECK(status == 200);
    CHECK(has(body, "\"open\":false"));
    CHECK(has(body, "\"frames\":0"));
    api(app, "POST", "/replay/load", R"({"path":"/nonexistent.asc"})", status);
    CHECK(status == 400);
    api(app, "POST", "/replay/play", "", status);
    CHECK(status == 409);
    api(app, "POST", "/replay/pause", "", status);
    CHECK(status == 409);
    api(app, "POST", "/replay/config", R"({"speed":20})", status);
    CHECK(status == 400);
    api(app, "POST", "/replay/config", R"({"from":"abc"})", status);
    CHECK(status == 400);
    api(app, "POST", "/replay/channel", R"({"channel":"vcan9"})", status);
    CHECK(status == 400);
    api(app, "POST", "/replay/nope", "", status);
    CHECK(status == 404);
}

TEST_CASE("replay routes: load, config, channel, step / pause / stop")
{
    App app;
    workspace_add_tab(app.workspace);
    const auto path = write_candump();
    std::string body = load(app, path);
    CHECK(has(body, "\"open\":true"));
    CHECK(has(body, "\"frames\":4"));
    CHECK(has(body, R"("name":"vcan0")"));
    CHECK(has(body, R"("name":"vcan1")"));

    int status = 0;
    body = api(app, "POST", "/replay/config", R"({"speed":2.5,"loop":true,"from":"0.1","break_at":"0.2"})", status);
    CHECK(status == 200);
    const Replay& r = app.replays[workspace_current(app.workspace)->uid];
    CHECK(r.speed == doctest::Approx(2.5));
    CHECK(r.loop);
    CHECK(r.range_from == "0.1");
    CHECK(r.break_at == "0.2");
    CHECK(has(body, "\"play_frames\":3")); // from 0.1 s on
    api(app, "POST", "/replay/config", R"({"from":"","loop":false})", status);
    CHECK(r.range_from.empty());
    CHECK_FALSE(r.loop);

    body = api(app, "POST", "/replay/channel", R"({"channel":"vcan1","enabled":false})", status);
    CHECK(status == 200);
    CHECK(has(body, R"("name":"vcan1","iface":"","enabled":false)"));
    api(app, "POST", "/replay/channel", R"({"channel":0,"iface":"nope"})", status);
    CHECK(status == 400);

    body = api(app, "POST", "/replay/step", "", status); // starts paused after one frame
    CHECK(status == 200);
    CHECK(has(body, "\"running\":true"));
    api(app, "POST", "/replay/play", "", status);
    CHECK(status == 409);
    api(app, "POST", "/replay/config", R"({"speed":1})", status);
    CHECK(status == 409); // fixed while playing
    api(app, "POST", "/replay/config", R"({"autoplay":true})", status);
    CHECK(status == 200);
    body = api(app, "POST", "/replay/stop", "", status);
    CHECK(has(body, "\"running\":false"));
    app.replays.clear();
    std::filesystem::remove(path);
}

TEST_CASE("trace routes: frames, filter, view, goto")
{
    App app;
    workspace_add_tab(app.workspace);
    int status = 0;
    BusMessage m{.id = 0x123, .ts_ns = 1'000'000'000};
    set_length(m, 2);
    m.data[0] = 0xAB;
    trace_append(app.trace, {&m, 1});
    m.id = 0x456;
    m.ts_ns += 500'000'000;
    trace_append(app.trace, {&m, 1});

    std::string body = api(app, "GET", "/trace/frames?last=1", "", status);
    CHECK(status == 200);
    CHECK(has(body, R"("index":1,"ts_ns":1500000000,"t":0.5)"));
    CHECK(has(body, R"("id":"0x456")"));
    CHECK(has(body, R"("data":"AB 00")"));
    CHECK_FALSE(has(body, "0x123"));
    body = api(app, "GET", "/trace/frames?from=0&count=5", "", status);
    CHECK(has(body, "0x123"));
    CHECK(has(body, "0x456"));
    api(app, "GET", "/trace/frames?last=x", "", status);
    CHECK(status == 400);

    body = api(app, "POST", "/trace/filter", R"({"text":"45","tx":false,"hidden_ids":"0x123, 7E8"})", status);
    CHECK(status == 200);
    const TraceWindowState& s = app.trace_windows[workspace_current(app.workspace)->uid];
    CHECK(s.filter.text == "45");
    CHECK_FALSE(s.filter.show_tx);
    CHECK(s.filter.hidden_ids == std::set<uint32_t>{0x123, 0x7E8});
    CHECK(has(body, R"("hidden_ids":["0x123","0x7E8"])"));
    api(app, "POST", "/trace/filter", R"({"hidden_ids":"xyz"})", status);
    CHECK(status == 400);
    api(app, "POST", "/trace/filter", R"({"hidden_ifaces":"nope"})", status);
    CHECK(status == 400);
    CHECK(s.filter.hidden_ids.size() == 2); // a refused request changes nothing

    body = api(app, "POST", "/trace/view", R"({"tab":"uds","view":"log"})", status);
    CHECK(status == 200);
    CHECK(s.tab == TraceTab::Uds);
    CHECK(s.modes[static_cast<int>(TraceTab::Uds)] == TraceViewMode::Rolling);
    api(app, "POST", "/trace/view", R"({"tab":"can"})", status);
    CHECK(status == 400);

    api(app, "POST", "/trace/goto", R"({"index":1})", status);
    CHECK(status == 200);
    CHECK(s.tab_goto == static_cast<int>(TraceTab::Monitor));
    api(app, "POST", "/trace/goto", R"({"index":7})", status);
    CHECK(status == 400);

    // Not mine: the existing routes still answer.
    api(app, "POST", "/trace/clear", "", status);
    CHECK(status == 200);
}

TEST_CASE("value search routes: raw search over a loaded file, hits")
{
    App app;
    workspace_add_tab(app.workspace);
    const auto path = write_candump();
    load(app, path);
    int status = 0;
    api(app, "POST", "/value_search/raw", "{}", status);
    CHECK(status == 400);
    api(app, "POST", "/value_search/raw", R"({"data":"0G"})", status);
    CHECK(status == 400);
    std::string body = api(app, "POST", "/value_search/raw", R"({"id":"123","data":"01 ??"})", status);
    CHECK(status == 200);
    CHECK(has(body, "\"raw\":true"));
    const auto t0 = std::chrono::steady_clock::now();
    do
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        body = api(app, "GET", "/value_search/hits?count=1", "", status);
    } while (has(body, "\"scanning\":true") && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    CHECK(status == 200);
    CHECK(has(body, "\"hits\":2"));
    CHECK(has(body, R"("list":[{"index":0,)"));
    CHECK_FALSE(has(body, R"("index":2,)")); // count=1
    body = api(app, "GET", "/value_search/hits?from=1", "", status);
    CHECK(has(body, R"("list":[{"index":2,)"));
    app.value_searches.clear();
    app.replays.clear();
    std::filesystem::remove(path);
}
