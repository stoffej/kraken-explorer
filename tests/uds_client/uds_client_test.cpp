// UDS client (decoders/uds_client): ISO-TP send and receive against a fake ECU that answers
// from inside the send callback, functional scans, address learning and response text.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <initializer_list>
#include <map>
#include <vector>

#include "decoders/uds_client.h"

namespace
{

using Bytes = std::vector<uint8_t>;

BusMessage frame(uint32_t id, std::initializer_list<uint8_t> bytes, bool ext = false, uint16_t iface = 0, int64_t ts = 0)
{
    BusMessage m{};
    m.id = id;
    m.iface = iface;
    m.flags = ext ? bus_flag::extended : 0;
    m.ts_ns = ts;
    std::size_t i = 0;
    for (const uint8_t b : bytes)
    {
        m.data[i++] = b;
    }
    set_length(m, 8);
    return m;
}

// The ECU side: sees every frame the tester sends, answers through uds_client_rx.
struct Ecu
{
    UdsClient client;
    std::vector<BusMessage> sent; // by the tester
    std::function<void(Ecu&, const BusMessage&)> on_frame;

    UdsSend send()
    {
        return [this](const BusMessage& m)
        {
            sent.push_back(m);
            if (on_frame)
            {
                on_frame(*this, m);
            }
            return true;
        };
    }
    void reply(uint32_t id, std::initializer_list<uint8_t> bytes, bool ext = false)
    {
        uds_client_rx(&client, frame(id, bytes, ext));
    }
};

const UdsTarget engine{.tx_id = 0x7E0, .rx_id = 0x7E8};
const UdsTiming quick{.p2 = std::chrono::milliseconds(50), .p2_star = std::chrono::milliseconds(200),
                      .n_bs = std::chrono::milliseconds(50), .n_cr = std::chrono::milliseconds(50)};

} // namespace

TEST_CASE("single frame request and response, padded to 8 bytes")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}); };
    const Bytes req = {0x10, 0x03};
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
    REQUIRE(ecu.sent.size() == 1);
    CHECK(ecu.sent[0].id == 0x7E0);
    CHECK(ecu.sent[0].len == 8);
    CHECK(ecu.sent[0].data[0] == 0x02);
    CHECK(ecu.sent[0].data[3] == 0xCC);
    CHECK(uds_describe(req, r.response) == "session 0x03 active, P2 50 ms P2* 5000 ms");
}

TEST_CASE("multi-frame response: the tester sends a flow control, the rest is reassembled")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        if (m.data[1] == 0x22)
        {
            e.reply(0x7E8, {0x10, 0x14, 0x62, 0xF1, 0x90, 'W', 'K', 'R'}); // 20 bytes
        }
        else if ((m.data[0] >> 4) == 3)
        {
            e.reply(0x7E8, {0x21, 'A', 'K', 'E', 'N', '0', '1', '2'});
            e.reply(0x7E8, {0x22, '3', '4', '5', '6', '7', '8', '9'});
        }
    };
    const Bytes req = {0x22, 0xF1, 0x90};
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    REQUIRE(r.error.empty());
    CHECK(r.response.size() == 20);
    CHECK(uds_describe(req, r.response).ends_with("\"WKRAKEN0123456789\""));
    REQUIRE(ecu.sent.size() == 2);
    CHECK(ecu.sent[1].data[0] == 0x30); // clear to send, no block limit
}

TEST_CASE("multi-frame request honours block size and waits for the next flow control")
{
    Ecu ecu;
    int cfs = 0;
    ecu.on_frame = [&cfs](Ecu& e, const BusMessage& m)
    {
        const uint8_t pci = m.data[0] >> 4;
        if (pci == 1)
        {
            e.reply(0x7E8, {0x31}); // WAIT first: the tester keeps waiting
            e.reply(0x7E8, {0x30, 0x02, 0x01}); // then go, two frames per block, 1 ms apart
        }
        else if (pci == 2 && ++cfs % 2 == 0 && cfs < 3)
        {
            e.reply(0x7E8, {0x30, 0x02, 0x01});
        }
        if (pci == 2 && cfs == 3)
        {
            e.reply(0x7E8, {0x03, 0x6E, 0xF1, 0x90});
        }
    };
    Bytes req = {0x2E, 0xF1, 0x90};
    for (int i = 0; i < 21; ++i)
    {
        req.push_back(static_cast<uint8_t>('A' + i));
    }
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x6E, 0xF1, 0x90});
    REQUIRE(ecu.sent.size() == 4); // FF + 3 CF (6 + 7 + 7 + 4 = 24)
    CHECK(ecu.sent[0].data[0] == 0x10);
    CHECK(ecu.sent[0].data[1] == 24);
    CHECK(ecu.sent[1].data[0] == 0x21);
    CHECK(ecu.sent[3].data[0] == 0x23);
    CHECK(ecu.sent[3].data[4] == 'U');
    CHECK(ecu.sent[3].data[5] == 0xCC); // padding after the last byte
}

TEST_CASE("flow control overflow and a missing flow control end the request")
{
    Ecu ecu;
    const Bytes req(12, 0x2E);
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x32}); };
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error.find("overflow") != std::string::npos);
    ecu.on_frame = nullptr;
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error.find("N_Bs") != std::string::npos);
}

TEST_CASE("response pending extends the wait, a negative response is a response")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x7E8, {0x03, 0x7F, 0x31, 0x78});
        e.reply(0x7E8, {0x05, 0x71, 0x01, 0xFF, 0x00, 0x00});
    };
    const Bytes routine = {0x31, 0x01, 0xFF, 0x00};
    UdsResult r = uds_request(ecu.client, ecu.send(), engine, routine, quick);
    CHECK(r.response == Bytes{0x71, 0x01, 0xFF, 0x00, 0x00});
    CHECK(uds_describe(routine, r.response) == "routine FF00 sub 0x01: 00");

    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x03, 0x7F, 0x22, 0x31}); };
    const Bytes read = {0x22, 0x12, 0x34};
    r = uds_request(ecu.client, ecu.send(), engine, read, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x7F, 0x22, 0x31});
    CHECK(uds_describe(read, r.response) == "NRC 0x31 Request Out Of Range");
}

TEST_CASE("no answer, an answer to another service or on another id: timeout")
{
    Ecu ecu;
    const Bytes req = {0x22, 0xF1, 0x90};
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error == "no response within 50 ms");
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x7E8, {0x02, 0x7E, 0x00}); // someone else's tester present
        e.reply(0x7E9, {0x03, 0x62, 0xF1, 0x90});
    };
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, quick).error.starts_with("no response"));
    CHECK(ecu.client.active.load() == 0); // the waiter is gone
}

TEST_CASE("suppressPosRspMsgIndicationBit: sent, nothing awaited")
{
    Ecu ecu;
    const Bytes req = {0x3E, 0x80};
    const UdsResult r = uds_request(ecu.client, ecu.send(), engine, req, quick);
    CHECK(r.suppressed);
    CHECK(r.error.empty());
    CHECK(ecu.sent.size() == 1);
}

TEST_CASE("functional request collects every answering ECU once")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x7E8, {0x02, 0x7E, 0x00});
        e.reply(0x7E9, {0x02, 0x7E, 0x00});
        e.reply(0x7E8, {0x02, 0x7E, 0x00}); // twice: listed once
        e.reply(0x123, {0x08, 0x01, 0x02});  // other traffic
    };
    const Bytes tp = {0x3E, 0x00};
    const UdsTarget fn{.tx_id = 0x7DF};
    const auto found = uds_functional(ecu.client, ecu.send(), fn, tp, std::chrono::milliseconds(30));
    REQUIRE(found.size() == 2);
    CHECK(found[0].rx_id == 0x7E8);
    CHECK(found[1].rx_id == 0x7E9);
    const UdsEcu e = uds_ecu_from_responder(fn, 0x7E9);
    CHECK(e.tx_id == 0x7E1);
    CHECK(e.guessed);

    // 29-bit: only answers to this tester (0x18DAF1xx) count, and their address is exact.
    ecu.on_frame = [](Ecu& e, const BusMessage&)
    {
        e.reply(0x18DAF110, {0x02, 0x7E, 0x00}, true);
        e.reply(0x18DAF210, {0x02, 0x7E, 0x00}, true);
        e.reply(0x7E8, {0x02, 0x7E, 0x00});
    };
    const UdsTarget fn29{.tx_id = 0x18DB33F1, .extended = true};
    const auto found29 = uds_functional(ecu.client, ecu.send(), fn29, tp, std::chrono::milliseconds(30));
    REQUIRE(found29.size() == 1);
    const UdsEcu body = uds_ecu_from_responder(fn29, found29[0].rx_id);
    CHECK(body.tx_id == 0x18DA10F1);
    CHECK(body.rx_id == 0x18DAF110);
    CHECK(!body.guessed);
}

TEST_CASE("a functional request refuses a multi-frame answer, cancel ends a wait")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x10, 0x14, 0x62, 0xF1, 0x90, 'W', 'K', 'R'}); };
    const Bytes req = {0x22, 0xF1, 0x90};
    const UdsTarget fn{.tx_id = 0x7DF, .rx_id = 0x7E8};
    CHECK(uds_request(ecu.client, ecu.send(), fn, req, quick).error.find("physical") != std::string::npos);
    CHECK(ecu.sent.size() == 1); // no flow control went out

    ecu.on_frame = [](Ecu& e, const BusMessage&) { uds_client_cancel(e.client); };
    const UdsTiming slow{.p2 = std::chrono::seconds(10)};
    CHECK(uds_request(ecu.client, ecu.send(), engine, req, slow).error == "cancelled");
}

TEST_CASE("DTC list text")
{
    const Bytes req = {0x19, 0x02, 0xFF};
    const Bytes rsp = {0x59, 0x02, 0xFF, 0x01, 0x23, 0x00, 0x2F, 0xC1, 0x55, 0x10, 0x09};
    CHECK(uds_describe(req, rsp) == "2 DTCs\n  P0123-00  status 0x2F testFailed|testFailedThisCycle|pending|confirmed|testFailedSinceClear\n  U0155-10  status 0x09 testFailed|confirmed");
}

TEST_CASE("extended data and snapshot records: raw, and split by the DTC text file's ext / snap lines")
{
    const Bytes req = {0x19, 0x06, 0x01, 0x01, 0x00, 0xFF};
    // P0101 status 0x09, record 01 (counters), record 02 (two timestamps), record 03 (odometer, hours, 0.1 V)
    const Bytes rsp = {0x59, 0x06, 0x01, 0x01, 0x00, 0x09, 0x01, 0x03, 0x02, 0x02, 0x69, 0x0F, 0x5A, 0x00, 0x69, 0x0F, 0x6E,
                       0x40, 0x03, 0x01, 0xE2, 0x40, 0x04, 0xD2, 0x8A};
    CHECK(uds_describe(req, rsp) == "DTC P0101-00  status 0x09 testFailed|confirmed\n  records: 01 03 02 02 69 0F 5A 00 69 0F 6E 40 03 01 E2 40 04 D2 8A");
    CHECK(uds_describe(Bytes{0x19, 0x06, 0x01, 0x01, 0x00, 0x01}, Bytes{0x59, 0x06, 0x01, 0x01, 0x00, 0x09, 0x01, 0x03, 0x02})
          == "DTC P0101-00  status 0x09 testFailed|confirmed\n  record 0x01: 03 02");

    UdsDtcDefs defs;
    CHECK(parse_dtc_defs("# texts\n010100 MAF\next 01 occurrences:u8 aging:u8\next 02 first:time last:time\n"
                         "ext 03 odometer_km:u24 hours:u16 voltage_V:u8*0.1\next zz bad:u8\n"
                         "snap F40C rpm:u16*0.25\nsnap F40D speed_kmh:u8\nsnap F405 coolant_C:u8-40\n", defs) == 6);
    CHECK(defs.ext.at(0x02).size == 8);
    CHECK(defs.snap.at(0xF405).offset == -40.0);
    CHECK(uds_dtc_record_text(rsp, defs) == "DTC P0101-00  status 0x09 testFailed|confirmed\n  record 0x01: occurrences 3 aging 2"
                                            "\n  record 0x02: first 2025-11-08 14:56:00Z last 2025-11-08 16:22:24Z"
                                            "\n  record 0x03: odometer_km 123456 hours 1234 voltage_V 13.8");
    // An undefined record ends the split: its length is unknown.
    defs.ext.erase(0x02);
    CHECK(uds_dtc_record_text(rsp, defs) == "DTC P0101-00  status 0x09 testFailed|confirmed\n  record 0x01: occurrences 3 aging 2"
                                            "\n  record 0x02: 69 0F 5A 00 69 0F 6E 40 03 01 E2 40 04 D2 8A");

    // Snapshot: record 01 with three DIDs (rpm 3000, 79 km/h, 90 C), record 02 with one unknown DID.
    const Bytes snap = {0x59, 0x04, 0x01, 0x01, 0x00, 0x09, 0x01, 0x03, 0xF4, 0x0C, 0x2E, 0xE0, 0xF4, 0x0D, 0x4F, 0xF4, 0x05, 0x82,
                        0x02, 0x01, 0xAA, 0xBB, 0x01};
    CHECK(uds_describe(Bytes{0x19, 0x04, 0x01, 0x01, 0x00, 0x01}, Bytes{0x59, 0x04, 0x01, 0x01, 0x00, 0x09, 0x01, 0x01, 0xF4, 0x0D, 0x4F})
          == "DTC P0101-00  status 0x09 testFailed|confirmed\n  snapshot 0x01: 01 F4 0D 4F");
    CHECK(uds_dtc_record_text(snap, defs) == "DTC P0101-00  status 0x09 testFailed|confirmed"
                                             "\n  snapshot 0x01: rpm 3000 speed_kmh 79 coolant_C 90"
                                             "\n  snapshot 0x02: AA BB 01");
}

TEST_CASE("CAN FD: long single frame and 64-byte consecutive frames")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        if ((m.data[0] >> 4) == 0) // our long single frame (0x00, len, ...)
        {
            BusMessage r{};
            r.id = 0x7E8;
            r.flags = bus_flag::fd;
            r.data[0] = 0x00;
            r.data[1] = 0x03;
            r.data[2] = 0x6E;
            r.data[3] = 0xF1;
            r.data[4] = 0x90;
            set_length(r, 12);
            uds_client_rx(&e.client, r);
        }
    };
    UdsTarget fd = engine;
    fd.fd = true;
    fd.tx_len = 64;
    Bytes req = {0x2E, 0xF1, 0x90};
    for (int i = 0; i < 37; ++i) req.push_back(static_cast<uint8_t>(i));
    const UdsResult r = uds_request(ecu.client, ecu.send(), fd, req, quick);
    CHECK(r.error.empty());
    CHECK(r.response == Bytes{0x6E, 0xF1, 0x90});
    REQUIRE(ecu.sent.size() == 1);        // 40 bytes fit one 64-byte FD single frame
    CHECK(ecu.sent[0].data[0] == 0x00);   // FD long single frame escape
    CHECK(ecu.sent[0].data[1] == 40);
    CHECK(has_flag(ecu.sent[0], bus_flag::fd));
}

TEST_CASE("extended addressing: an address byte leads every frame")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        CHECK(m.data[0] == 0xF1);          // our tx address extension
        e.reply(0x7E8, {0x10, 0x03, 0x62, 0xF1, 0x90});
    };
    UdsTarget t = engine;
    t.tx_ae = 0xF1;
    t.rx_ae = 0x10;
    const Bytes req = {0x22, 0xF1, 0x90};
    const UdsResult r = uds_request(ecu.client, ecu.send(), t, req, quick);
    REQUIRE(r.error.empty());
    CHECK(r.response == Bytes{0x62, 0xF1, 0x90});
    CHECK(ecu.sent[0].data[0] == 0xF1);
    CHECK(ecu.sent[0].data[1] == 0x03);    // single frame PCI after the address byte
    ecu.on_frame = [](Ecu& e, const BusMessage&) { e.reply(0x7E8, {0x99, 0x03, 0x62, 0xF1, 0x90}); };
    CHECK(uds_request(ecu.client, ecu.send(), t, req, quick).error.starts_with("no response"));
}

TEST_CASE("address extension keeps every byte: 7-byte single frame and a segmented request")
{
    Ecu ecu;
    std::vector<BusMessage> seen;
    ecu.on_frame = [&seen](Ecu& e, const BusMessage& m)
    {
        seen.push_back(m);
        if ((m.data[1] >> 4) == 1) // our first frame (after the address byte): clear to send
        {
            e.reply(0x7E8, {0x10, 0x30, 0x00, 0x00});
        }
        else if ((m.data[1] >> 4) == 0 || (m.data[1] >> 4) == 2)
        {
            e.reply(0x7E8, {0x10, 0x02, 0x6E, 0x00});
        }
    };
    UdsTarget t = engine;
    t.tx_ae = 0xF1;
    t.rx_ae = 0x10;
    // 6 bytes fit one frame with the address byte (AE + PCI + 6 = 8); the 7th byte must not be lost.
    const Bytes six = {0x2E, 0x01, 0x02, 0x03, 0x04, 0x05};
    CHECK(uds_request(ecu.client, ecu.send(), t, six, quick).error.empty());
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].data[1] == 0x06);
    CHECK(seen[0].data[7] == 0x05); // last payload byte present
    seen.clear();
    // 7 bytes no longer fit: first frame (5 bytes) + one consecutive frame (2 bytes).
    const Bytes seven = {0x2E, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    CHECK(uds_request(ecu.client, ecu.send(), t, seven, quick).error.empty());
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].data[1] == 0x10);
    CHECK(seen[0].data[2] == 0x07);
    CHECK(seen[0].data[7] == 0x04); // FF carries 5 bytes: 2E 01 02 03 04
    CHECK(seen[1].data[1] == 0x21);
    CHECK(seen[1].data[2] == 0x05);
    CHECK(seen[1].data[3] == 0x06); // nothing dropped
    // Functional: 7 bytes with an address byte do not fit a single frame -> refused, nothing sent.
    seen.clear();
    UdsTarget fn = t;
    fn.tx_id = 0x7DF;
    CHECK(uds_functional(ecu.client, ecu.send(), fn, seven, std::chrono::milliseconds(10)).empty());
    CHECK(seen.empty());
}

TEST_CASE("29-bit sweep ignores J1939 traffic and answers to other testers")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        if (m.id == 0x18DA10F1)
        {
            e.reply(0x18FEF100, {0x02, 0x7E, 0x00}, true); // J1939 PGN FEF1, looks like a UDS answer
            e.reply(0x18DAF210, {0x02, 0x7E, 0x00}, true); // answer to tester F2, not us
            e.reply(0x18DAF110, {0x02, 0x7E, 0x00}, true); // the real one
        }
    };
    UdsSweep sw;
    sw.transport.extended = true;
    sw.candidates = {0x10};
    sw.wait = std::chrono::milliseconds(20);
    const auto found = uds_sweep(ecu.client, ecu.send(), sw);
    REQUIRE(found.size() == 1);
    CHECK(found[0].rx_id == 0x18DAF110);
    CHECK(found[0].tx_id == 0x18DA10F1);
}

TEST_CASE("sweep: 11-bit range finds nodes, stop ends it, 29-bit is exact")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        if (m.id == 0x700) e.reply(0x708, {0x02, 0x7E, 0x00}); // exact: rx = tx + 8
        if (m.id == 0x701) e.reply(0x790, {0x02, 0x7E, 0x00}); // odd id: guessed pairing
    };
    UdsSweep sw;
    sw.transport.iface = 0;
    sw.first = 0x700;
    sw.last = 0x702;
    sw.wait = std::chrono::milliseconds(20);
    const auto found = uds_sweep(ecu.client, ecu.send(), sw);
    REQUIRE(found.size() == 2);
    CHECK(found[0].rx_id == 0x708);
    CHECK(found[0].tx_id == 0x700);
    CHECK(!found[0].guessed);
    CHECK(found[1].rx_id == 0x790);
    CHECK(found[1].guessed);

    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        // Only a proper single frame (PCI 02, then 3E 00) is answered.
        if (m.id == 0x18DA10F1 && m.data[0] == 0x02 && m.data[1] == 0x3E) e.reply(0x18DAF110, {0x02, 0x7E, 0x00}, true);
    };
    UdsSweep sw29;
    sw29.transport.extended = true;
    sw29.candidates = {0x10, 0x11};
    sw29.wait = std::chrono::milliseconds(20);
    const auto f29 = uds_sweep(ecu.client, ecu.send(), sw29);
    REQUIRE(f29.size() == 1);
    CHECK(f29[0].tx_id == 0x18DA10F1);
    CHECK(f29[0].rx_id == 0x18DAF110);
    CHECK(!f29[0].guessed);

    std::atomic<bool> stop{true};
    CHECK(uds_sweep(ecu.client, ecu.send(), sw, {}, &stop).empty());
}

TEST_CASE("service scan: not-supported vs supported, reads only")
{
    Ecu ecu;
    std::vector<uint8_t> sent;
    ecu.on_frame = [&sent](Ecu& e, const BusMessage& m)
    {
        const uint8_t sid = m.data[1];
        sent.push_back(sid);
        if (sid == 0x22) e.reply(0x7E8, {0x03, 0x62, 0x00, 0x00});
        else if (sid == 0x19) e.reply(0x7E8, {0x03, 0x7F, 0x19, 0x12});
        else e.reply(0x7E8, {0x03, 0x7F, sid, 0x11});
    };
    const auto probes = uds_service_scan(ecu.client, ecu.send(), engine, quick);
    const auto by = [&](uint8_t s) { return *std::ranges::find(probes, s, &UdsServiceProbe::sid); };
    CHECK(by(0x22).supported());
    CHECK(by(0x19).supported());
    CHECK(!by(0x23).supported());
    CHECK(by(0x23).answered);
    // Nothing that writes, resets or runs anything is sent.
    CHECK(sent == std::vector<uint8_t>{0x19, 0x22, 0x23, 0x24, 0x3E});
}

TEST_CASE("DID scan collects positive answers, skips out-of-range")
{
    Ecu ecu;
    ecu.on_frame = [](Ecu& e, const BusMessage& m)
    {
        const uint16_t did = static_cast<uint16_t>(m.data[2] << 8 | m.data[3]);
        if (did == 0xF190) e.reply(0x7E8, {0x06, 0x62, 0xF1, 0x90, 'A', 'B', 'C'});
        else e.reply(0x7E8, {0x03, 0x7F, 0x22, 0x31});
    };
    const auto dids = uds_did_scan(ecu.client, ecu.send(), engine, 0xF18F, 0xF191, quick);
    REQUIRE(dids.size() == 1);
    CHECK(dids[0].did == 0xF190);
    CHECK(dids[0].data == Bytes{'A', 'B', 'C'});
}

TEST_CASE("DID and service names in descriptions")
{
    CHECK(uds_did_name(0xF190) == "VIN");
    CHECK(uds_describe_request(Bytes{0x22, 0xF1, 0x90}) == "ReadDataByIdentifier F190 VIN");
    CHECK(uds_describe_request(Bytes{0x10, 0x83}) == "DiagnosticSessionControl 0x03 extendedDiagnosticSession (suppress positive response)");
    CHECK(uds_describe_request(Bytes{0x11, 0x01}) == "EcuReset 0x01 hardReset");
    CHECK(uds_describe_request(Bytes{0x19, 0x02, 0xFF}) == "ReadDTCInformation 0x02 reportDTCByStatusMask, status mask 0xFF");
    CHECK(uds_describe_request(Bytes{0x19, 0x04, 0xF0, 0x00, 0x16, 0xFF}) == "ReadDTCInformation 0x04 reportDTCSnapshotRecordByDTCNumber, DTC F00016 record 0xFF (all)");
    CHECK(uds_describe_request(Bytes{0x14, 0xFF, 0xFF, 0xFF}) == "ClearDiagnosticInformation group FFFFFF (all DTCs)");
    CHECK(uds_describe_request(Bytes{0x27, 0x03}) == "SecurityAccess 0x03 requestSeed level 2");
    CHECK(uds_describe_request(Bytes{0x27, 0x02, 0xAB, 0xCD}) == "SecurityAccess 0x02 sendKey level 1, key AB CD");
    CHECK(uds_describe_request(Bytes{0x31, 0x01, 0xFF, 0x00, 0x05}) == "RoutineControl 0x01 startRoutine, routine FF00, options 05");
    CHECK(uds_describe_request(Bytes{0x23, 0x24, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04}) == "ReadMemoryByAddress address 0x10000000 size 0x4");
    CHECK(uds_describe_request(Bytes{0x2F, 0x01, 0x02, 0x03, 0x7F}) == "InputOutputControlByIdentifier 0102 0x03 shortTermAdjustment, state 7F");
    CHECK(uds_describe(Bytes{0x22, 0xF1, 0x90}, Bytes{0x62, 0xF1, 0x90, 'X'}).starts_with("DID F190 VIN = 58"));
    CHECK(uds_describe({}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x05}) == "DTC count 5");
}

TEST_CASE("learning: 11-bit pairs, functional guesses refined, 29-bit from the answer")
{
    UdsLearner l;
    // Request on 0x7E0, answer on 0x7E8 within the window: an exact pair.
    CHECK(!uds_learn(l, frame(0x7E0, {0x02, 0x10, 0x03}, false, 0, 1'000)));
    CHECK(uds_learn(l, frame(0x7E8, {0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}, false, 0, 2'000)));
    REQUIRE(l.ecus.size() == 1);
    CHECK(l.ecus[0].tx_id == 0x7E0);
    CHECK(l.ecus[0].rx_id == 0x7E8);
    CHECK(!l.ecus[0].guessed);
    // The same answer again teaches nothing new.
    CHECK(!uds_learn(l, frame(0x7E8, {0x02, 0x7E, 0x00}, false, 0, 3'000)));

    // Functional request: the answer's request id is guessed, later replaced by the real one.
    uds_learn(l, frame(0x7DF, {0x02, 0x3E, 0x00}, false, 0, 10'000));
    CHECK(uds_learn(l, frame(0x7EA, {0x02, 0x7E, 0x00}, false, 0, 11'000)));
    CHECK(l.ecus[1].tx_id == 0x7E2);
    CHECK(l.ecus[1].guessed);
    uds_learn(l, frame(0x7E2, {0x03, 0x22, 0xF1, 0x90}, false, 0, 20'000));
    CHECK(uds_learn(l, frame(0x7EA, {0x10, 0x14, 0x62, 0xF1, 0x90, 1, 2, 3}, false, 0, 21'000)));
    CHECK(l.ecus[1].tx_id == 0x7E2);
    CHECK(!l.ecus[1].guessed);

    // Two requests outstanding: the answer on request + 8 belongs to that one, which is consumed.
    uds_learn(l, frame(0x7E0, {0x02, 0x10, 0x03}, false, 0, 25'000));
    uds_learn(l, frame(0x7E3, {0x02, 0x10, 0x03}, false, 0, 25'500));
    CHECK(!uds_learn(l, frame(0x7E8, {0x02, 0x50, 0x03}, false, 0, 26'000))); // known already
    CHECK(uds_learn(l, frame(0x7EB, {0x02, 0x50, 0x03}, false, 0, 26'500)));
    CHECK(l.ecus.back().tx_id == 0x7E3);
    CHECK(!uds_learn(l, frame(0x7EC, {0x02, 0x50, 0x03}, false, 0, 27'000))); // nothing left to answer

    // An answer long after its request (or with no request) pairs with nothing.
    uds_learn(l, frame(0x700, {0x02, 0x10, 0x01}, false, 0, 30'000));
    CHECK(!uds_learn(l, frame(0x708, {0x02, 0x50, 0x01}, false, 0, 30'000 + 3'000'000'000LL)));

    // Another interface is another ECU, even on the same ids.
    uds_learn(l, frame(0x7E0, {0x02, 0x10, 0x03}, false, 1, 40'000));
    CHECK(uds_learn(l, frame(0x7E8, {0x02, 0x50, 0x03}, false, 1, 41'000)));
    CHECK(l.ecus.size() == 4);

    // 29-bit normal fixed addressing: the answer names both ends. Requests alone do not count.
    CHECK(!uds_learn(l, frame(0x18DA10F1, {0x02, 0x3E, 0x00}, true)));
    CHECK(uds_learn(l, frame(0x18DAF110, {0x02, 0x7E, 0x00}, true)));
    CHECK(l.ecus.back().tx_id == 0x18DA10F1);
    CHECK(l.ecus.back().extended);

    // Not UDS: plain data, J1939, error frames.
    CHECK(!uds_learn(l, frame(0x123, {0x08, 0x01, 0x02, 0x03})));
    CHECK(!uds_learn(l, frame(0x18FEF100, {0x02, 0x7E, 0x00}, true)));
    BusMessage err = frame(0x7E8, {0x02, 0x7E, 0x00});
    err.errors = bus_error::bit;
    CHECK(!uds_learn(l, err));
}
