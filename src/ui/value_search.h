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


// Value Search: fuzzy-pick a signal, give a value range, list the stretches of the trace where
// the signal stays inside it. A click puts the Log and the Graph on that stretch together.

#pragma once

#include <cstdint>
#include <thread>
#include <span>
#include <future>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include "ui/signal_search.h"
#include "ui/vim_nav.h"

struct App;
struct BusMessage;
struct CanDbMessage;
struct CanDbSignal;
struct FrameCache;
struct WorkspaceTab;

// One matching sample: the frame (trace index), its time and the value.
struct ValueHit
{
    uint64_t index = 0;
    int64_t ts_ns = 0;
    double value = 0.0;
};

// What one scan found: every sample inside the range (one hit each, capped at max_hits), and the
// lowest / highest value seen at all (the signal's real range in this trace, offered as the bounds).
struct ValueResult
{
    std::vector<ValueHit> hits;
    bool truncated = false; // more matches than max_hits
    double seen_min = 0.0;
    double seen_max = 0.0;
    uint64_t samples = 0;   // every decoded sample, in range or not
};

// A raw match: the id (optional) and the data bytes, each either a value or any ("??").
struct RawPattern
{
    std::optional<uint32_t> id;
    std::vector<std::optional<uint8_t>> bytes; // byte i must equal bytes[i] when set
};

// "123" / "1ABCDEF0" (hex) and "01 ?? FF" / "01??FF" (hex pairs, ?? or XX = any); nullopt on bad text.
[[nodiscard]] std::optional<RawPattern> raw_pattern_parse(std::string_view id_text, std::string_view data_text);
[[nodiscard]] bool raw_pattern_matches(const RawPattern& p, const BusMessage& m) noexcept;

struct ValueSearch
{
    bool raw = false;           // Raw: id / data bytes instead of a signal value
    std::string raw_id;         // hex id, empty = any
    std::string raw_data;       // data pattern, empty = any
    std::string query;          // fuzzy signal search
    SignalSearch finder;
    std::optional<SignalEntry> signal; // the picked signal; dropped when the setup changes
    uint64_t setup_generation = 0;
    bool dock_checked = false; // the one-time move into the Log / Python Script tabs
    std::string from;           // value bounds, empty = open
    std::string to;
    std::vector<ValueHit> hits; // the table: every matching sample, one row each
    bool truncated = false;
    std::string status;
    int selected = -1;
    VimNav vim;                 // right-click on a hit: the copy menu (vim_yank_menu) at the mouse
    std::vector<VimYankItem> yank_items;
    // A scan of a file view runs on the worker (a 20 M-frame signal takes ~1 s): progress in
    // `scanned` of `to_scan`, the result through `job`; a newer scan or a window close stops it.
    // range_only: the scan after a pick, only its seen_min / seen_max are wanted (the bounds).
    std::atomic<uint64_t> scanned{0};
    uint64_t to_scan = 0;
    bool range_only = false;
    std::future<ValueResult> job;
    std::jthread worker; // last: stopped and joined before the members it uses go
};

inline constexpr std::size_t value_search_max_hits = 100000;

// Every sample whose physical value is in [lo, hi], in time order, over the frames
// `list` (indices into c) -- the frame cache path, safe to run on a worker: stop ends it early
// (partial result), progress counts the frames looked at.
[[nodiscard]] ValueResult value_search_scan(const FrameCache& c, std::span<const uint32_t> list, const CanDbMessage& msg,
                                            const CanDbSignal& sig, double lo, double hi,
                                            std::size_t max_hits = value_search_max_hits, std::stop_token stop = {},
                                            std::atomic<uint64_t>* progress = nullptr);

// The same over the app's trace, on the main thread: the cache index in a file view, else every
// live frame. CAN signals.
[[nodiscard]] ValueResult value_search_run(const App& app, const CanDbMessage& msg, const CanDbSignal& sig, double lo,
                                           double hi, std::size_t max_hits = value_search_max_hits);

// Every frame matching `pattern` (id and data bytes), in time order, over the frames `list` of c,
// or over all of c when `list` is empty and `all` is set. Worker-safe like value_search_scan.
[[nodiscard]] ValueResult raw_search_scan(const FrameCache& c, std::span<const uint32_t> list, bool all, const RawPattern& pattern,
                                          std::size_t max_hits = value_search_max_hits, std::stop_token stop = {},
                                          std::atomic<uint64_t>* progress = nullptr);
// The same over the app's trace, on the main thread.
[[nodiscard]] ValueResult raw_search_run(const App& app, const RawPattern& pattern, std::size_t max_hits = value_search_max_hits);
// Starts a raw scan of v.raw_id / v.raw_data (worker in a file view, at once otherwise).
void raw_search_start(App& app, ValueSearch& v);

// Starts a scan of v.signal for [lo, hi]: on the worker in a file view (the result lands in
// value_search_poll), at once otherwise. range_only keeps only the seen range as v.from / v.to.
void value_search_start(App& app, ValueSearch& v, double lo, double hi, bool range_only);
// Takes a finished worker scan into v (hits, status or bounds). True when something landed.
bool value_search_poll(ValueSearch& v);

// The tab's "Value Search" window (a tab next to Python Script).
void draw_value_search(App& app, ValueSearch& v, WorkspaceTab& tab);

// A click on hit row `hit`: selects it, shows it in the Log, and centres the graph that plots the
// signal on it; when none does, the tab's first graph (a new one when it has none) gets the signal.
void value_search_hit_clicked(App& app, ValueSearch& v, WorkspaceTab& tab, int hit);
