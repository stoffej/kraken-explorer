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

// SocketCAN: raw CAN sockets for I/O, libnl for enumeration and statistics, `ip link`
// (through pkexec when not root) for the bit timing.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/socket_can.h"
#include "drivers/driver.h"

#include <net/if.h> // before any <linux/if.h>, see <linux/libc-compat.h>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/if_arp.h>
#include <netlink/route/link.h>
#include <netlink/version.h>
#include <netlink/route/link/can.h>

extern char** environ;

namespace
{

struct SocketCan
{
    int fd = -1;
    int ifindex = 0;
    bool canfd = false;
    std::atomic<uint32_t> socket_drops{0}; // SO_RXQ_OVFL: frames the socket buffer dropped
};

// One libnl route socket + link cache for the duration of a call.
struct NlLinks
{
    nl_sock* sock = nullptr;
    nl_cache* cache = nullptr;
};

bool nl_links_open(NlLinks& nl)
{
    nl.sock = nl_socket_alloc();
    if (!nl.sock || nl_connect(nl.sock, NETLINK_ROUTE) < 0)
    {
        return false;
    }
    const int rc = rtnl_link_alloc_cache(nl.sock, AF_UNSPEC, &nl.cache);
    if (rc < 0)
    {
        log_error(std::format("Could not access netlink device list: {}", rc));
        return false;
    }
    return true;
}

void nl_links_close(NlLinks& nl)
{
    if (nl.cache)
    {
        nl_cache_free(nl.cache);
    }
    if (nl.sock)
    {
        nl_close(nl.sock);
        nl_socket_free(nl.sock);
    }
}

bool link_is_vcan(rtnl_link* link)
{
    const char* type = rtnl_link_get_type(link);
    return type && std::strcmp(type, "vcan") == 0;
}

constexpr unsigned classic_bitrates[] = {10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000};

std::vector<CanTiming> socketcan_bitrates(bool canfd)
{
    constexpr unsigned sample_points[] = {500, 625, 750, 875};
    constexpr unsigned fd_bitrates[] = {500000, 1000000, 2000000, 4000000, 5000000, 8000000};
    std::vector<CanTiming> out;
    for (unsigned br : classic_bitrates)
    {
        for (unsigned sp : sample_points)
        {
            out.push_back({.bitrate = br, .sample_point = sp});
        }
    }
    if (canfd)
    {
        for (unsigned br : classic_bitrates)
        {
            for (unsigned fdbr : fd_bitrates)
            {
                out.push_back({.bitrate = br, .bitrate_fd = fdbr, .sample_point = 800, .sample_point_fd = 800});
            }
        }
    }
    return out;
}

void socketcan_enumerate(std::vector<IfaceInfo>& out)
{
    utsname uts{};
    const std::string version = uname(&uts) == 0 ? uts.release : "";

    NlLinks nl;
    if (nl_links_open(nl))
    {
        for (nl_object* obj = nl_cache_get_first(nl.cache); obj; obj = nl_cache_get_next(obj))
        {
            auto* link = reinterpret_cast<rtnl_link*>(obj);
            if (rtnl_link_get_arptype(link) != ARPHRD_CAN)
            {
                continue;
            }
            const bool vcan = link_is_vcan(link);
            const bool canfd = vcan || rtnl_link_get_mtu(link) >= CANFD_MTU; // CAN XL links (mtu 2060) do FD too
            uint32_t caps = iface_cap::config_os | iface_cap::listen_only | iface_cap::auto_restart;
            if (canfd)
            {
                caps |= iface_cap::canfd;
            }
            const char* type = rtnl_link_get_type(link);
            out.push_back({
                .name = rtnl_link_get_name(link),
                .details = type ? type : "can",
                .version = version,
                .capabilities = caps,
                .bitrates = socketcan_bitrates(canfd),
                .up = (rtnl_link_get_flags(link) & IFF_UP) != 0,
            });
        }
    }
    nl_links_close(nl);
}

} // namespace

// ponytail: at Start (socketcan_configure) this blocks the main thread while pkexec asks for
// a password, as the Qt version did; the GUI link buttons run it on a worker thread. Run Start on link_worker too if the freeze matters.
IpResult socketcan_run_ip(const std::vector<std::string>& args)
{
    std::vector<std::string> cmd = ip_command(args, geteuid() == 0);

    std::string line;
    std::vector<char*> argv;
    for (auto& s : cmd)
    {
        argv.push_back(s.data());
        line += (line.empty() ? "" : " ") + s;
    }
    argv.push_back(nullptr);
    log_info(line);

    int err_pipe[2];
    if (pipe2(err_pipe, O_CLOEXEC) != 0)
    {
        log_error(std::format("ip command failed: pipe: {}", std::strerror(errno)));
        return IpResult::failed;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(err_pipe[1]);
    if (rc != 0)
    {
        close(err_pipe[0]);
        log_error(std::format("ip command failed: cannot start {}: {}", argv[0], std::strerror(rc)));
        return IpResult::failed;
    }

    std::string err;
    char buf[256];
    for (;;)
    {
        const ssize_t n = read(err_pipe[0], buf, sizeof(buf));
        if (n > 0)
        {
            err.append(buf, static_cast<std::size_t>(n));
        }
        else if (n == 0 || errno != EINTR)
        {
            break;
        }
    }
    close(err_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    const IpResult result = ip_result_classify(WIFEXITED(status) ? WEXITSTATUS(status) : -1, err);
    if (result != IpResult::ok)
    {
        while (!err.empty() && std::isspace(static_cast<unsigned char>(err.back())))
        {
            err.pop_back();
        }
        log_error(std::format("ip command failed: {}", err));
    }
    return result;
}

namespace
{

// `ip link set down`, then up with the bit timing of c.
void socketcan_set_timing(const std::string& name, const IfaceConfig& c)
{
    socketcan_run_ip({"link", "set", name, "down"});
    socketcan_run_ip(ip_link_args(LinkOp::Up, name, &c));
}

void socketcan_configure(const std::string& name, const IfaceConfig& c)
{
    if (!c.configure)
    {
        log_info(std::format("interface {} not managed by Kraken Explorer, not touching configuration", name));
        return;
    }
    log_info(std::format("reconfiguring interface {}", name));
    socketcan_set_timing(name, c);
}

// Raw CAN socket bound to ifindex with error frames enabled; -1 (logged) when that fails.
int open_raw(const std::string& name, int ifindex)
{
    const int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0)
    {
        log_error(std::format("SocketCAN: error while opening socket: {}", std::strerror(errno)));
        return -1;
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifindex;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        log_error(std::format("SocketCAN: error in socket bind for {}: {}", name, std::strerror(errno)));
        close(fd);
        return -1;
    }
    // The default error filter is 0 (no error frames at all); an analyzer must see them.
    const can_err_mask_t err_mask = CAN_ERR_MASK;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask)) != 0)
    {
        log_warning(std::format("SocketCAN: could not enable error frames for {}: {}", name, std::strerror(errno)));
    }
    return fd;
}

bool socketcan_open(Iface& iface, const IfaceConfig& config)
{
    const std::string& name = iface.info.name;
    // vcan has no bit timing: `ip link ... type can` would only fail there.
    if (iface.info.details != "vcan")
    {
        socketcan_configure(name, config);
    }
    // A socket bound to a down link only reports ENETDOWN: not managing the link (or reopening
    // after it went down), stay closed until it is up. Quiet: the listener retries every second.
    if (!config.configure && !socketcan_link_up(name))
    {
        return false;
    }

    const int ifindex = static_cast<int>(if_nametoindex(name.c_str()));
    if (ifindex == 0)
    {
        log_error(std::format("SocketCAN: no interface {}: {}", name, std::strerror(errno)));
        return false;
    }
    const int fd = open_raw(name, ifindex);
    if (fd < 0)
    {
        return false;
    }

    const int on = 1;
    const bool canfd = setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) == 0;
    if (!canfd)
    {
        log_warning(std::format("SocketCAN: could not enable CAN FD for {}: {}", name, std::strerror(errno)));
    }
    // Own frames come back flagged MSG_CONFIRM with the kernel TX timestamp: that is the
    // TX report, so send() needs no queue of its own.
    setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
    // The default ~200 kB holds only a few hundred frames (skb truesize ~800 B each), so a
    // listener descheduled for a millisecond drops frames under a flood. FORCE needs
    // CAP_NET_ADMIN; plain SO_RCVBUF is capped at net.core.rmem_max.
    const int rcvbuf = 32 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) != 0)
    {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    auto s = std::make_unique<SocketCan>();
    s->fd = fd;
    s->ifindex = ifindex;
    s->canfd = canfd && (iface.info.capabilities & iface_cap::canfd);
    iface_set_impl(iface, std::move(s));
    return true;
}

void socketcan_close(Iface& iface)
{
    if (auto* s = static_cast<SocketCan*>(iface.impl.get()); s && s->fd >= 0)
    {
        close(s->fd);
        s->fd = -1;
    }
}

bool socketcan_send(Iface& iface, const BusMessage& msg)
{
    auto& s = iface_impl<SocketCan>(iface);
    canfd_frame frame{};
    frame.can_id = socket_can::frame_id(msg);
    std::size_t size = CAN_MTU;
    if (has_flag(msg, bus_flag::fd) || (s.canfd && msg.len > CAN_MAX_DLEN))
    {
        size = CANFD_MTU;
        frame.len = msg.len;
        if (has_flag(msg, bus_flag::brs))
        {
            frame.flags |= CANFD_BRS;
        }
    }
    else
    {
        frame.len = std::min<uint8_t>(msg.len, CAN_MAX_DLEN);
    }
    if (is_error_frame(msg))
    {
        const auto e = socket_can::error_frame(msg);
        frame.len = socket_can::err_dlc;
        std::copy(e.data.begin(), e.data.end(), frame.data);
    }
    else if (!has_flag(msg, bus_flag::rtr))
    {
        std::copy_n(msg.data.begin(), frame.len, frame.data);
    }
    if (write(s.fd, &frame, size) < 0)
    {
        log_error(std::format("SocketCAN: error writing frame to {}: {}", iface.info.name, std::strerror(errno)));
        return false;
    }
    return true;
}

// <linux/can/error.h> error classes + payload -> bus_error bits.
uint16_t decode_error(uint32_t classes, const uint8_t* data)
{
    uint16_t e = 0;
    if (classes & CAN_ERR_TX_TIMEOUT) { e |= bus_error::tx_timeout; }
    if (classes & CAN_ERR_ACK)        { e |= bus_error::ack; }
    if (classes & CAN_ERR_BUSOFF)     { e |= bus_error::bus_off; }
    if (classes & CAN_ERR_RESTARTED)  { e |= bus_error::restarted; }
    if (classes & CAN_ERR_CRTL)
    {
        if (data[1] & 0x03) { e |= bus_error::overrun; }
        if (data[1] & 0x0C) { e |= bus_error::error_warning; }
        if (data[1] & 0x30) { e |= bus_error::error_passive; }
        if (data[1] & 0x40) { e |= bus_error::error_active; }
    }
    if (classes & CAN_ERR_PROT)
    {
        if (data[2] & 0x19) { e |= bus_error::bit; }
        if (data[2] & 0x02) { e |= bus_error::form; }
        if (data[2] & 0x04) { e |= bus_error::stuff; }
        if (data[3] == 0x08) { e |= bus_error::crc; }
    }
    return e ? e : bus_error::generic;
}

int socketcan_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    auto& s = iface_impl<SocketCan>(iface);
    pollfd pfd{.fd = s.fd, .events = POLLIN, .revents = 0};
    const int rv = poll(&pfd, 1, timeout_ms);
    if (rv < 0)
    {
        return errno == EINTR ? 0 : -1;
    }
    if (rv == 0)
    {
        return 0;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
        return -1; // e.g. the netdev went away
    }

    // One recvmmsg drains the whole batch: a syscall per frame was the RX thread's main cost.
    constexpr int batch = 256;
    constexpr size_t ctrl_len = CMSG_SPACE(sizeof(timespec)) + CMSG_SPACE(sizeof(uint32_t));
    canfd_frame frames[batch];
    iovec iovs[batch];
    alignas(cmsghdr) char ctrls[batch][ctrl_len];
    mmsghdr mhs[batch];
    const int want = std::min(max, batch);
    for (int k = 0; k < want; ++k)
    {
        iovs[k] = iovec{.iov_base = &frames[k], .iov_len = sizeof(canfd_frame)};
        mhs[k] = mmsghdr{};
        mhs[k].msg_hdr.msg_iov = &iovs[k];
        mhs[k].msg_hdr.msg_iovlen = 1;
        mhs[k].msg_hdr.msg_control = ctrls[k];
        mhs[k].msg_hdr.msg_controllen = ctrl_len;
    }
    const int got = recvmmsg(s.fd, mhs, static_cast<unsigned>(want), MSG_DONTWAIT, nullptr);
    if (got < 0)
    {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    }

    int n = 0;
    for (int k = 0; k < got; ++k)
    {
        const canfd_frame& frame = frames[k];
        msghdr& mh = mhs[k].msg_hdr;
        const auto nbytes = static_cast<ssize_t>(mhs[k].msg_len);
        if (nbytes != CAN_MTU && nbytes != CANFD_MTU)
        {
            continue;
        }

        BusMessage& m = out[n++];
        m = BusMessage{.iface = iface.index};
        for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        {
            if (c->cmsg_level != SOL_SOCKET)
            {
                continue;
            }
            if (c->cmsg_type == SCM_TIMESTAMPNS)
            {
                timespec ts{};
                std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
                m.ts_ns = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
            }
            else if (c->cmsg_type == SO_RXQ_OVFL)
            {
                uint32_t drops = 0;
                std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
                s.socket_drops.store(drops, std::memory_order_relaxed);
            }
        }
        if (m.ts_ns == 0)
        {
            m.ts_ns = now_ns();
        }
        if (mh.msg_flags & MSG_CONFIRM)
        {
            m.flags |= bus_flag::tx;
        }
        const uint32_t can_id =
            socket_can::decode_frame(reinterpret_cast<const uint8_t*>(&frame), static_cast<size_t>(nbytes), m);
        if (can_id & CAN_ERR_FLAG)
        {
            // The id carries error classes, not a CAN id: don't show or decode it as one.
            m.errors = decode_error(can_id & CAN_ERR_MASK, frame.data);
        }
    }
    return n;
}

IfaceState state_from_can(uint32_t state)
{
    switch (state)
    {
    case CAN_STATE_ERROR_ACTIVE:
        return IfaceState::Ok;
    case CAN_STATE_ERROR_WARNING:
        return IfaceState::Warning;
    case CAN_STATE_ERROR_PASSIVE:
        return IfaceState::Passive;
    case CAN_STATE_BUS_OFF:
        return IfaceState::BusOff;
    case CAN_STATE_STOPPED:
        return IfaceState::Stopped;
    default:
        return IfaceState::Unknown;
    }
}

void socketcan_stats(Iface& iface, IfaceStats& out)
{
    auto& s = iface_impl<SocketCan>(iface);
    out = {};
    out.rx_overruns = s.socket_drops.load(std::memory_order_relaxed);
    NlLinks nl;
    rtnl_link* link = nullptr;
    if (nl_links_open(nl) && rtnl_link_get_kernel(nl.sock, s.ifindex, nullptr, &link) == 0)
    {
        out.rx_frames = rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS);
        out.rx_overruns += rtnl_link_get_stat(link, RTNL_LINK_RX_OVER_ERR);
        out.tx_frames = rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS);
        out.tx_dropped = rtnl_link_get_stat(link, RTNL_LINK_TX_DROPPED);
        if (rtnl_link_is_can(link))
        {
            uint32_t state = 0;
            if (rtnl_link_can_state(link, &state) == 0)
            {
                out.state = state_from_can(state);
            }
            out.rx_errors = static_cast<uint64_t>(std::max(0, rtnl_link_can_berr_rx(link)));
            out.tx_errors = static_cast<uint64_t>(std::max(0, rtnl_link_can_berr_tx(link)));
            // Already in the link dump: no extra syscall. Fails (stays 0) without bit timing.
            rtnl_link_can_get_bitrate(link, &out.bitrate);
#if LIBNL_VER_NUM >= LIBNL_VER(3, 7)
            // ponytail: libnl < 3.7 (Ubuntu 22.04: 3.5) has no data bittiming getter, FD data rate
            // stays unknown there; parse IFLA_CAN_DATA_BITTIMING ourselves if that ever matters.
            uint32_t ctrlmode = 0;
            can_bittiming data{};
            if (rtnl_link_can_get_ctrlmode(link, &ctrlmode) == 0 && (ctrlmode & CAN_CTRLMODE_FD) &&
                rtnl_link_can_get_data_bittiming(link, &data) == 0)
            {
                out.data_bitrate = data.bitrate;
            }
#endif
        }
        else if (link_is_vcan(link))
        {
            out.state = IfaceState::Ok;
        }
        rtnl_link_put(link);
    }
    nl_links_close(nl);
}

// ponytail: fixed listen window per rate; a bus slower than ~2 frames per 500 ms reads as
// idle. Make it longer (or a setting) if sparse buses matter.
constexpr auto autobaud_window = std::chrono::milliseconds(500);

// ip: set when an `ip` call failed for a reason other than the controller refusing bitrate.
BaudProbe autobaud_probe(const std::string& name, unsigned bitrate, IpResult& ip)
{
    BaudProbe p;
    ip = socketcan_run_ip({"link", "set", name, "down"});
    if (ip != IpResult::ok)
    {
        return p;
    }
    // listen-only: never ACKs, never disturbs the bus
    const IpResult up = socketcan_run_ip({"link", "set", name, "up", "type", "can", "bitrate", std::to_string(bitrate), "listen-only", "on"});
    if (up != IpResult::ok)
    {
        ip = up == IpResult::failed ? IpResult::ok : up; // plain failure: bitrate refused, a miss
        return p;
    }
    const int fd = open_raw(name, static_cast<int>(if_nametoindex(name.c_str())));
    if (fd < 0)
    {
        return p;
    }
    const auto end = std::chrono::steady_clock::now() + autobaud_window;
    for (auto now = std::chrono::steady_clock::now(); now < end; now = std::chrono::steady_clock::now())
    {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(end - now);
        can_frame f{};
        if (::poll(&pfd, 1, static_cast<int>(left.count())) > 0 && ::read(fd, &f, sizeof(f)) == sizeof(f))
        {
            ++((f.can_id & CAN_ERR_FLAG) ? p.errors : p.frames);
        }
    }
    close(fd);
    log_info(std::format("auto-baud {} at {} bit/s: {} frames, {} error frames", name, bitrate, p.frames, p.errors));
    return p;
}

} // namespace

// every `ip` call goes through pkexec when not root; without the netdev polkit
// rule (packaging/) that is a password prompt per call, up to ~2 per rate.
AutobaudResult socketcan_autobaud(const std::string& name, IfaceConfig timing)
{
    log_info(std::format("auto-baud {}: scanning", name));
    for (auto it = std::rbegin(classic_bitrates); it != std::rend(classic_bitrates); ++it)
    {
        IpResult ip = IpResult::ok;
        const BaudProbe p = autobaud_probe(name, *it, ip);
        if (ip != IpResult::ok)
        {
            log_error(std::format("auto-baud {}: stopped, ip failed", name));
            return {.ip = ip};
        }
        if (autobaud_hit(p))
        {
            log_info(std::format("auto-baud {}: {} bit/s", name, *it));
            timing.bitrate = *it;
            timing.listen_only = false; // readable traffic: the link comes up active at that rate
            socketcan_set_timing(name, timing);
            return {.bitrate = *it};
        }
    }
    log_info(std::format("auto-baud {}: no traffic or no matching bitrate", name));
    socketcan_run_ip({"link", "set", name, "down"});
    std::vector<std::string> restore = ip_link_args(LinkOp::Up, name, &timing);
    restore.erase(restore.begin() + 3); // drop "up": restore the timing, leave the link down
    socketcan_run_ip(restore);
    return {};
}

bool socketcan_link_up(const std::string& name)
{
    std::ifstream f("/sys/class/net/" + name + "/flags");
    unsigned flags = 0;
    return f >> std::hex >> flags && (flags & IFF_UP) != 0;
}

bool socketcan_link_exists(const std::string& name)
{
    std::error_code ec;
    return std::filesystem::exists("/sys/class/net/" + name, ec);
}

extern const DriverOps socketcan_driver = {
    .name = "SocketCAN",
    .enumerate = socketcan_enumerate,
    .open = socketcan_open,
    .close = socketcan_close,
    .send = socketcan_send,
    .read = socketcan_read,
    .stats = socketcan_stats,
};
