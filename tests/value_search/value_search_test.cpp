#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_env.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "app.h"
#include "db/dbc/dbc_parser.h"
#include "ui/frame_cache.h"
#include "ui/value_search.h"

namespace
{

constexpr uint32_t angle_id = 0x111;
constexpr uint32_t flip_id = 0x222;

// 0x111 every 20 ms: a 16-bit signed signal x0.01 rising 0..499 then falling 500..1 (1000 frames,
// 0..5 deg). 0x222 in between: byte 0 flips 0/1 every frame (1000 one-sample stretches of value 1).
std::string candump_log()
{
    std::string s;
    for (int i = 0; i < 2000; ++i)
    {
        const double t = static_cast<double>(i) * 0.01;
        if (i % 2 == 0)
        {
            const int k = i / 2;
            const int raw = k < 500 ? k : 1000 - k;
            s += std::format("({:.6f}) vcan0 {:03X}#{:02X}{:02X}000000000000\n", t, angle_id, raw & 0xFF, raw >> 8);
        }
        else
        {
            s += std::format("({:.6f}) vcan0 {:03X}#{:02X}\n", t, flip_id, (i / 2) & 1);
        }
    }
    return s;
}

constexpr std::string_view dbc = R"(VERSION ""

NS_:

BS_:

BU_: Kraken

BO_ 273 Tentacle1: 8 Kraken
 SG_ Angle : 0|16@1- (0.01,0) [-180|180] "deg" Kraken

BO_ 546 Flip: 1 Kraken
 SG_ Bit : 0|8@1+ (1,0) [0|1] "" Kraken
)";

// The log as a file view (frame cache under a scratch XDG_CACHE_HOME) or appended live.
struct Fixture
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("kraken_value_search_{}", getpid());
    App app;
    std::shared_ptr<CanDb> db = std::make_shared<CanDb>();
    const CanDbMessage* angle_msg = nullptr;
    const CanDbSignal* angle = nullptr;
    const CanDbMessage* flip_msg = nullptr;
    const CanDbSignal* bit = nullptr;

    explicit Fixture(bool file_view)
    {
        REQUIRE(dbc_parse(dbc, *db));
        SetupNetwork net{.name = "Kraken"};
        net.can_dbs.push_back(db);
        app.setup.networks.push_back(std::move(net));
        setup_rebuild_cache(app.setup);
        angle_msg = &db->messages.at(angle_id);
        angle = can_db_find_signal(*angle_msg, "Angle");
        flip_msg = &db->messages.at(flip_id);
        bit = can_db_find_signal(*flip_msg, "Bit");
        const ReplayFile parsed = replay_parse(candump_log(), TraceFileFormat::CanDump);
        REQUIRE(parsed.frames.size() == 2000);
        if (!file_view)
        {
            trace_append(app.trace, parsed.frames);
            return;
        }
        std::filesystem::create_directories(dir);
        test_setenv("XDG_CACHE_HOME", dir);
        const std::filesystem::path src = dir / "value.log";
        std::ofstream(src, std::ios::binary) << candump_log();
        const std::filesystem::path kfc = frame_cache_path(src);
        {
            auto built = frame_cache_build(src, kfc, TraceFileFormat::CanDump);
            REQUIRE(built.has_value());
            frame_cache_close(*built);
            frame_cache_wait_saved(); // tests reopen it from disk
        }
        auto c = frame_cache_open(src, kfc);
        REQUIRE(c.has_value());
        app.trace_file = std::make_shared<FrameCache>(*c);
        trace_open_file(app.trace, app.trace_file->recs, app.trace_file->overflow);
    }

    ~Fixture()
    {
        app.trace_file.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    SignalEntry entry() const
    {
        return {.label = "Tentacle1.Angle", .network = 0, .raw_id = angle_id, .can_msg = angle_msg, .can_sig = angle};
    }
};

} // namespace

TEST_CASE("every sample of a value range, in the file view and in the live trace")
{
    for (const bool file_view : {true, false})
    {
        CAPTURE(file_view);
        Fixture f(file_view);
        const int64_t first_ts = trace_at(f.app.trace, f.app.trace.begin).ts_ns;

        // 2..3 deg: raw 200..300 going up (frames 400..600) and 300..200 going down (1400..1600).
        const ValueResult r = value_search_run(f.app, *f.angle_msg, *f.angle, 2.0, 3.0);
        REQUIRE(r.hits.size() == 202);
        CHECK_FALSE(r.truncated);
        CHECK(r.samples == 1000);
        CHECK(r.seen_min == doctest::Approx(0.0));
        CHECK(r.seen_max == doctest::Approx(5.0));
        CHECK(r.hits[0].index == f.app.trace.begin + 400);
        CHECK(r.hits[0].ts_ns == first_ts + 4'000'000'000);
        CHECK(r.hits[0].value == doctest::Approx(2.0));
        CHECK(r.hits[100].index == f.app.trace.begin + 600);
        CHECK(r.hits[100].value == doctest::Approx(3.0));
        CHECK(r.hits[101].index == f.app.trace.begin + 1400);
        CHECK(r.hits[201].index == f.app.trace.begin + 1600);
        CHECK(std::ranges::is_sorted(r.hits, {}, &ValueHit::ts_ns));

        // Open bounds: every sample. Nothing in range: no hit, the range still seen. One value: one hit.
        CHECK(value_search_run(f.app, *f.angle_msg, *f.angle, -1e9, 1e9).hits.size() == 1000);
        const ValueResult none = value_search_run(f.app, *f.angle_msg, *f.angle, 100.0, 200.0);
        CHECK(none.hits.empty());
        CHECK(none.samples == 1000);
        CHECK(none.seen_max == doctest::Approx(5.0));
        CHECK(value_search_run(f.app, *f.angle_msg, *f.angle, 5.0, 5.0).hits.size() == 1);
    }
}

TEST_CASE("the hit cap")
{
    Fixture f(true);
    // Bit == 1 on every other 0x222 frame: 500 hits; capped at 5.
    const ValueResult capped = value_search_run(f.app, *f.flip_msg, *f.bit, 1.0, 1.0, 5);
    CHECK(capped.hits.size() == 5);
    CHECK(capped.truncated);
    const ValueResult all = value_search_run(f.app, *f.flip_msg, *f.bit, 1.0, 1.0);
    CHECK(all.hits.size() == 500);
    CHECK_FALSE(all.truncated);
    CHECK(all.hits[1].index - all.hits[0].index == 4); // every other 0x222 frame, 0x111 in between
}

TEST_CASE("the worker scan lands through poll; a newer scan replaces a running one")
{
    Fixture f(true);
    ValueSearch v;
    v.signal = f.entry();
    // After a pick: the range only.
    value_search_start(f.app, v, -1e9, 1e9, true);
    CHECK(v.job.valid());
    for (int i = 0; i < 500 && !value_search_poll(v); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(v.job.valid());
    CHECK(v.from == "0");
    CHECK(v.to == "5");
    CHECK(v.hits.empty());
    CHECK(v.status.starts_with("in the trace: 0 .. 5"));

    // A search: hits and status; starting another at once discards the first.
    value_search_start(f.app, v, 2.0, 3.0, false);
    value_search_start(f.app, v, 4.0, 5.0, false); // raw 400..500 up (101) and 499..400 down (100)
    for (int i = 0; i < 500 && !value_search_poll(v); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(v.hits.size() == 201);
    CHECK(v.status.starts_with("201 hits"));
    CHECK(v.selected == -1);

    // Live trace: no worker, the result is there at once.
    Fixture live(false);
    ValueSearch lv;
    lv.signal = live.entry();
    value_search_start(live.app, lv, 2.0, 3.0, false);
    CHECK_FALSE(lv.job.valid());
    CHECK(lv.hits.size() == 202);
}

TEST_CASE("a stop ends a scan early with what it had")
{
    Fixture f(true);
    const std::vector<uint32_t> list = frame_cache_message_frames(*f.app.trace_file, f.app.setup, f.angle_msg);
    REQUIRE(list.size() == 1000);
    std::stop_source stop;
    stop.request_stop();
    std::atomic<uint64_t> progress{0};
    // The stop is checked every 65536 frames, so a 1000-frame list completes; progress ends at the size.
    const ValueResult r = value_search_scan(*f.app.trace_file, list, *f.angle_msg, *f.angle, 2.0, 3.0, value_search_max_hits,
                                            stop.get_token(), &progress);
    CHECK(r.hits.size() == 202);
    CHECK(progress == 1000);
}

TEST_CASE("raw patterns: id and data bytes with wildcards")
{
    const auto p = raw_pattern_parse("0x111", "01 ?? FF");
    REQUIRE(p.has_value());
    CHECK(p->id == 0x111);
    REQUIRE(p->bytes.size() == 3);
    CHECK(p->bytes[0] == 0x01);
    CHECK_FALSE(p->bytes[1].has_value());
    CHECK(p->bytes[2] == 0xFF);
    CHECK(raw_pattern_parse("", "01??FF")->bytes.size() == 3);
    CHECK_FALSE(raw_pattern_parse("", "")->id.has_value());
    CHECK(raw_pattern_parse("", "")->bytes.empty());
    CHECK_FALSE(raw_pattern_parse("zz", "").has_value());
    CHECK_FALSE(raw_pattern_parse("", "0").has_value());   // half a byte
    CHECK_FALSE(raw_pattern_parse("", "0G").has_value());
    BusMessage m{.id = 0x111};
    m.data = {0x01, 0x22, 0xFF, 0x00};
    set_length(m, 4);
    CHECK(raw_pattern_matches(*p, m));
    CHECK_FALSE(raw_pattern_matches(*raw_pattern_parse("222", "01 ?? FF"), m));
    CHECK_FALSE(raw_pattern_matches(*raw_pattern_parse("", "01 ?? FE"), m));
    CHECK_FALSE(raw_pattern_matches(*raw_pattern_parse("", "01 ?? FF 00 05"), m)); // a set byte past the length
    CHECK(raw_pattern_matches(*raw_pattern_parse("", "01 ?? FF 00 ??"), m));       // a wildcard past it is fine
    CHECK(raw_pattern_matches(*raw_pattern_parse("", "?? ?? ?? 00"), m));
}

TEST_CASE("raw search by id, by data and by both, file view and live")
{
    for (const bool file_view : {true, false})
    {
        CAPTURE(file_view);
        Fixture f(file_view);
        // Every 0x222 frame (1000), every 0x222 with byte 0 == 1 (500), that byte over every id (500:
        // the 0x111 frames never have byte 0 == 1 except raw 1 and 257... raw 1 at k=1 and k=999, raw 257
        // at k=257 and k=743 -> 4 more).
        CHECK(raw_search_run(f.app, *raw_pattern_parse("222", "")).hits.size() == 1000);
        CHECK(raw_search_run(f.app, *raw_pattern_parse("222", "01")).hits.size() == 500);
        CHECK(raw_search_run(f.app, *raw_pattern_parse("", "01")).hits.size() == 504);
        CHECK(raw_search_run(f.app, *raw_pattern_parse("111", "?? 01")).hits.size() == 489); // byte 1 == 1: raw 256..511, up and down
        CHECK(raw_search_run(f.app, *raw_pattern_parse("333", "")).hits.empty());
        const ValueResult hits = raw_search_run(f.app, *raw_pattern_parse("222", "01"), 7);
        CHECK(hits.hits.size() == 7);
        CHECK(hits.truncated);
        CHECK(hits.hits[0].index == f.app.trace.begin + 3); // frame 3: i = 3 -> (3/2)&1 == 1
    }
}
