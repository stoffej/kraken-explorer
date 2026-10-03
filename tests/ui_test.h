#pragma once

// Headless ImGui context for tests: no platform/renderer backend, 800x600 display, 60 fps.
// Feed input with io.AddInputCharacter / io.AddKeyEvent, then ImGui::NewFrame() ... EndFrame().
// Same config flags as main.cpp, so keyboard nav and docking behave as in the app.

#include <string>

#include <imgui.h>
#include <implot.h>

#include "ui/theme.h" // theme_test and the font-dependent windows

struct UiTest
{
    // docking: ImGuiConfigFlags_DockingEnable (workspace tabs, layouts). implot: an ImPlot context
    // too (Graph windows).
    explicit UiTest(ImVec2 display = ImVec2(800.0f, 600.0f), bool docking = true, bool implot = false)
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = display;
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | (docking ? ImGuiConfigFlags_DockingEnable : 0);
        // No renderer backend: ImGui 1.92 then wants a pre-built atlas (RendererHasTextures off).
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        io.Fonts->GetTexDataAsAlpha8(&pixels, &w, &h);
        // The clipboard stays inside the test: on Windows ImGui's default is the real one, shared
        // with every other test process and whatever the user copies meanwhile.
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { clipboard() = text; };
        pio.Platform_GetClipboardTextFn = [](ImGuiContext*) { return clipboard().c_str(); };
        if (implot)
        {
            ImPlot::CreateContext();
        }
    }
    ~UiTest()
    {
        if (ImPlot::GetCurrentContext() != nullptr)
        {
            ImPlot::DestroyContext();
        }
        ImGui::DestroyContext();
    }
    static std::string& clipboard()
    {
        static std::string text;
        return text;
    }
    UiTest(const UiTest&) = delete;
    UiTest& operator=(const UiTest&) = delete;
};
