#include "ui/status_bar.h"

#include <algorithm>
#include <format>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // BeginViewportSideBar

#include "app.h"
#include "ui/theme.h"

namespace
{

enum class Link
{
    Idle,
    Connected, // every enabled interface is open and running
    Partial,   // some are ("k/n up")
    Failed
};

Link link_state(const App& app, const LinkCount& c)
{
    if (!app.measuring)
    {
        return Link::Idle;
    }
    return c.up == 0 ? Link::Failed : c.up < c.total ? Link::Partial : Link::Connected;
}

} // namespace

void status_bar_sample(StatusBarState& s, bool measuring, uint64_t frames, std::chrono::steady_clock::time_point now)
{
    const double dt = std::chrono::duration<double>(now - s.sampled).count();
    if (!measuring || !s.measuring || frames < s.frames)
    {
        // Idle, or the first frame of a measurement: re-base, so no earlier frames count.
        s.rate = 0.0;
        s.measuring = measuring;
    }
    else if (dt >= 1.0)
    {
        s.rate = static_cast<double>(frames - s.frames) / dt;
        s.measuring = true;
    }
    else
    {
        return;
    }
    s.frames = frames; // fields one by one: a whole-struct assignment would drop the notice
    s.sampled = now;
}

void status_bar_notice(StatusBarState& s, std::string text, double seconds)
{
    s.notice = std::move(text);
    s.notice_until = std::chrono::steady_clock::now()
                     + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}

void draw_status_bar(App& app, StatusBarState& s)
{
    // Frames this app took into its trace (the interfaces of the measurement, TX echoes
    // included), not the kernel counters of every system interface.
    status_bar_sample(s, app.measuring, app.trace.end, std::chrono::steady_clock::now());

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
                                       | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    const float px = ImGui::GetFontSize() / 15.0f;
    // One frame-height line plus a pad of 1 px above and below, so the window border does not
    // clip the text's descenders (T42).
    const ImVec2 pad(8.0f * px, std::max(1.0f, px));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    const float height = ImGui::GetFrameHeight() + pad.y * 2.0f;
    if (ImGui::BeginViewportSideBar("##status_bar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags))
    {
        const char* text = app.recorder.recording ? "Recording" : app.measuring ? "Measuring" : "Ready";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        if (!s.notice.empty() && std::chrono::steady_clock::now() < s.notice_until)
        {
            ImGui::SameLine(0.0f, 24.0f * px);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::warn)), "%s", s.notice.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_menu_status(const App& app, const StatusBarState& s)
{
    const LinkCount count = ifaces_link_count(app.ifaces, app.setup);
    const Link link = link_state(app, count);
    const std::string partial = std::format("{}/{} up", count.up, count.total);
    const char* label = link == Link::Connected ? "Connected"
                        : link == Link::Partial ? partial.c_str()
                        : link == Link::Failed  ? "No interface"
                                                : "Disconnected";
    const std::string rate = std::format("{:.0f} frames/s", s.rate);
    const TriggerState trig = conditional_logging_state(app.conditional_logging);
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto pill_w = [&](const char* text) { return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f; };
    float w = pill_w(label) + style.ItemSpacing.x + ImGui::CalcTextSize(rate.c_str()).x + style.ItemSpacing.x * 2.0f;
    if (trig != TriggerState::Off)
    {
        w += pill_w(trigger_state_label(trig)) + style.ItemSpacing.x;
    }
    float x = ImGui::GetWindowContentRegionMax().x - w;
    if (x <= ImGui::GetCursorPosX())
    {
        return; // window too narrow, the menus win
    }
    // A rounded pill at x: text on bg. Returns its width.
    const auto pill = [&](float at, const char* text, ImU32 bg, ImU32 fg)
    {
        ImGui::SetCursorPosX(at);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(pill_w(text), ImGui::GetFrameHeight());
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, size.y * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, fg);
        ImGui::SetCursorPosX(at + style.FramePadding.x);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        return size.x;
    };
    constexpr ImU32 dark_text = IM_COL32(0x1a, 0x12, 0x02, 255);
    if (trig != TriggerState::Off)
    {
        // Conditional logging: brass while armed (waiting), coral once triggered (CSV being written).
        const ImU32 bg = trig == TriggerState::Armed ? theme_u32(theme_kraken_button().border) : theme_u32(theme_stop_button().border);
        x += pill(x, trigger_state_label(trig), bg, dark_text) + style.ItemSpacing.x;
        ImGui::SameLine(x);
    }
    // Pills: teal = all up, brass = partial, coral = none; idle blends into the frame.
    const ImU32 bg = link == Link::Connected ? theme_u32(theme_kraken_button().fill)
                     : link == Link::Partial ? theme_u32(theme_kraken_button().border)
                     : link == Link::Failed  ? theme_u32(theme_stop_button().border)
                                             : ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 fg = link == Link::Connected ? theme_u32(theme_kraken_button().text)
                     : link == Link::Idle    ? ImGui::GetColorU32(ImGuiCol_Text)
                                             : dark_text;
    const float pw = pill(x, label, bg, fg);
    ImGui::SameLine(x + pw + style.ItemSpacing.x * 2.0f);
    ImGui::TextUnformatted(rate.c_str());
}
