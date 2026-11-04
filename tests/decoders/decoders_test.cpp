// UDS and J1939 protocol decoders.
//
// The single-frame, multi-frame, negative-response and J1939 PGN cases started
// life as src/decoders/test/DecoderTest.cpp (never compiled), then became a Qt
// Test, and are doctest now with the same vectors. The stateful paths
// (multi-frame reassembly, reset, interleaved sessions) are where a decoder
// normally breaks.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <initializer_list>

#include "decoders/decoders.h"

namespace
{

struct FrameOpts
{
    bool extended = false;
    bool rx = true;
    uint16_t iface = 0;
    int len = -1; // -1: number of bytes given
};

[[nodiscard]] BusMessage frame(uint32_t id, std::initializer_list<uint8_t> bytes, FrameOpts o = {})
{
    BusMessage m{.id = id, .iface = o.iface};
    if (o.extended)
    {
        m.flags |= bus_flag::extended;
    }
    if (!o.rx)
    {
        m.flags |= bus_flag::tx;
    }
    std::copy(bytes.begin(), bytes.end(), m.data.begin());
    set_length(m, o.len < 0 ? static_cast<int>(bytes.size()) : o.len);
    return m;
}

// First frame + one consecutive frame of a 10-byte ReadDataByIdentifier request.
const auto uds_ff = [](uint32_t id = 0x7E0, bool ext = false)
{ return frame(id, {0x10, 0x0A, 0x22, 3, 4, 5, 6, 7}, {.extended = ext}); };
const auto uds_cf = [](uint32_t id = 0x7E0, bool ext = false)
{ return frame(id, {0x21, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0, 0}, {.extended = ext}); };

// The 33-byte ReadDataByIdentifier 0xF180 response from the issue #38 screenshot.
constexpr uint8_t isotp_response[5][8] = {
    {0x10, 0x21, 0x62, 0xF1, 0x80, 0x4D, 0x33, 0x30},
    {0x21, 0x4C, 0x2E, 0x5F, 0x5F, 0x46, 0x42, 0x4C},
    {0x22, 0x41, 0x2E, 0x42, 0x2E, 0x30, 0x30, 0x2E},
    {0x23, 0x30, 0x30, 0x31, 0x2E, 0x30, 0x30, 0x2E},
    {0x24, 0x65, 0x6C, 0x6F, 0x62, 0x61, 0x75, 0xFF},
};
constexpr uint32_t response_id = 0x18DAF9FE;

[[nodiscard]] BusMessage response_frame(int i, bool rx = true, uint16_t iface = 0)
{
    const auto* r = isotp_response[i];
    return frame(response_id, {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]},
                 {.extended = true, .rx = rx, .iface = iface});
}

[[nodiscard]] DecodeStatus expected_status(int i)
{
    return i == 4 ? DecodeStatus::Completed : DecodeStatus::Consumed;
}

} // namespace

// --- UDS ---

// Original DecoderTest case: 0x02 0x10 0x01 -> DiagnosticSessionControl.
TEST_CASE("uds single frame")
{
    UdsDecoder d;
    ProtocolMessage out;
    REQUIRE(uds_decode(d, frame(0x7E0, {0x02, 0x10, 0x01}, {.len = 8}), out) == DecodeStatus::Completed);
    CHECK(out.name == "DiagnosticSessionControl");
    CHECK(out.protocol == "uds");
    CHECK(out.id == 0x10u);
    CHECK(out.type == MessageType::Request);
    REQUIRE(out.payload.size() == 2);
    CHECK(out.payload[0] == 0x10);
}

// Original DecoderTest case: first frame then one consecutive frame.
TEST_CASE("uds multi frame")
{
    UdsDecoder d;
    ProtocolMessage out;
    CHECK(uds_decode(d, uds_ff(), out) == DecodeStatus::Consumed);
    REQUIRE(uds_decode(d, uds_cf(), out) == DecodeStatus::Completed);
    CHECK(out.name == "ReadDataByIdentifier");
    CHECK(out.payload.size() == 10);
    CHECK(out.id == 0x22u);
    CHECK(out.type == MessageType::Request);
    CHECK(out.raw_frames.size() == 2);
}

// Original DecoderTest case: 0x7F with NRC 0x33.
TEST_CASE("uds negative response")
{
    UdsDecoder d;
    ProtocolMessage out;
    REQUIRE(uds_decode(d, frame(0x7E8, {0x03, 0x7F, 0x22, 0x33}, {.len = 8}), out) == DecodeStatus::Completed);
    CHECK(out.type == MessageType::NegativeResponse);
    CHECK(out.name == "NegativeResponse");
    CHECK(out.description == "negative response: Security Access Denied");
}

TEST_CASE("uds ignores RTR and error frames")
{
    UdsDecoder d;
    ProtocolMessage out;

    auto rtr = frame(0x7E0, {0x02, 0x10}, {.len = 8});
    rtr.flags |= bus_flag::rtr;
    CHECK(uds_decode(d, rtr, out) == DecodeStatus::Ignored);

    auto err = frame(0x7E0, {0x02, 0x10}, {.len = 8});
    err.errors = bus_error::generic;
    CHECK(uds_decode(d, err, out) == DecodeStatus::Ignored);
}

TEST_CASE("uds ignores empty frame")
{
    UdsDecoder d;
    ProtocolMessage out;
    CHECK(uds_decode(d, frame(0x7E0, {}), out) == DecodeStatus::Ignored);
}

TEST_CASE("uds ignores unknown service")
{
    UdsDecoder d;
    ProtocolMessage out;
    CHECK(uds_decode(d, frame(0x7E0, {0x02, 0x00, 0x01}, {.len = 8}), out) == DecodeStatus::Ignored);
}

// A consecutive frame with no session in progress must not be mistaken for data.
TEST_CASE("uds consecutive frame without first frame is ignored")
{
    UdsDecoder d;
    ProtocolMessage out;
    CHECK(uds_decode(d, frame(0x7E0, {0x21, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), out)
          == DecodeStatus::Ignored);
}

// reset is called when a measurement starts; a half-received transfer from the
// previous run must not complete afterwards and emit a bogus message.
TEST_CASE("uds reset drops pending reassembly")
{
    UdsDecoder d;
    ProtocolMessage out;
    CHECK(uds_decode(d, uds_ff(), out) == DecodeStatus::Consumed);
    d = {};
    CHECK(uds_decode(d, uds_cf(), out) == DecodeStatus::Ignored);
}

// Two ECUs transferring at once must not have their payloads mixed.
TEST_CASE("uds interleaved sessions are kept apart")
{
    UdsDecoder d;
    ProtocolMessage out;

    const auto ff = [](uint32_t id, uint8_t sid, uint8_t fill)
    { return frame(id, {0x10, 0x0A, sid, fill, fill, fill, fill, fill}); };
    const auto cf = [](uint32_t id, uint8_t fill)
    { return frame(id, {0x21, fill, fill, fill, fill, fill, 0, 0}); };

    // Start both transfers, then finish them in the opposite order.
    CHECK(uds_decode(d, ff(0x7E0, 0x22, 0x11), out) == DecodeStatus::Consumed);
    CHECK(uds_decode(d, ff(0x7E1, 0x2E, 0x22), out) == DecodeStatus::Consumed);

    REQUIRE(uds_decode(d, cf(0x7E1, 0xBB), out) == DecodeStatus::Completed);
    CHECK(out.id == 0x2Eu);
    REQUIRE(out.payload.size() == 10);
    CHECK(out.payload[1] == 0x22);

    REQUIRE(uds_decode(d, cf(0x7E0, 0xAA), out) == DecodeStatus::Completed);
    CHECK(out.id == 0x22u);
    REQUIRE(out.payload.size() == 10);
    CHECK(out.payload[1] == 0x11);
}

// --- J1939 ---

// Original DecoderTest case: PGN 65263 with source address 1, priority 6.
TEST_CASE("j1939 single frame")
{
    J1939Decoder d;
    ProtocolMessage out;
    REQUIRE(j1939_decode(d, frame(0x18FEEF01, {0, 1, 2, 3, 4, 5, 6, 7}, {.extended = true}), out)
            == DecodeStatus::Completed);
    CHECK(out.name == "Engine Fluid Level/Pressure");
    CHECK(out.protocol == "J1939");
    CHECK(out.id == 0xFEEFu);
    CHECK(out.type == MessageType::Request);
    CHECK(out.payload.size() == 8);
}

// J1939 is an extended-identifier protocol; 11-bit frames are not its business.
TEST_CASE("j1939 ignores standard frames")
{
    J1939Decoder d;
    ProtocolMessage out;
    CHECK(j1939_decode(d, frame(0x123, {0, 1, 2, 3, 4, 5, 6, 7}), out) == DecodeStatus::Ignored);
}

TEST_CASE("j1939 extracts address metadata")
{
    J1939Decoder d;
    ProtocolMessage out;
    REQUIRE(j1939_decode(d, frame(0x18FEEF2A, {}, {.extended = true, .len = 8}), out) == DecodeStatus::Completed);
    CHECK(out.metadata.at("Source Address") == 0x2Au);
    CHECK(out.metadata.at("Priority") == 6u);
    CHECK(out.metadata.at("PDU Format") == 0xFEu);
}

// Issue #38: J1939's catch-all "single-packet PGN" branch used to claim every
// 29-bit frame outright (including PGN 0xDA00/0xDB00, which ISO 15765-4
// UDS-on-CAN extended addressing reuses), so the UDS decoder was never reached.
TEST_CASE("j1939 ignores diagnostic PGN so UDS can claim it")
{
    J1939Decoder d;
    ProtocolMessage out;
    CHECK(j1939_decode(d, frame(0x18DAFEF9, {}, {.extended = true, .len = 8}), out) == DecodeStatus::Ignored);
    CHECK(j1939_decode(d, frame(0x18DBFEF9, {}, {.extended = true, .len = 8}), out) == DecodeStatus::Ignored);
}

// BAM transfer of a 9-byte VIN-sized payload: TP.CM announce, then two TP.DT.
// Layout from J1939-21: CM = [32, size lo, size hi, packets, 0xFF, PGN lo, mid, hi].
TEST_CASE("j1939 BAM reassembly")
{
    J1939Decoder d;
    ProtocolMessage out;
    const FrameOpts ext{.extended = true};
    CHECK(j1939_decode(d, frame(0x18ECFF00, {32, 9, 0, 2, 0xFF, 0xEC, 0xFE, 0x00}, ext), out)
          == DecodeStatus::Consumed);
    CHECK(j1939_decode(d, frame(0x18EBFF00, {1, 'A', 'B', 'C', 'D', 'E', 'F', 'G'}, ext), out)
          == DecodeStatus::Consumed);
    REQUIRE(j1939_decode(d, frame(0x18EBFF00, {2, 'H', 'I', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, ext), out)
            == DecodeStatus::Completed);
    CHECK(out.id == 0xFEECu);
    CHECK(out.name == "Vehicle Identification (VIN)");
    CHECK(out.payload == std::vector<uint8_t>{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I'});
    CHECK(out.raw_frames.size() == 3);
    CHECK(out.metadata.at("PDU Format") == 0xECu);
}

TEST_CASE("j1939 TP.DT out of sequence drops the session")
{
    J1939Decoder d;
    ProtocolMessage out;
    const FrameOpts ext{.extended = true};
    CHECK(j1939_decode(d, frame(0x18ECFF00, {32, 9, 0, 2, 0xFF, 0xEC, 0xFE, 0x00}, ext), out)
          == DecodeStatus::Consumed);
    CHECK(j1939_decode(d, frame(0x18EBFF00, {2, 'H', 'I', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, ext), out)
          == DecodeStatus::Ignored);
    CHECK(d.sessions.empty());
}

// --- UDS on 29-bit (extended) identifiers ---

TEST_CASE("uds extended single frame extracts address metadata")
{
    UdsDecoder d;
    ProtocolMessage out;
    REQUIRE(uds_decode(d, frame(0x18DAFEF9, {0x02, 0x10, 0x01}, {.extended = true, .len = 8}), out)
            == DecodeStatus::Completed);
    CHECK(out.protocol == "uds");
    CHECK(out.metadata.at("Source Address") == 0xF9u);
    CHECK(out.metadata.at("Target Address") == 0xFEu);
}

TEST_CASE("uds extended multi frame extracts address metadata")
{
    UdsDecoder d;
    ProtocolMessage out;
    CHECK(uds_decode(d, uds_ff(0x18DAFEF9, true), out) == DecodeStatus::Consumed);
    REQUIRE(uds_decode(d, uds_cf(0x18DAFEF9, true), out) == DecodeStatus::Completed);
    CHECK(out.metadata.at("Source Address") == 0xF9u);
    CHECK(out.metadata.at("Target Address") == 0xFEu);
}

TEST_CASE("uds extended addressing skips the leading address byte")
{
    UdsDecoder d;
    d.addr_ext = true;
    ProtocolMessage out;
    // Address byte 0x10, then single frame 0x02 0x10 0x01.
    REQUIRE(uds_decode(d, frame(0x7E0, {0x10, 0x02, 0x10, 0x01}, {.len = 8}), out) == DecodeStatus::Completed);
    CHECK(out.payload == std::vector<uint8_t>{0x10, 0x01});
    CHECK(out.metadata.at("Address Extension") == 0x10u);

    // Multi frame with the same address byte in front of every frame.
    CHECK(uds_decode(d, frame(0x7E0, {0x10, 0x10, 0x0A, 0x22, 1, 2, 3, 4}, {.len = 8}), out) == DecodeStatus::Consumed);
    REQUIRE(uds_decode(d, frame(0x7E0, {0x10, 0x21, 5, 6, 7, 8, 9, 0}, {.len = 8}), out) == DecodeStatus::Completed);
    CHECK(out.payload.size() == 10);
    CHECK(out.payload[0] == 0x22);
}

// --- protocol_decode ---

// Issue #38 follow-up: a UDS request Kraken Explorer transmits itself must decode
// just like one it observes as RX -- a real external device never loops
// frames back, so the TX side alone has to carry the whole ISO-TP session.
TEST_CASE("protocol decodes TX on its own")
{
    ProtocolDecoder d;
    ProtocolMessage out;
    for (int i = 0; i < 5; ++i)
    {
        CAPTURE(i);
        CHECK(protocol_decode(d, response_frame(i, false), out) == expected_status(i));
    }
    CHECK(out.protocol == "uds");
    CHECK(out.payload.size() == 0x21);
}

// Issue #38 follow-up: interfaces that loop transmitted frames back to their
// own RX path (SocketCAN/vcan, some real adapters) deliver every TX'd frame
// twice. RX and TX sessions on the same ID are tracked independently, so a
// duplicate on one side must not kill the other side's sequence tracking.
TEST_CASE("protocol loopback duplicate on one direction does not corrupt the other")
{
    ProtocolDecoder d;
    ProtocolMessage out;

    // FF and CF1 each sent then looped back.
    for (int i = 0; i < 2; ++i)
    {
        CHECK(protocol_decode(d, response_frame(i, false), out) == DecodeStatus::Consumed);
        CHECK(protocol_decode(d, response_frame(i, true), out) == DecodeStatus::Consumed);
    }
    // The RX side alone must still complete despite the interleaved TX duplicates,
    // and then the TX side too.
    for (const bool rx : {true, false})
    {
        CAPTURE(rx);
        for (int i = 2; i < 5; ++i)
        {
            CAPTURE(i);
            CHECK(protocol_decode(d, response_frame(i, rx), out) == expected_status(i));
        }
        CHECK(out.protocol == "uds");
    }
}

TEST_CASE("protocol decodes genuine RX multi frame")
{
    ProtocolDecoder d;
    ProtocolMessage out;
    for (int i = 0; i < 5; ++i)
    {
        CAPTURE(i);
        CHECK(protocol_decode(d, response_frame(i), out) == expected_status(i));
    }
    CHECK(out.protocol == "uds");
    CHECK(out.name == "ReadDataByIdentifier");
    CHECK(out.type == MessageType::PositiveResponse);
}

// Diagnostic IDs are commonly reused across separate CAN buses; both channels
// must reassemble independently even when their frames interleave.
TEST_CASE("protocol keeps sessions separate per channel")
{
    ProtocolDecoder d;
    ProtocolMessage out;
    for (int i = 0; i < 5; ++i)
    {
        for (uint16_t iface = 0; iface < 2; ++iface)
        {
            CAPTURE(i);
            CAPTURE(iface);
            CHECK(protocol_decode(d, response_frame(i, true, iface), out) == expected_status(i));
        }
    }
}

// LIN diagnostic frame (ISO 17987): [NAD, PCI, SID, data...] -> NAD stripped, UDS single frame.
TEST_CASE("protocol decodes LIN master request")
{
    ProtocolDecoder d;
    ProtocolMessage out;
    auto lin = frame(0x3C, {0x7F, 0x02, 0x10, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
    lin.type = BusType::LIN;
    REQUIRE(protocol_decode(d, lin, out) == DecodeStatus::Completed);
    CHECK(out.name == "DiagnosticSessionControl");
    CHECK(out.payload == std::vector<uint8_t>{0x10, 0x01});
    REQUIRE(out.raw_frames.size() == 1);
    CHECK(out.raw_frames[0].type == BusType::LIN);

    lin.id = 0x10; // not a diagnostic frame
    CHECK(protocol_decode(d, lin, out) == DecodeStatus::Ignored);
}

TEST_CASE("protocol reset drops pending reassembly")
{
    ProtocolDecoder d;
    ProtocolMessage out;
    CHECK(protocol_decode(d, uds_ff(), out) == DecodeStatus::Consumed);
    protocol_reset(d);
    CHECK(protocol_decode(d, uds_cf(), out) == DecodeStatus::Ignored);
}
