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

// REST setup routes: /interface/<name> settings, LDF, link, /can_status, /setup/reload.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app.h"
#include "core/rest_api.h"
#include "drivers/driver.h"
#include "ui_test.h" // fake_driver

namespace
{

Iface& add_fake(App& app, const char* name, BusType bus = BusType::CAN)
{
    Iface& i = app.ifaces.emplace_back();
    i.ops = &fake_driver;
    i.index = static_cast<uint16_t>(app.ifaces.size() - 1);
    i.info.name = name;
    i.info.bus_type = bus;
    if (app.setup.networks.empty())
    {
        app.setup.networks.push_back({.name = "Net"});
    }
    app.setup.networks[0].interfaces.push_back({.driver = "Fake", .name = name, .bus_type = bus});
    return i;
}

bool has(const std::string& s, std::string_view part)
{
    return s.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("GET /interface/<name>: the full setup; unknown names and routes")
{
    App app;
    add_fake(app, "fake0");
    int status = 0;
    std::string body = rest_api_handle(app, "GET", "/interface/fake0", "", status);
    CHECK(status == 200);
    CHECK(has(body, "\"bitrate\":500000"));
    CHECK(has(body, "\"network\":\"Net\""));
    CHECK(has(body, "\"lin_protocol\":\"2.2A\""));

    (void)rest_api_handle(app, "GET", "/interface/nope", "", status);
    CHECK(status == 404);
    (void)rest_api_handle(app, "GET", "/interface/fake0/ldf", "", status);
    CHECK(status == 404); // GET of a POST-only sub route: no such route
    (void)rest_api_handle(app, "POST", "/interface/fake0/bogus", "{}", status);
    CHECK(status == 404);
}

TEST_CASE("POST /interface/<name>: CAN fields, validation, 409 while measuring")
{
    App app;
    Iface& i = add_fake(app, "fake0");
    i.info.capabilities = iface_cap::canfd;
    int status = 0;
    std::string body = rest_api_handle(app, "POST", "/interface/fake0", R"({"listen_only": false})", status);
    CHECK(status == 200);
    CHECK_FALSE(app.setup.networks[0].interfaces[0].can_fd); // untouched FD fields leave FD off
    body = rest_api_handle(app, "POST", "/interface/fake0",
                                       R"({"bitrate": 250000, "sample_point": 800, "listen_only": true, "one_shot": true,
                                           "auto_restart": true, "auto_restart_ms": 50, "fd_bitrate": 4000000})",
                                       status);
    CHECK(status == 200);
    const SetupInterface& si = app.setup.networks[0].interfaces[0];
    CHECK(si.bitrate == 250000);
    CHECK(si.sample_point == 800);
    CHECK(si.listen_only);
    CHECK(si.one_shot);
    CHECK(si.auto_restart);
    CHECK(si.auto_restart_ms == 50);
    CHECK(si.can_fd); // no bitrate list: taken as given
    CHECK(has(body, "\"fd_bitrate\":4000000"));

    (void)rest_api_handle(app, "POST", "/interface/fake0", R"({"can_fd": false})", status);
    CHECK(status == 200);
    CHECK_FALSE(si.can_fd);
    CHECK(si.fd_bitrate == 0);

    // Bad values leave the setup untouched.
    body = rest_api_handle(app, "POST", "/interface/fake0", R"({"bitrate": 100000, "listen_only": "maybe"})", status);
    CHECK(status == 400);
    CHECK(has(body, "listen_only"));
    CHECK(si.bitrate == 250000);
    (void)rest_api_handle(app, "POST", "/interface/fake0", R"({"sample_point": 1001})", status);
    CHECK(status == 400);
    (void)rest_api_handle(app, "POST", "/interface/fake0", R"({"bitrate": "12x"})", status);
    CHECK(status == 400);
    i.info.capabilities = 0;
    (void)rest_api_handle(app, "POST", "/interface/fake0", R"({"can_fd": true})", status);
    CHECK(status == 400); // no iface_cap::canfd
    CHECK_FALSE(si.can_fd);

    // An interface listing its bit timings: a rate it does not offer is refused with the choices.
    i.info.bitrates = {{.bitrate = 500000, .sample_point = 875}, {.bitrate = 1000000, .sample_point = 750}};
    body = rest_api_handle(app, "POST", "/interface/fake0", R"({"bitrate": 125000})", status);
    CHECK(status == 400);
    CHECK(has(body, "[500000,1000000]"));
    (void)rest_api_handle(app, "POST", "/interface/fake0", R"({"bitrate": 1000000, "sample_point": 750})", status);
    CHECK(status == 200);
    CHECK(si.bitrate == 1000000);

    app.measuring = true;
    (void)rest_api_handle(app, "POST", "/interface/fake0", R"({"bitrate": 500000})", status);
    CHECK(status == 409);
    (void)rest_api_handle(app, "GET", "/interface/fake0", "", status);
    CHECK(status == 200);
    app.measuring = false;

    // Present but in no network.
    Iface& other = app.ifaces.emplace_back();
    other.ops = &fake_driver;
    other.info.name = "fake9";
    body = rest_api_handle(app, "POST", "/interface/fake9", "{}", status);
    CHECK(status == 400);
    CHECK(has(body, "no network"));
}

TEST_CASE("POST /interface/<name>: LIN fields")
{
    App app;
    add_fake(app, "lin0", BusType::LIN);
    int status = 0;
    (void)rest_api_handle(app, "POST", "/interface/lin0",
                    R"({"lin_baudrate": 10400, "lin_protocol": "2.1", "lin_node_mode": "Slave", "lin_checksum_classic": true,
                        "lin_slave_node": "SlaveA", "lin_jitter_us": 100})",
                    status);
    CHECK(status == 200);
    const SetupInterface& si = app.setup.networks[0].interfaces[0];
    CHECK(si.lin_baudrate == 10400);
    CHECK(si.lin_protocol == LinProtocolVersion::V2_1);
    CHECK(si.lin_node_mode == LinNodeMode::Slave);
    CHECK(si.lin_checksum_classic);
    CHECK(si.lin_slave_node == "SlaveA");
    CHECK(si.lin_jitter_us == 100);
    const std::string body = rest_api_handle(app, "POST", "/interface/lin0", R"({"lin_protocol": "3.0"})", status);
    CHECK(status == 400);
    CHECK(has(body, "2.2A"));
}

TEST_CASE("POST /interface/<name>/ldf and /setup/reload")
{
    const auto dir = std::filesystem::temp_directory_path() / "kraken_rest_setup_test";
    std::filesystem::create_directories(dir);
    const std::string ldf = (dir / "t.ldf").string();
    std::ofstream(ldf) << R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_language_version = "2.1";
LIN_speed = 9.6 kbps;
Nodes {
    Master: MasterNode, 10 ms, 0.1 ms ;
    Slaves: SlaveA ;
}
Signals {
    MotorSpeed: 16, 0, MasterNode, SlaveA;
}
Frames {
    MasterFrame: 0x10, MasterNode, 2 {
        MotorSpeed, 0;
    }
}
)";
    App app;
    add_fake(app, "lin0", BusType::LIN);
    int status = 0;
    std::string body = rest_api_handle(app, "POST", "/interface/lin0/ldf", "{}", status);
    CHECK(status == 400);
    body = rest_api_handle(app, "POST", "/interface/lin0/ldf", R"({"path": "/no/such.ldf"})", status);
    CHECK(status == 400);
    body = rest_api_handle(app, "POST", "/interface/lin0/ldf", std::format(R"({{"path": {}}})", json_string(ldf)), status);
    REQUIRE(status == 200);
    CHECK(app.setup.networks[0].lin_dbs.size() == 1);
    const SetupInterface& si = app.setup.networks[0].interfaces[0];
    CHECK(si.lin_baudrate == 9600);
    CHECK(si.lin_timebase_ms == 10);
    CHECK(si.lin_protocol == LinProtocolVersion::V2_1);
    CHECK(si.lin_ldf_path == ldf);
    // Loading it again replaces it.
    (void)rest_api_handle(app, "POST", "/interface/lin0/ldf", std::format(R"({{"path": {}}})", json_string(ldf)), status);
    CHECK(app.setup.networks[0].lin_dbs.size() == 1);

    body = rest_api_handle(app, "POST", "/setup/reload", "", status);
    CHECK(status == 200);
    CHECK(has(body, "\"reloaded\":true"));
    std::filesystem::remove(ldf);
    body = rest_api_handle(app, "POST", "/setup/reload", "", status);
    CHECK(status == 400);
    CHECK(has(body, "t.ldf"));
    app.measuring = true;
    (void)rest_api_handle(app, "POST", "/setup/reload", "", status);
    CHECK(status == 409);
    std::filesystem::remove_all(dir);
}

TEST_CASE("POST /interface/<name>/link and GET /can_status")
{
    App app;
    add_fake(app, "fake0");
    int status = 0;
    std::string body = rest_api_handle(app, "POST", "/interface/fake0/link", "{}", status);
    CHECK(status == 400);
    CHECK(has(body, "up missing"));
    body = rest_api_handle(app, "POST", "/interface/fake0/link", R"({"up": false})", status);
    CHECK(status == 400);
    CHECK(has(body, "SocketCAN"));

    body = rest_api_handle(app, "GET", "/can_status", "", status);
    CHECK(status == 200);
    CHECK(has(body, "\"name\":\"fake0\""));
    CHECK(has(body, "\"seen\":false"));
    app.can_status.rows.resize(1);
    app.can_status.rows[0].seen = true;
    app.can_status.rows[0].stats.rx_frames = 42;
    app.can_status.rows[0].error_frames = 3;
    body = rest_api_handle(app, "GET", "/can_status", "", status);
    CHECK(has(body, "\"rx_frames\":42"));
    CHECK(has(body, "\"error_frames\":3"));
}
