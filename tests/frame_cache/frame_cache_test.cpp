#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_env.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "app.h"
#include "core/trace_file_writer.h"
#include "ui/frame_cache.h"
#include "ui/trace_window.h"

namespace
{

std::filesystem::path scratch(const char* name)
{
    return std::filesystem::temp_directory_path() / std::format("kraken_fc_{}_{}", getpid(), name);
}

void write(const std::filesystem::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

// A /proc/self/status field in MB: VmHWM = peak RSS (mapped file pages included, the OS can
// drop those), RssAnon = heap now (what really costs RAM).
long status_mb(std::string_view field)
{
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);)
    {
        if (line.starts_with(field))
        {
            return std::strtol(line.c_str() + field.size(), nullptr, 10) / 1024;
        }
    }
    return -1;
}

std::string mem() { return std::format("peak RSS {} MB, heap {} MB", status_mb("VmHWM:"), status_mb("RssAnon:")); }

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("frame cache matches replay_parse, sorted, with a per-id index")
{
    const std::string log = "(1.000000) vcan0 111#01\n"
                            "(3.000000) vcan1 222#02\n"
                            "(2.000000) vcan0 111#03\n" // out of order
                            "(2.500000) vcan0 444##1000102030405060708090A0B0C0D0E0F10111213\n" // CAN FD, 20 bytes: overflow
                            "garbage\n"
                            "(4.000000) vcan0 333#04\n"
                            "(5.000000) vcan0 111#05\n";
    const auto src = scratch("a.log");
    const auto cache = scratch("a.kfc");
    write(src, log);
    {
        auto built = frame_cache_build(src, cache, TraceFileFormat::CanDump);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    auto c = frame_cache_open(src, cache);
    REQUIRE(c.has_value());

    const ReplayFile ref = replay_parse(log, TraceFileFormat::CanDump);
    REQUIRE(c->recs.size() == ref.frames.size());
    CHECK(c->channels == ref.channels);
    for (std::size_t i = 0; i + 1 < c->recs.size(); ++i)
    {
        CHECK(c->recs[i].ts_ns <= c->recs[i + 1].ts_ns);
    }
    CHECK(c->recs[1].data[0] == 3);
    // The FD frame's payload is in the overflow area, its record keeps the index; decoded whole.
    REQUIRE(c->overflow.size() == 1);
    CHECK(c->recs[2].len == 20);
    const BusMessage fd = frame_cache_frame(*c, 2);
    CHECK(fd.id == 0x444);
    CHECK(fd.len == 20);
    CHECK(has_flag(fd, bus_flag::fd));
    CHECK(fd.data[0] == 0x00);
    CHECK(fd.data[19] == 0x13);
    CHECK(fd.data[20] == 0);
    CHECK(frame_cache_frame(*c, 1).data[0] == 3);
    CHECK(frame_cache_frame(*c, 1).data[1] == 0);

    // Index: every frame exactly once, in its row, in time order.
    REQUIRE(c->rows.size() == 4);
    CHECK(c->rows[0].id == 0x111);
    CHECK(c->rows[0].count == 3);
    // Cycle stats: 0x111 at 1, 2, 5 s -> cycles 1 s and 3 s; a lone frame has none.
    CHECK(c->rows[0].cycle_min_ns == 1'000'000'000);
    CHECK(c->rows[0].cycle_max_ns == 3'000'000'000);
    CHECK(c->rows[0].cycle_sum_ns / static_cast<int64_t>(c->rows[0].count - 1) == 2'000'000'000);
    CHECK(c->rows[1].count == 1);
    CHECK(c->rows[1].cycle_sum_ns == 0);
    std::size_t total = 0;
    for (const FrameCacheRow& r : c->rows)
    {
        for (uint64_t k = 0; k < r.count; ++k)
        {
            const BusMessage m = frame_cache_frame(*c, c->row_frames[r.first + k]);
            CHECK(m.id == r.id);
            CHECK(m.iface == r.channel);
            if (k > 0)
            {
                CHECK(c->row_frames[r.first + k - 1] < c->row_frames[r.first + k]);
            }
        }
        total += r.count;
    }
    CHECK(total == c->recs.size());

    frame_cache_close(*c);

    // Touched (new mtime), same content: the cache stays, and the next open needs no hash.
    std::filesystem::last_write_time(src, std::filesystem::last_write_time(src) + std::chrono::seconds(5));
    auto touched = frame_cache_open(src, cache);
    REQUIRE(touched.has_value());
    frame_cache_close(*touched);
    // Same size, other content: stale. Other size: stale.
    std::string edited = log;
    edited[edited.find("111#05")] = '2';
    write(src, edited);
    CHECK(frame_cache_open(src, cache).error() == "Stale cache");
    write(src, log + "(6.000000) vcan0 111#06\n");
    CHECK(frame_cache_open(src, cache).error() == "Stale cache");
    // Another format version: rebuilt, but not reported as a changed file.
    {
        std::fstream f(cache, std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t old_version = frame_cache_version - 1;
        f.seekp(offsetof(FrameCacheHeader, version));
        f.write(reinterpret_cast<const char*>(&old_version), sizeof old_version);
    }
    CHECK(frame_cache_open(src, cache).error() == "Old cache");

    std::filesystem::remove(src);
    std::filesystem::remove(cache);
}

TEST_CASE("frame cache splits big text files at line ends")
{
    // > 64 MB so the block loop runs more than once; every frame must survive the cut.
    const auto src = scratch("big.log");
    const auto cache = scratch("big.kfc");
    std::size_t n = 0;
    {
        std::ofstream out(src, std::ios::binary);
        std::string line;
        for (std::size_t bytes = 0; bytes < (std::size_t{66} << 20); ++n)
        {
            line = std::format("({}.{:06}) vcan0 {:03X}#0011223344556677\n", 1000 + n / 1000, n % 1000 * 1000, n % 0x7FF);
            out << line;
            bytes += line.size();
        }
    }
    {
        auto built = frame_cache_build(src, cache, TraceFileFormat::CanDump);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    auto c = frame_cache_open(src, cache);
    REQUIRE(c.has_value());
    CHECK(c->recs.size() == n);
    CHECK(c->recs.back().id == (n - 1) % 0x7FF);
    CHECK(c->overflow.empty());
    // The index and cycle stats, built in parallel slices, against one pass over the records.
    std::map<uint32_t, std::vector<uint32_t>> want;
    for (uint32_t i = 0; i < c->recs.size(); ++i)
    {
        want[c->recs[i].id].push_back(i);
    }
    REQUIRE(c->rows.size() == want.size());
    for (const FrameCacheRow& r : c->rows)
    {
        const std::vector<uint32_t>& list = want[r.id];
        REQUIRE(std::ranges::equal(c->row_frames.subspan(r.first, r.count), list));
        int64_t lo = INT64_MAX, hi = 0, sum = 0;
        for (std::size_t k = 1; k < list.size(); ++k)
        {
            const int64_t cycle = c->recs[list[k]].ts_ns - c->recs[list[k - 1]].ts_ns;
            lo = std::min(lo, cycle);
            hi = std::max(hi, cycle);
            sum += cycle;
        }
        CHECK(r.cycle_min_ns == lo);
        CHECK(r.cycle_max_ns == hi);
        CHECK(r.cycle_sum_ns == sum);
    }
    // Classic CAN: 32 bytes per record and 4 per index entry, plus the header, rows and names.
    const uint64_t size = std::filesystem::file_size(cache);
    CHECK(size >= 36 * n);
    CHECK(size <= 4096 + 36 * n + 0x7FF * sizeof(FrameCacheRow) + 64);
    frame_cache_close(*c);
    std::filesystem::remove(src);
    std::filesystem::remove(cache);
}

TEST_CASE("file view: the trace shows the cache, a filter merges the per-id lists")
{
    const std::string log = "(1.0) vcan0 111#01\n(2.0) vcan0 222#02\n(3.0) vcan0 111#03\n(4.0) vcan0 333#04\n(5.0) vcan0 222#05\n";
    const auto src = scratch("view.log");
    const auto cache = scratch("view.kfc");
    write(src, log);
    {
        auto built = frame_cache_build(src, cache, TraceFileFormat::CanDump);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    auto c = frame_cache_open(src, cache);
    REQUIRE(c.has_value());
    App app;
    app.trace_file = std::make_shared<FrameCache>(*c); // closed by hand below
    trace_open_file(app.trace, app.trace_file->recs, app.trace_file->overflow);
    CHECK(trace_size(app.trace) == 5);
    CHECK(trace_at(app.trace, app.trace.begin + 3).id == 0x333);

    TraceWindowState s;
    trace_window_update(s, app);
    REQUIRE(s.agg.size() == 3); // one Monitor row per id, from the index
    CHECK(s.agg[0].cycle.count == 1); // 0x111 at 1 and 3 s
    CHECK(s.agg[0].cycle.min_ns == 2'000'000'000);
    CHECK(cycle_stats_median(s.agg[0].cycle) == 2e9);
    CHECK(s.agg[1].cycle.count == 1); // 0x222 at 2 and 5 s
    CHECK(s.agg[1].cycle.min_ns == 3'000'000'000);
    CHECK(s.agg[2].cycle.count == 0); // 0x333 once
    // The merge runs on a worker; the update after it is done takes the list (the app's frame loop).
    const auto filter = [&](const char* text)
    {
        s.filter.text = text;
        s.filter_dirty = true;
        trace_window_update(s, app);
        for (int i = 0; i < 5000 && s.file_filter_result.valid(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            trace_window_update(s, app);
        }
        CHECK_FALSE(s.file_filter_result.valid());
    };
    filter("0x222");
    CHECK(s.file_filtered == std::vector<uint32_t>{1, 4});
    filter("0x");
    CHECK(s.file_filtered == std::vector<uint32_t>{0, 1, 2, 3, 4}); // merged back into time order
    s.filter.text = "0x222";
    s.filter_dirty = true;
    trace_window_update(s, app); // a merge starts...
    filter("0x333");             // ...and a newer filter replaces it, finished or not
    CHECK(s.file_filtered == std::vector<uint32_t>{3});

    const BusMessage live = frame_cache_frame(*c, 0);
    trace_append(app.trace, std::span(&live, 1)); // live frames end the file view
    CHECK(app.trace.file.empty());
    CHECK(trace_size(app.trace) == 1);
    frame_cache_close(*c);
    std::filesystem::remove(src);
    std::filesystem::remove(cache);
}

namespace
{

// Every frame of the cache equals the parse of the whole file, in order.
void check_same_as_parse(const std::filesystem::path& src, const std::string& bytes, TraceFileFormat format)
{
    const auto cache = scratch("blocks.kfc");
    write(src, bytes);
    {
        auto built = frame_cache_build(src, cache, format);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    auto c = frame_cache_open(src, cache);
    REQUIRE(c.has_value());
    const ReplayFile ref = replay_parse(bytes, format);
    REQUIRE(c->recs.size() == ref.frames.size());
    CHECK(c->channels == ref.channels);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < ref.frames.size(); ++i)
    {
        const BusMessage a = frame_cache_frame(*c, i);
        const BusMessage& b = ref.frames[i];
        mismatches += a.ts_ns != b.ts_ns || a.id != b.id || a.flags != b.flags || a.len != b.len || a.iface != b.iface
                      || a.errors != b.errors || !std::equal(a.data.begin(), a.data.begin() + a.len, b.data.begin());
    }
    CHECK(mismatches == 0);
    frame_cache_close(*c);
    std::filesystem::remove(src);
    std::filesystem::remove(cache);
}

void le32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        s += static_cast<char>(v >> (8 * i));
    }
}

} // namespace

TEST_CASE("frame cache splits a pcap between records")
{
    // > 32 MB of records of two sizes (classic and FD), so three blocks and cuts that do not fall
    // on a record boundary by chance; the global header (byte order, time unit) is passed to every block.
    std::string d;
    le32(d, 0xA1B23C4D); // nanosecond magic
    le32(d, 4 << 16 | 2);
    le32(d, 0);
    le32(d, 0);
    le32(d, 65535);
    le32(d, 227); // LINKTYPE_CAN_SOCKETCAN
    std::size_t n = 0;
    for (; d.size() < (std::size_t{34} << 20); ++n)
    {
        const bool fd = n % 3 == 0;
        le32(d, static_cast<uint32_t>(1000 + n / 1000));
        le32(d, static_cast<uint32_t>(n % 1000 * 1000));
        le32(d, fd ? 72 : 16);
        le32(d, fd ? 72 : 16);
        const uint32_t id = static_cast<uint32_t>(n % 0x7FF);
        d += static_cast<char>(id >> 24);
        d += static_cast<char>(id >> 16);
        d += static_cast<char>(id >> 8);
        d += static_cast<char>(id);
        d += static_cast<char>(fd ? 64 : 8); // len
        d.append(3, '\0');
        for (int i = 0; i < (fd ? 64 : 8); ++i)
        {
            d += static_cast<char>(n + static_cast<std::size_t>(i));
        }
    }
    check_same_as_parse(scratch("blocks.pcap"), d, TraceFileFormat::Pcap);
}

TEST_CASE("frame cache splits a BLF between top-level objects")
{
    // > 32 MB from our writer: 128 KB stored containers whose object stream runs across container
    // ends, so a cut inside a container would lose frames; the "LOGG" header (start time) is passed
    // to every block.
    std::vector<BusMessage> frames;
    for (std::size_t n = 0; n < 750'000; ++n)
    {
        BusMessage m;
        m.ts_ns = 1'700'000'000'000'000'000 + static_cast<int64_t>(n) * 100'000;
        m.id = static_cast<uint32_t>(n % 0x7FF);
        m.iface = static_cast<uint16_t>(n % 2);
        m.flags = n % 5 == 0 ? bus_flag::tx : 0;
        set_length(m, 8);
        for (std::size_t i = 0; i < 8; ++i)
        {
            m.data[i] = static_cast<uint8_t>(n + i);
        }
        frames.push_back(m);
    }
    std::ostringstream out;
    write_trace_file(out, TraceFileFormat::Blf, frames, [](uint16_t) { return std::string(); });
    REQUIRE(out.str().size() > (std::size_t{32} << 20));
    check_same_as_parse(scratch("blocks.blf"), out.str(), TraceFileFormat::Blf);
}

TEST_CASE("frame cache prune drops the least recently opened .kfc files, keeps the newest build")
{
    const auto dir = scratch("cache_home");
    std::filesystem::create_directories(dir);
    test_setenv("XDG_CACHE_HOME", dir);
    const auto src_old = scratch("old.log");
    const auto src_new = scratch("new.log");
    write(src_old, "(1.0) vcan0 111#01\n");
    write(src_new, "(1.0) vcan0 222#02\n");
    const auto old_cache = frame_cache_path(src_old);
    const auto new_cache = frame_cache_path(src_new);
    {
        auto built = frame_cache_build(src_old, old_cache, TraceFileFormat::CanDump);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    std::filesystem::last_write_time(old_cache, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));
    {
        auto built = frame_cache_build(src_new, new_cache, TraceFileFormat::CanDump);
        REQUIRE(built.has_value());
        frame_cache_close(*built);
        frame_cache_wait_saved(); // tests reopen it from disk
    }
    const auto other = old_cache.parent_path() / "notes.txt";
    const auto stale_tmp = old_cache.parent_path() / "x.kfc.1.tmp";
    write(other, "x");
    write(stale_tmp, "x");
    std::filesystem::last_write_time(stale_tmp, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));
    CHECK(std::filesystem::exists(old_cache));

    frame_cache_prune(old_cache.parent_path(), 1, new_cache);
    CHECK(!std::filesystem::exists(old_cache));
    CHECK(std::filesystem::exists(new_cache));
    CHECK(std::filesystem::exists(other));
    CHECK(!std::filesystem::exists(stale_tmp));
    std::filesystem::remove_all(dir);
    std::filesystem::remove(src_old);
    std::filesystem::remove(src_new);
}

// Not a test: KRAKEN_BENCH_FILE=big.log build/tests/frame_cache/frame_cache_test -tc="*bench*"
// KRAKEN_BENCH_BASELINE=1 instead measures the old path (whole file in RAM + replay_parse).
TEST_CASE("frame cache bench")
{
    const char* file = std::getenv("KRAKEN_BENCH_FILE");
    if (file == nullptr)
    {
        return;
    }
    const std::filesystem::path src = file;
    const auto format = trace_format_from_path(src.string()).value_or(TraceFileFormat::VectorAsc);
    const double mb = static_cast<double>(std::filesystem::file_size(src)) / (1 << 20);
    auto t0 = std::chrono::steady_clock::now();

    if (std::getenv("KRAKEN_BENCH_BASELINE") != nullptr)
    {
        std::ifstream in(src, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), {});
        const double read_s = seconds_since(t0);
        const ReplayFile f = replay_parse(data, format);
        const double parse_s = seconds_since(t0) - read_s;
        const auto rows = replay_id_rows(f);
        std::printf("baseline %.0f MB: read %.2f s, parse %.2f s, id rows %.2f s, total %.2f s, %zu frames, %s\n",
                    mb, read_s, parse_s, seconds_since(t0) - read_s - parse_s, seconds_since(t0), f.frames.size(), mem().c_str());
        return;
    }

    const auto cache = frame_cache_path(src);
    if (!frame_cache_open(src, cache).has_value())
    {
        auto built = frame_cache_build(src, cache, format);
        REQUIRE(built.has_value());
        const double build_s = seconds_since(t0);
        frame_cache_close(*built);
        frame_cache_wait_saved();
        std::printf("build %.0f MB: %.2f s (%.0f MB/s), saved after %.2f s, cache %.0f MB, %s\n", mb, build_s, mb / build_s,
                    seconds_since(t0), static_cast<double>(std::filesystem::file_size(cache)) / (1 << 20), mem().c_str());
    }
    t0 = std::chrono::steady_clock::now();
    auto c = frame_cache_open(src, cache);
    REQUIRE(c.has_value());
    std::printf("open: %.3f ms, %zu frames, %zu ids\n", seconds_since(t0) * 1e3, c->recs.size(), c->rows.size());

    // 1000 random jumps: seek to a time, read a page of 50 rows and the previous frame of each one's id.
    std::mt19937_64 rng(1);
    const int64_t t_first = c->recs.front().ts_ns;
    const int64_t t_span = c->recs.back().ts_ns - t_first;
    uint64_t sink = 0;
    t0 = std::chrono::steady_clock::now();
    for (int j = 0; j < 1000; ++j)
    {
        const int64_t ts = t_first + static_cast<int64_t>(rng() % static_cast<uint64_t>(t_span + 1));
        const auto top = static_cast<std::size_t>(std::ranges::lower_bound(c->recs, ts, {}, &FrameCacheRec::ts_ns) - c->recs.begin());
        for (std::size_t i = top; i < std::min(top + 50, c->recs.size()); ++i)
        {
            const BusMessage m = frame_cache_frame(*c, i);
            const auto row = std::ranges::lower_bound(c->rows, std::pair{m.iface, m.id}, {},
                                                      [](const FrameCacheRow& r) { return std::pair{r.channel, r.id}; });
            const auto list = c->row_frames.subspan(row->first, row->count);
            sink += static_cast<uint64_t>(std::ranges::lower_bound(list, static_cast<uint32_t>(i)) - list.begin()) + m.data[0];
        }
    }
    const double jumps_s = seconds_since(t0);
    std::printf("jump + 50 rows with Δt: %.1f us per jump (sink %llu)\n", jumps_s * 1e3, static_cast<unsigned long long>(sink % 10));

    // Graph window: decode one id over the whole file (worst case zoomed out).
    t0 = std::chrono::steady_clock::now();
    const FrameCacheRow& r = c->rows.front();
    double acc = 0.0;
    for (uint64_t k = 0; k < r.count; ++k)
    {
        acc += static_cast<double>(extract_raw_signal(frame_cache_frame(*c, c->row_frames[r.first + k]), 0, 16, false));
    }
    std::printf("decode id 0x%X over the whole file: %llu frames in %.1f ms (acc %.0f), %s\n", r.id,
                static_cast<unsigned long long>(r.count), seconds_since(t0) * 1e3, acc / static_cast<double>(r.count), mem().c_str());
    frame_cache_close(*c);
}
