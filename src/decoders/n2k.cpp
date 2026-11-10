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

// NMEA 2000: PGN names, fast-packet reassembly and field decoding for a few PGNs.
//
// The fast-packet PGN list, the PGN names and the field layouts of 126992, 127250, 127251,
// 127257, 129025, 129026 and 129029 follow the NMEA2000 library by Timo Lappalainen
// (https://github.com/ttlappalainen/NMEA2000, src/NMEA2000.cpp and src/N2kMessages.cpp),
// MIT License, Copyright (c) 2015-2026 Timo Lappalainen, Kave Oy, www.kave.fi:
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify, merge,
//   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
//   to whom the Software is furnished to do so, subject to the following conditions: The above
//   copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software.
// 129027, 129028 and 129542 follow what an IMU2 sends (its DBC).

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

#include "decoders/decoders.h"

namespace
{

struct PgnInfo
{
    uint32_t pgn;
    bool fast; // sent as a fast packet
    const char* name;
};

// Sorted by PGN (binary search). ponytail: the PGNs the library knows, add more when a bus needs them.
constexpr PgnInfo pgn_table[] = {
    {126208, true, "Request/Command/Acknowledge Group Function"},
    {126464, true, "PGN List (Transmit and Receive)"},
    {126720, true, "Proprietary (fast packet)"},
    {126983, true, "Alert"},
    {126984, true, "Alert Response"},
    {126985, true, "Alert Text"},
    {126986, true, "Alert Configuration"},
    {126987, true, "Alert Threshold"},
    {126988, true, "Alert Value"},
    {126992, false, "System Date/Time"},
    {126993, false, "Heartbeat"},
    {126996, true, "Product Information"},
    {126998, true, "Configuration Information"},
    {127233, true, "Man Overboard Notification"},
    {127237, true, "Heading/Track Control"},
    {127245, false, "Rudder"},
    {127250, false, "Vessel Heading"},
    {127251, false, "Rate of Turn"},
    {127252, false, "Heave"},
    {127257, false, "Attitude"},
    {127488, false, "Engine Parameters, Rapid Update"},
    {127489, true, "Engine Parameters, Dynamic"},
    {127490, true, "Electric Drive Status (Dynamic)"},
    {127491, true, "Electric Energy Storage Status (Dynamic)"},
    {127493, false, "Transmission Parameters, Dynamic"},
    {127494, true, "Electric Drive Information"},
    {127495, true, "Electric Energy Storage Information"},
    {127496, true, "Trip Fuel Consumption, Vessel"},
    {127497, true, "Trip Fuel Consumption, Engine"},
    {127498, true, "Engine Parameters, Static"},
    {127501, false, "Binary Status Report"},
    {127503, true, "AC Input Status"},
    {127504, true, "AC Output Status"},
    {127505, false, "Fluid Level"},
    {127506, true, "DC Detailed Status"},
    {127507, true, "Charger Status"},
    {127508, false, "Battery Status"},
    {127509, true, "Inverter Status"},
    {127510, true, "Charger Configuration Status"},
    {127511, true, "Inverter Configuration Status"},
    {127512, true, "AGS Configuration Status"},
    {127513, true, "Battery Configuration Status"},
    {127514, true, "AGS Status"},
    {127750, false, "Charger Status (new)"},
    {128259, false, "Boat Speed"},
    {128267, false, "Water Depth"},
    {128275, true, "Distance Log"},
    {128520, true, "Tracked Target Data"},
    {128538, true, "Elevator Car Status"},
    {129025, false, "Position, Rapid Update"},
    {129026, false, "COG & SOG, Rapid Update"},
    {129027, false, "Position Delta, Rapid Update"},
    {129028, false, "Altitude Delta, Rapid Update"},
    {129029, true, "GNSS Position Data"},
    {129038, true, "AIS Class A Position Report"},
    {129039, true, "AIS Class B Position Report"},
    {129040, true, "AIS Class B Extended Position Report"},
    {129041, true, "AIS Aids to Navigation (AtoN) Report"},
    {129044, true, "Datum"},
    {129045, true, "User Datum Settings"},
    {129283, false, "Cross Track Error"},
    {129284, true, "Navigation Info"},
    {129285, true, "Waypoint List"},
    {129301, true, "Time to/from Mark"},
    {129302, true, "Bearing and Distance between two Marks"},
    {129538, true, "GNSS Control Status"},
    {129540, true, "GNSS Sats in View"},
    {129541, true, "GPS Almanac Data"},
    {129542, true, "GNSS Pseudorange Noise Statistics"},
    {129545, true, "GNSS RAIM Output"},
    {129547, true, "GNSS Pseudorange Error Statistics"},
    {129549, true, "DGNSS Corrections"},
    {129551, true, "GNSS Differential Correction Receiver Signal"},
    {129556, true, "GLONASS Almanac Data"},
    {129792, true, "AIS DGNSS Broadcast Binary Message"},
    {129793, true, "AIS UTC and Date Report"},
    {129794, true, "AIS Class A Static Data"},
    {129795, true, "AIS Addressed Binary Message"},
    {129796, true, "AIS Acknowledge"},
    {129797, true, "AIS Binary Broadcast Message"},
    {129798, true, "AIS SAR Aircraft Position Report"},
    {129799, true, "Radio Frequency/Mode/Power"},
    {129800, true, "AIS UTC/Date Inquiry"},
    {129801, true, "AIS Addressed Safety Related Message"},
    {129802, true, "AIS Safety Related Broadcast Message"},
    {129803, true, "AIS Interrogation"},
    {129804, true, "AIS Assignment Mode Command"},
    {129805, true, "AIS Data Link Management Message"},
    {129806, true, "AIS Channel Management"},
    {129807, true, "AIS Group Assignment"},
    {129808, true, "DSC Call Information"},
    {129809, true, "AIS Class B Static Data, Part A"},
    {129810, true, "AIS Class B Static Data, Part B"},
    {129811, true, "AIS Single Slot Binary Message"},
    {129812, true, "AIS Multi Slot Binary Message"},
    {129813, true, "AIS Long-Range Broadcast Message"},
    {129814, true, "AIS Single Slot Binary Message"},
    {129815, true, "AIS Multi Slot Binary Message"},
    {129816, true, "AIS Acknowledge"},
    {130052, true, "Loran-C TD Data"},
    {130053, true, "Loran-C Range Data"},
    {130054, true, "Loran-C Signal Data"},
    {130060, true, "Label"},
    {130061, true, "Channel Source Configuration"},
    {130064, true, "Route and WP Service - Database List"},
    {130065, true, "Route and WP Service - Route List"},
    {130066, true, "Route and WP Service - Route/WP-List Attributes"},
    {130067, true, "Route and WP Service - Route - WP Name & Position"},
    {130068, true, "Route and WP Service - Route - WP Name"},
    {130069, true, "Route and WP Service - XTE Limit & Navigation Method"},
    {130070, true, "Route and WP Service - WP Comment"},
    {130071, true, "Route and WP Service - Route Comment"},
    {130072, true, "Route and WP Service - Database Comment"},
    {130073, true, "Route and WP Service - Radius of Turn"},
    {130074, true, "Route and WP Service - WP List - WP Name & Position"},
    {130306, false, "Wind Data"},
    {130310, false, "Environmental Parameters (Outside)"},
    {130311, false, "Environmental Parameters"},
    {130312, false, "Temperature"},
    {130313, false, "Humidity"},
    {130314, false, "Actual Pressure"},
    {130316, false, "Temperature, Extended Range"},
    {130320, true, "Tide Station Data"},
    {130321, true, "Salinity Station Data"},
    {130322, true, "Current Station Data"},
    {130323, true, "Meteorological Station Data"},
    {130324, true, "Moored Buoy Station Data"},
    {130330, true, "Lighting System Settings"},
    {130561, true, "Lighting Zone"},
    {130562, true, "Lighting Scene"},
    {130563, true, "Lighting Device"},
    {130564, true, "Lighting Device Enumeration"},
    {130565, true, "Lighting Color Sequence"},
    {130566, true, "Lighting Program"},
    {130567, true, "Watermaker Input Setting and Status"},
    {130568, true, "Entertainment Diagnostic Status"},
    {130569, true, "Entertainment Current File and Status"},
    {130570, true, "Entertainment Library Data File"},
    {130571, true, "Entertainment Library Data Group"},
    {130572, true, "Entertainment Library Data Search"},
    {130573, true, "Entertainment Supported Source Data"},
    {130574, true, "Entertainment Supported Zone Data"},
    {130575, true, "Entertainment Parental Control Status"},
    {130576, false, "Small Craft Status"},
    {130577, true, "Direction Data"},
    {130578, true, "Vessel Speed Components"},
    {130580, true, "Entertainment System Configuration Status"},
    {130581, true, "Entertainment Zone Configuration Status"},
    {130583, true, "Entertainment Available Audio EQ Presets"},
    {130584, true, "Entertainment Bluetooth Devices"},
    {130586, true, "Entertainment Zone Configuration Status"},
};
static_assert(std::ranges::is_sorted(pgn_table, {}, &PgnInfo::pgn));

const PgnInfo* pgn_info(uint32_t pgn)
{
    const auto it = std::ranges::lower_bound(pgn_table, pgn, {}, &PgnInfo::pgn);
    return it != std::end(pgn_table) && it->pgn == pgn ? &*it : nullptr;
}

// --- field decoding ---

// Little-endian field reader; values with all bits set (unsigned) or the max positive
// value (signed) are "not available" in NMEA 2000.
struct Reader
{
    const std::vector<uint8_t>& p;
    std::size_t at = 0;

    [[nodiscard]] bool has(std::size_t n) const { return at + n <= p.size(); }
    uint64_t raw(std::size_t n)
    {
        uint64_t v = 0;
        for (std::size_t i = 0; i < n && at + i < p.size(); ++i)
        {
            v |= static_cast<uint64_t>(p[at + i]) << (8 * i);
        }
        at += n;
        return v;
    }
    // Unsigned n bytes times scale; NaN when not available.
    double u(std::size_t n, double scale)
    {
        const uint64_t v = raw(n);
        const uint64_t na = n >= 8 ? ~0ull : (1ull << (8 * n)) - 1;
        return v == na ? NAN : static_cast<double>(v) * scale;
    }
    // Signed n bytes times scale; NaN when not available.
    double s(std::size_t n, double scale)
    {
        const uint64_t v = raw(n);
        const int bits = static_cast<int>(8 * n);
        const uint64_t max = bits >= 64 ? 0x7FFFFFFFFFFFFFFFull : (1ull << (bits - 1)) - 1;
        if (v == max)
        {
            return NAN;
        }
        const int64_t sv = bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
        return static_cast<double>(sv) * scale;
    }
};

constexpr double rad2deg = 180.0 / std::numbers::pi;

std::string num(double v, const char* unit, int decimals = 2)
{
    return std::isnan(v) ? std::string("n/a") : std::format("{:.{}f}{}", v, decimals, unit);
}
std::string deg(double rad, int decimals = 2) { return num(rad * rad2deg, "°", decimals); }

const char* heading_ref(uint8_t r)
{
    switch (r & 0x03)
    {
    case 0: return "True";
    case 1: return "Magnetic";
    case 2: return "Error";
    default: return "n/a";
    }
}

std::string decode_fields(uint32_t pgn, const std::vector<uint8_t>& payload)
{
    Reader r{payload};
    switch (pgn)
    {
    case 126992:
    {
        if (!r.has(8)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const uint8_t source = static_cast<uint8_t>(r.raw(1) & 0x0F);
        const double days = r.u(2, 1.0);
        const double secs = r.u(4, 0.0001);
        return std::format("SID {}  source {}  days since 1970 {}  time {}", sid, source, num(days, "", 0), num(secs, " s", 1));
    }
    case 127250:
    {
        if (!r.has(8)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const double heading = r.u(2, 0.0001);
        const double deviation = r.s(2, 0.0001);
        const double variation = r.s(2, 0.0001);
        return std::format("SID {}  heading {}  deviation {}  variation {}  ref {}", sid, deg(heading), deg(deviation),
                           deg(variation), heading_ref(static_cast<uint8_t>(r.raw(1))));
    }
    case 127251:
    {
        if (!r.has(5)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        return std::format("SID {}  rate of turn {}/s", sid, deg(r.s(4, 3.125e-8), 3));
    }
    case 127257:
    {
        if (!r.has(7)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const double yaw = r.s(2, 0.0001);
        const double pitch = r.s(2, 0.0001);
        const double roll = r.s(2, 0.0001);
        return std::format("SID {}  yaw {}  pitch {}  roll {}", sid, deg(yaw), deg(pitch), deg(roll));
    }
    case 129025:
    {
        if (!r.has(8)) return "";
        const double lat = r.s(4, 1e-7);
        const double lon = r.s(4, 1e-7);
        return std::format("lat {}  lon {}", num(lat, "°", 7), num(lon, "°", 7));
    }
    case 129026:
    {
        if (!r.has(6)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const uint8_t ref = static_cast<uint8_t>(r.raw(1));
        const double cog = r.u(2, 0.0001);
        const double sog = r.u(2, 0.01);
        return std::format("SID {}  ref {}  COG {}  SOG {}", sid, heading_ref(ref), deg(cog), num(sog, " m/s"));
    }
    case 129027:
    {
        if (!r.has(8)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const double dt = r.u(1, 0.005);
        const double dlat = r.s(3, 1.0 / 3600.0 * 1e-5);
        const double dlon = r.s(3, 1.0 / 3600.0 * 1e-5);
        return std::format("SID {}  time delta {}  lat delta {}  lon delta {}", sid, num(dt, " s", 3), num(dlat, "°", 9),
                           num(dlon, "°", 9));
    }
    case 129028:
    {
        if (!r.has(8)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const double dt = r.u(1, 0.005);
        const uint8_t b = static_cast<uint8_t>(r.raw(1)); // GNSS quality bits 0..3, direction ref bits 4..5
        const double cog = r.u(2, 0.0001);
        const double dalt = r.s(3, 0.001);
        return std::format("SID {}  time delta {}  GNSS quality {}  ref {}  COG {}  altitude delta {}", sid, num(dt, " s", 3),
                           b & 0x0F, heading_ref(static_cast<uint8_t>(b >> 4)), deg(cog), num(dalt, " m", 3));
    }
    case 129029:
    {
        if (!r.has(43)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const double days = r.u(2, 1.0);
        const double secs = r.u(4, 0.0001);
        const double lat = r.s(8, 1e-16);
        const double lon = r.s(8, 1e-16);
        const double alt = r.s(8, 1e-6);
        const uint8_t type = static_cast<uint8_t>(r.raw(1));
        r.raw(1); // integrity
        const double sats = r.u(1, 1.0);
        const double hdop = r.s(2, 0.01);
        const double pdop = r.s(2, 0.01);
        return std::format("SID {}  days {}  time {}  lat {}  lon {}  alt {}  type {} method {}  sats {}  HDOP {}  PDOP {}", sid,
                           num(days, "", 0), num(secs, " s", 1), num(lat, "°", 7), num(lon, "°", 7), num(alt, " m"), type & 0x0F,
                           type >> 4, num(sats, "", 0), num(hdop, ""), num(pdop, ""));
    }
    case 129542:
    {
        if (!r.has(15)) return "";
        const uint8_t sid = static_cast<uint8_t>(r.raw(1));
        const double rms = r.u(2, 0.01);
        const double major = r.u(2, 0.01);
        const double minor = r.u(2, 0.01);
        const double orient = r.u(2, 0.0001);
        const double slat = r.u(2, 0.01);
        const double slon = r.u(2, 0.01);
        const double salt = r.u(2, 0.01);
        return std::format("SID {}  RMS {}  std major {}  minor {}  orientation {}  std lat {}  lon {}  alt {}", sid, num(rms, " m"),
                           num(major, " m"), num(minor, " m"), deg(orient), num(slat, " m"), num(slon, " m"), num(salt, " m"));
    }
    default: return "";
    }
}

void fill(ProtocolMessage& out, const BusMessage& first, uint32_t pgn)
{
    const uint32_t id = first.id;
    const auto* info = pgn_info(pgn);
    out.protocol = "N2K";
    out.id = pgn;
    out.type = MessageType::Request;
    out.ts_ns = first.ts_ns;
    out.name = info != nullptr ? info->name : std::format("PGN {}", pgn);
    out.metadata.clear();
    out.metadata["Priority"] = (id >> 26) & 0x7;
    out.metadata["Source Address"] = id & 0xFF;
    if (((id >> 16) & 0xFF) < 240)
    {
        out.metadata["Destination Address"] = (id >> 8) & 0xFF;
    }
    out.description = decode_fields(pgn, out.payload);
}

bool is_fast_packet(uint32_t pgn)
{
    if (pgn >= 130816 && pgn <= 131071) // proprietary fast packet range (126720 is in the table)
    {
        return true;
    }
    const auto* info = pgn_info(pgn);
    return info != nullptr && info->fast;
}

} // namespace

DecodeStatus n2k_decode(N2kDecoder& d, const BusMessage& frame, ProtocolMessage& out)
{
    if (is_error_frame(frame) || has_flag(frame, bus_flag::rtr) || !has_flag(frame, bus_flag::extended))
    {
        return DecodeStatus::Ignored;
    }
    const uint32_t pgn = j1939_pgn(frame.id);
    // ponytail: NMEA 2000 = the data page 1 PGNs (126208..131071); the ISO network PGNs it shares
    // with J1939 (address claim, request, ...) stay in the J1939 tab.
    if (pgn < 0x10000)
    {
        return DecodeStatus::Ignored;
    }
    const int len = std::min<int>(frame.len, bus_max_data_bytes);
    const auto& b = frame.data;

    if (!is_fast_packet(pgn))
    {
        out.payload.assign(b.begin(), b.begin() + len);
        out.raw_frames.assign(1, frame);
        fill(out, frame, pgn);
        return DecodeStatus::Completed;
    }

    if (len < 1)
    {
        return DecodeStatus::Ignored;
    }
    // Fast packet: byte 0 = sequence counter (3 bits) | frame counter (5 bits). Frame 0 carries
    // the total length in byte 1 and 6 data bytes, every next frame 7 data bytes.
    const uint64_t key = static_cast<uint64_t>(frame.iface) << 40 | static_cast<uint64_t>(has_flag(frame, bus_flag::tx)) << 32
                         | static_cast<uint64_t>(pgn) << 8 | (frame.id & 0xFF);
    auto it = d.sessions.find(key);
    if ((b[0] & 0x1F) == 0) // first frame
    {
        if (len < 2)
        {
            return DecodeStatus::Ignored;
        }
        it = d.sessions.insert_or_assign(key, N2kSession{.frames = {frame}, .data = {b.begin() + 2, b.begin() + len},
                                                         .expected_size = b[1], .last_frame = b[0]}).first;
    }
    else if (it == d.sessions.end())
    {
        return DecodeStatus::Ignored; // orphan frame
    }
    else if (static_cast<uint8_t>(it->second.last_frame + 1) != b[0])
    {
        d.sessions.erase(it); // lost a frame
        return DecodeStatus::Ignored;
    }
    else
    {
        it->second.last_frame = b[0];
        it->second.frames.push_back(frame);
        it->second.data.insert(it->second.data.end(), b.begin() + 1, b.begin() + len);
    }
    N2kSession& s = it->second;
    if (static_cast<int>(s.data.size()) < s.expected_size)
    {
        return DecodeStatus::Consumed;
    }
    s.data.resize(static_cast<std::size_t>(s.expected_size));
    out.payload = std::move(s.data);
    out.raw_frames = std::move(s.frames);
    fill(out, out.raw_frames.front(), pgn);
    d.sessions.erase(it);
    return DecodeStatus::Completed;
}
