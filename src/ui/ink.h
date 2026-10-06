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


#pragma once

// Blooper: typing "ink" (outside text fields) brings the Kraken up from the bottom edge and splats
// ink over the screen, Mario Kart style. The ink sprays, drips and runs; everything is gone in a
// few seconds.

#include <array>
#include <vector>

struct InkBlob
{
    float x = 0.0f, y = 0.0f, r = 0.0f; // centre and radius of the main splat (pixels)
    float delay = 0.0f;                  // s after the trigger it lands
    float drip = 0.0f;                   // how fast it runs down the screen (fraction of r per s)
    std::array<float, 36> lobes{};       // 12 x (angle, distance, radius) of satellites and droplets, as fractions of r
    std::array<float, 30> streaks{};     // 10 x (angle, length, width) of the spray, as fractions of r
};

struct InkTentacle
{
    float x = 0.0f;     // where it breaks the bottom edge (pixels)
    float reach = 0.0f; // how far up it gets (pixels)
    float sway = 0.0f;  // side-to-side amplitude (fraction of reach)
    float curl = 0.0f;  // which way the tip curls, and how much
    float phase = 0.0f;
};

struct InkState
{
    std::vector<InkBlob> blobs;
    std::vector<InkTentacle> tentacles;
    double started = -1.0; // ImGui time of the trigger; < 0: no ink on screen
    int typed = 0;         // letters of "ink" matched so far
};

inline constexpr float ink_life = 4.5f; // s from the trigger to the last trace

[[nodiscard]] bool ink_active(const InkState& s) noexcept;
// Splats now: fresh random blobs and tentacles over the main viewport.
void ink_trigger(InkState& s);
// Watches the typed letters and draws the ink on the foreground; once per frame after the windows.
void ink_frame(InkState& s);
