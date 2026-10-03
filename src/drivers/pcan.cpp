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

// PEAK PCAN adapters on Windows through PCAN-Basic (PCANBasic.dll of the PEAK driver package),
// loaded at run time like CANlib in kvaser.cpp: nothing of PEAK's is needed to build or package.
// The declarations below are PCAN-Basic's public, stable API (PCANBasic.h), only what is used.
// On Linux the kernel's peak_usb driver makes the same adapters SocketCAN interfaces.
// ponytail: classic CAN only, FD adapters open in classic mode; CAN_InitializeFD / CAN_ReadFD /
// CAN_WriteFD with a bitrate string when someone has a PCAN-USB FD to test with.

#include <algorithm>
#include <atomic>
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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{

using PcanHandle = uint16_t;
using PcanStatus = uint32_t;

constexpr PcanStatus pcan_ok = 0;
constexpr PcanStatus pcan_bus_light = 0x4;
constexpr PcanStatus pcan_bus_heavy = 0x8;       // error warning
constexpr PcanStatus pcan_bus_off = 0x10;
constexpr PcanStatus pcan_queue_empty = 0x20;
constexpr PcanStatus pcan_bus_passive = 0x40000;
constexpr PcanStatus pcan_ill_handle = 0x1C00;   // mask: hardware, net or client handle invalid
constexpr PcanStatus pcan_not_initialized = 0x4000000;

constexpr uint8_t pcan_receive_event = 0x03;
constexpr uint8_t pcan_listen_only = 0x08;
constexpr uint8_t pcan_channel_condition = 0x0D;
constexpr uint8_t pcan_hardware_name = 0x0E;
constexpr uint8_t pcan_channel_version = 0x06;
constexpr uint32_t pcan_channel_unavailable = 0;

constexpr uint8_t pcan_msg_rtr = 0x01;
constexpr uint8_t pcan_msg_extended = 0x02;
constexpr uint8_t pcan_msg_errframe = 0x40;
constexpr uint8_t pcan_msg_status = 0x80;

struct PcanMsg
{
    uint32_t id;
    uint8_t type;
    uint8_t len;
    uint8_t data[8];
};

struct PcanTimestamp // since Windows started
{
    uint32_t millis;
    uint16_t millis_overflow;
    uint16_t micros;
};

struct PcanBasic
{
    PcanStatus (__stdcall *CAN_Initialize)(PcanHandle ch, uint16_t btr0btr1, uint8_t hw_type, uint32_t io_port, uint16_t interrupt);
    PcanStatus (__stdcall *CAN_Uninitialize)(PcanHandle ch);
    PcanStatus (__stdcall *CAN_Read)(PcanHandle ch, PcanMsg* msg, PcanTimestamp* ts);
    PcanStatus (__stdcall *CAN_Write)(PcanHandle ch, PcanMsg* msg);
    PcanStatus (__stdcall *CAN_GetStatus)(PcanHandle ch);
    PcanStatus (__stdcall *CAN_GetValue)(PcanHandle ch, uint8_t parameter, void* buffer, uint32_t size);
    PcanStatus (__stdcall *CAN_SetValue)(PcanHandle ch, uint8_t parameter, void* buffer, uint32_t size);
};

// nullptr when PCAN-Basic is not installed, which the driver reports as "no channels".
const PcanBasic* pcan_load()
{
    static const PcanBasic* lib = []() -> const PcanBasic*
    {
        const HMODULE dll = LoadLibraryA("PCANBasic.dll");
        if (dll == nullptr)
        {
            log_info("PCAN: PCANBasic.dll not found, install the PEAK driver package (with PCAN-Basic) for PEAK adapters");
            return nullptr;
        }
        static PcanBasic t;
        bool ok = true;
        const auto load = [&](auto& fn, const char* name)
        {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
            ok = ok && fn != nullptr;
        };
        load(t.CAN_Initialize, "CAN_Initialize");
        load(t.CAN_Uninitialize, "CAN_Uninitialize");
        load(t.CAN_Read, "CAN_Read");
        load(t.CAN_Write, "CAN_Write");
        load(t.CAN_GetStatus, "CAN_GetStatus");
        load(t.CAN_GetValue, "CAN_GetValue");
        load(t.CAN_SetValue, "CAN_SetValue");
        if (!ok)
        {
            log_error("PCAN: PCANBasic.dll is missing a function this build needs");
            return nullptr;
        }
        return &t;
    }();
    return lib;
}

struct PcanChannel
{
    PcanHandle handle;
    const char* label;
};

// PCAN-Basic has no channel list: every handle it defines is asked for its condition.
std::vector<PcanChannel> pcan_handles()
{
    std::vector<PcanChannel> out;
    static const std::string labels = []
    {
        std::string s;
        for (const char* bus : {"USB", "PCI", "LAN"})
        {
            for (int i = 1; i <= 16; ++i)
            {
                s += std::format("{}{}", bus, i);
                s += '\0';
            }
        }
        return s;
    }();
    const char* label = labels.c_str();
    for (const uint16_t base : {0x50, 0x40, 0x800}) // PCAN_USBBUS1 = 0x51, PCAN_PCIBUS1 = 0x41, PCAN_LANBUS1 = 0x801
    {
        for (uint16_t i = 1; i <= 16; ++i)
        {
            // USB and PCI 9..16 were added later as 0x509.. and 0x409..
            const uint16_t handle = base != 0x800 && i > 8 ? static_cast<uint16_t>(base * 0x10 + i) : static_cast<uint16_t>(base + i);
            out.push_back({handle, label});
            label += std::char_traits<char>::length(label) + 1;
        }
    }
    return out;
}

std::string pcan_channel_name(const PcanBasic& pb, const PcanChannel& ch)
{
    char hw[64] = {};
    pb.CAN_GetValue(ch.handle, pcan_hardware_name, hw, sizeof hw);
    return std::format("{} ({})", hw[0] != '\0' ? hw : "PCAN", ch.label);
}

constexpr unsigned pcan_bitrates[] = {5000, 10000, 20000, 33333, 47619, 50000, 83333, 95238, 100000, 125000, 250000, 500000, 800000, 1000000};

// BTR0BTR1 of the SJA1000 at 16 MHz: PCAN-Basic's PCAN_BAUD_* constants.
uint16_t pcan_btr(unsigned bitrate)
{
    switch (bitrate)
    {
    case 5000:    return 0x7F7F;
    case 10000:   return 0x672F;
    case 20000:   return 0x532F;
    case 33333:   return 0x8B2F;
    case 47619:   return 0x1414;
    case 50000:   return 0x472F;
    case 83333:   return 0x852B;
    case 95238:   return 0xC34E;
    case 100000:  return 0x432F;
    case 125000:  return 0x031C;
    case 250000:  return 0x011C;
    case 800000:  return 0x0016;
    case 1000000: return 0x0014;
    default:
        if (bitrate != 500000)
        {
            log_warning(std::format("PCAN: bitrate {} not supported, using 500000", bitrate));
        }
        return 0x001C;
    }
}

struct Pcan
{
    PcanHandle handle = 0;
    HANDLE event = nullptr;      // set by PCAN-Basic when the receive queue gets a frame, and by send()
    int64_t offset_ns = 0;       // host time - device time, taken at the first frame
    uint32_t bitrate = 0;

    std::mutex tx_mutex;
    std::deque<BusMessage> tx_done;   // sent frames, reported by read()

    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> rx_errors{0};
    std::atomic<uint64_t> tx_frames{0};
    std::atomic<uint64_t> tx_errors{0};
};

void pcan_enumerate(std::vector<IfaceInfo>& out)
{
    const PcanBasic* pb = pcan_load();
    if (pb == nullptr)
    {
        return;
    }
    std::vector<CanTiming> bitrates;
    for (const unsigned br : pcan_bitrates)
    {
        bitrates.push_back({.bitrate = br});
    }
    for (const PcanChannel& ch : pcan_handles())
    {
        uint32_t condition = pcan_channel_unavailable;
        if (pb->CAN_GetValue(ch.handle, pcan_channel_condition, &condition, sizeof condition) != pcan_ok || condition == pcan_channel_unavailable)
        {
            continue;
        }
        char version[256] = {};
        pb->CAN_GetValue(ch.handle, pcan_channel_version, version, sizeof version);
        const std::string_view first_line(version, std::string_view(version).find('\n'));
        out.push_back({
            .name = pcan_channel_name(*pb, ch),
            .details = std::format("PEAK channel {}", ch.label),
            .version = std::string(first_line),
            .capabilities = iface_cap::listen_only,
            .bitrates = bitrates,
        });
    }
}

bool pcan_open(Iface& iface, const IfaceConfig& config)
{
    const PcanBasic* pb = pcan_load();
    if (pb == nullptr)
    {
        return false;
    }
    const std::string& name = iface.info.name;
    const std::vector<PcanChannel> handles = pcan_handles();
    const auto ch = std::ranges::find_if(handles, [&](const PcanChannel& c) { return name.ends_with(std::format("({})", c.label)); });
    if (ch == handles.end())
    {
        log_error(std::format("PCAN: channel {} is gone", name));
        return false;
    }
    uint32_t listen = config.listen_only ? 1 : 0;
    pb->CAN_SetValue(ch->handle, pcan_listen_only, &listen, sizeof listen); // before Initialize: no frame is acked in between
    if (const PcanStatus st = pb->CAN_Initialize(ch->handle, pcan_btr(config.bitrate), 0, 0, 0); st != pcan_ok)
    {
        log_error(std::format("PCAN {}: CAN_Initialize failed: 0x{:X} (in use by another program?)", name, st));
        return false;
    }
    auto p = std::make_unique<Pcan>();
    p->handle = ch->handle;
    p->bitrate = config.bitrate;
    p->event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (const PcanStatus st = pb->CAN_SetValue(ch->handle, pcan_receive_event, &p->event, sizeof p->event); st != pcan_ok)
    {
        log_error(std::format("PCAN {}: receive event refused: 0x{:X}", name, st));
        pb->CAN_Uninitialize(ch->handle);
        CloseHandle(p->event);
        return false;
    }
    iface_set_impl(iface, std::move(p));
    return true;
}

void pcan_close(Iface& iface)
{
    const PcanBasic* pb = pcan_load();
    if (auto* p = static_cast<Pcan*>(iface.impl.get()); pb != nullptr && p != nullptr && p->event != nullptr)
    {
        pb->CAN_Uninitialize(p->handle);
        CloseHandle(p->event);
        p->event = nullptr;
    }
}

bool pcan_send(Iface& iface, const BusMessage& msg)
{
    const PcanBasic& pb = *pcan_load();
    auto& p = iface_impl<Pcan>(iface);
    PcanMsg out{.id = can_id(msg), .type = 0, .len = static_cast<uint8_t>(std::min<unsigned>(msg.len, 8)), .data = {}};
    if (has_flag(msg, bus_flag::extended))
    {
        out.type |= pcan_msg_extended;
    }
    if (has_flag(msg, bus_flag::rtr))
    {
        out.type |= pcan_msg_rtr;
    }
    else
    {
        std::copy_n(msg.data.begin(), out.len, out.data);
    }
    if (const PcanStatus st = pb.CAN_Write(p.handle, &out); st != pcan_ok)
    {
        log_error(std::format("PCAN {}: CAN_Write failed: 0x{:X}", iface.info.name, st));
        ++p.tx_errors;
        return false;
    }
    ++p.tx_frames;
    BusMessage tx = msg;
    tx.iface = iface.index;
    tx.flags |= bus_flag::tx;
    tx.ts_ns = now_ns();
    {
        std::lock_guard lock(p.tx_mutex);
        p.tx_done.push_back(tx);
    }
    SetEvent(p.event); // read() reports it now, not after its timeout
    return true;
}

int pcan_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    const PcanBasic& pb = *pcan_load();
    auto& p = iface_impl<Pcan>(iface);
    WaitForSingleObject(p.event, static_cast<DWORD>(timeout_ms));
    int n = 0;
    {
        std::lock_guard lock(p.tx_mutex);
        while (n < max && !p.tx_done.empty())
        {
            out[n++] = p.tx_done.front();
            p.tx_done.pop_front();
        }
    }
    while (n < max)
    {
        PcanMsg msg{};
        PcanTimestamp ts{};
        const PcanStatus st = pb.CAN_Read(p.handle, &msg, &ts);
        if ((st & (pcan_ill_handle | pcan_not_initialized)) != 0)
        {
            log_error(std::format("PCAN {}: CAN_Read failed: 0x{:X}", iface.info.name, st));
            return n > 0 ? n : -1;
        }
        if (st != pcan_ok) // queue empty, or a bus state: stats() reports that
        {
            break;
        }
        if ((msg.type & pcan_msg_status) != 0)
        {
            continue;
        }
        if ((msg.type & pcan_msg_errframe) != 0)
        {
            ++p.rx_errors;
            continue;
        }
        const int64_t device_ns = (static_cast<int64_t>(ts.millis_overflow) << 32 | ts.millis) * 1000000 + ts.micros * 1000ll;
        if (p.offset_ns == 0)
        {
            // ponytail: the first frame's delivery delay (about 1 ms) becomes a constant offset;
            // QueryPerformanceCounter against the device clock at open if it matters.
            p.offset_ns = now_ns() - device_ns;
        }
        BusMessage& m = out[n++];
        m = BusMessage{.id = msg.id & can_id_mask_extended, .iface = iface.index};
        if ((msg.type & pcan_msg_extended) != 0)
        {
            m.flags |= bus_flag::extended;
        }
        if ((msg.type & pcan_msg_rtr) != 0)
        {
            m.flags |= bus_flag::rtr;
        }
        m.ts_ns = device_ns + p.offset_ns;
        set_length(m, std::min<int>(msg.len, 8));
        std::copy_n(msg.data, m.len, m.data.begin());
        ++p.rx_frames;
    }
    if (n == max)
    {
        SetEvent(p.event); // more may be queued: the event is only set when the queue goes non-empty
    }
    return n;
}

void pcan_stats(Iface& iface, IfaceStats& out)
{
    const PcanBasic& pb = *pcan_load();
    auto& p = iface_impl<Pcan>(iface);
    out = {
        .state = IfaceState::Ok,
        .rx_frames = p.rx_frames,
        .rx_errors = p.rx_errors,
        .tx_frames = p.tx_frames,
        .tx_errors = p.tx_errors,
        .bitrate = p.bitrate,
    };
    const PcanStatus st = pb.CAN_GetStatus(p.handle);
    if ((st & pcan_bus_off) != 0)
    {
        out.state = IfaceState::BusOff;
    }
    else if ((st & pcan_bus_passive) != 0)
    {
        out.state = IfaceState::Passive;
    }
    else if ((st & (pcan_bus_heavy | pcan_bus_light)) != 0)
    {
        out.state = IfaceState::Warning;
    }
}

} // namespace

extern const DriverOps pcan_driver = {
    .name = "PCAN",
    .enumerate = pcan_enumerate,
    .open = pcan_open,
    .close = pcan_close,
    .send = pcan_send,
    .read = pcan_read,
    .stats = pcan_stats,
};
