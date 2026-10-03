#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "ui/file_dialog.h"

namespace fs = std::filesystem;

TEST_CASE("filter patterns")
{
    CHECK(file_filter_match("*.asc", "trace.asc"));
    CHECK(file_filter_match("*.asc", "TRACE.ASC")); // case-insensitive
    CHECK_FALSE(file_filter_match("*.asc", "trace.asc.bak"));
    CHECK(file_filter_match("*.asc *.candump *.log", "x.log"));
    CHECK_FALSE(file_filter_match("*.asc *.candump", "x.log"));
    CHECK(file_filter_match("*", "anything"));
    CHECK(file_filter_match("", "anything")); // no pattern = all
    CHECK(file_filter_match("trace_??.asc", "trace_01.asc"));
    CHECK_FALSE(file_filter_match("trace_??.asc", "trace_1.asc"));
    CHECK(file_filter_match("*a*b*", "xxaYYbzz"));
    CHECK_FALSE(file_filter_match("*.dbc", ""));
}

TEST_CASE("save path appends the filter extension")
{
    const fs::path dir = "/tmp/x";
    CHECK(file_dialog_save_path(dir, "work", "*.kraken") == "/tmp/x/work.kraken");
    CHECK(file_dialog_save_path(dir, "work.xml", "*.kraken") == "/tmp/x/work.xml");
    CHECK(file_dialog_save_path(dir, "log", "*") == "/tmp/x/log");
    CHECK(file_dialog_save_path(dir, "log", "*.log *.txt") == "/tmp/x/log.log");
    CHECK(file_dialog_save_path(dir, "/abs/name", "*.csv") == "/abs/name.csv");
}

TEST_CASE("picking another save type swaps the name's extension (T87b a1 F2: Save Trace wrote ASC)")
{
    CHECK(file_dialog_retype("trace.asc", "*.pcapng") == "trace.pcapng");
    CHECK(file_dialog_retype("trace.asc", "*.candump *.log") == "trace.candump");
    CHECK(file_dialog_retype("trace", "*.mf4") == "trace.mf4");
    CHECK(file_dialog_retype("sub/trace.asc", "*.trc") == "sub/trace.trc");
    CHECK(file_dialog_retype("trace.asc", "*") == "trace.asc");
    CHECK(file_dialog_retype("", "*.asc").empty());
}

TEST_CASE("listing and sorting a directory")
{
    const fs::path root = fs::temp_directory_path() / ("file_dialog_test_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "alpha");
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // alpha is the older one even where the times below cannot be set
    fs::create_directories(root / "Zeta");
    const auto write = [&](const char* name, std::size_t bytes) { std::ofstream(root / name) << std::string(bytes, 'x'); };
    write("b.asc", 30);
    write("A.dbc", 10);
    write("c.log", 20);
    write(".hidden", 1);
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(root / "b.asc", now - std::chrono::hours(3));
    fs::last_write_time(root / "A.dbc", now - std::chrono::hours(1));
    fs::last_write_time(root / "c.log", now - std::chrono::hours(2));
    std::error_code no_dir_times; // Windows: std::filesystem cannot set a directory's time
    fs::last_write_time(root / "alpha", now - std::chrono::hours(5), no_dir_times);
    fs::last_write_time(root / "Zeta", now - std::chrono::hours(4), no_dir_times);

    std::string error;
    auto entries = file_dialog_list(root, false, error);
    CHECK(error.empty());
    REQUIRE(entries.size() == 5);
    const auto names = [&] {
        std::vector<std::string> n;
        for (const auto& e : entries)
        {
            n.push_back(e.name);
        }
        return n;
    };

    file_dialog_sort(entries, FileSortColumn::Name, true);
    CHECK(names() == std::vector<std::string>{"alpha", "Zeta", "A.dbc", "b.asc", "c.log"});
    CHECK(entries[0].is_dir);
    CHECK(entries[2].size == 10);
    file_dialog_sort(entries, FileSortColumn::Name, false);
    CHECK(names() == std::vector<std::string>{"Zeta", "alpha", "c.log", "b.asc", "A.dbc"});
    file_dialog_sort(entries, FileSortColumn::Size, false);
    CHECK(names() == std::vector<std::string>{"Zeta", "alpha", "b.asc", "c.log", "A.dbc"});
    file_dialog_sort(entries, FileSortColumn::Modified, true);
    CHECK(names() == std::vector<std::string>{"alpha", "Zeta", "b.asc", "c.log", "A.dbc"}); // oldest first

    CHECK(file_dialog_list(root, true, error).size() == 6);

    error.clear();
    CHECK(file_dialog_list(root / "missing", false, error).empty());
    CHECK_FALSE(error.empty());

#ifndef _WIN32 // std::filesystem cannot take read access away there
    if (geteuid() != 0) // root reads anything
    {
        fs::create_directories(root / "locked");
        fs::permissions(root / "locked", fs::perms::none);
        error.clear();
        CHECK(file_dialog_list(root / "locked", false, error).empty());
        CHECK(error.find("Cannot open") != std::string::npos);
        fs::permissions(root / "locked", fs::perms::owner_all);
    }
#endif
    fs::remove_all(root);
}

TEST_CASE("folder with only hidden entries is empty, not unreadable")
{
    const fs::path root = fs::temp_directory_path() / ("file_dialog_hidden_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / ".git");
    std::ofstream(root / ".hidden") << "x";
    std::string error;
    CHECK(file_dialog_list(root, false, error).empty());
    CHECK(error.empty());

    FileDialog d;
    file_dialog_open(d, FileDialogMode::Open, "Open", root.string(), {});
    CHECK(d.entries.empty());
    CHECK_FALSE(d.unreadable);
    d.error = "x is not a folder"; // a validation message must not turn the list into "Cannot read"
    CHECK_FALSE(d.unreadable);
    fs::remove_all(root);
}
