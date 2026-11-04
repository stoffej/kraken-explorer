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

// Protocol decoders: UDS (ISO 15765-2 transport, incl. LIN diagnostic frames) and
// J1939 (single-packet PGNs, TP.CM/TP.DT reassembly). Each decoder is plain state
// plus free functions; protocol_decode() routes a frame to the right one.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/bus_message.h"

enum class DecodeStatus
{
    Ignored,   // frame not relevant to this decoder
    Consumed,  // frame accepted, sequence in progress
    Completed, // frame accepted, sequence finished, out populated
};

enum class MessageType
{
    Request,
    PositiveResponse,
    NegativeResponse,
    Unknown,
};

struct ProtocolMessage
{
    std::string name;                    // e.g. "ReadDataByIdentifier"
    std::string description;             // details or NRC info
    int64_t ts_ns = 0;                   // timestamp of the first frame
    std::vector<uint8_t> payload;        // reassembled data
    std::vector<BusMessage> raw_frames;  // the frames that made this up
    std::string protocol;                // "uds", "J1939"
    MessageType type = MessageType::Unknown;
    uint32_t id = 0;                     // SID or PGN
    std::map<std::string, uint32_t> metadata; // Priority, Source Address, ...
};

// --- UDS ---

struct IsotpSession
{
    std::vector<BusMessage> frames;
    std::vector<uint8_t> data;
    int expected_size = 0;
    int next_sn = 1;
};

struct UdsDecoder
{
    bool addr_ext = false; // the bus uses extended addressing: an address byte leads every frame
    std::unordered_map<uint64_t, IsotpSession> sessions; // keyed by id, direction, interface, address byte
};

[[nodiscard]] DecodeStatus uds_decode(UdsDecoder& d, const BusMessage& frame, ProtocolMessage& out);
[[nodiscard]] std::string uds_service_name(uint8_t sid);
[[nodiscard]] std::string uds_nrc_name(uint8_t nrc);

// --- J1939 ---

struct J1939Session
{
    std::vector<BusMessage> frames;
    std::vector<uint8_t> data;
    uint32_t pgn = 0;
    int expected_size = 0;
    int expected_packets = 0;
    int received_packets = 0;
};

struct J1939Decoder
{
    std::unordered_map<uint64_t, J1939Session> sessions; // keyed by SA, DA, direction, interface
};

[[nodiscard]] DecodeStatus j1939_decode(J1939Decoder& d, const BusMessage& frame, ProtocolMessage& out);
[[nodiscard]] uint32_t j1939_pgn(uint32_t id) noexcept;
[[nodiscard]] std::string j1939_pgn_name(uint32_t pgn);

// --- Routing ---

struct ProtocolDecoder
{
    UdsDecoder uds;
    J1939Decoder j1939;
};

// Extended ids go to J1939 first (then UDS), standard ids and LIN
// 0x3C/0x3D diagnostic frames to UDS.
[[nodiscard]] DecodeStatus protocol_decode(ProtocolDecoder& d, const BusMessage& frame, ProtocolMessage& out);
void protocol_reset(ProtocolDecoder& d) noexcept;
