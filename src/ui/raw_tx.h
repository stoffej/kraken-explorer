#pragma once

#include <cstdint>
#include <optional>

#include "core/bus_message.h"

struct App;
struct BusMessage;
struct CanDbMessage;

// Combo of the setup's interfaces ("<network>: <interface>"), only those of `bus` when given;
// iface is an index into App::ifaces (UINT16_MAX = none). Returns true when the selection changed.
bool draw_iface_combo(const char* id, App& app, uint16_t& iface, std::optional<BusType> bus = std::nullopt);

// Main thread, once per frame: counts App::rx_scratch's received CAN frames by id width.
void rx_id_width_update(App& app);

// Whether most CAN frames received on `iface` had 29-bit ids; nullopt before the first one.
std::optional<bool> iface_rx_extended(const App& app, uint16_t iface);

// "11-bit" / "29-bit" radio pair; the width the traffic on `iface` does not use is drawn dimmed
// (still clickable). Returns true on a click, also on the already selected one.
bool draw_id_width(const App& app, uint16_t iface, bool& extended);

// The old RawTxWindow as an editor for one frame: ID, DLC, Extended/RTR/FD/BRS, interface,
// a hex grid of up to 64 bytes and, with a DBC message, a table of editable signal values.
// Edits msg in place (msg.iface is the interface); returns true when anything changed.
bool draw_raw_tx(App& app, BusMessage& msg, const CanDbMessage* db);
