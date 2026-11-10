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

#pragma once

// Streams every trace message to disk while armed and a measurement runs, so
// long recordings are not limited by the in-memory trace.
//
// recorder_enqueue() is called from the RX threads (register recorder_rx_consumer
// in App::rx_consumers) and only copies the message into a queue. Everything else,
// including recorder_drain() every drain_interval, runs on the main thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/bus_message.h"
#include "core/trace_file_format.h"
#include "core/trace_file_writer.h"

struct RecordingConfig
{
    std::string folder;
    std::string file_name_pattern = "trace_{date}_{time}";
    TraceFileFormat format = TraceFileFormat::VectorAsc;
    int split_size_mb = 0;    // 0 = never start a new file
    bool stay_armed = true;   // keep recording armed for the next measurement
};

struct Recorder
{
    static constexpr std::chrono::milliseconds drain_interval{250};
    static constexpr size_t max_pending = 1000000;
    // ponytail: a frame reaching the queue later than this behind a newer one of another
    // interface is still written out of order; raise it if a slow driver shows that.
    static constexpr std::chrono::milliseconds reorder_window{50};

    IfaceNameFn iface_name;   // labels candump lines and pcapng interfaces
    RecordingConfig config;   // takes effect with the next recording
    RecordingConfig active;   // snapshot of config for the running recording

    bool armed = false;
    std::atomic<bool> recording{false};

    std::mutex pending_mutex;
    std::vector<BusMessage> pending;
    uint64_t dropped = 0;

    std::ofstream file;
    std::string file_path;
    std::string last_error;
    std::chrono::local_seconds session_start{};
    std::map<uint16_t, int> channels;              // ASC channel per interface, stable across parts
    std::map<uint16_t, uint32_t> pcap_interfaces;  // IDB index per interface in the current file
    MdfLayout mdf;                                 // MF4: what finish_file patches to finalize
    int64_t file_start_ns = 0;
    int file_index = 0;
    int64_t file_bytes = 0;
    uint64_t file_frames = 0;
    int64_t total_bytes = 0;
    uint64_t frames_written = 0;
};

[[nodiscard]] constexpr bool recorder_format_supported(TraceFileFormat format) noexcept
{
    return format == TraceFileFormat::VectorAsc || format == TraceFileFormat::CanDump
        || format == TraceFileFormat::PcapNg || format == TraceFileFormat::Trc || format == TraceFileFormat::VectorMdf;
}

// File name (without folder) of part `index` (1-based) of a recording that
// started at local time `start`. Placeholders: {date}, {time}, {index}.
[[nodiscard]] std::string recorder_file_name(const RecordingConfig& config, std::chrono::local_seconds start, int index);

// Arming while `measurement_running` starts recording immediately; disarming stops it.
void recorder_set_armed(Recorder& r, bool armed, bool measurement_running);

// Called before the listeners start / after they have finished, so no frame at
// either end of a measurement is missed.
void recorder_measurement_starting(Recorder& r);
void recorder_measurement_stopped(Recorder& r);
// The measurement that recorder_measurement_starting began never started (no interface opened):
// the recording file, empty, is removed and the arming kept, as if Start had not been pressed.
void recorder_measurement_aborted(Recorder& r);

// Thread-safe.
void recorder_enqueue(Recorder& r, const BusMessage& m);
inline void recorder_rx_consumer(void* user, const BusMessage& m)
{
    recorder_enqueue(*static_cast<Recorder*>(user), m);
}

// Writes the queued frames in time order, all but the newest reorder_window (written by the
// next drain or at the end of the recording). Main thread, every Recorder::drain_interval.
void recorder_drain(Recorder& r);
