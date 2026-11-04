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

// The "UDS Diagnostics" window: pick an ECU (learned from the traffic, found by a functional scan
// or a physical sweep, or typed in), then on the Services tab send sessions, tester present, resets,
// DIDs, DTCs, routines, I/O control, memory access, security access or a raw request; the Scan tab
// finds ECUs and probes their services and DIDs; the Transport tab sets CAN FD, addressing and
// timing. Requests run on the window's worker thread; the log below is a copyable text box. One
// state for every tab, like the Python Script window.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "decoders/uds_client.h"

struct App;
struct WorkspaceTab;

struct UdsWindow
{
    bool open = false;
    UdsLearner learner;          // main thread: ECUs learned from the traffic, scans and sweeps
    int ecu = -1;                // learner.ecus index; -1: the manual ids below
    uint16_t iface = 0;          // manual target's interface (App::ifaces index)
    std::string tx_text = "7E0"; // manual request id, hex
    std::string rx_text = "7E8"; // manual response id, hex
    bool extended = false;       // manual target uses 29-bit ids
    int tab = 0;                 // Services / Scan / Transport

    // Services tab fields.
    std::string did = "F190";
    std::string did_data;
    std::string routine = "FF00";
    std::string routine_data;
    std::string mem_addr = "00000000";
    std::string mem_size = "0004";
    std::string mem_data;
    std::string io_did = "0000";
    std::string io_data;
    int reset_kind = 0;          // 0 hard, 1 keyOffOn, 2 soft
    int dtc_report = 1;          // index into the DTC report combo
    std::string dtc_mask = "FF";
    std::string dtc_code = "000000";
    std::map<uint32_t, std::string> dtc_names; // 3-byte trouble code -> text, loaded from a file
    std::string dtc_file;        // path of the loaded DTC text file (for the status line)
    std::string sec_level = "01";
    std::string sec_key;
    std::string raw = "22 F1 90";

    // Scan tab fields.
    std::string sweep_first = "700";
    std::string sweep_last = "7FF";
    std::string sweep_tester = "F1";
    std::string sweep_wait = "40";
    bool sweep_j1939 = true;     // 29-bit sweep uses the J1939 source addresses seen on the bus
    std::string did_first = "F180";
    std::string did_last = "F1FF";

    // Transport tab fields (applied to every request).
    bool fd = false;
    bool brs = false;
    int tx_len_idx = 0;          // index into {8,12,16,20,24,32,48,64}
    std::string padding = "CC";
    std::string tx_ae;           // empty = none
    std::string rx_ae;
    std::string p2 = "1000";
    std::string p2_star = "5000";
    std::string n_bs = "1000";
    std::string n_cr = "1000";

    std::string log_view;        // copy of log for the read-only text box
    uint64_t log_seen = 0;
    bool log_follow = false;     // new lines last frame: scroll to the bottom
    bool log_active = false;     // the user is selecting in the log

    // Shared with the worker, guarded by mutex.
    std::mutex mutex;
    std::condition_variable_any cv; // waits on the worker's stop token too
    std::deque<std::function<void()>> jobs;
    std::string log;
    uint64_t log_total = 0;
    std::vector<UdsEcu> found;   // scan / sweep results for the main thread to merge
    UdsTarget target;            // current target, for tester present
    bool tester_present = false; // 3E 80 every 2 s to target
    bool busy = false;           // a job is running
    std::atomic<bool> cancel{false}; // set by Stop and shutdown: a sweep / scan ends early
    std::chrono::steady_clock::time_point log_start{}; // the log's first line
    std::jthread worker;         // last: stops and joins before the rest goes
};

// Main thread, every frame: learns ECUs from the batch of new frames.
void uds_window_on_rx(UdsWindow& w, std::span<const BusMessage> batch);

// Sends a frame on app.ifaces[m.iface]; for UdsClient requests from any thread.
bool uds_send(App& app, const BusMessage& m);

void draw_uds_window(App& app, UdsWindow& w, const WorkspaceTab& tab);

// Ends the running request and joins the worker: before the interfaces close (main.cpp).
void uds_window_shutdown(App& app, UdsWindow& w);
