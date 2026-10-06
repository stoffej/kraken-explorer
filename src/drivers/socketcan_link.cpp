/*
  Copyright (c) 2015, 2016 Hubert Denkmair
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

// The parts of SocketCAN link control that only build command lines and classify results
// (driver.h): portable, so the CAN Status window and its tests build everywhere. Windows has no
// SocketCAN: there the calls that would run `ip` report failure and no link exists.

#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "drivers/driver.h"

namespace
{

// First existing absolute path: pkexec's polkit rule (packaging/) matches on it.
std::string ip_executable()
{
    std::error_code ec;
    for (const char* p : {"/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", "/bin/ip"})
    {
        if (std::filesystem::exists(p, ec))
        {
            return p;
        }
    }
    return "ip";
}

std::string sample_point_arg(unsigned per_mille)
{
    return std::format("{:.3f}", per_mille / 1000.0);
}

// `ip link set <name> up type can ...` with the bit timing of c (ip-link(8) CAN syntax).
std::vector<std::string> can_up_args(const std::string& name, const IfaceConfig& c)
{
    std::vector<std::string> up = {"link", "set", name, "up", "type", "can",
                                   "bitrate", std::to_string(c.bitrate), "sample-point", sample_point_arg(c.sample_point)};
    if (c.can_fd)
    {
        up.insert(up.end(), {"dbitrate", std::to_string(c.fd_bitrate), "dsample-point",
                             sample_point_arg(c.fd_sample_point), "fd", "on"});
    }
    // Always explicit: the kernel keeps a ctrlmode flag until told otherwise (auto-baud sets it).
    up.insert(up.end(), {"listen-only", c.listen_only ? "on" : "off",
                         "restart-ms", c.auto_restart ? std::to_string(c.auto_restart_ms) : "0"});
    return up;
}

} // namespace

std::vector<std::string> ip_link_args(LinkOp op, const std::string& name, const IfaceConfig* timing)
{
    switch (op)
    {
    case LinkOp::Up:
        if (timing)
        {
            return can_up_args(name, *timing);
        }
        return {"link", "set", name, "up"};
    case LinkOp::Down:
        return {"link", "set", name, "down"};
    case LinkOp::AddVcan:
        return {"link", "add", "dev", name, "up", "type", "vcan"}; // up at once: a new vcan is otherwise created down
    case LinkOp::Delete:
        return {"link", "delete", name};
    }
    return {};
}

std::vector<std::string> ip_command(const std::vector<std::string>& args, bool root)
{
    std::vector<std::string> cmd;
    if (!root)
    {
        cmd.emplace_back("pkexec");
    }
    cmd.push_back(ip_executable());
    cmd.insert(cmd.end(), args.begin(), args.end());
    return cmd;
}

std::string next_vcan_name(bool (*taken)(const std::string& name))
{
    for (unsigned n = 0;; ++n)
    {
        std::string name = std::format("vcan{}", n);
        if (!taken(name))
        {
            return name;
        }
    }
}

IpResult ip_result_classify(int exit_code, std::string_view err) noexcept
{
    if (exit_code == 0)
    {
        return IpResult::ok;
    }
    if (exit_code == 126)
    {
        return IpResult::denied;
    }
    if (exit_code == 127 && err.find("authentication agent") != std::string_view::npos)
    {
        return IpResult::no_agent;
    }
    return IpResult::failed;
}

bool autobaud_hit(const BaudProbe& p) noexcept
{
    return p.frames >= 2 && p.errors == 0;
}

#ifdef _WIN32
IpResult socketcan_run_ip(const std::vector<std::string>&)
{
    return IpResult::failed;
}

AutobaudResult socketcan_autobaud(const std::string&, IfaceConfig)
{
    return {.bitrate = std::nullopt, .ip = IpResult::failed};
}

bool socketcan_link_up(const std::string&)
{
    return false;
}

bool socketcan_link_exists(const std::string&)
{
    return false;
}
#endif
