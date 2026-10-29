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

#include "trace_recorder.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <utility>

#include "core/local_time.h"
#include "core/log.h"
#include "core/pcapng.h"
#include "core/text.h"
#include "core/trace_line_format.h"

namespace fs = std::filesystem;
using namespace std::chrono;

namespace
{

void replace_all(std::string& s, std::string_view from, std::string_view to)
{
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
    {
        s.replace(pos, from.size(), to);
    }
}

system_clock::time_point to_time_point(int64_t ts_ns)
{
    return system_clock::time_point(duration_cast<system_clock::duration>(nanoseconds(ts_ns)));
}

void set_io_error(Recorder& r, std::string_view what)
{
    r.last_error = std::format("cannot {} {}: {}", what, r.file_path, std::strerror(errno));
}

bool write_chunk(Recorder& r, const std::string& chunk)
{
    if (chunk.empty())
    {
        return true;
    }
    r.file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    if (!r.file)
    {
        set_io_error(r, "write");
        return false;
    }
    r.file_bytes += static_cast<int64_t>(chunk.size());
    r.total_bytes += static_cast<int64_t>(chunk.size());
    return true;
}

std::string next_file_path(const Recorder& r)
{
    const fs::path path = fs::path(r.active.folder) / recorder_file_name(r.active, r.session_start, r.file_index);
    if (!fs::exists(path))
    {
        return path.string();
    }
    // Never overwrite an earlier recording: add a counter before the extension.
    const fs::path base = path.parent_path() / path.stem();
    const std::string ext = path.extension().string();
    for (int n = 2; ; ++n)
    {
        const std::string candidate = std::format("{}_{}{}", base.string(), n, ext);
        if (!fs::exists(candidate))
        {
            return candidate;
        }
    }
}

bool open_next_file(Recorder& r)
{
    std::error_code ec;
    if (r.active.folder.empty() || (fs::create_directories(r.active.folder, ec), ec)
        || !fs::is_directory(r.active.folder))
    {
        r.last_error = std::format("cannot create folder '{}'", r.active.folder);
        return false;
    }

    ++r.file_index;
    r.file_path = next_file_path(r);
    r.file.open(r.file_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!r.file.is_open())
    {
        set_io_error(r, "open");
        return false;
    }

    r.file_bytes = 0;
    r.file_frames = 0;
    r.pcap_interfaces.clear();
    return true;
}

bool finish_file(Recorder& r)
{
    if (!r.file.is_open())
    {
        return true;
    }

    std::string tail;
    if (r.active.format == TraceFileFormat::VectorAsc)
    {
        // A file without frames still gets a header so it is a valid ASC trace.
        if (r.file_frames == 0)
        {
            append_asc_header(tail, system_clock::now());
        }
        tail += asc_footer;
        tail += '\n';
    }
    else if (r.active.format == TraceFileFormat::Trc && r.file_frames == 0)
    {
        append_trc_header(tail, duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
    }
    else if (r.active.format == TraceFileFormat::PcapNg && r.file_frames == 0)
    {
        // A pcapng file must at least contain its section header.
        std::vector<uint8_t> shb;
        pcapng_append_section_header(shb);
        tail.append(reinterpret_cast<const char*>(shb.data()), shb.size());
    }

    bool ok = write_chunk(r, tail);
    if (ok && !r.file.flush())
    {
        set_io_error(r, "write");
        ok = false;
    }
    r.file.close();
    return ok;
}

// The queued frames ordered by time. RX threads of several interfaces append interleaved and a
// little late relative to each other, so unless `all` (end of recording) the newest
// Recorder::reorder_window stays queued and is merged with what arrives until the next drain.
std::vector<BusMessage> take_pending(Recorder& r, bool all)
{
    const std::scoped_lock lock(r.pending_mutex);
    std::vector<BusMessage> batch = std::exchange(r.pending, {});
    std::ranges::stable_sort(batch, {}, &BusMessage::ts_ns);
    if (!all && !batch.empty())
    {
        const int64_t cut = batch.back().ts_ns - std::chrono::nanoseconds(Recorder::reorder_window).count();
        const auto keep = std::ranges::upper_bound(batch, cut, {}, &BusMessage::ts_ns);
        r.pending.assign(keep, batch.end());
        batch.erase(keep, batch.end());
    }
    return batch;
}

bool write_messages(Recorder& r, const std::vector<BusMessage>& messages)
{
    if (messages.empty())
    {
        return true;
    }

    const int64_t split_bytes = int64_t{r.active.split_size_mb} * 1024 * 1024;
    std::string chunk;
    std::vector<uint8_t> bin;

    for (const BusMessage& m : messages)
    {
        if (split_bytes > 0 && r.file_frames > 0 && r.file_bytes + static_cast<int64_t>(chunk.size()) >= split_bytes)
        {
            if (!write_chunk(r, chunk) || !finish_file(r) || !open_next_file(r))
            {
                return false;
            }
            chunk.clear();
            log_info(std::format("Recording continues in {}", r.file_path));
        }

        switch (r.active.format)
        {
            case TraceFileFormat::VectorAsc:
            {
                if (r.file_frames == 0)
                {
                    // Like the one-shot export, times are relative to the file's first frame.
                    r.file_start_ns = m.ts_ns;
                    append_asc_header(chunk, to_time_point(m.ts_ns));
                }
                const auto [it, inserted] = r.channels.try_emplace(m.iface, static_cast<int>(r.channels.size()) + 1);
                // Batches are sorted, but a later batch can hold a slightly older frame: clamp to 0.
                append_asc_line(chunk, m, std::min(r.file_start_ns, m.ts_ns), it->second);
                chunk += '\n';
                break;
            }

            case TraceFileFormat::PcapNg:
            {
                bin.clear();
                if (r.file_frames == 0)
                {
                    pcapng_append_section_header(bin);
                }
                // Each file is its own section: interfaces are described the first
                // time they appear in it, before their first packet.
                const auto [it, inserted] =
                    r.pcap_interfaces.try_emplace(m.iface, static_cast<uint32_t>(r.pcap_interfaces.size()));
                if (inserted)
                {
                    pcapng_append_interface(bin, r.iface_name(m.iface));
                }
                pcapng_append_packet(bin, m, it->second);
                chunk.append(reinterpret_cast<const char*>(bin.data()), bin.size());
                break;
            }

            case TraceFileFormat::Trc:
            {
                if (r.file_frames == 0)
                {
                    r.file_start_ns = m.ts_ns;
                    append_trc_header(chunk, m.ts_ns);
                }
                if (m.type != BusType::CAN)
                {
                    break;  // no LIN in TRC; leaves a gap in the message numbers
                }
                const auto [it, inserted] = r.channels.try_emplace(m.iface, static_cast<int>(r.channels.size()) + 1);
                append_trc_line(chunk, m, r.file_frames + 1, r.file_start_ns, it->second);
                chunk += "\r\n";
                break;
            }

            default:
                append_candump_line(chunk, m, r.iface_name(m.iface));
                chunk += '\n';
                break;
        }

        ++r.file_frames;
        ++r.frames_written;
    }

    if (!write_chunk(r, chunk))
    {
        return false;
    }
    // Flush every batch so a crash loses at most one drain interval.
    if (!r.file.flush())
    {
        set_io_error(r, "write");
        return false;
    }
    return true;
}

void report_failure(Recorder& r)
{
    r.recording = false;
    {
        const std::scoped_lock lock(r.pending_mutex);
        r.pending.clear();
    }
    r.file.close();

    log_error(std::format("Trace recording failed: {}", r.last_error));
    r.armed = false;
}

void start_recording(Recorder& r)
{
    if (r.recording)
    {
        return;
    }

    r.active = r.config;
    if (!recorder_format_supported(r.active.format))
    {
        r.last_error = std::format("{} cannot be recorded continuously", trace_format_name(r.active.format));
        report_failure(r);
        return;
    }

    {
        const std::scoped_lock lock(r.pending_mutex);
        r.pending.clear();
        r.dropped = 0;
    }
    r.session_start = floor<seconds>(to_local_time(system_clock::now()));
    r.channels.clear();
    r.file_index = 0;
    r.total_bytes = 0;
    r.frames_written = 0;

    if (!open_next_file(r))
    {
        report_failure(r);
        return;
    }

    r.recording = true;
    log_info(std::format("Recording trace to {}", r.file_path));
}

void stop_recording(Recorder& r)
{
    if (!r.recording.exchange(false))
    {
        return;
    }

    if (!write_messages(r, take_pending(r, true)) || !finish_file(r))
    {
        report_failure(r);
        return;
    }

    log_info(std::format("Recording stopped: {} frames ({:.1f} MiB) in {} file(s)", r.frames_written,
                         static_cast<double>(r.total_bytes) / (1024.0 * 1024.0), r.file_index));

    uint64_t dropped = 0;
    {
        const std::scoped_lock lock(r.pending_mutex);
        dropped = r.dropped;
    }
    if (dropped > 0)
    {
        log_warning(std::format("Recording could not keep up with the bus: {} frames were dropped", dropped));
    }
}

}

std::string recorder_file_name(const RecordingConfig& config, local_seconds start, int index)
{
    std::string name(trim(config.file_name_pattern));
    if (name.empty())
    {
        name = RecordingConfig{}.file_name_pattern;
    }

    // Split parts need distinct names even if the pattern does not ask for it.
    if (config.split_size_mb > 0 && name.find("{index}") == std::string::npos)
    {
        name += "_{index}";
    }

    replace_all(name, "{date}", std::format("{:%Y%m%d}", start));
    replace_all(name, "{time}", std::format("{:%H%M%S}", start));
    replace_all(name, "{index}", std::format("{:03}", index));
    replace_all(name, "/", "_");
    replace_all(name, "\\", "_");

    return std::format("{}.{}", name, trace_format_extension(config.format));
}

void recorder_set_armed(Recorder& r, bool armed, bool measurement_running)
{
    if (armed == r.armed)
    {
        return;
    }
    r.armed = armed;

    if (!armed)
    {
        stop_recording(r);
    }
    else if (measurement_running)
    {
        start_recording(r);
    }
}

void recorder_measurement_starting(Recorder& r)
{
    if (r.armed)
    {
        start_recording(r);
    }
}

void recorder_measurement_aborted(Recorder& r)
{
    if (!r.recording.exchange(false))
    {
        return;
    }
    r.file.close(); // no listener ran, nothing was received: drop the file rather than finish it
    std::error_code ec;
    fs::remove(r.file_path, ec);
    log_info(std::format("Recording discarded, the measurement did not start: {}", r.file_path));
}

void recorder_measurement_stopped(Recorder& r)
{
    stop_recording(r);
    if (r.armed && !r.config.stay_armed)
    {
        r.armed = false;
    }
}

void recorder_enqueue(Recorder& r, const BusMessage& m)
{
    if (!r.recording.load(std::memory_order_relaxed))
    {
        return;
    }

    const std::scoped_lock lock(r.pending_mutex);
    if (r.pending.size() >= Recorder::max_pending)
    {
        ++r.dropped;
        return;
    }
    r.pending.push_back(m);
}

void recorder_drain(Recorder& r)
{
    if (r.recording && !write_messages(r, take_pending(r, false)))
    {
        report_failure(r);
    }
}
