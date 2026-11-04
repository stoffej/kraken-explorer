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

#include <algorithm>
#include <format>

#include "decoders/decoders.h"
#include "decoders/uds_client.h"

namespace
{

constexpr uint32_t extended_session_flag = 0x80000000u;
constexpr uint32_t tx_session_flag       = 0x40000000u;

// RX and TX traffic on the same ID get independent session slots. Without
// this, an interface that loops transmitted frames back on the RX path
// (SocketCAN/vcan, some real adapters) delivers every sent frame twice, and
// a duplicate consecutive frame trips the strict sequence-number check below
// and kills the session. Keying by direction also lets Kraken Explorer decode a UDS
// request it transmits itself independently of whatever it receives back.
//
// The interface/channel is folded in too: diagnostic IDs (0x7E0/0x7E8,
// 0x18DAxxyy, ...) are commonly reused across separate buses, so without a
// per-channel key an interleaved frame from a second channel would look
// like a sequence-number violation and kill the first channel's session.
[[nodiscard]] uint64_t session_key(const BusMessage& frame, uint8_t ae_byte) noexcept
{
    uint32_t key32 = has_flag(frame, bus_flag::extended) ? (frame.id | extended_session_flag) : frame.id;
    if (has_flag(frame, bus_flag::tx))
    {
        key32 |= tx_session_flag;
    }
    // Fold the interface and (with extended addressing) the address byte in, so parallel sessions
    // on reused ids stay apart.
    return (static_cast<uint64_t>(ae_byte) << 48) | (static_cast<uint64_t>(frame.iface) << 32) | key32;
}

// UDS service IDs begin at 0x10. Anything below that cannot be a valid SID.
[[nodiscard]] constexpr bool is_valid_sid(uint8_t sid) noexcept
{
    return sid >= 0x10;
}

void fill_out(ProtocolMessage& out, std::vector<uint8_t> payload, std::vector<BusMessage> frames,
              const BusMessage& last)
{
    const uint8_t sid = payload.front();
    out.ts_ns = frames.front().ts_ns;
    out.payload = std::move(payload);
    out.raw_frames = std::move(frames);
    out.protocol = "uds";
    out.id = sid;
    out.name = uds_service_name(sid);
    out.metadata.clear();

    if (sid == 0x7F)
    {
        out.type = MessageType::NegativeResponse;
        out.description = (out.payload.size() >= 3)
            ? "negative response: " + uds_nrc_name(out.payload[2])
            : "negative response: incomplete";
    }
    else if (sid & 0x40)
    {
        out.type = MessageType::PositiveResponse;
        out.description = uds_describe({}, out.payload);
    }
    else
    {
        out.type = MessageType::Request;
        out.description = uds_describe_request(out.payload);
    }

    if (has_flag(last, bus_flag::extended))
    {
        // ISO 15765-4 extended addressing: SA in the low byte, target
        // address (DA) in the PS byte of the 29-bit identifier.
        out.metadata["Source Address"] = last.id & 0xFF;
        out.metadata["Target Address"] = (last.id >> 8) & 0xFF;
    }
}

} // namespace

DecodeStatus uds_decode(UdsDecoder& d, const BusMessage& frame, ProtocolMessage& out)
{
    // Extended addressing (CPAC internal bus, some gateways) leads every frame with an address
    // byte; the LIN diagnostic path passes a synthetic LIN frame that already has its NAD stripped,
    // so it never gets the offset.
    const int ae = (d.addr_ext && frame.type == BusType::CAN) ? 1 : 0;
    const int len = std::min<int>(frame.len, bus_max_data_bytes) - ae;
    if (is_error_frame(frame) || has_flag(frame, bus_flag::rtr) || len < 1)
    {
        return DecodeStatus::Ignored;
    }

    const uint8_t* b = frame.data.data() + ae;
    const uint8_t type = (b[0] >> 4) & 0x0F;
    const uint8_t ae_byte = ae ? frame.data[0] : 0;
    const uint64_t key = session_key(frame, ae_byte);

    if (has_flag(frame, bus_flag::extended))
    {
        const uint8_t pf = (frame.id >> 16) & 0xFF;
        if (pf == 0xEC || pf == 0xEB) // J1939 TP.CM or TP.DT
        {
            return DecodeStatus::Ignored;
        }
    }

    if (type == 0) // Single Frame
    {
        int size = b[0] & 0x0F;
        int offset = 1;

        // ISO 15765-2:2016 CAN FD long single frame: size nibble == 0
        if (size == 0 && len > 1)
        {
            size = b[1];
            offset = 2;
        }

        if (size > 0 && offset + size <= len)
        {
            if (!is_valid_sid(b[offset]))
            {
                return DecodeStatus::Ignored;
            }
            fill_out(out, {b + offset, b + offset + size}, {frame}, frame);
            if (ae)
            {
                out.metadata["Address Extension"] = ae_byte;
            }
            d.sessions.erase(key);
            return DecodeStatus::Completed;
        }
    }
    else if (type == 1) // First Frame
    {
        int size = ((b[0] & 0x0F) << 8) | b[1];
        int offset = 2;

        // ISO 15765-2:2016 CAN FD long first frame: 12-bit size field == 0
        if (size == 0 && len >= 6)
        {
            // ponytail: sizes >= 2^31 wrap negative like the Qt version did; ISO-TP never gets there. uint32_t size if a spec ever allows it.
            size = static_cast<int>((static_cast<uint32_t>(b[2]) << 24) | (static_cast<uint32_t>(b[3]) << 16)
                                    | (static_cast<uint32_t>(b[4]) << 8) | b[5]);
            offset = 6;
        }

        // Require at least the SID byte to be present before opening a session.
        if (offset >= len || !is_valid_sid(b[offset]))
        {
            return DecodeStatus::Ignored;
        }

        auto& s = d.sessions[key];
        s.data.assign(b + offset, b + len);
        s.expected_size = size;
        s.next_sn = 1;
        s.frames.assign(1, frame);
        return DecodeStatus::Consumed;
    }
    else if (type == 2) // Consecutive Frame
    {
        const auto it = d.sessions.find(key);
        if (it != d.sessions.end())
        {
            auto& s = it->second;
            if ((b[0] & 0x0F) != s.next_sn)
            {
                d.sessions.erase(it);
                return DecodeStatus::Ignored;
            }

            s.frames.push_back(frame);
            for (int i = 1; i < len && static_cast<int>(s.data.size()) < s.expected_size; ++i)
            {
                s.data.push_back(b[i]);
            }
            s.next_sn = (s.next_sn + 1) % 16;

            if (static_cast<int>(s.data.size()) < s.expected_size)
            {
                return DecodeStatus::Consumed;
            }
            if (s.data.empty())
            {
                d.sessions.erase(it);
                return DecodeStatus::Ignored;
            }
            fill_out(out, std::move(s.data), std::move(s.frames), frame);
            if (ae)
            {
                out.metadata["Address Extension"] = ae_byte;
            }
            d.sessions.erase(it);
            return DecodeStatus::Completed;
        }
    }
    else if (type == 3) // Flow Control: only consumed while a session is active
    {
        return d.sessions.contains(key) ? DecodeStatus::Consumed : DecodeStatus::Ignored;
    }

    return DecodeStatus::Ignored;
}

std::string uds_service_name(uint8_t sid)
{
    if (sid == 0x7F)
    {
        return "NegativeResponse";
    }

    const bool is_response = (sid & 0x40) && sid != 0x40; // 0x40 itself is not a UDS response
    const uint8_t base = is_response ? (sid - 0x40) : sid;

    switch (base)
    {
        case 0x10: return "DiagnosticSessionControl";
        case 0x11: return "EcuReset";
        case 0x14: return "ClearDiagnosticInformation";
        case 0x19: return "ReadDTCInformation";
        case 0x22: return "ReadDataByIdentifier";
        case 0x23: return "ReadMemoryByAddress";
        case 0x27: return "SecurityAccess";
        case 0x28: return "CommunicationControl";
        case 0x29: return "Authentication";
        case 0x2A: return "ReadDataByPeriodicIdentifier";
        case 0x2C: return "DynamicallyDefineDataIdentifier";
        case 0x2E: return "WriteDataByIdentifier";
        case 0x2F: return "InputOutputControlByIdentifier";
        case 0x31: return "RoutineControl";
        case 0x34: return "RequestDownload";
        case 0x35: return "RequestUpload";
        case 0x36: return "TransferData";
        case 0x37: return "RequestTransferExit";
        case 0x38: return "RequestFileTransfer";
        case 0x3D: return "WriteMemoryByAddress";
        case 0x3E: return "TesterPresent";
        case 0x83: return "AccessTimingParameter";
        case 0x84: return "SecuredDataTransmission";
        case 0x85: return "ControlDTCSetting";
        case 0x86: return "ResponseOnEvent";
        case 0x87: return "LinkControl";
        default:   return std::format("Service 0x{:02x}", sid);
    }
}

std::string uds_nrc_name(uint8_t nrc)
{
    switch (nrc)
    {
        case 0x10: return "General Reject";
        case 0x11: return "Service Not Supported";
        case 0x12: return "SubFunction Not Supported";
        case 0x13: return "Incorrect Message Length Or Invalid Format";
        case 0x14: return "Response Too Long";
        case 0x21: return "Busy Repeat Request";
        case 0x22: return "Conditions Not Correct";
        case 0x24: return "Request Sequence Error";
        case 0x25: return "No Response From Subnet Component";
        case 0x26: return "Failure Prevents Execution Of Requested Action";
        case 0x29: return "Request Out Of Range (29)";
        case 0x31: return "Request Out Of Range";
        case 0x33: return "Security Access Denied";
        case 0x34: return "Authentication Required";
        case 0x35: return "Invalid Key";
        case 0x36: return "Exceed Number Of Attempts";
        case 0x37: return "Required Time Delay Not Expired";
        case 0x38: return "Secure Data Transmission Required";
        case 0x39: return "Secure Data Transmission Not Allowed";
        case 0x3A: return "Secure Data Verification Failed";
        case 0x50: return "Certificate Verification Failed - Invalid Time Period";
        case 0x51: return "Certificate Verification Failed - Invalid Signature";
        case 0x52: return "Certificate Verification Failed - Invalid Chain Of Trust";
        case 0x53: return "Certificate Verification Failed - Invalid Type";
        case 0x54: return "Certificate Verification Failed - Invalid Format";
        case 0x55: return "Certificate Verification Failed - Invalid Content";
        case 0x56: return "Certificate Verification Failed - Invalid Scope";
        case 0x57: return "Certificate Verification Failed - Invalid Certificate (Revoked)";
        case 0x58: return "Ownership Verification Failed";
        case 0x59: return "Challenge Calculation Failed";
        case 0x5A: return "Setting Access Rights Failed";
        case 0x5B: return "Session Key Creation/Derivation Failed";
        case 0x5C: return "Configuration Data Usage Failed";
        case 0x5D: return "DeAuthentication Failed";
        case 0x70: return "Upload Download Not Accepted";
        case 0x71: return "Transfer Data Suspended";
        case 0x72: return "General Programming Failure";
        case 0x73: return "Wrong Block Sequence Counter";
        case 0x78: return "Request Correctly Received - Response Pending";
        case 0x7E: return "SubFunction Not Supported In Active Session";
        case 0x7F: return "Service Not Supported In Active Session";
        case 0x81: return "Rpm Too High";
        case 0x82: return "Rpm Too Low";
        case 0x83: return "Engine Is Running";
        case 0x84: return "Engine Is Not Running";
        case 0x85: return "Engine Run Time Too Low";
        case 0x86: return "Temperature Too High";
        case 0x87: return "Temperature Too Low";
        case 0x88: return "Vehicle Speed Too High";
        case 0x89: return "Vehicle Speed Too Low";
        case 0x8A: return "Throttle Pedal Too High";
        case 0x8B: return "Throttle Pedal Too Low";
        case 0x8C: return "Transmission Range Not In Neutral";
        case 0x8D: return "Transmission Range Not In Gear";
        case 0x8F: return "Brake Switch Not Closed";
        case 0x90: return "Shifter Lever Not In Park";
        case 0x91: return "Torque Converter Clutch Locked";
        case 0x92: return "Voltage Too High";
        case 0x93: return "Voltage Too Low";
        default:   return std::format("unknown (0x{:02x})", nrc);
    }
}
