/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "core/rest_api.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <future>

#include "app.h"
#include "core/log.h"
#include "core/net.h"
#include "core/tasks.h"

namespace
{

// ponytail: one connection at a time, HTTP/1.0, bodies up to 1 MB; enough for curl and scripts.
constexpr std::size_t max_request = 1 << 20;

std::string json_string(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out + '"';
}

std::string status_json(const App& app)
{
    std::string ifaces;
    for (const Iface& i : app.ifaces)
    {
        ifaces += std::format("{}{{\"index\":{},\"driver\":{},\"name\":{},\"open\":{},\"up\":{}}}", ifaces.empty() ? "" : ",",
                              i.index, json_string(i.ops ? i.ops->name : ""), json_string(i.info.name),
                              i.open && !i.failed.load(), i.info.up);
    }
    return std::format("{{\"measuring\":{},\"recording\":{},\"record_armed\":{},\"trace_frames\":{},\"interfaces\":[{}]}}",
                       app.measuring, app.recorder.recording.load(), app.recorder.armed, trace_size(app.trace), ifaces);
}

// {"iface":"vcan0","id":..,"data":"01 02",...} -> sent; the error text when not.
std::string send_json(App& app, std::string_view body)
{
    const auto iface = std::ranges::find(app.ifaces, json_get(body, "iface"), [](const Iface& i) { return i.info.name; });
    if (iface == app.ifaces.end())
    {
        return "no such interface";
    }
    BusMessage m;
    const std::string id = json_get(body, "id");
    if (std::from_chars(id.data(), id.data() + id.size(), m.id).ec != std::errc{})
    {
        return "id missing";
    }
    m.iface = iface->index;
    m.flags |= json_get(body, "extended") == "true" ? bus_flag::extended : 0;
    m.flags |= json_get(body, "fd") == "true" ? bus_flag::fd : 0;
    int len = 0;
    const std::string hex = json_get(body, "data");
    for (std::size_t i = 0; i < hex.size();)
    {
        if (hex[i] == ' ')
        {
            ++i;
            continue;
        }
        unsigned byte = 0;
        if (i + 2 > hex.size() || len == bus_max_data_bytes
            || std::from_chars(hex.data() + i, hex.data() + i + 2, byte, 16).ec != std::errc{})
        {
            return "data: hex bytes like \"01 02 A3\"";
        }
        m.data[static_cast<std::size_t>(len++)] = static_cast<uint8_t>(byte);
        i += 2;
    }
    set_length(m, static_cast<uint8_t>(len));
    return iface_send(*iface, m) ? "" : "interface not open";
}

void serve(std::stop_token stop, RestApi& api, App& app)
{
    while (!stop.stop_requested())
    {
        if (net_wait_readable(api.fd, 200) <= 0)
        {
            continue;
        }
        const int client = static_cast<int>(accept(api.fd, nullptr, nullptr));
        if (client < 0)
        {
            continue;
        }
        std::string request;
        std::size_t header_end = std::string::npos;
        std::size_t content_length = 0;
        for (char buf[4096]; request.size() < max_request;)
        {
            if (header_end == std::string::npos)
            {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos)
                {
                    const std::size_t at = request.find("Content-Length:");
                    if (at != std::string::npos && at < header_end)
                    {
                        content_length = std::strtoul(request.c_str() + at + 15, nullptr, 10);
                    }
                }
            }
            if (header_end != std::string::npos && request.size() >= header_end + 4 + content_length)
            {
                break;
            }
            const auto n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        int status = 400;
        std::string body = "{\"error\":\"bad request\"}";
        if (header_end != std::string::npos)
        {
            const std::string_view line(request.data(), request.find("\r\n"));
            const std::size_t sp1 = line.find(' ');
            const std::size_t sp2 = line.find(' ', sp1 + 1);
            const std::string_view method = line.substr(0, sp1);
            const std::string_view path = sp1 == std::string_view::npos ? "" : line.substr(sp1 + 1, sp2 - sp1 - 1);
            const std::string_view content(request.data() + header_end + 4, std::min(content_length, request.size() - header_end - 4));
            try
            {
                tasks_run_on_main_blocking(app.tasks, app, [&](App& a) { body = rest_api_handle(a, method, path, content, status); });
            }
            catch (const std::future_error&)
            {
                status = 503; // shutting down
                body = "{\"error\":\"shutting down\"}";
            }
        }
        const std::string response = std::format("HTTP/1.0 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                                                 status, status == 200 ? "OK" : "Error", body.size(), body);
        for (std::size_t sent = 0; sent < response.size();)
        {
            const auto n = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
            if (n <= 0)
            {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        net_close(client);
    }
    api.done = true;
}

} // namespace

std::string json_get(std::string_view body, std::string_view key)
{
    const std::string quoted = std::format("\"{}\"", key);
    std::size_t at = body.find(quoted);
    if (at == std::string_view::npos)
    {
        return "";
    }
    at = body.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (at == std::string_view::npos)
    {
        return "";
    }
    if (body[at] != '"')
    {
        const std::size_t end = body.find_first_of(",}] \t\r\n", at);
        return std::string(body.substr(at, end - at));
    }
    std::string out;
    for (++at; at < body.size() && body[at] != '"'; ++at)
    {
        if (body[at] == '\\' && at + 1 < body.size())
        {
            const char e = body[++at];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
            continue;
        }
        out += body[at];
    }
    return out;
}

std::string rest_api_handle(App& app, std::string_view method, std::string_view path, std::string_view body, int& status)
{
    status = 200;
    const bool post = method == "POST";
    if (method == "GET" && path == "/status")
    {
        return status_json(app);
    }
    if (post && path == "/measurement/start")
    {
        app_measurement_start(app);
        return status_json(app);
    }
    if (post && path == "/measurement/stop")
    {
        app_measurement_stop(app);
        return status_json(app);
    }
    if (post && path == "/record")
    {
        app.menu.record_armed = json_get(body, "armed") == "true";
        recorder_set_armed(app.recorder, app.menu.record_armed, app.measuring);
        return status_json(app);
    }
    if (post && path == "/trace/clear")
    {
        trace_clear(app.trace);
        return status_json(app);
    }
    if (post && path == "/trace/save")
    {
        const std::string file = json_get(body, "path");
        if (file.empty())
        {
            status = 400;
            return "{\"error\":\"path missing\"}";
        }
        app_trace_save(app, file);
        return status_json(app);
    }
    if (post && path == "/send")
    {
        const std::string error = send_json(app, body);
        status = error.empty() ? 200 : 400;
        return error.empty() ? "{\"sent\":true}" : std::format("{{\"error\":{}}}", json_string(error));
    }
    if (post && path == "/script")
    {
        const std::string code = json_get(body, "code");
        if (code.empty())
        {
            status = 400;
            return "{\"error\":\"code missing\"}";
        }
        const bool started = python_run(app, app.python, code, "<rest>");
        status = started ? 200 : 409;
        return started ? "{\"started\":true}" : "{\"error\":\"a script is running\"}";
    }
    status = 404;
    return "{\"error\":\"no such route\"}";
}

bool rest_api_start(RestApi& api, App& app, uint16_t port)
{
    api.fd = net_socket(AF_INET, SOCK_STREAM, 0);
#ifndef _WIN32 // Winsock's SO_REUSEADDR lets a second process bind the same port; a closed port is free at once there
    const int on = 1;
    net_setsockopt(api.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (api.fd < 0 || bind(api.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(api.fd, 8) != 0
        || getsockname(api.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
    {
        log_error(std::format("REST API: cannot listen on 127.0.0.1:{}: {}", port, net_error_text()));
        return false;
    }
    api.port = ntohs(addr.sin_port);
    log_info(std::format("REST API listening on http://127.0.0.1:{}", api.port));
    api.thread = std::jthread(serve, std::ref(api), std::ref(app));
    return true;
}

void rest_api_stop(RestApi& api, App& app)
{
    if (!api.thread.joinable())
    {
        return;
    }
    api.thread.request_stop();
    while (!api.done)
    {
        tasks_drain(app.tasks, app); // a request waiting for the main thread
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    api.thread.join();
    net_close(api.fd);
    api.fd = -1;
}
