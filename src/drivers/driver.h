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

// Driver layer: every driver is a DriverOps table of free functions, every bus channel
// an Iface. Replaces CanDriver / BusInterface / BusListener.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/bus_message.h"
#include "core/setup.h"
#include "core/trace.h"

// Interface capability bits (IfaceInfo::capabilities), same values as BusInterface.
namespace iface_cap
{
inline constexpr uint32_t canfd                = 0x001;
inline constexpr uint32_t listen_only          = 0x002;
inline constexpr uint32_t auto_restart         = 0x010;
inline constexpr uint32_t config_os            = 0x020;
inline constexpr uint32_t custom_bitrate       = 0x040;
inline constexpr uint32_t custom_canfd_bitrate = 0x080;
inline constexpr uint32_t lin_master           = 0x100;
inline constexpr uint32_t lin_slave            = 0x200;
}

enum class IfaceState : uint8_t
{
    Ok,
    Warning,
    Passive,
    BusOff,
    Stopped,
    Unknown,
};

[[nodiscard]] const char* iface_state_name(IfaceState s) noexcept;

// Wall-clock time as BusMessage::ts_ns: nanoseconds since the Unix epoch.
[[nodiscard]] inline int64_t now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Bitrate preset shown in the setup dialog. bitrate_fd == 0: classic CAN timing.
struct CanTiming
{
    unsigned bitrate = 0;
    unsigned bitrate_fd = 0;
    unsigned sample_point = 875;      // per mille
    unsigned sample_point_fd = 0;     // per mille
};

// The per-interface settings a driver opens with (bitrate, FD, LIN mode, ...).
using IfaceConfig = SetupInterface;

// What enumerate() reports for one channel.
struct IfaceInfo
{
    std::string name;          // e.g. "can0"; matched against SetupInterface::name
    std::string details;       // longer text for the setup dialog
    std::string version;       // driver / firmware version, if known
    BusType bus_type = BusType::CAN;
    uint32_t capabilities = 0; // iface_cap::*
    std::vector<CanTiming> bitrates;
    bool up = true;            // false: link is down (SocketCAN), left out of the default setup
};

// Counters since the interface was opened (UI keeps its own offsets for a reset).
struct IfaceStats
{
    IfaceState state = IfaceState::Unknown;
    uint64_t rx_frames = 0;
    uint64_t rx_errors = 0;
    uint64_t tx_frames = 0;
    uint64_t tx_errors = 0;
    uint64_t rx_overruns = 0;
    uint64_t tx_dropped = 0;
    uint32_t bitrate = 0;      // controller's actual rate in bit/s, 0 = unknown (vcan, other drivers)
    uint32_t data_bitrate = 0; // CAN FD data phase, 0 = unknown / FD off
};

struct Iface;

struct DriverOps
{
    const char* name;   // SetupInterface::driver, e.g. "SocketCAN"
    void (*enumerate)(std::vector<IfaceInfo>& out);
    // Configures and opens the channel, fills Iface::impl. Main thread, listener not running.
    bool (*open)(Iface& iface, const IfaceConfig& config);
    // Releases everything open() acquired. Main thread, after the listener stopped.
    void (*close)(Iface& iface);
    // Any thread, only while open. TX frames are reported back through read() (flag tx).
    bool (*send)(Iface& iface, const BusMessage& msg);
    // Listener thread: waits up to timeout_ms, returns the number of frames written to
    // out (iface index and timestamp set), 0 on timeout, -1 when the channel is gone.
    int (*read)(Iface& iface, BusMessage* out, int max, int timeout_ms);
    // Any thread while open.
    void (*stats)(Iface& iface, IfaceStats& out);
    // LIN (nullptr = not supported)
    void (*lin_sleep_wakeup)(Iface& iface, bool wakeup);
    void (*lin_set_schedule)(Iface& iface, uint8_t table_index);
    void (*lin_diag_request)(Iface& iface, uint8_t nad, std::span<const uint8_t> data);
};

// Every driver compiled into this build (SocketCAN, ...).
[[nodiscard]] std::span<const DriverOps* const> all_drivers() noexcept;

// CANblaster discovery listens 2 s on UDP during enumerate, so it is opt-in
// (Settings, default off, like the Qt build's mainWindow/CANblaster).
extern bool canblast_enabled;

// One bus channel. Not movable (inbox, thread, atomics): keep them in a std::deque.
struct Iface
{
    const DriverOps* ops = nullptr;
    IfaceInfo info;
    uint16_t index = 0;   // BusMessage::iface; position in App::ifaces
    std::unique_ptr<void, void (*)(void*)> impl{nullptr, nullptr};   // driver-private state

    Inbox inbox;                           // RX thread -> main thread
    std::jthread listener;
    std::shared_mutex io_mutex;            // send (shared) vs. open/close (unique)
    bool open = false;                     // guarded by io_mutex
    std::atomic<bool> failed{false};       // listener stopped on a read error
    std::atomic<uint64_t> total_bits{0};   // for the bus load
};

// Driver-private state: iface_set_impl(i, std::make_unique<T>(...)); iface_impl<T>(i).
template <class T>
void iface_set_impl(Iface& iface, std::unique_ptr<T> impl)
{
    iface.impl = {impl.release(), [](void* p) { delete static_cast<T*>(p); }};
}

template <class T>
[[nodiscard]] T& iface_impl(Iface& iface) noexcept
{
    return *static_cast<T*>(iface.impl.get());
}

// Approximate bits on the wire incl. stuffing, as BusInterface::addFrameBits.
[[nodiscard]] uint32_t bus_frame_bits(const BusMessage& m) noexcept;

// Runs enumerate() of every driver; adds channels not seen before and refreshes the info
// of known ones. Existing indices stay valid. Only while no measurement runs.
void ifaces_enumerate(std::deque<Iface>& ifaces);

// Setup used when no workspace is loaded: one network per interface ("Network 1", ...).
void ifaces_default_setup(const std::deque<Iface>& ifaces, Setup& setup);

// Index into ifaces or -1.
[[nodiscard]] int ifaces_find(const std::deque<Iface>& ifaces, std::string_view driver, std::string_view name);

// Resolves every enabled SetupInterface to an Iface (sets its `iface`, warns when
// missing), opens it and starts its listener thread. Consumers run on the RX threads,
// wake is called after each batch. Returns the number of interfaces opened.
int ifaces_start(std::deque<Iface>& ifaces, Setup& setup, std::span<const RxConsumer> consumers, void (*wake)());

// Stops all listener threads, then closes the channels.
void ifaces_stop(std::deque<Iface>& ifaces);

// Enabled interfaces of the setup that are open and still running (`up`) out of all enabled
// ones (`total`, including those that were not found). Main thread only (reads Iface::open).
struct LinkCount
{
    int up = 0;
    int total = 0;
};
[[nodiscard]] LinkCount ifaces_link_count(const std::deque<Iface>& ifaces, const Setup& setup);

// Sends on an open interface; false if it is closed or the driver failed.
bool iface_send(Iface& iface, const BusMessage& msg);

// Driver statistics; false (and out untouched) while the interface is closed.
bool iface_stats(Iface& iface, IfaceStats& out);

// SocketCAN link control from the GUI (socketcan.cpp). `ip` goes through pkexec when not
// root, so socketcan_run_ip() can block for a password prompt: never call it on the main thread.
enum class LinkOp
{
    Up,
    Down,
    AddVcan,
    Delete,
};

// `ip` arguments for op on the interface `name`, e.g. {"link", "set", "can0", "up"}.
// Up with timing (physical CAN; the kernel refuses up without bit timing) adds
// `type can bitrate ... sample-point ...` [dbitrate ... fd on] restart-ms ...; vcan passes nullptr.
[[nodiscard]] std::vector<std::string> ip_link_args(LinkOp op, const std::string& name,
                                                    const IfaceConfig* timing = nullptr);

// Full argv: [pkexec] <absolute ip> args... (pkexec unless root).
[[nodiscard]] std::vector<std::string> ip_command(const std::vector<std::string>& args, bool root);

// First vcanN (N = 0, 1, ...) for which taken(name) is false.
[[nodiscard]] std::string next_vcan_name(bool (*taken)(const std::string& name));

// Outcome of one `[pkexec] ip` call.
enum class IpResult
{
    ok,
    failed,   // ip itself failed (or could not start); stderr is in the Log
    denied,   // pkexec 126: authorization dismissed or refused
    no_agent, // pkexec 127 "No authentication agent found": no polkit agent in this session
};

// Classifies an exit code (-1: killed by a signal) and stderr of `[pkexec] ip`.
[[nodiscard]] IpResult ip_result_classify(int exit_code, std::string_view err) noexcept;

// Runs `[pkexec] ip args...` and waits; logs stderr on failure. Blocking.
IpResult socketcan_run_ip(const std::vector<std::string>& args);

// Auto-baud (CAN Status): what one listen-only probe saw.
struct BaudProbe
{
    unsigned frames = 0; // valid data/remote frames
    unsigned errors = 0; // error frames (bus/protocol errors at a wrong rate)
};

// A probe hits with >= 2 valid frames and no error frame (idle bus or wrong rate otherwise).
[[nodiscard]] bool autobaud_hit(const BaudProbe& p) noexcept;

// Probes the classic bitrates from 1M down, each listen-only for a short window. Hit: the
// link is left up at that rate with the rest of timing; none: left down, timing restored.
// An `ip` failure that is not a rejected bitrate stops the scan: ip != ok, bitrate empty.
// Blocking (several `ip` calls): run it on a worker thread.
struct AutobaudResult
{
    std::optional<unsigned> bitrate;
    IpResult ip = IpResult::ok;
};
AutobaudResult socketcan_autobaud(const std::string& name, IfaceConfig timing);

// IFF_UP from /sys/class/net/<name>/flags; false when the link does not exist. Cheap (one read).
[[nodiscard]] bool socketcan_link_up(const std::string& name);

// The link exists (/sys/class/net/<name>).
[[nodiscard]] bool socketcan_link_exists(const std::string& name);

// Linux only: elsewhere the functions above fail and the UI leaves out the vcan / link controls.
#ifdef _WIN32
inline constexpr bool socketcan_available = false;
#else
inline constexpr bool socketcan_available = true;
#endif
