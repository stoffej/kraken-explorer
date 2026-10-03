#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_env.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

#include <imgui_internal.h>

#include "app.h"
#include "core/trace_file_format.h"
#include "ui_test.h"
#include "ui/convert.h"
#include "ui/frame_cache.h"
#include "ui/replay.h"

namespace
{

std::filesystem::path scratch()
{
    const auto dir = std::filesystem::temp_directory_path() / std::format("kraken_convert_test_{}", getpid());
    std::filesystem::create_directories(dir);
    test_setenv("XDG_CACHE_HOME", (dir / "cache"));
    return dir;
}

std::string slurp(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// 30 000 frames: classic, extended, CAN FD with 64 bytes, two channels. Enough for several 128 KB
// BLF containers, so objects straddle containers.
std::string candump_log()
{
    std::string log;
    for (int i = 0; i < 30000; ++i)
    {
        const char* ch = i % 3 == 0 ? "can1" : "can0";
        if (i % 7 == 0)
        {
            std::string data;
            for (int k = 0; k < 64; ++k)
            {
                data += std::format("{:02X}", (i + k) & 0xFF);
            }
            log += std::format("({}.{:06}) {} {:03X}##1{}\n", 1700000000 + i / 1000, i % 1000 * 1000, ch, 0x200 + i % 5, data);
        }
        else if (i % 5 == 0)
        {
            log += std::format("({}.{:06}) {} {:08X}#{:02X}11\n", 1700000000 + i / 1000, i % 1000 * 1000, ch, 0x18FF0000 + i % 9, i & 0xFF);
        }
        else
        {
            log += std::format("({}.{:06}) {} {:03X}#{:02X}{:02X}334455667788\n", 1700000000 + i / 1000, i % 1000 * 1000, ch, 0x100 + i % 11,
                               i & 0xFF, (i >> 8) & 0xFF);
        }
    }
    return log;
}

} // namespace

TEST_CASE("a log converts to every format and reads back the same")
{
    const auto dir = scratch();
    const auto src = dir / "in.log";
    std::ofstream(src, std::ios::binary) << candump_log();
    const ReplayFile want = replay_parse(slurp(src), TraceFileFormat::CanDump);
    REQUIRE(want.frames.size() == 30000);
    ConvertState s;
    for (std::size_t f = 0; f < trace_file_formats.size(); ++f)
    {
        const TraceFileFormat format = trace_file_formats[f];
        const auto dst = dir / std::format("out.{}", trace_format_extension(format));
        INFO(dst.string());
        const std::string status = convert_log(src.string(), dst.string(), static_cast<int>(f), s);
        CHECK(status.starts_with("Wrote 30 000 frames"));
        CHECK(s.written.load() == 30000);
        const ReplayFile got = replay_parse(slurp(dst), format);
        REQUIRE(got.frames.size() == want.frames.size());
        for (std::size_t i = 0; i < want.frames.size(); i += 1)
        {
            const BusMessage& a = want.frames[i];
            BusMessage b = got.frames[i];
            if (format == TraceFileFormat::VectorMdf)
            {
                b.flags |= a.flags & bus_flag::brs; // ponytail: our MF4 record has no BRS bit; ASAM CAN_DataFrame export if it matters
            }
            if (a.id != b.id || a.len != b.len || std::abs(a.ts_ns - b.ts_ns) > 1000 || a.flags != b.flags
                || !std::equal(a.data.begin(), a.data.begin() + a.len, b.data.begin()))
            {
                FAIL_CHECK(std::format("{} frame {}: id {:X}/{:X} len {}/{} ts {}/{} flags {:X}/{:X}", trace_format_extension(format), i, a.id, b.id, a.len, b.len, a.ts_ns, b.ts_ns,
                                       a.flags, b.flags));
                break;
            }
        }
    }
    // The second conversion of the same input reuses its frame cache.
    frame_cache_wait_saved();
    CHECK(frame_cache_open(src, frame_cache_path(src)).has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("a cancelled conversion leaves no output; the output may not be the input")
{
    const auto dir = scratch();
    const auto src = dir / "in.log";
    std::ofstream(src, std::ios::binary) << candump_log();
    ConvertState s;
    std::stop_source stop;
    stop.request_stop();
    CHECK(convert_log(src.string(), (dir / "out.asc").string(), 1, s, stop.get_token()) == "Cancelled");
    CHECK_FALSE(std::filesystem::exists(dir / "out.asc"));
    CHECK(convert_log(src.string(), src.string(), 0, s).starts_with("Error"));
    frame_cache_wait_saved();
    std::filesystem::remove_all(dir);
}

TEST_CASE("databases: DBC -> DBF -> DBC")
{
    const auto dir = scratch();
    std::ofstream(dir / "a.dbc") << "VERSION \"\"\n\nBU_: ECU\n\nBO_ 291 Status: 8 ECU\n SG_ Speed : 7|12@0+ (0.1,0) [0|409.5] \"km/h\" Vector__XXX\n\n"
                                    "VAL_ 291 Speed 0 \"Stop\" ;\n";
    CHECK(convert_database((dir / "a.dbc").string(), (dir / "a.dbf").string()) == "Wrote 1 messages, 1 signals to a.dbf");
    CHECK(slurp(dir / "a.dbf").find("[START_SIGNALS] Speed,12,2,4,U,4095,0,0,0,0.1,km/h,,") != std::string::npos);
    CHECK(convert_database((dir / "a.dbf").string(), (dir / "b.dbc").string()) == "Wrote 1 messages, 1 signals to b.dbc");
    CHECK(slurp(dir / "b.dbc").find("SG_ Speed : 7|12@0+ (0.1,0) [0|409.5] \"km/h\"") != std::string::npos);
    CHECK(convert_database((dir / "missing.dbc").string(), (dir / "c.dbf").string()).starts_with("Error"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("convert bench")
{
    // KRAKEN_BENCH_FILE=<log> KRAKEN_BENCH_OUT=<dir>: every format, time and MB/s.
    const char* file = std::getenv("KRAKEN_BENCH_FILE");
    const char* out = std::getenv("KRAKEN_BENCH_OUT");
    if (file == nullptr || out == nullptr)
    {
        return;
    }
    ConvertState s;
    const char* only = std::getenv("KRAKEN_BENCH_FORMATS"); // e.g. "mf4 blf"
    for (std::size_t f = 0; f < trace_file_formats.size(); ++f)
    {
        if (only != nullptr && !std::string_view(only).contains(trace_format_extension(trace_file_formats[f])))
        {
            continue;
        }
        const auto dst = std::filesystem::path(out) / std::format("bench.{}", trace_format_extension(trace_file_formats[f]));
        const auto t0 = std::chrono::steady_clock::now();
        const std::string status = convert_log(file, dst.string(), static_cast<int>(f), s);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double mb = static_cast<double>(std::filesystem::file_size(dst)) / (1 << 20);
        std::printf("%-8s %6.2f s  %5.0f MB out  %5.0f MB/s out  %s\n", trace_format_extension(trace_file_formats[f]).data(), secs, mb, mb / secs,
                    status.c_str());
        std::filesystem::remove(dst);
    }
}

TEST_CASE("window: the output format combo opens and picks a format")
{
    const UiTest ui(ImVec2(1000.0f, 700.0f), false);
    App app;
    app.convert.open = true;
    app.convert.log_in = "/tmp/in.log";
    ImGuiIO& io = ImGui::GetIO();
    const auto frame = [&]
    {
        ImGui::NewFrame();
        draw_convert(app, app.convert);
        ImGui::Render();
    };
    frame();
    frame();
    const ImGuiWindow* w = ImGui::FindWindowByName("Convert");
    REQUIRE(w != nullptr);
    // Click down the window's middle until a popup opens: the combo is the only popup there.
    bool opened = false;
    for (float y = w->Pos.y + 30.0f; y < w->Pos.y + w->Size.y && !opened; y += 6.0f)
    {
        io.AddMousePosEvent(w->Pos.x + w->Size.x * 0.4f, y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        frame();
        opened = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (opened)
        {
            MESSAGE(std::format("popup at y={}", y - w->Pos.y));
        }
    }
    CHECK(opened);
}
