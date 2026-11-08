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

// UDS client (ISO 14229 over ISO 15765-2 on classic CAN and CAN FD): sends a request as a single
// frame or segmented (first frame, then consecutive frames paced by the ECU's flow control), waits
// for the response with P2 / P2* (NRC 0x78 "response pending" extends the wait) and answers the
// ECU's first frame with a flow control. Normal (11-bit), normal fixed (29-bit 0x18DA<TA><SA>) and
// extended / mixed addressing (an address byte leading every frame). Blocking: call it from a
// worker, the Python thread or the REST thread, never from the main loop. Frames reach it through
// uds_client_rx, an RxConsumer on the RX threads.
//
// Address discovery: uds_learn pairs request and response ids from the traffic (passive),
// uds_functional sends one functional request and collects every ECU that answers, uds_sweep
// tries every physical id of a range. uds_service_scan and uds_did_scan probe one ECU.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/bus_message.h"

struct UdsTarget
{
    uint16_t iface = 0;
    uint32_t tx_id = 0x7E0; // request id: physical, or functional (0x7DF, 0x18DB33F1; single-frame answers only)
    uint32_t rx_id = 0x7E8; // response id
    bool extended = false;  // 29-bit ids
    uint8_t padding = 0xCC; // fills every frame to 8 bytes (FD: to the next DLC step)
    bool fd = false;        // ISO 15765-2:2016 on CAN FD: long single / first frames, frames up to tx_len bytes
    bool brs = false;       // bit rate switch on the frames sent (fd only)
    uint8_t tx_len = 8;     // largest frame sent with fd: 8, 12, 16, 20, 24, 32, 48 or 64
    std::optional<uint8_t> tx_ae; // extended / mixed addressing: this byte (N_TA / N_AE) leads every frame sent
    std::optional<uint8_t> rx_ae; // and this one every frame accepted
};

struct UdsTiming
{
    // ISO's P2 is 50 ms; gateways and USB adapters add latency, so wait longer before giving up.
    std::chrono::milliseconds p2{1000};
    std::chrono::milliseconds p2_star{5000}; // after NRC 0x78
    std::chrono::milliseconds n_bs{1000};    // the ECU's flow control after our first frame / block
    std::chrono::milliseconds n_cr{1000};    // between the ECU's consecutive frames
};

struct UdsResult
{
    std::vector<uint8_t> response; // positive (SID + 0x40, ...) or negative (7F SID NRC)
    std::string error;             // timeout, send failure, flow control overflow; empty when answered
    bool suppressed = false;       // suppressPosRspMsgIndicationBit set: no response was awaited
};

using UdsSend = std::function<bool(const BusMessage&)>;

struct UdsWaiter
{
    uint16_t iface = 0;
    bool extended = false;
    uint32_t id = 0;
    bool any_id = false;           // functional request / sweep: every id of the iface and id width
    std::optional<uint8_t> ae;     // only frames leading with this address byte
    std::vector<BusMessage> frames; // queued by uds_client_rx, guarded by UdsClient::mutex
};

// One per application; requests from several threads may run at once.
struct UdsClient
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<UdsWaiter*> waiters;
    std::atomic<int> active{0}; // waiters.size(), read without the lock on the RX threads
    std::atomic<bool> cancel{false}; // every wait ends with an error (shutdown)
};

// RxConsumer: queues received frames for the requests waiting on them. Cheap when none waits.
void uds_client_rx(void* client, const BusMessage& msg);
// Ends every running and future request with an error: before the threads that call them join.
void uds_client_cancel(UdsClient& c);

[[nodiscard]] UdsResult uds_request(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                    std::span<const uint8_t> request, const UdsTiming& timing = {});

// Sends request as a single frame to t.tx_id and returns every response to it within window,
// one per responding id (rx_id is the responder's id, t.rx_id is ignored).
struct UdsResponder
{
    uint32_t rx_id = 0;
    std::vector<uint8_t> response;
};
[[nodiscard]] std::vector<UdsResponder> uds_functional(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                                       std::span<const uint8_t> request,
                                                       std::chrono::milliseconds window);

// Functional TesterPresent on 11-bit (0x7DF) and 29-bit (0x18DB33F1), every ECU that answers
// within window, by uds_ecu_from_responder. transport lends padding, fd, brs, tx_len, tx_ae, rx_ae.
[[nodiscard]] std::vector<struct UdsEcu> uds_scan(UdsClient& c, const UdsSend& send, uint16_t iface,
                                                   std::chrono::milliseconds window, const UdsTarget& transport = {});

// --- Probing ---

// Physical sweep: request to every id of the range (11-bit) or to 0x18DA<SA><tester> for every
// candidate SA (29-bit); every answer within wait names an ECU. 11-bit answers are paired with the
// request just sent; one on request + 8 is certain, any other id is marked guessed.
struct UdsSweep
{
    UdsTarget transport;                  // iface, extended, padding, fd, brs, tx_len, tx_ae, rx_ae; the ids come from below
    uint32_t first = 0x700;               // 11-bit request ids tried, first..last
    uint32_t last = 0x7FF;
    uint8_t tester = 0xF1;                // 29-bit: requests go to 0x18DA<SA><tester>
    std::vector<uint8_t> candidates;      // 29-bit: SAs tried; empty = all 256
    std::vector<uint8_t> request = {0x3E, 0x00}; // one single frame
    std::chrono::milliseconds wait{50};   // answers are collected this long after each request
};
[[nodiscard]] std::vector<struct UdsEcu> uds_sweep(UdsClient& c, const UdsSend& send, const UdsSweep& sweep,
                                                    const std::function<void(const struct UdsEcu&)>& on_found = {},
                                                    const std::atomic<bool>* stop = nullptr);

// Which services an ECU knows: every SID of ISO 14229-1 is sent alone (no sub-function, nothing
// happens); NRC 0x11 means not supported, every other answer that it is.
struct UdsServiceProbe
{
    uint8_t sid = 0;
    bool answered = false; // false: no response within P2
    uint8_t nrc = 0;       // 0: positive response
    [[nodiscard]] bool supported() const { return answered && nrc != 0x11; }
};
[[nodiscard]] std::vector<UdsServiceProbe> uds_service_scan(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                                            const UdsTiming& timing = {},
                                                            const std::atomic<bool>* stop = nullptr);

// ReadDataByIdentifier for every DID of first..last; the ones answered positively, with their data.
struct UdsDidValue
{
    uint16_t did = 0;
    std::vector<uint8_t> data;
};
[[nodiscard]] std::vector<UdsDidValue> uds_did_scan(UdsClient& c, const UdsSend& send, const UdsTarget& t,
                                                    uint16_t first, uint16_t last, const UdsTiming& timing = {},
                                                    const std::function<void(const UdsDidValue&)>& on_found = {},
                                                    const std::atomic<bool>* stop = nullptr);

// --- Text ---

// Readable form of a response to request: DIDs as hex and text, DTCs, NRC names. request may be
// empty (the trace decoder has only the payload).
[[nodiscard]] std::string uds_describe(std::span<const uint8_t> request, std::span<const uint8_t> response);
// Readable form of a request: service, sub-function, DID / routine / DTC it names.
[[nodiscard]] std::string uds_describe_request(std::span<const uint8_t> request);
// ISO 14229-1 Annex C name of a standard DID (F180..F19F), empty otherwise.
[[nodiscard]] std::string_view uds_did_name(uint16_t did);
// "<hex code> <text>" lines (DTC text / DID name files, one per product): one entry per line,
// "#" comments, an optional 0x prefix, the text after the first blank. Added to `out`; returns
// how many lines were entries.
int parse_code_lines(std::string_view text, std::map<uint32_t, std::string>& out);

// One field of a DTC record, manufacturer specific, so it comes from the DTC text file:
// "ext <record> <name>:<type>[*scale][+offset] ..." lays out an extended data record (0x19 0x06),
// "snap <DID> <name>:<type>[*scale][+offset]" one DID of a snapshot record (0x19 0x04). Types:
// u8 u16 u24 u32 (big endian), time (u32 seconds since 1970, shown as UTC).
struct UdsExtField
{
    std::string name;
    uint8_t size = 1;
    bool time = false;
    double scale = 1.0;
    double offset = 0.0;
};
struct UdsExtRecord
{
    std::vector<UdsExtField> fields;
    std::size_t size = 0; // sum of the fields' bytes
};
struct UdsDtcDefs
{
    std::map<uint8_t, UdsExtRecord> ext; // extended data record number -> layout
    std::map<uint16_t, UdsExtField> snap; // snapshot DID -> its one field
    [[nodiscard]] bool empty() const noexcept { return ext.empty() && snap.empty(); }
};
// Reads the "ext" and "snap" lines of a DTC text file into `out`; other lines are skipped.
// Returns how many definitions were read.
int parse_dtc_defs(std::string_view text, UdsDtcDefs& out);
// A 59 04 (snapshot) or 59 06 (extended data) response with its records split and named by
// `defs`; a record or DID without a definition ends the split and the rest is shown as hex (its
// length is unknown).
[[nodiscard]] std::string uds_dtc_record_text(std::span<const uint8_t> response, const UdsDtcDefs& defs);

// --- Address discovery ---

struct UdsEcu
{
    uint16_t iface = 0;
    bool extended = false;
    uint32_t tx_id = 0;   // tester -> ECU
    uint32_t rx_id = 0;   // ECU -> tester
    bool guessed = false; // tx_id inferred (answer to a functional request: rx_id - 8; sweep: pairing uncertain)
    std::string name;     // filled by the UI from DID F197 / F190 when it asks, or typed by the user
    std::string services; // SIDs the service scan found, as text
};

struct UdsLearner
{
    struct Pending
    {
        uint16_t iface = 0;
        uint32_t id = 0;
        uint8_t sid = 0;
        int64_t ts_ns = 0;
    };
    bool addr_ext = false;        // the bus uses extended addressing: every frame leads with an address byte
    std::vector<Pending> pending; // 11-bit requests waiting for their answer
    std::vector<UdsEcu> ecus;
    std::vector<uint8_t> j1939_sources; // source addresses seen in 29-bit traffic (sweep candidates), sorted
};

// Learns from one frame (RX or TX echo) of the traffic. True when ecus changed.
bool uds_learn(UdsLearner& l, const BusMessage& frame);
// Adds or refines an ECU (same iface, id width and rx_id: an exact tx_id replaces a guess, a
// name or service list is taken over).
// True when ecus changed.
bool uds_learner_add(UdsLearner& l, const UdsEcu& ecu);
// The ECU behind a responder of a functional request sent with t: 29-bit 0x18DA<TA><SA> is
// exact, 11-bit is the OBD convention (request = response - 8), marked guessed.
[[nodiscard]] UdsEcu uds_ecu_from_responder(const UdsTarget& t, uint32_t rx_id);
