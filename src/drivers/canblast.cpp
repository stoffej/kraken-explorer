/*
  Copyright (c) 2022 Ethan Zonca
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

// CANblaster: CAN frames streamed over UDP by a CANblaster server.
//   discovery  server -> multicast 239.255.43.21:20000  {"protocol":"CANblaster","version":1}
//   frames     server -> client :20001                   struct can_frame / canfd_frame
//   heartbeat  client -> server :20002                   "Heartbeat" once per second

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/socket_can.h"
#include "drivers/driver.h"

bool canblast_enabled = false;

// Discovery datagram: exactly {"protocol":"CANblaster","version":1}, either key order, any
// whitespace. Not in the anonymous namespace so tests/canblast can call it directly.
bool is_discovery(const char* data, std::size_t size)
{
    static const std::regex re(R"(\s*\{\s*(?:"protocol"\s*:\s*"CANblaster"\s*,\s*"version"\s*:\s*1)"
                               R"(|"version"\s*:\s*1\s*,\s*"protocol"\s*:\s*"CANblaster")\s*\}\s*)");
    return std::regex_match(data, data + size, re);
}

namespace
{

using Clock = std::chrono::steady_clock;

constexpr uint16_t discovery_port = 20000;
constexpr uint16_t frame_port = 20001;
constexpr uint16_t heartbeat_port = 20002;
constexpr const char* discovery_group = "239.255.43.21";
constexpr auto discovery_time = std::chrono::seconds(2);
constexpr auto heartbeat_interval = std::chrono::seconds(1);

struct CanBlast
{
    int fd = -1;
    sockaddr_in server{};
    Clock::time_point last_heartbeat{};
    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> rx_errors{0};
    std::atomic<uint64_t> tx_dropped{0};
};

// UDP socket bound to INADDR_ANY:port, -1 on failure.
int bind_udp(uint16_t port, bool share)
{
    const int fd = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        return -1;
    }
    if (share)
    {
        const int one = 1;
        net_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        net_close(fd);
        return -1;
    }
    return fd;
}

void canblast_enumerate(std::vector<IfaceInfo>& out)
{
    if (!canblast_enabled)
    {
        return;
    }
    const int fd = bind_udp(discovery_port, true);
    if (fd < 0)
    {
        log_warning(std::format("CANblaster: cannot listen on UDP port {}", discovery_port));
        return;
    }
    ip_mreq group{};
    inet_pton(AF_INET, discovery_group, &group.imr_multiaddr);
    group.imr_interface.s_addr = htonl(INADDR_ANY);
    net_setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group));

    // ponytail: blocks the main thread for the whole discovery window, as the Qt driver did; a worker if the freeze at Reload bothers.
    std::vector<std::string> servers;
    const auto deadline = Clock::now() + discovery_time;
    for (auto now = Clock::now(); now < deadline; now = Clock::now())
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (net_wait_readable(fd, static_cast<int>(left) + 1) <= 0)
        {
            continue;
        }
        std::array<char, 1024> buf{};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const auto n = net_recvfrom(fd, buf.data(), buf.size(), false, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0)
        {
            continue;
        }
        if (!is_discovery(buf.data(), static_cast<std::size_t>(n)))
        {
            log_warning("CANblaster: ignoring invalid discovery datagram");
            continue;
        }
        std::array<char, INET_ADDRSTRLEN> ip{};
        inet_ntop(AF_INET, &from.sin_addr, ip.data(), ip.size());
        if (std::ranges::find(servers, ip.data()) == servers.end())
        {
            servers.emplace_back(ip.data());
        }
    }
    net_close(fd);
    log_info(std::format("CANblaster: found {} server(s)", servers.size()));

    std::vector<CanTiming> bitrates;
    for (unsigned br : {10000U, 20000U, 50000U, 83333U, 100000U, 125000U, 250000U, 500000U, 800000U, 1000000U})
    {
        for (unsigned br_fd : {0U, 2000000U, 5000000U})
        {
            bitrates.push_back({.bitrate = br, .bitrate_fd = br_fd});
        }
    }
    for (auto& s : servers)
    {
        out.push_back({
            .name = s,
            .details = std::format("CANblaster server at {}", s),
            .version = "1",
            .bus_type = BusType::CAN,
            .capabilities = iface_cap::config_os | iface_cap::listen_only | iface_cap::auto_restart,
            .bitrates = bitrates,
        });
    }
}

bool canblast_open(Iface& iface, const IfaceConfig&)
{
    auto s = std::make_unique<CanBlast>();
    s->server.sin_family = AF_INET;
    s->server.sin_port = htons(heartbeat_port);
    if (inet_pton(AF_INET, iface.info.name.c_str(), &s->server.sin_addr) != 1)
    {
        log_error(std::format("CANblaster: invalid server address {}", iface.info.name));
        return false;
    }
    // ponytail: one socket per interface on a fixed port, so only one CANblaster server
    // can be open at a time (as before); demux on the sender address if that matters
    s->fd = bind_udp(frame_port, false);
    if (s->fd < 0)
    {
        log_error(std::format("CANblaster: cannot bind UDP port {}", frame_port));
        return false;
    }
    iface_set_impl(iface, std::move(s));
    return true;
}

void canblast_close(Iface& iface)
{
    if (iface.impl)
    {
        net_close(iface_impl<CanBlast>(iface).fd);
    }
    iface.impl.reset();
}

// The CANblaster protocol has no TX direction.
bool canblast_send(Iface& iface, const BusMessage&)
{
    ++iface_impl<CanBlast>(iface).tx_dropped;
    return false;
}

void heartbeat(CanBlast& s)
{
    const auto now = Clock::now();
    if (now - s.last_heartbeat < heartbeat_interval)
    {
        return;
    }
    s.last_heartbeat = now;
    constexpr char msg[] = "Heartbeat";
    ::sendto(s.fd, msg, sizeof(msg) - 1, 0, reinterpret_cast<const sockaddr*>(&s.server), sizeof(s.server));
}

int canblast_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    auto& s = iface_impl<CanBlast>(iface);
    heartbeat(s);

    const int rv = net_wait_readable(s.fd, std::min(timeout_ms, 1000));
    if (rv <= 0)
    {
        return rv == 0 ? 0 : -1;
    }

    int n = 0;
    while (n < max)
    {
        std::array<uint8_t, socket_can::canfd_mtu> buf{};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const auto nbytes = net_recvfrom(s.fd, buf.data(), buf.size(), true, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (nbytes < 0)
        {
            break; // EAGAIN; a UDP socket has no hang-up to report
        }
        const auto size = static_cast<std::size_t>(nbytes);
        if (from.sin_addr.s_addr != s.server.sin_addr.s_addr
            || (size != socket_can::can_mtu && size != socket_can::canfd_mtu))
        {
            ++s.rx_errors;
            continue;
        }

        BusMessage& m = out[n++];
        m = BusMessage{.iface = iface.index};
        m.ts_ns = now_ns();
        socket_can::decode_frame(buf.data(), size, m); // host byte order, as the Qt driver read it
        ++s.rx_frames;
    }
    return n;
}

void canblast_stats(Iface& iface, IfaceStats& out)
{
    const auto& s = iface_impl<CanBlast>(iface);
    out.state = IfaceState::Ok;
    out.rx_frames = s.rx_frames;
    out.rx_errors = s.rx_errors;
    out.tx_dropped = s.tx_dropped;
}

} // namespace

extern const DriverOps canblast_driver = {
    .name = "CANblaster",
    .enumerate = canblast_enumerate,
    .open = canblast_open,
    .close = canblast_close,
    .send = canblast_send,
    .read = canblast_read,
    .stats = canblast_stats,
};
