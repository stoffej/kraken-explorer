// drivers/canblast: discovery, heartbeat and frame decoding against a fake CANblaster
// server on 127.0.0.1. Datagram layouts are struct can_frame / canfd_frame from <linux/can.h>.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/net.h"
#include "drivers/driver.h"

extern const DriverOps canblast_driver;
bool is_discovery(const char* data, std::size_t size);

namespace
{

void send_to(uint16_t port, const void* data, std::size_t size)
{
    const int fd = net_socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(fd, static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    net_close(fd);
}

void send_str(uint16_t port, const std::string& s) { send_to(port, s.data(), s.size()); }

} // namespace

TEST_CASE("canblast: discovery datagram check")
{
    const auto ok = [](const std::string& s) { return is_discovery(s.data(), s.size()); };
    CHECK(ok(R"({"protocol":"CANblaster","version":1})"));
    CHECK(ok(" {\n\t\"protocol\" : \"CANblaster\",\r\n  \"version\": 1\n}\n"));
    CHECK(ok(R"({"version":1,"protocol":"CANblaster"})"));
    CHECK(ok(R"( { "version" : 1 , "protocol" : "CANblaster" } )"));
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":1,"x":0})")); // extra key
    CHECK_FALSE(ok(R"({"x":0,"version":1,"protocol":"CANblaster"})"));
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":2})"));       // wrong version
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":10})"));
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":1.0})"));
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":"1"})"));
    CHECK_FALSE(ok(R"({"protocol":"canblaster","version":1})"));       // wrong protocol
    CHECK_FALSE(ok(R"({"protocol":"CANblaster"})"));                  // missing key
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":1)"));        // broken
    CHECK_FALSE(ok(R"({"protocol":"CANblaster","version":1}})"));
    CHECK_FALSE(ok("not json"));
    CHECK_FALSE(ok(""));
}

TEST_CASE("canblast: disabled driver enumerates nothing without blocking")
{
    canblast_enabled = false;
    std::vector<IfaceInfo> out;
    const auto t0 = std::chrono::steady_clock::now();
    canblast_driver.enumerate(out);
    CHECK(out.empty());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
}

TEST_CASE("canblast: discovery, heartbeat and frames")
{
    canblast_enabled = true;
    std::jthread server([] {
        for (int i = 0; i < 5; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            send_str(20000, R"({"protocol":"CANblaster","version":2})");       // wrong version
            send_str(20000, R"({"protocol":"CANblaster","version":1,"x":0})"); // extra key
            send_str(20000, "not json");
            send_str(20000, R"({"protocol":"CANblaster","version":1})");
        }
    });
    std::vector<IfaceInfo> infos;
    canblast_driver.enumerate(infos);
    server.join();
    canblast_enabled = false;
    REQUIRE(infos.size() == 1);
    CHECK(infos[0].name == "127.0.0.1");
    CHECK(infos[0].bitrates.size() == 30);

    // Heartbeat listener stands in for the server's port 20002.
    const int hb = net_socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(20002);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(hb, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);

    Iface iface;
    iface.ops = &canblast_driver;
    iface.info = infos[0];
    iface.index = 3;
    REQUIRE(canblast_driver.open(iface, IfaceConfig{}));

    // classic extended RTR-less frame, 3 bytes
    std::array<uint8_t, 16> cf{};
    const uint32_t id1 = 0x80000000U | 0x18DAF110U;
    std::memcpy(cf.data(), &id1, 4);
    cf[4] = 3;
    cf[8] = 0xAA; cf[9] = 0xBB; cf[10] = 0xCC;
    send_to(20001, cf.data(), cf.size());
    // CAN FD standard frame with BRS, 12 bytes
    std::array<uint8_t, 72> fdf{};
    const uint32_t id2 = 0x123;
    std::memcpy(fdf.data(), &id2, 4);
    fdf[4] = 12;
    fdf[5] = 0x01;
    for (int i = 0; i < 12; ++i) { fdf[8 + i] = static_cast<uint8_t>(i); }
    send_to(20001, fdf.data(), fdf.size());
    // wrong size: dropped
    send_str(20001, "garbage");

    std::vector<BusMessage> got;
    std::array<BusMessage, 16> buf{};
    for (int tries = 0; tries < 20 && got.size() < 2; ++tries)
    {
        const int n = canblast_driver.read(iface, buf.data(), 16, 100);
        REQUIRE(n >= 0);
        got.insert(got.end(), buf.begin(), buf.begin() + n);
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0].id == 0x18DAF110U);
    CHECK(has_flag(got[0], bus_flag::extended));
    CHECK_FALSE(has_flag(got[0], bus_flag::fd));
    CHECK(got[0].len == 3);
    CHECK(got[0].data[2] == 0xCC);
    CHECK(got[0].iface == 3);
    CHECK(got[0].ts_ns > 0);
    CHECK(got[1].id == 0x123);
    CHECK(has_flag(got[1], bus_flag::fd));
    CHECK(has_flag(got[1], bus_flag::brs));
    CHECK(got[1].len == 12);
    CHECK(got[1].dlc == 9);
    CHECK(got[1].data[11] == 11);

    REQUIRE(net_wait_readable(hb, 1000) == 1);
    std::array<char, 32> hbuf{};
    const auto hn = recv(hb, hbuf.data(), hbuf.size(), 0);
    CHECK(std::string(hbuf.data(), static_cast<std::size_t>(hn)) == "Heartbeat");
    net_close(hb);

    IfaceStats st;
    canblast_driver.stats(iface, st);
    CHECK(st.rx_frames == 2);
    CHECK(st.rx_errors == 1);
    CHECK_FALSE(canblast_driver.send(iface, BusMessage{}));

    canblast_driver.close(iface);
}
