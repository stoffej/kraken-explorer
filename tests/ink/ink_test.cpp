// ui/ink: typing "ink" splats ink; it is gone after ink_life seconds; a wrong key resets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/ink.h"
#include "ui_test.h"

namespace
{

void frame()
{
    ImGui::NewFrame();
    ImGui::EndFrame();
}

} // namespace

TEST_CASE("typing ink triggers the ink, a wrong key resets the sequence, the ink fades out")
{
    UiTest ui;
    InkState s;
    frame();
    const ImGuiKey code[] = {ImGuiKey_I, ImGuiKey_N, ImGuiKey_K};
    // "in", then "i" again: a new attempt from its first letter.
    for (ImGuiKey k : {ImGuiKey_I, ImGuiKey_N})
    {
        ImGui::GetIO().AddKeyEvent(k, true);
        ImGui::NewFrame();
        ink_frame(s);
        ImGui::EndFrame();
        ImGui::GetIO().AddKeyEvent(k, false);
        frame();
    }
    CHECK(s.typed == 2);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_I, true);
    ImGui::NewFrame();
    ink_frame(s);
    ImGui::EndFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_I, false);
    frame();
    CHECK(s.typed == 1);
    CHECK_FALSE(ink_active(s));

    for (ImGuiKey k : code)
    {
        ImGui::GetIO().AddKeyEvent(k, true);
        ImGui::NewFrame();
        ink_frame(s);
        ImGui::EndFrame();
        ImGui::GetIO().AddKeyEvent(k, false);
        frame();
    }
    CHECK(ink_active(s));
    CHECK(s.blobs.size() == 12);
    CHECK(s.tentacles.size() == 5);
    // 60 fps: gone after ink_life + 0.5 s.
    for (int i = 0; i < static_cast<int>((ink_life + 1.0f) * 60.0f); ++i)
    {
        ImGui::NewFrame();
        ink_frame(s);
        ImGui::EndFrame();
    }
    CHECK_FALSE(ink_active(s));
    CHECK(s.blobs.empty());
    CHECK(s.tentacles.empty());
}
