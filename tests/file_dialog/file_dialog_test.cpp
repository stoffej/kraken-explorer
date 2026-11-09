#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/platform.h"
#include "ui/file_dialog.h"
#include "test_env.h"
#include "ui_test.h"
#include <imgui_internal.h> // FindWindowByName, debug log

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

// Address bar (Explorer-style): Ctrl+L or a click past the breadcrumbs edits the path with all
// of it selected, Ctrl+X / Ctrl+V move it through the clipboard, Enter goes to a folder or, for
// a file path (quotes and newline from "Copy as path" / a terminal tolerated), chooses the file.
TEST_CASE("address bar: cut, paste and Enter on a folder or file path")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    const fs::path tmp = fs::temp_directory_path() / ("kraken_fd_addr_" + std::to_string(getpid()));
    fs::create_directories(tmp / "a");
    fs::create_directories(tmp / "b");
    std::ofstream(tmp / "b" / "x.asc") << "date\n";

    FileDialog d;
    file_dialog_open(d, FileDialogMode::Open, "Open", (tmp / "a").string());
    std::vector<std::string> result;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        if (auto r = file_dialog_draw(d); !r.empty())
        {
            result = std::move(r);
        }
        ImGui::EndFrame();
    };
    const auto press = [&](ImGuiKeyChord chord)
    {
        const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        for (const bool down : {true, false})
        {
            if (chord & ImGuiMod_Ctrl)
            {
                io.AddKeyEvent(ImGuiMod_Ctrl, down);
            }
            io.AddKeyEvent(key, down);
            frame();
        }
    };
    frame();
    frame();
    CHECK_FALSE(d.edit_location);

    press(ImGuiMod_Ctrl | ImGuiKey_L);
    frame();
    CHECK(d.edit_location);
    press(ImGuiMod_Ctrl | ImGuiKey_X); // the whole path is selected on entry
    CHECK(UiTest::clipboard() == (tmp / "a").string());
    CHECK(d.location.empty());

    UiTest::clipboard() = "\"" + (tmp / "b").string() + "\"\n";
    press(ImGuiMod_Ctrl | ImGuiKey_V);
    CHECK(d.location.contains((tmp / "b").string())); // whatever of the quotes / newline the field keeps, Enter strips
    press(ImGuiKey_Enter);
    CHECK(d.dir == tmp / "b");
    CHECK_FALSE(d.edit_location);
    CHECK(d.error.empty());

    press(ImGuiMod_Ctrl | ImGuiKey_L);
    frame();
    UiTest::clipboard() = (tmp / "b" / "x.asc").string();
    press(ImGuiMod_Ctrl | ImGuiKey_V); // replaces the selected path
    press(ImGuiKey_Enter);
    CHECK(result == std::vector<std::string>{(tmp / "b" / "x.asc").string()});
    CHECK_FALSE(d.visible);

    // Reopened: Ctrl+L, Esc, Ctrl+L works again (a GTK bug that must not be ours), and Esc in
    // the field does not close the dialog.
    file_dialog_open(d, FileDialogMode::Open, "Open", (tmp / "a").string());
    frame();
    frame();
    press(ImGuiMod_Ctrl | ImGuiKey_L);
    CHECK(d.edit_location);
    press(ImGuiKey_Escape);
    CHECK_FALSE(d.edit_location);
    CHECK(d.visible);
    press(ImGuiMod_Ctrl | ImGuiKey_L);
    CHECK(d.edit_location);
    press(ImGuiKey_Escape);

    // Nothing focused: a typed "/" starts a path with the caret after it, "~" is home.
    io.AddInputCharacter('/');
    frame();
    CHECK(d.edit_location);
    CHECK(d.location == "/");
    frame();
    io.AddInputCharactersUTF8("tmp");
    frame();
    CHECK(d.location == "/tmp");
    press(ImGuiKey_Escape);
    io.AddInputCharacter('~');
    frame();
    frame();
    press(ImGuiKey_Enter);
    CHECK(d.dir == platform_home_dir().lexically_normal());

    // Nothing focused: Ctrl+V puts the clipboard's path in the field, Enter goes there.
    UiTest::clipboard() = "  '" + (tmp / "b").string() + "'\n";
    press(ImGuiMod_Ctrl | ImGuiKey_V);
    CHECK(d.edit_location);
    CHECK(d.location == (tmp / "b").string());
    press(ImGuiKey_Enter);
    CHECK(d.dir == tmp / "b");

    // A quoted full path pasted into the Name field opens that file (Explorer, GTK).
    d.file_name = "\"" + (tmp / "b" / "x.asc").string() + "\"";
    result.clear();
    press(ImGuiKey_Enter);
    CHECK(result == std::vector<std::string>{(tmp / "b" / "x.asc").string()});

    fs::remove_all(tmp);
}

TEST_CASE("address bar: a click past the breadcrumbs edits the path")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    FileDialog d;
    file_dialog_open(d, FileDialogMode::Open, "Open", fs::temp_directory_path().string());
    const auto frame = [&]
    {
        ImGui::NewFrame();
        (void)file_dialog_draw(d);
        ImGui::EndFrame();
    };
    frame();
    frame();
    ImGuiWindow* w = ImGui::FindWindowByName("Open###file_dialog");
    REQUIRE(w != nullptr);
    // The empty end of the address bar: right side of the first row.
    io.AddMousePosEvent(w->Pos.x + w->Size.x - 30.0f, w->Pos.y + ImGui::GetFrameHeight() * 2.0f);
    frame();
    frame();
    CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_TextInput);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    CHECK(d.edit_location);
    frame();
    frame();
    io.AddInputCharacter('Q');
    frame();
    CHECK(d.location == "Q"); // the whole path was selected, typing replaces it; not the name field
    CHECK(d.file_name.empty());
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    CHECK_FALSE(d.edit_location); // back to the breadcrumbs, the dialog stays
    CHECK(d.visible);
    CHECK(d.location == d.dir.string()); // the shown folder again (Windows' temp path has a trailing separator) // the whole path was selected, typing replaces it
}

// The same driver as above, for the edge cases of the address bar and the Name field.
struct AddressBarRig
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    fs::path tmp = fs::temp_directory_path() / ("kraken_fd_edge_" + std::to_string(getpid()));
    FileDialog d;
    std::vector<std::string> result;

    AddressBarRig()
    {
        fs::create_directories(tmp / "a");
        fs::create_directories(tmp / "with space" / "deep");
        std::ofstream(tmp / "a" / "x.asc") << "date\n";
        std::ofstream(tmp / "with space" / "y.asc") << "date\n";
    }
    ~AddressBarRig()
    {
        fs::remove_all(tmp);
    }
    void open(FileDialogMode mode, const fs::path& start)
    {
        file_dialog_open(d, mode, "Edge", start.string(), {{"ASC", "*.asc"}});
        frame();
        frame();
    }
    void frame()
    {
        ImGui::NewFrame();
        if (auto r = file_dialog_draw(d); !r.empty())
        {
            result = std::move(r);
        }
        ImGui::EndFrame();
    }
    void press(ImGuiKeyChord chord)
    {
        const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        for (const bool down : {true, false})
        {
            if (chord & ImGuiMod_Ctrl)
            {
                io.AddKeyEvent(ImGuiMod_Ctrl, down);
            }
            io.AddKeyEvent(key, down);
            frame();
        }
    }
    // Ctrl+L, then the text replaces the selected path, then Enter.
    void enter_path(const std::string& text)
    {
        press(ImGuiMod_Ctrl | ImGuiKey_L);
        REQUIRE(d.edit_location);
        io.AddInputCharactersUTF8(text.c_str());
        frame();
        REQUIRE(d.location.contains(text.substr(0, text.find_first_of("\r\n")))); // the field drops newlines
        press(ImGuiKey_Enter);
    }
    ImGuiWindow* window()
    {
        ImGuiWindow* w = ImGui::FindWindowByName("Edge###file_dialog");
        REQUIRE(w != nullptr);
        return w;
    }
    void click(float x, float y)
    {
        io.AddMousePosEvent(x, y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    }
    // Click into the Name field (second row from the bottom, left part: the first spot from the
    // bottom where the mouse shows the text cursor) and type.
    void type_name(const char* text)
    {
        ImGuiWindow* w = window();
        bool found = false;
        for (float y = w->Pos.y + w->Size.y - 4.0f; y > w->Pos.y && !found; y -= 4.0f)
        {
            io.AddMousePosEvent(w->Pos.x + w->Size.x * 0.4f, y);
            frame();
            found = ImGui::GetMouseCursor() == ImGuiMouseCursor_TextInput;
        }
        REQUIRE(found);
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        press(ImGuiMod_Ctrl | ImGuiKey_A); // the field keeps its text on focus: replace it
        io.AddInputCharactersUTF8(text);
        frame();
    }
};

TEST_CASE("address bar: relative paths, trailing slash, quotes and spaces")
{
    AddressBarRig r;
    r.open(FileDialogMode::Open, r.tmp);
    r.enter_path("a");
    CHECK(r.d.dir == r.tmp / "a");
    CHECK(r.d.error.empty());
    r.enter_path("..");
    CHECK(r.d.dir == r.tmp);
    r.enter_path((r.tmp / "a").string() + "/");
    CHECK(r.d.dir == r.tmp / "a");
    r.enter_path("\"" + (r.tmp / "with space" / "deep").string() + "\"\r\n");
    CHECK(r.d.dir == r.tmp / "with space" / "deep");
    CHECK_FALSE(r.d.edit_location);
    r.enter_path("'" + (r.tmp / "with space" / "y.asc").string() + "'");
    CHECK(r.result == std::vector<std::string>{(r.tmp / "with space" / "y.asc").string()});
    CHECK_FALSE(r.d.visible);
}

TEST_CASE("address bar: a missing path is an error that stays for the next Ctrl+L, an empty one does nothing")
{
    AddressBarRig r;
    r.open(FileDialogMode::Open, r.tmp);
    r.enter_path((r.tmp / "nope").string());
    CHECK(r.d.dir == r.tmp);
    CHECK(r.d.error == (r.tmp / "nope").string() + " not found");
    CHECK_FALSE(r.d.edit_location);
    r.press(ImGuiMod_Ctrl | ImGuiKey_L);
    CHECK(r.d.location == (r.tmp / "nope").string()); // to correct it
    r.press(ImGuiKey_Escape);
    CHECK(r.d.visible);
    r.enter_path(" ");
    CHECK(r.d.visible);
    CHECK(r.d.dir == r.tmp);
    CHECK(r.d.error.empty()); // an emptied field drops the rejected text and its error
    r.press(ImGuiMod_Ctrl | ImGuiKey_L);
    CHECK(r.d.location == r.tmp.string());
    r.press(ImGuiKey_Escape);
    r.enter_path((r.tmp / "nope").string());
    CHECK_FALSE(r.d.error.empty());
    r.enter_path("a");
    CHECK(r.d.error.empty()); // navigating clears it
}

TEST_CASE("address bar in Save mode: an existing file asks before replacing, a new file's path fills the name")
{
    AddressBarRig r;
    r.open(FileDialogMode::Save, r.tmp / "out.asc");
    r.enter_path((r.tmp / "a" / "x.asc").string());
    CHECK(r.d.dir == r.tmp / "a");
    CHECK(r.d.file_name == "x.asc");
    CHECK(r.d.confirm_path == (r.tmp / "a" / "x.asc").string()); // "Replace?"
    CHECK(r.result.empty());
    r.press(ImGuiKey_Enter); // Replace
    CHECK(r.result == std::vector<std::string>{(r.tmp / "a" / "x.asc").string()});

    r.result.clear();
    r.open(FileDialogMode::Save, r.tmp / "out.asc");
    r.enter_path((r.tmp / "with space" / "new").string());
    CHECK(r.d.dir == r.tmp / "with space");
    CHECK(r.d.file_name == "new");
    CHECK(r.result.empty());
    CHECK(r.d.visible);
    r.press(ImGuiKey_Enter); // Save, with the filter's extension
    CHECK(r.result == std::vector<std::string>{(r.tmp / "with space" / "new.asc").string()});

    r.result.clear();
    r.open(FileDialogMode::Save, r.tmp / "out.asc");
    r.enter_path((r.tmp / "missing" / "new.asc").string());
    CHECK(r.result.empty());
    CHECK(r.d.error == (r.tmp / "missing" / "new.asc").string() + " not found");
}

TEST_CASE("address bar in SelectFolder mode: a file path is refused, a folder is entered, Enter selects it")
{
    AddressBarRig r;
    r.open(FileDialogMode::SelectFolder, r.tmp);
    r.enter_path((r.tmp / "a" / "x.asc").string());
    CHECK(r.result.empty());
    CHECK(r.d.visible);
    CHECK(r.d.error == (r.tmp / "a" / "x.asc").string() + " is not a folder");
    r.enter_path((r.tmp / "a").string());
    CHECK(r.d.dir == r.tmp / "a");
    CHECK(r.result.empty());
    r.press(ImGuiKey_Enter); // Select
    CHECK(r.result == std::vector<std::string>{(r.tmp / "a").string()});
}

TEST_CASE("address bar: ~ expands to HOME, also with a sub path; typed ~/ in one frame is kept whole")
{
    AddressBarRig r;
    const char* old_home = std::getenv("HOME");
    const fs::path saved_home = old_home != nullptr ? old_home : "";
    test_setenv("HOME", r.tmp / "with space");
    r.open(FileDialogMode::Open, r.tmp);
    r.enter_path("~");
    CHECK(r.d.dir == r.tmp / "with space");
    r.enter_path("~/deep");
    CHECK(r.d.dir == r.tmp / "with space" / "deep");
    r.enter_path("~/y.asc");
    CHECK(r.result == std::vector<std::string>{(r.tmp / "with space" / "y.asc").string()});

    // Save mode: a Name of "~" goes home, it does not save a ".asc" there.
    r.result.clear();
    r.open(FileDialogMode::Save, r.tmp / "out.asc");
    r.type_name("~");
    REQUIRE(r.d.file_name == "~");
    r.press(ImGuiKey_Enter);
    CHECK(r.result.empty());
    CHECK(r.d.dir == r.tmp / "with space");
    CHECK_FALSE(fs::exists(r.tmp / "with space" / ".asc"));

    // Fast typing lands two characters in one frame (no text field was active to trickle them).
    r.io.AddInputCharactersUTF8("~/de");
    r.frame();
    CHECK(r.d.edit_location);
    CHECK(r.d.location == "~/de");
    r.frame();
    r.io.AddInputCharactersUTF8("ep");
    r.frame();
    CHECK(r.d.location == "~/deep");
    r.press(ImGuiKey_Enter);
    CHECK(r.d.dir == r.tmp / "with space" / "deep");
    test_setenv("HOME", saved_home);
}

TEST_CASE("address bar: Ctrl+V with multi-line text on the clipboard is not a path and does nothing")
{
    AddressBarRig r;
    r.open(FileDialogMode::Open, r.tmp);
    UiTest::clipboard() = "line one\nline two\n";
    r.press(ImGuiMod_Ctrl | ImGuiKey_V);
    CHECK_FALSE(r.d.edit_location);
    CHECK(r.d.dir == r.tmp);
    UiTest::clipboard() = (r.tmp / "a").string() + "\n";
    r.press(ImGuiMod_Ctrl | ImGuiKey_V);
    CHECK(r.d.edit_location);
    r.press(ImGuiKey_Enter);
    CHECK(r.d.dir == r.tmp / "a");
}

TEST_CASE("address bar: a click elsewhere restores the breadcrumbs without navigating")
{
    AddressBarRig r;
    r.open(FileDialogMode::Open, r.tmp);
    r.press(ImGuiMod_Ctrl | ImGuiKey_L);
    r.io.AddInputCharactersUTF8("/nowhere");
    r.frame();
    CHECK(r.d.location == "/nowhere");
    ImGuiWindow* w = r.window();
    r.click(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.5f); // the file list
    CHECK_FALSE(r.d.edit_location);
    CHECK(r.d.dir == r.tmp);
    CHECK(r.d.error.empty());
    CHECK(r.d.location == r.tmp.string()); // the abandoned text is gone
}

TEST_CASE("Ctrl+L while typing a name moves to the address bar; '/' typed into the name stays there")
{
    AddressBarRig r;
    r.open(FileDialogMode::Open, r.tmp);
    r.type_name("a/b");
    CHECK(r.d.file_name == "a/b"); // not the address bar's '/'
    CHECK_FALSE(r.d.edit_location);
    r.press(ImGuiMod_Ctrl | ImGuiKey_L);
    CHECK(r.d.edit_location);
    r.io.AddInputCharactersUTF8("a");
    r.frame();
    CHECK(r.d.location == "a"); // typing continues in the address bar
    CHECK(r.d.file_name == "a/b");
    r.press(ImGuiKey_Enter);
    CHECK(r.d.dir == r.tmp / "a");
}

TEST_CASE("Name field: a folder path navigates, a quoted file path with spaces opens it")
{
    AddressBarRig r;
    r.open(FileDialogMode::Open, r.tmp);
    r.type_name(("\"" + (r.tmp / "with space").string() + "\" ").c_str());
    r.press(ImGuiKey_Enter);
    CHECK(r.d.dir == r.tmp / "with space");
    CHECK(r.result.empty());
    r.type_name((" '" + (r.tmp / "with space" / "y.asc").string() + "'").c_str());
    r.press(ImGuiKey_Enter);
    CHECK(r.result == std::vector<std::string>{(r.tmp / "with space" / "y.asc").string()});
}
