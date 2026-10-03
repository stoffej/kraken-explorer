// ui/tx_generator: the cyclic sender jthread (deadline scheduling, disable on send failure,
// clean stop) against a fake driver, and real frames on vcan0 read by a second socket.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h> // FocusWindow, the child windows by name
#include <pugixml.hpp>

#include "app.h"
#include "db/dbc/dbc_parser.h"
#include "drivers/driver.h"
#include "ui_test.h"
#include "ui/tx_generator.h"

#ifdef __linux__
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#endif
#include <unistd.h>

extern const DriverOps socketcan_driver;

namespace
{

std::atomic<int> fake_sent{0};

const DriverOps fake_driver = {
    .name = "Fake",
    .enumerate = nullptr,
    .open = [](Iface&, const IfaceConfig&) { return true; },
    .close = [](Iface&) {},
    .send = [](Iface&, const BusMessage&) { ++fake_sent; return true; },
    .read =
        [](Iface&, BusMessage*, int, int timeout_ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    },
    .stats = nullptr,
    .lin_sleep_wakeup = nullptr,
    .lin_set_schedule = nullptr,
    .lin_diag_request = nullptr,
};

void enable(TxGenerator& gen, std::size_t row)
{
    std::scoped_lock lock(gen.mutex);
    gen.rows[row].enabled = true;
    gen.rows[row].next_due = {};
    gen.changed = true;
    gen.wake.notify_one();
}

// One headless frame; focus = a window name (or a child's "/##name_" part) to focus first.
void ui_frame(App& app, const WorkspaceTab& tab, TxGenerator& gen, std::string_view focus = {})
{
    ImGui::NewFrame();
    if (!focus.empty())
    {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        {
            if (std::string_view(w->Name).find(focus) != std::string_view::npos)
            {
                ImGui::FocusWindow(w);
            }
        }
    }
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({1400, 800});
    draw_tx_generator(app, tab, gen);
    ImGui::EndFrame();
}

void type(App& app, const WorkspaceTab& tab, TxGenerator& gen, ImWchar c)
{
    ImGui::GetIO().AddInputCharacter(c);
    ui_frame(app, tab, gen);
}

void press(App& app, const WorkspaceTab& tab, TxGenerator& gen, ImGuiKey key)
{
    ImGui::GetIO().AddKeyEvent(key, true);
    ui_frame(app, tab, gen);
    ImGui::GetIO().AddKeyEvent(key, false);
    ui_frame(app, tab, gen);
}

} // namespace

TEST_CASE("cyclic sender keeps the interval, disables rows whose send fails, stops cleanly")
{
    std::deque<Iface> ifaces;
    Iface& i = ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "Fake", .name = "fake0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);

    {
        TxGenerator gen;
        gen.rows.push_back({.msg = BusMessage{.id = 0x100, .iface = 0}, .interval_ms = 10});
        gen.rows.push_back({.msg = BusMessage{.id = 0x101, .iface = 7}, .interval_ms = 10}); // no such interface
        tx_generator_start(gen, ifaces);
        enable(gen, 0);
        enable(gen, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int sent = fake_sent.load();
        CHECK(sent >= 20); // 30 due; generous for ASan / loaded CI
        CHECK(sent <= 32); // no bursts
        {
            std::scoped_lock lock(gen.mutex);
            CHECK(gen.rows[0].enabled);
            CHECK_FALSE(gen.rows[1].enabled);
        }
        tx_generator_stop_all(gen);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int after_stop = fake_sent.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(fake_sent.load() == after_stop);
        // Destructor: request_stop wakes the condition wait, join returns.
        const auto t0 = std::chrono::steady_clock::now();
        gen.sender = {};
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
    }
    ifaces_stop(ifaces);
}

TEST_CASE("workspace <frame> round trip, Qt files without driver attribute")
{
    std::deque<Iface> ifaces;
    ifaces.emplace_back().info.name = "other";
    Iface& i = ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";

    TxGenerator gen;
    BusMessage m{.id = 0x18FF1234, .flags = bus_flag::extended | bus_flag::fd | bus_flag::brs, .iface = 1};
    set_length(m, 12);
    m.data[0] = 0xAB;
    m.data[11] = 0x01;
    gen.rows.push_back({.msg = m, .name = "Engine", .interval_ms = 250, .enabled = true});
    gen.rows.push_back({.msg = m, .name = "Resp", .trigger = TxTrigger::OnReceive, .rx_id = 0x7E0, .rx_iface = 1, .delay_ms = 15});
    gen.rows.push_back({.msg = m, .name = "Btn", .trigger = TxTrigger::Manual});
    pugi::xml_document doc;
    tx_generator_save_xml(gen, ifaces, doc.append_child("tx"));
    const pugi::xml_node f = doc.child("tx").child("frame");
    CHECK(std::string_view(f.attribute("id").as_string()) == "0x18FF1234");
    CHECK(std::string_view(f.attribute("data").as_string()) == "AB 00 00 00 00 00 00 00 00 00 00 01");

    CHECK(f.attribute("trigger").empty()); // Cyclic rows stay as before

    TxGenerator back;
    tx_generator_load_xml(back, ifaces, doc.child("tx"));
    REQUIRE(back.rows.size() == 3);
    CHECK(back.rows[1].trigger == TxTrigger::OnReceive);
    CHECK(back.rows[1].rx_id == 0x7E0);
    CHECK_FALSE(back.rows[1].rx_extended);
    CHECK(back.rows[1].rx_iface == 1);
    CHECK(back.rows[1].delay_ms == 15);
    CHECK(back.rows[2].trigger == TxTrigger::Manual);
    const TxCyclic& r = back.rows[0];
    CHECK(r.trigger == TxTrigger::Cyclic);
    CHECK(r.name == "Engine");
    CHECK(r.interval_ms == 250);
    CHECK_FALSE(r.enabled);
    CHECK(r.msg.id == 0x18FF1234);
    CHECK(r.msg.flags == m.flags);
    CHECK(r.msg.len == 12);
    CHECK(r.msg.iface == 1);
    CHECK(r.msg.data == m.data);

    // Qt-written frame: no driver attribute, unknown interface stays unresolved.
    pugi::xml_document qt;
    qt.load_string(R"(<w><frame id="0x123" name="A" dlc="2" extended="0" fd="0" brs="0" interval="50" interface="fake0" data="01 02"/>)"
                   R"(<frame id="0x124" dlc="1" interface="gone" data="FF"/></w>)");
    tx_generator_load_xml(back, ifaces, qt.child("w"));
    REQUIRE(back.rows.size() == 2);
    CHECK(back.rows[0].msg.iface == 1);
    CHECK(back.rows[0].msg.data[1] == 0x02);
    CHECK(back.rows[1].msg.iface == UINT16_MAX);
}

// Expected bytes from cantools 0.x (Message.encode, one signal per frame, the others raw 0),
// never from Kraken itself.
TEST_CASE("physical signal values encode like cantools, clamped to the DBC range")
{
    CanDb db;
    REQUIRE(dbc_parse_file(DEMO_DBC, db));
    const CanDbMessage* engine = can_db_find_message(std::as_const(db), 256);
    REQUIRE(engine != nullptr);
    const auto encode = [&](std::string_view name, double value)
    {
        BusMessage m{.id = 256};
        set_length(m, 8);
        tx_signal_set(*can_db_find_signal(*engine, name), m, value);
        return std::vector<uint8_t>(m.data.begin(), m.data.begin() + 8);
    };
    using B = std::vector<uint8_t>;
    CHECK(encode("EngineSpeed", 1234) == B{0x00, 0x09, 0xA4, 0, 0, 0, 0, 0});
    CHECK(encode("EngineTemp", 90) == B{0x00, 0x00, 0x01, 0x04, 0, 0, 0, 0});
    CHECK(encode("OilPressure", 4.5) == B{0x00, 0x00, 0x00, 0x00, 0x5A, 0, 0, 0});
    CHECK(encode("EngineSpeed", 8000) == B{0x00, 0x3E, 0x80, 0, 0, 0, 0, 0});
    CHECK(encode("EngineSpeed", 9000) == encode("EngineSpeed", 8000)); // [0|8000]
    CHECK(encode("EngineTemp", -100) == encode("EngineTemp", -40));   // [-40|215] -> raw 0
}

// Motorola signals that don't start at a byte's LSB (demo.dbc TransmissionData / AmbientData).
// Expected bytes from cantools (Message.encode, one signal per frame, the others raw 0).
TEST_CASE("Motorola signals in the middle of a byte encode like cantools")
{
    CanDb db;
    REQUIRE(dbc_parse_file(DEMO_DBC, db));
    const auto encode = [&](uint32_t id, std::string_view name, double value)
    {
        const CanDbMessage* msg = can_db_find_message(std::as_const(db), id);
        REQUIRE(msg != nullptr);
        BusMessage m{.id = id};
        set_length(m, msg->dlc);
        tx_signal_set(*can_db_find_signal(*msg, name), m, value);
        return std::vector<uint8_t>(m.data.begin(), m.data.begin() + msg->dlc);
    };
    using B = std::vector<uint8_t>;
    CHECK(encode(512, "GearPos", 3) == B{0x30, 0x00, 0x00, 0x00});
    CHECK(encode(512, "VehicleSpeed", 123.4) == B{0x04, 0xD2, 0x00, 0x00});
    CHECK(encode(768, "OutsideTemp", 21) == B{0x3D, 0x00});
    CHECK(encode(768, "Humidity", 65) == B{0x00, 0x82});
}

TEST_CASE("multiplexed message: the mux value selects the signals shown and encoded")
{
    CanDb db;
    REQUIRE(dbc_parse(R"(VERSION ""

NS_:

BS_:

BU_: X

BO_ 1024 Muxed: 8 X
 SG_ Mux M : 0|8@1+ (1,0) [0|255] "" X
 SG_ A m1 : 8|16@1+ (0.5,0) [0|1000] "V" X
 SG_ B m2 : 8|16@1- (1,-100) [-1000|1000] "A" X
)",
                      db));
    const CanDbMessage& msg = db.messages.at(1024);
    const CanDbSignal& mux = *can_db_find_signal(msg, "Mux");
    const CanDbSignal& a = *can_db_find_signal(msg, "A");
    const CanDbSignal& b = *can_db_find_signal(msg, "B");
    BusMessage m{.id = 1024};
    set_length(m, 8);

    can_signal_inject_raw(mux, m, 2); // what the mux combo does
    CHECK_FALSE(can_signal_present(msg, a, m));
    REQUIRE(can_signal_present(msg, b, m));
    tx_signal_set(b, m, -150);
    CHECK(std::vector<uint8_t>(m.data.begin(), m.data.begin() + 8) == std::vector<uint8_t>{0x02, 0xCE, 0xFF, 0, 0, 0, 0, 0});

    m.data = {};
    can_signal_inject_raw(mux, m, 1);
    CHECK(can_signal_present(msg, a, m));
    CHECK_FALSE(can_signal_present(msg, b, m));
    tx_signal_set(a, m, 12.5);
    CHECK(std::vector<uint8_t>(m.data.begin(), m.data.begin() + 8) == std::vector<uint8_t>{0x01, 0x19, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("on-receive trigger arms matching rows with their delay")
{
    TxGenerator gen; // no sender thread: only the arming logic
    TxCyclic trig{.name = "resp", .enabled = true, .trigger = TxTrigger::OnReceive, .rx_id = 0x123, .rx_iface = 1, .delay_ms = 50};
    gen.rows.push_back(trig);
    trig.rx_extended = true;
    gen.rows.push_back(trig); // same id but extended: must not fire on a standard frame
    trig.rx_extended = false;
    trig.enabled = false;
    gen.rows.push_back(trig); // not armed (stopped)
    gen.rows.push_back({.name = "cyclic", .enabled = true});

    const auto t0 = std::chrono::steady_clock::now();
    const BusMessage other_iface{.id = 0x123, .iface = 0};
    const BusMessage echo{.id = 0x123, .flags = bus_flag::tx, .iface = 1};
    const BusMessage err{.id = 0x123, .errors = 1, .iface = 1};
    tx_generator_on_rx(gen, std::array{other_iface, echo, err}, t0);
    CHECK_FALSE(gen.rows[0].pending);

    const BusMessage hit{.id = 0x123, .iface = 1};
    tx_generator_on_rx(gen, std::span(&hit, 1), t0);
    CHECK(gen.rows[0].pending);
    CHECK(gen.rows[0].next_due == t0 + std::chrono::milliseconds(50));
    CHECK_FALSE(gen.rows[1].pending);
    CHECK_FALSE(gen.rows[2].pending);
    CHECK_FALSE(gen.rows[3].pending);
    CHECK(gen.changed);

    // A second frame while pending does not move the deadline.
    tx_generator_on_rx(gen, std::span(&hit, 1), t0 + std::chrono::milliseconds(10));
    CHECK(gen.rows[0].next_due == t0 + std::chrono::milliseconds(50));

    // Edited trigger: index rebuilt once rx_dirty is set.
    gen.rows[1].rx_extended = false;
    gen.rx_dirty = true;
    tx_generator_on_rx(gen, std::span(&hit, 1), t0);
    CHECK(gen.rows[1].pending);
}

// T87b a2 F10: the frame was received 30 ms before this UI frame; a 50 ms delay is due 20 ms
// from now, not 50. A stale timestamp (older than 1 s, e.g. replayed) counts as received now.
TEST_CASE("on-receive delay runs from the frame's reception time")
{
    TxGenerator gen;
    gen.rows.push_back({.name = "resp", .enabled = true, .trigger = TxTrigger::OnReceive, .rx_id = 0x123, .rx_iface = 1, .delay_ms = 50});
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int64_t wall = 1757000000000000000LL;
    const BusMessage late{.id = 0x123, .iface = 1, .ts_ns = wall - 30000000};
    tx_generator_on_rx(gen, std::span(&late, 1), t0, wall);
    CHECK(gen.rows[0].next_due == t0 + std::chrono::milliseconds(20));

    gen.rows[0].pending = false;
    const BusMessage stale{.id = 0x123, .iface = 1, .ts_ns = wall - 5000000000};
    tx_generator_on_rx(gen, std::span(&stale, 1), t0, wall);
    CHECK(gen.rows[0].next_due == t0 + std::chrono::milliseconds(50));
}

TEST_CASE("on-receive row is sent once per trigger by the sender thread")
{
    std::deque<Iface> ifaces;
    Iface& i = ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "Fake", .name = "fake0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);
    {
        TxGenerator gen;
        gen.rows.push_back({.msg = BusMessage{.id = 0x200, .iface = 0}, .enabled = true, .trigger = TxTrigger::OnReceive,
                            .rx_id = 0x100, .rx_iface = 0, .delay_ms = 20});
        gen.rows.push_back({.msg = BusMessage{.id = 0x201, .iface = 0}, .enabled = true, .trigger = TxTrigger::Manual});
        tx_generator_start(gen, ifaces);
        const int before = fake_sent.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(fake_sent.load() == before); // nothing armed, Manual never sent by the thread
        const BusMessage hit{.id = 0x100, .iface = 0};
        tx_generator_on_rx(gen, std::span(&hit, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(fake_sent.load() == before); // still within the delay
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK(fake_sent.load() == before + 1);
        {
            std::scoped_lock lock(gen.mutex);
            CHECK_FALSE(gen.rows[0].pending);
        }
    }
    ifaces_stop(ifaces);
}

#ifdef __linux__ // SocketCAN
TEST_CASE("cyclic frames come out on vcan0")
{
    if (if_nametoindex("vcan0") == 0)
    {
        MESSAGE("vcan0 not up, skipped");
        return;
    }
    constexpr uint32_t test_id = 0x5A7; // own id: vcan0 is shared with other test runs
    const int peer = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    REQUIRE(peer >= 0);
    const can_filter filter{.can_id = test_id, .can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG};
    setsockopt(peer, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter));
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex("vcan0"));
    REQUIRE(bind(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    std::deque<Iface> ifaces;
    Iface& i = ifaces.emplace_back();
    i.ops = &socketcan_driver;
    i.info.name = "vcan0";
    i.info.details = "vcan"; // as enumerate: never `ip link set` (pkexec) the shared vcan
    Setup setup;
    setup.networks.push_back({.interfaces = {{.driver = "SocketCAN", .name = "vcan0"}}});
    REQUIRE(ifaces_start(ifaces, setup, {}, nullptr) == 1);

    int got = 0;
    {
        TxGenerator gen;
        BusMessage m{.id = test_id, .iface = 0};
        set_length(m, 2);
        m.data[0] = 0xCA;
        m.data[1] = 0xFE;
        gen.rows.push_back({.msg = m, .interval_ms = 20});
        tx_generator_start(gen, ifaces);
        enable(gen, 0);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < deadline)
        {
            pollfd p{.fd = peer, .events = POLLIN, .revents = 0};
            if (poll(&p, 1, 20) > 0)
            {
                can_frame f{};
                if (read(peer, &f, sizeof(f)) == static_cast<ssize_t>(sizeof(f)) && f.can_id == test_id)
                {
                    CHECK(f.len == 2);
                    CHECK(f.data[1] == 0xFE);
                    ++got;
                }
            }
        }
        tx_generator_stop_all(gen);
    } // jthread joined here, before the interface closes
    ifaces_stop(ifaces);
    close(peer);
    MESSAGE("vcan0: ", got, " cyclic frames in 300 ms");
    CHECK(got >= 8);  // 15 due
    CHECK(got <= 18);
}
#endif

TEST_CASE("vim keys in Active Cyclic Transmissions: j/k select, Space runs/stops, Enter expands, / searches")
{
    const UiTest ui({1600, 900});
    App app;
    app.measuring = true;
    const WorkspaceTab tab{.uid = 5};
    TxGenerator gen; // after app: its sender thread is joined first
    // OnReceive rows: the sender thread leaves them alone until a frame arms them.
    for (const uint32_t id : {0x300u, 0x301u, 0x302u})
    {
        gen.rows.push_back({.msg = BusMessage{.id = id}, .name = "r", .trigger = TxTrigger::OnReceive, .rx_id = 0x7FF});
    }
    ui_frame(app, tab, gen);
    ui_frame(app, tab, gen, workspace_window_name(tab, "Generator View")); // e.g. after Ctrl+w
    const auto enabled = [&](std::size_t i)
    {
        std::scoped_lock lock(gen.mutex);
        return gen.rows[i].enabled;
    };

    type(app, tab, gen, 'j');
    CHECK(gen.current == 0);
    type(app, tab, gen, 'j');
    CHECK(gen.current == 1);
    CHECK(gen.rows[1].selected);
    CHECK_FALSE(gen.rows[0].selected);
    press(app, tab, gen, ImGuiKey_Space);
    CHECK(enabled(1));
    CHECK_FALSE(enabled(0));
    CHECK_FALSE(enabled(2));
    press(app, tab, gen, ImGuiKey_Space);
    CHECK_FALSE(enabled(1));
    type(app, tab, gen, 'G');
    CHECK(gen.current == 2);
    type(app, tab, gen, 'k');
    type(app, tab, gen, 'k');
    CHECK(gen.current == 0);
    press(app, tab, gen, ImGuiKey_Enter);
    CHECK(gen.rows[0].expanded);
    CHECK_FALSE(gen.rows[1].expanded);

    app.measuring = false; // starting needs a measurement, like the disabled ▶
    press(app, tab, gen, ImGuiKey_Space);
    CHECK_FALSE(enabled(0));

    type(app, tab, gen, '/');
    CHECK(gen.focus_search); // taken by the search field next frame
    ui_frame(app, tab, gen);
    CHECK_FALSE(gen.focus_search);
}

TEST_CASE("vim keys in Available Messages: j selects, Enter adds the DBC message")
{
    const UiTest ui({1600, 900});
    App app;
    Iface& i = app.ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    auto db = std::make_shared<CanDb>();
    REQUIRE(dbc_parse_file(DEMO_DBC, *db));
    SetupNetwork net;
    net.interfaces.push_back({.driver = "Fake", .name = "fake0"});
    net.can_dbs.push_back(db);
    app.setup.networks.push_back(std::move(net));
    const WorkspaceTab tab{.uid = 6};
    TxGenerator gen;
    ui_frame(app, tab, gen);
    ui_frame(app, tab, gen, "/##available_");

    type(app, tab, gen, 'j');
    const CanDbMessage& first = db->messages.begin()->second;
    REQUIRE(gen.layout_msg == &first);
    CHECK(gen.avail_selected == std::vector<const CanDbMessage*>{&first});
    press(app, tab, gen, ImGuiKey_Enter);
    REQUIRE(gen.rows.size() == 1);
    CHECK(gen.rows[0].name == first.name);
    CHECK(gen.rows[0].msg.id == (first.raw_id & can_id_mask_extended));
}

// A DBC in a network without interfaces (a log analysed offline): its messages are still listed,
// so they can be added and edited in Message View.
TEST_CASE("Available Messages lists every DBC when no network uses the interface")
{
    const UiTest ui({1600, 900});
    App app;
    auto db = std::make_shared<CanDb>();
    REQUIRE(dbc_parse_file(DEMO_DBC, *db));
    SetupNetwork net;
    net.can_dbs.push_back(db);
    app.setup.networks.push_back(std::move(net));
    const WorkspaceTab tab{.uid = 8};
    TxGenerator gen;
    ui_frame(app, tab, gen);
    ui_frame(app, tab, gen, "/##available_");
    type(app, tab, gen, 'j');
    REQUIRE(gen.layout_msg == &db->messages.begin()->second);
    press(app, tab, gen, ImGuiKey_Enter);
    CHECK(gen.rows.size() == 1);
}

// T87b a2 F1: the DBC keeps bit 31 on extended ids (0x98FEEE00); the list shows and searches 0x18FEEE00.
TEST_CASE("Available Messages lists and finds extended ids without the DBC's bit 31")
{
    const UiTest ui({1600, 900});
    App app;
    auto db = std::make_shared<CanDb>();
    std::vector<DbcError> errors;
    const bool parsed = dbc_parse("VERSION \"\"\n\nNS_ :\n\nBS_:\n\nBU_: X\n\n"
                      "BO_ 256 Other: 8 X\n SG_ A : 0|8@1+ (1,0) [0|255] \"\" X\n\n"
                      "BO_ 2566843904 Temp: 8 X\n SG_ B : 0|8@1+ (1,0) [0|255] \"\" X\n",
                      *db, &errors);
    CHECK(errors.empty());
    REQUIRE(parsed);
    Iface& i = app.ifaces.emplace_back();
    i.ops = &fake_driver;
    i.info.name = "fake0";
    SetupNetwork net;
    net.interfaces.push_back({.driver = "Fake", .name = "fake0"});
    net.can_dbs.push_back(db);
    app.setup.networks.push_back(std::move(net));
    const WorkspaceTab tab{.uid = 7};
    TxGenerator gen;
    std::snprintf(gen.search, sizeof gen.search, "18feee");
    ui_frame(app, tab, gen);
    ui_frame(app, tab, gen, "/##available_");
    type(app, tab, gen, 'j');
    REQUIRE(gen.layout_msg != nullptr);
    CHECK(gen.layout_msg->name == "Temp");
}

// T87b a2 F4: RUN / ▶ on a selection never enables its Manual rows (they only send on Send),
// so a later trigger change can't start sending on its own.
TEST_CASE("starting a selection leaves its Manual rows stopped")
{
    const UiTest ui({1600, 900});
    App app;
    app.measuring = true;
    const WorkspaceTab tab{.uid = 8};
    TxGenerator gen;
    gen.rows.push_back({.msg = BusMessage{.id = 0x300}, .name = "r", .selected = true, .trigger = TxTrigger::OnReceive, .rx_id = 0x7FF});
    gen.rows.push_back({.msg = BusMessage{.id = 0x301}, .name = "m", .selected = true, .trigger = TxTrigger::Manual});
    ui_frame(app, tab, gen);
    ui_frame(app, tab, gen, workspace_window_name(tab, "Generator View"));
    gen.current = 0;
    press(app, tab, gen, ImGuiKey_Space);
    std::scoped_lock lock(gen.mutex);
    CHECK(gen.rows[0].enabled);
    CHECK_FALSE(gen.rows[1].enabled);
}

// K58's IntelF vector from cantools 44.1 (tests/can_db_signal): 0|32@1 float32 = 3.14159 -> d00f4940.
// The signal editor's InputDouble feeds tx_signal_set, which must store the IEEE bit pattern.
TEST_CASE("a float32 signal set from the editor stores the IEEE-754 bit pattern like cantools")
{
    const CanDbSignal intel_f{.start_bit = 0, .length = 32, .value_type = SignalValueType::float32};
    BusMessage m{.id = 0x10};
    set_length(m, 8);
    tx_signal_set(intel_f, m, 3.14159);
    CHECK(std::vector<uint8_t>(m.data.begin(), m.data.begin() + 8) == std::vector<uint8_t>{0xd0, 0x0f, 0x49, 0x40, 0, 0, 0, 0});
    CHECK(can_signal_extract_physical(intel_f, m) == 3.141590118408203);
}

// T87b a2: the expanded row's On-receive settings and signal table were squeezed into the Name
// column (labels clipped, "deg(" for units); they span the whole table width now.
TEST_CASE("expanded row details span the table width, not the Name column")
{
    const UiTest ui({1600, 900});
    App app;
    const WorkspaceTab tab{.uid = 6};
    TxGenerator gen;
    gen.rows.push_back({.msg = BusMessage{.id = 0x300}, .name = "r", .trigger = TxTrigger::OnReceive, .rx_id = 0x7FF, .expanded = true});
    ui_frame(app, tab, gen);
    ui_frame(app, tab, gen);
    const auto window_width = [](std::string_view part)
    {
        for (const ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        {
            if (std::string_view(w->Name).find(part) != std::string_view::npos)
            {
                return w->Size.x;
            }
        }
        return 0.0f;
    };
    const float table = window_width("##active");
    const float details = window_width("##details");
    REQUIRE(table > 0.0f);
    CHECK(details >= table * 0.9f);
}
