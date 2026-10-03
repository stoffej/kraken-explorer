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

// Watch window: a list of chosen DBC signals with their last value, min / max and count.
// Plain data plus free functions. No <imgui.h> here: app.h includes this header.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/signal_search.h"

struct App;
struct CanDbMessage;
struct CanDbSignal;
struct Setup;
struct Trace;
struct WorkspaceTab;
namespace pugi
{
class xml_node;
}

struct WatchItem
{
    // Binding: network + DBC raw id + signal name, re-resolved when Setup::generation changes.
    std::string network;
    uint32_t raw_id = 0;
    std::string signal;
    const CanDbMessage* msg = nullptr;
    const CanDbSignal* sig = nullptr;
    // Runtime, not saved.
    bool has_value = false;
    double value = 0.0;   // last physical value
    uint64_t raw = 0;     // its raw value (value-table lookup)
    bool changed = false; // the last reception's value differs from the one before
    double min = 0.0;     // of the values seen since the trace was cleared or Reset
    double max = 0.0;
    uint64_t count = 0;   // receptions
};

struct WatchWindow
{
    bool open = false;
    std::vector<WatchItem> items;
    std::string search;          // signal picker filter
    SignalSearch picker;         // fuzzy ranking of `search`
    uint64_t next_index = 0;     // next trace index to decode
    uint64_t trace_clears = 0;
    uint64_t setup_generation = UINT64_MAX; // forces a resolve on the first ingest
};

// Appends the signal unless the list has it already (false).
bool watch_add(WatchWindow& w, const Setup& setup, const SignalEntry& entry);
// Forgets values, min / max and counts.
void watch_reset(WatchWindow& w);
// Decodes the trace frames appended since the last call into the items' values, as
// instrument_panel_ingest: O(new frames x items), a file view is not live traffic.
void watch_ingest(WatchWindow& w, const Setup& setup, const Trace& trace);

// The "Watch" window of `tab` while w.open.
void draw_watch_window(App& app, const WorkspaceTab& tab, WatchWindow& w);

void watch_save_xml(const WatchWindow& w, pugi::xml_node el);
void watch_load_xml(WatchWindow& w, pugi::xml_node el);
