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
// A channel that reports FEATURE_FD_CAPABLE opens through CAN_InitializeFD and then speaks
// CAN_ReadFD / CAN_WriteFD only (PCAN-Basic refuses the classic calls on it); classic frames pass
// through the same calls.
// ponytail: the FD path is written against the PCAN-Basic documentation and has not seen a
// PCAN-USB FD yet; bit timing assumes the 80 MHz clock every PEAK FD adapter offers.

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
constexpr uint8_t pcan_channel_features = 0x16;
constexpr uint32_t pcan_feature_fd = 0x01;
constexpr uint32_t pcan_channel_unavailable = 0;

constexpr uint8_t pcan_msg_rtr = 0x01;
constexpr uint8_t pcan_msg_extended = 0x02;
constexpr uint8_t pcan_msg_fd = 0x04;
constexpr uint8_t pcan_msg_brs = 0x08;
constexpr uint8_t pcan_msg_errframe = 0x40;
constexpr uint8_t pcan_msg_status = 0x80;

struct PcanMsg
{
    uint32_t id;
    uint8_t type;
    uint8_t len;
    uint8_t data[8];
};

struct PcanMsgFd
{
    uint32_t id;
    uint8_t type;
    uint8_t dlc;      // the DLC code 0..15, not a byte count
    uint8_t data[64];
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
    // PCAN-Basic 4.0 (2014) and later; nullptr in an older DLL, which then has no FD channels.
    PcanStatus (__stdcall *CAN_InitializeFD)(PcanHandle ch, char* bitrate);
    PcanStatus (__stdcall *CAN_ReadFD)(PcanHandle ch, PcanMsgFd* msg, uint64_t* ts_us);
    PcanStatus (__stdcall *CAN_WriteFD)(PcanHandle ch, PcanMsgFd* msg);
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
        load(t.CAN_InitializeFD, "CAN_InitializeFD");
        load(t.CAN_ReadFD, "CAN_ReadFD");
        load(t.CAN_WriteFD, "CAN_WriteFD");
        if (!ok)
        {
            t.CAN_InitializeFD = nullptr; // pcan_enumerate: no channel gets iface_cap::canfd
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

constexpr unsigned pcan_fd_bitrates[] = {500000, 1000000, 2000000, 4000000, 5000000, 8000000};
constexpr uint64_t pcan_fd_clock = 80'000'000;

struct PcanTiming
{
    unsigned brp = 0;
    unsigned tseg1 = 0;
    unsigned tseg2 = 0;
};

// Bit timing at the 80 MHz clock: the smallest prescaler (most time quanta, finest sample
// point) whose segments fit the controller, within 0.1 % of the rate.
bool pcan_fd_timing(unsigned bitrate, unsigned sample_point, unsigned max_tseg1, unsigned max_tseg2, PcanTiming& out)
{
    if (bitrate == 0)
    {
        return false;
    }
    if (sample_point == 0 || sample_point >= 1000)
    {
        sample_point = 800;
    }
    for (unsigned brp = 1; brp <= 1024; ++brp)
    {
        const uint64_t tq_rate = static_cast<uint64_t>(brp) * bitrate;
        const uint64_t tq = (pcan_fd_clock + tq_rate / 2) / tq_rate;
        if (tq < 4)
        {
            break;
        }
        const uint64_t actual = pcan_fd_clock / (brp * tq);
        if ((actual > bitrate ? actual - bitrate : bitrate - actual) * 1000 > bitrate)
        {
            continue;
        }
        const uint64_t tseg2 = tq - (tq * sample_point + 500) / 1000;
        if (tseg2 < 1 || tseg2 > max_tseg2 || tq < tseg2 + 2 || tq - 1 - tseg2 > max_tseg1)
        {
            continue;
        }
        out = {.brp = brp, .tseg1 = static_cast<unsigned>(tq - 1 - tseg2), .tseg2 = static_cast<unsigned>(tseg2)};
        return true;
    }
    return false;
}

struct Pcan
{
    PcanHandle handle = 0;
    bool fd = false;             // opened with CAN_InitializeFD: CAN_ReadFD / CAN_WriteFD only
    uint32_t data_bitrate = 0;
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
    std::vector<CanTiming> fd_bitrates; // every data rate from the nominal rate up
    for (const unsigned br : pcan_bitrates)
    {
        bitrates.push_back({.bitrate = br});
        for (const unsigned data : pcan_fd_bitrates)
        {
            if (PcanTiming t; data >= br && pcan_fd_timing(br, 875, 256, 128, t))
            {
                fd_bitrates.push_back({.bitrate = br, .bitrate_fd = data, .sample_point = 875, .sample_point_fd = 750});
            }
        }
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
        uint32_t features = 0;
        pb->CAN_GetValue(ch.handle, pcan_channel_features, &features, sizeof features);
        const bool fd = pb->CAN_InitializeFD != nullptr && (features & pcan_feature_fd) != 0;
        out.push_back({
            .name = pcan_channel_name(*pb, ch),
            .details = std::format("PEAK channel {}", ch.label),
            .version = std::string(first_line),
            .capabilities = iface_cap::listen_only | (fd ? iface_cap::canfd : 0u),
            .bitrates = fd ? fd_bitrates : bitrates,
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
    const bool fd = config.can_fd && (iface.info.capabilities & iface_cap::canfd) != 0;
    if (fd)
    {
        PcanTiming nom;
        PcanTiming data;
        if (!pcan_fd_timing(config.bitrate, config.sample_point, 256, 128, nom) ||
            !pcan_fd_timing(config.fd_bitrate, config.fd_sample_point, 32, 16, data))
        {
            log_error(std::format("PCAN {}: no bit timing for {} / {} bit/s", name, config.bitrate, config.fd_bitrate));
            return false;
        }
        std::string bitrate = std::format("f_clock_mhz=80, nom_brp={}, nom_tseg1={}, nom_tseg2={}, nom_sjw={}, "
                                          "data_brp={}, data_tseg1={}, data_tseg2={}, data_sjw={}",
                                          nom.brp, nom.tseg1, nom.tseg2, nom.tseg2, data.brp, data.tseg1, data.tseg2, data.tseg2);
        if (const PcanStatus st = pb->CAN_InitializeFD(ch->handle, bitrate.data()); st != pcan_ok)
        {
            log_error(std::format("PCAN {}: CAN_InitializeFD({}) failed: 0x{:X} (in use by another program?)", name, bitrate, st));
            return false;
        }
    }
    else if (const PcanStatus st = pb->CAN_Initialize(ch->handle, pcan_btr(config.bitrate), 0, 0, 0); st != pcan_ok)
    {
        log_error(std::format("PCAN {}: CAN_Initialize failed: 0x{:X} (in use by another program?)", name, st));
        return false;
    }
    auto p = std::make_unique<Pcan>();
    p->handle = ch->handle;
    p->fd = fd;
    p->bitrate = config.bitrate;
    p->data_bitrate = fd ? config.fd_bitrate : 0;
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
    const bool fd = p.fd && has_flag(msg, bus_flag::fd);
    const bool rtr = !fd && has_flag(msg, bus_flag::rtr);
    const uint8_t dlc = bus_length_to_dlc(std::min<int>(msg.len, fd ? 64 : 8));
    const uint8_t type = static_cast<uint8_t>((has_flag(msg, bus_flag::extended) ? pcan_msg_extended : 0) | (rtr ? pcan_msg_rtr : 0) |
                                              (fd ? pcan_msg_fd : 0) | (fd && has_flag(msg, bus_flag::brs) ? pcan_msg_brs : 0));
    PcanStatus st = pcan_ok;
    if (p.fd)
    {
        PcanMsgFd out{.id = can_id(msg), .type = type, .dlc = dlc, .data = {}};
        std::copy_n(msg.data.begin(), rtr ? 0 : bus_dlc_lengths[dlc], out.data); // FD pads to the DLC's length with 0
        st = pb.CAN_WriteFD(p.handle, &out);
    }
    else
    {
        PcanMsg out{.id = can_id(msg), .type = type, .len = dlc, .data = {}};
        std::copy_n(msg.data.begin(), rtr ? 0 : dlc, out.data);
        st = pb.CAN_Write(p.handle, &out);
    }
    if (st != pcan_ok)
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
        PcanMsgFd msg{};
        int64_t device_ns = 0;
        PcanStatus st = pcan_ok;
        if (p.fd)
        {
            uint64_t ts_us = 0;
            st = pb.CAN_ReadFD(p.handle, &msg, &ts_us);
            device_ns = static_cast<int64_t>(ts_us) * 1000;
        }
        else
        {
            PcanMsg classic{};
            PcanTimestamp ts{};
            st = pb.CAN_Read(p.handle, &classic, &ts);
            msg = {.id = classic.id, .type = classic.type, .dlc = std::min<uint8_t>(classic.len, 8), .data = {}};
            std::copy_n(classic.data, 8, msg.data);
            device_ns = (static_cast<int64_t>(ts.millis_overflow) << 32 | ts.millis) * 1000000 + ts.micros * 1000ll;
        }
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
        const bool fd = (msg.type & pcan_msg_fd) != 0;
        if (fd)
        {
            m.flags |= bus_flag::fd | ((msg.type & pcan_msg_brs) != 0 ? bus_flag::brs : 0);
        }
        m.ts_ns = device_ns + p.offset_ns;
        set_length(m, fd ? bus_dlc_lengths[msg.dlc & 0xF] : std::min<int>(msg.dlc, 8));
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
        .data_bitrate = p.data_bitrate,
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
