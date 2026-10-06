// drivers/driver: listener thread + Iface lifecycle against a fake DriverOps, and the
// SocketCAN driver against the kernel's vcan (skipped when vcan0 is not up).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "drivers/driver.h"

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

extern const DriverOps socketcan_driver;

namespace
{

struct Fake
{
    std::atomic<int> opened{0};
    std::atomic<int> closed{0};
    std::atomic<int> sent{0};
    std::atomic<int> reads{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> fail{false}; // next read reports the channel gone
};
Fake fake;

const DriverOps fake_driver = {
    .name = "Fake",
    .enumerate = [](std::vector<IfaceInfo>& out) { out.push_back({.name = "fake0"}); },
    .open = [](Iface& iface, const IfaceConfig&) { ++fake.opened; return iface.info.name != "dead0"; },
    .close = [](Iface&) { ++fake.closed; },
    .send = [](Iface&, const BusMessage&) { ++fake.sent; return true; },
    .read =
        [](Iface& iface, BusMessage* out, int max, int timeout_ms)
    {
        if (fake.fail.exchange(false))
        {
            return -1;
        }
        if (fake.reads++ == 0 && max >= 3)
        {
            for (int i = 0; i < 3; ++i)
            {
                out[i] = BusMessage{.id = static_cast<uint32_t>(0x100 + i), .iface = iface.index};
            }
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    },
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

// Polls the inbox until it holds `count` frames or 2 s pass.
std::vector<BusMessage> wait_inbox(Inbox& inbox, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::vector<BusMessage> got;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::scoped_lock lock(inbox.mutex);
            got.insert(got.end(), inbox.msgs.begin(), inbox.msgs.end());
            inbox.msgs.clear();
        }
        if (got.size() >= count)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return got;
}

} // namespace

TEST_CASE("listener delivers read batches to consumers and inbox, stop closes")
{
    std::deque<Iface> ifaces;
    auto& i = ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";

    Setup setup;
    setup.networks.push_back({.name = "Network 1",
                              .interfaces = {{.driver = "Fake", .name = "fake0"}, {.driver = "Fake", .name = "gone"}}});
    const RxConsumer consumers[] = {{.fn = [](void*, const BusMessage&) { ++fake.consumed; }, .user = nullptr}};

    CHECK_FALSE(iface_send(i, BusMessage{}));
    CHECK(ifaces_start(ifaces, setup, consumers, nullptr) == 1);
    CHECK(setup.networks[0].interfaces[0].iface == 0);
    CHECK(setup.networks[0].interfaces[1].iface == -1);
    CHECK(fake.opened == 1);

    const auto got = wait_inbox(i.inbox, 3);
    REQUIRE(got.size() == 3);
    CHECK(got[2].id == 0x102);
    CHECK(fake.consumed == 3);
    CHECK(i.total_bits > 0);
    CHECK(iface_send(i, BusMessage{}));
    CHECK(fake.sent == 1);

    ifaces_stop(ifaces);
    CHECK(fake.closed == 1);
    CHECK_FALSE(i.listener.joinable());
    CHECK_FALSE(iface_send(i, BusMessage{}));
    CHECK(fake.sent == 1);
}

TEST_CASE("an interface that fails to open is marked failed and not counted as up")
{
    std::deque<Iface> ifaces;
    for (const char* name : {"fake0", "dead0", "fake1"})
    {
        auto& i = ifaces.emplace_back();
        i.ops = &fake_driver;
        i.info.name = name;
        i.index = static_cast<uint16_t>(ifaces.size() - 1);
    }
    Setup setup;
    setup.networks.push_back({.name = "Network 1",
                              .interfaces = {{.driver = "Fake", .name = "fake0"},
                                             {.driver = "Fake", .name = "dead0"},
                                             {.driver = "Fake", .name = "fake1", .enabled = false},
                                             {.driver = "Fake", .name = "gone"}}});

    CHECK(ifaces_start(ifaces, setup, {}, nullptr) == 1);
    CHECK_FALSE(ifaces[0].failed);
    CHECK(ifaces[1].failed);
    CHECK_FALSE(ifaces[1].open);
    const auto c = ifaces_link_count(ifaces, setup);
    CHECK(c.up == 1);
    CHECK(c.total == 3); // disabled fake1 left out, missing "gone" counts as down
    ifaces_stop(ifaces);
}

TEST_CASE("default setup leaves out interfaces that are down")
{
    std::deque<Iface> ifaces;
    for (const char* name : {"up0", "down0", "up1"})
    {
        auto& i = ifaces.emplace_back();
        i.ops = &fake_driver;
        i.info.name = name;
        i.info.up = std::strcmp(name, "down0") != 0;
    }
    Setup setup;
    ifaces_default_setup(ifaces, setup);
    REQUIRE(setup.networks.size() == 2);
    CHECK(setup.networks[0].interfaces[0].name == "up0");
    CHECK(setup.networks[1].name == "Network 2");
    CHECK(setup.networks[1].interfaces[0].name == "up1");
}

TEST_CASE("a read error closes the interface and the listener reopens it a second later")
{
    fake.opened = 0;
    fake.closed = 0;
    fake.reads = 1; // no first-read batch
    std::deque<Iface> ifaces;
    auto& i = ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "Fake", .name = "fake0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);
    const auto wait_failed = [&](bool failed)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (i.failed != failed && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return i.failed == failed;
    };
    fake.fail = true;
    REQUIRE(wait_failed(true));
    CHECK(fake.closed == 1);
    CHECK_FALSE(iface_send(i, BusMessage{})); // closed meanwhile
    REQUIRE(wait_failed(false));
    CHECK(fake.opened == 2);
    CHECK(iface_send(i, BusMessage{}));
    ifaces_stop(ifaces);
    CHECK(fake.closed == 2);
}

TEST_CASE("SocketCAN on vcan0: RX from another socket, TX echo, error frames")
{
    if (if_nametoindex("vcan0") == 0)
    {
        MESSAGE("vcan0 not up, skipped");
        return;
    }
    std::vector<IfaceInfo> found;
    socketcan_driver.enumerate(found);
    const auto it = std::find_if(found.begin(), found.end(), [](const IfaceInfo& f) { return f.name == "vcan0"; });
    REQUIRE(it != found.end());
    CHECK(it->details == "vcan");
    CHECK((it->capabilities & iface_cap::canfd) != 0);

    std::deque<Iface> ifaces;
    auto& i = ifaces.emplace_back();
    i.ops = &socketcan_driver;
    i.info = *it;
    i.index = 7;
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "SocketCAN", .name = "vcan0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);

    // An independent raw socket plays the other node, like cangen.
    const int peer = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    REQUIRE(peer >= 0);
    const int on = 1;
    setsockopt(peer, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on));
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex("vcan0"));
    REQUIRE(bind(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // vcan0 is shared with parallel runs of this test: every frame carries our pid
    // (error frame: in data[4..7], which ACK/BUSOFF classes leave unused).
    const auto pid = static_cast<uint32_t>(getpid());
    can_frame std_frame{};
    std_frame.can_id = 0x123;
    std_frame.len = 7;
    std_frame.data[0] = 0xDE;
    std_frame.data[1] = 0xAD;
    std_frame.data[2] = 0x01;
    std::memcpy(&std_frame.data[3], &pid, sizeof(pid));
    REQUIRE(write(peer, &std_frame, sizeof(std_frame)) == CAN_MTU);

    canfd_frame fd_frame{};
    fd_frame.can_id = 0x1ABCDEF0 | CAN_EFF_FLAG;
    fd_frame.len = 12;
    fd_frame.flags = CANFD_BRS;
    fd_frame.data[11] = 0x5A;
    std::memcpy(&fd_frame.data[0], &pid, sizeof(pid));
    REQUIRE(write(peer, &fd_frame, sizeof(fd_frame)) == CANFD_MTU);

    can_frame err{};
    err.can_id = CAN_ERR_FLAG | CAN_ERR_ACK | CAN_ERR_BUSOFF;
    err.len = CAN_ERR_DLC;
    std::memcpy(&err.data[4], &pid, sizeof(pid));
    REQUIRE(write(peer, &err, sizeof(err)) == CAN_MTU);

    BusMessage tx{.id = 0x321, .flags = bus_flag::extended};
    set_length(tx, 6);
    tx.data[0] = 0x42;
    std::memcpy(&tx.data[2], &pid, sizeof(pid));
    REQUIRE(iface_send(i, tx));

    auto ours = [pid](const std::vector<BusMessage>& all)
    {
        auto tagged = [pid](const BusMessage& m, std::size_t at)
        { return m.len >= at + sizeof(pid) && std::memcmp(&m.data[at], &pid, sizeof(pid)) == 0; };
        std::vector<BusMessage> out;
        for (const auto& m : all)
        {
            if ((m.id == 0x123 && tagged(m, 3)) || (m.id == 0x1ABCDEF0 && tagged(m, 0)) ||
                (m.id == 0x321 && tagged(m, 2)) || (is_error_frame(m) && tagged(m, 4)))
            {
                out.push_back(m);
            }
        }
        return out;
    };
    std::vector<BusMessage> got;
    for (int tries = 0; tries < 20 && got.size() < 4; ++tries)
    {
        const auto more = ours(wait_inbox(i.inbox, 4 - got.size()));
        got.insert(got.end(), more.begin(), more.end());
    }

    // A burst: every frame arrives once, or the socket reports it as dropped.
    // SO_RXQ_OVFL only reaches us on a received frame, so drops after the last
    // burst frame show up on a trailing sentinel (0x7AC), resent until one lands.
    IfaceStats stats;
    REQUIRE(iface_stats(i, stats));
    const auto drops_before = stats.rx_overruns;
    constexpr int burst = 5000;
    can_frame b{};
    b.can_id = 0x7AB;
    b.len = 5;
    std::memcpy(&b.data[1], &pid, sizeof(pid));
    for (int k = 0; k < burst; ++k)
    {
        b.data[0] = static_cast<uint8_t>(k);
        REQUIRE(write(peer, &b, sizeof(b)) == CAN_MTU);
    }
    can_frame sentinel = b;
    sentinel.can_id = 0x7AC;
    auto mine = [pid](const BusMessage& m)
    {
        return m.len == 5 && std::memcmp(&m.data[1], &pid, sizeof(pid)) == 0;
    };
    int burst_got = 0;
    bool sentinel_seen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sentinel_seen && std::chrono::steady_clock::now() < deadline)
    {
        REQUIRE(write(peer, &sentinel, sizeof(sentinel)) == CAN_MTU);
        for (const auto& m : wait_inbox(i.inbox, 1))
        {
            burst_got += m.id == 0x7AB && mine(m) ? 1 : 0;
            sentinel_seen = sentinel_seen || (m.id == 0x7AC && mine(m));
        }
    }
    REQUIRE(iface_stats(i, stats));
    ifaces_stop(ifaces);
    close(peer);
    const auto drops = static_cast<int>(stats.rx_overruns - drops_before);
    MESSAGE("burst: ", burst_got, " received, ", drops, " dropped");
    CHECK(sentinel_seen);
    CHECK(burst_got <= burst);
    // Foreign vcan0 traffic (other test runs) can add to the drop count, never subtract.
    CHECK(burst_got + drops >= burst);
    CHECK(stats.state == IfaceState::Ok);
    REQUIRE(got.size() == 4);

    CHECK(got[0].id == 0x123);
    CHECK(got[0].flags == 0);
    CHECK(got[0].len == 7);
    CHECK(got[0].data[1] == 0xAD);
    CHECK(got[0].iface == 7);
    CHECK(got[0].ts_ns > 0);

    CHECK(got[1].id == 0x1ABCDEF0);
    CHECK(got[1].flags == (bus_flag::extended | bus_flag::fd | bus_flag::brs));
    CHECK(got[1].len == 12);
    CHECK(got[1].dlc == 9);
    CHECK(got[1].data[11] == 0x5A);

    CHECK(got[2].id == 0);
    CHECK(got[2].errors == (bus_error::ack | bus_error::bus_off));

    CHECK(got[3].id == 0x321);
    CHECK(got[3].flags == (bus_flag::extended | bus_flag::tx));
    CHECK(got[3].len == 6);
    CHECK(got[3].data[0] == 0x42);
}

TEST_CASE("SocketCAN read: one batched read returns at most max, the rest stays queued")
{
    if (if_nametoindex("vcan0") == 0)
    {
        MESSAGE("vcan0 not up, skipped");
        return;
    }
    Iface i;
    i.ops = &socketcan_driver;
    i.info.name = "vcan0";
    i.info.details = "vcan"; // as enumerate: never `ip link set` (pkexec) the shared vcan
    REQUIRE(socketcan_driver.open(i, IfaceConfig{.driver = "SocketCAN", .name = "vcan0"}));

    const int peer = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    REQUIRE(peer >= 0);
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex("vcan0"));
    REQUIRE(bind(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const auto pid = static_cast<uint32_t>(getpid());
    constexpr int count = 10;
    can_frame f{};
    f.can_id = 0x7AD;
    f.len = 5;
    std::memcpy(&f.data[1], &pid, sizeof(pid));
    for (int k = 0; k < count; ++k)
    {
        f.data[0] = static_cast<uint8_t>(k);
        REQUIRE(write(peer, &f, sizeof(f)) == CAN_MTU);
    }

    std::vector<int> seq;
    BusMessage out[3];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (seq.size() < count && std::chrono::steady_clock::now() < deadline)
    {
        const int n = socketcan_driver.read(i, out, 3, 100);
        REQUIRE(n >= 0);
        CHECK(n <= 3);
        for (int k = 0; k < n; ++k)
        {
            if (out[k].id == 0x7AD && out[k].len == 5 && std::memcmp(&out[k].data[1], &pid, sizeof(pid)) == 0)
            {
                seq.push_back(out[k].data[0]);
            }
        }
    }
    socketcan_driver.close(i);
    close(peer);
    REQUIRE(seq.size() == count);
    for (int k = 0; k < count; ++k)
    {
        CHECK(seq[k] == k);
    }
}

// Expected rate is how this bench's can0 (PCAN-USB) is brought up: ip link set can0 type can bitrate 250000.
TEST_CASE("SocketCAN stats report the controller's bitrate: can0 at 250k, 0 for vcan0")
{
    const auto stats_of = [](const char* name)
    {
        std::deque<Iface> ifaces;
        auto& i = ifaces.emplace_back();
        i.ops = &socketcan_driver;
        i.info.name = name;
        IfaceStats st;
        // configure = false: never `ip link set` (pkexec) the bench adapter, just read it.
        REQUIRE(socketcan_driver.open(i, IfaceConfig{.driver = "SocketCAN", .name = name, .configure = false}));
        socketcan_driver.stats(i, st);
        socketcan_driver.close(i);
        return st;
    };
    if (if_nametoindex("vcan0") != 0)
    {
        const IfaceStats st = stats_of("vcan0");
        CHECK(st.bitrate == 0);
        CHECK(st.data_bitrate == 0);
    }
    else
    {
        MESSAGE("vcan0 not up, skipped");
    }
    if (!socketcan_link_up("can0"))
    {
        MESSAGE("can0 not up, skipped");
        return;
    }
    const IfaceStats st = stats_of("can0");
    CHECK(st.bitrate == 250000);
    CHECK(st.data_bitrate == 0); // PCAN-USB is classic CAN
}

TEST_CASE("SocketCAN link commands (GUI up/down, vcan add/delete)")
{
    using V = std::vector<std::string>;
    CHECK(ip_link_args(LinkOp::Up, "vcan0") == V{"link", "set", "vcan0", "up"}); // vcan: no bit timing
    // Physical CAN: bit timing in the same call, ip-link(8) `type can` syntax.
    const IfaceConfig classic{.bitrate = 500000, .sample_point = 875};
    CHECK(ip_link_args(LinkOp::Up, "can0", &classic)
          == V{"link", "set", "can0", "up", "type", "can", "bitrate", "500000", "sample-point", "0.875",
               "listen-only", "off", "restart-ms", "0"});
    const IfaceConfig fd{.bitrate = 250000, .sample_point = 800, .can_fd = true, .fd_bitrate = 2000000,
                         .fd_sample_point = 750, .auto_restart = true, .auto_restart_ms = 100};
    CHECK(ip_link_args(LinkOp::Up, "can1", &fd)
          == V{"link", "set", "can1", "up", "type", "can", "bitrate", "250000", "sample-point", "0.800",
               "dbitrate", "2000000", "dsample-point", "0.750", "fd", "on", "listen-only", "off", "restart-ms", "100"});
    CHECK(ip_link_args(LinkOp::Down, "can0") == V{"link", "set", "can0", "down"});
    CHECK(ip_link_args(LinkOp::AddVcan, "vcan2") == V{"link", "add", "dev", "vcan2", "up", "type", "vcan"});
    CHECK(ip_link_args(LinkOp::Delete, "vcan2") == V{"link", "delete", "vcan2"});

    const V as_user = ip_command({"link", "delete", "vcan2"}, false);
    REQUIRE(as_user.size() == 5);
    CHECK(as_user[0] == "pkexec");
    CHECK(as_user[1].ends_with("ip")); // absolute when found: the polkit rule matches on the path
    CHECK(V(as_user.begin() + 2, as_user.end()) == V{"link", "delete", "vcan2"});
    const V as_root = ip_command({"link", "set", "can0", "up"}, true);
    CHECK(as_root[0] == as_user[1]);
    CHECK(as_root.size() == 5);

    CHECK(next_vcan_name([](const std::string&) { return false; }) == "vcan0");
    CHECK(next_vcan_name([](const std::string& n) { return n == "vcan0" || n == "vcan1" || n == "vcan3"; }) == "vcan2");

    CHECK_FALSE(socketcan_link_exists("nosuchcan9"));
    CHECK_FALSE(socketcan_link_up("nosuchcan9"));
    CHECK(socketcan_link_exists("lo"));
    CHECK(socketcan_link_up("lo"));
}

TEST_CASE("pkexec exit status and stderr classify the ip failure")
{
    // Strings and exit codes from pkexec(1) in a sway session without a polkit agent.
    CHECK(ip_result_classify(0, "") == IpResult::ok);
    CHECK(ip_result_classify(127, "Error creating textual authentication agent: Error opening current "
                                  "controlling terminal for the process (`/dev/tty'): No such device or address\n")
          == IpResult::no_agent);
    CHECK(ip_result_classify(127, "==== AUTHENTICATION FAILED ====\nError executing command as another user: "
                                  "No authentication agent found.\n")
          == IpResult::no_agent);
    CHECK(ip_result_classify(126, "Error executing command as another user: Not authorized\n") == IpResult::denied);
    CHECK(ip_result_classify(127, "") == IpResult::failed); // command not found
    CHECK(ip_result_classify(2, "RTNETLINK answers: Operation not permitted\n") == IpResult::failed);
    CHECK(ip_result_classify(-1, "") == IpResult::failed); // killed by a signal
}

TEST_CASE("auto-baud hits on frames without error frames")
{
    // Wrong rates show error frames, an idle rate shows nothing.
    CHECK(autobaud_hit({.frames = 40, .errors = 0}));
    CHECK_FALSE(autobaud_hit({.frames = 0, .errors = 7}));
    CHECK_FALSE(autobaud_hit({.frames = 1, .errors = 3}));
    CHECK_FALSE(autobaud_hit({})); // idle
    CHECK_FALSE(autobaud_hit({.frames = 1, .errors = 0})); // one frame could be a fluke
    CHECK_FALSE(autobaud_hit({.frames = 12, .errors = 1}));
}
