// drivers/slcan: the SLCAN driver against a pseudo terminal standing in for the device.
// Expected wire bytes follow the Lawicel SLCAN protocol / canable2 slcanfd firmware.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <poll.h>
#ifdef __APPLE__
#include <util.h> // openpty
#else
#include <pty.h>
#endif
#include <string>
#include <thread>
#include <unistd.h>

#include "drivers/driver.h"

extern const DriverOps slcan_driver;

namespace
{

struct Pty
{
    int master = -1;
    int slave_fd = -1; // held open: with no slave open macOS reports a hangup on the master
    std::string slave;
};

Pty pty_open()
{
    Pty p;
    int slave = -1;
    char name[256] = {};
    REQUIRE(openpty(&p.master, &slave, name, nullptr, nullptr) == 0);
    p.slave_fd = slave; // the driver opens it again by name
    p.slave = name;
    return p;
}

// Everything the driver wrote until it is quiet for quiet_ms.
std::string pty_drain(int fd, int quiet_ms = 150)
{
    std::string out;
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    while (poll(&pfd, 1, quiet_ms) > 0 && (pfd.revents & POLLIN))
    {
        char buf[256];
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

void pty_write(int fd, std::string_view s)
{
    REQUIRE(write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size()));
}

// Reads until at least one frame arrived or ~1 s passed.
int read_frames(Iface& iface, BusMessage* out, int max)
{
    for (int i = 0; i < 10; ++i)
    {
        const int n = slcan_driver.read(iface, out, max, 100);
        if (n != 0)
        {
            return n;
        }
    }
    return 0;
}

void iface_for(Iface& iface, const Pty& p, uint32_t caps)
{
    iface.ops = &slcan_driver;
    iface.index = 3;
    iface.info.name = p.slave;
    iface.info.capabilities = caps;
}

} // namespace

TEST_CASE("slcan: device without confirmations")
{
    Pty p = pty_open();
    Iface iface;
    iface_for(iface, p, iface_cap::canfd);
    IfaceConfig cfg;
    cfg.bitrate = 250000;
    cfg.fd_bitrate = 5000000;
    REQUIRE(slcan_driver.open(iface, cfg));
    CHECK(pty_drain(p.master) == "C\rV\rS5\rY5\rM0\rO\r");

    // TX: standard data frame, reported back through read() right away.
    BusMessage tx{.id = 0x123};
    set_length(tx, 2);
    tx.data[0] = 0xAA;
    tx.data[1] = 0x0B;
    REQUIRE(slcan_driver.send(iface, tx));
    CHECK(pty_drain(p.master) == "t1232AA0B\r");
    BusMessage out[8];
    REQUIRE(read_frames(iface, out, 8) == 1);
    CHECK(has_flag(out[0], bus_flag::tx));
    CHECK(out[0].iface == 3);
    CHECK(out[0].ts_ns > 0);

    // Extended RTR, extended FD+BRS with 12 bytes (DLC 9), padding up to the DLC length.
    BusMessage rtr{.id = 0x1ABCDE, .flags = bus_flag::extended | bus_flag::rtr};
    set_length(rtr, 4);
    REQUIRE(slcan_driver.send(iface, rtr));
    CHECK(pty_drain(p.master) == "R001ABCDE4\r");
    BusMessage fd{.id = 0x7FF, .flags = bus_flag::fd | bus_flag::brs};
    set_length(fd, 12);
    fd.data[11] = 0x5A;
    REQUIRE(slcan_driver.send(iface, fd));
    CHECK(pty_drain(p.master) == "b7FF900000000000000000000005A\r");
    BusMessage fdx{.id = 0x10, .flags = bus_flag::extended | bus_flag::fd};
    set_length(fdx, 1);
    REQUIRE(slcan_driver.send(iface, fdx));
    CHECK(pty_drain(p.master) == "D00000010100\r");
    CHECK(read_frames(iface, out, 8) == 3);

    // Not expressible: classic frame > 8 bytes, FD RTR.
    BusMessage bad{.id = 1, .flags = bus_flag::fd | bus_flag::rtr};
    CHECK_FALSE(slcan_driver.send(iface, bad));

    // RX split over two writes, plus one malformed line.
    pty_write(p.master, "T1234567830102");
    pty_write(p.master, "03\rtXYZ\r");
    REQUIRE(read_frames(iface, out, 8) == 1);
    CHECK(out[0].id == 0x12345678);
    CHECK(has_flag(out[0], bus_flag::extended));
    CHECK_FALSE(has_flag(out[0], bus_flag::tx));
    CHECK(out[0].len == 3);
    CHECK(out[0].data[2] == 0x03);
    CHECK(out[0].iface == 3);

    IfaceStats st;
    slcan_driver.stats(iface, st);
    CHECK(st.rx_frames == 1);
    CHECK(st.rx_errors == 1);
    CHECK(st.tx_frames == 4);
    CHECK(st.tx_dropped == 1);

    slcan_driver.close(iface);
    CHECK(pty_drain(p.master) == "C\r");
    iface.impl.reset();
    close(p.slave_fd);
    close(p.master);
}

TEST_CASE("slcan: confirm mode, CR = ACK, BEL = NACK")
{
    Pty p = pty_open();
    std::atomic<bool> stop{false};
    std::string seen;
    // Device: answers every command line with CR, except frame 0x666 with BEL.
    std::jthread dev([&] {
        std::string line;
        while (!stop)
        {
            pollfd pfd{.fd = p.master, .events = POLLIN, .revents = 0};
            if (poll(&pfd, 1, 20) <= 0)
            {
                continue;
            }
            char c = 0;
            if (read(p.master, &c, 1) != 1)
            {
                break;
            }
            if (c != '\r')
            {
                line += c;
                continue;
            }
            seen += line + '\r';
            const char* reply = line == "V" ? "V1013\r" : line.starts_with("t666") ? "\a" : "\r";
            (void)write(p.master, reply, std::char_traits<char>::length(reply));
            line.clear();
        }
    });

    Iface iface;
    iface_for(iface, p, iface_cap::listen_only);
    IfaceConfig cfg;
    cfg.listen_only = true;
    cfg.is_custom_bitrate = true;
    cfg.custom_bitrate = 0x1234;
    REQUIRE(slcan_driver.open(iface, cfg));
    CHECK(iface.info.version == "V1013");

    BusMessage ok{.id = 0x100};
    BusMessage nack{.id = 0x666};
    REQUIRE(slcan_driver.send(iface, ok));
    REQUIRE(slcan_driver.send(iface, nack));
    BusMessage out[8];
    int n = 0;
    for (int i = 0; i < 20 && n < 1; ++i)
    {
        n += slcan_driver.read(iface, out + n, 8 - n, 50);
    }
    REQUIRE(n == 1);
    CHECK(out[0].id == 0x100);
    CHECK(has_flag(out[0], bus_flag::tx));
    // Give the NACK time to arrive, then check it was counted and not reported.
    for (int i = 0; i < 5; ++i)
    {
        CHECK(slcan_driver.read(iface, out, 8, 50) == 0);
    }
    IfaceStats st;
    slcan_driver.stats(iface, st);
    CHECK(st.tx_frames == 1);
    CHECK(st.tx_errors == 1);

    slcan_driver.close(iface);
    iface.impl.reset();
    stop = true;
    dev.join();
    CHECK(seen == "C\rV\rS001234\rM1\rO\rt1000\rt6660\rC\r");
    close(p.slave_fd);
    close(p.master);
}
