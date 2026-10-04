/*
  Copyright (c) 2026 Schildkroet

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

// drivers: iface_autobaud. Every probe opens listen-only, the first rate with readable traffic
// wins, the channel is closed again, and a channel without a listen-only mode is never opened.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "drivers/driver.h"

namespace
{

constexpr unsigned bus_rate = 250000;
std::vector<IfaceConfig> opens;
int closes = 0;
unsigned current = 0;

// Traffic only at the bus's rate; error frames one step above it, silence elsewhere.
int fake_read(Iface&, BusMessage* out, int, int timeout_ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 20)));
    if (current == bus_rate)
    {
        out[0] = BusMessage{.id = 0x123};
        return 1;
    }
    if (current == 500000)
    {
        out[0] = BusMessage{.errors = bus_error::stuff};
        return 1;
    }
    return 0;
}

const DriverOps fake_can = {
    .name = "FakeCan",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig& c)
    {
        opens.push_back(c);
        current = c.bitrate;
        return c.bitrate != 800000; // a rate the adapter refuses
    },
    .close = [](Iface&) { ++closes; },
    .send = [](Iface&, const BusMessage&) { return true; },
    .read = fake_read,
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

void setup(Iface& iface, uint32_t caps)
{
    iface.ops = &fake_can;
    iface.info.name = "can0";
    iface.info.capabilities = caps;
    for (const unsigned br : {125000u, 250000u, 500000u, 800000u, 1000000u})
    {
        iface.info.bitrates.push_back({.bitrate = br});
        iface.info.bitrates.push_back({.bitrate = br, .bitrate_fd = 2000000}); // FD presets repeat the rate
    }
    opens.clear();
    closes = 0;
}

} // namespace

TEST_CASE("auto-baud probes listen-only from the highest rate down and stops at readable traffic")
{
    Iface iface;
    setup(iface, iface_cap::listen_only | iface_cap::canfd);
    IfaceConfig config;
    config.listen_only = false; // the setup's own mode must not leak into the probe
    config.can_fd = true;
    const std::optional<unsigned> rate = iface_autobaud(iface, config);
    REQUIRE(rate.has_value());
    CHECK(*rate == bus_rate);
    REQUIRE(opens.size() == 4); // 1M idle, 800k refused, 500k errors, 250k traffic; 125k not tried
    CHECK(opens[0].bitrate == 1000000);
    CHECK(opens[3].bitrate == 250000);
    CHECK(std::ranges::all_of(opens, [](const IfaceConfig& c) { return c.listen_only && !c.can_fd; }));
    CHECK(closes == 3); // every opened probe closed again, the refused one never opened
    CHECK_FALSE(iface.open);
}

TEST_CASE("auto-baud never opens a channel that cannot listen passively, nor one that is in use")
{
    Iface active_only;
    setup(active_only, 0);
    CHECK_FALSE(iface_autobaud(active_only, {}).has_value());
    CHECK(opens.empty());

    Iface busy;
    setup(busy, iface_cap::listen_only);
    busy.open = true; // a measurement owns it
    CHECK_FALSE(iface_autobaud(busy, {}).has_value());
    CHECK(opens.empty());
    busy.open = false;
}
