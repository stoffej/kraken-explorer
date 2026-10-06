// ui/conditional_logging: operators, AND/OR evaluation, and the CSV written from decoded
// frames (header, 5 s pre-buffer, falling edge closes, stale values, setup changes).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "core/setup.h"
#include "db/dbc/dbc_parser.h"
#include "ui/conditional_logging.h"

namespace
{

constexpr const char* dbc = R"(VERSION "1"

NS_:

BS_:

BU_: ECU

BO_ 256 Msg1: 8 ECU
 SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] "km/h" Vector__XXX
 SG_ Temp : 16|8@1+ (1,-40) [-40|215] "degC" Vector__XXX
)";

Setup make_setup()
{
    auto db = std::make_shared<CanDb>();
    REQUIRE(dbc_parse(dbc, *db));
    Setup setup;
    setup.networks.push_back({.name = "Net", .can_dbs = {db}});
    setup_rebuild_cache(setup);
    return setup;
}

const CanDbSignal* signal(const Setup& setup, const char* name)
{
    CanDbMessage& msg = setup.networks[0].can_dbs[0]->messages.at(256);
    return can_db_find_signal(msg, name);
}

BusMessage frame(double speed, double seconds)
{
    BusMessage m{.id = 0x100, .ts_ns = static_cast<int64_t>(seconds * 1e9)};
    set_length(m, 8);
    const auto raw = static_cast<uint16_t>(speed / 0.1 + 0.5);
    m.data[0] = static_cast<uint8_t>(raw);
    m.data[1] = static_cast<uint8_t>(raw >> 8);
    m.data[2] = 65; // Temp = 25
    return m;
}

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("condition_holds: every operator")
{
    CHECK(condition_holds(ConditionOp::Greater, 2, 1));
    CHECK_FALSE(condition_holds(ConditionOp::Greater, 1, 1));
    CHECK(condition_holds(ConditionOp::Less, 0, 1));
    CHECK(condition_holds(ConditionOp::Equal, 1, 1));
    CHECK(condition_holds(ConditionOp::GreaterEqual, 1, 1));
    CHECK(condition_holds(ConditionOp::LessEqual, 1, 1));
    CHECK_FALSE(condition_holds(ConditionOp::LessEqual, 2, 1));
    CHECK(condition_holds(ConditionOp::NotEqual, 2, 1));
    CHECK_FALSE(condition_holds(ConditionOp::NotEqual, 1, 1));
}

TEST_CASE("state: off when disabled, armed while waiting, triggered while the conditions hold")
{
    const Setup setup = make_setup();
    ConditionalLogging cl;
    CHECK(conditional_logging_state(cl) == TriggerState::Off);
    CHECK(std::string(trigger_state_label(TriggerState::Off)).empty());
    ConditionalLoggingConfig cfg{.enabled = true};
    cfg.conditions.push_back({.signal = signal(setup, "Speed"), .op = ConditionOp::Greater, .threshold = 50.0});
    conditional_logging_apply(cl, cfg);
    CHECK(conditional_logging_state(cl) == TriggerState::Armed);
    CHECK(trigger_state_label(TriggerState::Armed) == std::string("Armed: waiting for trigger"));
    const auto now = std::chrono::steady_clock::now();
    conditional_logging_process(cl, setup, frame(60.0, 1.0), now);
    CHECK(conditional_logging_state(cl) == TriggerState::Triggered);
    CHECK(trigger_state_label(TriggerState::Triggered) == std::string("Triggered: logging"));
    conditional_logging_process(cl, setup, frame(10.0, 2.0), now);
    CHECK(conditional_logging_state(cl) == TriggerState::Armed);
}

TEST_CASE("conditions_met: AND needs every value, OR any; no conditions never")
{
    const Setup setup = make_setup();
    const CanDbSignal* speed = signal(setup, "Speed");
    const CanDbSignal* temp = signal(setup, "Temp");
    const std::vector<LogCondition> conds = {{speed, ConditionOp::Greater, 10.0}, {temp, ConditionOp::Less, 0.0}};
    CHECK_FALSE(conditions_met({}, true, {{speed, 100.0}}));
    CHECK_FALSE(conditions_met(conds, true, {{speed, 20.0}}));            // temp missing
    CHECK(conditions_met(conds, false, {{speed, 20.0}}));                 // OR: speed suffices
    CHECK_FALSE(conditions_met(conds, false, {{speed, 5.0}}));
    CHECK(conditions_met(conds, true, {{speed, 20.0}, {temp, -5.0}}));
    CHECK_FALSE(conditions_met(conds, true, {{speed, 20.0}, {temp, 5.0}}));
}

TEST_CASE("CSV: opened on the rising edge with header + pre-buffer, closed on the falling edge")
{
    const Setup setup = make_setup();
    const CanDbSignal* speed = signal(setup, "Speed");
    const auto path = std::filesystem::temp_directory_path() / ("kraken_cl_" + std::to_string(getpid()) + ".csv");
    std::filesystem::remove(path);

    ConditionalLogging cl;
    conditional_logging_apply(cl, {.enabled = true,
                                   .conditions = {{speed, ConditionOp::Greater, 10.0}},
                                   .log_signals = {speed},
                                   .log_file_path = path.string()});
    const auto now = std::chrono::steady_clock::now();
    conditional_logging_process(cl, setup, frame(5.0, 1.0), now);
    CHECK_FALSE(cl.condition_met);
    CHECK(cl.values.at(speed) == doctest::Approx(5.0));
    CHECK(cl.pre_buffer.size() == 1);
    CHECK_FALSE(std::filesystem::exists(path));

    conditional_logging_process(cl, setup, frame(20.0, 2.0), now);
    CHECK(cl.condition_met);
    conditional_logging_process(cl, setup, frame(30.0, 2.5), now);
    conditional_logging_process(cl, setup, frame(5.0, 3.0), now); // falling edge: this row is not logged
    CHECK_FALSE(cl.condition_met);
    CHECK_FALSE(cl.file.is_open());
    CHECK(read_file(path) == "Timestamp,Speed [km/h]\n1.000000,5\n2.000000,20\n2.500000,30\n");

    // Pre-buffer keeps 5 s: the 3.0 s sample is older than 9.0 - 5 and dropped.
    conditional_logging_process(cl, setup, frame(40.0, 9.0), now);
    CHECK(cl.condition_met);
    conditional_logging_apply(cl, {}); // disable closes and flushes
    CHECK(read_file(path) == "Timestamp,Speed [km/h]\n1.000000,5\n2.000000,20\n2.500000,30\n9.000000,40\n");
    std::filesystem::remove(path);
}

TEST_CASE("stale values drop after 1.5 s and re-evaluate; unknown frames are ignored")
{
    const Setup setup = make_setup();
    const CanDbSignal* speed = signal(setup, "Speed");
    ConditionalLogging cl;
    conditional_logging_apply(cl, {.enabled = true, .conditions = {{speed, ConditionOp::Greater, 10.0}}});
    const auto t0 = std::chrono::steady_clock::now();
    conditional_logging_process(cl, setup, BusMessage{.id = 0x7FF}, t0); // not in the DBC
    CHECK(cl.values.empty());
    conditional_logging_process(cl, setup, frame(20.0, 1.0), t0);
    CHECK(cl.condition_met);
    CHECK_FALSE(conditional_logging_timeout(cl, t0 + std::chrono::milliseconds(1000)));
    CHECK(cl.condition_met);
    CHECK(conditional_logging_timeout(cl, t0 + std::chrono::milliseconds(1600)));
    CHECK_FALSE(cl.condition_met);
    CHECK(cl.values.empty());
}

TEST_CASE("prune drops conditions and columns whose signals left the setup")
{
    const Setup setup = make_setup();
    const CanDbSignal* speed = signal(setup, "Speed");
    ConditionalLogging cl;
    conditional_logging_apply(cl, {.enabled = true, .conditions = {{speed, ConditionOp::Greater, 10.0}}, .log_signals = {speed}});
    conditional_logging_process(cl, setup, frame(20.0, 1.0), std::chrono::steady_clock::now());
    CHECK(cl.condition_met);
    conditional_logging_prune(cl, setup); // same setup: nothing changes
    CHECK(cl.config.conditions.size() == 1);
    CHECK(cl.condition_met);
    conditional_logging_prune(cl, Setup{}); // new workspace: everything gone
    CHECK(cl.config.conditions.empty());
    CHECK(cl.config.log_signals.empty());
    CHECK(cl.values.empty());
    CHECK_FALSE(cl.condition_met);
}

TEST_CASE("a condition bound to an interface only triggers on frames from it")
{
    const Setup setup = make_setup();
    const CanDbSignal* speed = signal(setup, "Speed");
    ConditionalLogging cl;
    conditional_logging_apply(cl, {.enabled = true, .conditions = {{speed, ConditionOp::Greater, 10.0, 2}}});
    const auto now = std::chrono::steady_clock::now();
    BusMessage m = frame(20.0, 1.0);
    m.iface = 1;
    conditional_logging_process(cl, setup, m, now);
    CHECK_FALSE(cl.condition_met); // same message, other interface
    m.iface = 2;
    conditional_logging_process(cl, setup, m, now);
    CHECK(cl.condition_met);
    m = frame(5.0, 2.0);
    m.iface = 1;
    conditional_logging_process(cl, setup, m, now);
    CHECK(cl.condition_met); // iface 2 still holds 20
    CHECK(cl.values.at(speed) == doctest::Approx(5.0));

    // Any (the default) takes the latest value from every interface.
    ConditionalLogging any;
    conditional_logging_apply(any, {.enabled = true, .conditions = {{speed, ConditionOp::Greater, 10.0}}});
    m = frame(20.0, 1.0);
    m.iface = 1;
    conditional_logging_process(any, setup, m, now);
    CHECK(any.condition_met);
    CHECK(conditional_logging_timeout(cl, now + std::chrono::milliseconds(1600)));
    CHECK(cl.iface_values.empty());
    CHECK_FALSE(cl.condition_met);
}
