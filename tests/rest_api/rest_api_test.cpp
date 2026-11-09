// REST API: route handling on the main thread and one real HTTP round trip on localhost.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include "app.h"
#include "core/net.h"
#include "core/rest_api.h"
#include "drivers/driver.h"
#include "ui_test.h" // fake_driver

namespace
{

// A measurement start needs an openable interface since the refused-start change.
void add_fake(App& app)
{
    Iface& i = app.ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    app.setup.networks.push_back({.name = "Net"});
    app.setup.networks[0].interfaces.push_back({.driver = "Fake", .name = "fake0", .enabled = true});
}

} // namespace

TEST_CASE("json_get: a value spelled like a key is not the key; spacing, negatives, null, truncated bodies")
{
    CHECK(json_get(R"({"name":"id","id":5})", "id") == "5");
    CHECK(json_get(R"({"comment":"path","path":"/x/y"})", "path") == "/x/y");
    CHECK(json_get(R"({"path":"path"})", "path") == "path");
    CHECK(json_get(R"({ "id"  :  7 })", "id") == "7");
    CHECK(json_get("{\"id\":\n\t-3.5}", "id") == "-3.5");
    CHECK(json_get(R"({"id":null})", "id") == "null");
    CHECK(json_get(R"({"id":"unterminated)", "id") == "unterminated");
    CHECK(json_get(R"({"id"})", "id").empty());
    CHECK(json_get(R"({"id":})", "id").empty());
    CHECK(json_get(R"({"id")", "id").empty());
    CHECK(json_get("", "id").empty());
    CHECK(json_get(R"({"identity":1,"id":2})", "id") == "2");
    CHECK(json_get(R"({"a":"x\"id\":9","id":1})", "id") == "1");
}

TEST_CASE("json_get: strings with escapes, numbers, booleans, missing keys")
{
    const std::string_view body = R"({"path": "a\"b\\c\nd", "id": 291, "fd": true, "iface":"vcan0"})";
    CHECK(json_get(body, "path") == "a\"b\\c\nd");
    CHECK(json_get(body, "id") == "291");
    CHECK(json_get(body, "fd") == "true");
    CHECK(json_get(body, "iface") == "vcan0");
    CHECK(json_get(body, "nope").empty());
}

TEST_CASE("routes: status, measurement start/stop, trace clear, record, errors")
{
    App app;
    int status = 0;
    std::string body = rest_api_handle(app, "GET", "/status", "", status);
    CHECK(status == 200);
    CHECK(body.find("\"measuring\":false") != std::string::npos);
    CHECK(body.find("\"trace_frames\":0") != std::string::npos);

    // No interface: the start is refused with the reason, and no GUI modal is left pending.
    body = rest_api_handle(app, "POST", "/measurement/start", "", status);
    CHECK(status == 409);
    CHECK_FALSE(app.measuring);
    CHECK(body.find("Setup Interface") != std::string::npos);
    CHECK(app.measurement_error.empty());

    add_fake(app);
    body = rest_api_handle(app, "POST", "/measurement/start", "", status);
    CHECK(status == 200);
    CHECK(app.measuring);
    CHECK(body.find("\"measuring\":true") != std::string::npos);
    const BusMessage m{.id = 0x123};
    trace_append(app.trace, {&m, 1});
    body = rest_api_handle(app, "GET", "/status", "", status);
    CHECK(body.find("\"trace_frames\":1") != std::string::npos);
    rest_api_handle(app, "POST", "/trace/clear", "", status);
    CHECK(trace_size(app.trace) == 0);
    rest_api_handle(app, "POST", "/measurement/stop", "", status);
    CHECK_FALSE(app.measuring);
    rest_api_handle(app, "POST", "/record", R"({"armed": true})", status);
    CHECK(app.recorder.armed); // armed for the next measurement (while one runs it would open the file)
    rest_api_handle(app, "POST", "/record", R"({"armed": false})", status);
    CHECK_FALSE(app.recorder.armed);

    rest_api_handle(app, "GET", "/measurement/start", "", status);
    CHECK(status == 404);
    rest_api_handle(app, "GET", "/nope", "", status);
    CHECK(status == 404);
    rest_api_handle(app, "POST", "/trace/save", "{}", status);
    CHECK(status == 400);
    body = rest_api_handle(app, "POST", "/send", R"({"iface":"none","id":1,"data":"01"})", status);
    CHECK(status == 400);
    CHECK(body.find("no such interface") != std::string::npos);
}

TEST_CASE("HTTP round trip on 127.0.0.1: the request runs on the main thread")
{
    App app;
    add_fake(app);
    RestApi api;
    REQUIRE(rest_api_start(api, app, 0));
    REQUIRE(api.port != 0);
    std::string reply;
    std::jthread client([&]
    {
        const int fd = net_socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(api.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        const std::string request = "POST /measurement/start HTTP/1.0\r\nContent-Length: 0\r\n\r\n";
        REQUIRE(send(fd, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()));
        for (char buf[1024];;)
        {
            const auto n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            reply.append(buf, static_cast<std::size_t>(n));
        }
        net_close(fd);
    });
    while (client.joinable() && reply.empty()) // the main thread's job: run the request
    {
        tasks_drain(app.tasks, app);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();
    CHECK(reply.starts_with("HTTP/1.0 200 OK"));
    CHECK(reply.find("\"measuring\":true") != std::string::npos);
    CHECK(app.measuring);
    rest_api_stop(api, app);
    CHECK_FALSE(api.thread.joinable());
    app_measurement_stop(app);
}

namespace
{

// One request to the server, answered by draining the tasks on this (the main) thread.
std::string http(App& app, const RestApi& api, const std::string& method, const std::string& path, const std::string& body)
{
    std::string reply;
    std::jthread client([&]
    {
        const int fd = net_socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(api.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        const std::string request = std::format("{} {} HTTP/1.0\r\nContent-Length: {}\r\n\r\n{}", method, path, body.size(), body);
        REQUIRE(send(fd, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()));
        for (char buf[1024];;)
        {
            const auto n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            reply.append(buf, static_cast<std::size_t>(n));
        }
        net_close(fd);
    });
    while (reply.empty())
    {
        tasks_drain(app.tasks, app);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();
    return reply;
}

} // namespace

TEST_CASE("vcan: bad names refused; an existing link is only re-enumerated")
{
    App app;
    RestApi api;
    REQUIRE(rest_api_start(api, app, 0));
    CHECK(http(app, api, "POST", "/vcan", R"({"name":"a b"})").starts_with("HTTP/1.0 400"));
    CHECK(http(app, api, "POST", "/vcan", R"({"name":"-help"})").starts_with("HTTP/1.0 400"));
    if (socketcan_available)
    {
        // lo exists and is up: no `ip` (no pkexec prompt), just the interface reload.
        const std::string reply = http(app, api, "POST", "/vcan", R"({"name":"lo"})");
        CHECK(reply.starts_with("HTTP/1.0 200"));
        CHECK(reply.find("\"vcan\":\"lo\"") != std::string::npos);
        CHECK(reply.find("\"interfaces\":[") != std::string::npos);
    }
    rest_api_stop(api, app);
}

TEST_CASE("interface, database, setup, graph, value search, commands")
{
    UiTest ui(ImVec2(800.0f, 600.0f), true, true); // graph colours need ImPlot
    App app;
    workspace_add_tab(app.workspace);
    Iface& i = app.ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    int status = 0;

    std::string body = rest_api_handle(app, "POST", "/interface", R"({"name":"nope"})", status);
    CHECK(status == 400);
    body = rest_api_handle(app, "POST", "/interface", R"({"name":"fake0"})", status);
    CHECK(status == 200);
    REQUIRE(app.setup.networks.size() == 1);
    CHECK(app.setup.networks[0].interfaces[0].enabled);
    CHECK(body.find("\"enabled\":true") != std::string::npos);
    rest_api_handle(app, "POST", "/interface", R"({"name":"fake0","enabled":false})", status);
    CHECK_FALSE(app.setup.networks[0].interfaces[0].enabled);
    rest_api_handle(app, "POST", "/interface", R"({"name":"fake0","enabled":true})", status);
    CHECK(app.setup.networks.size() == 1); // the same entry, no second network

    const std::filesystem::path dbc = std::filesystem::temp_directory_path() / "kraken_rest_api_test.dbc";
    {
        std::ofstream f(dbc);
        f << "VERSION \"\"\n\nBU_: N\n\nBO_ 256 Status: 8 N\n"
             " SG_ Depth : 0|16@1+ (0.1,0) [0|6553.5] \"m\" N\n"
             " SG_ Rate : 16|8@1+ (1,0) [0|255] \"bpm\" N\n\n"
             "BO_ 257 Other: 8 N\n SG_ Rate : 0|8@1+ (1,0) [0|255] \"\" N\n";
    }
    rest_api_handle(app, "POST", "/database", R"({"path":"/nonexistent.dbc","iface":"fake0"})", status);
    CHECK(status == 400);
    rest_api_handle(app, "POST", "/database", std::format(R"({{"path":"{}","iface":"other"}})", dbc.generic_string()), status);
    CHECK(status == 400); // in no network
    body = rest_api_handle(app, "POST", "/database", std::format(R"({{"path":"{}","iface":"fake0"}})", dbc.generic_string()), status);
    CHECK(status == 200);
    CHECK(body.find("\"messages\":2") != std::string::npos);
    body = rest_api_handle(app, "GET", "/setup", "", status);
    CHECK(body.find("\"name\":\"Status.Depth\",\"unit\":\"m\"") != std::string::npos);

    rest_api_handle(app, "POST", "/graph", R"({"signals":""})", status);
    CHECK(status == 400);
    rest_api_handle(app, "POST", "/graph", R"({"signals":"Status.Depth","view":"xy"})", status);
    CHECK(status == 400); // xy needs two
    rest_api_handle(app, "POST", "/graph", R"({"signals":"Status.Depth","view":"polar"})", status);
    CHECK(status == 400);
    body = rest_api_handle(app, "POST", "/graph", R"({"signals":"Rate"})", status);
    CHECK(status == 400); // in two messages
    CHECK(body.find("Other.Rate") != std::string::npos); // the closest matches
    body = rest_api_handle(app, "POST", "/graph", R"({"signals":"Depth, Status.Rate","view":"xy"})", status);
    CHECK(status == 200);
    const auto& graphs = app.workspace.tabs[0].graphs;
    REQUIRE(graphs.size() == 1); // no docked "Graph" yet (it comes with the first frame): a new one
    CHECK(graphs[0].view == GraphView::XY);
    CHECK(graphs[0].signals.size() == 2);
    CHECK(graphs[0].signals[0].name == "Depth"); // X
    auto& tab_graphs = app.workspace.tabs[0].graphs;
    tab_graphs.emplace(tab_graphs.begin()); // the docked "Graph", empty: filled instead of a new window
    body = rest_api_handle(app, "POST", "/graph", R"({"signals":"Status.Depth"})", status);
    CHECK(body.find("\"graph\":\"Graph\"") != std::string::npos);
    CHECK(tab_graphs.size() == 2);
    CHECK(tab_graphs[0].signals.size() == 1);

    rest_api_handle(app, "POST", "/value_search", R"({"signal":"Status.Depth","from":"x"})", status);
    CHECK(status == 400);
    rest_api_handle(app, "POST", "/value_search", R"({"signal":"Nope"})", status);
    CHECK(status == 400);
    CHECK(app.value_searches[app.workspace.tabs[0].uid].query == "Nope");
    BusMessage m{.id = 256};
    m.data[0] = 0x10; // 0x2710 * 0.1 = 1000 m
    m.data[1] = 0x27;
    set_length(m, 8);
    trace_append(app.trace, {&m, 1});
    body = rest_api_handle(app, "POST", "/value_search", R"({"signal":"Status.Depth","from":999,"to":1001})", status);
    CHECK(status == 200);
    CHECK(body.find("\"hits\":1") != std::string::npos);

    body = rest_api_handle(app, "GET", "/commands", "", status);
    CHECK(body.find("{\"name\":\"Replay\",\"dialog\":false,\"enabled\":true}") != std::string::npos);
    CHECK(body.find("\"Exit\"") == std::string::npos);
    rest_api_handle(app, "POST", "/command", R"({"name":"Replay"})", status);
    CHECK(status == 200);
    CHECK(menu_take(app.menu, Command::NewReplayView));
    rest_api_handle(app, "POST", "/command", R"({"name":"Stop Measurement"})", status);
    CHECK(status == 409); // not measuring
    rest_api_handle(app, "POST", "/command", R"({"name":"Exit"})", status);
    CHECK(status == 400);
    CHECK_FALSE(app.quit);
    rest_api_handle(app, "POST", "/focus", R"({"window":"Nope"})", status);
    CHECK(status == 400);

    rest_api_handle(app, "POST", "/measurement/start", "", status);
    REQUIRE(app.measuring);
    rest_api_handle(app, "POST", "/interface", R"({"name":"fake0","enabled":false})", status);
    CHECK(status == 409);
    app_measurement_stop(app);
    std::filesystem::remove(dbc);
}

TEST_CASE("routes refuse garbage without touching the app")
{
    App app;
    int status = 0;
    for (const char* body : {"", "garbage", "{", R"({"iface":"nope","id":"zzz","data":"xx"})", R"({"id":"123"})"})
    {
        INFO("body ", body);
        const std::string out = rest_api_handle(app, "POST", "/send", body, status);
        CHECK(status == 400);
        CHECK(out.find("\"error\"") != std::string::npos);
    }
    CHECK(trace_size(app.trace) == 0);
    (void)rest_api_handle(app, "POST", "/trace/save", "{}", status);
    CHECK(status == 400);
    (void)rest_api_handle(app, "POST", "/command", R"({"nope":"x"})", status);
    CHECK(status == 400);
    (void)rest_api_handle(app, "POST", "/focus", "{}", status);
    CHECK(status == 400);
    (void)rest_api_handle(app, "GET", "/nope", "", status);
    CHECK(status == 404);
    (void)rest_api_handle(app, "DELETE", "/status", "", status);
    CHECK(status == 404);
    // The command is found by its key even when another value spells "name".
    const std::string label = command_label(Command::MeasurementStart);
    const std::string out =
        rest_api_handle(app, "POST", "/command", std::format(R"({{"comment":"name","name":{}}})", json_string(label)), status);
    CHECK(status == 200);
    CHECK(out.find(label) != std::string::npos);
    CHECK(menu_take(app.menu, Command::MeasurementStart)); // queued for the frame, like the menu item
}
