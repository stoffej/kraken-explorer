// app_measurement_start: pressing "Release the Kraken" with no interface selected, or with
// every enabled interface down, must refuse to start and tell the user what to do.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>

#include <imgui.h>

#include "app.h"
#include "drivers/driver.h"
#include "ui/raw_tx.h"
#include "ui_test.h"

namespace
{

void add_fake(App& app, const DriverOps& ops, const char* name, bool enabled = true)
{
    Iface& i = app.ifaces.emplace_back();
    i.ops = &ops;
    i.index = static_cast<uint16_t>(app.ifaces.size() - 1);
    i.info.name = name;
    if (app.setup.networks.empty())
    {
        app.setup.networks.push_back({.name = "Net"});
    }
    app.setup.networks[0].interfaces.push_back({.driver = "Fake", .name = name, .enabled = enabled});
}

} // namespace

TEST_CASE("start with no interface selected is refused with an actionable error")
{
    const UiTest ui;
    App app;
    app_measurement_start(app);
    CHECK_FALSE(app.measuring);                                             // nothing to measure on
    CHECK(app.measurement_error.find("Setup Interface") != std::string::npos); // ... and this is the fix
    app_measurement_stop(app);
}

TEST_CASE("start with every enabled interface down is refused and names the interface")
{
    const UiTest ui;
    App app;
    add_fake(app, fake_down_driver, "fake0");
    app.setup.networks[0].interfaces.push_back({.driver = "Fake", .name = "gone0", .enabled = true}); // not enumerated
    app_measurement_start(app);
    CHECK_FALSE(app.measuring);
    CHECK(app.measurement_error.find("fake0") != std::string::npos);
    CHECK(app.measurement_error.find("gone0") != std::string::npos);
    app_measurement_stop(app);
}

TEST_CASE("start with one interface up and one down still starts, with a notice")
{
    const UiTest ui;
    App app;
    add_fake(app, fake_driver, "fake0");
    add_fake(app, fake_down_driver, "fake1");
    app_measurement_start(app);
    CHECK(app.measuring);
    CHECK(app.measurement_error.empty()); // a notice, not the blocking error
    app_measurement_stop(app);
}

TEST_CASE("the refused start shows the Cannot start measurement modal until closed")
{
    const UiTest ui;
    App app;
    app.measurement_error = "No interface selected.";
    for (int i = 0; i < 2; ++i)
    {
        ImGui::NewFrame();
        draw_measurement_error(app);
        ImGui::EndFrame();
    }
    ImGui::NewFrame();
    CHECK(ImGui::IsPopupOpen("Cannot start measurement"));
    draw_measurement_error(app);
    ImGui::EndFrame();
}

TEST_CASE("the id width the frame editors pre-select follows the received traffic")
{
    App app;
    CHECK_FALSE(iface_rx_extended(app, 1).has_value()); // nothing received: no suggestion
    BusMessage ext{.id = 0x0EFF0080, .flags = bus_flag::extended, .iface = 1};
    BusMessage std11{.id = 0x100, .iface = 1};
    BusMessage tx_echo{.id = 0x200, .flags = bus_flag::tx, .iface = 1};
    app.rx_scratch = {ext, ext, std11, tx_echo, tx_echo}; // own TX echoes do not count
    rx_id_width_update(app);
    CHECK(iface_rx_extended(app, 1) == true);
    CHECK_FALSE(iface_rx_extended(app, 0).has_value());
    app.rx_scratch = {std11, std11};
    rx_id_width_update(app);
    CHECK(iface_rx_extended(app, 1) == false); // 2 x 29-bit vs 3 x 11-bit
}
