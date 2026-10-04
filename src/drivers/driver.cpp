/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "drivers/driver.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <thread>
#include <mutex>

#include "core/log.h"

extern const DriverOps socketcan_driver;
extern const DriverOps slcan_driver;
extern const DriverOps grip_driver;
extern const DriverOps canblast_driver;
extern const DriverOps linde_driver;
extern const DriverOps kvaser_driver;
#ifdef _WIN32
extern const DriverOps pcan_driver;
#endif

namespace
{

constexpr const DriverOps* drivers[] = {
#ifndef _WIN32
    &socketcan_driver,
#endif
    &slcan_driver,
    &grip_driver,
    &canblast_driver,
    &linde_driver,
    &kvaser_driver,
#ifdef _WIN32
    &pcan_driver,
#endif
};

constexpr int listener_batch = 256;
constexpr int listener_timeout_ms = 100; // bounds the stop latency

// Releases the channel: main thread after the listener stopped, or the listener itself
// after a read error. send() waits on the same mutex, so it never sees a half-closed channel.
void iface_close(Iface& iface)
{
    std::unique_lock lock(iface.io_mutex);
    if (iface.open)
    {
        iface.ops->close(iface);
        iface.impl.reset();
        iface.open = false;
    }
}

void listen(std::stop_token stop, Iface& iface, IfaceConfig config, std::span<const RxConsumer> consumers, void (*wake)())
{
    std::array<BusMessage, listener_batch> buf;
    config.configure = false; // a reopen must not run `ip link set ...` (pkexec) again
    while (!stop.stop_requested())
    {
        const int n = iface.ops->read(iface, buf.data(), listener_batch, listener_timeout_ms);
        if (n < 0)
        {
            // Link down or device gone: closed now, opened again once it is back (tried every
            // second) like a fresh start, so the counters restart from zero. CAN Status shows
            // "stopped" meanwhile. ponytail: a driver whose open() logs a failure (SLCAN with
            // the tty unplugged) logs a line per try; give DriverOps a probe if that gets noisy.
            log_warning(std::format("Interface {} lost (link down or device gone), reopening when it is back", iface.info.name));
            iface.failed = true;
            iface_close(iface);
            while (!iface.open && !stop.stop_requested())
            {
                for (int i = 0; i < 10 && !stop.stop_requested(); ++i) // 1 s, stop latency 100 ms
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(listener_timeout_ms));
                }
                std::unique_lock lock(iface.io_mutex);
                iface.open = !stop.stop_requested() && iface.ops->open(iface, config);
            }
            if (!iface.open)
            {
                return; // stopped while waiting
            }
            iface.total_bits = 0;
            iface.failed = false;
            log_info(std::format("Interface {} is back, listening again", iface.info.name));
            continue;
        }
        if (n == 0)
        {
            continue;
        }
        const std::span<const BusMessage> msgs(buf.data(), static_cast<std::size_t>(n));
        uint64_t bits = 0;
        for (const auto& m : msgs)
        {
            bits += bus_frame_bits(m);
        }
        iface.total_bits.fetch_add(bits, std::memory_order_relaxed);
        rx_deliver(iface.inbox, consumers, msgs, wake);
    }
}

} // namespace

const char* iface_state_name(IfaceState s) noexcept
{
    switch (s)
    {
    case IfaceState::Ok:
        return "ok";
    case IfaceState::Warning:
        return "warning";
    case IfaceState::Passive:
        return "error passive";
    case IfaceState::BusOff:
        return "bus off";
    case IfaceState::Stopped:
        return "stopped";
    case IfaceState::Unknown:
        break;
    }
    return "unknown";
}

std::span<const DriverOps* const> all_drivers() noexcept
{
    return drivers;
}

uint32_t bus_frame_bits(const BusMessage& m) noexcept
{
    if (m.type == BusType::LIN)
    {
        // Break(14) + Sync(10) + PID(10) + data bytes (10 each) + checksum(10)
        return 44 + static_cast<uint32_t>(m.len) * 10;
    }
    uint32_t bits = 47 + static_cast<uint32_t>(m.len) * 8;
    if (has_flag(m, bus_flag::extended))
    {
        bits += 18 + 2;
    }
    if (has_flag(m, bus_flag::fd))
    {
        bits += 20;
    }
    return bits + bits / 5; // approximate bit stuffing
}

int ifaces_find(const std::deque<Iface>& ifaces, std::string_view driver, std::string_view name)
{
    for (std::size_t i = 0; i < ifaces.size(); ++i)
    {
        if (ifaces[i].ops->name == driver && ifaces[i].info.name == name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ifaces_enumerate(std::deque<Iface>& ifaces)
{
    std::vector<IfaceInfo> found;
    for (const DriverOps* ops : all_drivers())
    {
        found.clear();
        ops->enumerate(found);
        for (auto& info : found)
        {
            const int idx = ifaces_find(ifaces, ops->name, info.name);
            if (idx >= 0)
            {
                ifaces[static_cast<std::size_t>(idx)].info = std::move(info);
                continue;
            }
            // channels that disappeared stay listed, so BusMessage::iface never dangles
            auto& i = ifaces.emplace_back();
            i.ops = ops;
            i.info = std::move(info);
            i.index = static_cast<uint16_t>(ifaces.size() - 1);
        }
    }
}

void ifaces_default_setup(const std::deque<Iface>& ifaces, Setup& setup)
{
    setup.networks.clear();
    for (const auto& i : ifaces)
    {
        if (!i.info.up)
        {
            continue; // a down can0 would only fail at Start (and ask pkexec to bring it up)
        }
        setup.networks.push_back({
            .name = std::format("Network {}", setup.networks.size() + 1),
            .interfaces = {{.driver = i.ops->name, .name = i.info.name, .bus_type = i.info.bus_type}},
        });
    }
    setup_rebuild_cache(setup);
}

int ifaces_start(std::deque<Iface>& ifaces, Setup& setup, std::span<const RxConsumer> consumers, void (*wake)())
{
    int opened = 0;
    for (auto& network : setup.networks)
    {
        for (auto& si : network.interfaces)
        {
            si.iface = ifaces_find(ifaces, si.driver, si.name);
            if (si.iface < 0)
            {
                log_warning(std::format("Interface {}/{} not found, not listening on it", si.driver, si.name));
                continue;
            }
            auto& iface = ifaces[static_cast<std::size_t>(si.iface)];
            if (!si.enabled || iface.listener.joinable())
            {
                continue;
            }
            {
                std::unique_lock lock(iface.io_mutex);
                iface.open = iface.ops->open(iface, si);
                if (!iface.open)
                {
                    log_error(std::format("Could not open interface {}", iface.info.name));
                    iface.failed = true; // not running: the status pill must not count it as up
                    continue;
                }
            }
            iface.failed = false;
            iface.total_bits = 0;
            log_info(std::format("Listening on interface #{}: {}, version: {}", iface.index, iface.info.name,
                                 iface.info.version));
            iface.listener = std::jthread(listen, std::ref(iface), si, consumers, wake);
            ++opened;
        }
    }
    return opened;
}

LinkCount ifaces_link_count(const std::deque<Iface>& ifaces, const Setup& setup)
{
    LinkCount c;
    for (const auto& network : setup.networks)
    {
        for (const auto& si : network.interfaces)
        {
            if (!si.enabled)
            {
                continue;
            }
            ++c.total;
            if (si.iface >= 0 && static_cast<std::size_t>(si.iface) < ifaces.size())
            {
                const auto& iface = ifaces[static_cast<std::size_t>(si.iface)];
                c.up += iface.open && !iface.failed.load() ? 1 : 0;
            }
        }
    }
    return c;
}

void ifaces_stop(std::deque<Iface>& ifaces)
{
    for (auto& i : ifaces)
    {
        i.listener.request_stop();
    }
    for (auto& i : ifaces)
    {
        if (!i.listener.joinable())
        {
            continue;
        }
        i.listener.join();
        log_info(std::format("Closing interface: {}", i.info.name));
        iface_close(i);
    }
}

std::optional<unsigned> iface_autobaud(Iface& iface, IfaceConfig config)
{
    // ponytail: fixed listen window per rate, as socketcan_autobaud: a bus slower than ~2 frames
    // per 500 ms reads as idle.
    constexpr auto window = std::chrono::milliseconds(500);
    if (iface.ops == nullptr || iface.info.bus_type != BusType::CAN || (iface.info.capabilities & iface_cap::listen_only) == 0)
    {
        log_warning(std::format("auto-baud {}: no listen-only mode, not probed", iface.info.name));
        return std::nullopt;
    }
    std::unique_lock lock(iface.io_mutex);
    if (iface.open)
    {
        return std::nullopt;
    }
    std::vector<unsigned> rates;
    for (const CanTiming& t : iface.info.bitrates)
    {
        rates.push_back(t.bitrate);
    }
    std::ranges::sort(rates, std::greater{});
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    config.listen_only = true;
    config.can_fd = false;
    config.is_custom_bitrate = false;
    log_info(std::format("auto-baud {}: scanning, listen-only", iface.info.name));
    std::array<BusMessage, listener_batch> buf;
    for (const unsigned rate : rates)
    {
        config.bitrate = rate;
        if (!iface.ops->open(iface, config))
        {
            iface.impl.reset();
            continue; // the driver refuses this rate: a miss
        }
        BaudProbe p;
        const auto end = std::chrono::steady_clock::now() + window;
        for (auto now = std::chrono::steady_clock::now(); now < end; now = std::chrono::steady_clock::now())
        {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(end - now);
            const int n = iface.ops->read(iface, buf.data(), static_cast<int>(buf.size()), static_cast<int>(left.count()));
            if (n < 0)
            {
                break;
            }
            for (int i = 0; i < n; ++i)
            {
                ++(buf[static_cast<std::size_t>(i)].errors != 0 ? p.errors : p.frames);
            }
        }
        iface.ops->close(iface);
        iface.impl.reset();
        log_info(std::format("auto-baud {} at {} bit/s: {} frames, {} error frames", iface.info.name, rate, p.frames, p.errors));
        if (autobaud_hit(p))
        {
            log_info(std::format("auto-baud {}: {} bit/s", iface.info.name, rate));
            return rate;
        }
    }
    log_info(std::format("auto-baud {}: no traffic or no matching bitrate", iface.info.name));
    return std::nullopt;
}

bool iface_send(Iface& iface, const BusMessage& msg)
{
    std::shared_lock lock(iface.io_mutex);
    return iface.open && iface.ops->send(iface, msg);
}

bool iface_stats(Iface& iface, IfaceStats& out)
{
    std::shared_lock lock(iface.io_mutex);
    if (!iface.open || !iface.ops->stats)
    {
        return false;
    }
    iface.ops->stats(iface, out);
    return true;
}
