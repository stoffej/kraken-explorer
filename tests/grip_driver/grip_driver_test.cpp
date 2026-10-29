// drivers/grip: GrIP wire codec and the GrIP-CANIL driver against a pseudo terminal
// standing in for the CANIL. CRC vector: CRC-8/SAE-J1850 check value from the reveng
// CRC catalogue; packet layouts from the firmware structs of the Qt GrIPHandler.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <poll.h>
#ifdef __APPLE__
#include <util.h> // openpty
#else
#include <pty.h>
#endif
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app.h"
#include "drivers/grip.h"

namespace
{

using namespace std::chrono_literals;

std::span<const uint8_t> bytes(std::string_view s)
{
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::vector<uint8_t> sys(uint8_t cmd, std::vector<uint8_t> fields, uint8_t data = 0)
{
    std::vector<uint8_t> p = {1, cmd, static_cast<uint8_t>(fields.size()), data};
    p.insert(p.end(), fields.begin(), fields.end());
    return p;
}

void le32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
    {
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    }
}

uint32_t get32(const GripPacket& p, std::size_t at)
{
    return static_cast<uint32_t>(p.data[at]) | static_cast<uint32_t>(p.data[at + 1]) << 8 |
           static_cast<uint32_t>(p.data[at + 2]) << 16 | static_cast<uint32_t>(p.data[at + 3]) << 24;
}

// Fake CANIL: 1 classic CAN + 1 LIN channel, answers info, capability and send requests.
struct FakeDevice
{
    int master = -1;
    std::string slave;
    std::mutex mutex;
    std::vector<GripPacket> received;   // every system command from the host
    std::atomic<bool> stop{false};
    std::thread thread;
};

void fake_write(FakeDevice& d, uint8_t msg_type, std::span<const uint8_t> payload)
{
    std::string wire;
    REQUIRE(grip_encode(wire, msg_type, grip::ret_ok, payload));
    std::lock_guard lock(d.mutex);
    REQUIRE(write(d.master, wire.data(), wire.size()) == static_cast<ssize_t>(wire.size()));
}

void fake_run(FakeDevice& d)
{
    GripParser parser;
    while (!d.stop)
    {
        pollfd pfd{.fd = d.master, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 20) <= 0 || !(pfd.revents & POLLIN))
        {
            continue;
        }
        uint8_t buf[512];
        const ssize_t n = read(d.master, buf, sizeof(buf));
        if (n <= 0)
        {
            continue;
        }
        grip_parse(parser, std::span(buf, static_cast<std::size_t>(n)));
        for (const auto& pkt : parser.packets)
        {
            if (pkt.msg_type != grip::msg_system_cmd)
            {
                continue;
            }
            {
                std::lock_guard lock(d.mutex);
                d.received.push_back(pkt);
            }
            const uint8_t cmd = pkt.data[1];
            if (cmd == 0) // SYSTEM_REPORT_INFO
            {
                auto p = sys(0, {1, 2, 0, /*CAN*/ 1, /*CANFD*/ 0, /*LIN*/ 1, 0, 0, /*tables*/ 4, /*cyclic*/ 8});
                for (char c : std::string_view("2026-01-01"))
                {
                    p.push_back(static_cast<uint8_t>(c));
                }
                p.push_back(0);
                fake_write(d, grip::msg_system_cmd, p);
            }
            else if (cmd == 34) // SYSTEM_GET_CHANNEL_CAPABILITIES -> DATA_CHANNEL_CAPABILITIES
            {
                const uint8_t bus = pkt.data[4];
                const uint32_t caps = bus == 0 ? (grip_cap::can_baud_500k | grip_cap::can_baud_1m | grip_cap::can_listen_only |
                                                  grip_cap::can_abom | grip_cap::can_fd)
                                               : grip_cap::lin_mode_master;
                std::vector<uint8_t> f = {bus, pkt.data[5]};
                le32(f, caps);
                fake_write(d, grip::msg_data, sys(251, f));
            }
            else if (cmd == 30) // SYSTEM_SEND_CAN_FRAME -> TX echo 209 with the hash from Time
            {
                std::vector<uint8_t> f;
                le32(f, get32(pkt, 12));
                fake_write(d, grip::msg_data, sys(209, f));
            }
        }
        parser.packets.clear();
        parser.acks.clear();
    }
}

void fake_start(FakeDevice& d)
{
    int slave = -1;
    char name[256] = {};
    REQUIRE(openpty(&d.master, &slave, name, nullptr, nullptr) == 0);
    close(slave); // the driver opens it by name
    d.slave = name;
    d.thread = std::thread(fake_run, std::ref(d));
}

void fake_stop(FakeDevice& d)
{
    d.stop = true;
    d.thread.join();
    close(d.master);
}

// Last system command `cmd` the fake device received.
std::optional<GripPacket> find_cmd(FakeDevice& d, uint8_t cmd)
{
    std::lock_guard lock(d.mutex);
    for (auto it = d.received.rbegin(); it != d.received.rend(); ++it)
    {
        if (it->data[1] == cmd)
        {
            return *it;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("crc8 is CRC-8/SAE-J1850")
{
    CHECK(grip::crc8(bytes("123456789")) == 0x4B);
    CHECK(grip::crc8({}) == 0x00);
}

TEST_CASE("encode produces SOH, hex header, SOT, hex payload, EOT")
{
    std::string wire;
    const uint8_t payload[] = {0x01, 0x00, 0x00, 0x00};
    REQUIRE(grip_encode(wire, grip::msg_system_cmd, grip::ret_ok, payload));
    const uint8_t crc_data = grip::crc8(payload);
    const uint8_t hdr[] = {4, 0, 0, 0, 4, 0};
    const uint8_t crc_hdr = grip::crc8(hdr);
    const std::string expect = std::format("\x01" "040000000400{:02X}{:02X}\x02" "01000000\x03", crc_hdr, crc_data);
    CHECK(wire == expect);

    std::string big;
    const std::vector<uint8_t> too_long(257);
    CHECK_FALSE(grip_encode(big, grip::msg_data, grip::ret_ok, too_long));
    CHECK(big.empty());
}

TEST_CASE("parse: round trip, acks, noise and corruption")
{
    const uint8_t payload[] = {1, 254, 3, 0, 0xAA, 0xBB, 0xCC};

    SUBCASE("data packet with noise in front is decoded and acknowledged")
    {
        std::string wire = "xx\x03";
        REQUIRE(grip_encode(wire, grip::msg_data, grip::ret_ok, payload));
        GripParser p;
        // byte by byte: the state machine must not depend on read boundaries
        for (uint8_t c : bytes(wire))
        {
            grip_parse(p, std::span(&c, 1));
        }
        REQUIRE(p.packets.size() == 1);
        CHECK(p.packets[0].msg_type == grip::msg_data);
        CHECK(p.packets[0].length == sizeof(payload));
        CHECK(std::equal(std::begin(payload), std::end(payload), p.packets[0].data.begin()));
        CHECK(p.acks == std::vector<uint8_t>{grip::ret_ok});
    }
    SUBCASE("no ack for MSG_DATA_NO_RESPONSE, responses are dropped")
    {
        std::string wire;
        REQUIRE(grip_encode(wire, grip::msg_data_no_response, grip::ret_ok, payload));
        REQUIRE(grip_encode(wire, grip::msg_response, grip::ret_ok, {}));
        GripParser p;
        grip_parse(p, bytes(wire));
        CHECK(p.packets.size() == 1);
        CHECK(p.acks.empty());
    }
    SUBCASE("header-only packet")
    {
        std::string wire;
        REQUIRE(grip_encode(wire, grip::msg_system_cmd, grip::ret_ok, {}));
        GripParser p;
        grip_parse(p, bytes(wire));
        REQUIRE(p.packets.size() == 1);
        CHECK(p.packets[0].length == 0);
    }
    SUBCASE("payload CRC mismatch drops the packet, next packet still decodes")
    {
        std::string wire;
        REQUIRE(grip_encode(wire, grip::msg_data, grip::ret_ok, payload));
        wire[wire.size() - 2] = wire[wire.size() - 2] == '0' ? '1' : '0';
        REQUIRE(grip_encode(wire, grip::msg_data, grip::ret_ok, payload));
        GripParser p;
        grip_parse(p, bytes(wire));
        CHECK(p.crc_errors == 1);
        CHECK(p.packets.size() == 1);
    }
    SUBCASE("header CRC mismatch")
    {
        std::string wire;
        REQUIRE(grip_encode(wire, grip::msg_data, grip::ret_ok, payload));
        wire[1] = '5';   // version 0x54
        GripParser p;
        grip_parse(p, bytes(wire));
        CHECK(p.packets.empty());
        wire.clear();
        REQUIRE(grip_encode(wire, grip::msg_data, grip::ret_ok, payload));
        wire[4] = wire[4] == '0' ? '1' : '0';   // protocol byte, CRC no longer matches
        grip_parse(p, bytes(wire));
        CHECK(p.packets.empty());
        CHECK(p.crc_errors == 1);
    }
    SUBCASE("non-hex payload is answered with RET_WRONG_PARAM")
    {
        std::string wire;
        REQUIRE(grip_encode(wire, grip::msg_data, grip::ret_ok, payload));
        wire[wire.find('\x02') + 1] = 'G';
        GripParser p;
        grip_parse(p, bytes(wire));
        CHECK(p.packets.empty());
        CHECK(p.acks == std::vector<uint8_t>{grip::ret_wrong_param});
    }
}

TEST_CASE("caps map to setup capabilities and bitrates")
{
    IfaceInfo i;
    grip_apply_caps(i, false, false, grip_cap::can_baud_500k | grip_cap::can_listen_only);
    CHECK(i.capabilities == iface_cap::listen_only);
    REQUIRE(i.bitrates.size() == 1);
    CHECK(i.bitrates[0].bitrate == 500000);

    grip_apply_caps(i, false, true, 0);   // unknown caps: every rate incl. 800k, FD from the channel type
    CHECK(i.capabilities == (iface_cap::auto_restart | iface_cap::listen_only | iface_cap::canfd));
    CHECK(i.bitrates.size() == 9 * 2);

    grip_apply_caps(i, true, false, grip_cap::lin_mode_master | grip_cap::lin_mode_slave);
    CHECK(i.capabilities == (iface_cap::lin_master | iface_cap::lin_slave));
    CHECK(i.bitrates.empty());
}

TEST_CASE("GrIP-CANIL driver against a fake device")
{
    FakeDevice fake;
    fake_start(fake);
    App app;
    grip_attach(app.tasks, app.ifaces);
    grip_port = fake.slave;

    SUBCASE("device info and capabilities")
    {
        auto dev = grip_device_open(fake.slave);
        REQUIRE(dev);
        {
            std::lock_guard lock(dev->mutex);
            CHECK(dev->info_received);
            CHECK(dev->info.version == "1.2-<2026-01-01>");
            CHECK(dev->info.can == 1);
            CHECK(dev->info.lin == 1);
            CHECK(dev->info.lin_tables == 4);
        }
        CHECK(grip_request_caps(*dev, 1, 0, 500ms) == grip_cap::lin_mode_master);
    }

    SUBCASE("CAN channel: config, RX, TX echo, GPIO via tasks")
    {
        Iface iface;
        iface.ops = &grip_driver;
        iface.index = 3;
        iface.info.name = "CANIL-CAN0";
        IfaceConfig cfg{.bitrate = 500000, .fd_bitrate = 2000000, .listen_only = true, .auto_restart = true};
        REQUIRE(grip_driver.open(iface, cfg));
        std::this_thread::sleep_for(100ms); // let the fake device read the last commands
        auto dev = grip_open_device();
        REQUIRE(dev);

        // Device has FD caps -> SYSTEM_SEND_CANFD_CFG: Channel, Arb, Data, EchoTx, ABOM, Listen
        const auto fdcfg = find_cmd(fake, 35);
        REQUIRE(fdcfg);
        CHECK(fdcfg->data[2] == 12);
        CHECK(fdcfg->data[4] == 0);
        CHECK(get32(*fdcfg, 5) == 500000);
        CHECK(get32(*fdcfg, 9) == 2000000);
        CHECK(fdcfg->data[13] == 1);
        CHECK(fdcfg->data[14] == 1);
        CHECK(fdcfg->data[15] == 1);
        const auto start = find_cmd(fake, 22);   // SYSTEM_START_CAN {ch0, ch1}
        REQUIRE(start);
        CHECK(start->data[4] == 1);

        // RX: DATA_REPORT_CAN_MSG, extended FD frame with BRS and 12 data bytes
        std::vector<uint8_t> f = {0};
        le32(f, 0x18DAF110);
        f.insert(f.end(), {12, 0x01 | 0x02 | 0x08, 0});
        le32(f, 0);
        for (uint8_t b = 0; b < 64; ++b)
        {
            f.push_back(b < 12 ? static_cast<uint8_t>(0xA0 + b) : 0);
        }
        fake_write(fake, grip::msg_data, sys(254, f));

        BusMessage out[8];
        int n = 0;
        for (int i = 0; i < 20 && n == 0; ++i)
        {
            n = grip_driver.read(iface, out, 8, 50);
        }
        REQUIRE(n == 1);
        CHECK(out[0].id == 0x18DAF110);
        CHECK(out[0].flags == (bus_flag::extended | bus_flag::fd | bus_flag::brs));
        CHECK(out[0].len == 12);
        CHECK(out[0].dlc == 9);
        CHECK(out[0].data[11] == 0xAB);
        CHECK(out[0].iface == 3);
        CHECK(out[0].errors == 0);

        // Error flags: ACK + bit dominant
        f[7] = 0x04 | 0x10; // ErrFlags
        fake_write(fake, grip::msg_data, sys(254, f));
        n = 0;
        for (int i = 0; i < 20 && n == 0; ++i)
        {
            n = grip_driver.read(iface, out, 8, 50);
        }
        REQUIRE(n == 1);
        CHECK(out[0].errors == (bus_error::ack | bus_error::bit));

        // TX: the frame comes back through the echo report
        BusMessage tx{.id = 0x123, .type = BusType::CAN};
        set_length(tx, 2);
        tx.data[0] = 0x11;
        tx.data[1] = 0x22;
        REQUIRE(grip_driver.send(iface, tx));
        n = 0;
        for (int i = 0; i < 20 && n == 0; ++i)
        {
            n = grip_driver.read(iface, out, 8, 50);
        }
        REQUIRE(n == 1);
        CHECK(out[0].id == 0x123);
        CHECK(has_flag(out[0], bus_flag::tx));
        CHECK(out[0].data[1] == 0x22);
        const auto sent = find_cmd(fake, 30);
        REQUIRE(sent);
        CHECK(sent->data[2] == 1 + 4 + 1 + 1 + 1 + 4 + 2);
        CHECK(get32(*sent, 5) == 0x123);
        CHECK(sent->data[9] == 2);

        IfaceStats st;
        grip_driver.stats(iface, st);
        CHECK(st.rx_frames == 2);
        CHECK(st.tx_frames == 1);

        // GPIO report reaches dev->gpio through the task queue
        std::vector<uint8_t> g = {0x05, 0x80};
        for (uint16_t mv = 100; mv < 900; mv += 100)
        {
            g.push_back(static_cast<uint8_t>(mv));
            g.push_back(static_cast<uint8_t>(mv >> 8));
        }
        fake_write(fake, grip::msg_data, sys(252, g));
        for (int i = 0; i < 50 && dev->gpio.reports == 0; ++i)
        {
            std::this_thread::sleep_for(10ms);
            tasks_drain(app.tasks, app);
        }
        CHECK(dev->gpio.reports == 1);
        CHECK(dev->gpio.pins == 0x8005);
        CHECK(dev->gpio.mv[7] == 800);

        grip_gpio_output(*dev, 0x00F0);
        std::this_thread::sleep_for(100ms);
        const auto gout = find_cmd(fake, 33);
        REQUIRE(gout);
        CHECK(gout->data[4] == 0xF0);

        grip_driver.close(iface);
        iface.impl.reset();
        dev.reset();
        CHECK_FALSE(grip_open_device()); // last channel closed -> port closed
    }

    SUBCASE("unknown channel does not open")
    {
        Iface iface;
        iface.ops = &grip_driver;
        iface.info.name = "CANIL-CAN5";
        CHECK_FALSE(grip_driver.open(iface, IfaceConfig{}));
    }

    grip_port.clear();
    fake_stop(fake);
}
