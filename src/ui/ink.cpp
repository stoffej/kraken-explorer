/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.

*/


#include "ui/ink.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <random>

#include <imgui.h>

bool ink_active(const InkState& s) noexcept
{
    return s.started >= 0.0;
}

void ink_trigger(InkState& s)
{
    std::minstd_rand rng(static_cast<unsigned>(ImGui::GetTime() * 1000.0) + 1u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const ImVec2 size = ImGui::GetMainViewport()->Size;
    constexpr float tau = 2.0f * std::numbers::pi_v<float>;
    s.blobs.clear();
    for (int i = 0; i < 12; ++i)
    {
        InkBlob b{.x = size.x * (0.05f + 0.9f * u(rng)),
                  .y = size.y * (0.05f + 0.9f * u(rng)),
                  .r = size.y * (0.06f + 0.13f * u(rng)),
                  .delay = 0.25f + 0.6f * u(rng), // the tentacles come first, then the squirts
                  .drip = 0.05f + 0.15f * u(rng)};
        for (int k = 0; k < 12; ++k)
        {
            const bool droplet = k >= 6; // the first 6 lobes sit on the rim, the rest fly off it
            b.lobes[static_cast<std::size_t>(k * 3)] = (static_cast<float>(k) + u(rng)) * tau / 12.0f;
            b.lobes[static_cast<std::size_t>(k * 3 + 1)] = droplet ? 1.4f + 1.4f * u(rng) : 0.6f + 0.3f * u(rng);
            b.lobes[static_cast<std::size_t>(k * 3 + 2)] = droplet ? 0.06f + 0.12f * u(rng) : 0.3f + 0.3f * u(rng);
        }
        for (int k = 0; k < 10; ++k)
        {
            b.streaks[static_cast<std::size_t>(k * 3)] = (static_cast<float>(k) + u(rng)) * tau / 10.0f;
            b.streaks[static_cast<std::size_t>(k * 3 + 1)] = 1.3f + 1.4f * u(rng);
            b.streaks[static_cast<std::size_t>(k * 3 + 2)] = 0.06f + 0.14f * u(rng);
        }
        s.blobs.push_back(b);
    }
    s.tentacles.clear();
    for (int i = 0; i < 5; ++i)
    {
        s.tentacles.push_back({.x = size.x * ((static_cast<float>(i) + 0.2f + 0.6f * u(rng)) / 5.0f),
                               .reach = size.y * (0.5f + 0.4f * u(rng)),
                               .sway = 0.08f + 0.1f * u(rng),
                               .curl = (u(rng) < 0.5f ? -1.0f : 1.0f) * (0.3f + 0.4f * u(rng)),
                               .phase = tau * u(rng)});
    }
    s.started = ImGui::GetTime();
}

namespace
{

void watch_typed(InkState& s)
{
    constexpr ImGuiKey code[] = {ImGuiKey_I, ImGuiKey_N, ImGuiKey_K};
    if (ImGui::GetIO().WantTextInput)
    {
        return;
    }
    if (ImGui::IsKeyPressed(code[s.typed], false))
    {
        if (++s.typed == static_cast<int>(std::size(code)))
        {
            s.typed = 0;
            ink_trigger(s);
        }
        return;
    }
    for (ImGuiKey k : code)
    {
        if (ImGui::IsKeyPressed(k, false))
        {
            s.typed = k == code[0] ? 1 : 0; // out of order: start over (an "i" may begin a new attempt)
        }
    }
}

float smoothstep(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// A tentacle up from the bottom edge: it rises in the first second, whips about, and sinks back as
// the ink fades. Dark skin, darker edges, teal suckers along the inner side.
void draw_tentacle(ImDrawList* dl, const InkTentacle& tc, float now, float fade, float bottom)
{
    constexpr int segments = 48;
    const float rise = smoothstep(now / 0.9f) * fade;
    if (rise <= 0.01f)
    {
        return;
    }
    const float reach = tc.reach * rise;
    const auto a = static_cast<int>(255.0f * std::min(1.0f, fade * 1.5f));
    const ImU32 skin = IM_COL32(14, 26, 36, a);
    const ImU32 edge = IM_COL32(3, 6, 12, a);
    const ImU32 sucker = IM_COL32(60, 140, 140, a);
    const ImU32 cup = IM_COL32(8, 20, 24, a);
    const auto centre = [&](float t)
    {
        const float sway = std::sin(t * 6.0f - now * 4.0f + tc.phase) * tc.sway * t * reach;
        const float curl = tc.curl * t * t * t * reach * 0.6f;
        return ImVec2(tc.x + sway + curl, bottom - reach * t);
    };
    const auto half = [&](float t) { return reach * 0.11f * std::pow(1.0f - t, 0.7f) + 1.5f; };
    for (int i = 0; i <= segments; ++i)
    {
        const float t = static_cast<float>(i) / segments;
        dl->PathLineTo({centre(t).x - half(t), centre(t).y});
    }
    for (int i = segments; i >= 0; --i)
    {
        const float t = static_cast<float>(i) / segments;
        dl->PathLineTo({centre(t).x + half(t), centre(t).y});
    }
    dl->PathFillConcave(skin);
    for (const float side : {-1.0f, 1.0f})
    {
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / segments;
            dl->PathLineTo({centre(t).x + side * half(t), centre(t).y});
        }
        dl->PathStroke(edge, 0, 3.0f);
    }
    const float inner = tc.curl < 0.0f ? -1.0f : 1.0f; // suckers face the way the tip curls
    for (int i = 2; i < segments * 9 / 10; i += 3)
    {
        const float t = static_cast<float>(i) / segments;
        const float row = (i / 3) % 2 == 0 ? 0.55f : 0.15f;
        const ImVec2 c{centre(t).x + inner * half(t) * row, centre(t).y};
        const float r = half(t) * (row > 0.5f ? 0.4f : 0.28f);
        dl->AddCircleFilled(c, r, sucker);
        dl->AddCircleFilled(c, r * 0.4f, cup);
    }
}

void draw_blob(ImDrawList* dl, const InkBlob& b, float now, float fade)
{
    const float t = now - b.delay;
    if (t < 0.0f)
    {
        return;
    }
    const float grow = 1.0f - std::exp(-t * 9.0f); // splash: fast in, then still
    const auto a = static_cast<int>(235.0f * fade);
    const ImU32 ink = IM_COL32(8, 10, 24, a);
    const float r = b.r * grow;
    const float y = b.y + b.drip * b.r * t; // runs down the screen
    const ImVec2 c(b.x, y);
    dl->AddCircleFilled(c, r, ink, 48);
    // Spray: thin streaks shooting out from the rim, longest at the splash.
    for (int k = 0; k < 10; ++k)
    {
        const float ang = b.streaks[static_cast<std::size_t>(k * 3)];
        const float len = b.streaks[static_cast<std::size_t>(k * 3 + 1)] * r;
        const float w = b.streaks[static_cast<std::size_t>(k * 3 + 2)] * r;
        const ImVec2 dir(std::cos(ang), std::sin(ang));
        const ImVec2 nrm(-dir.y * w, dir.x * w);
        const ImVec2 base(c.x + dir.x * r * 0.8f, c.y + dir.y * r * 0.8f);
        dl->AddTriangleFilled(ImVec2(base.x + nrm.x, base.y + nrm.y), ImVec2(base.x - nrm.x, base.y - nrm.y),
                              ImVec2(c.x + dir.x * len, c.y + dir.y * len), ink);
        dl->AddCircleFilled(ImVec2(c.x + dir.x * len, c.y + dir.y * len), w * 0.9f, ink, 12);
    }
    // The drips: tails below the centre, longer and thinner with time.
    for (const float dx : {-0.35f, 0.0f, 0.4f})
    {
        const float w = r * (dx == 0.0f ? 0.18f : 0.1f);
        const float len = r * (0.3f + (dx == 0.0f ? 0.6f : 0.35f) * std::min(t, 2.5f));
        dl->AddRectFilled(ImVec2(c.x + dx * r - w, c.y), ImVec2(c.x + dx * r + w, c.y + len), ink, w);
        dl->AddCircleFilled(ImVec2(c.x + dx * r, c.y + len), w * 1.3f, ink, 16);
    }
    for (int k = 0; k < 12; ++k)
    {
        const float ang = b.lobes[static_cast<std::size_t>(k * 3)];
        const float dist = b.lobes[static_cast<std::size_t>(k * 3 + 1)] * r;
        const float rad = b.lobes[static_cast<std::size_t>(k * 3 + 2)] * r;
        const float fall = k >= 6 ? b.r * 1.2f * t * t : 0.0f; // droplets fly off, then fall
        dl->AddCircleFilled(ImVec2(c.x + std::cos(ang) * dist, c.y + std::sin(ang) * dist + fall), rad, ink, 24);
    }
}

void draw_ink(InkState& s)
{
    const float now = static_cast<float>(ImGui::GetTime() - s.started);
    if (now > ink_life + 0.5f)
    {
        s.started = -1.0;
        s.blobs.clear();
        s.tentacles.clear();
        return;
    }
    const float fade = std::clamp((ink_life - now) / 1.2f, 0.0f, 1.0f);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    for (const InkTentacle& tc : s.tentacles)
    {
        draw_tentacle(dl, tc, now, fade, vp->Pos.y + vp->Size.y);
    }
    for (const InkBlob& b : s.blobs)
    {
        draw_blob(dl, b, now, fade);
    }
}

} // namespace

void ink_frame(InkState& s)
{
    watch_typed(s);
    if (ink_active(s))
    {
        draw_ink(s);
    }
}
