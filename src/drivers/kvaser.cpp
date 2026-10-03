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

// Kvaser CANlib (linuxcan), classic CAN only.
// Built with -DKRAKEN_KVASER=ON.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "core/log.h"
#include "drivers/driver.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "drivers/kvaser_canlib.h"

const Canlib* canlib_load()
{
    static const Canlib* lib = []() -> const Canlib*
    {
#ifdef _WIN32
        // canlib32.dll is the 64-bit library too; the Kvaser driver installer puts it in System32.
        const HMODULE so = LoadLibraryA("canlib32.dll");
        if (so == nullptr)
        {
            log_info("Kvaser: canlib32.dll not found, install the Kvaser drivers for Kvaser channels");
            return nullptr;
        }
        const auto dlsym = [](HMODULE dll, const char* name) { return reinterpret_cast<void*>(GetProcAddress(dll, name)); };
#else
        void* so = dlopen("libcanlib.so.1", RTLD_NOW | RTLD_LOCAL);
        if (so == nullptr)
        {
            so = dlopen("libcanlib.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (so == nullptr)
        {
            log_info("Kvaser: libcanlib not installed, native Kvaser channels unavailable (USB devices still work through SocketCAN's kvaser_usb)");
            return nullptr;
        }
#endif
        static Canlib t;
        bool ok = true;
        const auto load = [&](auto& fn, const char* name)
        {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name));
            ok = ok && fn != nullptr;
        };
        load(t.canInitializeLibrary, "canInitializeLibrary");
        load(t.canGetNumberOfChannels, "canGetNumberOfChannels");
        load(t.canGetChannelData, "canGetChannelData");
        load(t.canOpenChannel, "canOpenChannel");
        load(t.canSetBusParams, "canSetBusParams");
        load(t.canSetBusParamsFd, "canSetBusParamsFd");
        load(t.canSetBusOutputControl, "canSetBusOutputControl");
        load(t.canBusOn, "canBusOn");
        load(t.canBusOff, "canBusOff");
        load(t.canClose, "canClose");
        load(t.canWrite, "canWrite");
        load(t.canReadWait, "canReadWait");
        load(t.canRequestChipStatus, "canRequestChipStatus");
        load(t.canReadStatus, "canReadStatus");
        load(t.canIoCtl, "canIoCtl");
        if (!ok)
        {
            log_error("Kvaser: libcanlib is missing a function this build needs (an old linuxcan?)");
            return nullptr;
        }
        t.canInitializeLibrary();
        return &t;
    }();
    return lib;
}

namespace
{

// Device timestamps count from canBusOn, 1 us per tick on linuxcan (set at open on Windows,
// where a tick is 1 ms unless asked).
constexpr int64_t kvaser_tick_ns = 1000;

struct Kvaser
{
    canHandle handle = -1;
    int64_t open_ns = 0;   // host time at canBusOn, base for device timestamps
    bool fd = false;
    unsigned long last_ts = 0;   // read(): the previous device timestamp
    uint64_t ts_wraps = 0;       // times it wrapped (32-bit unsigned long on Windows: every 71.6 min)

    std::mutex tx_mutex;
    std::deque<BusMessage> tx_done;   // sent frames, reported by read()

    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> rx_errors{0};
    std::atomic<uint64_t> tx_frames{0};
    std::atomic<uint64_t> tx_errors{0};
};

std::string kvaser_channel_name(const Canlib& cl, int ch)
{
    char dev[256] = {};
    cl.canGetChannelData(ch, canCHANNELDATA_DEVDESCR_ASCII, dev, sizeof(dev));
    return std::format("{} (ch{})", dev, ch);
}

void kvaser_enumerate(std::vector<IfaceInfo>& out)
{
    const Canlib* cl = canlib_load();
    if (cl == nullptr)
    {
        return;
    }
    int channels = 0;
    if (const canStatus st = cl->canGetNumberOfChannels(&channels); st != canOK)
    {
        log_error(std::format("Kvaser: canGetNumberOfChannels failed: {}", st));
        return;
    }
    std::vector<CanTiming> bitrates;
    for (unsigned br : {10000u, 50000u, 62500u, 83333u, 100000u, 125000u, 250000u, 500000u, 1000000u})
    {
        bitrates.push_back({.bitrate = br, .bitrate_fd = 2000000});
    }
    for (int ch = 0; ch < channels; ++ch)
    {
        unsigned caps = 0;
        cl->canGetChannelData(ch, canCHANNELDATA_CHANNEL_CAP, &caps, sizeof caps);
        out.push_back({
            .name = kvaser_channel_name(*cl, ch),
            .details = std::format("Kvaser channel {}", ch),
            .version = "",
            .capabilities = iface_cap::listen_only | ((caps & canCHANNEL_CAP_CAN_FD) != 0 ? iface_cap::canfd : 0u),
            .bitrates = bitrates,
        });
    }
}

long kvaser_bitrate(unsigned bitrate)
{
    switch (bitrate)
    {
    case 10000:   return canBITRATE_10K;
    case 50000:   return canBITRATE_50K;
    case 62500:   return canBITRATE_62K;
    case 83333:   return canBITRATE_83K;
    case 100000:  return canBITRATE_100K;
    case 125000:  return canBITRATE_125K;
    case 250000:  return canBITRATE_250K;
    case 1000000: return canBITRATE_1M;
    default:
        // ponytail: 20k/800k have no canBITRATE_* constant, would need explicit tseg values
        if (bitrate != 500000)
        {
            log_warning(std::format("Kvaser: bitrate {} not supported, using 500000", bitrate));
        }
        return canBITRATE_500K;
    }
}

// The data phase bitrate as CANlib's predefined constants (80 % sample point).
long kvaser_fd_bitrate(unsigned bitrate)
{
    switch (bitrate)
    {
    case 500000:  return canFD_BITRATE_500K_80P;
    case 1000000: return canFD_BITRATE_1M_80P;
    case 4000000: return canFD_BITRATE_4M_80P;
    case 8000000: return canFD_BITRATE_8M_80P;
    default:
        if (bitrate != 2000000)
        {
            log_warning(std::format("Kvaser: FD bitrate {} not supported, using 2000000", bitrate));
        }
        return canFD_BITRATE_2M_80P;
    }
}

bool kvaser_open(Iface& iface, const IfaceConfig& config)
{
    const Canlib* cl = canlib_load();
    if (cl == nullptr)
    {
        return false;
    }
    const std::string& name = iface.info.name;
    int channels = 0;
    cl->canGetNumberOfChannels(&channels);
    int ch = 0;
    while (ch < channels && kvaser_channel_name(*cl, ch) != name)
    {
        ++ch;
    }
    if (ch == channels)
    {
        log_error(std::format("Kvaser: channel {} is gone", name));
        return false;
    }

    const bool fd = config.can_fd && (iface.info.capabilities & iface_cap::canfd) != 0;
    const canHandle h = cl->canOpenChannel(ch, canOPEN_ACCEPT_VIRTUAL | (fd ? canOPEN_CAN_FD : 0));
    if (h < 0)
    {
        log_error(std::format("Kvaser {}: canOpenChannel failed: {}", name, h));
        return false;
    }
#ifdef _WIN32
    uint32_t tick_us = kvaser_tick_ns / 1000;
    cl->canIoCtl(h, canIOCTL_SET_TIMER_SCALE, &tick_us, sizeof tick_us);
#endif
    if (config.configure)
    {
        if (const canStatus st = cl->canSetBusParams(h, kvaser_bitrate(config.bitrate), 0, 0, 0, 0, 0); st != canOK)
        {
            log_error(std::format("Kvaser {}: canSetBusParams failed: {}", name, st));
            cl->canClose(h);
            return false;
        }
        if (fd)
        {
            if (const canStatus st = cl->canSetBusParamsFd(h, kvaser_fd_bitrate(config.fd_bitrate), 0, 0, 0); st != canOK)
            {
                log_error(std::format("Kvaser {}: canSetBusParamsFd failed: {}", name, st));
                cl->canClose(h);
                return false;
            }
        }
        cl->canSetBusOutputControl(h, config.listen_only ? canDRIVER_SILENT : canDRIVER_NORMAL);
    }
    if (const canStatus st = cl->canBusOn(h); st != canOK)
    {
        log_error(std::format("Kvaser {}: canBusOn failed: {}", name, st));
        cl->canClose(h);
        return false;
    }

    auto k = std::make_unique<Kvaser>();
    k->handle = h;
    k->open_ns = now_ns();
    k->fd = fd;
    iface_set_impl(iface, std::move(k));
    return true;
}

void kvaser_close(Iface& iface)
{
    const Canlib* cl = canlib_load();
    if (auto* k = static_cast<Kvaser*>(iface.impl.get()); cl != nullptr && k != nullptr && k->handle >= 0)
    {
        cl->canBusOff(k->handle);
        cl->canClose(k->handle);
        k->handle = -1;
    }
}

bool kvaser_send(Iface& iface, const BusMessage& msg)
{
    const Canlib& cl = *canlib_load();
    auto& k = iface_impl<Kvaser>(iface);
    unsigned flags = has_flag(msg, bus_flag::extended) ? canMSG_EXT : canMSG_STD;
    if (has_flag(msg, bus_flag::rtr))
    {
        flags |= canMSG_RTR;
    }
    const bool fd = k.fd && has_flag(msg, bus_flag::fd);
    if (fd)
    {
        flags |= canFDMSG_FDF | (has_flag(msg, bus_flag::brs) ? canFDMSG_BRS : 0);
    }
    const unsigned len = std::min<unsigned>(msg.len, fd ? 64 : 8);
    uint8_t data[64] = {};
    if (!has_flag(msg, bus_flag::rtr))
    {
        std::copy_n(msg.data.begin(), len, data);
    }
    if (const canStatus st = cl.canWrite(k.handle, static_cast<long>(can_id(msg)), data, len, flags); st != canOK)
    {
        log_error(std::format("Kvaser {}: canWrite failed: {}", iface.info.name, st));
        ++k.tx_errors;
        return false;
    }
    ++k.tx_frames;
    BusMessage tx = msg;
    tx.iface = iface.index;
    tx.flags |= bus_flag::tx;
    tx.ts_ns = now_ns();
    std::lock_guard lock(k.tx_mutex);
    k.tx_done.push_back(tx);
    return true;
}

int kvaser_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    const Canlib& cl = *canlib_load();
    auto& k = iface_impl<Kvaser>(iface);
    int n = 0;
    {
        std::lock_guard lock(k.tx_mutex);
        while (n < max && !k.tx_done.empty())
        {
            out[n++] = k.tx_done.front();
            k.tx_done.pop_front();
        }
    }
    // ponytail: a send during the wait is reported after the timeout (<= 100 ms late); shorter wait if TX echo latency matters.
    unsigned long wait = n > 0 ? 0 : static_cast<unsigned long>(timeout_ms);
    while (n < max)
    {
        long id = 0;
        uint8_t data[64] = {};
        unsigned dlc = 0;
        unsigned flags = 0;
        unsigned long ts = 0;
        const canStatus st = cl.canReadWait(k.handle, &id, data, &dlc, &flags, &ts, wait);
        wait = 0;
        if (st == canERR_NOMSG || st == canERR_TIMEOUT)
        {
            break;
        }
        if (st != canOK)
        {
            log_error(std::format("Kvaser {}: canReadWait failed: {}", iface.info.name, st));
            return n > 0 ? n : -1;
        }
        if (flags & canMSG_ERROR_FRAME)
        {
            ++k.rx_errors;
            continue;
        }
        BusMessage& m = out[n++];
        m = BusMessage{.id = static_cast<uint32_t>(id) & can_id_mask_extended, .iface = iface.index};
        if (flags & canMSG_EXT)
        {
            m.flags |= bus_flag::extended;
        }
        if (flags & canMSG_RTR)
        {
            m.flags |= bus_flag::rtr;
        }
        const bool fd = (flags & canFDMSG_FDF) != 0;
        if (fd)
        {
            m.flags |= bus_flag::fd | ((flags & canFDMSG_BRS) != 0 ? bus_flag::brs : 0);
        }
        // Half the range back is a wrap, less is frames of two queues slightly out of order.
        if (sizeof ts == 4 && ts < k.last_ts && k.last_ts - ts > 0x80000000ul)
        {
            ++k.ts_wraps;
        }
        k.last_ts = ts;
        m.ts_ns = k.open_ns + static_cast<int64_t>((k.ts_wraps << 32) + ts) * kvaser_tick_ns;
        set_length(m, static_cast<int>(std::min(dlc, fd ? 64u : 8u))); // CANlib reports FD lengths in bytes
        std::copy_n(data, m.len, m.data.begin());
        ++k.rx_frames;
    }
    return n;
}

void kvaser_stats(Iface& iface, IfaceStats& out)
{
    const Canlib& cl = *canlib_load();
    auto& k = iface_impl<Kvaser>(iface);
    out = {
        .state = IfaceState::Ok,
        .rx_frames = k.rx_frames,
        .rx_errors = k.rx_errors,
        .tx_frames = k.tx_frames,
        .tx_errors = k.tx_errors,
    };
    unsigned long flags = 0;
    cl.canRequestChipStatus(k.handle);
    cl.canReadStatus(k.handle, &flags);
    if (flags & canSTAT_BUS_OFF)
    {
        out.state = IfaceState::BusOff;
    }
    else if (flags & canSTAT_ERROR_PASSIVE)
    {
        out.state = IfaceState::Passive;
    }
    else if (flags & canSTAT_ERROR_WARNING)
    {
        out.state = IfaceState::Warning;
    }
}

} // namespace

extern const DriverOps kvaser_driver = {
    .name = "Kvaser",
    .enumerate = kvaser_enumerate,
    .open = kvaser_open,
    .close = kvaser_close,
    .send = kvaser_send,
    .read = kvaser_read,
    .stats = kvaser_stats,
};
