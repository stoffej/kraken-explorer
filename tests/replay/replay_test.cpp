// ui/replay: trace file parsing (vectors written by hand from the candump, Vector ASC,
// pcap and pcapng formats, not from Kraken Explorer's writers), the filter/plan, and playback
// on vcan0 with the original timing (skipped when vcan0 is not up).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_env.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app.h"
#include "core/trace_file_writer.h"
#include "ui/frame_cache.h"
#include "ui/replay.h"
#include "ui/workspace_tabs.h"
#include "ui_test.h"

#ifdef __linux__
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif
#include <unistd.h>

extern const DriverOps socketcan_driver;

namespace
{

void le32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        s += static_cast<char>(v >> (8 * i));
    }
}

void le16(std::string& s, uint16_t v)
{
    s += static_cast<char>(v);
    s += static_cast<char>(v >> 8);
}

// struct can_frame with can_id in network byte order (LINKTYPE_CAN_SOCKETCAN).
std::string socketcan_frame(uint32_t can_id, std::vector<uint8_t> data)
{
    std::string s;
    for (int i = 3; i >= 0; --i)
    {
        s += static_cast<char>(can_id >> (8 * i));
    }
    s += static_cast<char>(data.size());
    s.append(3, '\0');
    data.resize(8);
    s.append(data.begin(), data.end());
    return s;
}

// Unique across processes: getpid() alone collides between PID-namespaced sandboxes that
// share /tmp (every run is pid 2 or 3 there), and one run then truncates the other's file.
std::filesystem::path temp_trace(std::string_view stem)
{
    return std::filesystem::temp_directory_path()
        / std::format("{}_{}_{:08x}{:08x}.candump", stem, getpid(), std::random_device{}(), std::random_device{}());
}

// Loads `text` as a candump through the frame cache on the loader thread, as the Replay View
// does (the player walks the cache's records). The mapping is the loader's default: trace only
// unless app.ifaces has the channel.
void load_candump(App& app, Replay& r, const std::string& text, std::string_view stem)
{
    test_setenv("XDG_CACHE_HOME", std::filesystem::temp_directory_path());
    const auto path = temp_trace(stem);
    std::ofstream(path, std::ios::binary) << text;
    replay_load(app, r, path.string());
    while (!replay_load_poll(app, r))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(r.data.cache != nullptr);
    std::filesystem::remove(path);
}

} // namespace

TEST_CASE("candump -L lines")
{
    const ReplayFile f = replay_parse("(1700000000.000000) vcan0 123#DEADBEEF\n"
                                      "(1700000000.010000) vcan0 00000456#00\n"
                                      "(1700000000.020000) vcan1 321##1AABB\n"
                                      "(1700000000.030000) vcan0 7FF#R\n"
                                      "(1700000000.040000) vcan0 20000004#0000000000000000\n"
                                      "garbage\n",
                                      TraceFileFormat::CanDump);
    REQUIRE(f.frames.size() == 5);
    REQUIRE(f.channels == std::vector<std::string>{"vcan0", "vcan1"});
    CHECK(f.frames[0].id == 0x123);
    CHECK(f.frames[0].ts_ns == 1700000000000000000);
    CHECK(f.frames[0].len == 4);
    CHECK(f.frames[0].data[3] == 0xEF);
    CHECK(!has_flag(f.frames[0], bus_flag::extended));
    CHECK(has_flag(f.frames[1], bus_flag::extended)); // 8 hex digits
    CHECK(f.frames[1].ts_ns - f.frames[0].ts_ns == 10000000);
    CHECK(f.frames[2].iface == 1);
    CHECK(has_flag(f.frames[2], bus_flag::fd));
    CHECK(has_flag(f.frames[2], bus_flag::brs));
    CHECK(f.frames[2].len == 2);
    CHECK(f.frames[2].data[1] == 0xBB);
    CHECK(has_flag(f.frames[3], bus_flag::rtr));
    CHECK(is_error_frame(f.frames[4]));
}

TEST_CASE("Vector ASC events")
{
    const ReplayFile f = replay_parse("date Mon Sep 25 10:00:00.000 am 2026\n"
                                      "base hex  timestamps absolute\n"
                                      "Begin TriggerBlock Mon Sep 25 10:00:00.000 am 2026\n"
                                      "   0.000000 1  123             Rx   d 2 11 22  Length = 0 BitCount = 0 ID = 291\n"
                                      "   0.500000 2  1ABCDEFx        Tx   d 1 FF\n"
                                      "   1.000000 1  ErrorFrame\n"
                                      "   1.500000 CANFD   1 Rx        456 1 0 9 12 00 01 02 03 04 05 06 07 08 09 0A 0B\n"
                                      "   2.000000 1  LIN 3c Rx d 2 01 02 checksum = 4a\n"
                                      "End TriggerBlock\n",
                                      TraceFileFormat::VectorAsc);
    REQUIRE(f.frames.size() == 5);
    CHECK(f.channels == std::vector<std::string>{"CH 1", "CH 2"});
    CHECK(f.frames[0].id == 0x123);
    CHECK(f.frames[0].data[1] == 0x22);
    CHECK(f.frames[1].id == 0x1ABCDEF);
    CHECK(has_flag(f.frames[1], bus_flag::extended));
    CHECK(has_flag(f.frames[1], bus_flag::tx));
    CHECK(f.frames[1].iface == 1);
    CHECK(f.frames[1].ts_ns - f.frames[0].ts_ns == 500000000);
    // Frame times count from the header's date (local time).
    using namespace std::chrono;
    const auto date = current_zone()->to_sys(local_days{2026y / September / 25} + 10h);
    CHECK(f.frames[0].ts_ns == duration_cast<nanoseconds>(date.time_since_epoch()).count());
    CHECK(is_error_frame(f.frames[2]));
    CHECK(has_flag(f.frames[3], bus_flag::fd));
    CHECK(f.frames[3].len == 12);
    CHECK(f.frames[3].data[11] == 0x0B);
    CHECK(f.frames[3].iface == 0);
    CHECK(f.frames[4].type == BusType::LIN);
    CHECK(f.frames[4].id == 0x3C);
    CHECK(f.frames[4].len == 2);
}

TEST_CASE("PEAK TRC 2.1")
{
    // Values as python-can's can.TRCReader reads these lines (STARTTIME 45000.5 = 1678881600 s).
    const ReplayFile f = replay_parse(";$FILEVERSION=2.1\r\n"
                                      ";$STARTTIME=45000.5\r\n"
                                      ";$COLUMNS=N,O,T,B,I,d,R,L,D\r\n"
                                      ";   comment\r\n"
                                      "      1      1059.900 DT 1     0300 Rx -  7    00 00 00 00 04 00 00\r\n"
                                      "      2      1753.227 FB 2     0400 Tx -  9    01 02 03 04 05 06 07 08 09 0A 0B 0C\r\n"
                                      "      3      1900.000 RR 2     0123 Rx -  2\r\n"
                                      "      4      2000.000 ER 1     -    Rx -  5    04 00 08 00 00\r\n"
                                      "      5      2100.000 ST 1     -    Rx -  4    00 00 00 04\r\n",
                                      TraceFileFormat::Trc);
    REQUIRE(f.frames.size() == 4);
    CHECK(f.channels == std::vector<std::string>{"CH 1", "CH 2"});
    CHECK(f.frames[0].ts_ns == 1678881601059900000);
    CHECK(f.frames[0].id == 0x300);
    CHECK(f.frames[0].len == 7);
    CHECK(f.frames[0].data[4] == 0x04);
    CHECK(f.frames[1].iface == 1);
    CHECK(has_flag(f.frames[1], bus_flag::fd));
    CHECK(has_flag(f.frames[1], bus_flag::brs));
    CHECK(has_flag(f.frames[1], bus_flag::tx));
    CHECK(f.frames[1].len == 12);
    CHECK(f.frames[1].data[11] == 0x0C);
    CHECK(has_flag(f.frames[2], bus_flag::rtr));
    CHECK(f.frames[2].len == 2);
    CHECK(is_error_frame(f.frames[3]));
    CHECK(f.frames[3].iface == 0);
}

TEST_CASE("pcap, LINKTYPE_CAN_SOCKETCAN")
{
    std::string d;
    le32(d, 0xA1B2C3D4);
    le16(d, 2);
    le16(d, 4);
    le32(d, 0);
    le32(d, 0);
    le32(d, 65535);
    le32(d, 227);
    const std::string frame = socketcan_frame(0x80001234, {1, 2, 3});
    le32(d, 10);
    le32(d, 500);
    le32(d, 16);
    le32(d, 16);
    d += frame;
    const ReplayFile f = replay_parse(d, TraceFileFormat::Pcap);
    REQUIRE(f.frames.size() == 1);
    CHECK(f.frames[0].id == 0x1234);
    CHECK(has_flag(f.frames[0], bus_flag::extended));
    CHECK(f.frames[0].len == 3);
    CHECK(f.frames[0].data[2] == 3);
    CHECK(f.frames[0].ts_ns == 10000500000);
}

TEST_CASE("pcapng with if_name")
{
    std::string d;
    // SHB
    le32(d, 0x0A0D0D0A);
    le32(d, 28);
    le32(d, 0x1A2B3C4D);
    le16(d, 1);
    le16(d, 0);
    le32(d, 0xFFFFFFFF);
    le32(d, 0xFFFFFFFF);
    le32(d, 28);
    // IDB: if_name "vcan0" (padded to 8) + opt_endofopt
    le32(d, 1);
    le32(d, 36);
    le16(d, 227);
    le16(d, 0);
    le32(d, 0);
    le16(d, 2);
    le16(d, 5);
    d += std::string("vcan0\0\0\0", 8);
    le32(d, 0);
    le32(d, 36);
    // EPB, microseconds
    le32(d, 6);
    le32(d, 48);
    le32(d, 0);
    le32(d, 0);
    le32(d, 2000000);
    le32(d, 16);
    le32(d, 16);
    d += socketcan_frame(0x7FF, {0xAA});
    le32(d, 48);
    const ReplayFile f = replay_parse(d, TraceFileFormat::PcapNg);
    REQUIRE(f.frames.size() == 1);
    CHECK(f.channels == std::vector<std::string>{"vcan0"});
    CHECK(f.frames[0].id == 0x7FF);
    CHECK(f.frames[0].data[0] == 0xAA);
    CHECK(f.frames[0].ts_ns == 2000000000);
}

TEST_CASE("filter rows and plan")
{
    const ReplayFile f = replay_parse("(10.000000) a 100#01\n"
                                      "(10.100000) a 200#02\n"
                                      "(10.200000) b 100#03\n"
                                      "(10.300000) a 100#04\n"
                                      "(10.400000) a 20000004#0000000000000000\n",
                                      TraceFileFormat::CanDump);
    std::vector<ReplayIdRow> rows = replay_id_rows(f);
    REQUIRE(rows.size() == 4); // a:100, a:200, a:error, b:100
    CHECK(rows[0].count == 2);
    CHECK(rows[2].id == replay_error_id);
    CHECK(rows[3].channel == 1);
    rows[1].rx_on = false; // a:200 off

    std::vector<ReplayStep> plan;
    for (const BusMessage& m : f.frames)
    {
        if (ReplayStep step; replay_step(m, f.frames.front().ts_ns, rows, {3, replay_trace_only}, step))
        {
            plan.push_back(step);
        }
    }
    REQUIRE(plan.size() == 4);
    CHECK(plan[0].at_ns == 0);
    CHECK(plan[0].target == 3);
    CHECK(plan[1].at_ns == 200000000);
    CHECK(plan[1].target == replay_trace_only); // channel b unmapped
    CHECK(plan[2].msg.data[0] == 4);
    CHECK(plan[3].target == replay_trace_only); // error frames are never sent
}

#ifdef __linux__ // SocketCAN
TEST_CASE("replay a candump file onto vcan0 with its timing")
{
    if (if_nametoindex("vcan0") == 0)
    {
        MESSAGE("vcan0 not up, skipped");
        return;
    }
    std::deque<Iface> ifaces;
    auto& i = ifaces.emplace_back();
    i.ops = &socketcan_driver;
    i.info.name = "vcan0";
    i.info.details = "vcan"; // as enumerate: never `ip link set` (pkexec) the shared vcan
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "SocketCAN", .name = "vcan0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);

    const int peer = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    REQUIRE(peer >= 0);
    timeval tv{.tv_sec = 1, .tv_usec = 0};
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex("vcan0"));
    REQUIRE(bind(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // vcan0 is shared with other runs: tag the frames with our pid.
    const auto pid = static_cast<uint32_t>(getpid());
    const std::string tag = std::format("{:08X}", pid);
    const std::string text = std::format("(100.000000) vcan0 5A1#{0}01\n"
                                         "(100.050000) vcan0 5A1#{0}02\n"
                                         "(100.150000) vcan0 5A1#{0}03\n",
                                         tag);
    App app;
    Replay r;
    load_candump(app, r, text, "replay_vcan");
    r.mapping = {0};
    Tasks tasks;
    const auto start = std::chrono::steady_clock::now();
    replay_start(r, ifaces, tasks);

    std::vector<std::pair<uint8_t, std::chrono::steady_clock::duration>> got;
    while (got.size() < 3)
    {
        can_frame fr{};
        if (read(peer, &fr, sizeof(fr)) != CAN_MTU)
        {
            break;
        }
        if (fr.can_id == 0x5A1 && fr.len == 5 && std::format("{:02X}{:02X}{:02X}{:02X}", fr.data[0], fr.data[1], fr.data[2], fr.data[3]) == tag)
        {
            got.emplace_back(fr.data[4], std::chrono::steady_clock::now() - start);
        }
    }
    close(peer);
    REQUIRE(got.size() == 3);
    CHECK(got[0].first == 1);
    CHECK(got[2].first == 3);
    // 150 ms file time; allow scheduling slack under the sanitizers.
    CHECK(got[2].second - got[0].second >= std::chrono::milliseconds(140));
    CHECK(got[2].second - got[0].second < std::chrono::milliseconds(400));
    for (int t = 0; t < 100 && r.running; ++t)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!r.running);
    CHECK(r.position == 3);
    CHECK(tasks.queue.empty()); // everything went out on the interface
    replay_stop(r);
    ifaces_stop(ifaces);
}
#endif

TEST_CASE("stop interrupts a long wait at once")
{
    App app;
    Replay r;
    load_candump(app, r, "(0.0) x 1#01\n(60.0) x 1#02\n", "replay_wait");
    r.mapping = {replay_trace_only};
    std::deque<Iface> ifaces;
    Tasks tasks;
    replay_start(r, ifaces, tasks);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t = std::chrono::steady_clock::now();
    replay_stop(r);
    CHECK(std::chrono::steady_clock::now() - t < std::chrono::milliseconds(500));
    CHECK(r.position == 1);
    CHECK(tasks.queue.size() == 1); // trace-only frame posted to the main thread
}

TEST_CASE("as fast as possible skips the file's timing and batches trace-only frames")
{
    App app;
    Replay r;
    load_candump(app, r, "(0.0) x 1#01\n(60.0) x 1#02\n(3600.0) x 1#03\n", "replay_fast");
    r.mapping = {replay_trace_only};
    r.fast = true;
    std::deque<Iface> ifaces;
    Tasks tasks;
    replay_start(r, ifaces, tasks);
    for (int t = 0; t < 100 && r.running; ++t)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!r.running); // an hour of file time, done at once
    CHECK(r.position == 3);
    CHECK(tasks.queue.size() == 1); // one batch, not one task per frame
    replay_stop(r);
    tasks_drain(tasks, app); // the live frames replace the file view
    REQUIRE(trace_size(app.trace) == 3);
    CHECK(trace_at(app.trace, app.trace.begin + 2).ts_ns == 3'600'000'000'000); // the file's time, not the wall clock
}

namespace
{

// The player is at `position`, paused there (or finished when `running` is false).
bool wait_for_player(const Replay& r, std::size_t position, bool running = true)
{
    for (int t = 0; t < 500; ++t)
    {
        if (r.position == position && (running ? r.hold == replay_paused : !r.running))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("a breakpoint on an id pauses before its frames; step and continue go on from there")
{
    App app;
    Replay r;
    load_candump(app, r, "(0.0) x 1#01\n(0.1) x 2#02\n(0.2) x 1#03\n(0.3) x 2#04\n", "replay_break");
    r.mapping = {replay_trace_only};
    r.fast = true;
    REQUIRE(r.data.rows.size() == 2);
    r.data.rows[1].brk = true; // id 2
    std::deque<Iface> ifaces;
    Tasks tasks;
    replay_start(r, ifaces, tasks);
    REQUIRE(wait_for_player(r, 1)); // 1#01 played, held before 2#02
    CHECK(r.running);
    replay_single_step(r);
    REQUIRE(wait_for_player(r, 2)); // the breakpoint's own frame, nothing more
    replay_resume(r);
    REQUIRE(wait_for_player(r, 3)); // 1#03, then the next frame of id 2
    replay_resume(r);
    REQUIRE(wait_for_player(r, 4, false));
    replay_stop(r);
    tasks_drain(tasks, app);
    CHECK(trace_size(app.trace) == 4);
}

TEST_CASE("a breakpoint by time, a step from stopped, and stop while paused")
{
    App app;
    Replay r;
    load_candump(app, r, "(5.0) x 1#01\n(5.1) x 1#02\n(5.2) x 1#03\n(65.0) x 1#04\n", "replay_break_at");
    r.mapping = {replay_trace_only};
    r.break_at = "junk, 0.15, 1:00";
    std::deque<Iface> ifaces;
    Tasks tasks;
    replay_start(r, ifaces, tasks);
    REQUIRE(r.play_breaks == std::vector<std::size_t>{2, 3}); // the first frame at or after each time
    REQUIRE(wait_for_player(r, 2)); // with the file's timing: 0.1 s in
    replay_pause(r); // already paused: no effect
    replay_resume(r);
    REQUIRE(wait_for_player(r, 3)); // held at 1:00 without waiting for it: a breakpoint is checked before the wait
    const auto t = std::chrono::steady_clock::now();
    replay_stop(r);
    CHECK(std::chrono::steady_clock::now() - t < std::chrono::milliseconds(500));
    CHECK(!r.running);

    r.break_at.clear();
    replay_start(r, ifaces, tasks, replay_stepping);
    REQUIRE(wait_for_player(r, 1));
    replay_single_step(r);
    REQUIRE(wait_for_player(r, 2)); // at once, not after the file's 0.1 s
    replay_stop(r);
}

namespace
{

// 300k candump lines on two channels, in a temp file; returns the text too.
std::string write_big_candump(const std::filesystem::path& path)
{
    std::string text;
    for (int i = 0; i < 300000; ++i)
    {
        text += std::format("({}.{:06}) {} {:03X}#{:02X}{:02X}0102\n", 1000 + i / 1000, (i % 1000) * 1000,
                            i % 3 == 0 ? "vcanX" : "vcanY", 0x100 + i % 50, i & 0xFF, (i >> 8) & 0xFF);
    }
    std::ofstream(path, std::ios::binary) << text;
    return text;
}

bool same_frame(const BusMessage& a, const BusMessage& b)
{
    return a.id == b.id && a.flags == b.flags && a.errors == b.errors && a.iface == b.iface && a.len == b.len
        && a.dlc == b.dlc && a.type == b.type && a.ts_ns == b.ts_ns && a.data == b.data;
}

} // namespace

TEST_CASE("replay_load parses a big file on the loader thread")
{
    test_setenv("XDG_CACHE_HOME", std::filesystem::temp_directory_path()); // frame caches go there, not to ~/.cache
    const auto path = temp_trace("replay_big");
    const std::string text = write_big_candump(path);
    const ReplayFile sync = replay_parse(text, TraceFileFormat::CanDump);
    REQUIRE(sync.frames.size() == 300000);

    App app;
    Replay r;
    r.open = true;
    const auto t0 = std::chrono::steady_clock::now();
    replay_load(app, r, path.string());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)); // returns at once
    CHECK(r.loader.joinable());
    CHECK(replay_frames(r.data).empty());
    float last = 0.0f;
    bool monotonic = true;
    while (!replay_load_poll(app, r) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
    {
        const float f = r.load_fraction;
        monotonic = monotonic && f >= last;
        last = f;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(monotonic);
    CHECK(r.load_fraction == 1.0f);
    CHECK(!r.loader.joinable());
    CHECK(r.data.path == path.string());
    REQUIRE(replay_frames(r.data).size() == sync.frames.size());
    CHECK(r.data.channels == sync.channels);
    bool same = true;
    for (std::size_t i = 0; i < sync.frames.size(); ++i)
    {
        same = same && same_frame(frame_cache_frame(*r.data.cache, i), sync.frames[i]);
    }
    CHECK(same);
    CHECK(r.data.rows.size() == 100);
    CHECK(r.mapping == std::vector<int>{replay_trace_only, replay_trace_only});
    CHECK(r.data.channel_lin == std::vector<char>{0, 0});
    CHECK(r.data.info.find("300 000 messages") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("replay_load: cancel, restart and destroy mid-load leave no threads")
{
    test_setenv("XDG_CACHE_HOME", std::filesystem::temp_directory_path());
    const auto path = temp_trace("replay_cancel");
    write_big_candump(path);
    App app;
    {
        Replay r;
        replay_load(app, r, path.string());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto t = std::chrono::steady_clock::now();
        replay_load_cancel(r);
        CHECK(std::chrono::steady_clock::now() - t < std::chrono::milliseconds(500));
        CHECK(!r.loader.joinable());
        CHECK(!replay_load_poll(app, r));
        CHECK(replay_frames(r.data).empty());

        // A new load cancels the running one; only the second result arrives.
        replay_load(app, r, "/nonexistent/replay.candump");
        replay_load(app, r, path.string());
        while (!replay_load_poll(app, r))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(replay_frames(r.data).size() == 300000);

        replay_load(app, r, path.string()); // destroyed while loading: the jthread joins
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Replay missing;
    replay_load(app, missing, "/nonexistent/replay.candump");
    while (!replay_load_poll(app, missing))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(missing.data.info == "Error: Cannot open file.");
    CHECK(missing.data.path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("closing the Replay window stops the playback (T87b a3 F4)")
{
    App app;
    app.workspace.tabs.push_back({.uid = 7});
    Replay r;
    load_candump(app, r, "(0.0) x 1#01\n(60.0) x 1#02\n", "replay_close");
    r.mapping = {replay_trace_only};
    replay_start(r, app.ifaces, app.tasks);
    REQUIRE(r.running);
    r.open = false; // the window's X
    draw_replay(app, app.workspace.tabs[0], r);
    CHECK_FALSE(r.running);
    CHECK_FALSE(r.player.joinable());
}

namespace
{

void replay_frame(App& app, Replay& r)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({900, 600});
    draw_replay(app, app.workspace.tabs[0], r);
    ImGui::EndFrame();
}

} // namespace

TEST_CASE("the channel checkbox in the filter table takes the click, not the tree node (T87b a3 F5)")
{
    UiTest ctx({1000, 700});
    App app;
    app.workspace.tabs.push_back({.uid = 8});
    Replay r;
    r.open = true;
    r.data.rows = replay_id_rows(replay_parse("(0.0) a 100#01\n(0.1) a 200#02\n", TraceFileFormat::CanDump));
    r.data.channels = {"a"};
    r.data.channel_lin = {0};
    r.mapping = {replay_trace_only};
    REQUIRE(r.data.rows.size() == 2);
    REQUIRE(r.data.rows[0].rx_on);
    replay_frame(app, r);
    replay_frame(app, r);
    ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(app.workspace.tabs[0], "Replay").c_str());
    REQUIRE(w != nullptr);
    ImGuiTable* t = ImGui::TableFindByID(w->GetID("##filter"));
    REQUIRE(t != nullptr);
    const ImGuiStyle& st = ImGui::GetStyle();
    // On the label of the channel checkbox ("a (CAN)"), right of the tree arrow, in the first body row.
    const ImVec2 at{t->Columns[0].MinX + st.CellPadding.x + ImGui::GetTreeNodeToLabelSpacing() + st.ItemSpacing.x
                        + ImGui::GetFrameHeight() + 12.0f,
                    t->OuterRect.Min.y + ImGui::GetTextLineHeight() + st.CellPadding.y * 3.0f + ImGui::GetFrameHeight() * 0.5f};
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    replay_frame(app, r);
    io.AddMouseButtonEvent(0, true);
    replay_frame(app, r);
    io.AddMouseButtonEvent(0, false);
    replay_frame(app, r);
    CHECK_FALSE(r.data.rows[0].rx_on); // unticked the whole channel
    CHECK_FALSE(r.data.rows[1].rx_on);
}

// tests/.../gen_ref.py: python-can 4.6.1 BLFWriter (zlib containers, header v1, CAN_MESSAGE,
// CAN_FD_MESSAGE_64, CAN_ERROR_EXT), six frames at 1700000000.00 + 10 ms steps.
constexpr unsigned char python_can_blf[] = {
      0x4c, 0x4f, 0x47, 0x47, 0x90, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
      0x02, 0x06, 0x08, 0x01, 0x4e, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x24, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xe7, 0x07, 0x0b, 0x00, 0x02, 0x00, 0x0e, 0x00,
      0x16, 0x00, 0x0d, 0x00, 0x14, 0x00, 0x00, 0x00, 0xe7, 0x07, 0x0b, 0x00,
      0x02, 0x00, 0x0e, 0x00, 0x16, 0x00, 0x0d, 0x00, 0x14, 0x00, 0x32, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x4c, 0x4f, 0x42, 0x4a, 0x10, 0x00, 0x01, 0x00, 0xbb, 0x00, 0x00, 0x00,
      0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x74, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x9c, 0xf3, 0xf1,
      0x77, 0xf2, 0x52, 0x60, 0x60, 0x64, 0x30, 0x60, 0x60, 0x00, 0x92, 0x0c,
      0x0c, 0x4c, 0x0c, 0xa8, 0x00, 0x28, 0xc6, 0xac, 0xcc, 0xc8, 0xd0, 0xc0,
      0xc8, 0xc4, 0x0c, 0xe6, 0xfb, 0xe0, 0x50, 0x5f, 0x36, 0x6d, 0x06, 0x98,
      0x06, 0xf2, 0x39, 0x3e, 0xdc, 0xdb, 0x33, 0x8b, 0x01, 0xa8, 0x81, 0x85,
      0x95, 0x8d, 0x1d, 0x97, 0xfa, 0x37, 0x3a, 0x86, 0x8c, 0x10, 0xf3, 0x1b,
      0x58, 0xfe, 0xb3, 0x33, 0x34, 0xc0, 0xec, 0x83, 0xa9, 0x2f, 0x01, 0xb2,
      0x53, 0x90, 0xd4, 0x27, 0x1f, 0x3e, 0x09, 0x55, 0xcf, 0xc0, 0x1d, 0xc6,
      0x02, 0x55, 0xcf, 0x2c, 0x02, 0x75, 0x24, 0xc4, 0x2e, 0x0e, 0x4e, 0x2e,
      0x6e, 0x1e, 0x5e, 0x3e, 0x7e, 0x01, 0x41, 0x21, 0x61, 0x06, 0x12, 0x00,
      0xcc, 0x4e, 0x07, 0x20, 0xdb, 0x13, 0xc9, 0xce, 0x9b, 0x91, 0x49, 0x4c,
      0x0c, 0x58, 0xc2, 0x04, 0x08, 0x1a, 0x90, 0x39, 0xb8, 0xfc, 0x18, 0xf0,
      0xe1, 0x17, 0x13, 0xc4, 0xcd, 0x8c, 0x8c, 0x8a, 0xcc, 0x0c, 0x0d, 0xab,
      0xa0, 0xe2, 0x00, 0xc8, 0x12, 0x1c, 0x23, 0x00, 0x00, 0x00
};

TEST_CASE("BLF: python-can's file, and our writer read back")
{
    const std::string_view ref(reinterpret_cast<const char*>(python_can_blf), sizeof python_can_blf);
    CHECK(trace_format_from_path("x.blf") == TraceFileFormat::Blf);
    const ReplayFile f = replay_parse(ref, TraceFileFormat::Blf);
    REQUIRE(f.frames.size() == 6);
    CHECK(f.channels == std::vector<std::string>{"CH 1", "CH 2"});
    for (std::size_t i = 0; i < f.frames.size(); ++i)
    {
        // python-can turns float seconds into ns: ~10 ns off in the file itself.
        CHECK(std::abs(f.frames[i].ts_ns - (1'700'000'000'000'000'000 + static_cast<int64_t>(i) * 10'000'000)) < 1000);
    }
    const BusMessage& std_frame = f.frames[0];
    CHECK(std_frame.id == 0x123);
    CHECK(std_frame.len == 3);
    CHECK(std_frame.data[2] == 3);
    CHECK(has_flag(f.frames[1], bus_flag::extended));
    CHECK(f.frames[1].id == 0x1ABCDEF0);
    CHECK(f.frames[1].iface == 1);
    CHECK(has_flag(f.frames[2], bus_flag::rtr));
    CHECK(f.frames[2].len == 4);
    CHECK(has_flag(f.frames[3], bus_flag::fd));
    CHECK(has_flag(f.frames[3], bus_flag::brs));
    CHECK(f.frames[3].len == 20);
    CHECK(f.frames[3].data[19] == 19);
    CHECK(is_error_frame(f.frames[4]));
    CHECK(has_flag(f.frames[5], bus_flag::tx));

    // Our writer (stored containers) gives the same frames back.
    std::ostringstream out;
    write_trace_file(out, TraceFileFormat::Blf, f.frames, [](uint16_t) { return std::string(); });
    const ReplayFile back = replay_parse(out.str(), TraceFileFormat::Blf);
    REQUIRE(back.frames.size() == f.frames.size());
    for (std::size_t i = 0; i < f.frames.size(); ++i)
    {
        CHECK(same_frame(back.frames[i], f.frames[i]));
        CHECK(back.frames[i].iface == f.frames[i].iface);
    }
}

TEST_CASE("BLF: LIN frames, checksum errors and sleep events round-trip through our writer")
{
    constexpr int64_t t0 = 1'700'000'000'000'000'000;
    const auto lin = [&](int i, uint32_t id, std::vector<uint8_t> data, uint16_t flags, uint16_t errors, uint16_t iface)
    {
        BusMessage m{.id = id, .flags = flags, .errors = errors, .iface = iface, .type = BusType::LIN, .ts_ns = t0 + i * 10'000'000};
        set_length(m, static_cast<int>(data.size()));
        std::ranges::copy(data, m.data.begin());
        return m;
    };
    std::vector<BusMessage> frames = {
        lin(0, 0x3C, {0, 1, 2, 3, 4, 5, 6, 7}, 0, 0, 0),
        lin(1, 0x10, {0xAA, 0xBB}, bus_flag::tx, 0, 1),
        lin(2, 0x22, {9, 8, 7}, 0, bus_error::lin_checksum_error, 0),
        lin(3, 0, {}, bus_flag::lin_sleep, 0, 0),
        lin(4, 0, {}, bus_flag::lin_wakeup, 0, 1),
    };
    BusMessage can{.id = 0x123, .iface = 0, .ts_ns = t0 + 50'000'000};
    set_length(can, 1);
    frames.push_back(can);
    std::ostringstream out;
    write_trace_file(out, TraceFileFormat::Blf, frames, [](uint16_t) { return std::string(); });
    const ReplayFile back = replay_parse(out.str(), TraceFileFormat::Blf);
    CHECK(back.channels == std::vector<std::string>{"CH 1", "CH 2"});
    REQUIRE(back.frames.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        CAPTURE(i);
        CHECK(same_frame(back.frames[i], frames[i]));
    }
}

TEST_CASE("BLF: LIN_MESSAGE2, LIN_CRC_ERROR2 and LIN_SLEEP objects as Vector writes them")
{
    // "LOGG" header: size 144, SYSTEMTIME 2023-11-14 22:13:20 (1700000000) at 40, then bare objects.
    std::string d = "LOGG";
    le32(d, 144);
    d.resize(40);
    for (const uint16_t v : {2023, 11, 2, 14, 22, 13, 20, 0})
    {
        le16(d, v);
    }
    d.resize(144);
    const auto object = [&](uint32_t type, uint64_t ts_ns, std::string body)
    {
        body.resize(body.size() + body.size() % 4);
        d += "LOBJ";
        le16(d, 32);                                   // header size
        le16(d, 1);                                    // header version
        le32(d, static_cast<uint32_t>(32 + body.size()));
        le32(d, type);
        le32(d, 2);                                    // flags: ns timestamps
        le32(d, 0);                                    // client index, object version
        le32(d, static_cast<uint32_t>(ts_ns));
        le32(d, static_cast<uint32_t>(ts_ns >> 32));
        d += body;
    };
    // LinDatabyteTimestampEvent: bus event (sof, baudrate, channel at 12), synch field (two u64),
    // descriptor (supplier, message id, nad, id at 37, dlc at 38, checksum model), 9 databyte
    // timestamps; then data[8] at 112, crc at 120, dir at 122.
    std::string msg2(132, '\0');
    msg2[12] = 2;
    msg2[37] = static_cast<char>(0xBC);                // id 0x3C with its parity bits
    msg2[38] = 3;
    msg2[112] = 0x11;
    msg2[113] = 0x22;
    msg2[114] = 0x33;
    msg2[122] = 1;                                     // dir: tx
    object(57, 5'000'000, msg2);
    std::string crc2(128, '\0');
    crc2[12] = 1;
    crc2[37] = 0x22;
    crc2[38] = 2;
    crc2[112] = static_cast<char>(0xAA);
    crc2[113] = static_cast<char>(0xBB);
    object(60, 6'000'000, crc2);
    std::string sleep(8, '\0');
    sleep[0] = 2;
    sleep[3] = 0x02;                                   // flags: awake now
    object(20, 7'000'000, sleep);
    object(13, 8'000'000, std::string(20, '\0'));      // LIN_DLC_INFO: skipped

    const ReplayFile f = replay_parse(d, TraceFileFormat::Blf);
    REQUIRE(f.frames.size() == 3);
    CHECK(f.channels == std::vector<std::string>{"CH 2", "CH 1"});
    const BusMessage& m = f.frames[0];
    CHECK(m.type == BusType::LIN);
    CHECK(m.ts_ns == 1'700'000'000'005'000'000);
    CHECK(m.id == 0x3C);
    CHECK(m.len == 3);
    CHECK(m.data[0] == 0x11);
    CHECK(m.data[2] == 0x33);
    CHECK(has_flag(m, bus_flag::tx));
    CHECK(m.errors == 0);
    CHECK(m.iface == 0);
    const BusMessage& e = f.frames[1];
    CHECK(e.type == BusType::LIN);
    CHECK(e.errors == bus_error::lin_checksum_error);
    CHECK(e.id == 0x22);
    CHECK(e.len == 2);
    CHECK(e.data[1] == 0xBB);
    CHECK(!has_flag(e, bus_flag::tx));
    CHECK(e.iface == 1);
    CHECK(f.frames[2].type == BusType::LIN);
    CHECK(has_flag(f.frames[2], bus_flag::lin_wakeup));
    CHECK(f.frames[2].len == 0);
    CHECK(f.frames[2].ts_ns == 1'700'000'000'007'000'000);
}

TEST_CASE("MF4: python-can's bus logging file, asammdf's DZ variant, and our own export")
{
    const auto read = [](const char* name)
    {
        std::ifstream in(std::filesystem::path(REPLAY_DATA_DIR) / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    for (const char* name : {"python_can.mf4", "python_can_dz.mf4"})
    {
        CAPTURE(name);
        ReplayFile f = replay_parse(read(name), TraceFileFormat::VectorMdf);
        REQUIRE(f.frames.size() == 6);
        // One channel group per frame kind: sort by time as the frame cache does.
        std::ranges::stable_sort(f.frames, {}, &BusMessage::ts_ns);
        // python-can stamps the header with the writer's local wall-clock time and no UTC offset
        // (hd_time_flags = 1), so the reader resolves it in its own zone. The file was written in
        // CEST (UTC+2), where the first frame reads as 1'700'000'000 s; elsewhere it is off by the
        // difference to that zone (no time zone database: the parser keeps the header as UTC).
        int64_t first_ns = 1'700'000'000'000'000'000 + int64_t{7200} * 1'000'000'000;
        try
        {
            using namespace std::chrono;
            const local_seconds written{1'790'961'382s}; // the header's start time, as in the file
            first_ns -= duration_cast<nanoseconds>(current_zone()->get_info(written).first.offset).count();
        }
        catch (const std::exception&)
        {
        }
        for (std::size_t i = 0; i < f.frames.size(); ++i)
        {
            CHECK(std::abs(f.frames[i].ts_ns - (first_ns + static_cast<int64_t>(i) * 10'000'000)) < 1000);
        }
        CHECK(f.frames[0].id == 0x123);
        CHECK(f.frames[0].len == 3);
        CHECK(f.frames[0].data[2] == 3);
        CHECK(has_flag(f.frames[1], bus_flag::extended));
        CHECK(f.frames[1].id == 0x1ABCDEF0);
        CHECK(f.channels[f.frames[1].iface] == "CH 1");
        CHECK(has_flag(f.frames[2], bus_flag::rtr));
        CHECK(f.frames[2].len == 4);
        CHECK(has_flag(f.frames[3], bus_flag::fd));
        CHECK(has_flag(f.frames[3], bus_flag::brs));
        CHECK(f.frames[3].len == 20);
        CHECK(f.frames[3].data[19] == 19);
        CHECK(is_error_frame(f.frames[4]));
        CHECK(has_flag(f.frames[5], bus_flag::tx));
    }

    // Our MF4 export (CAN_ID as canid_t) reads back.
    ReplayFile f = replay_parse(read("python_can.mf4"), TraceFileFormat::VectorMdf);
    std::ranges::stable_sort(f.frames, {}, &BusMessage::ts_ns);
    std::ostringstream out;
    write_trace_file(out, TraceFileFormat::VectorMdf, f.frames, [](uint16_t) { return std::string(); });
    const ReplayFile back = replay_parse(out.str(), TraceFileFormat::VectorMdf);
    REQUIRE(back.frames.size() == f.frames.size());
    for (std::size_t i = 0; i < f.frames.size(); ++i)
    {
        CAPTURE(i);
        CHECK(back.frames[i].errors == f.frames[i].errors);
        if (!is_error_frame(f.frames[i])) // an error canid_t carries its error class in the id bits
        {
            CHECK(back.frames[i].id == f.frames[i].id);
            CHECK(back.frames[i].len == f.frames[i].len);
            CHECK(back.frames[i].data == f.frames[i].data);
            CHECK(has_flag(back.frames[i], bus_flag::extended) == has_flag(f.frames[i], bus_flag::extended));
        }
        CHECK(has_flag(back.frames[i], bus_flag::tx) == has_flag(f.frames[i], bus_flag::tx));
        CHECK(std::abs(back.frames[i].ts_ns - f.frames[i].ts_ns) < 1000);
    }
}

TEST_CASE("MF4: asammdf's LIN_Frame and LIN_ChecksumError groups")
{
    std::ifstream in(std::filesystem::path(REPLAY_DATA_DIR) / "asammdf_lin.mf4", std::ios::binary);
    ReplayFile f = replay_parse(std::string((std::istreambuf_iterator<char>(in)), {}), TraceFileFormat::VectorMdf);
    REQUIRE(f.frames.size() == 4);
    std::ranges::stable_sort(f.frames, {}, &BusMessage::ts_ns);
    for (std::size_t i = 0; i < f.frames.size(); ++i)
    {
        CAPTURE(i);
        CHECK(f.frames[i].type == BusType::LIN);
        CHECK(std::abs(f.frames[i].ts_ns - (1'700'000'000'000'000'000 + static_cast<int64_t>(i) * 10'000'000)) < 1000);
    }
    CHECK(f.frames[0].id == 0x3C);
    CHECK(f.frames[0].len == 8);
    CHECK(f.frames[0].data[7] == 7);
    CHECK(!has_flag(f.frames[0], bus_flag::tx));
    CHECK(f.channels[f.frames[0].iface] == "CH 1");
    CHECK(f.frames[1].id == 0x10);
    CHECK(f.frames[1].len == 2);
    CHECK(f.frames[1].data[1] == 0xBB);
    CHECK(has_flag(f.frames[1], bus_flag::tx));
    CHECK(f.frames[2].id == 0x3F);
    CHECK(f.frames[2].len == 4);
    CHECK(f.channels[f.frames[2].iface] == "CH 2");
    CHECK(f.frames[3].errors == bus_error::lin_checksum_error);
    CHECK(f.frames[3].id == 0x22);
    CHECK(f.frames[3].len == 3);
    CHECK(f.frames[3].data[0] == 9);
}

TEST_CASE("MF4: VLSD DataBytes (asammdf), plain and deflated SD blocks")
{
    for (const char* name : {"asammdf_vlsd.mf4", "asammdf_vlsd_dz.mf4"})
    {
        CAPTURE(name);
        std::ifstream in(std::filesystem::path(REPLAY_DATA_DIR) / name, std::ios::binary);
        const ReplayFile f = replay_parse(std::string((std::istreambuf_iterator<char>(in)), {}), TraceFileFormat::VectorMdf);
        REQUIRE(f.frames.size() == 3);
        CHECK(f.frames[0].id == 0x123);
        CHECK(f.frames[0].len == 3);
        CHECK(f.frames[0].data[0] == 1);
        CHECK(f.frames[0].data[2] == 3);
        CHECK(f.frames[0].data[3] == 0);
        CHECK(f.frames[1].id == 0x1ABCDEF0);
        CHECK(has_flag(f.frames[1], bus_flag::extended));
        CHECK(f.frames[1].len == 8);
        CHECK(f.frames[1].data[0] == 1);
        CHECK(f.frames[1].data[7] == 8);
        CHECK(f.frames[2].id == 0x7FF);
        CHECK(f.frames[2].len == 1);
        CHECK(f.frames[2].data[0] == 0xAA);
        CHECK(has_flag(f.frames[2], bus_flag::tx));
        CHECK(f.channels[f.frames[2].iface] == "CH 2");
        CHECK(std::abs(f.frames[2].ts_ns - 1'700'000'000'020'000'000) < 1000);
    }
}

TEST_CASE("replay_range picks the part of the file between two times")
{
    const ReplayFile f = replay_parse("(100.0) x 1#01\n(130.0) x 1#02\n(190.0) x 1#03\n(3700.0) x 1#04\n",
                                      TraceFileFormat::CanDump);
    std::vector<FrameCacheRec> all;
    for (const BusMessage& m : f.frames)
    {
        all.push_back(frame_cache_encode(m, 0));
    }
    CHECK(replay_range(all, "", "").size() == 4);
    CHECK(replay_range(all, "30", "").front().data[0] == 2);
    CHECK(replay_range(all, "0:30", "1:30").size() == 2);     // 130 and 190 s
    CHECK(replay_range(all, "1:00:00", "").front().data[0] == 4);
    CHECK(replay_range(all, "2d", "").empty());
    CHECK(replay_range(all, "1:31", "1:30").empty());
    CHECK(replay_range(all, "nonsense", "0").size() == 1);   // malformed = start
}

TEST_CASE("replay bench")
{
    // KRAKEN_BENCH_FILE=<log>: load as the Replay window does (parse into the frame cache when new,
    // map it when cached), then play the whole file "as fast as possible" into the trace with the
    // main thread draining tasks as app_frame does. Run twice: the second load is from the cache.
    const char* file = std::getenv("KRAKEN_BENCH_FILE");
    if (file == nullptr)
    {
        return;
    }
    for (int round = 0; round < 2; ++round)
    {
        App app;
        Replay r;
        r.open = true;
        auto t0 = std::chrono::steady_clock::now();
        replay_load(app, r, file);
        while (!replay_load_poll(app, r))
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(r.data.cache != nullptr);
        const std::size_t frames = r.data.cache->recs.size();
        r.fast = true;
        std::deque<Iface> ifaces;
        Tasks tasks;
        t0 = std::chrono::steady_clock::now();
        replay_start(r, ifaces, tasks);
        while (r.running || !tasks.queue.empty())
        {
            tasks_drain(tasks, app);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const double play_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        replay_stop(r);
        std::printf("replay %s: load %.3f s (%s), play as fast as possible %.2f s = %.1f M frames/s, %zu frames\n",
                    std::filesystem::path(file).filename().c_str(), load_s, round == 0 ? "new or cached" : "cached", play_s,
                    static_cast<double>(frames) / play_s / 1e6, frames);
    }
}
