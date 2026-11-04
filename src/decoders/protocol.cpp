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

#include "decoders/decoders.h"

DecodeStatus protocol_decode(ProtocolDecoder& d, const BusMessage& frame, ProtocolMessage& out)
{
    // RX and TX traffic are decoded independently (both decoders key their
    // reassembly sessions by direction) so that a request Kraken Explorer transmits
    // itself decodes just like one it observes as RX, and so that an interface
    // which loops transmitted frames back to its own RX path (SocketCAN/vcan,
    // some real adapters) can't have a duplicate delivery of the same frame
    // corrupt the other direction's session.

    auto status = DecodeStatus::Ignored;

    // LIN diagnostic frames (ISO 17987): 0x3C = master request, 0x3D = slave response.
    // On-bus layout: [NAD, PCI, SID, data...]. Strip NAD and pass the rest
    // (PCI + payload) to the UDS decoder as an ISO 15765-2 single-frame transport PDU.
    if (frame.type == BusType::LIN)
    {
        if ((frame.id == 0x3C || frame.id == 0x3D) && frame.len >= 3)
        {
            BusMessage synthetic{.flags = static_cast<uint16_t>(frame.flags & bus_flag::tx),
                                 .iface = frame.iface, .type = BusType::LIN, .ts_ns = frame.ts_ns};
            const int n = std::min<int>(frame.len, bus_max_data_bytes) - 1;
            std::copy_n(frame.data.begin() + 1, n, synthetic.data.begin());
            set_length(synthetic, n);

            status = uds_decode(d.uds, synthetic, out);
            if (status == DecodeStatus::Completed)
            {
                out.raw_frames.assign(1, frame);
            }
        }
        return status;
    }

    if (has_flag(frame, bus_flag::extended))
    {
        // 29-bit ID: J1939 first, UDS only if J1939 ignored it
        status = j1939_decode(d.j1939, frame, out);
        if (status == DecodeStatus::Ignored)
        {
            status = uds_decode(d.uds, frame, out);
        }
    }
    else
    {
        status = uds_decode(d.uds, frame, out);
    }
    return status;
}

void protocol_reset(ProtocolDecoder& d) noexcept
{
    d = {};
}
