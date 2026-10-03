// REST API: route handling on the main thread and one real HTTP round trip on localhost.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <thread>

#include "app.h"
#include "core/net.h"
#include "core/rest_api.h"

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
