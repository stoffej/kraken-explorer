/*
  Copyright (c) 2022 Ethan Zonca
  Copyright (c) 2024 CANgaroo Contributors
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

// SLCAN (Lawicel ASCII) over core/serial: CANable 1.0/2.0 and WeAct Studio USB2CAN.
// Frame decoding lives in slcan_codec.h. send() writes the line directly; the TX
// report is handed back by read() once the device confirms it (bare CR, BEL = NACK),
// or right away for devices that send no confirmations.

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/serial.h"
#include "drivers/driver.h"
#include "drivers/slcan_codec.h"

namespace
{

using namespace std::chrono_literals;

constexpr int port_baud = 1000000; // USB CDC: ignored by the device, any valid value works

struct Slcan
{
    SerialPort port;
    bool no_confirm = false;   // set in open(), read-only afterwards
    std::string rx_line;       // listener thread only

    std::mutex tx_mutex;             // serialises writes and guards both queues
    std::deque<BusMessage> tx_wait;  // written, waiting for the device's CR / BEL
    std::deque<BusMessage> tx_done;  // written, report without confirmation (no_confirm)

    std::atomic<uint64_t> rx_frames{0};
    std::atomic<uint64_t> rx_errors{0};
    std::atomic<uint64_t> tx_frames{0};
    std::atomic<uint64_t> tx_errors{0};
    std::atomic<uint64_t> tx_dropped{0};
};

struct RateCmd
{
    unsigned bitrate;
    const char* cmd;
};

// Nominal bitrate -> 'S' command, FD data bitrate -> 'Y' command.
constexpr RateCmd bitrate_cmds[] = {
    {1000000, "S8"}, {800000, "S7"}, {500000, "S6"}, {250000, "S5"}, {125000, "S4"},
    {100000, "S3"},  {83333, "S9"},  {75000, "SA"},  {62500, "SB"},  {50000, "S2"},
    {33333, "SC"},   {20000, "S1"},  {10000, "S0"},  {5000, "SD"},
};
constexpr RateCmd fd_bitrate_cmds[] = {
    {1000000, "Y1"}, {2000000, "Y2"}, {3000000, "Y3"}, {4000000, "Y4"}, {5000000, "Y5"},
};

struct Model
{
    uint16_t vid;
    uint16_t pid;
    const char* serial_prefix;   // nullptr = any
    bool canfd;
    bool weact;
    const char* details;
};

constexpr Model models[] = {
    {0xAD50, 0x60C4, nullptr, false, false, "CANable with standard CAN support"},
    {0x0403, 0x6015, nullptr, false, false, "CANable with standard CAN support"},
    {0x16D0, 0x117E, nullptr, true, false, "CANable with CANFD support"},
    {0x0483, 0x5740, "AAA", true, true, "WeAct Studio USB2CAN with CANFD support"},
};

std::vector<CanTiming> model_bitrates(const Model& m)
{
    static constexpr unsigned canable[] = {10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000};
    static constexpr unsigned canable_fd[] = {2000000, 5000000};
    static constexpr unsigned weact[] = {5000,   10000,  20000,  33333,  50000,  62500,  75000,
                                         83333,  100000, 125000, 250000, 500000, 800000, 1000000};
    static constexpr unsigned weact_fd[] = {1000000, 2000000, 3000000, 4000000, 5000000};
    const std::span<const unsigned> br = m.weact ? std::span<const unsigned>(weact) : canable;
    const std::span<const unsigned> fd = m.weact ? std::span<const unsigned>(weact_fd) : canable_fd;
    std::vector<CanTiming> out;
    for (unsigned b : br)
    {
        for (unsigned f : fd)
        {
            out.push_back({.bitrate = b, .bitrate_fd = f, .sample_point = 875, .sample_point_fd = 750});
        }
    }
    return out;
}

void slcan_enumerate(std::vector<IfaceInfo>& out)
{
    for (const auto& p : serial_list_ports())
    {
        for (const auto& m : models)
        {
            if (p.vid != m.vid || p.pid != m.pid || (m.serial_prefix && !p.serial.starts_with(m.serial_prefix)))
            {
                continue;
            }
            uint32_t caps = m.weact ? iface_cap::listen_only | iface_cap::custom_bitrate | iface_cap::custom_canfd_bitrate
                                    : iface_cap::config_os | iface_cap::auto_restart | iface_cap::listen_only;
            if (m.canfd)
            {
                caps |= iface_cap::canfd;
            }
            // Workspaces store the bare port name ("ttyACM0", "COM3"), as the Qt version did.
            out.push_back({.name = std::filesystem::path(p.name).filename().string(),
                           .details = m.details,
                           .version = {},
                           .bus_type = BusType::CAN,
                           .capabilities = caps,
                           .bitrates = model_bitrates(m)});
            break;
        }
    }
}

std::string port_path(const std::string& name)
{
#ifdef _WIN32
    return name; // "COM7"
#else
    return name.starts_with('/') ? name : "/dev/" + name;
#endif
}

void write_cmd(Slcan& s, std::string_view cmd)
{
    std::string line(cmd);
    line += '\r';
    serial_write(s.port, line.data(), line.size());
}

// Reads until '\r' or the timeout; returns the line without it.
std::string read_reply(Slcan& s, std::chrono::milliseconds timeout)
{
    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char c = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (serial_read(s.port, &c, 1, std::max(left, 1ms)) != 1 || c == '\r')
        {
            break;
        }
        reply += c;
    }
    return reply;
}

void write_rate(Slcan& s, char prefix, bool custom, uint32_t custom_value, unsigned bitrate, std::span<const RateCmd> table,
                const char* fallback)
{
    if (custom)
    {
        write_cmd(s, std::format("{}{:06X}", prefix, custom_value & 0xFFFFFF));
        return;
    }
    for (const auto& e : table)
    {
        if (e.bitrate == bitrate)
        {
            write_cmd(s, e.cmd);
            return;
        }
    }
    write_cmd(s, fallback);
}

bool slcan_open(Iface& iface, const IfaceConfig& config)
{
    auto s = std::make_unique<Slcan>();
    const std::string path = port_path(iface.info.name);
    if (!serial_open(s->port, path, port_baud))
    {
        log_error(std::format("SLCAN: failed to open {}: {}", path, s->port.error));
        return false;
    }
    serial_clear(s->port);

    // Close any open channel; a device in confirm mode answers with CR (or BEL).
    write_cmd(*s, "C");
    char buf[64];
    s->no_confirm = serial_read(s->port, buf, sizeof(buf), 50ms) <= 0;

    serial_clear(s->port);
    write_cmd(*s, "V");
    iface.info.version = read_reply(*s, 50ms);

    write_rate(*s, 'S', config.is_custom_bitrate, config.custom_bitrate, config.bitrate, bitrate_cmds, "S6");
    if (iface.info.capabilities & iface_cap::canfd)
    {
        write_rate(*s, 'Y', config.is_custom_fd_bitrate, config.custom_fd_bitrate, config.fd_bitrate, fd_bitrate_cmds,
                   "Y2");
    }
    write_cmd(*s, config.listen_only ? "M1" : "M0");
    write_cmd(*s, "O");
    // Discard the replies to the setup commands.
    while (serial_read(s->port, buf, sizeof(buf), 10ms) > 0)
    {
    }
    iface_set_impl(iface, std::move(s));
    return true;
}

void slcan_close(Iface& iface)
{
    auto* s = static_cast<Slcan*>(iface.impl.get());
    if (!s || !serial_is_open(s->port))
    {
        return;
    }
    write_cmd(*s, "C");
    char buf[64];
    (void)serial_read(s->port, buf, sizeof(buf), 20ms);
    serial_clear(s->port);
    serial_close(s->port);
}

// SLCAN line incl. the trailing CR; empty if the frame cannot be expressed.
std::string encode_frame(const BusMessage& msg)
{
    const bool ext = has_flag(msg, bus_flag::extended);
    const bool fd = has_flag(msg, bus_flag::fd);
    const bool rtr = has_flag(msg, bus_flag::rtr);
    if (msg.len > bus_max_data_bytes || (fd && rtr) || (!fd && msg.len > 8))
    {
        return {};
    }
    char type = 't';
    if (fd)
    {
        type = has_flag(msg, bus_flag::brs) ? 'b' : 'd';
    }
    else if (rtr)
    {
        type = 'r';
    }
    if (ext)
    {
        type = static_cast<char>(type - 'a' + 'A'); // same letters parse_frame_line() reads
    }
    const int id_len = ext ? slcan::ext_id_len : slcan::std_id_len;
    std::string line(1, type);
    uint32_t id = can_id(msg);
    line.resize(1 + id_len);
    for (int i = id_len; i >= 1; --i, id >>= 4)
    {
        line[i] = slcan::hex_nibble(static_cast<uint8_t>(id & 0xF));
    }
    const uint8_t dlc = bus_length_to_dlc(msg.len);
    line += slcan::hex_nibble(dlc);
    if (!rtr)
    {
        for (int i = 0; i < bus_dlc_lengths[dlc]; ++i) // FD lengths round up, data is zero-padded
        {
            line += slcan::hex_nibble(msg.data[i] >> 4);
            line += slcan::hex_nibble(msg.data[i] & 0xF);
        }
    }
    line += '\r';
    return line;
}

bool slcan_send(Iface& iface, const BusMessage& msg)
{
    auto& s = iface_impl<Slcan>(iface);
    const std::string line = encode_frame(msg);
    if (line.empty())
    {
        ++s.tx_dropped;
        return false;
    }
    // Held across the write so a confirmation can never overtake the queue entry.
    std::scoped_lock lock(s.tx_mutex);
    if (!serial_write(s.port, line.data(), line.size()))
    {
        ++s.tx_errors;
        ++s.tx_dropped;
        log_error(std::format("SLCAN: error writing to {}: {}", iface.info.name, s.port.error));
        return false;
    }
    BusMessage tx = msg;
    tx.flags |= bus_flag::tx;
    tx.iface = iface.index;
    (s.no_confirm ? s.tx_done : s.tx_wait).push_back(tx);
    return true;
}

// Moves the oldest unconfirmed TX frame to out (ACK) or counts it failed (NACK).
void confirm_tx(Slcan& s, BusMessage* out, int& n, bool ok)
{
    std::scoped_lock lock(s.tx_mutex);
    if (s.tx_wait.empty())
    {
        return; // reply to a setup command
    }
    BusMessage tx = s.tx_wait.front();
    s.tx_wait.pop_front();
    if (!ok)
    {
        ++s.tx_errors;
        return;
    }
    tx.ts_ns = now_ns();
    out[n++] = tx;
    ++s.tx_frames;
}

int slcan_read(Iface& iface, BusMessage* out, int max, int timeout_ms)
{
    auto& s = iface_impl<Slcan>(iface);
    int n = 0;
    {
        std::scoped_lock lock(s.tx_mutex);
        const int64_t ts = now_ns();
        for (; n < max && !s.tx_done.empty(); ++n)
        {
            out[n] = s.tx_done.front();
            out[n].ts_ns = ts;
            s.tx_done.pop_front();
            ++s.tx_frames;
        }
    }
    if (n == max)
    {
        return n;
    }

    // Every byte completes at most one frame, so max - n bytes can never overflow out.
    char buf[4096];
    const auto want = std::min<std::size_t>(sizeof(buf), static_cast<std::size_t>(max - n));
    const long got = serial_read(s.port, buf, want, std::chrono::milliseconds(n > 0 ? 0 : timeout_ms));
    if (got < 0)
    {
        log_error(std::format("SLCAN: {} lost: {}", iface.info.name, s.port.error));
        return -1;
    }
    const int64_t ts = now_ns();
    for (long i = 0; i < got; ++i)
    {
        const char c = buf[i];
        if (c == '\r')
        {
            if (s.rx_line.empty())
            {
                if (!s.no_confirm)
                {
                    confirm_tx(s, out, n, true);
                }
            }
            else if (BusMessage msg; slcan::parse_frame_line(s.rx_line, msg))
            {
                msg.iface = iface.index;
                msg.ts_ns = ts;
                out[n++] = msg;
                ++s.rx_frames;
            }
            else
            {
                ++s.rx_errors;
            }
            s.rx_line.clear();
        }
        else if (c == '\x07')
        {
            if (!s.rx_line.empty())
            {
                ++s.rx_errors; // partial RX line discarded
            }
            else if (!s.no_confirm)
            {
                confirm_tx(s, out, n, false);
            }
            s.rx_line.clear();
        }
        else if (s.rx_line.size() < 256) // longest valid line: B + 8 id + dlc + 128 data
        {
            s.rx_line += c;
        }
    }
    return n;
}

void slcan_stats(Iface& iface, IfaceStats& out)
{
    auto& s = iface_impl<Slcan>(iface);
    out.state = IfaceState::Ok;
    out.rx_frames = s.rx_frames;
    out.rx_errors = s.rx_errors;
    out.tx_frames = s.tx_frames;
    out.tx_errors = s.tx_errors;
    out.tx_dropped = s.tx_dropped;
}

} // namespace

extern const DriverOps slcan_driver = {
    .name = "SLCAN",
    .enumerate = slcan_enumerate,
    .open = slcan_open,
    .close = slcan_close,
    .send = slcan_send,
    .read = slcan_read,
    .stats = slcan_stats,
};
