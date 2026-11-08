#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h> // NavWindow

#include "ui_test.h"
#include "ui/vim_nav.h"

namespace
{

struct List
{
    VimNav vim;
    int selected = 0;
    bool focus_search = false;
    int h = 0;
    bool with_input = false; // an InputText in the window, focused on the first frame
    bool focus_input = false;
    std::array<char, 32> text{};
};

constexpr int rows = 100;
constexpr int page = 20;

void frame(List& l)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({400, 400});
    ImGui::Begin("List");
    vim_nav(l.vim, l.selected, rows, page, l.focus_search, l.h);
    if (l.with_input)
    {
        if (l.focus_input)
        {
            ImGui::SetKeyboardFocusHere();
            l.focus_input = false;
        }
        ImGui::InputText("##filter", l.text.data(), l.text.size());
    }
    ImGui::End();
    ImGui::EndFrame();
}

void type(List& l, ImWchar c)
{
    ImGui::GetIO().AddInputCharacter(c);
    frame(l);
}

void ctrl(List& l, ImGuiKey key)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(key, true);
    frame(l);
    io.AddKeyEvent(key, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame(l);
}

} // namespace

TEST_CASE("list motions: j k gg G Ctrl+d/u/f/b, clamped to the rows")
{
    const UiTest ui;
    List l;
    frame(l); // the new window takes focus
    type(l, 'j');
    CHECK(l.selected == 1);
    type(l, 'j');
    type(l, 'k');
    CHECK(l.selected == 1);
    type(l, 'k');
    type(l, 'k');
    CHECK(l.selected == 0); // clamped
    type(l, 'G');
    CHECK(l.selected == rows - 1);
    type(l, 'j');
    CHECK(l.selected == rows - 1);
    type(l, 'g');
    CHECK(l.selected == rows - 1); // one g waits
    type(l, 'g');
    CHECK(l.selected == 0);
    ctrl(l, ImGuiKey_D);
    CHECK(l.selected == page / 2);
    ctrl(l, ImGuiKey_F);
    CHECK(l.selected == page / 2 + page);
    ctrl(l, ImGuiKey_B);
    ctrl(l, ImGuiKey_U);
    CHECK(l.selected == 0);
    type(l, 'G');
    ctrl(l, ImGuiKey_F);
    CHECK(l.selected == rows - 1);
}

TEST_CASE("gg needs both g within 0.5 s; j between them cancels")
{
    const UiTest ui;
    List l;
    l.selected = 50;
    frame(l);
    type(l, 'g');
    for (int i = 0; i < 40; ++i) // 0.67 s
    {
        frame(l);
    }
    type(l, 'g');
    CHECK(l.selected == 50);
    type(l, 'j');
    type(l, 'g');
    CHECK(l.selected == 51);
    type(l, 'g');
    CHECK(l.selected == 0);
}

TEST_CASE("h / l and / are reported to the caller")
{
    const UiTest ui;
    List l;
    frame(l);
    type(l, 'h');
    CHECK(l.h == -1);
    type(l, 'l');
    type(l, 'l');
    CHECK(l.h == 1);
    CHECK_FALSE(l.focus_search);
    type(l, '/');
    CHECK(l.focus_search);
    CHECK(l.selected == 0);
}

TEST_CASE("nothing happens while a text field has the keyboard")
{
    const UiTest ui;
    List l;
    l.with_input = true;
    l.focus_input = true;
    for (int i = 0; i < 3; ++i)
    {
        frame(l);
    }
    REQUIRE(ImGui::GetIO().WantTextInput);
    type(l, 'j');
    type(l, 'G');
    ctrl(l, ImGuiKey_D);
    CHECK(l.selected == 0);
    CHECK(std::string_view(l.text.data()) == "jG"); // the field got the letters instead
}

TEST_CASE("copy menu: y copies the whole row, the column's letter copies that column")
{
    const UiTest ui;
    VimNav v;
    const std::vector<VimYankItem> items = {{"Row", "1\t2\t3"}, {"Rx Frames", "1"}, {"Rx Errors", "2"}, {"Rx Overrun", "3"}};
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::Begin("List");
        vim_yank_menu(v, items);
        ImGui::End();
        ImGui::EndFrame();
    };
    draw();
    v.yank_menu = 1;
    draw();
    CHECK(v.yank_menu == 2);
    ImGui::GetIO().AddInputCharacter('x'); // Rx Frames took r, so Rx Errors got x
    draw();
    CHECK(v.yank_menu == 0);
    CHECK(std::string_view(ImGui::GetClipboardText()) == "2");
    v.yank_menu = 1;
    draw();
    ImGui::GetIO().AddInputCharacter('o');
    draw();
    CHECK(std::string_view(ImGui::GetClipboardText()) == "3");
    v.yank_menu = 1;
    draw();
    ImGui::GetIO().AddInputCharacter('y');
    draw();
    CHECK(std::string_view(ImGui::GetClipboardText()) == "1\t2\t3");
}

TEST_CASE("copy menu opened by a right-click appears at the mouse, and forgets the spot when it closes")
{
    const UiTest ui;
    VimNav v;
    const std::vector<VimYankItem> items = {{"Row", "1\t2"}, {"Time", "1"}, {"Data", "2"}};
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::Begin("List");
        vim_yank_menu(v, items);
        ImGui::End();
        ImGui::EndFrame();
    };
    draw();
    v.yank_x = 321.0f;
    v.yank_y = 123.0f;
    v.yank_menu = 1;
    draw();
    draw();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(g.OpenPopupStack.Size == 1);
    REQUIRE(g.OpenPopupStack[0].Window != nullptr);
    CHECK(g.OpenPopupStack[0].Window->Pos.x == doctest::Approx(321.0f));
    CHECK(g.OpenPopupStack[0].Window->Pos.y == doctest::Approx(123.0f));
    ImGui::GetIO().AddInputCharacter('d');
    draw();
    CHECK(std::string_view(ImGui::GetClipboardText()) == "2");
    CHECK(v.yank_menu == 0);
    CHECK(v.yank_x < 0.0f); // the next 'y' menu sits at the window's bottom again
}

TEST_CASE("a right-click on a row arms the copy menu at the mouse; a left-click or a miss does not")
{
    const UiTest ui;
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    bool armed = false;
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({300, 200});
        ImGui::Begin("List");
        ImGui::Selectable("row", false);
        armed = vim_yank_right_click(v);
        ImGui::End();
        ImGui::EndFrame();
    };
    draw();
    const ImGuiWindow* w = ImGui::FindWindowByName("List");
    REQUIRE(w != nullptr);
    const ImVec2 at(w->Pos.x + 40.0f, w->Pos.y + ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y + 4.0f);
    io.AddMousePosEvent(at.x, at.y);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    draw();
    CHECK_FALSE(armed);
    CHECK(v.yank_menu == 0);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    draw();
    CHECK(armed);
    CHECK(v.yank_menu == 1);
    CHECK(v.yank_x == doctest::Approx(at.x));
    CHECK(v.yank_y == doctest::Approx(at.y));
    // Off the row: nothing.
    v = VimNav{};
    io.AddMousePosEvent(at.x, at.y + 120.0f);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    draw();
    CHECK_FALSE(armed);
    CHECK(v.yank_menu == 0);
}

TEST_CASE("shared table copy menu: a right-click on a plain row, or after its Selectable, copies its columns")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({400, 300});
        ImGui::Begin("Table");
        if (ImGui::BeginTable("##other", 1)) // a second table in the window must not close the menu
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("other");
            vim_yank_table_row({{.label = "O", .text = "o"}});
            vim_yank_end_table();
        }
        if (ImGui::BeginTable("##t", 2))
        {
            ImGui::TableNextRow(); // plain text cells: the row-end variant
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("a1");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("b1");
            vim_yank_table_row({{.label = "A", .text = "a1"}, {.label = "B", .text = "b1"}});
            ImGui::TableNextRow(); // a spanning Selectable: the item variant
            ImGui::TableNextColumn();
            ImGui::Selectable("a2", false, ImGuiSelectableFlags_SpanAllColumns);
            vim_yank_table_row({{.label = "A", .text = "a2"}, {.label = "B", .text = "b2"}}, true);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("b2");
            vim_yank_end_table();
        }
        ImGui::End();
        ImGui::EndFrame();
    };
    draw();
    const ImGuiWindow* w = ImGui::FindWindowByName("Table");
    REQUIRE(w != nullptr);
    // Sweep down the window right-clicking: every menu that opens gets `keys[n]`, and what lands on
    // the clipboard is collected. Three rows (other, a1, a2) stack from the top.
    const char keys[] = {'y', 'y', 'b'};
    std::vector<std::string> got;
    for (float y = w->Pos.y + 2.0f; y < w->Pos.y + w->Size.y && got.size() < 3; y += 3.0f)
    {
        io.AddMousePosEvent(w->Pos.x + 30.0f, y);
        draw();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        draw();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        draw();
        draw();
        if (ImGui::GetCurrentContext()->OpenPopupStack.Size == 1)
        {
            ImGui::SetClipboardText("");
            io.AddInputCharacter(static_cast<unsigned>(keys[got.size()]));
            draw();
            CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
            got.emplace_back(ImGui::GetClipboardText());
            y += ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().CellPadding.y; // on to the next row
        }
    }
    REQUIRE(got.size() == 3);
    CHECK(got[0] == "o");
    CHECK(got[1] == "a1\tb1");
    CHECK(got[2] == "b2");
}

TEST_CASE("pick window: 2x2 grid")
{
    // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    const std::array<VimRect, 4> grid = {{
        {.x0 = 0, .y0 = 0, .x1 = 100, .y1 = 100},
        {.x0 = 100, .y0 = 0, .x1 = 200, .y1 = 100},
        {.x0 = 0, .y0 = 100, .x1 = 100, .y1 = 200},
        {.x0 = 100, .y0 = 100, .x1 = 200, .y1 = 200},
    }};
    CHECK(vim_pick_window(grid, 0, VimDir::Right) == 1);
    CHECK(vim_pick_window(grid, 0, VimDir::Down) == 2);
    CHECK(vim_pick_window(grid, 0, VimDir::Left) == -1);
    CHECK(vim_pick_window(grid, 0, VimDir::Up) == -1);
    CHECK(vim_pick_window(grid, 3, VimDir::Up) == 1);
    CHECK(vim_pick_window(grid, 3, VimDir::Left) == 2);
    CHECK(vim_pick_window(grid, 3, VimDir::Next) == 0);
    CHECK(vim_pick_window(grid, -1, VimDir::Right) == 0);
    CHECK(vim_pick_window({}, 0, VimDir::Right) == -1);
}

TEST_CASE("pick window: going back prefers the window we came from (full-height pane, two neighbours)")
{
    // 0 top-left (Trace), 1 bottom-left (tabs), 2 right, full height (Graph)
    const std::array<VimRect, 3> panes = {{
        {.x0 = 0, .y0 = 0, .x1 = 100, .y1 = 80},
        {.x0 = 0, .y0 = 80, .x1 = 100, .y1 = 200}, // centre nearer Graph's
        {.x0 = 100, .y0 = 0, .x1 = 200, .y1 = 200},
    }};
    CHECK(vim_pick_window(panes, 2, VimDir::Left) == 1);     // nearest centre
    CHECK(vim_pick_window(panes, 2, VimDir::Left, 0) == 0);  // came from 0, and it lies to the left
    CHECK(vim_pick_window(panes, 2, VimDir::Right, 0) == -1); // the preference never overrides the direction
}

TEST_CASE("Ctrl+w l focuses the window to the right of the same tab")
{
    const UiTest ui;
    const auto draw = [] {
        ImGui::NewFrame();
        vim_window_nav(*static_cast<VimNav*>(ImGui::GetIO().UserData), 7);
        const auto window = [](const char* name, float x) {
            ImGui::SetNextWindowPos({x, 0});
            ImGui::SetNextWindowSize({100, 100});
            ImGui::Begin(name);
            ImGui::End();
        };
        window("B###B@7", 100);
        window("Other###Other@17", 300); // another tab (uid 17 also ends in 7)
        window("A###A@7", 0);            // last: focused on appearing
        ImGui::EndFrame();
    };
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    io.UserData = &v;
    draw();
    draw();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(g.NavWindow != nullptr);
    REQUIRE(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_W, true);
    draw();
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    io.AddInputCharacter('l');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
    io.AddKeyEvent(ImGuiMod_Ctrl, true); // Ctrl+w Ctrl+l: nothing further right in this tab
    io.AddKeyEvent(ImGuiKey_W, true);
    draw();
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiKey_L, true);
    draw();
    io.AddKeyEvent(ImGuiKey_L, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
}

TEST_CASE("Shift+h / Shift+l focus the window left / right without Ctrl+w")
{
    const UiTest ui;
    const auto draw = [] {
        ImGui::NewFrame();
        vim_window_nav(*static_cast<VimNav*>(ImGui::GetIO().UserData), 7);
        const auto window = [](const char* name, float x) {
            ImGui::SetNextWindowPos({x, 0});
            ImGui::SetNextWindowSize({100, 100});
            ImGui::Begin(name);
            ImGui::End();
        };
        window("B###B@7", 100);
        window("A###A@7", 0); // last: focused on appearing
        ImGui::EndFrame();
    };
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    io.UserData = &v;
    draw();
    draw();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddInputCharacter('L');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
    io.AddInputCharacter('H');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddInputCharacter('L'); // no Shift held (e.g. Caps Lock): not a window move
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "A###A@7");
}

TEST_CASE("Ctrl+w l moves between docked windows (the default layout docks everything)")
{
    const UiTest ui({800.0f, 600.0f}, true);
    ImGuiID left_node = 0;
    ImGuiID right_node = 0;
    const auto draw = [&] {
        ImGui::NewFrame();
        vim_window_nav(*static_cast<VimNav*>(ImGui::GetIO().UserData), 7);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 600});
        ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoDecoration);
        const ImGuiID space = ImGui::GetID("space");
        if (left_node == 0)
        {
            ImGui::DockBuilderRemoveNode(space);
            ImGui::DockBuilderAddNode(space, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(space, {800, 600});
            left_node = ImGui::DockBuilderSplitNode(space, ImGuiDir_Left, 0.5f, nullptr, &right_node);
            ImGui::DockBuilderDockWindow("A###A@7", left_node);
            ImGui::DockBuilderDockWindow("B###B@7", right_node);
            ImGui::DockBuilderFinish(space);
        }
        ImGui::DockSpace(space);
        ImGui::End();
        ImGui::Begin("B###B@7");
        ImGui::End();
        ImGui::Begin("A###A@7");
        ImGui::End();
        ImGui::EndFrame();
    };
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    io.UserData = &v;
    draw();
    draw();
    draw();
    ImGui::SetWindowFocus("A###A@7");
    draw();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(g.NavWindow != nullptr);
    REQUIRE(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_W, true);
    draw();
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    io.AddInputCharacter('l');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
}

TEST_CASE("Ctrl+w h reaches a docked window whose node hides its tab bar (the Trace pane)")
{
    const UiTest ui({800.0f, 600.0f}, true);
    ImGuiID left_node = 0;
    ImGuiID right_node = 0;
    const auto draw = [&] {
        ImGui::NewFrame();
        vim_window_nav(*static_cast<VimNav*>(ImGui::GetIO().UserData), 7);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 600});
        ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoDecoration);
        const ImGuiID space = ImGui::GetID("space");
        if (left_node == 0)
        {
            ImGui::DockBuilderRemoveNode(space);
            ImGui::DockBuilderAddNode(space, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(space, {800, 600});
            left_node = ImGui::DockBuilderSplitNode(space, ImGuiDir_Left, 0.5f, nullptr, &right_node);
            ImGui::DockBuilderGetNode(left_node)->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar; // as workspace_tabs.cpp's Trace node
            ImGui::DockBuilderDockWindow("Trace###Trace@7", left_node);
            ImGui::DockBuilderDockWindow("Graph###Graph@7", right_node);
            ImGui::DockBuilderFinish(space);
        }
        ImGui::DockSpace(space);
        ImGui::End();
        ImGui::Begin("Trace###Trace@7");
        ImGui::End();
        ImGui::Begin("Graph###Graph@7");
        ImGui::End();
        ImGui::EndFrame();
    };
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    io.UserData = &v;
    draw();
    draw();
    draw();
    ImGui::SetWindowFocus("Graph###Graph@7");
    draw();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(std::string_view(g.NavWindow->Name) == "Graph###Graph@7");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_W, true);
    draw();
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    io.AddInputCharacter('h');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "Trace###Trace@7");
}
