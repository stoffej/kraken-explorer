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

// Recorder: arming, file naming, splitting and the structure of every part.
//
// Structure checks follow the formats themselves (Vector ASC header and footer,
// candump -L line shape, pcapng block framing with interfaces described before
// use) and the input frames. Line and block encoding is covered separately by
// trace_line_format and pcapng_format; tests/format_compat reads recorder output
// back with python-can and scapy.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/bus_message.h"
#include "core/trace_file_writer.h"
#include "core/trace_recorder.h"

namespace fs = std::filesystem;

namespace
{

constexpr int64_t base_ns = 1757000000000000000LL;

std::string iface_name(uint16_t id)
{
    return std::format("can{}", id);
}

BusMessage bulk_frame(int i)
{
    BusMessage m{
        .id = static_cast<uint32_t>(0x100 + i % 0x600),
        .iface = static_cast<uint16_t>(i % 2),
        .ts_ns = base_ns + i * 1000000LL,
    };
    set_length(m, 8);
    for (uint8_t k = 0; k < 8; ++k)
    {
        m.data[k] = static_cast<uint8_t>(i + k);
    }
    return m;
}

// Removed again when the test ends.
struct TempDir
{
    fs::path path;
    TempDir()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / std::format("kraken_rec_{:08x}{:08x}", rd(), rd());
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void configure(Recorder& r, const fs::path& folder, TraceFileFormat format, int split_mb = 0)
{
    r.iface_name = iface_name;
    r.config = RecordingConfig{
        .folder = folder.string(),
        .file_name_pattern = "rec",
        .format = format,
        .split_size_mb = split_mb,
        .stay_armed = true,
    };
}

std::vector<fs::path> files_in(const fs::path& folder)
{
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(folder))
    {
        if (e.is_regular_file())
        {
            files.push_back(e.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::string read_all(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::vector<std::string> lines_of(const std::string& content)
{
    std::vector<std::string> lines;
    std::istringstream in(content);
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty())
        {
            lines.push_back(line);
        }
    }
    return lines;
}

std::string trimmed(const std::string& s)
{
    const auto a = s.find_first_not_of(' ');
    return a == std::string::npos ? std::string{} : s.substr(a, s.find_last_not_of(' ') - a + 1);
}

uint32_t le32(const std::string& b, size_t off)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(b[off])) | static_cast<uint32_t>(static_cast<uint8_t>(b[off + 1])) << 8
         | static_cast<uint32_t>(static_cast<uint8_t>(b[off + 2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(b[off + 3])) << 24;
}

// Frame count of a Vector ASC file, or -1 if header or footer are broken.
int asc_frames(const std::string& content)
{
    const auto lines = lines_of(content);
    if (lines.size() < 7 || !lines[0].starts_with("date ") || lines[1] != "base hex  timestamps absolute"
        || !lines[4].starts_with("Begin Triggerblock ") || trimmed(lines[5]) != "0.000000 Start of measurement"
        || lines.back() != "End TriggerBlock")
    {
        return -1;
    }
    return static_cast<int>(lines.size()) - 7;
}

// Frame count of a candump -L file, or -1 if any line has the wrong shape.
int candump_frames(const std::string& content)
{
    static const std::regex shape(R"(^\(\d+\.\d{6}\) can[01] [0-9A-F]{3}#[0-9A-F]{16}$)");
    const auto lines = lines_of(content);
    for (const auto& line : lines)
    {
        if (!std::regex_match(line, shape))
        {
            return -1;
        }
    }
    return static_cast<int>(lines.size());
}

// Enhanced packet count of a pcapng file, or -1 for broken block framing, a
// missing section header, or a packet whose interface was not described first.
int pcapng_frames(const std::string& content)
{
    if (content.size() < 12 || le32(content, 0) != 0x0A0D0D0A)
    {
        return -1;
    }
    size_t pos = 0;
    uint32_t interfaces = 0;
    int packets = 0;
    while (pos < content.size())
    {
        if (pos + 12 > content.size())
        {
            return -1;
        }
        const uint32_t type = le32(content, pos);
        const uint32_t length = le32(content, pos + 4);
        if (length < 12 || length % 4 != 0 || pos + length > content.size() || le32(content, pos + length - 4) != length)
        {
            return -1;
        }
        if (type == 1)
        {
            ++interfaces;
        }
        else if (type == 6)
        {
            if (le32(content, pos + 8) >= interfaces)
            {
                return -1;
            }
            ++packets;
        }
        pos += length;
    }
    return packets;
}

// Records of our MF4 layout; -1 unless finalized (a clean stop finalizes) and the DT block, the
// file's last, holds exactly its stated records.
int mf4_frames(const std::string& content)
{
    const std::size_t dt = content.rfind("##DT");
    if (!content.starts_with("MDF     ") || dt == std::string::npos || dt + 24 > content.size())
    {
        return -1;
    }
    const uint64_t length = le32(content, dt + 8) | uint64_t{le32(content, dt + 12)} << 32;
    return length == content.size() - dt && (length - 24) % mdf_record_size == 0
        ? static_cast<int>((length - 24) / mdf_record_size) : -1;
}

int frames_in(TraceFileFormat format, const std::string& content)
{
    switch (format)
    {
        case TraceFileFormat::VectorAsc: return asc_frames(content);
        case TraceFileFormat::PcapNg:    return pcapng_frames(content);
        case TraceFileFormat::VectorMdf: return mf4_frames(content);
        default:                         return candump_frames(content);
    }
}

constexpr TraceFileFormat recordable[] = {TraceFileFormat::VectorAsc, TraceFileFormat::CanDump, TraceFileFormat::PcapNg,
                                          TraceFileFormat::VectorMdf};

}

TEST_CASE("recorder_file_name")
{
    struct Row
    {
        const char* name;
        const char* pattern;
        TraceFileFormat format;
        int split_mb;
        int index;
        const char* expected;
    };
    constexpr Row rows[] = {
        {"date and time", "trace_{date}_{time}", TraceFileFormat::VectorAsc, 0, 1, "trace_20260913_140509.asc"},
        {"split adds index", "trace_{date}_{time}", TraceFileFormat::VectorAsc, 100, 2, "trace_20260913_140509_002.asc"},
        {"explicit index", "run_{index}", TraceFileFormat::CanDump, 100, 12, "run_012.candump"},
        {"path separators", "a/b\\c", TraceFileFormat::PcapNg, 0, 1, "a_b_c.pcapng"},
        {"empty pattern", "", TraceFileFormat::VectorAsc, 0, 1, "trace_20260913_140509.asc"},
    };

    using namespace std::chrono;
    const local_seconds start = local_days{2026y / 9 / 13} + 14h + 5min + 9s;
    for (const Row& row : rows)
    {
        CAPTURE(row.name);
        const RecordingConfig cfg{.file_name_pattern = row.pattern, .format = row.format, .split_size_mb = row.split_mb};
        CHECK(recorder_file_name(cfg, start, row.index) == row.expected);
    }
}

TEST_CASE("records only while measuring")
{
    TempDir dir;
    Recorder r;
    configure(r, dir.path, TraceFileFormat::CanDump);

    recorder_set_armed(r, true, false);
    CHECK(r.armed);
    CHECK(!r.recording);
    for (int i = 0; i < 5; ++i)
    {
        recorder_enqueue(r, bulk_frame(i));   // armed, but no measurement yet
    }

    recorder_measurement_starting(r);
    CHECK(r.recording);
    for (int i = 5; i < 8; ++i)
    {
        recorder_rx_consumer(&r, bulk_frame(i));
    }
    recorder_measurement_stopped(r);
    CHECK(!r.recording);

    // Frames after stopping are ignored as well.
    recorder_enqueue(r, bulk_frame(8));

    CHECK(r.frames_written == 3);
    const auto files = files_in(dir.path);
    REQUIRE(files.size() == 1);
    const std::string content = read_all(files[0]);
    CHECK(candump_frames(content) == 3);
    CHECK(content.starts_with("(1757000000.005000) can1 105#"));
}

TEST_CASE("arming during a measurement starts immediately")
{
    TempDir dir;
    Recorder r;
    configure(r, dir.path, TraceFileFormat::CanDump);

    recorder_set_armed(r, true, true);
    CHECK(r.recording);
    recorder_enqueue(r, bulk_frame(0));
    recorder_drain(r);

    // Disarming stops and finishes the file.
    recorder_set_armed(r, false, true);
    CHECK(!r.recording);
    CHECK(r.frames_written == 1);
    const auto files = files_in(dir.path);
    REQUIRE(files.size() == 1);
    CHECK(candump_frames(read_all(files[0])) == 1);
}

TEST_CASE("empty recording is valid")
{
    for (const TraceFileFormat fmt : recordable)
    {
        CAPTURE(trace_format_name(fmt));
        TempDir dir;
        Recorder r;
        configure(r, dir.path, fmt);
        recorder_set_armed(r, true, false);
        recorder_measurement_starting(r);
        recorder_measurement_stopped(r);

        const auto files = files_in(dir.path);
        REQUIRE(files.size() == 1);
        CHECK(frames_in(fmt, read_all(files[0])) == 0);
    }
}

TEST_CASE("splits into valid parts")
{
    constexpr int frame_count = 30000;
    constexpr int64_t split_bytes = 1024 * 1024;

    for (const TraceFileFormat fmt : recordable)
    {
        CAPTURE(trace_format_name(fmt));
        TempDir dir;
        Recorder r;
        configure(r, dir.path, fmt, 1);
        recorder_set_armed(r, true, false);
        recorder_measurement_starting(r);
        for (int i = 0; i < frame_count; ++i)
        {
            recorder_enqueue(r, bulk_frame(i));
            if (i % 5000 == 4999)
            {
                recorder_drain(r);   // split across drain batches as well as within one
            }
        }
        recorder_measurement_stopped(r);
        CHECK(r.frames_written == frame_count);

        const auto files = files_in(dir.path);
        REQUIRE(files.size() >= 2);
        CHECK(files[0].stem() == "rec_001");

        int total = 0;
        for (const auto& path : files)
        {
            CAPTURE(path.string());
            const std::string content = read_all(path);
            // A part may overshoot the limit by the one record that crossed it plus the footer.
            CHECK(static_cast<int64_t>(content.size()) <= split_bytes + 256);

            const int frames = frames_in(fmt, content);
            CHECK(frames > 0);
            total += frames;

            if (fmt == TraceFileFormat::VectorAsc)
            {
                // Every part is a complete trace whose times start at its own first frame.
                CHECK(trimmed(lines_of(content).at(6)).starts_with("0.000000 "));
            }
        }
        CHECK(total == frame_count);
    }
}

TEST_CASE("never overwrites")
{
    TempDir dir;
    std::ofstream(dir.path / "rec.candump", std::ios::binary) << "keep\n";

    Recorder r;
    configure(r, dir.path, TraceFileFormat::CanDump);
    recorder_set_armed(r, true, false);
    recorder_measurement_starting(r);
    recorder_enqueue(r, bulk_frame(0));
    recorder_measurement_stopped(r);

    CHECK(read_all(dir.path / "rec.candump") == "keep\n");
    CHECK(candump_frames(read_all(dir.path / "rec_2.candump")) == 1);
}

TEST_CASE("stay armed")
{
    TempDir dir;
    Recorder r;
    configure(r, dir.path, TraceFileFormat::CanDump);

    recorder_set_armed(r, true, false);
    recorder_measurement_starting(r);
    recorder_measurement_stopped(r);
    CHECK(r.armed);

    recorder_measurement_starting(r);
    CHECK(r.recording);
    r.config.stay_armed = false;
    recorder_measurement_stopped(r);
    CHECK(!r.armed);

    // Each measurement produced its own file.
    CHECK(files_in(dir.path).size() == 2);
}

TEST_CASE("MF4 recording is unfinalized while it runs, finalized at the end")
{
    TempDir dir;
    Recorder r;
    configure(r, dir.path, TraceFileFormat::VectorMdf);
    recorder_set_armed(r, true, false);
    recorder_measurement_starting(r);
    for (int i = 0; i < 100; ++i)
    {
        recorder_enqueue(r, bulk_frame(i));
    }
    recorder_drain(r);

    // What a crash now would leave: "UnFinMF ", the DT length open, the records so far after it.
    const auto files = files_in(dir.path);
    REQUIRE(files.size() == 1);
    std::string open = read_all(files[0]);
    CHECK(open.starts_with("UnFinMF "));
    CHECK(le32(open, 60) % 0x10000 == mdf_unfin_flags);
    const std::size_t dt = open.rfind("##DT");
    REQUIRE(dt != std::string::npos);
    CHECK(le32(open, dt + 8) == 24);
    CHECK((open.size() - dt - 24) % mdf_record_size == 0);
    CHECK(open.size() - dt - 24 > 0);

    recorder_measurement_stopped(r);
    CHECK(mf4_frames(read_all(files[0])) == 100);
}

TEST_CASE("unusable folder disarms")
{
    TempDir dir;
    std::ofstream(dir.path / "not_a_folder") << "";

    Recorder r;
    configure(r, dir.path / "not_a_folder" / "sub", TraceFileFormat::CanDump);
    recorder_set_armed(r, true, false);
    recorder_measurement_starting(r);

    CHECK(!r.recording);
    CHECK(!r.armed);
}

TEST_CASE("unsupported format disarms")
{
    TempDir dir;
    Recorder r;
    configure(r, dir.path, TraceFileFormat::Blf);
    recorder_set_armed(r, true, false);
    recorder_measurement_starting(r);

    CHECK(!r.recording);
    CHECK(!r.armed);
    CHECK(files_in(dir.path).empty());
}

// RX threads of several interfaces queue interleaved and a little late relative to each other
// (up to 1.66 ms seen, T87b a1 F6): a later drain batch can hold an older frame. The drain keeps
// the newest Recorder::reorder_window queued, so the file stays in time order.
TEST_CASE("ASC recording of frames queued out of order across drains is time-ordered")
{
    TempDir dir;
    Recorder r;
    configure(r, dir.path, TraceFileFormat::VectorAsc);
    recorder_set_armed(r, true, true);
    for (const int i : {3, 1, 2})
    {
        recorder_enqueue(r, bulk_frame(i));
    }
    recorder_drain(r);                    // all within 50 ms of the newest: nothing written yet
    recorder_enqueue(r, bulk_frame(0));   // 1 ms older than everything queued so far
    recorder_enqueue(r, bulk_frame(100));
    recorder_drain(r);                    // 0..3 ms are older than 100 - 50 ms: written, sorted
    CHECK(r.frames_written == 4);
    recorder_set_armed(r, false, true);   // the end of the recording flushes the rest

    const auto files = files_in(dir.path);
    REQUIRE(files.size() == 1);
    const auto lines = lines_of(read_all(files[0]));
    REQUIRE(asc_frames(read_all(files[0])) == 5);
    std::vector<std::string> times;
    for (std::size_t i = 6; i < lines.size() - 1; ++i)
    {
        times.push_back(trimmed(lines[i]).substr(0, trimmed(lines[i]).find(' ')));
    }
    CHECK(times == std::vector<std::string>{"0.000000", "0.001000", "0.002000", "0.003000", "0.100000"});
}

TEST_CASE("an aborted start (no interface opened) leaves no file and keeps the arming")
{
    for (const TraceFileFormat fmt : recordable)
    {
        CAPTURE(trace_format_name(fmt));
        TempDir dir;
        Recorder r;
        configure(r, dir.path, fmt);
        r.config.stay_armed = false; // a stop would disarm: an abort must not
        recorder_set_armed(r, true, false);
        recorder_measurement_starting(r);
        REQUIRE(r.recording);
        REQUIRE(files_in(dir.path).size() == 1);
        recorder_measurement_aborted(r);
        CHECK_FALSE(r.recording);
        CHECK(r.armed);
        CHECK(files_in(dir.path).empty());

        // The next real start records as usual.
        recorder_measurement_starting(r);
        recorder_rx_consumer(&r, bulk_frame(0));
        recorder_measurement_stopped(r);
        REQUIRE(files_in(dir.path).size() == 1);
        CHECK(frames_in(fmt, read_all(files_in(dir.path)[0])) == 1);
    }
}
