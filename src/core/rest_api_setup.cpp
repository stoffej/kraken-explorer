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

#include "core/rest_api_routes.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <memory>

#include "app.h"
#include "core/rest_api.h"
#include "core/setup.h"
#include "db/model/lin_db.h"
#include "ui/can_status.h"
#include "ui/setup_dialog.h"

namespace
{

constexpr std::string_view lin_versions[] = {"1.3", "2.0", "2.1", "2.2", "2.2A"}; // index = LinProtocolVersion
constexpr std::string_view lin_modes[] = {"monitor", "master", "slave"};         // index = LinNodeMode

std::string error_json(std::string_view text)
{
    return std::format("{{\"error\":{}}}", json_string(text));
}

std::string uint_list(const std::vector<unsigned>& v)
{
    std::string out;
    for (const unsigned x : v)
    {
        out += std::format("{}{}", out.empty() ? "" : ",", x);
    }
    return "[" + out + "]";
}

// The fields of one request body, read into a SetupInterface; the first bad value wins `error`.
struct Fields
{
    std::string_view body;
    std::string error;
};

// Decimal, or hex with 0x (custom bit timing registers). Missing: untouched, false.
template <class T>
bool num(Fields& f, std::string_view key, T& out, uint64_t max)
{
    const std::string v = json_get(f.body, key);
    if (v.empty())
    {
        return false;
    }
    const bool hex = v.starts_with("0x") || v.starts_with("0X");
    uint64_t n = 0;
    const char* first = v.data() + (hex ? 2 : 0);
    const auto [p, ec] = std::from_chars(first, v.data() + v.size(), n, hex ? 16 : 10);
    if (ec != std::errc{} || p != v.data() + v.size() || n > max)
    {
        f.error = f.error.empty() ? std::format("{}: a number up to {}", key, max) : f.error;
        return false;
    }
    out = static_cast<T>(n);
    return true;
}

bool flag(Fields& f, std::string_view key, bool& out)
{
    const std::string v = json_get(f.body, key);
    if (v.empty())
    {
        return false;
    }
    if (v != "true" && v != "false")
    {
        f.error = f.error.empty() ? std::format("{}: true or false", key) : f.error;
        return false;
    }
    out = v == "true";
    return true;
}

template <class E, std::size_t N>
void choice(Fields& f, std::string_view key, E& out, const std::string_view (&names)[N])
{
    const std::string v = json_get(f.body, key);
    if (v.empty())
    {
        return;
    }
    const auto it = std::ranges::find_if(names, [&](std::string_view n)
    {
        return std::ranges::equal(n, v, [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    });
    if (it == std::end(names))
    {
        std::string all;
        for (const std::string_view n : names)
        {
            all += std::format("{}{}", all.empty() ? "" : ", ", n);
        }
        f.error = f.error.empty() ? std::format("{}: one of {}", key, all) : f.error;
        return;
    }
    out = static_cast<E>(it - std::begin(names));
}

std::string interface_json(const Iface& iface, const SetupInterface& si, const std::string& network)
{
    SetupInterface snapped = si;
    const CanTimingLists lists = can_setup_snap(iface.info, snapped);
    return std::format(
        "{{\"name\":{},\"driver\":{},\"network\":{},\"bus_type\":{},\"up\":{},\"capabilities\":{},"
        "\"offered\":{{\"bitrates\":{},\"sample_points\":{},\"fd_bitrates\":{},\"fd_sample_points\":{}}},"
        "\"enabled\":{},\"configure\":{},\"bitrate\":{},\"sample_point\":{},\"can_fd\":{},\"fd_bitrate\":{},"
        "\"fd_sample_point\":{},\"listen_only\":{},\"one_shot\":{},\"triple_sampling\":{},\"auto_restart\":{},"
        "\"auto_restart_ms\":{},\"is_custom_bitrate\":{},\"custom_bitrate\":{},\"is_custom_fd_bitrate\":{},"
        "\"custom_fd_bitrate\":{},\"lin_baudrate\":{},\"lin_protocol\":{},\"lin_node_mode\":{},\"lin_listen_only\":{},"
        "\"lin_checksum_classic\":{},\"lin_ldf_path\":{},\"lin_schedule_table\":{},\"lin_slave_node\":{},"
        "\"lin_timebase_ms\":{},\"lin_jitter_us\":{}}}",
        json_string(si.name), json_string(si.driver), json_string(network),
        json_string(si.bus_type == BusType::LIN ? "LIN" : "CAN"), iface.info.up, iface.info.capabilities,
        uint_list(lists.bitrates), uint_list(lists.sample_points), uint_list(lists.fd_bitrates),
        uint_list(lists.fd_sample_points), si.enabled, si.configure, si.bitrate, si.sample_point, si.can_fd,
        si.fd_bitrate, si.fd_sample_point, si.listen_only, si.one_shot, si.triple_sampling, si.auto_restart,
        si.auto_restart_ms, si.is_custom_bitrate, si.custom_bitrate, si.is_custom_fd_bitrate, si.custom_fd_bitrate,
        si.lin_baudrate, json_string(lin_versions[static_cast<int>(si.lin_protocol)]),
        json_string(lin_modes[static_cast<int>(si.lin_node_mode)]), si.lin_listen_only, si.lin_checksum_classic,
        json_string(si.lin_ldf_path), json_string(si.lin_schedule_table), json_string(si.lin_slave_node),
        si.lin_timebase_ms, si.lin_jitter_us);
}

// Applies the fields of `body` to a copy of si, snaps CAN timing to what the interface offers and
// refuses a requested value it does not offer. "" on success.
std::string apply_fields(const Iface& iface, SetupInterface& si, std::string_view body)
{
    SetupInterface t = si;
    Fields f{body};
    const bool b = num(f, "bitrate", t.bitrate, 10'000'000);
    const bool sp = num(f, "sample_point", t.sample_point, 1000);
    const bool fb = num(f, "fd_bitrate", t.fd_bitrate, 20'000'000);
    const bool fsp = num(f, "fd_sample_point", t.fd_sample_point, 1000);
    bool fd = true;
    const bool fd_given = flag(f, "can_fd", fd);
    flag(f, "enabled", t.enabled);
    flag(f, "configure", t.configure);
    flag(f, "listen_only", t.listen_only);
    flag(f, "one_shot", t.one_shot);
    flag(f, "triple_sampling", t.triple_sampling);
    flag(f, "auto_restart", t.auto_restart);
    num(f, "auto_restart_ms", t.auto_restart_ms, 60'000);
    flag(f, "is_custom_bitrate", t.is_custom_bitrate);
    num(f, "custom_bitrate", t.custom_bitrate, 0xFFFFFF);
    flag(f, "is_custom_fd_bitrate", t.is_custom_fd_bitrate);
    num(f, "custom_fd_bitrate", t.custom_fd_bitrate, 0xFFFFFF);
    num(f, "lin_baudrate", t.lin_baudrate, 20'000);
    choice(f, "lin_protocol", t.lin_protocol, lin_versions);
    choice(f, "lin_node_mode", t.lin_node_mode, lin_modes);
    flag(f, "lin_listen_only", t.lin_listen_only);
    flag(f, "lin_checksum_classic", t.lin_checksum_classic);
    num(f, "lin_timebase_ms", t.lin_timebase_ms, 255);
    num(f, "lin_jitter_us", t.lin_jitter_us, 65535);
    // Strings: a key that is present sets them, also to "".
    if (body.find("\"lin_schedule_table\"") != std::string_view::npos)
    {
        t.lin_schedule_table = json_get(body, "lin_schedule_table");
    }
    if (body.find("\"lin_slave_node\"") != std::string_view::npos)
    {
        t.lin_slave_node = json_get(body, "lin_slave_node");
    }
    if (!f.error.empty())
    {
        return f.error;
    }
    t.custom_bitrate = custom_bitrate_clamp(t.custom_bitrate);
    t.custom_fd_bitrate = custom_fd_bitrate_clamp(t.custom_fd_bitrate);
    if (t.bus_type == BusType::CAN)
    {
        if (fd_given && fd && t.fd_bitrate == 0 && !t.is_custom_fd_bitrate)
        {
            t.fd_bitrate = 2'000'000; // FD on without a rate: the default, snapped below
        }
        const SetupInterface asked = t;
        const CanTimingLists lists = can_setup_snap(iface.info, t);
        const auto refused = [](bool given, unsigned want, unsigned got, std::string_view key, const std::vector<unsigned>& v)
        {
            return given && want != got ? std::format("{} {} not offered by the interface: {}", key, want, uint_list(v)) : "";
        };
        for (std::string e : {refused(b, asked.bitrate, t.bitrate, "bitrate", lists.bitrates),
                              refused(sp, asked.sample_point, t.sample_point, "sample_point", lists.sample_points),
                              refused(fb, asked.fd_bitrate, t.fd_bitrate, "fd_bitrate", lists.fd_bitrates),
                              refused(fsp, asked.fd_sample_point, t.fd_sample_point, "fd_sample_point", lists.fd_sample_points)})
        {
            if (!e.empty())
            {
                return e;
            }
        }
        if (fd_given && !fd)
        {
            t.fd_bitrate = 0; // ponytail: the setup dialog snaps it back on when it lists FD rates
            t.is_custom_fd_bitrate = false;
        }
        // As the dialog's CAN page: FD follows the data rate (snap already did that when it had lists).
        if (fd_given || fb || !lists.bitrates.empty() || body.find("\"is_custom_fd_bitrate\"") != std::string_view::npos)
        {
            t.can_fd = t.fd_bitrate > 0 || t.is_custom_fd_bitrate;
        }
        if (t.can_fd && !si.can_fd && (iface.info.capabilities & iface_cap::canfd) == 0)
        {
            return "can_fd: the interface has no CAN FD";
        }
    }
    si = t;
    return "";
}

// POST /interface/<name>/ldf {"path": "/abs/x.ldf"}: the LDF onto the interface's network, as
// Setup's "Add Database..." on a LIN network does (a file already there is replaced).
std::string ldf_json(App& app, const Iface& iface, SetupNetwork& net, SetupInterface& si, std::string_view body, int& status)
{
    const std::string path = json_get(body, "path");
    if (path.empty())
    {
        return error_json("path missing");
    }
    auto db = std::make_shared<LinDb>();
    if (!lin_db_load(*db, path))
    {
        return error_json(std::format("cannot load {}: {}", path, db->last_error));
    }
    std::erase_if(net.lin_dbs, [&](const auto& d) { return d->path == path; });
    for (SetupInterface& other : net.interfaces)
    {
        if (other.bus_type == BusType::LIN && (&other == &si || other.lin_ldf_path.empty()))
        {
            lin_setup_apply_ldf(*db, other);
        }
    }
    net.lin_dbs.push_back(std::move(db));
    setup_rebuild_cache(app.setup);
    status = 200;
    return interface_json(iface, si, net.name);
}

// POST /interface/<name>/link {"up": true}: SocketCAN link up / down on the link worker (pkexec
// may prompt there, never here on the main thread). 202: started, poll GET /status for "up".
std::string link_json(App& app, const Iface& iface, std::string_view body, int& status)
{
    bool up = true;
    Fields f{body};
    if (!flag(f, "up", up))
    {
        return error_json(f.error.empty() ? "up missing" : f.error);
    }
    if (iface.ops == nullptr || std::string_view(iface.ops->name) != "SocketCAN")
    {
        return error_json("link up / down is for SocketCAN interfaces only");
    }
    if (iface.info.up == up)
    {
        status = 200;
        return std::format("{{\"name\":{},\"up\":{},\"changed\":false}}", json_string(iface.info.name), up);
    }
    if (app.can_status.link_busy)
    {
        status = 409;
        return error_json("another link command is running");
    }
    toggle_link(app, iface);
    status = 202;
    return std::format("{{\"name\":{},\"up\":{},\"pending\":true}}", json_string(iface.info.name), up);
}

// GET /can_status: the CAN Status window's counters per interface (since its last open).
std::string can_status_json(const App& app)
{
    std::string out;
    for (const Iface& i : app.ifaces)
    {
        const CanStatusRow row = i.index < app.can_status.rows.size() ? app.can_status.rows[i.index] : CanStatusRow{};
        const IfaceStats& s = row.stats;
        out += std::format("{}{{\"index\":{},\"name\":{},\"driver\":{},\"seen\":{},\"state\":{},\"rx_frames\":{},"
                           "\"rx_errors\":{},\"tx_frames\":{},\"tx_errors\":{},\"rx_overruns\":{},\"tx_dropped\":{},"
                           "\"bitrate\":{},\"data_bitrate\":{},\"load\":{},\"error_frames\":{}}}",
                           out.empty() ? "" : ",", i.index, json_string(i.info.name),
                           json_string(i.ops != nullptr ? i.ops->name : ""), row.seen, json_string(iface_state_name(s.state)),
                           s.rx_frames, s.rx_errors, s.tx_frames, s.tx_errors, s.rx_overruns, s.tx_dropped, s.bitrate,
                           s.data_bitrate, json_string(row.load), row.error_frames);
    }
    return std::format("{{\"interfaces\":[{}]}}", out);
}

} // namespace

std::optional<std::string> rest_setup_routes(App& app, std::string_view method, std::string_view path,
                                             std::string_view body, int& status)
{
    const bool post = method == "POST";
    if (method == "GET" && path == "/can_status")
    {
        status = 200;
        return can_status_json(app);
    }
    if (post && path == "/setup/reload")
    {
        if (app.measuring)
        {
            status = 409; // LinFrame* held by a running measurement do not survive a reload
            return error_json("stop the measurement first");
        }
        std::vector<std::string> errors;
        const bool ok = setup_reload_databases(app.setup, &errors);
        std::string list;
        for (const std::string& e : errors)
        {
            list += (list.empty() ? "" : ",") + json_string(e);
        }
        status = ok ? 200 : 400;
        return std::format("{{\"reloaded\":{},\"errors\":[{}]}}", ok, list);
    }
    constexpr std::string_view prefix = "/interface/";
    if (!path.starts_with(prefix) || (method != "GET" && !post))
    {
        return std::nullopt;
    }
    std::string_view name = path.substr(prefix.size());
    std::string_view sub;
    if (const std::size_t slash = name.find('/'); slash != std::string_view::npos)
    {
        sub = name.substr(slash + 1);
        name = name.substr(0, slash);
    }
    if ((sub != "" && sub != "ldf" && sub != "link") || (!post && sub != ""))
    {
        return std::nullopt;
    }
    status = 400;
    const auto it = std::ranges::find(app.ifaces, name, [](const Iface& i) { return i.info.name; });
    if (it == app.ifaces.end() || it->ops == nullptr)
    {
        status = 404;
        return error_json(std::format("no such interface \"{}\" (see /status)", name));
    }
    if (post && app.measuring)
    {
        status = 409;
        return error_json("stop the measurement first");
    }
    if (sub == "link")
    {
        return link_json(app, *it, body, status);
    }
    SetupInterface* si = setup_of(app.setup, *it);
    if (si == nullptr)
    {
        return error_json(std::format("\"{}\" is in no network: enable it first (/interface)", name));
    }
    SetupNetwork* owner = &*std::ranges::find_if(app.setup.networks, [&](const SetupNetwork& n)
    {
        return si >= n.interfaces.data() && si < n.interfaces.data() + n.interfaces.size();
    });
    if (sub == "ldf")
    {
        return ldf_json(app, *it, *owner, *si, body, status);
    }
    if (post)
    {
        if (std::string e = apply_fields(*it, *si, body); !e.empty())
        {
            return error_json(e);
        }
        setup_rebuild_cache(app.setup);
    }
    status = 200;
    return interface_json(*it, *si, owner->name);
}
