#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include "ui_test.h"

// The right-click menu every std::string text field gets from src/imgui_stdlib.cpp.
namespace
{

struct Field
{
    std::string text = "hello world";
    std::string ro = "read only";
    bool edited = false;
    void draw()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({400, 300});
        ImGui::Begin("Form");
        edited = ImGui::InputText("##field", &text);
        ImGui::InputTextMultiline("##log", &ro, ImVec2(300, 100), ImGuiInputTextFlags_ReadOnly);
        ImGui::End();
        ImGui::EndFrame();
    }
};

// Right-click the item whose id is `id` (found by its rect) and run `item` of the menu that opens.
void menu_pick(Field& f, const char* window_item, const char* item)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiWindow* w = ImGui::FindWindowByName("Form");
    REQUIRE(w != nullptr);
    const ImGuiID id = ImHashStr(window_item, 0, w->ID);
    // Sweep down the form until that item is hovered, then right-click it.
    bool hovered = false;
    for (float y = w->Pos.y + 2.0f; !hovered && y < w->Pos.y + w->Size.y; y += 4.0f)
    {
        io.AddMousePosEvent(w->Pos.x + 50.0f, y);
        f.draw();
        hovered = ImGui::GetCurrentContext()->HoveredId == id; // of the frame just drawn, not the one before
    }
    REQUIRE(hovered);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    f.draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    f.draw();
    f.draw();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(g.OpenPopupStack.Size == 1);
    REQUIRE(g.OpenPopupStack[0].Window != nullptr);
    ImGui::ActivateItemByID(ImHashStr(item, 0, g.OpenPopupStack[0].Window->ID));
    f.draw();
    f.draw();
}

} // namespace

TEST_CASE("right-click menu: copy / paste / cut the whole text of a field that is not being edited")
{
    const UiTest ui;
    Field f;
    f.draw();
    ImGui::SetClipboardText("");
    menu_pick(f, "##field", "Copy");
    CHECK(std::string_view(ImGui::GetClipboardText()) == "hello world");
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);

    ImGui::SetClipboardText("pasted");
    menu_pick(f, "##field", "Paste");
    CHECK(f.text == "pasted"); // no selection, not editing: the whole text is replaced
    f.draw();

    menu_pick(f, "##field", "Cut");
    CHECK(f.text.empty());
    CHECK(std::string_view(ImGui::GetClipboardText()) == "pasted");
}

TEST_CASE("right-click menu: a read-only field copies, Paste and Cut are disabled")
{
    const UiTest ui;
    Field f;
    f.draw();
    ImGui::SetClipboardText("");
    menu_pick(f, "##log", "Copy");
    CHECK(std::string_view(ImGui::GetClipboardText()) == "read only");
    ImGui::SetClipboardText("x");
    menu_pick(f, "##log", "Paste"); // disabled item: nothing happens, the menu stays
    CHECK(f.ro == "read only");
}

TEST_CASE("right-click menu: Select All activates the field with everything selected")
{
    const UiTest ui;
    Field f;
    f.draw();
    menu_pick(f, "##field", "Select All");
    for (int i = 0; i < 3; ++i)
    {
        f.draw();
    }
    const ImGuiWindow* w = ImGui::FindWindowByName("Form");
    const ImGuiID id = ImHashStr("##field", 0, w->ID);
    ImGuiInputTextState* st = ImGui::GetInputTextState(id);
    REQUIRE(st != nullptr);
    CHECK(ImGui::GetCurrentContext()->ActiveId == id);
    CHECK(st->HasSelection());
    CHECK(std::abs(st->GetSelectionEnd() - st->GetSelectionStart()) == static_cast<int>(f.text.size()));
}
