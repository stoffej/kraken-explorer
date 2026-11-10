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

#include "decoders/uds_client.h"

#include "core/text.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <thread>
#include <utility>

#include "decoders/decoders.h"

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int64_t pairing_window_ns = 2'000'000'000; // an 11-bit answer belongs to a request this recent
constexpr std::size_t max_pending = 32;
constexpr int max_fc_wait = 10; // N_WFTmax: flow control WAITs in a row before giving up

// The SIDs ISO 14229-1 assigns to requests.
constexpr std::array<uint8_t, 27> request_sids = {
    0x10, 0x11, 0x14, 0x19, 0x22, 0x23, 0x24, 0x27, 0x28, 0x29, 0x2A, 0x2C, 0x2E, 0x2F,
    0x31, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3D, 0x3E, 0x83, 0x84, 0x85, 0x86, 0x87};

// The services the service scan probes: only reads (and TesterPresent). A probe of a write,
// reset, routine or download service can change the ECU (a short 2E / 14 / 31 an ECU takes
// as valid), so those are never sent; they show up as not probed.
constexpr std::array<uint8_t, 5> read_sids = {0x19, 0x22, 0x23, 0x24, 0x3E};

bool functional_id(const UdsTarget& t)
{
    return t.extended ? ((t.tx_id >> 16) & 0xFF) == 0xDB : t.tx_id == 0x7DF;
}

// Services with a sub-function byte, whose bit 7 is suppressPosRspMsgIndicationBit.
bool has_suppress_bit(uint8_t sid)
{
    switch (sid)
    {
    case 0x10: case 0x11: case 0x19: case 0x27: case 0x28: case 0x29: case 0x2C: case 0x31: case 0x3E:
    case 0x83: case 0x85: case 0x86: case 0x87:
        return true;
    default:
        return false;
    }
}

// The SIDs ISO 14229-1 assigns; anything else in a first byte is not a UDS request.
bool is_request_sid(uint8_t sid)
{
    return std::ranges::find(request_sids, sid) != request_sids.end();
}

// Smallest frame length (CAN FD step, or 8) that holds `used` bytes on a target.
int pad_len(const UdsTarget& t, int used)
{
    if (!t.fd)
    {
        return 8;
    }
    const int cap = std::clamp<int>(t.tx_len, 8, bus_max_data_bytes);
    return std::min<int>(bus_dlc_lengths[bus_length_to_dlc(used)], cap);
}

// Largest ISO-TP frame this target sends (classic 8, else the chosen CAN FD length).
int frame_cap(const UdsTarget& t)
{
    return t.fd ? std::clamp<int>(t.tx_len, 8, bus_max_data_bytes) : 8;
}

// Registered for its lifetime, so a reply that comes before the request call returns from
// send() is not missed.
struct WaiterScope
{
    UdsClient& c;
    UdsWaiter w;

    WaiterScope(UdsClient& client, UdsWaiter waiter) : c(client), w(std::move(waiter))
    {
        const std::scoped_lock lock(c.mutex);
        c.waiters.push_back(&w);
        c.active.store(static_cast<int>(c.waiters.size()), std::memory_order_relaxed);
    }
    ~WaiterScope()
    {
        const std::scoped_lock lock(c.mutex);
        std::erase(c.waiters, &w);
        c.active.store(static_cast<int>(c.waiters.size()), std::memory_order_relaxed);
    }
    WaiterScope(const WaiterScope&) = delete;
    WaiterScope& operator=(const WaiterScope&) = delete;

    std::optional<BusMessage> next(Clock::time_point deadline)
    {
        std::unique_lock lock(c.mutex);
        if (!c.cv.wait_until(lock, deadline, [this] { return !w.frames.empty() || c.cancel.load(); }) || c.cancel.load())
        {
            return std::nullopt;
        }
        const BusMessage m = w.frames.front();
        w.frames.erase(w.frames.begin());
        return m;
    }
};

// A frame on `id` carrying `bytes` (already including the ISO-TP PCI and any address extension
// byte), padded to a legal length with the target's padding byte.
BusMessage make_frame(const UdsTarget& t, uint32_t id, std::span<const uint8_t> bytes)
{
    BusMessage m{};
    m.id = id;
    m.iface = t.iface;
    m.flags = static_cast<uint16_t>((t.extended ? bus_flag::extended : 0)
                                    | (t.fd ? bus_flag::fd : 0) | (t.fd && t.brs ? bus_flag::brs : 0));
    const int cap = frame_cap(t);
    const int n = std::min<int>(static_cast<int>(bytes.size()), cap);
    m.data.fill(t.padding);
    std::copy_n(bytes.begin(), n, m.data.begin());
    set_length(m, pad_len(t, n));
    return m;
}

// Prepends the address extension byte (when the target uses one) to an ISO-TP PDU and sends it.
bool send_pdu(const UdsSend& send, const UdsTarget& t, uint32_t id, std::span<const uint8_t> pdu)
{
    if (!t.tx_ae)
    {
        return send(make_frame(t, id, pdu));
    }
    std::array<uint8_t, bus_max_data_bytes> buf{};
    buf[0] = *t.tx_ae;
    const int n = std::min<int>(static_cast<int>(pdu.size()), bus_max_data_bytes - 1);
    std::copy_n(pdu.begin(), n, buf.begin() + 1);
    return send(make_frame(t, id, std::span(buf).first(n + 1)));
}

// The ISO-TP bytes of a received frame (past the address extension byte), and its length.
std::span<const uint8_t> tp_bytes(const UdsTarget& t, const BusMessage& m)
{
    const int off = t.rx_ae ? 1 : 0;
    const int len = std::min<int>(m.len, bus_max_data_bytes);
    if (len <= off)
    {
        return {};
    }
    return std::span<const uint8_t>(m.data.data() + off, static_cast<std::size_t>(len - off));
}

// First UDS byte of an ISO-TP single or first frame (nullopt for CF / FC / anything else).
// ae is the number of address-extension bytes leading the frame (0 or 1).
std::optional<std::span<const uint8_t>> first_payload(const BusMessage& m, int ae = 0)
{
    const int len = std::min<int>(m.len, bus_max_data_bytes) - ae;
    const uint8_t* p = m.data.data() + ae;
    if (len < 2)
    {
        return std::nullopt;
    }
    const uint8_t pci = p[0] >> 4;
    if (pci == 0)
    {
        int n = p[0] & 0x0F;
        int off = 1;
        if (n == 0 && len > 1) // CAN FD long single frame
        {
            n = p[1];
            off = 2;
        }
        if (n < 1 || n > len - off)
        {
            return std::nullopt;
        }
        return std::span<const uint8_t>(p + off, static_cast<std::size_t>(n));
    }
    if (pci == 1 && len >= 3)
    {
        int off = 2;
        if ((p[0] & 0x0F) == 0 && p[1] == 0 && len >= 6) // CAN FD long first frame
        {
            off = 6;
        }
        if (off >= len)
        {
            return std::nullopt;
        }
        return std::span<const uint8_t>(p + off, static_cast<std::size_t>(len - off));
    }
    return std::nullopt;
}

// STmin byte of a flow control (ISO 15765-2): 0..0x7F ms, 0xF1..0xF9 100..900 us, else 127 ms.
std::chrono::microseconds st_min(uint8_t v)
{
    if (v <= 0x7F)
    {
        return std::chrono::milliseconds(v);
    }
    if (v >= 0xF1 && v <= 0xF9)
    {
        return std::chrono::microseconds((v - 0xF0) * 100);
    }
    return std::chrono::milliseconds(127);
}

} // namespace

void uds_client_cancel(UdsClient& c)
{
    c.cancel.store(true);
    c.cv.notify_all();
}

void uds_client_rx(void* client, const BusMessage& msg)
{
    UdsClient& c = *static_cast<UdsClient*>(client);
    if (c.active.load(std::memory_order_relaxed) == 0 || has_flag(msg, bus_flag::tx) || is_error_frame(msg)
        || has_flag(msg, bus_flag::rtr) || msg.type != BusType::CAN)
    {
        return;
    }
    const bool ext = has_flag(msg, bus_flag::extended);
    bool queued = false;
    {
        const std::scoped_lock lock(c.mutex);
        for (UdsWaiter* w : c.waiters)
        {
            if (w->iface != msg.iface || w->extended != ext || !(w->any_id || w->id == can_id(msg)))
            {
                continue;
            }
            if (w->ae && (msg.len < 1 || msg.data[0] != *w->ae))
            {
                continue;
            }
            w->frames.push_back(msg);
            queued = true;
        }
    }
    if (queued)
    {
        c.cv.notify_all();
    }
}

UdsResult uds_request(UdsClient& c, const UdsSend& send, const UdsTarget& t, std::span<const uint8_t> request,
                      const UdsTiming& timing)
{
    UdsResult r;
    if (request.empty() || request.size() > 4095)
    {
        r.error = "request must be 1..4095 bytes";
        return r;
    }
    const uint8_t sid = request[0];
    // ISO-TP bytes a frame can carry: the whole frame, minus the address extension byte that
    // send_pdu puts in front (without this a 7-byte request with tx_ae lost its last byte).
    const int cap = frame_cap(t) - (t.tx_ae ? 1 : 0);
    const int ro = t.rx_ae ? 1 : 0;               // address-extension offset on received frames
    const auto send_error = [&t] { return std::format("interface {} not open or send failed", t.iface); };
    WaiterScope scope(c, {.iface = t.iface, .extended = t.extended, .id = t.rx_id, .ae = t.rx_ae});

    // Largest single frame payload: classic 1-byte PCI, FD long form uses a 2-byte PCI.
    const int sf_small = cap - 1;
    if (static_cast<int>(request.size()) <= sf_small && request.size() <= 7)
    {
        std::array<uint8_t, bus_max_data_bytes> sf{};
        sf[0] = static_cast<uint8_t>(request.size());
        std::copy(request.begin(), request.end(), sf.begin() + 1);
        if (!send_pdu(send, t, t.tx_id, std::span(sf).first(request.size() + 1)))
        {
            r.error = send_error();
            return r;
        }
    }
    else if (t.fd && static_cast<int>(request.size()) <= cap - 2)
    {
        // CAN FD long single frame: 0x00, length, payload.
        std::array<uint8_t, bus_max_data_bytes> sf{};
        sf[0] = 0x00;
        sf[1] = static_cast<uint8_t>(request.size());
        std::copy(request.begin(), request.end(), sf.begin() + 2);
        if (!send_pdu(send, t, t.tx_id, std::span(sf).first(request.size() + 2)))
        {
            r.error = send_error();
            return r;
        }
    }
    else
    {
        std::array<uint8_t, bus_max_data_bytes> ff{};
        ff[0] = static_cast<uint8_t>(0x10 | (request.size() >> 8));
        ff[1] = static_cast<uint8_t>(request.size() & 0xFF);
        const int first = std::min<int>(cap - 2, static_cast<int>(request.size()));
        std::copy_n(request.begin(), first, ff.begin() + 2);
        if (!send_pdu(send, t, t.tx_id, std::span(ff).first(first + 2)))
        {
            r.error = send_error();
            return r;
        }
        std::size_t at = static_cast<std::size_t>(first);
        uint8_t sn = 1;
        while (at < request.size())
        {
            // Wait for the flow control: CTS (go), WAIT (keep waiting), overflow (give up).
            uint8_t block = 0;
            std::chrono::microseconds gap{};
            int waits = 0;
            for (auto deadline = Clock::now() + timing.n_bs;;)
            {
                const std::optional<BusMessage> m = scope.next(deadline);
                if (!m)
                {
                    r.error = c.cancel.load() ? "cancelled" : "no flow control from the ECU (N_Bs)";
                    return r;
                }
                const std::span<const uint8_t> fcb = tp_bytes(t, *m);
                if (fcb.size() < 3 || (fcb[0] >> 4) != 3)
                {
                    continue;
                }
                const uint8_t fs = fcb[0] & 0x0F;
                if (fs == 1)
                {
                    if (++waits > max_fc_wait)
                    {
                        r.error = "the ECU kept asking to wait (N_WFTmax)";
                        return r;
                    }
                    deadline = Clock::now() + timing.n_bs;
                    continue;
                }
                if (fs != 0)
                {
                    r.error = "the ECU refused the request size (flow control overflow)";
                    return r;
                }
                block = fcb[1];
                gap = st_min(fcb[2]);
                break;
            }
            for (int sent = 0; at < request.size() && (block == 0 || sent < block); ++sent)
            {
                std::array<uint8_t, bus_max_data_bytes> cf{};
                cf[0] = static_cast<uint8_t>(0x20 | sn);
                const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(cap - 1), request.size() - at);
                std::copy_n(request.begin() + at, n, cf.begin() + 1);
                if (!send_pdu(send, t, t.tx_id, std::span(cf).first(n + 1)))
                {
                    r.error = send_error();
                    return r;
                }
                at += n;
                sn = static_cast<uint8_t>((sn + 1) & 0x0F);
                if (at < request.size() && gap.count() > 0)
                {
                    std::this_thread::sleep_for(gap);
                }
            }
        }
    }

    if (request.size() >= 2 && has_suppress_bit(sid) && (request[1] & 0x80) != 0)
    {
        r.suppressed = true;
        return r;
    }

    bool pending = false; // NRC 0x78 seen: the wait is P2*
    for (auto deadline = Clock::now() + timing.p2;;)
    {
        const std::optional<BusMessage> m = scope.next(deadline);
        if (!m)
        {
            r.error = c.cancel.load() ? "cancelled"
                                      : std::format("no response within {} ms{}", (pending ? timing.p2_star : timing.p2).count(),
                                                    pending ? " after response pending" : "");
            return r;
        }
        const int len = std::min<int>(m->len, bus_max_data_bytes) - ro;
        if (len < 1)
        {
            continue;
        }
        const uint8_t* b = m->data.data() + ro;
        const uint8_t pci = b[0] >> 4;
        std::vector<uint8_t> payload;
        if (pci == 0)
        {
            int n = b[0] & 0x0F;
            int off = 1;
            if (n == 0 && len > 1) // CAN FD long single frame
            {
                n = b[1];
                off = 2;
            }
            n = std::min(n, len - off);
            payload.assign(b + off, b + off + std::max(n, 0));
        }
        else if (pci == 1 && len >= 3)
        {
            std::size_t size = static_cast<std::size_t>((b[0] & 0x0F) << 8 | b[1]);
            int off = 2;
            if ((b[0] & 0x0F) == 0 && b[1] == 0 && len >= 6) // CAN FD long first frame: 32-bit length
            {
                size = (static_cast<std::size_t>(b[2]) << 24) | (static_cast<std::size_t>(b[3]) << 16)
                       | (static_cast<std::size_t>(b[4]) << 8) | b[5];
                off = 6;
            }
            if (size <= static_cast<std::size_t>(len - off))
            {
                continue; // not a valid first frame
            }
            if (functional_id(t))
            {
                // A flow control may not go to a functional address: the caller needs the physical id.
                r.error = "multi-frame response to a functional request: use the ECU's physical request id";
                return r;
            }
            payload.assign(b + off, b + len);
            // Clear to send everything, no gap: the tester reads as fast as frames come.
            const std::array<uint8_t, 3> fc = {0x30, 0x00, 0x00};
            if (!send_pdu(send, t, t.tx_id, fc))
            {
                r.error = send_error();
                return r;
            }
            uint8_t sn = 1;
            while (payload.size() < size)
            {
                const std::optional<BusMessage> cf = scope.next(Clock::now() + timing.n_cr);
                if (!cf)
                {
                    r.error = std::format("response cut off after {} of {} bytes (N_Cr)", payload.size(), size);
                    return r;
                }
                const int clen = std::min<int>(cf->len, bus_max_data_bytes) - ro;
                if (clen < 1)
                {
                    continue;
                }
                const uint8_t* cb = cf->data.data() + ro;
                if ((cb[0] >> 4) != 2)
                {
                    continue;
                }
                if ((cb[0] & 0x0F) != sn)
                {
                    r.error = std::format("consecutive frame {} where {} was expected", cb[0] & 0x0F, sn);
                    return r;
                }
                const int n = std::min<int>(clen - 1, static_cast<int>(size - payload.size()));
                payload.insert(payload.end(), cb + 1, cb + 1 + std::max(n, 0));
                sn = static_cast<uint8_t>((sn + 1) & 0x0F);
            }
        }
        if (payload.empty())
        {
            continue; // flow control, stray consecutive frame
        }
        if (payload[0] == 0x7F && payload.size() >= 3 && payload[1] == sid)
        {
            if (payload[2] == 0x78) // response pending
            {
                pending = true;
                deadline = Clock::now() + timing.p2_star;
                continue;
            }
            r.response = std::move(payload);
            return r;
        }
        if (payload[0] == static_cast<uint8_t>(sid + 0x40))
        {
            r.response = std::move(payload);
            return r;
        }
        // Someone else's answer on the same id: keep waiting for ours.
    }
}

std::vector<UdsResponder> uds_functional(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                         std::span<const uint8_t> request, std::chrono::milliseconds window)
{
    std::vector<UdsResponder> out;
    // One single frame: 7 ISO-TP bytes on classic CAN, one fewer with an address extension byte.
    if (request.empty() || request.size() + (t.tx_ae ? 1 : 0) > 7)
    {
        return out;
    }
    const uint8_t sid = request[0];
    const int ae = t.rx_ae ? 1 : 0;
    WaiterScope scope(c, {.iface = t.iface, .extended = t.extended, .any_id = true, .ae = t.rx_ae});
    std::array<uint8_t, 8> sf{};
    sf[0] = static_cast<uint8_t>(request.size());
    std::copy(request.begin(), request.end(), sf.begin() + 1);
    if (!send_pdu(send, t, t.tx_id, std::span(sf).first(request.size() + 1)))
    {
        return out;
    }
    const uint8_t tester = static_cast<uint8_t>(t.tx_id & 0xFF);
    for (const auto deadline = Clock::now() + window;;)
    {
        const std::optional<BusMessage> m = scope.next(deadline);
        if (!m)
        {
            return out;
        }
        const uint32_t id = can_id(*m);
        // 29-bit: only physical answers to this tester (0x18DA<tester><ECU>).
        if (t.extended && (((id >> 16) & 0xFF) != 0xDA || ((id >> 8) & 0xFF) != tester))
        {
            continue;
        }
        const auto payload = first_payload(*m, ae);
        if (!payload || ((m->data[ae] >> 4) != 0))
        {
            continue;
        }
        const std::span<const uint8_t> p = *payload;
        const bool answer = p[0] == static_cast<uint8_t>(sid + 0x40) || (p[0] == 0x7F && p.size() >= 2 && p[1] == sid);
        if (answer && std::ranges::none_of(out, [id](const UdsResponder& r) { return r.rx_id == id; }))
        {
            out.push_back({.rx_id = id, .response = {p.begin(), p.end()}});
        }
    }
}

std::vector<UdsEcu> uds_scan(UdsClient& c, const UdsSend& send, uint16_t iface, std::chrono::milliseconds window,
                             const UdsTarget& transport)
{
    std::vector<UdsEcu> found;
    const std::array<uint8_t, 2> tester_present = {0x3E, 0x00};
    for (const bool ext : {false, true})
    {
        UdsTarget t = transport;
        t.iface = iface;
        t.tx_id = ext ? 0x18DB33F1u : 0x7DFu;
        t.extended = ext;
        for (const UdsResponder& r : uds_functional(c, send, t, tester_present, window))
        {
            found.push_back(uds_ecu_from_responder(t, r.rx_id));
        }
    }
    return found;
}

std::vector<UdsEcu> uds_sweep(UdsClient& c, const UdsSend& send, const UdsSweep& sweep,
                              const std::function<void(const UdsEcu&)>& on_found, const std::atomic<bool>* stop)
{
    std::vector<UdsEcu> found;
    const int ae = sweep.transport.rx_ae ? 1 : 0;
    if (sweep.request.empty() || sweep.request.size() + (sweep.transport.tx_ae ? 1 : 0) > 7)
    {
        return found; // the probe must fit one single frame
    }
    std::array<uint8_t, 8> sf{}; // single frame: PCI length byte, then the request
    sf[0] = static_cast<uint8_t>(sweep.request.size());
    std::ranges::copy(sweep.request, sf.begin() + 1);
    const auto probe = std::span(sf).first(sweep.request.size() + 1);

    const auto report = [&](const UdsEcu& e)
    {
        if (std::ranges::any_of(found, [&](const UdsEcu& f) { return f.rx_id == e.rx_id && f.extended == e.extended; }))
        {
            return;
        }
        found.push_back(e);
        if (on_found)
        {
            on_found(e);
        }
    };

    // Every id tried in turn; one waiter catches answers on any id while we move on.
    WaiterScope scope(c, {.iface = sweep.transport.iface, .extended = sweep.transport.extended,
                          .any_id = true, .ae = sweep.transport.rx_ae});

    const auto drain = [&](uint32_t tx_id, Clock::time_point until)
    {
        for (;;)
        {
            const std::optional<BusMessage> m = scope.next(until);
            if (!m)
            {
                return;
            }
            const uint32_t id = can_id(*m);
            const auto payload = first_payload(*m, ae);
            if (!payload)
            {
                continue;
            }
            const uint8_t s = (*payload)[0];
            const bool uds = s == 0x7F || (s >= 0x40 && is_request_sid(static_cast<uint8_t>(s - 0x40)));
            if (!uds)
            {
                continue;
            }
            UdsEcu e{.iface = sweep.transport.iface, .extended = sweep.transport.extended, .rx_id = id};
            if (sweep.transport.extended)
            {
                // Only physical answers to this tester count, 0x18DA<tester><node>: J1939 traffic
                // on the bus (0x18FExxyy ...) can look like UDS and must not become an ECU.
                if (((id >> 16) & 0xFF) != 0xDA || ((id >> 8) & 0xFF) != sweep.tester)
                {
                    continue;
                }
                // The ECU answered on 0x18DA<tester><node>; its request id is 0x18DA<node><tester>.
                const uint32_t node = id & 0xFF;
                const uint32_t tester = (id >> 8) & 0xFF;
                e.tx_id = (id & 0x1FFF0000u) | (node << 8) | tester;
            }
            else
            {
                // The request went to the id just sent; an answer that is not exactly tx + 8 (the
                // OBD convention) makes the pairing a guess.
                e.tx_id = tx_id;
                e.guessed = id != tx_id + 8;
            }
            report(e);
        }
    };

    if (sweep.transport.extended)
    {
        std::vector<uint8_t> sas = sweep.candidates;
        if (sas.empty())
        {
            sas.resize(256);
            for (int i = 0; i < 256; ++i)
            {
                sas[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
            }
        }
        for (const uint8_t sa : sas)
        {
            if (stop && stop->load())
            {
                break;
            }
            UdsTarget t = sweep.transport;
            t.tx_id = 0x18DA0000u | (static_cast<uint32_t>(sa) << 8) | sweep.tester;
            (void)send_pdu(send, t, t.tx_id, probe);
            drain(t.tx_id, Clock::now() + sweep.wait);
        }
    }
    else
    {
        for (uint32_t id = sweep.first; id <= sweep.last && id <= can_id_mask_standard; ++id)
        {
            if (stop && stop->load())
            {
                break;
            }
            UdsTarget t = sweep.transport;
            t.tx_id = id;
            (void)send_pdu(send, t, id, probe);
            drain(id, Clock::now() + sweep.wait);
        }
    }
    return found;
}

std::vector<UdsServiceProbe> uds_service_scan(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                              const UdsTiming& timing, const std::atomic<bool>* stop)
{
    std::vector<UdsServiceProbe> out;
    for (const uint8_t sid : read_sids)
    {
        if (stop && stop->load())
        {
            break;
        }
        // The smallest well-formed request: services with a sub-function get 0x00, the rest just the SID.
        std::vector<uint8_t> req = {sid};
        if (has_suppress_bit(sid) || sid == 0x22)
        {
            req.push_back(0x00);
        }
        const UdsResult r = uds_request(c, send, t, req, timing);
        UdsServiceProbe p{.sid = sid};
        if (r.error.empty())
        {
            p.answered = true;
            p.nrc = (r.response.size() >= 3 && r.response[0] == 0x7F) ? r.response[2] : 0;
        }
        else if (!r.error.starts_with("no response"))
        {
            // A transport failure (interface closed) is worth stopping for.
            break;
        }
        out.push_back(p);
    }
    return out;
}

std::vector<UdsDidValue> uds_did_scan(UdsClient& c, const UdsSend& send, const UdsTarget& t, uint16_t first,
                                      uint16_t last, const UdsTiming& timing,
                                      const std::function<void(const UdsDidValue&)>& on_found,
                                      const std::atomic<bool>* stop)
{
    std::vector<UdsDidValue> out;
    for (uint32_t did = first; did <= last; ++did)
    {
        if (stop && stop->load())
        {
            break;
        }
        const std::array<uint8_t, 3> req = {0x22, static_cast<uint8_t>(did >> 8), static_cast<uint8_t>(did)};
        const UdsResult r = uds_request(c, send, t, req, timing);
        if (!r.error.empty())
        {
            if (!r.error.starts_with("no response"))
            {
                break;
            }
            continue;
        }
        if (r.response.size() >= 4 && r.response[0] == 0x62 && r.response[1] == (did >> 8)
            && r.response[2] == (did & 0xFF))
        {
            UdsDidValue v{.did = static_cast<uint16_t>(did), .data = {r.response.begin() + 3, r.response.end()}};
            out.push_back(v);
            if (on_found)
            {
                on_found(v);
            }
        }
    }
    return out;
}

// --- Text ---

namespace
{

std::string hex_bytes(std::span<const uint8_t> b)
{
    std::string s;
    for (const uint8_t c : b)
    {
        s += std::format("{}{:02X}", s.empty() ? "" : " ", c);
    }
    return s;
}

std::string ascii_bytes(std::span<const uint8_t> b)
{
    std::string s;
    for (const uint8_t c : b)
    {
        s += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
    }
    return s;
}

// DTC status byte (ISO 14229-1 D.2) as the bits that are set.
std::string dtc_status(uint8_t st)
{
    static constexpr std::array<const char*, 8> names = {
        "testFailed", "testFailedThisCycle", "pending", "confirmed",
        "testNotCompleteSinceClear", "testFailedSinceClear", "testNotCompleteThisCycle", "warningIndicator"};
    std::string s;
    for (int i = 0; i < 8; ++i)
    {
        if (st & (1 << i))
        {
            s += std::format("{}{}", s.empty() ? "" : "|", names[static_cast<std::size_t>(i)]);
        }
    }
    return s.empty() ? "none" : s;
}

// One "code status" entry: the ISO 15031-6 letter is in the top two bits (P C B U), the rest as digits.
std::string dtc_entry(std::span<const uint8_t> d)
{
    const uint8_t hi = d[0];
    return std::format("{}{:01X}{:01X}{:02X}-{:02X}  status 0x{:02X} {}", "PCBU"[hi >> 6], (hi >> 4) & 0x3, hi & 0x0F,
                       d[1], d[2], d[3], dtc_status(d[3]));
}

std::string dtc_list(std::span<const uint8_t> dtcs)
{
    std::string s;
    int n = 0;
    for (std::size_t i = 0; i + 4 <= dtcs.size(); i += 4, ++n)
    {
        s += "\n  " + dtc_entry(dtcs.subspan(i, 4));
    }
    return std::format("{} DTC{}{}", n, n == 1 ? "" : "s", s);
}

// 59 04 / 59 06 without record layouts: the DTC, then the raw records. A single requested record
// is named; "all" (0xFF) cannot be split without knowing each record's length.
std::string dtc_record_raw(std::span<const uint8_t> request, std::span<const uint8_t> response)
{
    if (response.size() < 6)
    {
        return std::format("DTC record: {}", hex_bytes(response.subspan(1)));
    }
    const std::span<const uint8_t> records = response.subspan(6);
    const bool single_record = request.size() >= 6 && request[5] != 0xFF && !records.empty();
    const char* kind = response[1] == 0x04 ? "snapshot" : "record";
    if (single_record)
    {
        return std::format("DTC {}\n  {} 0x{:02X}: {}", dtc_entry(response.subspan(2, 4)), kind, records[0], hex_bytes(records.subspan(1)));
    }
    return std::format("DTC {}\n  {}s: {}", dtc_entry(response.subspan(2, 4)), kind, hex_bytes(records));
}

} // namespace

std::string_view uds_did_name(uint16_t did)
{
    switch (did)
    {
    case 0xF180: return "BootSoftwareIdentification";
    case 0xF181: return "ApplicationSoftwareIdentification";
    case 0xF182: return "ApplicationDataIdentification";
    case 0xF183: return "BootSoftwareFingerprint";
    case 0xF184: return "ApplicationSoftwareFingerprint";
    case 0xF185: return "ApplicationDataFingerprint";
    case 0xF186: return "ActiveDiagnosticSession";
    case 0xF187: return "VehicleManufacturerSparePartNumber";
    case 0xF188: return "VehicleManufacturerECUSoftwareNumber";
    case 0xF189: return "VehicleManufacturerECUSoftwareVersionNumber";
    case 0xF18A: return "SystemSupplierIdentifier";
    case 0xF18B: return "ECUManufacturingDate";
    case 0xF18C: return "ECUSerialNumber";
    case 0xF18D: return "SupportedFunctionalUnits";
    case 0xF18E: return "VehicleManufacturerKitAssemblyPartNumber";
    case 0xF190: return "VIN";
    case 0xF191: return "VehicleManufacturerECUHardwareNumber";
    case 0xF192: return "SystemSupplierECUHardwareNumber";
    case 0xF193: return "SystemSupplierECUHardwareVersionNumber";
    case 0xF194: return "SystemSupplierECUSoftwareNumber";
    case 0xF195: return "SystemSupplierECUSoftwareVersionNumber";
    case 0xF196: return "ExhaustRegulationOrTypeApprovalNumber";
    case 0xF197: return "SystemNameOrEngineType";
    case 0xF198: return "RepairShopCodeOrTesterSerialNumber";
    case 0xF199: return "ProgrammingDate";
    case 0xF19A: return "CalibrationRepairShopCodeOrCalibrationEquipmentSerialNumber";
    case 0xF19B: return "CalibrationDate";
    case 0xF19C: return "CalibrationEquipmentSoftwareNumber";
    case 0xF19D: return "ECUInstallationDate";
    case 0xF19E: return "ODXFile";
    case 0xF19F: return "Entity";
    default: return {};
    }
}

// Sub-function names of the services that have one (ISO 14229-1), "" when unknown.
const char* uds_sub_name(uint8_t sid, uint8_t sub)
{
    sub &= 0x7F; // without suppressPosRspMsgIndicationBit
    switch (sid)
    {
    case 0x10:
        switch (sub)
        {
        case 0x01: return "defaultSession";
        case 0x02: return "programmingSession";
        case 0x03: return "extendedDiagnosticSession";
        case 0x04: return "safetySystemDiagnosticSession";
        default: return "";
        }
    case 0x11:
        switch (sub)
        {
        case 0x01: return "hardReset";
        case 0x02: return "keyOffOnReset";
        case 0x03: return "softReset";
        case 0x04: return "enableRapidPowerShutDown";
        case 0x05: return "disableRapidPowerShutDown";
        default: return "";
        }
    case 0x19:
        switch (sub)
        {
        case 0x01: return "reportNumberOfDTCByStatusMask";
        case 0x02: return "reportDTCByStatusMask";
        case 0x03: return "reportDTCSnapshotIdentification";
        case 0x04: return "reportDTCSnapshotRecordByDTCNumber";
        case 0x05: return "reportDTCStoredDataByRecordNumber";
        case 0x06: return "reportDTCExtDataRecordByDTCNumber";
        case 0x0A: return "reportSupportedDTC";
        case 0x0B: return "reportFirstTestFailedDTC";
        case 0x0C: return "reportFirstConfirmedDTC";
        case 0x0D: return "reportMostRecentTestFailedDTC";
        case 0x0E: return "reportMostRecentConfirmedDTC";
        case 0x14: return "reportDTCFaultDetectionCounter";
        case 0x15: return "reportDTCWithPermanentStatus";
        default: return "";
        }
    case 0x27: return sub & 1 ? "requestSeed" : "sendKey";
    case 0x28:
        switch (sub)
        {
        case 0x00: return "enableRxAndTx";
        case 0x01: return "enableRxAndDisableTx";
        case 0x02: return "disableRxAndEnableTx";
        case 0x03: return "disableRxAndTx";
        default: return "";
        }
    case 0x2F: // control parameter, not a sub-function, but named the same way
        switch (sub)
        {
        case 0x00: return "returnControlToECU";
        case 0x01: return "resetToDefault";
        case 0x02: return "freezeCurrentState";
        case 0x03: return "shortTermAdjustment";
        default: return "";
        }
    case 0x31:
        switch (sub)
        {
        case 0x01: return "startRoutine";
        case 0x02: return "stopRoutine";
        case 0x03: return "requestRoutineResults";
        default: return "";
        }
    case 0x3E: return sub == 0 ? "zeroSubFunction" : "";
    case 0x85: return sub == 1 ? "on" : sub == 2 ? "off" : "";
    default: return "";
    }
}

std::string uds_describe_request(std::span<const uint8_t> request)
{
    if (request.empty())
    {
        return {};
    }
    const uint8_t sid = request[0];
    const std::string name = uds_service_name(sid);
    const auto did = [&](std::size_t at) -> std::string
    {
        if (request.size() < at + 2)
        {
            return {};
        }
        const uint16_t d = static_cast<uint16_t>(request[at] << 8 | request[at + 1]);
        const std::string_view n = uds_did_name(d);
        return n.empty() ? std::format(" {:04X}", d) : std::format(" {:04X} {}", d, n);
    };
    // "0x01 hardReset", plus the suppress bit when set.
    const auto sub = [&](std::size_t at = 1) -> std::string
    {
        if (request.size() < at + 1)
        {
            return {};
        }
        const uint8_t b = request[at];
        std::string out = std::format(" 0x{:02X}", b & 0x7F);
        if (const char* n = uds_sub_name(sid, b); *n != '\0')
        {
            out += std::format(" {}", n);
        }
        if (b & 0x80)
        {
            out += " (suppress positive response)";
        }
        return out;
    };
    const auto rest = [&](std::size_t from, const char* what) -> std::string
    {
        if (request.size() <= from)
        {
            return {};
        }
        return std::format(", {} {}", what, hex_bytes(request.subspan(from)));
    };
    // 23 / 3D: addressAndLengthFormatIdentifier, then the address and size bytes.
    const auto memory = [&]() -> std::string
    {
        if (request.size() < 2)
        {
            return name;
        }
        const std::size_t na = request[1] & 0x0F;
        const std::size_t ns = request[1] >> 4;
        if (na == 0 || ns == 0 || request.size() < 2 + na + ns)
        {
            return std::format("{} (ALFID 0x{:02X}: {} address + {} size bytes expected)", name, request[1], na, ns);
        }
        uint64_t addr = 0;
        uint64_t size = 0;
        for (std::size_t i = 0; i < na; ++i)
        {
            addr = addr << 8 | request[2 + i];
        }
        for (std::size_t i = 0; i < ns; ++i)
        {
            size = size << 8 | request[2 + na + i];
        }
        return std::format("{} address 0x{:X} size 0x{:X}", name, addr, size) + rest(2 + na + ns, "data");
    };
    switch (sid)
    {
    case 0x10: case 0x11: case 0x3E: case 0x28: case 0x85:
        return name + sub();
    case 0x14:
        if (request.size() >= 4)
        {
            const uint32_t group = static_cast<uint32_t>(request[1] << 16 | request[2] << 8 | request[3]);
            return std::format("{} group {:06X}{}", name, group, group == 0xFFFFFF ? " (all DTCs)" : "");
        }
        return name;
    case 0x22:
        return name + did(1);
    case 0x2E:
        return name + did(1) + rest(3, "data");
    case 0x2F: // byte 3 is the control parameter: no suppress bit
        return name + did(1)
            + (request.size() >= 4 ? std::format(" 0x{:02X} {}", request[3], uds_sub_name(0x2F, request[3])) : std::string())
            + rest(4, "state");
    case 0x19:
    {
        std::string out = name + sub();
        const uint8_t s = request.size() >= 2 ? request[1] & 0x7F : 0;
        if ((s == 0x01 || s == 0x02 || s == 0x0F || s == 0x11 || s == 0x12 || s == 0x13) && request.size() >= 3)
        {
            out += std::format(", status mask 0x{:02X}", request[2]);
        }
        else if (s == 0x05 && request.size() >= 3)
        {
            out += std::format(", record 0x{:02X}", request[2]);
        }
        else if ((s == 0x03 || s == 0x04 || s == 0x06 || s == 0x10) && request.size() >= 5)
        {
            out += std::format(", DTC {:02X}{:02X}{:02X}", request[2], request[3], request[4]);
            if (request.size() >= 6)
            {
                out += std::format(" record 0x{:02X}{}", request[5], request[5] == 0xFF ? " (all)" : "");
            }
        }
        return out;
    }
    case 0x31:
        return name + sub() + (request.size() >= 4 ? std::format(", routine {:02X}{:02X}", request[2], request[3]) : "")
            + rest(4, "options");
    case 0x27:
        if (request.size() >= 2)
        {
            const uint8_t s = request[1] & 0x7F;
            return std::format("{}{} level {}", name, sub(), (s + 1) / 2) + rest(2, s & 1 ? "data" : "key");
        }
        return name;
    case 0x23: case 0x3D:
        return memory();
    case 0x36:
        return request.size() >= 2 ? std::format("{} block 0x{:02X}", name, request[1]) + rest(2, "data") : name;
    case 0x7F:
        return request.size() >= 3 ? std::format("{} to {}: NRC 0x{:02X} {}", name, uds_service_name(request[1]),
                                                 request[2], uds_nrc_name(request[2]))
                                   : name;
    default:
        return name + rest(1, "data");
    }
}

std::string uds_describe(std::span<const uint8_t> request, std::span<const uint8_t> response)
{
    if (response.empty())
    {
        return {};
    }
    if (response[0] == 0x7F)
    {
        return response.size() >= 3 ? std::format("NRC 0x{:02X} {}", response[2], uds_nrc_name(response[2]))
                                    : "negative response";
    }
    switch (response[0])
    {
    case 0x50:
        if (response.size() >= 4)
        {
            return std::format("session 0x{:02X} active, P2 {} ms P2* {} ms", response[1],
                               response[2] << 8 | response[3],
                               response.size() >= 6 ? (response[4] << 8 | response[5]) * 10 : 0);
        }
        return response.size() >= 2 ? std::format("session 0x{:02X} active", response[1]) : "session changed";
    case 0x51:
        return response.size() >= 2 ? std::format("ECU reset 0x{:02X} accepted", response[1]) : "ECU reset accepted";
    case 0x54:
        return "DTCs cleared";
    case 0x62:
        if (response.size() >= 3)
        {
            const uint16_t d = static_cast<uint16_t>(response[1] << 8 | response[2]);
            const std::string_view nm = uds_did_name(d);
            const auto data = response.subspan(3);
            return std::format("DID {:04X}{} = {}  \"{}\"", d, nm.empty() ? "" : std::format(" {}", nm),
                               hex_bytes(data), ascii_bytes(data));
        }
        break;
    case 0x6E:
        if (response.size() >= 3)
        {
            return std::format("DID {:02X}{:02X} written", response[1], response[2]);
        }
        break;
    case 0x6F:
        if (response.size() >= 3)
        {
            return std::format("I/O control DID {:02X}{:02X}", response[1], response[2]);
        }
        break;
    case 0x63:
        return std::format("memory read: {}", hex_bytes(response.subspan(1)));
    case 0x7D:
        return "memory written";
    case 0x68:
        return "communication control done";
    case 0xC5:
        return "DTC setting changed";
    case 0x71:
        if (response.size() >= 4)
        {
            return std::format("routine {:02X}{:02X} sub 0x{:02X}{}{}", response[2], response[3], response[1],
                               response.size() > 4 ? ": " : "", hex_bytes(response.subspan(4)));
        }
        break;
    case 0x59:
    {
        // The report type is echoed in response[1], so the request is not needed to read it:
        // 01/07/11 (count), 02/0A (list by status), 04/06 (snapshot / extended data).
        const uint8_t sub = response.size() >= 2 ? response[1] : 0;
        if ((sub == 0x01 || sub == 0x07 || sub == 0x11) && response.size() >= 6)
        {
            return std::format("DTC count {}", response[4] << 8 | response[5]);
        }
        if ((sub == 0x02 || sub == 0x0A) && response.size() >= 3)
        {
            return dtc_list(response.subspan(3));
        }
        if (sub == 0x04 || sub == 0x06)
        {
            return dtc_record_raw(request, response);
        }
        break;
    }
    case 0x7E:
        return "tester present";
    case 0x67:
        if (response.size() >= 2)
        {
            return response[1] % 2 == 1 ? std::format("seed {}", hex_bytes(response.subspan(2))) : "security access granted";
        }
        break;
    default:
        break;
    }
    return std::format("positive response to {}", uds_service_name(static_cast<uint8_t>(response[0] - 0x40)));
}

UdsEcu uds_ecu_from_responder(const UdsTarget& t, uint32_t rx_id)
{
    UdsEcu e{.iface = t.iface, .extended = t.extended, .rx_id = rx_id};
    if (t.extended)
    {
        // 0x18DA<TA=tester><SA=ECU> answers 0x18DA<ECU><tester>.
        const uint32_t ecu = rx_id & 0xFF;
        const uint32_t tester = (rx_id >> 8) & 0xFF;
        e.tx_id = (rx_id & 0x1FFF0000u) | (ecu << 8) | tester;
    }
    else
    {
        e.tx_id = rx_id >= 8 ? rx_id - 8 : rx_id;
        e.guessed = true;
    }
    return e;
}

bool uds_learner_add(UdsLearner& l, const UdsEcu& ecu)
{
    for (UdsEcu& e : l.ecus)
    {
        if (e.iface == ecu.iface && e.extended == ecu.extended && e.rx_id == ecu.rx_id)
        {
            bool changed = false;
            if (e.guessed && !ecu.guessed)
            {
                e.tx_id = ecu.tx_id;
                e.guessed = false;
                changed = true;
            }
            if (!ecu.name.empty() && e.name != ecu.name)
            {
                e.name = ecu.name;
                changed = true;
            }
            if (!ecu.services.empty() && e.services != ecu.services)
            {
                e.services = ecu.services;
                changed = true;
            }
            return changed;
        }
    }
    l.ecus.push_back(ecu);
    return true;
}

bool uds_learn(UdsLearner& l, const BusMessage& frame)
{
    if (frame.type != BusType::CAN || is_error_frame(frame) || has_flag(frame, bus_flag::rtr))
    {
        return false;
    }
    const int ae = l.addr_ext ? 1 : 0;
    if (has_flag(frame, bus_flag::extended))
    {
        // Track J1939 source addresses (low byte) as sweep candidates, 0xFE/0xFF aside.
        const uint8_t sa = static_cast<uint8_t>(frame.id & 0xFF);
        if (sa != 0xFE && sa != 0xFF)
        {
            const auto at = std::ranges::lower_bound(l.j1939_sources, sa);
            if (at == l.j1939_sources.end() || *at != sa)
            {
                l.j1939_sources.insert(at, sa);
            }
        }
    }
    const auto payload = first_payload(frame, ae);
    if (!payload)
    {
        return false;
    }
    const std::span<const uint8_t> p = *payload;
    const uint32_t id = can_id(frame);
    const bool response = p[0] == 0x7F || (p[0] >= 0x40 && is_request_sid(static_cast<uint8_t>(p[0] - 0x40)));
    const uint8_t sid = p[0] == 0x7F ? (p.size() >= 2 ? p[1] : 0) : response ? static_cast<uint8_t>(p[0] - 0x40) : p[0];
    if (!is_request_sid(sid))
    {
        return false;
    }

    if (has_flag(frame, bus_flag::extended))
    {
        // Normal fixed addressing, 0x18DA<TA><SA>: an answer names both ends.
        if (((id >> 16) & 0xFF) != 0xDA || !response)
        {
            return false;
        }
        const uint32_t ecu = id & 0xFF;
        const uint32_t tester = (id >> 8) & 0xFF;
        return uds_learner_add(l, {.iface = frame.iface, .extended = true,
                                   .tx_id = (id & 0x1FFF0000u) | (ecu << 8) | tester, .rx_id = id});
    }

    std::erase_if(l.pending, [&](const UdsLearner::Pending& q) { return frame.ts_ns - q.ts_ns > pairing_window_ns; });
    if (!response)
    {
        if (l.pending.size() >= max_pending)
        {
            l.pending.erase(l.pending.begin());
        }
        l.pending.push_back({.iface = frame.iface, .id = id, .sid = sid, .ts_ns = frame.ts_ns});
        return false;
    }
    // The request answered: the one on id - 8 (the OBD convention) when it is pending, else the
    // newest of this service on the same bus. It is consumed, unless the answer was "pending".
    auto match = l.pending.end();
    for (auto it = l.pending.begin(); it != l.pending.end(); ++it)
    {
        if (it->iface == frame.iface && it->sid == sid && it->id != id
            && (match == l.pending.end() || it->id + 8 == id || match->id + 8 != id))
        {
            match = it;
        }
    }
    if (match == l.pending.end())
    {
        return false;
    }
    const bool functional = match->id == 0x7DF;
    const UdsEcu e{.iface = frame.iface, .tx_id = functional ? (id >= 8 ? id - 8 : id) : match->id, .rx_id = id,
                   .guessed = functional};
    if (!(p[0] == 0x7F && p.size() >= 3 && p[2] == 0x78) && !functional)
    {
        l.pending.erase(match);
    }
    return uds_learner_add(l, e);
}

int parse_code_lines(std::string_view text, std::map<uint32_t, std::string>& out)
{
    int n = 0;
    for (std::size_t at = 0; at < text.size();)
    {
        std::size_t end = text.find('\n', at);
        end = end == std::string_view::npos ? text.size() : end;
        const std::string_view line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        const std::size_t sp = line.find_first_of(" \t,;");
        std::string_view code = line.substr(0, sp);
        if (code.starts_with("0x") || code.starts_with("0X"))
        {
            code.remove_prefix(2);
        }
        uint32_t id = 0;
        if (!parse_number(code, id, 16))
        {
            continue;
        }
        const std::size_t t = sp == std::string_view::npos ? std::string_view::npos : line.find_first_not_of(" \t,;", sp);
        out[id] = t == std::string_view::npos ? std::string() : std::string(line.substr(t));
        ++n;
    }
    return n;
}

namespace
{

// The first blank-separated word of `rest`, removed from it ("" when rest is empty).
std::string_view next_word(std::string_view& rest)
{
    rest = trim(rest);
    const std::size_t blank = rest.find_first_of(" \t");
    const std::string_view word = rest.substr(0, blank);
    rest = blank == std::string_view::npos ? std::string_view() : rest.substr(blank);
    return word;
}

// "u8" / "u16" / "u24" / "u32" / "time" -> the field's size (and time flag); false for anything else.
bool parse_field_type(std::string_view type_name, UdsExtField& field)
{
    static constexpr std::pair<std::string_view, uint8_t> sizes[] = {{"u8", 1}, {"u16", 2}, {"u24", 3}, {"u32", 4}, {"time", 4}};
    for (const auto& [name, size] : sizes)
    {
        if (type_name == name)
        {
            field.size = size;
            field.time = name == "time";
            return true;
        }
    }
    return false;
}

// "*0.25-40": every "*scale", "+offset" or "-offset" in turn.
void parse_field_modifiers(std::string_view modifiers, UdsExtField& field)
{
    while (!modifiers.empty())
    {
        const char sign = modifiers[0];
        const std::size_t next = modifiers.find_first_of("*+-", 1);
        const std::string_view number_text = modifiers.substr(1, next == std::string_view::npos ? next : next - 1);
        double number = 0.0;
        if (parse_number(number_text, number))
        {
            if (sign == '*')
            {
                field.scale = number;
            }
            else
            {
                field.offset = sign == '-' ? -number : number;
            }
        }
        modifiers = next == std::string_view::npos ? std::string_view() : modifiers.substr(next);
    }
}

// "<name>:<type>[*scale][+offset]" -> field; false when the word is not of that form.
bool parse_field(std::string_view word, UdsExtField& field)
{
    const std::size_t colon = word.find(':');
    if (colon == std::string_view::npos)
    {
        return false;
    }
    field.name = std::string(word.substr(0, colon));
    const std::string_view type_and_modifiers = word.substr(colon + 1);
    const std::size_t first_modifier = type_and_modifiers.find_first_of("*+-");
    if (first_modifier != std::string_view::npos)
    {
        parse_field_modifiers(type_and_modifiers.substr(first_modifier), field);
    }
    return parse_field_type(type_and_modifiers.substr(0, first_modifier), field);
}

// " name value" for one field read big endian from bytes (bytes.size() >= field.size).
void append_field_value(std::string& out, const UdsExtField& field, std::span<const uint8_t> bytes)
{
    uint64_t raw = 0;
    for (std::size_t i = 0; i < field.size; ++i)
    {
        raw = raw << 8 | bytes[i];
    }
    if (field.time)
    {
        const std::chrono::sys_seconds when{std::chrono::seconds{raw}};
        out += std::format(" {} {:%Y-%m-%d %H:%M:%S}Z", field.name, when);
    }
    else if (field.scale != 1.0 || field.offset != 0.0)
    {
        out += std::format(" {} {:g}", field.name, static_cast<double>(raw) * field.scale + field.offset);
    }
    else
    {
        out += std::format(" {} {}", field.name, raw);
    }
}

// One extended data record at the front of `bytes`: appends its line and returns how many bytes it
// took, 0 when its layout is unknown (the rest cannot be split then).
std::size_t append_extended_record(std::string& out, std::span<const uint8_t> bytes, const UdsDtcDefs& defs)
{
    const uint8_t record_number = bytes[0];
    const auto layout = defs.ext.find(record_number);
    if (layout == defs.ext.end() || bytes.size() < 1 + layout->second.size)
    {
        return 0;
    }
    out += std::format("\n  record 0x{:02X}:", record_number);
    std::size_t at = 1;
    for (const UdsExtField& field : layout->second.fields)
    {
        append_field_value(out, field, bytes.subspan(at));
        at += field.size;
    }
    return at;
}

// One snapshot record at the front of `bytes`: record number, number of DIDs, then DID (2 bytes)
// + value each. Appends its line and returns the bytes taken, 0 when a DID has no definition.
std::size_t append_snapshot_record(std::string& out, std::span<const uint8_t> bytes, const UdsDtcDefs& defs)
{
    if (bytes.size() < 2)
    {
        return 0;
    }
    const uint8_t record_number = bytes[0];
    const int did_count = bytes[1];
    std::string line = std::format("\n  snapshot 0x{:02X}:", record_number);
    std::size_t at = 2;
    for (int i = 0; i < did_count; ++i)
    {
        if (at + 2 > bytes.size())
        {
            return 0;
        }
        const auto did = static_cast<uint16_t>(bytes[at] << 8 | bytes[at + 1]);
        const auto field = defs.snap.find(did);
        if (field == defs.snap.end() || at + 2 + field->second.size > bytes.size())
        {
            return 0;
        }
        append_field_value(line, field->second, bytes.subspan(at + 2));
        at += 2 + field->second.size;
    }
    out += line;
    return at;
}

} // namespace

int parse_dtc_defs(std::string_view text, UdsDtcDefs& out)
{
    int defined = 0;
    for (std::size_t at = 0; at < text.size();)
    {
        std::size_t end = text.find('\n', at);
        end = end == std::string_view::npos ? text.size() : end;
        std::string_view rest = text.substr(at, end - at);
        at = end + 1;

        const std::string_view keyword = next_word(rest);
        const bool extended = keyword == "ext";
        const bool snapshot = keyword == "snap";
        if (!extended && !snapshot)
        {
            continue;
        }
        uint32_t id = 0; // the record number (ext) or the DID (snap)
        if (!parse_number(next_word(rest), id, 16) || id > (extended ? 0xFFu : 0xFFFFu))
        {
            continue;
        }
        UdsExtRecord record;
        for (std::string_view word = next_word(rest); !word.empty(); word = next_word(rest))
        {
            UdsExtField field;
            if (parse_field(word, field))
            {
                record.size += field.size;
                record.fields.push_back(std::move(field));
            }
        }
        if (record.fields.empty())
        {
            continue;
        }
        if (extended)
        {
            out.ext[static_cast<uint8_t>(id)] = std::move(record);
        }
        else
        {
            out.snap[static_cast<uint16_t>(id)] = std::move(record.fields.front()); // one DID, one field
        }
        ++defined;
    }
    return defined;
}

std::string uds_dtc_record_text(std::span<const uint8_t> response, const UdsDtcDefs& defs)
{
    if (response.size() < 6 || response[0] != 0x59 || (response[1] != 0x04 && response[1] != 0x06))
    {
        return uds_describe({}, response);
    }
    const bool snapshot = response[1] == 0x04;
    std::string out = "DTC " + dtc_entry(response.subspan(2, 4));
    std::span<const uint8_t> rest = response.subspan(6);
    while (!rest.empty())
    {
        const std::size_t taken = snapshot ? append_snapshot_record(out, rest, defs) : append_extended_record(out, rest, defs);
        if (taken == 0) // no definition for this record: its length is unknown, the rest stays hex
        {
            const std::size_t header = std::min<std::size_t>(snapshot ? 2 : 1, rest.size()); // record number (+ DID count)
            out += std::format("\n  {} 0x{:02X}: {}", snapshot ? "snapshot" : "record", rest[0], hex_bytes(rest.subspan(header)));
            break;
        }
        rest = rest.subspan(taken);
    }
    return out;
}
