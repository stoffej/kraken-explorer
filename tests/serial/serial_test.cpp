// core/serial against a pseudo-terminal pair: the slave end behaves like a tty device.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>

#include "core/serial.h"

#ifdef __APPLE__
#include <util.h> // openpty
#else
#include <pty.h>
#endif
#include <unistd.h>

using namespace std::chrono_literals;

TEST_CASE("open, write, read and time out on a pty")
{
    int master = -1;
    int slave = -1;
    char name[256] = {};
    REQUIRE(openpty(&master, &slave, name, nullptr, nullptr) == 0);

    SerialPort port;
    REQUIRE_MESSAGE(serial_open(port, name, 1000000), port.error);
    CHECK(serial_is_open(port));

    char buf[64] = {};
    CHECK(serial_read(port, buf, sizeof(buf), 20ms) == 0); // nothing sent yet

    REQUIRE(::write(master, "t1238\r", 6) == 6);
    const long n = serial_read(port, buf, sizeof(buf), 500ms);
    CHECK(std::string(buf, n > 0 ? static_cast<size_t>(n) : 0) == "t1238\r");

    CHECK(serial_write(port, "O\r", 2));
    char out[8] = {};
    CHECK(::read(master, out, sizeof(out)) == 2);
    CHECK(std::memcmp(out, "O\r", 2) == 0);

    serial_clear(port);
    serial_close(port);
    CHECK_FALSE(serial_is_open(port));
    serial_close(port); // double close is harmless
    ::close(slave);
    ::close(master);
}

TEST_CASE("open failures set an error")
{
    SerialPort port;
    CHECK_FALSE(serial_open(port, "/dev/does-not-exist", 115200));
    CHECK_FALSE(port.error.empty());
    CHECK_FALSE(serial_is_open(port));
    CHECK_FALSE(serial_open(port, "/dev/null", 12345)); // unsupported baud
}

TEST_CASE("port listing does not crash and gives /dev paths")
{
    for (const auto &p : serial_list_ports())
    {
        CHECK(p.name.starts_with("/dev/"));
        // No serial8250 placeholder without a UART (sysfs type 0) may be listed.
        std::ifstream type("/sys/class/tty/" + p.name.substr(5) + "/type");
        std::string t;
        CHECK_FALSE((p.name.starts_with("/dev/ttyS") && std::getline(type, t) && t == "0"));
    }
}
