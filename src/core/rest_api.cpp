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
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <utility>
#include <future>

#include "app.h"
#include "core/charconv.h"
#include "core/log.h"
#include "core/net.h"
#include "core/tasks.h"
#include "core/text.h"
#include "decoders/decoders.h"
#include "ui/diag_dialog.h"

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

// "0x7E0", "7E0h" is not accepted; a JSON number is decimal.
std::optional<uint32_t> json_id(std::string_view v)
{
    int base = 10;
    if (v.starts_with("0x") || v.starts_with("0X"))
    {
        v.remove_prefix(2);
        base = 16;
    }
    uint32_t id = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), id, base);
    return ec == std::errc{} && end == v.data() + v.size() && id <= can_id_mask_extended ? std::optional(id) : std::nullopt;
}

// /uds and /uds/scan wait for ECUs, up to seconds: only the interface lookup runs on the main
// thread, the request itself on this one, so the UI keeps drawing meanwhile.
// /uds/dtc_file, /uds/did_file: write (or extend) a "<hex code> <text>" file, the format the UDS
// window's DTC text / DID text Load reads, one per product. codes: one "<code> <text>" per line.
std::string code_file_json(std::string_view path, std::string_view body, int& status)
{
    const std::string file = json_get(body, "path");
    if (file.empty())
    {
        status = 400;
        return "{\"error\":\"path missing\"}";
    }
    const bool dtc = path == "/uds/dtc_file";
    std::map<uint32_t, std::string> incoming;
    parse_code_lines(json_get(body, "codes"), incoming);
    // The file's own lines stay as they are (comments, hand-written entries); a line whose code
    // is in `incoming` gets the new text, the rest of `incoming` is appended.
    std::string out;
    bool fresh = true;
    if (std::ifstream in(file); in && json_get(body, "replace") != "true")
    {
        fresh = false;
        for (std::string line; std::getline(in, line);)
        {
            std::map<uint32_t, std::string> one;
            if (parse_code_lines(line, one) == 1 && incoming.contains(one.begin()->first))
            {
                const uint32_t code = one.begin()->first;
                line = std::format("{:0{}X} {}", code, dtc ? 6 : 4, incoming[code]);
                incoming.erase(code);
            }
            out += line;
            out += '\n';
        }
    }
    std::string product = json_get(body, "product");
    std::ranges::replace_if(product, [](char c) { return c == '\n' || c == '\r'; }, ' ');
    if (fresh && !product.empty())
    {
        out += std::format("# {} for {}\n", dtc ? "DTC texts" : "DID names", product);
    }
    for (const auto& [code, text] : incoming)
    {
        out += std::format("{:0{}X} {}\n", code, dtc ? 6 : 4, text);
    }
    std::ofstream f(file, std::ios::binary);
    if (!f)
    {
        status = 400;
        return std::format("{{\"error\":{}}}", json_string("cannot write " + file));
    }
    f << out;
    std::map<uint32_t, std::string> all;
    parse_code_lines(out, all);
    status = 200;
    return std::format("{{\"path\":{},\"codes\":{}}}", json_string(file), all.size());
}

std::string uds_json(App& app, std::string_view path, std::string_view body, int& status)
{
    if (path == "/uds/dtc_file" || path == "/uds/did_file")
    {
        return code_file_json(path, body, status);
    }
    static constexpr std::string_view routes[] = {"/uds", "/uds/scan", "/uds/services", "/uds/dids", "/uds/session",
                                                  "/uds/tester_present", "/uds/reset", "/uds/security", "/uds/dtc",
                                                  "/uds/dtc/clear"};
    if (std::ranges::find(routes, path) == std::end(routes))
    {
        status = 404;
        return "{\"error\":\"no such route\"}";
    }
    int index = -1;
    const std::string name = json_get(body, "iface");
    tasks_run_on_main_blocking(app.tasks, app, [&](App& a)
    {
        const auto it = std::ranges::find(a.ifaces, name, [](const Iface& i) { return i.info.name; });
        index = it != a.ifaces.end() ? it->index : -1;
    });
    status = 400;
    if (index < 0)
    {
        return "{\"error\":\"no such interface\"}";
    }
    const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
    const bool ext = json_get(body, "extended") == "true";
    const auto ecu_json = [](const UdsEcu& e)
    {
        return std::format("{{\"tx\":\"0x{:X}\",\"rx\":\"0x{:X}\",\"extended\":{},\"guessed\":{}{}}}",
                           e.tx_id, e.rx_id, e.extended, e.guessed,
                           e.name.empty() ? "" : std::format(",\"name\":{}", json_string(e.name)));
    };
    if (path == "/uds/scan")
    {
        std::vector<UdsEcu> ecus;
        if (json_get(body, "physical") == "true")
        {
            UdsSweep s;
            s.transport = {.iface = static_cast<uint16_t>(index), .extended = ext};
            if (const auto f = json_id(json_get(body, "first")))
            {
                s.first = *f;
            }
            if (const auto l = json_id(json_get(body, "last")))
            {
                s.last = *l;
            }
            if (const auto te = json_id(json_get(body, "tester")))
            {
                s.tester = static_cast<uint8_t>(*te);
            }
            ecus = uds_sweep(app.uds_client, send, s);
        }
        else
        {
            ecus = uds_scan(app.uds_client, send, static_cast<uint16_t>(index), std::chrono::milliseconds(300));
        }
        std::string out;
        for (const UdsEcu& ecu : ecus)
        {
            out += (out.empty() ? "" : ",") + ecu_json(ecu);
        }
        status = 200;
        return std::format("{{\"ecus\":[{}]}}", out);
    }

    // Transport shared by /uds, /uds/services and /uds/dids.
    const auto byte_of = [&](const char* key) -> std::optional<uint8_t>
    {
        const auto v = json_id(json_get(body, key));
        return v && *v <= 0xFF ? std::optional<uint8_t>(static_cast<uint8_t>(*v)) : std::nullopt;
    };
    const auto tx = json_id(json_get(body, "tx"));
    const auto rx = json_id(json_get(body, "rx"));
    if (!tx || !rx)
    {
        return "{\"error\":\"need tx, rx (number or \\\"0x7E0\\\")\"}";
    }
    UdsTarget t{.iface = static_cast<uint16_t>(index), .tx_id = *tx, .rx_id = *rx, .extended = ext,
                .fd = json_get(body, "fd") == "true", .brs = json_get(body, "brs") == "true", .tx_len = 64};
    if (const auto p = byte_of("padding"))
    {
        t.padding = *p;
    }
    t.tx_ae = byte_of("tx_ae");
    t.rx_ae = byte_of("rx_ae");
    UdsTiming timing;
    const auto seconds = [&](const char* key, std::chrono::milliseconds& dst)
    {
        const std::string s = json_get(body, key);
        double sec = 0.0;
        if (!s.empty() && float_from_chars(s.data(), s.data() + s.size(), sec).ec == std::errc{} && sec > 0.0)
        {
            dst = std::chrono::milliseconds(static_cast<int64_t>(sec * 1000.0));
        }
    };
    seconds("timeout", timing.p2);
    seconds("p2_star", timing.p2_star);

    if (path == "/uds/services")
    {
        std::string out;
        for (const UdsServiceProbe& p : uds_service_scan(app.uds_client, send, t, timing))
        {
            if (p.supported())
            {
                out += std::format("{}{{\"sid\":\"0x{:02X}\",\"name\":{}}}", out.empty() ? "" : ",", p.sid,
                                   json_string(uds_service_name(p.sid)));
            }
        }
        status = 200;
        return std::format("{{\"services\":[{}]}}", out);
    }
    if (path == "/uds/dids")
    {
        const auto first = json_id(json_get(body, "first")).value_or(0xF180);
        const auto last = json_id(json_get(body, "last")).value_or(0xF1FF);
        std::string out;
        for (const UdsDidValue& v : uds_did_scan(app.uds_client, send, t, static_cast<uint16_t>(first),
                                                 static_cast<uint16_t>(last), timing))
        {
            out += std::format("{}{{\"did\":\"0x{:04X}\",\"name\":{},\"data\":{}}}", out.empty() ? "" : ",", v.did,
                               json_string(std::string(uds_did_name(v.did))), json_string(format_hex_bytes(v.data)));
        }
        status = 200;
        return std::format("{{\"dids\":[{}]}}", out);
    }

    // The convenience routes build the request bytes; /uds takes them as given.
    std::optional<std::vector<uint8_t>> req;
    std::string bad; // a field the route could not use: 400 with its name
    const auto named = [&](const char* key, std::initializer_list<std::pair<const char*, uint8_t>> names, uint8_t fallback)
    {
        const std::string v = json_get(body, key);
        if (v.empty())
        {
            return fallback;
        }
        for (const auto& [name, sub] : names)
        {
            if (v == name)
            {
                return sub;
            }
        }
        if (const auto b = byte_of(key))
        {
            return *b;
        }
        bad = key;
        return fallback;
    };
    if (path == "/uds/session")
    {
        req = {0x10, named("session", {{"default", 1}, {"programming", 2}, {"extended", 3}, {"safety", 4}}, 3)};
    }
    else if (path == "/uds/tester_present")
    {
        req = {0x3E, 0x00};
    }
    else if (path == "/uds/reset")
    {
        req = {0x11, named("kind", {{"hard", 1}, {"keyOffOn", 2}, {"soft", 3}}, 1)};
    }
    else if (path == "/uds/security")
    {
        int level = 1;
        const std::string lv = json_get(body, "level");
        std::from_chars(lv.data(), lv.data() + lv.size(), level);
        level = std::max(1, level);
        const auto key = parse_hex_bytes(json_get(body, "key"));
        req = {0x27, static_cast<uint8_t>(key && !key->empty() ? 2 * level : 2 * level - 1)};
        if (key)
        {
            req->insert(req->end(), key->begin(), key->end());
        }
    }
    else if (path == "/uds/dtc")
    {
        const std::string m = json_get(body, "status_mask");
        const auto mask = parse_hex_bytes(m); // hex, "FF" or "0x2F"
        if (!m.empty() && (!mask || mask->size() != 1))
        {
            bad = "status_mask";
        }
        req = {0x19, 0x02, mask && mask->size() == 1 ? mask->front() : uint8_t{0xFF}};
    }
    else if (path == "/uds/dtc/clear")
    {
        const std::string g = json_get(body, "group");
        const auto group = parse_hex_bytes(g);
        if (!g.empty() && (!group || group->size() != 3))
        {
            bad = "group"; // never fall back to "all DTCs" on a typo
        }
        req = {0x14, 0xFF, 0xFF, 0xFF};
        if (group && group->size() == 3)
        {
            std::ranges::copy(*group, req->begin() + 1);
        }
    }
    else
    {
        req = parse_hex_bytes(json_get(body, "data"));
    }
    if (!bad.empty())
    {
        return std::format("{{\"error\":{}}}", json_string("bad " + bad));
    }
    if (!req || req->empty())
    {
        return "{\"error\":\"need data (hex bytes like \\\"22 F1 90\\\")\"}";
    }
    const UdsResult r = uds_request(app.uds_client, send, t, *req, timing);
    if (!r.error.empty())
    {
        status = r.error.starts_with("no response") ? 504 : 409;
        return std::format("{{\"error\":{}}}", json_string(r.error));
    }
    status = 200;
    std::string extra;
    if (path == "/uds/dtc" && r.response.size() >= 3 && r.response[0] == 0x59)
    {
        // The list as the DTC text files spell the codes (6 hex digits), with the status byte.
        std::string list;
        for (std::size_t i = 3; i + 4 <= r.response.size(); i += 4)
        {
            list += std::format("{}{{\"code\":\"{:02X}{:02X}{:02X}\",\"status\":\"0x{:02X}\"}}", list.empty() ? "" : ",",
                                r.response[i], r.response[i + 1], r.response[i + 2], r.response[i + 3]);
        }
        extra = std::format(",\"dtcs\":[{}]", list);
    }
    return std::format("{{\"request\":{},\"response\":{},\"text\":{}{}}}", json_string(format_hex_bytes(*req)),
                       json_string(format_hex_bytes(r.response)), json_string(uds_describe(*req, r.response)), extra);
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
                if (method == "POST" && (path == "/uds" || path.starts_with("/uds/")))
                {
                    body = uds_json(app, path, content, status);
                }
                else
                {
                    tasks_run_on_main_blocking(app.tasks, app, [&](App& a) { body = rest_api_handle(a, method, path, content, status); });
                }
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
        if (!app.measuring && !app.measurement_error.empty())
        {
            // Refused (no interface / all down): the API caller gets the reason, not the GUI modal.
            status = 409;
            return std::format("{{\"error\":{}}}", json_string(std::exchange(app.measurement_error, {})));
        }
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
